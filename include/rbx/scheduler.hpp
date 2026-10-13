// Illustrative reconstruction: sharded job queue behind one mutex,
// weak-ptr job capture, explicit wakeups (cf. 0x104f54c96 shape).
// Original code, MIT.
#pragma once
#include <array>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace rbx {

class TaskScheduler : public std::enable_shared_from_this<TaskScheduler> {
public:
    static constexpr size_t kShards = 8;
    using Job = std::function<void()>;

    static std::shared_ptr<TaskScheduler> create() {
        return std::shared_ptr<TaskScheduler>(new TaskScheduler());
    }

    // Enqueue onto shard (index*stride layout mirrors +0x780[idx*0x10]).
    void submit(Job job, size_t shard = 0) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queues_[shard % kShards].push(std::move(job));
        }
        wake_.notify_one();
    }

    // Worker loop: pop one job or sleep until woken (cf. 0x102ee57de role).
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

} // namespace rbx
