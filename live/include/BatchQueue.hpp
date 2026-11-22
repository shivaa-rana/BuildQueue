#pragma once

#include "Checksum.hpp"
#include "Semaphore.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace bq {
enum class Policy { Fcfs, Priority, Sjf, RoundRobin };
enum class Input { FolderNames, AbsolutePaths };
enum class State { Queued, Running, Completed, Failed, Cancelled };

struct JobRequest {
    std::string file;
    int priority;
};

struct Job {
    using Clock = std::chrono::steady_clock;

    std::uint64_t id = 0;
    std::uint64_t dispatchOrder = 0;
    Policy dispatchPolicy = Policy::Fcfs;
    std::string file;
    int priority = 0;
    std::optional<std::uintmax_t> estimatedBytes;
    State state = State::Queued;
    Clock::time_point submitted;
    Clock::time_point started;
    Clock::time_point finished;
    ChecksumResult result;
    Clock::time_point readySince, sliceStarted;
    Clock::duration waited{}, executed{};
    std::vector<std::pair<Clock::time_point, Clock::time_point>> slices;
    ChecksumProgress progress;
    std::uint64_t readyOrder = 0;
    bool timelineTruncated = false;

    Clock::duration waitTime(Clock::time_point now) const {
        return waited + (state == State::Queued ? now - readySince : Clock::duration{});
    }
    Clock::duration burstTime(Clock::time_point now) const {
        return executed + (state == State::Running ? now - sliceStarted : Clock::duration{});
    }
};

struct Snapshot {
    std::vector<Job> jobs;
    int workers;
    int slots;
    int running;
    int peak;
    Policy policy;
    DeadlockReport deadlocks;
};

class BatchQueue {
public:
    using Work = std::function<ChecksumResult(const std::string&)>;
    using Estimate = std::function<std::optional<std::uintmax_t>(const std::string&)>;
    using SliceWork = std::function<std::optional<ChecksumResult>(const std::string&, ChecksumProgress&)>;

    BatchQueue(int workers, int slots, Work work, Input input = Input::FolderNames,
               Estimate estimate = {}, SliceWork slice = {});
    ~BatchQueue();
    BatchQueue(const BatchQueue&) = delete;
    BatchQueue& operator=(const BatchQueue&) = delete;

    void setPolicy(Policy policy);
    void pauseDispatch(bool paused);
    std::vector<std::uint64_t> submit(const std::vector<std::string>& files, int priority);
    std::vector<std::uint64_t> submit(const std::vector<JobRequest>& requests);
    bool cancel(std::uint64_t id);
    void clearFinished();
    Snapshot snapshot() const;
    void waitUntilIdle();

private:
    void worker();
    void stop();
    std::vector<Job>::iterator nextJob();
    static bool terminal(State state);

    const int slots_;
    const Input input_;
    Work work_;
    Estimate estimate_;
    SliceWork slice_;
    DeadlockDetector detector_;
    Semaphore permits_;
    mutable TrackedMutex mutex_;
    std::condition_variable_any changed_;
    std::vector<std::thread> workers_;
    std::vector<Job> jobs_;
    Policy policy_ = Policy::Fcfs;
    std::uint64_t nextId_ = 1;
    std::uint64_t nextDispatch_ = 1;
    std::uint64_t nextReady_ = 1;
    int queued_ = 0;
    int running_ = 0;
    int peak_ = 0;
    bool stopping_ = false;
    bool paused_ = false;
};
}
