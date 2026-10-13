# 04 — Job system + TaskScheduler (decompiled)

All functions below decompiled with Ghidra 12.1.4 from the `-noanalysis`
import; addresses are `ram:` (= file offset + `0x100000000`).

## Submit path

`0x10521311e` (0x260 bytes) — build + submit a work item:

* Takes scheduler singleton (`0x10520e072` → `0x107c54890`, call-once
  initialized) + client struct.
* `weak_ptr::lock`, then allocates a 0x30-byte job object with vtable
  `0x10742d0f0`, capturing the scheduler + payload shared_ptrs (LOCK-inc).
* Submits via `0x104f54dc6` (0x4a1 bytes): copies the item, calls
  `0x104f54c96(scheduler, item, queueIndex)`, destroys the temp
  (`0x104544be6`).
* `0x104f54c96` (0xf7 bytes): locks mutex at `sched+0x778`
  (`0x10320983c`/`0x103209848`), indexes queue array at `sched+0x780` by
  `index*0x10`, copies the item in, inserts with `0x104f54876(queue+0x1b0)`
  → `0x104545ba0`, then wakes workers with `0x102ee57de`.
* `0x102ee57de` (0x313 bytes): thread-primitive juggling plus
  dispatch-to-main registration (`"AppleMainThreadBlockingFix"`,
  key `_DAT_10780cf10`).

Pattern: **mutex-guarded sharded queues + explicit wakeups**, payloads are
refcounted (`LOCK`-inc/dec, `__shared_weak_count::__release_weak`).

## Guards

* `0x1051efba0` (0xc bytes): returns byte at `obj+0x108` — the "initialized"
  flag set by `AppUpdateAdapter::init` (`0x1051efaa2`, `[FLog::AppUpdate]`).
* `0x10003ddee` (0xb5 bytes): seqlock-guarded lazy global at `0x10748fb28`
  (counter `0x10748fb48`, `0x3fffffff` mask + `0x80000000` busy bit),
  resolved once via virtual `+0x30` on `0x107810840`.
* All ObjC entry points use `__stack_chk_guard` canaries.

## Frame jobs (string-observed)

`Heartbeat`, `Stepped`, `RenderStepped`, `PreRender`, `PostSimulation`,
`DeserializePacketsJob`, `WaitingHybridScriptsJob`, `GcJob`, `TelemetryJob`,
`ClearUnusedLuaRefsJob`, `LuauHeapMemoryReportProcess`
(`FLog::DataModelJobs`).

## C++ sketch

`include/rbx/scheduler.hpp` + `src/jobqueue_demo.cpp` — sharded mutex
queues, weak-ptr job capture, wake predicate; compiles and runs standalone.
