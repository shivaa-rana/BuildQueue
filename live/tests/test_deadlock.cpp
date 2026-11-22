#include "DeadlockDetector.hpp"
#include "DeadlockDemo.hpp"

#include <atomic>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    bq::DeadlockDetector detector;
    bq::TrackedMutex first(detector, "first resource");
    bq::TrackedMutex second(detector, "second resource");
    int ready = 0;
    std::mutex barrier;
    std::condition_variable bothReady;
    std::atomic<int> completed{0};
    std::atomic<int> rejected{0};

    auto worker = [&](bq::TrackedMutex& owned, bq::TrackedMutex& requested) {
        try {
            std::lock_guard<bq::TrackedMutex> hold(owned);
            {
                std::unique_lock<std::mutex> lock(barrier);
                ++ready;
                bothReady.notify_all();
                bothReady.wait(lock, [&] { return ready == 2; });
            }
            std::lock_guard<bq::TrackedMutex> acquire(requested);
            ++completed;
        } catch (const bq::DeadlockDetected&) {
            ++rejected;
        }
    };

    std::thread a([&] { worker(first, second); });
    std::thread b([&] { worker(second, first); });
    a.join();
    b.join();

    const auto report = detector.report();
    if (completed != 1 || rejected != 1 || report.count != 1 ||
        report.lastCycle.find("first resource") == std::string::npos ||
        report.lastCycle.find("second resource") == std::string::npos) {
        std::cerr << "FAIL: expected one confirmed cycle and one successful worker\n";
        return 1;
    }

    {
        std::lock_guard<bq::TrackedMutex> holdFirst(first);
        std::lock_guard<bq::TrackedMutex> holdSecond(second);
    }
    if (detector.report().count != 1) {
        std::cerr << "FAIL: uncontended locks should remain usable without a new alert\n";
        return 1;
    }
    std::cout << "Detected cycle: " << report.lastCycle << '\n'
              << "PASS: one request was rejected and the other worker finished\n";
    const auto folder = std::filesystem::path(argc > 1 ? argv[1] : "demo-files/deadlock");
    for (const auto& scenario : bq::recoveryCases) {
        const auto file = folder / scenario.filename;
        std::ifstream fixture(file);
        std::string header, countLine;
        std::getline(fixture, header);
        std::size_t count = 2;
        if (header == "BuildQueue deadlock case v2") {
            std::getline(fixture, countLine);
            count = static_cast<std::size_t>(countLine.at(6) - '0');
        }
        const auto expected = bq::Checksum::runPath(std::filesystem::absolute(file));
        if (expected.exitCode != 0)
            return 1;
        const auto recovery = bq::runLockedChecksumFile(file);
        const auto& edges = recovery.detection.cycleEdges;
        if (!recovery.recovered || recovery.completed != static_cast<int>(count) ||
            recovery.detection.count != 1 || recovery.victim < 1 ||
            recovery.victim > static_cast<int>(count) || edges.size() != count ||
            !edges.front().rejected || edges.front().waiter != recovery.workers[recovery.victim - 1]) {
            std::cerr << "FAIL: incomplete cycle or recovery in " << scenario.filename << '\n';
            return 1;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (edges[i].owner != edges[(i + 1) % count].waiter || (i && edges[i].rejected)) {
                std::cerr << "FAIL: wait-for edges must form a directed cycle\n";
                return 1;
            }
        }
        for (const auto& result : recovery.checksums) {
            if (result.exitCode != 0 || result.checksum != expected.checksum ||
                result.bytes != std::filesystem::file_size(file)) {
                std::cerr << "FAIL: both jobs must checksum the entire selected file\n";
                return 1;
            }
        }
        std::cout << "PASS: " << scenario.filename << " detected a cycle and recovered both jobs\n";
    }
    const auto multi = folder / "09-catalog-metadata.txt";
    bool observed = false;
    const auto timeout = bq::runLockedChecksumFile(multi, std::chrono::milliseconds(500), nullptr,
        [&](const bq::RecoveryDemoResult& live) {
            observed = live.detection.cycleEdges.size() == 4 && !live.detection.cycleEdges.front().rejected;
        });
    if (!observed || !timeout.timedOut || timeout.recovered || timeout.completed != 0) {
        std::cerr << "FAIL: expected an observed four-job cycle followed by timeout\n";
        return 1;
    }
    std::atomic<bool> stopped{false};
    const auto interrupted = bq::runLockedChecksumFile(multi, std::chrono::seconds(10), &stopped,
        [&](const bq::RecoveryDemoResult&) { stopped = true; });
    if (!interrupted.stopped || interrupted.timedOut || interrupted.completed != 0) {
        std::cerr << "FAIL: Stop all did not release a waiting cycle\n";
        return 1;
    }
    try {
        bq::runLockedChecksumFile(folder.parent_path() / "README.txt");
        std::cerr << "FAIL: accepted a file without a deadlock header\n";
        return 1;
    } catch (const std::invalid_argument&) {
    }
}
