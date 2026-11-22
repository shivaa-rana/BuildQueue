#include "DeadlockDemo.hpp"

#include <atomic>
#include <exception>
#include <fstream>
#include <functional>
#include <utility>
#include <algorithm>
#include <memory>

namespace bq {
namespace {
std::string readHeaderLine(std::ifstream& input, const std::filesystem::path& path) {
    char buffer[96];
    if (!input.getline(buffer, sizeof(buffer)))
        throw std::invalid_argument("Missing or too long lock-job header in " + path.filename().string());
    std::string line(buffer, static_cast<std::size_t>(input.gcount()) - (input.eof() ? 0 : 1));
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}

std::array<std::string, 2> readLockOrder(std::ifstream& input, const std::filesystem::path& path) {
    const std::array<std::string, 2> prefixes{"Lock first: ", "Lock second: "};
    std::array<std::string, 2> order;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto line = readHeaderLine(input, path);
        if (line.compare(0, prefixes[i].size(), prefixes[i]) != 0 || line.size() == prefixes[i].size())
            throw std::invalid_argument("Each job must declare both Lock first and Lock second.");
        order[i] = line.substr(prefixes[i].size());
        if (order[i].size() > 48 || order[i].find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _.-") != std::string::npos)
            throw std::invalid_argument("Use up to 48 letters, digits, spaces, underscores, periods or hyphens in lock names.");
    }
    return order;
}

ChecksumResult checksumJob(const std::filesystem::path& path, const std::atomic<bool>* stopped) {
    auto result = Checksum::runPath(path, std::chrono::minutes(2), defaultMemoryMiB * bytesPerMiB, nullptr, stopped);
    if (result.exitCode != 125 && (result.exitCode != 0 || !result.error.empty()))
        throw std::runtime_error("Could not checksum " + path.filename().string() + ": " + result.error);
    return result;
}
}

RecoveryDemoResult runLockedChecksumFile(const std::filesystem::path& file,
                                         std::chrono::milliseconds waitForTimeout,
                                         const std::atomic<bool>* stopped,
                                         const CycleObserver& onCycle) {
    using Clock = std::chrono::steady_clock;
    const auto path = std::filesystem::absolute(file);
    if (!Checksum::pathSize(path))
        throw std::invalid_argument("Choose a readable regular file: " + path.filename().string());
    std::ifstream input(path);
    const auto header = readHeaderLine(input, path);
    std::size_t count = 2;
    if (header == "BuildQueue deadlock case v2") {
        const auto line = readHeaderLine(input, path);
        if (line.size() != 7 || line.compare(0, 6, "Jobs: ") != 0 || line[6] < '2' || line[6] > '8')
            throw std::invalid_argument("Declare Jobs: 2 through Jobs: 8.");
        count = static_cast<std::size_t>(line[6] - '0');
    } else if (header != "BuildQueue deadlock case v1") {
        throw std::invalid_argument("Choose a BuildQueue deadlock case from demo-files/deadlock.");
    }

    DeadlockDetector detector;
    std::vector<std::array<std::string, 2>> orders;
    std::unordered_map<std::string, std::size_t> owners;
    std::vector<std::unique_ptr<TrackedMutex>> locks;
    for (std::size_t i = 0; i < count; ++i) {
        orders.push_back(readLockOrder(input, path));
        const auto& name = orders.back()[0];
        if (!owners.emplace(name, i).second)
            throw std::invalid_argument("Each job needs a different first lock.");
        locks.push_back(std::make_unique<TrackedMutex>(detector, name));
    }
    std::vector<std::size_t> targets;
    for (const auto& order : orders) {
        const auto target = owners.find(order[1]);
        if (target == owners.end())
            throw std::invalid_argument("Each second lock must belong to another job.");
        targets.push_back(target->second);
    }
    std::vector<bool> visited(count);
    std::size_t next = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (visited[next])
            throw std::invalid_argument("The lock orders must form one cycle through every job.");
        visited[next] = true;
        next = targets[next];
    }
    if (next != 0)
        throw std::invalid_argument("The lock orders must return to the first job.");

    RecoveryDemoResult result;
    result.workers.resize(count);
    result.checksums.resize(count);
    std::vector<std::exception_ptr> failures(count);
    std::mutex gate;
    std::condition_variable ready;
    std::size_t arrivals = 0;
    bool abort = false;
    auto deadline = Clock::time_point::max();
    std::atomic<int> completed{0}, victim{0};
    const bool holdCycle = waitForTimeout.count() > 0;
    const auto shouldStop = [&] { return stopped && stopped->load(); };

    auto attempt = [&](std::size_t worker) {
        try {
            result.workers[worker] = std::this_thread::get_id();
            std::lock_guard<TrackedMutex> hold(*locks[worker]);
            {
                std::unique_lock<std::mutex> lock(gate);
                if (++arrivals == count) {
                    if (holdCycle)
                        deadline = Clock::now() + waitForTimeout;
                    ready.notify_all();
                }
                ready.wait(lock, [&] { return arrivals == count || abort; });
                if (abort)
                    return;
            }
            auto& requested = *locks[targets[worker]];
            if (!requested.lockUntil(deadline, stopped, holdCycle, [&](const DeadlockReport& report) {
                if (onCycle) {
                    RecoveryDemoResult snapshot;
                    snapshot.detection = report;
                    snapshot.workers = result.workers;
                    onCycle(snapshot);
                }
            }))
                return;
            std::lock_guard<TrackedMutex> acquire(requested, std::adopt_lock);
            result.checksums[worker] = checksumJob(path, stopped);
            if (result.checksums[worker].exitCode == 0)
                ++completed;
        } catch (const DeadlockDetected&) {
            victim = static_cast<int>(worker + 1);
        } catch (...) {
            failures[worker] = std::current_exception();
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(count);
    try {
        for (std::size_t i = 0; i < count; ++i)
            threads.emplace_back(attempt, i);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(gate);
            abort = true;
        }
        ready.notify_all();
        for (auto& thread : threads)
            thread.join();
        throw;
    }
    for (auto& thread : threads)
        thread.join();
    result.detection = detector.report();
    result.completed = completed;
    result.stopped = shouldStop();
    if (result.stopped)
        return result;
    for (auto failure : failures)
        if (failure)
            std::rethrow_exception(failure);
    if (result.detection.count != 1)
        throw std::runtime_error("The jobs did not produce a complete wait-for cycle.");
    if (holdCycle) {
        result.timedOut = true;
        return result;
    }
    result.victim = victim;
    if (result.victim == 0 || result.completed != static_cast<int>(count) - 1)
        throw std::runtime_error("The remaining workers did not finish after rejecting the closing request.");
    const auto job = static_cast<std::size_t>(result.victim - 1);
    const auto first = std::min(job, targets[job]);
    const auto second = std::max(job, targets[job]);
    std::lock_guard<TrackedMutex> holdFirst(*locks[first]);
    std::lock_guard<TrackedMutex> holdSecond(*locks[second]);
    result.checksums[job] = checksumJob(path, stopped);
    result.stopped = shouldStop();
    result.recovered = !result.stopped && result.checksums[job].exitCode == 0;
    if (result.recovered)
        ++result.completed;
    return result;
}
}
