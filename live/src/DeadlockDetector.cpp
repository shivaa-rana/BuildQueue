#include "DeadlockDetector.hpp"

#include <exception>
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
    while (resource->owner_ != std::thread::id{}) {
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
    const auto requester = std::this_thread::get_id();
    std::unique_lock<std::mutex> graphLock(detector_.graphMutex_);
    if (owner_ != std::thread::id{}) {
        if (closesCycle(requester)) {
            auto edges = captureCycle(requester);
            std::ostringstream description;
            description << "Thread " << requester;
            for (const auto& edge : edges)
                description << " waits for " << edge.resource << " held by thread " << edge.owner;
            auto message = description.str();
            detector_.report_ = {detector_.report_.count + 1, message, std::move(edges)};
            throw DeadlockDetected(message);
        }
        detector_.waiting_.emplace(requester, this);
        try {
            available_.wait(graphLock, [this] { return owner_ == std::thread::id{}; });
        } catch (...) {
            detector_.waiting_.erase(requester);
            throw;
        }
        detector_.waiting_.erase(requester);
    }
    owner_ = requester;
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
