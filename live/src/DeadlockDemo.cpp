#include "DeadlockDemo.hpp"

#include <atomic>
#include <exception>
#include <fstream>
#include <functional>

namespace bq {
namespace {
std::string readResourceLabel(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string name;
    if (!std::getline(input, name) || name.empty())
        throw std::runtime_error("Cannot read demo resource: " + path.string());
    if (name.back() == '\r')
        name.pop_back();
    return name;
}

void readResource(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    char byte;
    if (!input.get(byte))
        throw std::runtime_error("Cannot read demo resource: " + path.string());
}
}

RecoveryDemoResult runRecoveryDemo(const std::filesystem::path& firstFile,
                                   const std::filesystem::path& secondFile) {
    DeadlockDetector detector;
    TrackedMutex first(detector, readResourceLabel(firstFile));
    TrackedMutex second(detector, readResourceLabel(secondFile));
    std::mutex gate;
    std::condition_variable ready;
    int arrivals = 0;
    bool abort = false;
    std::atomic<int> completed{0};
    std::atomic<int> victim{0};
    std::exception_ptr failures[2];
    std::array<std::thread::id, 2> workers;

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
            readResource(firstFile);
            readResource(secondFile);
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
        readResource(firstFile);
        readResource(secondFile);
        ++completed;
    }
    return {detector.report(), victim, completed, true, workers};
}
}
