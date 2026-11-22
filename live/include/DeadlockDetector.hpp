#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bq {
class TrackedMutex;

class DeadlockDetected : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct WaitForEdge {
    std::thread::id waiter;
    std::thread::id owner;
    std::string resource;
    bool rejected;
};

struct DeadlockReport {
    std::size_t count = 0;
    std::string lastCycle;
    std::vector<WaitForEdge> cycleEdges;
};

class DeadlockDetector {
public:
    DeadlockReport report() const;

private:
    friend class TrackedMutex;
    mutable std::mutex graphMutex_;
    std::unordered_map<std::thread::id, const TrackedMutex*> waiting_;
    DeadlockReport report_;
};

class TrackedMutex {
public:
    TrackedMutex(DeadlockDetector& detector, std::string name);
    TrackedMutex(const TrackedMutex&) = delete;
    TrackedMutex& operator=(const TrackedMutex&) = delete;
    void lock();
    void unlock() noexcept;

private:
    bool closesCycle(std::thread::id requester) const;
    std::vector<WaitForEdge> captureCycle(std::thread::id requester) const;

    DeadlockDetector& detector_;
    const std::string name_;
    std::condition_variable available_;
    // The same mutex guards ownership and the wait-for graph.
    std::thread::id owner_;
};
}
