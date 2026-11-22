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
        std::string header, first, second;
        if (!std::getline(fixture, header) || !std::getline(fixture, first) ||
            !std::getline(fixture, second) || first.rfind("Lock first: ", 0) != 0 ||
            second.rfind("Lock second: ", 0) != 0) {
            std::cerr << "FAIL: invalid fixture " << scenario.filename << '\n';
            return 1;
        }
        const std::array<std::string, 2> lockNames{first.substr(12), second.substr(13)};
        const auto expected = bq::Checksum::runPath(std::filesystem::absolute(file));
        if (expected.exitCode != 0)
            return 1;
        const auto recovery = bq::runLockedChecksumFile(file);
        const auto& edges = recovery.detection.cycleEdges;
        if (!recovery.recovered || recovery.completed != 2 || recovery.detection.count != 1 ||
            recovery.victim < 1 || recovery.victim > 2 || edges.size() != 2) {
            std::cerr << "FAIL: expected one cycle and recovery of both jobs in " << scenario.filename << '\n';
            return 1;
        }
        const auto victim = recovery.victim - 1;
        if (!edges[0].rejected || edges[1].rejected ||
            edges[0].waiter != recovery.workers[victim] ||
            edges[0].owner != recovery.workers[1 - victim] ||
            edges[1].waiter != edges[0].owner || edges[1].owner != edges[0].waiter ||
            edges[0].resource != lockNames[1 - victim] || edges[1].resource != lockNames[victim]) {
            std::cerr << "FAIL: graph has incorrect directions or lock names\n";
            return 1;
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
    try {
        bq::runLockedChecksumFile(folder.parent_path() / "README.txt");
        std::cerr << "FAIL: accepted a file without a deadlock header\n";
        return 1;
    } catch (const std::invalid_argument&) {
    }
}
