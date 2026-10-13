# TaskScheduler + job system

The engine does almost no work inline. ObjC entry points, Lua callbacks,
network receives, and frame steps all become **jobs**: small refcounted
objects submitted to sharded mutex-guarded queues, run by worker threads, with
explicit cross-thread wakeups.

## Submit path (`ram:0x10521311e` → `0x104f54dc6` → `0x104f54c96`)

1. Caller holds the scheduler singleton (`0x10520e072` → `0x107c54890`,
   call-once initialized) and a payload struct of shared_ptrs.
2. `0x10521311e` (0x260 bytes) locks the payload's `weak_ptr`, allocates a
   0x30-byte job object (vtable `0x10742d0f0`) capturing scheduler + payload
   with `LOCK`-guarded refcount bumps, then calls `0x104f54dc6`.
3. `0x104f54dc6` (0x4a1 bytes) copies the item and calls
   `0x104f54c96(scheduler, item, queueIndex)`, then destroys the temp
   (`0x104544be6`).
4. `0x104f54c96` (0xf7 bytes):
   - lock mutex at `sched+0x778`, unlock on scope exit;
   - index the queue array at `sched+0x780` by `index * 0x10`;
   - copy the item in, insert with `0x104f54876(queue+0x1b0)` →
     `0x104545ba0`;
   - wake workers with `0x102ee57de`.
5. `0x102ee57de` (0x313 bytes): thread-primitive juggling plus
   dispatch-to-main registration (`"AppleMainThreadBlockingFix"`,
   key `_DAT_10780cf10`) — this is also how background threads bounce UI work
   to the AppKit thread.

## Queue internals

* `0x104545ba0` (0xc3 bytes) is an **open-addressing hash insert** over
  0x80-byte slots: placement `(base + hash) % capacity`, growth factor 1.5x
  (`cap*3>>1`, minimum 4), full rehash through `0x104545c64`. The queue index
  is a hash, not a plain array slot.
* `0x10453826a` (0x52 bytes) is the item copy used at enqueue time — payloads
  are copied under the lock, never referenced.
* Worker side waits on a predicate and pops one item per wakeup (see
  `TaskScheduler::runOne` in `include/rbx/scheduler.hpp`).

## Guard patterns used everywhere

| Pattern | Shape | Example |
|---|---|---|
| Init flag | 1-byte check + call-once, `+0x108` on client structs | `0x1051efba0` (0xc bytes) reads it; `AppUpdateAdapter::init` sets it |
| Seqlock lazy global | 30-bit counter + `0x80000000` busy bit, resolve-once via vtable `+0x30` | `0x10003ddee` → `0x10748fb28` |
| Call-once singletons | guard byte + init + `cxa_atexit` destructor | `0x10118fab6` → `0x107a61588`, `0x105235e96` → `0x107510308` |
| Shared ownership | `LOCK`-inc/dec, `__shared_weak_count::__release_weak` at zero | every job, event, and DataModel handoff |
| Stack protection | `__stack_chk_guard` canary + `___stack_chk_fail` | every ObjC entry and engine function above |

## Frame jobs

Each frame the scheduler runs, among others:

| Job | Role |
|---|---|
| `Heartbeat` / `Stepped` | fixed-step simulation + script stepping |
| `RenderStepped` / `PreRender` / `PostSimulation` | render sync points scripts can hook |
| `DeserializePacketsJob` | drain the network receive queue into the replicator |
| `WaitingHybridScriptsJob` | resume yielded script threads (Lua coroutines) |
| `ParallelLuauWorkQueue::executePendingWork` | Actor-isolated parallel Lua |
| `GcJob` / `ClearUnusedLuaRefsJob` | incremental Luau GC assist + stale ref cleanup |
| `TelemetryJob` / `LuauHeapMemoryReportProcess` | sampling + memory telemetry |

`include/rbx/scheduler.hpp` + `src/jobqueue_demo.cpp` model the
mutex/shard/wake discipline with worker threads.
