// Task scheduler: sharded queues, one mutex, explicit wakeups.
//
// What this shows: the submit-side discipline from 0x104f54c96.
// A caller picks a shard, the queue is locked, the item is copied in,
// the ring grows if needed, the lock is released, and a worker is woken.
// Workers pop one item per wakeup and run it.
//
// Original illustration, MIT.
#pragma once
#include <array>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace rbx {

// Example:
//
//   auto sched = TaskScheduler::create();
//   sched->submit([] { /* deserialized batch */ }, /*shard=*/3);
//   while (sched->runOne()) {}
//   sched->shutdown();
class TaskScheduler : public std::enable_shared_from_this<TaskScheduler> {
public:
    static constexpr size_t kShards = 8;
    using Job = std::function<void()>;

    static std::shared_ptr<TaskScheduler> create() {
        return std::shared_ptr<TaskScheduler>(new TaskScheduler());
    }

    // Copy the job into the shard queue, then wake one worker.
    // Mirrors the lock / index*0x10 / copy / insert / wake order at
    // 0x104f54c96. The real ring stores 0x80-byte records and grows
    // capacity by 1.5x; here std::queue keeps the same ownership shape.
    void submit(Job job, size_t shard = 0) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queues_[shard % kShards].push(std::move(job));
        }
        wake_.notify_one();
    }

    // Pop one job or sleep until woken. Returns false after shutdown.
    bool runOne() {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return shutdown_ || hasWork(); });
            if (shutdown_) return false;
            job = std::move(nextQueue().front());
            nextQueue().pop();
        }
        job();
        return true;
    }

    void shutdown() {
        { std::lock_guard<std::mutex> lock(mutex_); shutdown_ = true; }
        wake_.notify_all();
    }

private:
    TaskScheduler() = default;
    bool hasWork() const {
        for (auto& q : queues_) if (!q.empty()) return true;
        return false;
    }
    std::queue<Job>& nextQueue() {
        for (auto& q : queues_) if (!q.empty()) return q;
        return queues_[0];
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::array<std::queue<Job>, kShards> queues_;
    bool shutdown_ = false;
};

// Bounded node pool, mirroring 0x102ee3136: acquisition can fail,
// publication is atomic, exhaustion is fatal, and each node carries a
// deadline used by the arbiter.
struct JobNode {
    JobNode() = default;
    bool active = false;
    double deadline = 0.0;
    TaskScheduler::Job job;
};

} // namespace rbx
