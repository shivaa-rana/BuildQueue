#include "DeadlockDetector.hpp"

#include <exception>
#include <algorithm>
#include <sstream>
#include <utility>

namespace bq {
DeadlockReport DeadlockDetector::report() const {
    std::lock_guard<std::mutex> lock(graphMutex_);
    return report_;
}

TrackedMutex::TrackedMutex(DeadlockDetector& detector, std::string name)
    : detector_(detector), name_(std::move(name)) {}

bool TrackedMutex::closesCycle(std::thread::id requester) const {
    const TrackedMutex* resource = this;
    for (std::size_t hops = 0; hops <= detector_.waiting_.size() &&
         resource->owner_ != std::thread::id{}; ++hops) {
        if (resource->owner_ == requester)
            return true;
        auto next = detector_.waiting_.find(resource->owner_);
        if (next == detector_.waiting_.end())
            return false;
        resource = next->second;
    }
    return false;
}

std::vector<WaitForEdge> TrackedMutex::captureCycle(std::thread::id requester) const {
    std::vector<WaitForEdge> edges;
    auto resource = this;
    auto waiter = requester;
    do {
        const auto owner = resource->owner_;
        edges.push_back({waiter, owner, resource->name_, edges.empty()});
        if (owner == requester)
            break;
        waiter = owner;
        resource = detector_.waiting_.at(owner);
    } while (true);
    return edges;
}

void TrackedMutex::lock() {
    acquire(std::chrono::steady_clock::time_point::max(), nullptr, false, {});
}

bool TrackedMutex::lockUntil(std::chrono::steady_clock::time_point deadline,
                            const std::atomic<bool>* stopped, bool holdCycle,
                            const std::function<void(const DeadlockReport&)>& onCycle) {
    return acquire(deadline, stopped, holdCycle, onCycle);
}

bool TrackedMutex::acquire(std::chrono::steady_clock::time_point deadline,
                          const std::atomic<bool>* stopped, bool holdCycle,
                          const std::function<void(const DeadlockReport&)>& onCycle) {
    using Clock = std::chrono::steady_clock;
    const auto interrupted = [&] { return (stopped && stopped->load()) || Clock::now() >= deadline; };
    const auto requester = std::this_thread::get_id();
    std::unique_lock<std::mutex> graphLock(detector_.graphMutex_);
    if (interrupted())
        return false;
    if (owner_ != std::thread::id{}) {
        if (closesCycle(requester)) {
            auto edges = captureCycle(requester);
            edges.front().rejected = !holdCycle;
            std::ostringstream description;
            description << "Thread " << requester;
            for (const auto& edge : edges)
                description << " waits for " << edge.resource << " held by thread " << edge.owner;
            auto message = description.str();
            detector_.report_ = {detector_.report_.count + 1, message, std::move(edges)};
            if (onCycle)
                onCycle(detector_.report_);
            if (!holdCycle)
                throw DeadlockDetected(message);
        }
        detector_.waiting_.emplace(requester, this);
        try {
            while (owner_ != std::thread::id{} && !interrupted()) {
                if (stopped)
                    available_.wait_until(graphLock, std::min(deadline, Clock::now() + std::chrono::milliseconds(20)));
                else if (deadline == Clock::time_point::max())
                    available_.wait(graphLock);
                else
                    available_.wait_until(graphLock, deadline);
            }
        } catch (...) {
            detector_.waiting_.erase(requester);
            throw;
        }
        detector_.waiting_.erase(requester);
    }
    if (interrupted())
        return false;
    owner_ = requester;
    return true;
}

void TrackedMutex::unlock() noexcept {
    {
        std::lock_guard<std::mutex> lock(detector_.graphMutex_);
        if (owner_ != std::this_thread::get_id())
            std::terminate();
        owner_ = {};
    }
    available_.notify_all();
}
}
