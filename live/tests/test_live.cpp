#include "BatchQueue.hpp"

#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace bq;

void require(bool condition, const char* problem) {
    if (!condition)
        throw std::runtime_error(problem);
}

struct Fixture {
    std::filesystem::path path;
    Fixture() {
        char name[] = "/tmp/buildqueue-test-XXXXXX";
        auto directory = ::mkdtemp(name);
        require(directory != nullptr, "Could not create fixture directory");
        path = directory;
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void bufferAllocation() {
    for (const auto fit : {Fit::First, Fit::Best, Fit::Worst}) {
        BufferPool pool(52);
        pool.setFit(fit);
        char* start = nullptr;
        {
            auto separator = [&] {
                auto first = pool.acquire(12);
                start = first.data();
                return pool.acquire(4);
            }();
            auto middle = [&] {
                pool.setFit(Fit::Worst);
                auto hole = pool.acquire(8);
                return pool.acquire(4);
            }();
            const auto fragmented = pool.stats();
            require(fragmented.free == 44 && fragmented.largest == 24 && fragmented.fragmentation > 0,
                    "Released separated allocations should leave fragmented space.");
            pool.setFit(fit);
            auto selected = pool.acquire(8);
            const auto expected = fit == Fit::First ? 0 : fit == Fit::Best ? 16 : 28;
            require(selected.data() == start + expected, "Allocation strategy chose the wrong block.");
            selected.data()[0] = 'x';
            require(selected.size() == 8, "Expected the requested buffer size.");
        }
        const auto released = pool.stats();
        require(released.blocks.size() == 1 && released.largest == 52 && released.merges > 0 &&
                released.fragmentation == 0 && released.peakFragmentation > 0,
                "Adjacent released buffers should coalesce.");
    }
}

void checksumProcess() {
    Fixture fixture;
    std::ofstream(fixture.path / "empty file");
    std::filesystem::create_directory(fixture.path / "folder");
    std::filesystem::create_symlink(fixture.path / "empty file", fixture.path / "alias");
    Checksum checksum(fixture.path);
    const auto listed = checksum.files();
    require(listed.size() == 1 && listed.front().name == "empty file",
            "Catalog must include only regular files.");
    const auto result = checksum.run("empty file");
    std::ofstream(fixture.path / "large", std::ios::binary) << std::string(600000, 'x');
    BufferPool buffers;
    const auto streamed = Checksum::runPath(fixture.path / "large", std::chrono::seconds(10),
                                          defaultMemoryMiB * bytesPerMiB, &buffers);
    require(streamed.exitCode == 0 && streamed.bytes == 600000 &&
            buffers.stats().peak == BufferPool::maxBuffer && buffers.stats().free == 2 * BufferPool::maxBuffer,
            "Checksum should stream a large file through a released pool buffer.");
    {
        auto occupied = buffers.acquire(2 * BufferPool::maxBuffer);
        const auto timedOut = Checksum::runPath(fixture.path / "large", std::chrono::milliseconds(30),
                                               defaultMemoryMiB * bytesPerMiB, &buffers);
        require(timedOut.exitCode == 124 && timedOut.pid < 0 &&
                timedOut.error.find("checksum buffer") != std::string::npos,
                "Waiting for a buffer should honor the job deadline without spawning a child.");
    }
    require(buffers.stats().free == 2 * BufferPool::maxBuffer,
            "The pool should remain usable after a timed-out allocation.");
    require(checksum.size("empty file") == 0 && Checksum::pathSize(fixture.path / "empty file") == 0,
            "Empty files should have a known zero-byte estimate.");
    require(!checksum.size("alias") && !Checksum::pathSize(fixture.path / "alias"),
            "Symlinks should have no size estimate.");
    require(result.pid > 0 && result.exitCode == 0 && result.error.empty(),
            "Expected a real successful checksum process.");
    require(result.checksum == 4294967295U && result.bytes == 0, "Incorrect empty-file checksum.");
    require(result.addressSpaceLimitBytes == defaultMemoryMiB * bytesPerMiB &&
            result.peakRssKiB.has_value(), "Expected the applied memory limit and OS peak RSS.");
    const auto blocked = checksum.run("empty file", std::chrono::seconds(5), 4096);
    require(blocked.pid > 0 && blocked.addressSpaceLimitBytes == 4096 &&
            blocked.exitCode > 0 && blocked.exitCode != 124 && !blocked.error.empty(),
            "Linux should reject a checksum child that cannot start under its address-space limit.");
    require(!checksum.run("missing").error.empty(), "Missing file should fail.");
    require(!checksum.run("alias").error.empty(), "Symbolic link should fail.");
    require(!checksum.run("../file").error.empty(), "Relative path should fail.");
    require(!checksum.run("folder").error.empty(), "Directory should fail.");
    require(!Checksum::runPath("empty file").error.empty(), "Desktop requires an absolute path.");
    require(!Checksum::runPath(fixture.path / "alias").error.empty(), "Desktop rejects symlinks.");
    BatchQueue desktop(2, 1, [](const std::string& name) { return Checksum::runPath(name); },
                       Input::AbsolutePaths);
    desktop.submit({(fixture.path / "empty file").string()}, 0);
    desktop.waitUntilIdle();
    const auto snapshot = desktop.snapshot();
    require(snapshot.jobs.front().state == State::Completed &&
            snapshot.jobs.front().result.checksum == result.checksum,
            "Desktop should run a real checksum on an absolute file path.");
}

void scheduling(Policy policy) {
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;
    std::vector<std::string> started;
    BatchQueue queue(4, 1, [&](const std::string& name) {
        std::unique_lock<std::mutex> lock(mutex);
        started.push_back(name);
        changed.notify_all();
        if (name == "first")
            changed.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
        ChecksumResult result;
        result.exitCode = 0;
        return result;
    }, Input::FolderNames, [](const std::string& name) -> std::optional<std::uintmax_t> {
        if (name == "unknown")
            return std::nullopt;
        return name == "high" ? 0 : 20;
    });
    require(queue.snapshot().policy == Policy::Fcfs, "FCFS should be the default.");
    queue.setPolicy(policy);
    queue.submit({"first"}, 0);
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return !started.empty(); }),
                "Worker did not start.");
    }
    queue.submit(std::vector<JobRequest>{{"low", 9}, {"unknown", 9}, {"high", 1}, {"high-tie", 1}});
    const auto cancelled = queue.submit({"cancel"}, 0);
    const bool didCancel = queue.cancel(cancelled.front());
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
    }
    changed.notify_all();
    queue.waitUntilIdle();
    std::vector<std::string> expected;
    switch (policy) {
        case Policy::Fcfs: expected = {"first", "low", "unknown", "high", "high-tie"}; break;
        case Policy::Priority: expected = {"first", "high", "high-tie", "low", "unknown"}; break;
        case Policy::Sjf: expected = {"first", "high", "low", "high-tie", "unknown"}; break;
        case Policy::RoundRobin: throw std::logic_error("Use the dedicated Round Robin test.");
    }
    require(didCancel, "Queued job should be cancellable.");
    require(started == expected, "Jobs ran in the wrong order.");
    const auto snapshot = queue.snapshot();
    for (const auto& job : snapshot.jobs) {
        if (job.state == State::Cancelled) {
            require(job.dispatchOrder == 0, "Cancelled jobs should have no dispatch order.");
            require(job.finished - job.submitted == job.waitTime(job.finished),
                    "Cancellation before dispatch should count all elapsed time as waiting.");
            continue;
        }
        require(job.dispatchOrder > 0 && job.dispatchOrder <= expected.size(),
                "Completed job should have a dispatch order.");
        require(expected[job.dispatchOrder - 1] == job.file && job.dispatchPolicy == policy,
                "Dispatch metadata should match the measured run.");
    }
    require(queue.snapshot().peak == 1, "Exceeded the concurrency limit.");
    queue.clearFinished();
    require(queue.snapshot().jobs.empty(), "Clearing terminal jobs should empty history.");
}

void concurrency() {
    std::mutex mutex;
    std::condition_variable changed;
    int started = 0;
    bool release = false;
    BatchQueue queue(4, 2, [&](const std::string&) {
        std::unique_lock<std::mutex> lock(mutex);
        ++started;
        changed.notify_all();
        changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
        ChecksumResult result;
        result.exitCode = 0;
        return result;
    });
    queue.submit({"one", "two", "three", "four"}, 0);
    bool reached;
    Snapshot active;
    {
        std::unique_lock<std::mutex> lock(mutex);
        reached = changed.wait_for(lock, std::chrono::seconds(5), [&] { return started >= 2; });
        active = queue.snapshot();
        release = true;
    }
    changed.notify_all();
    queue.waitUntilIdle();
    require(reached && active.running == 2, "Two jobs should run concurrently.");
    require(queue.snapshot().peak == 2 && started == 4, "Slot limit or queue progress is incorrect.");
}
}

void roundRobinRotation() {
    using namespace bq;
    std::vector<std::string> turns;
    BatchQueue queue(1, 1, [](const std::string&) { return ChecksumResult{}; },
                     Input::FolderNames, {},
                     [&](const std::string& file, ChecksumProgress& progress) -> std::optional<ChecksumResult> {
        turns.push_back(file);
        if (++progress.offset < 2)
            return std::nullopt;
        ChecksumResult result;
        result.exitCode = 0;
        return result;
    });
    queue.setPolicy(Policy::RoundRobin);
    queue.submit({"a", "b", "c"}, 0);
    queue.waitUntilIdle();
    require(turns == std::vector<std::string>{"a", "b", "c", "a", "b", "c"},
            "Round Robin must send unfinished jobs to the back of the ready queue.");
    for (const auto& job : queue.snapshot().jobs) {
        require(job.state == State::Completed && job.slices.size() == 2,
                "Each job should record two processing slices.");
        require(job.finished - job.submitted == job.waitTime(job.finished) + job.burstTime(job.finished),
                "Turnaround should equal waiting plus burst time.");
    }
}

int main() {
    try {
        bufferAllocation();
        checksumProcess();
        scheduling(bq::Policy::Fcfs);
        scheduling(bq::Policy::Priority);
        scheduling(bq::Policy::Sjf);
        roundRobinRotation();
        concurrency();
        std::cout << "PASS: real checksum process, FCFS, Priority, SJF, Round Robin, cancellation and concurrency\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
