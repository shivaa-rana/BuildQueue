#include "DeadlockDemo.hpp"

#include <atomic>
#include <exception>
#include <fstream>
#include <functional>
#include <utility>

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

ChecksumResult checksumJob(const std::filesystem::path& path) {
    auto result = Checksum::runPath(path);
    if (result.exitCode != 0 || !result.error.empty())
        throw std::runtime_error("Could not checksum " + path.filename().string() + ": " + result.error);
    return result;
}
}

RecoveryDemoResult runLockedChecksumFile(const std::filesystem::path& file) {
    const auto path = std::filesystem::absolute(file);
    if (!Checksum::pathSize(path))
        throw std::invalid_argument("Choose a readable regular file: " + path.filename().string());
    std::ifstream input(path);
    if (readHeaderLine(input, path) != "BuildQueue deadlock case v1")
        throw std::invalid_argument("Choose one BuildQueue deadlock case from demo-files/deadlock.");
    const auto firstOrder = readLockOrder(input, path);
    const auto secondOrder = readLockOrder(input, path);
    if (firstOrder[0] == firstOrder[1] ||
        firstOrder[0] != secondOrder[1] || firstOrder[1] != secondOrder[0])
        throw std::invalid_argument("The two jobs in the file must request distinct locks in opposite orders.");
    DeadlockDetector detector;
    TrackedMutex first(detector, firstOrder[0]);
    TrackedMutex second(detector, firstOrder[1]);
    std::mutex gate;
    std::condition_variable ready;
    int arrivals = 0;
    bool abort = false;
    std::atomic<int> completed{0};
    std::atomic<int> victim{0};
    std::exception_ptr failures[2];
    std::array<std::thread::id, 2> workers;
    std::array<ChecksumResult, 2> checksums;

    auto attempt = [&](int worker, TrackedMutex& owned, TrackedMutex& requested) {
        try {
            workers[worker - 1] = std::this_thread::get_id();
            std::lock_guard<TrackedMutex> hold(owned);
            {
                std::unique_lock<std::mutex> lock(gate);
                ++arrivals;
                ready.notify_all();
                ready.wait(lock, [&] { return arrivals == 2 || abort; });
                if (abort)
                    return;
            }
            std::lock_guard<TrackedMutex> acquire(requested);
            checksums[worker - 1] = checksumJob(path);
            ++completed;
        } catch (const DeadlockDetected&) {
            victim = worker;
        } catch (...) {
            failures[worker - 1] = std::current_exception();
        }
    };

    std::thread a(attempt, 1, std::ref(first), std::ref(second));
    std::thread b;
    try {
        b = std::thread(attempt, 2, std::ref(second), std::ref(first));
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(gate);
            abort = true;
        }
        ready.notify_all();
        a.join();
        throw;
    }
    a.join();
    b.join();
    for (auto failure : failures)
        if (failure)
            std::rethrow_exception(failure);

    const auto detection = detector.report();
    if (detection.count != 1 || completed != 1 || victim == 0)
        throw std::runtime_error("Expected one cycle and one worker to complete the first attempt.");

    // The rejected worker released its first lock during stack unwinding.
    // Retry its work after the survivor finishes, using a consistent lock order.
    {
        std::lock_guard<TrackedMutex> holdFirst(first);
        std::lock_guard<TrackedMutex> holdSecond(second);
        checksums[victim - 1] = checksumJob(path);
        ++completed;
    }
    return {detector.report(), victim, completed, true, workers, std::move(checksums)};
}
}
