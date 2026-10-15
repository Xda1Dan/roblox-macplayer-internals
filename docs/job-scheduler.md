# Scheduler reference

In one paragraph: almost nothing runs inline. App callbacks, network
receives, Lua callbacks, and frame steps become small refcounted jobs.
They are copied into sharded queues under a mutex, appended to a ring,
and run by workers after an explicit wakeup.

## Submit path

Job build (`0x10521311e`) → dispatch (`0x104f54dc6`) → enqueue (`0x104f54c96`)

1. The caller holds the scheduler singleton (`0x10520e072` →
   `0x107c54890`, initialized once) and a payload of shared pointers.
2. Job build (`0x10521311e`, 0x260 bytes) locks the payload, builds a 0x30-byte job
   object (vtable `0x10742d0f0`), bumps reference counts atomically, and
   calls job dispatch (`0x104f54dc6`).
3. Job dispatch (`0x104f54dc6`, 0x4a1 bytes) copies the item, calls
   enqueue (`0x104f54c96(scheduler, item, queueIndex)`), then destroys the temporary
   (`0x104544be6`).
4. Enqueue (`0x104f54c96`, 0xf7 bytes):
   - lock the mutex at `scheduler+0x778`, unlock on scope exit;
   - select the queue at `scheduler+0x780` by `index * 0x10`;
   - copy the item in, insert with ring insert (`0x104f54876(queue+0x1b0)`) into
     the record ring (`0x104545ba0`);
   - wake workers with the wakeup helper (`0x102ee57de`).
5. The wakeup helper (`0x102ee57de`, 0x313 bytes) handles thread wakeup plus main-thread
   dispatch (`AppleMainThreadBlockingFix`, key `_DAT_10780cf10`). This is
   also how background threads hand UI work to the app thread.

Behavior in code form:

```cpp
// Simplified illustration.
void submit(Job job, size_t shard = 0) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queues_[shard % queues_.size()].push(std::move(job));
  }
  wake_.notify_one();
}
```

## Queue storage

- The record ring (`0x104545ba0`, 0xc3 bytes) copies 0x80-byte records into a circular
  buffer. Capacity grows by 1.5x (`cap*3/2`, minimum 4), then records move
  using the old head offset and current count. Placement is
  `((count + head) % capacity) * 0x80`. This is a ring, not a hash table.
- The item copier (`0x10453826a`, 0x52 bytes) copies the item at enqueue time. Payloads are
  copied under the lock, never referenced.
- Workers wait on a predicate and pop one item per wakeup. See
  `TaskScheduler::runOne` in `include/rbx/scheduler.hpp`.

## Common patterns

| Pattern | Shape | Example |
|---|---|---|
| Init flag | 1-byte check plus once-only init, `+0x108` on client structs | `0x1051efba0` reads it; app-update init sets it |
| Lazy global with busy bit | 30-bit counter plus `0x80000000` busy bit, resolve once via vtable `+0x30` | `0x10003ddee` → `0x10748fb28` |
| Once-only singleton | guard byte, init, destructor at exit | `0x10118fab6` → `0x107a61588`, `0x105235e96` → `0x107510308` |
| Shared ownership | atomic increment and decrement, weak release at zero | every job, event, and DataModel handoff |
| Stack guard | canary plus failure handler | every app entry and engine function above |

## Frame jobs

| Job | Role |
|---|---|
| `Heartbeat` / `Stepped` | fixed-step simulation and script stepping |
| `RenderStepped` / `PreRender` / `PostSimulation` | render sync points for scripts |
| `DeserializePacketsJob` | drain the network receive queue |
| `WaitingHybridScriptsJob` | resume yielded script threads |
| `ParallelLuauWorkQueue::executePendingWork` | Actor-isolated parallel Lua |
| `GcJob` / `ClearUnusedLuaRefsJob` | incremental GC help and stale-reference cleanup |
| `TelemetryJob` / `LuauHeapMemoryReportProcess` | sampling and memory telemetry |

See `include/rbx/scheduler.hpp` and `src/jobqueue_demo.cpp`.

## Run and node pool

The start wrapper at `0x102eec6da` marks a job active, logs arbiter
and start time, then calls the step function at vtable offset `+0x10`.
Dispatch is polymorphic. There is no large switch over job types.

The stop wrapper at `0x102eec76c` clears the active bit, calls the
stop hook at `+0x18`, records elapsed and stop time, updates the arbiter,
and emits `JobStop`. Profiling builds also record worker thread and job
name in a bounded diagnostic ring.

The allocator at `0x102ee3136` takes a free node from a bounded pool,
copies shared job state with atomic increments, updates a
deadline-like atomic value, and publishes through an indexed table with
locked compare-and-exchange behavior. Exhaustion is fatal:

```text
Out of job nodes. Check for job leaks and/or increase the
FInt::TaskSchedulerMaxNumOfJobs value
```

The diagnostic at `0x102ee6a60` walks active jobs and prints name,
state, and elapsed seconds. Limits exist for jobs, arbiters, deadlines,
threads, stack sizes, and per-cycle budgets.

## Worker pop and run

The worker side mirrors enqueue. At `0x102eeac76` (`TS::Reschedule`
region) the worker locks the same queue record enqueue inserts into
(`+0x1b0`), pops the next job, and promotes its weak reference with
`shared_weak_count::lock`. An expired job simply never runs. The live job
goes through the worker dispatch stub (`0x102ee3332`), which re-checks the lock state and makes
the virtual step call at vtable offset `+0x10`.

```cpp
// Simplified illustration.
void workerPopAndRun(QueueRecord& rec) {
  std::unique_lock lock(rec.mutex);   // +0x1b0 record
  auto weak = rec.tryPop();
  lock.unlock();
  if (auto job = weak.lock())         // weak -> shared promote
    job->step(stats);                 // virtual dispatch
}                                     // shared release on scope exit
```

Behavior in code form:

```cpp
// Simplified illustration.
JobNode* node = nodePool.acquire();  // bounded; empty is fatal
node->job = sharedJob;
node->deadline = job->deadline();
arbiter.publish(node);               // atomic
worker:
  node = arbiter.next();
  node->active = true;
  node->job->step(stats);            // virtual dispatch
  nodePool.release(node);
```

The C++ example uses `std::function` queues on purpose. It shows the
mutex, shard, and wakeup discipline without claiming private table
offsets.

## Addresses in this build

| Place | Address |
|---|---|
| Scheduler singleton | `0x10520e072` → `0x107c54890` |
| Job build | `0x10521311e` |
| Job dispatch | `0x104f54dc6` |
| Enqueue | `0x104f54c96` |
| Ring insert | `0x104f54876` → `0x104545ba0` |
| Wake | `0x102ee57de` |
| Job start | `0x102eec6da` |
| Job stop | `0x102eec76c` |
| Node allocator | `0x102ee3136` |
| Worker pop | `0x102eeac76` |
| Worker dispatch | `0x102ee3332` |
| Job diagnostics | `0x102ee6a60` |

## What is not claimed

- Worker entry and work-stealing policy between shards.
- Exact arbiter priority weights.
- Per-class cycle budgets for other builds.
