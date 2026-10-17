# RobloxPlayer (macOS) — Reverse Engineering Report

**Target:** `MacPlayer-version-f9247f9560044102/RobloxPlayer.app/Contents/MacOS/RobloxPlayer`
**Format:** Mach-O (x86-64), ~120 MB, Objective-C++ / C++ (libc++), stripped of internal names
**Analyzed with:** Ghidra MCP (headless instance on `http://127.0.0.1:8089`, project `tmp-rea-ghidra/ghidra-proj`)

> **Status — early report.** This was the first pass over the binary and is kept for history. Where
> it disagrees with the later verified write-ups it is superseded by those (`docs/` and
> `vm-research/luau-vm.md`). Two figures below are stale: the function count was a
> partial-analysis snapshot, and after full auto-analysis the image has **~549,782 functions**, not
> ~2,518. Corrected offsets live in `offsets.md` and `vm-research/`.

---

## 1. High-level architecture

RobloxPlayer is a **Cocoa (Objective-C++) application**. The Mach-O entry point is a thin stub that
parses the bundle/resources directory and calls `NSApplicationMain`. All engine initialization happens
from Cocoa delegate callbacks (`applicationDidFinishLaunching`, etc.), which spin up the C++ engine.

```
entry (0x100041fe2)
  └─ CFBundleGetMainBundle / CFBundleCopyResourcesDirectoryURL
  └─ chdir(resources)
  └─ parse argv / read "BaseUrl"
  └─ NSApplicationMain(argc, argv)          ← Cocoa main run loop
        └─ (ObjC app delegate) → engine init → DataModel creation → RunService loop
```

The engine subsystems map cleanly onto the areas you asked about:

| Subsystem | Key evidence |
|-----------|--------------|
| **ScriptContext** | `ScriptContext` FVar + `[FLog::ScriptContext]`, resume trampoline, C-stack manager, coroutine yield queue |
| **TaskScheduler** | `TaskScheduler` class/flag, `TaskSchedulerInit`/`TaskSchedulerRun` FVars, `GenericJob::Step`, job profiler |
| **DataModel** | `[FLog::CloseDataModel]` service-clearing sequence, `Create DataModel`, content init / patch load |
| **RakNet / networking** | `RakNet*` flags (legacy UDP) + `RbxTransportRnaExpConnection` (new QUIC/WebTransport) |

---

## 2. Two global infrastructure systems

Almost everything in the engine is governed by two singleton-ish systems that are referenced *everywhere*.

### 2.1 FVar / FFlag system (tunable flags)
- Registration call: `func_0x000102f4a716(uint64_t* out_flag, int kind, char* name)` — creates a
  runtime flag instance stored at a fixed table slot and associates it with a string name.
- The registry init function is **`sub_104c18388`** — it registers a large batch of flags, including
  `ScriptContext`, `ScriptContextAdd`, `ScriptContextRemove`, `ScriptContextClose`, `LegacyLock`,
  `ContentProviderRequests`, `MouseCommand`, `WeakThreadRef`, `CoreScripts`, `LuaMemoryPool`,
  `NetworkCache`, `NetworkInstances`, `NetworkJoin`, `Safety`, `PlayerShutdownLuaTimeoutSeconds`, etc.
- Flag instances live in a table around `0x10789f860`–`0x10789fa30`. Each slot holds a value + a
  pointer + the name string.
- A second, **static pointer table at `0x1073b8040`** holds pointers to hundreds of flag/class name
  strings (`TaskScheduler`, `SchedulerDutyCycle`, `SchedulerRate`, `ThreadPoolSize`, `SurfaceSelection`,
  `GetTeams`, `RebalanceTeams`, `TeleportData`, …). This is the bulk flag/class name directory.

### 2.2 FLog system (structured logging)
- Core log call: `func_0x000102f6fdf0(int level, char* channel, char* format, ...)`.
- Variants: `func_0x0001001107b8(...)` (used for `[FLog::Output] {}`), `func_0x000102f6d5d1(level, msg)`
  (fatal asserts, e.g. `"Script execution is not allowed in this context"`, `"Invalid Facet Access"`).
- Channels are the `[FLog::X]` / `[DFLog::X]` prefixes seen throughout: `ScriptContext`, `CloseDataModel`,
  `TaskSchedulerRun`, `TaskSchedulerInit`, `RbxTransport`, `RbxTransportRnaExpConnection`, `DataModelJobs`, …
- Logging is level-gated: e.g. `if (2 < (level >> 8) && 5 < level)` gates verbose per-channel output.

---

## 3. ScriptContext — script execution

This is the most important subsystem for "how it executes scripts". It is a **coroutine-based VM host**
(Luau) with an explicit C stack.

### 3.1 Lifecycle
- Flags/events: `ScriptContextAdd`, `ScriptContextRemove`, `ScriptContextClose` (registered in §2.1).
- Log channel `[FLog::ScriptContext]` with messages:
  - `"Resuming script: %p"`, `"Pending script name: %s"`, `"Running script %p"`,
    `"Resuming waiting threads"`, `"Resuming delayed threads"`.
- **Facet access pattern** — `ScriptContext` is composed of *facets* (sub-modules):
  `LoaderFacet::addScript/removeScript/disassociateState`, `SchedulerFacet::resumeWaitingScripts`,
  `SchedulerFacet::~SchedulerFacet`, `VmCollectionFacet::~VmCollectionFacet`, `RomarkProfilerFacet`.
- Facet accessor **`FUN_101556664`**: checks `*(int*)(ctx + 0xa18) >= 3` (lifecycle level must be ≥ 3,
  else `"Invalid Facet Access"`), then returns `ctx - 0x888` (the facet sub-object).

### 3.2 Script execution wrappers — `sub_101556235` / `sub_1015562c1`
These two are the **ScriptContext execute/resume entry points** (the two variants differ in one setup
call — likely "run" vs "resume" or "client" vs "server" context). Flow:
1. Call `sub_1015566d6()` — the resume trampoline (see §3.3).
2. Call context setup: `func_0x000101542be0`, then `func_0x000102e8b022` (or `func_0x0001018f5858` in the
   second variant), then `func_0x0001016fb4b0`.
3. Manage the ScriptContext `shared_ptr`.
4. **Read the script's current status** `*(int*)(ctx - 0x1c0)`:
   - `0` → return 0 (suspended / no-op)
   - `1` → return 1 (already yielded / running elsewhere)
   - `6` → error path: return status 1 with code 6
   - otherwise → `2` (continuing)
5. If the VM lane is not ready (`*(long*)(lane+0x78)==0 || *(long*)(lane+0x50)==0`), **grow the C stack**
   by `0x708` bytes via `func_0x000101552990(stack_ptr, lua_value_at +0x28)`.
6. Fire completion signal via `func_0x000102f65de9(status)`.
7. Guard: if the C stack can't be set up → `func_0x000102f6d5d1(0, "Script execution is not allowed in this context")` (fatal).

### 3.3 Resume trampoline — `sub_1015566d6`
The function that actually drives a script forward. It:
1. Logs `[FLog::ScriptContext] Resuming script: %p` (gated by the `ScriptContext` FVar level) and
   `"Pending script name: %s"`.
2. **C-stack depth check**: if depth `> 200` → raise `"C stack overflow"` via `FUN_1015566ae` and return 5.
3. Calls `FUN_1015c486e(ctx)` — **shutdown-deadline check**: if `*(byte*)(ctx+0x40) & 1` (shutdown
   started) and `*(double*)(ctx+0x38) <= now` → `"Not running script because past shutdown deadline"`.
4. Increments the depth counter at `ctx+0`.
5. Calls the **core VM executor** `func_0x000102f95b7f` (runs Luau bytecode until yield/error/return).
6. On the value path: `FUN_102995af5` (resume + pop return values); on the no-value path:
   `FUN_102995dc4` (resume + drain yield queue).
7. On the first call (depth==1) it records wall-clock start/end (`func_0x000102f7ea5c`) for job timing.

### 3.4 C-stack manager — `FUN_10298534b(ctx, n)`
Manages the explicit per-script C stack (16-byte entries, libc++-style SSO not used here — raw slots).
- Rejects if `n >= 0x1f41` (~8000) entries.
- Stack pointer at `ctx+0x20`, limit at `ctx+0x28`; entry count = `(sp - base) >> 4`.
- High-water mark updated at `ctx+8`.
- Grows the stack by calling `sub_102995084` when the new top would exceed the limit.

### 3.5 Coroutine resume / yield queue — `FUN_102995dc4` / `FUN_102995af5`
- `func_0x000102995b7f` — the **core VM resume** (the actual `luau`-style execute loop).
- After execution, invoke the ScriptContext vtable callbacks:
  - `*(code**)(*(long*)(ctx+0x18) + 0x6a0)` — **pre-execution** hook
  - `*(code**)(*(long*)(ctx+0x18) + 0x640)` — **post-execution** hook
- `FUN_102995af5(ctx, arg, n)` — resume and **pop `n` return values** off the C stack
  (`stack_ptr + n*-0x10`), then set new status via `func_0x000102995cd2`.
- `FUN_102995dc4(ctx, arg)` — resume **without** values; walks a queue of 48-byte (`0x30`) yield entries
  at `ctx+8`, checking flag `*(byte*)(entry+0x2c) & 2` (waiting/yielded bit), growing the stack as needed.
- ScriptContext vtable is at `ctx+0x18`; the **state byte** is at `ctx+0x58`.

### 3.6 ScriptContext object layout (approximate)
| Offset | Type | Meaning |
|--------|------|---------|
| `+0x00` | int | C-stack depth counter |
| `+0x08` | uint64 | C-stack high-water mark |
| `+0x10` | long | (related) |
| `+0x18` | ptr | **vtable** (callbacks `+0x640`, `+0x6a0`) |
| `+0x20` | long | C-stack pointer |
| `+0x28` | long | C-stack base/limit |
| `+0x38` | double | shutdown deadline (wall clock) |
| `+0x40` | byte | bit0 = shutdown started |
| `+0x58` | byte | script state |
| `+0xa18` | int | lifecycle level (must be ≥ 3 for facet access) |
| `-0x888` | ptr | facet sub-object base |

---

## 4. TaskScheduler — job system

The TaskScheduler runs the engine's per-frame and background jobs.

### 4.1 FVars
`TaskSchedulerInit`, `TaskSchedulerRun`, `TaskSchedulerJobInitThreads`, `TaskSchedulerJobInGameThreads`,
`TaskSchedulerJobDeadlineMs`, `TaskSchedulerMaxNumOfJobs`, `TaskSchedulerMaxNumOfArbiters`,
`TaskSchedulerMaxStackSizeBytes`, `TaskSchedulerTaskMaxStackSizeBytes`, `TaskSchedulerTasksDefaultMaxCountPerCycle`,
`TaskSchedulerTasksDefaultMaxTimeMsPerCycle`, `TaskSchedulerAutoThreadLimit`, `TaskSchedulerThreadMin`,
`TaskSchedulerTargetFps`, `TaskSchedulerBackgroundCycleRateMs`, `TaskSchedulerWatchThreads`, …

### 4.2 Job list & Job object layout
The TaskScheduler object holds a **job list** (`std::vector`-like) at `+0xd0` (begin) / `+0xd8` (end),
with **16-byte entries** (2 pointers each). Each entry points to a Job object:
| Offset | Type | Meaning |
|--------|------|---------|
| `+0x00` | ptr | Job object (vtable) |
| `+0x18` | std::string | **Job name** (libc++ SSO: inline if `*(byte*)(+0x18)&1==0` at `+0x19`, else heap at `+0x28`) |
| `+0x48` | double | accumulated/last time in job |
| `+0x58` | byte | **state** |
| `+0xe0` | int | timing/profiling counter |

### 4.3 Job profiler — `sub_102ee6a60`
Walks the job list and builds a log line per job:
`"TaskScheduler::Job: <name>, state: <N>[, seconds spend in job: <T>]\n"` and emits it via the
`[FLog::Output] {}` channel. This is the telemetry/hang-detection dump.

### 4.4 GenericJob::Step — `sub_1045455d3`
The per-job step driver. It:
1. Acquires a `shared_ptr` to the job's target at `job+0x1a8` / `job+0x1a0` (a DataModel weak ref).
2. Checks `*(uint*)(job+0x164) < 9` against a bitmask `0x183` (which job types are enabled this cycle).
3. Calls `func_0x000104f55d6c(tmp, job+0x1f0)` — reads job-specific data at `+0x1f0`.
4. Calls `func_0x000104545776()` — the actual job execute.
5. Calls `func_0x000104f5dac(tmp)` — cleanup.

### 4.5 Known jobs
- **Heartbeat** (`RunService.Heartbeat`, `HeartbeatTime`/`HeartbeatTimeMs`, `prof/Jobs/Heartbeat`,
  `RunService.Heartbeat` event)
- **Render** (`RenderJob`, `PreRenderJob`, `Render Duty`, `Render FPS`, `Render Nominal FPS`,
  `BindToRenderStep`, `PreRender`, `RenderStepped`)
- **Physics** (`StepPhysics`, `WorldRoot::StepPhysics`, `PhysicsStepTime`, `PreSimulation`,
  `PostSimulation`, `PreAnimation`, `RunService.Stepped`, `RunService.PostSimulation`)
- **DataModel jobs** (`TextScraper::ClientScraperJob::stepDataModelJob`, `DynamicTranslation…stepDataModelJob`,
  `DataModelRecorderJob`, `LoadTestCoordinator`)

---

## 5. DataModel — the game container

### 5.1 Lifecycle (`[FLog::CloseDataModel]` channel reveals the full sequence)
**Creation:**
1. `Create DataModel - heartbeat(%d)` — heartbeat job setup
2. `Concurrent contentInitialization start` / `Async contentInitialization start` / `Sync contentInitialization start`
3. `Concurrent patchLoad start` → `patchLoad complete, can continue`
4. `DataModel change tracker` (DMCT) — enabled only if free RAM / system RAM / CPU cores / HTTP bandwidth
   above thresholds (else `DMCT disabled for session`)
5. `DataModel created: {}`
6. `Setting place version %u` / `Setting place ID %lld` / `Setting DataCenter ID {}`

**Teardown** (`doCloseDataModel` → `Raising close..` → `Flushing defer queue...`):
1. `Removing all players`
2. `Clearing contents...` → `Removing all Children...` → `Resetting workspace...`
3. `Resetting PhysicsInstructions...`
4. `Removing GenericJobs...`
5. `Closing Script Context`
6. **Clearing services** (each with an instance count):
   `Workspace`, `World`, `Lighting`, `Teams`, `ChatService`, `TextChatService`, `StarterGui`, `CoreGui`,
   `StarterPack`, `StarterPlayer`, `JointsService`, `ChangeHistoryService`, `ReplicatedFirst`,
   `ReplicatedStorage`, `RobloxReplicatedStorage`, `ServerStorage`, `ServerScriptService`,
   `BackendServerScriptService`, `BackendReplicatedStorage`, `BackendServerStorage`, `LocalizationService`,
   `TestService`
7. `Close DataModel Done!`

### 5.2 Services are DataModel children
Services are looked up by name from the DataModel: `TimerService`, `MarketplaceService`, `Workspace`,
`RunService`, `Lighting`, `Players`, `ReplicatedStorage`, `StarterGui`, `CoreGui`, `Teams`, `ChatService`,
`LocalizationService`, `TextChatService`, `ChangeHistoryService`, `JointsService`, `TestService`, etc.
Events: `DataModelAdded`, `DataModelSessionStarted`, `DataModelSessionEnded`, `CurrentDataModelTypeChanged`,
`FocusedDataModelSession`.

---

## 6. RunService — the frame loop

The heartbeat/frame loop is driven by `RunService`, with these ordered per-frame events (confirmed via
`prof/*` and FLog strings):
```
PreRender → RenderStepped → PreSimulation → Stepped (simulation) → PostSimulation → PreAnimation → Heartbeat
```
`BindToRenderStep` / `UnbindFromRenderStep` register render callbacks. `RunService` is required for
physics (`WorldRoot::StepPhysics: RunService was not available…`) and animation.

---

## 7. Networking — RbxTransport (RakNet legacy + Rna/QUIC new)

Roblox is mid-migration; **two transports coexist**.

### 7.1 RakNet (legacy UDP)
- FVars: `RakNetResendMaxThresholdTimeInUs`, `RakNetResendScaleBackFactorHundredthsPercent`,
  `RakNetUseSlidingWindow2_*` (congestion control), `RakNetApplicationFeedback*`, `RakNetPingFrequencyMillisecond`,
  `RakNetMtuValue1/2/3InBytes`, `RakNetBanOnNetworkMisbehavior*`, `RakNetDetectRecvThreadOverload*`,
  `RakNetSocketSendTimeoutMs`, `RakNetSocketRecvTimeoutMs`, `RakNetLoopMs`, `RakNetPktTrace`, `RakNetPingTrace`.
- Memory scopes: `raknet/raknet`, `raknet/startup`, `raknet/recv-buffer`, `raknet/tx-outgoing`,
  `raknet/rx-ordered-heap`, `raknet/ack-nak`, `raknet/datagram-history`, …
- Socket logs: `[FLog::Network] RakNet Socket Open/Close`, `RakNet Socket bound to invalid (privileged) port`.

### 7.2 Rna / RbxTransport (new QUIC/WebTransport)
- Connection class: **`RbxTransportRnaExpConnection`** — uses **QUIC streams**:
  `QUIC stream accepted`, `QUIC handshake state changed`, `Handshake complete. scid=… dcid=…`,
  `QUIC connection closed with reason`, `QUIC stream acked data. streamId, offset, dataLen`.
- Channels: `RbxTransport->RakNet` / `RakNet->RbxTransport` (the bridge between transports).
- TLS/WebTransport certs: `RbxTransportRtcIoCertValidityMs`, `…CertRotationIntervalMs`, `…CertRetirementDelayMs`,
  `Failed to generate a replacement RtcIo WebTransport certificate`.
- Channels/reliability: `Opening send channel: app=… channelId=… reliability=…`, `Unreliable datagram dropped`,
  `RbxTransportRxOverflow`/`TxOverflow` (backpressure), `RbxTransportServerStreamLimits`,
  `RbxTransportMaxActiveStreams`.
- Experiment flags: `RbxTransportRnaExpConnection`, `RbxTransportRnaExpAppListener`, `RbxTransportRnaExpConnection`.

---

## 8. Key function / address reference

| Address | Identity |
|---------|----------|
| `0x100041fe2` | Mach-O `entry` (→ `NSApplicationMain`) |
| `0x104c18388` | FVar registry init (registers ScriptContext*, etc.) |
| `0x1015566d6` | ScriptContext resume trampoline |
| `0x101556235` / `0x1015562c1` | ScriptContext execute wrappers |
| `0x101556664` | Facet accessor (lifecycle ≥ 3 check) |
| `0x1015c486e` | Shutdown-deadline check |
| `0x10298534b` | C-stack manager |
| `0x102995af5` | Resume + pop return values |
| `0x102995dc4` | Resume + drain yield queue |
| `0x102995084` | C-stack grow |
| `0x102ee6a60` | TaskScheduler job profiler |
| `0x1045455d3` | GenericJob::Step |
| `0x106863b20` | `"ScriptContext"` flag string |
| `0x1068a5fa2` | `"TaskScheduler"` flag/class string |
| `0x1073b8040` | Static flag/class name pointer table |
| `0x10789f860`–`0x10789fa30` | Runtime FVar instance table |

Helper functions: `func_0x000102f4a716` (FVar register), `func_0x000102f6fdf0`/`func_0x0001001107b8`/
`func_0x000102f6d5d1` (FLog), `func_0x000102995b7f` (core VM resume).

---

## 9. Deep dive — Luau VM execution core (confirmed against Luau 0.742.0.7421053)

The Luau VM offsets in the community dump are **valid for this binary** (unlike several Roblox-side
offsets — see §11). Add the image base `0x100000000` to convert the dump's RVAs to Ghidra VAs.

### 9.1 `luau_execute` (RVA `0x29adfd3` → VA `0x1029adfd3`) — the interpreter
This is the bytecode interpreter loop. It does **not** decompile cleanly ("Backward normalization not
implemented") because it is a **threaded computed-goto dispatcher**. The disassembly confirms it:

```asm
1029b3609: MOVZX EAX, byte ptr [R11]        ; opcode = *ip   (R11 = instruction pointer)
1029b360d: LEA    R15, [0x1072d9230]        ; handler table base
1029b3614: LEA    RAX, [R15 + RAX*0x8]      ; index by opcode
1029b3618: JMP    qword ptr [RAX]           ; goto handler
```

- **Real opcode dispatch table is at RVA `0x72d9230` (VA `0x1072d9230`)** — *not* the dump's
  `OpcodeLookupTable = 0x63bb680` (that address is not even readable in memory; it is a stale/wrong table).
- The table has 256 entries (one per opcode byte); many are `NULL` (NOP/unused). Handlers live in
  `0x1029b3xxx`–`0x1029b8xxx`. A few resolved entries: opcode 1→`0x1029b3d0d`, 2→`0x1029b4ed7`,
  7→`0x1029b52ab`, 15→`0x1029b5485`, 20→`0x1029b58bf`, 32→`0x1029b5963`, …
- The instruction pointer is read from the closure: `ip = *(long*)(closure+0x18)` → the Proto's code array.
- Entry checks a flag at `state+6`; if set it jumps to a continuation at `0x1029b359c` (the "resume an
  existing coroutine" path), otherwise falls into the fresh-call path at `0x1029ae615`.

### 9.2 `LuaVMLoad` (RVA `0x29a9f36` → VA `0x1029a9f36`) — bytecode chunk loader
Decompiles cleanly. This turns compiled bytecode into a runnable closure:
1. Stack check: `top = *(ulong*)(closure+8)`, `limit = *(ulong*)(closure+0x10)`.
2. If `top <= limit` → call **`luaC_step`** (`0x102996c06`) — a GC step to free space.
3. Call **`luaD_rawrunprotected`** (`0x102995084`) with the protected fn **`loadsafe`** (`0x1029aa05a`)
   and the chunk args. `loadsafe` builds the Proto/closure.
4. If result `== 4` (LUA_ERRMEM) → raise `"not enough memory"`.
5. Cleanup via `func_0x0001029abcee` / `func_0x0001029abd1a` (free the proto/closure on failure).

### 9.3 `luaD_rawrunprotected` (RVA `0x2995084` → VA `0x102995084`) — protected call
The pcall/error-handling wrapper. Used both for loading (`loadsafe`) and running. The decompiled body is
the inner `(*f)(L, ud)` call; the setjmp/errfunc machinery surrounds it.

### 9.4 Full execution model (now end-to-end)
```
task.* / ScriptContextResume (0x1015566d6)
  → lua_rawcheckstack (0x102995b7f)            ; grow C stack if needed
  → luaD_rawrunprotected (0x102995084)         ; setjmp error frame
      → luau_execute (0x1029adfd3)             ; bytecode interpreter loop
          → dispatch via table @ 0x1072d9230   ; opcode → handler
              → handler implements one VM instruction, advances ip, loops
```
Script *loading* (LuaVMLoad) is the sibling path: bytecode → `loadsafe` under `luaD_rawrunprotected` → closure.

---

## 10. Task library → TaskScheduler mapping (confirmed)

The `task.*` Lua library is the user-facing API over the TaskScheduler / coroutine system. All seven
functions decompile cleanly. `task_lib_init` (`0x1014cebe0`) registers the luaL_Reg table at `0x107a702e0`:

| Lua name | Real function (VA) | RVA | Dump correct? |
|----------|--------------------|-----|----------------|
| `synchronize` | `sub_1014cedfe` | `0x14cedfe` | ✓ |
| `desynchronize` | `sub_1014ceec6` | `0x14ceec6` | ✓ |
| `defer` | `sub_1014cef92` | `0x14cef92` | ✓ |
| **`spawn`** | **`sub_1014cf416`** | **`0x14cf416`** | ✗ dump says `0x14cff67` |
| **`delay`** | **`sub_1014cf56a`** | **`0x14cf56a`** | ✗ dump says `0x14cef92` (= defer!) |
| `wait` | `sub_1014cf74a` | `0x14cf74a` | ✓ |
| `cancel` | `sub_1014cf880` | `0x14cf880` | ✓ |

> The dump's `spawn` and `delay` offsets are **wrong/stale**. `sub_1014cff67` is actually an
> `__index`/`__namecall` string-lookup helper, not `spawn`. Use `0x14cf416` (spawn) / `0x14cf56a` (delay).

### 10.1 Core scheduler dispatch — `func_0x0001015a8050`
All of spawn/defer/delay/wait funnel into `func_0x0001015a8050(event, sched_queue, args, closure/priority, flags)`:
- `func_0x0001040d49da(lane)` — gets the scheduler/task-queue object for a VM lane.
- `func_0x0001015f1b16` / `func_0x0001015f18de` — marshal the Lua args into the scheduler's arg buffer.
- `func_0x0001015f1f02` — free the arg buffer.

### 10.2 Thread / scheduler state byte — `thread + 0xac`
The coroutine's scheduler state is a single byte at offset `+0xac` (on the thread object from
`func_0x00010298877e`):
- `0` = running, `1` = yielded/suspended, `2` = deferred, `3` = waiting.
- `defer`/`delay` set `2`; `wait` sets `3`; the cancel/recovery path resets to `0`/`1`.

### 10.3 VM lane / ScriptContext access
- `func_0x00010154240a(state)` — get the ScriptContext (VM lane) for a Lua state.
- `*(long*)(lane + 0x100)` — the parallel/VM-lane sub-object; `*(char*)(... + 0x515)` — its state flag.
- `func_0x00010154400a(lane)` — get the Parallel Luau lane (used by synchronize/desynchronize).

### 10.4 synchronize / desynchronize → Parallel Luau
These two switch VM lanes and are the Lua face of the **ParallelLuauHybridQueue / ParallelLuauWorkQueue**
system found earlier. Hard guard:
`"task.synchronize() may only be called from a script that is a descendant of an Actor"` (and the same for
desynchronize). `func_0x0001014d74b6` (sync) / `func_0x0001014d768a` (desync) do the lane switch.

### 10.5 AuroraScripts restriction
`task.defer/delay/wait/spawn` are **blocked for AuroraScripts**:
`if (is_aurora && *(char*)(thread+0xa9)==1) → "task.defer is not available for AuroraScripts"`.
This is a permission gate on the task library (server-side / restricted scripts).

### 10.6 Scheduler core internals
The scheduler dispatch `func_0x0001015a8050` (now decompiled) confirms a **time-ordered priority queue**:
- `scheduled_time = delay + now` (wall clock via `func_0x000102f7ea5c`).
- Args marshaled via `func_0x0001015f1e0c`, pushed onto the queue via `func_0x0001015ab762(queue+0x68, args)`.
- `func_0x0001015ab9ee(begin=queue+0x68, end=queue+0x70, &new_entry, count*magic)` inserts the entry in
  time order (the `0x6db6db6db6db6db7` constant is a fixed-point division multiplier used in the
  count/average bookkeeping).
- `func_0x000103209848(0x107a70cd0)` then **signals the scheduler** to wake and process the new event.

The queue object itself lives on the VM lane:
- **`func_0x0001040d49da(lane)`** returns `lane + 0xac0` (the scheduler queue), gated by lifecycle
  `*(int*)(lane + 0x12a0) >= 3` (else `"Invalid Facet Access"`).
- Queue entries are 8 bytes (`>> 3`); begin at `+0x68`, end at `+0x70`.

### 10.7 ObjC startup path (confirmed)
The Cocoa entry point (`NSApplicationMain`) hands off to **`RobloxPlayerAppDelegate`** / **`RBXAppDelegate`**
(ObjC class-name strings at `0x106bbdc6e` / `0x106bbdcc2`). `applicationDidFinishLaunching` runs:
1. Skip update check (`"This is a custom build. Skip update"` / `"skipping update"`).
2. Parse CLI args / placeId (`"... Arg #%d/%d: [%s]"`, `"placeId = %lld."`).
3. **"Initializing lua app"** → **"Bind to services in game-engine"** → **"Launching game from web"**.
4. Machine-id check (`"Doing machine id check."`, `[FLog::Warning] (RobloxPlayerAppDelegate) Machine id check failure.`).
5. `reportClientStatus` → then the game/DataModel is created and the RunService loop starts.

Other delegate hooks: `applicationShouldTerminate`, `applicationWillTerminate`, `windowShouldClose`,
`application:openURLs:`, `handleDeeplinkUrl`, `leaveGameWithCallback`, `appLuaWillStart`.

---

## 11. Offset dump validation (important)

Cross-checking the community dump against this exact binary (`f9247f9560044102`):

**✓ Confirmed correct (in `__text`, decompiled/verified):**
- Luau VM: `luau_execute` `0x29adfd3`, `LuaVMLoad` `0x29a9f36`, `luaD_rawrunprotected` `0x2995084`,
  `loadsafe` `0x29aa05a`, `luaC_step` `0x2996c06`, plus the LuaLib/LuaBase functions.
- `ScriptContextResume` `0x15566d6` — **exact match** with the `sub_1015566d6` I found independently.
- Task library: `defer` `0x14cef92`, `wait` `0x14cf74a`, `cancel` `0x14cf880`,
  `synchronize` `0x14cedfe`, `desynchronize` `0x14ceec6`, `task_lib_init` `0x14cebe0`.

**✗ Wrong / stale in the dump:**
- `OpcodeLookupTable` `0x63bb680` → not readable; real dispatch table is `0x72d9230`.
- `spawn` `0x14cff67` → real is `0x14cf416`. `delay` `0x14cef92` (= defer) → real is `0x14cf56a`.

**Out of range for this binary (RVAs beyond `__text` end `0x5bbb4b5` — from a different/larger build):**
- `GetGlobalState` `0x55d5e74`, `PushInstance` `0x1487d49`
- Input: `FireLeftMouseClick` `0x4c75cd4`, `FireRightMouseClick` `0x4c75e36`, `FireMouseHoverEnter`
  `0x4c76a7c`, `FireMouseHoverLeave` `0x4c76b62`, `FireProximityPrompt` `0x42ea5f0`, `FireTouchInterest` `0x10e3030`
- RakNet: `HandleConnectionState` `0x54afa04`, `ProcessNetworkPacket` `0x2d6df78`,
  `ReportNetworkError` `0x54af5b8`, `Send` `0x2d74f8c`
- (`Print` `0x2ce6789` *is* in `__text` range — valid candidate.)

The dump is a **merge of offsets from multiple Roblox builds**. Only the Luau VM + ScriptContextResume +
task-library (+ Print) offsets apply to this binary. The `GetGlobalState`/`PushInstance`/input/RakNet
offsets point outside this binary's mapped code.

---

## 12. Suggested next steps

1. **Full re-analysis.** The single biggest limitation is that Ghidra has only found ~2.5 K functions in a
   ~91 MB code segment. Running a full analysis (or targeted recursive-descent from the ObjC delegate
   entry points and the vtable callbacks above) would recover the remaining ~95 % of engine functions.
2. **ObjC layer.** Map the Cocoa delegate (`applicationDidFinishLaunching`, etc.) that calls into the
   engine after `NSApplicationMain` — that is the true "game start" path.
3. **VM internals.** `func_0x000102995b7f` is the core Luau resume; following it into the bytecode
   interpreter (and the `luau-telemetry.*` capability markers) would complete the script-execution picture.
4. **Transport bridge.** The `RbxTransport->RakNet` / `RakNet->RbxTransport` channels define the seam where
   the new QUIC path hands off to (or replaces) the legacy RakNet path.
