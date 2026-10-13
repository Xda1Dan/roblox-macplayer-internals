// Demo: sharded job queue with weak-ptr capture + wakeups,
// mirroring the observed submit path. Build: make jobqueue_demo
#include <atomic>
#include <cstdio>
#include <thread>
#include "rbx/scheduler.hpp"

int main() {
    auto sched = rbx::TaskScheduler::create();
    std::atomic<int> done{0};
    auto workerData = std::make_shared<int>(42); // payload like a job struct
    std::weak_ptr<int> weak = workerData;        // weak lock before running
    for (int i = 0; i < 4; ++i) {
        sched->submit([weak, &done, i] {
            if (auto pinned = weak.lock()) {
                std::printf("job %d ran with payload %d\n", i, *pinned);
                ++done;
            }
        }, /*shard=*/static_cast<size_t>(i));
    }
    std::thread worker([&] {
        while (done.load() < 4) sched->runOne();
        sched->shutdown();
    });
    worker.join();
    std::printf("done=%d\n", done.load());
    return 0;
}
