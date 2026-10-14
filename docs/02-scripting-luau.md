# Scripting reference

In one paragraph: scripts are `Instance` objects with source text. The
engine decides whether they may run, compiles or loads their bytecode,
runs them on pooled Luau threads, and lets them yield and resume later.
This page is the catalog. The step-by-step walkthrough lives in
[07-script-execution.md](07-script-execution.md) and
[09-luau-vm-lifecycle.md](09-luau-vm-lifecycle.md).

## The ten stages

1. **Write** — `Script`, `LocalScript`, `ModuleScript`, `CoreScript`.
2. **Permission check** — `RunContext` (Server, Client, Plugin, Legacy),
   `RunClientScript` / `RunServerScript` capabilities, security identity,
   ancestry, DataModel state. Decided at `ram:0x10142a7a6`.
3. **Wrap** — source travels as `ProtectedString`, not a raw `char*`.
4. **Load** — `LoaderFacet`: `addScript`, `removeScript`, `requireModule`,
   `loadModuleScriptFromInstance` / `FromAssetId` / `FromRbxLibraryPath`,
   `handleModuleScriptResult`, `callAsyncCallbackFromSource`,
   `reloadModuleScriptInternal`, per-VM `ModuleScript::PerVMState`.
5. **Check** — bytecode authentication (`Bytecode Auth Check missing or
   wrong version`), `Invalid Bytecode.`, `DebugDisableOptimizedBytecode`.
6. **Compile** — embedded Luau compiler: `LuauCompile*`,
   `BytecodeBuilder`, `JitInliner::RuntimeBytecodeBuilder`,
   `SharedCodeGenContext`.
7. **Schedule** — `ExecutorFacet::callInNewThread` and `callAsyncCallback`
   place closures on threads from the VM pool.
8. **Start and resume** — start at `ram:0x10158ac46` (at most three levels
   of re-entrant start, `Script Start` region, VM handoff, shared
   ownership across callbacks). Resume through `ram:0x1015566d6`
   (pending-name log, shutdown deadline, C-stack limit of 200, timing).
   Waiting drain at `ram:0x1015a8d94` / `ram:0x1015a9249` (one resume
   flag, bounded drain, deadline, per-waiter release). Yielding calls
   suspend through `BoundYieldFuncDesc`, `CallYieldHelper`,
   `YieldFunctionState`.
9. **Parallel** — `ParallelLuauWorkQueue::executePendingWork`,
   `ParallelLuauHybridCaller::callInParallel`. A `ModuleScript` may use
   parallel APIs only when required by an Actor script.
10. **Collect** — `GcJob`, `ClearUnusedLuaRefsJob`,
    `LuauHeapMemoryReport`, `ScriptScopeTokenPool` stats.

Shutdown reverses it: `disassociateState`, scheduler drain
(`waitingThreads`, `YieldEvent->waiterCount`), VM release
(`state thread count`).

## What level means

Three different ideas share one confusing word:

1. **RunContext** — where the script is meant to run.
2. **Security identity** — native context data carried with execution and
   consulted by reflection and security checks.
3. **Capabilities** — the native permission bits checked before the VM is
   entered.

Changing `RunContext` does not grant permission by itself. The native
context must already hold the matching capability, and ancestry,
container, and DataModel checks must also pass.

Policy words in this build include `Plugin`, `LocalUser`, `WritePlayer`,
`RobloxScript`, `RobloxEngine`, `Unassigned`, `InternalTest`,
`PluginOrOpenCloud`, `RemoteCommand`, and groups such as
`ServerCommunication`, `Social`, `Teleport`, `AssetRead`,
`AssetManagement`, and `Logging`.

## Loading details

- `ram:0x101593650` is the compilation-cache boundary. It builds a
  Luau-facing value, reads compact or out-of-line string storage, asks a
  helper for cached or compiled data, and can fail with `Unexpected type
  in script compilation cache.`
- `ram:0x10159502e` is an early policy gate with `scriptClass` and
  `script` diagnostics: `Script "{}" was blocked from being ran under a
  restricted container.` and `detected as malicious. ... will not run.`
- `ram:0x10158cc58` fills in the execution record: compact fields,
  shared references, flags from script and context, inherited limits and
  identity.
- `loadstring` at `ram:0x1014c86c8` is policy-gated. It can answer
  `loadstring() is not available` or `loadstring() is not available in
  RobloxScript context.` When allowed, it still goes through the engine
  compile and load helpers.

## Require

`ram:0x10158dd88` is a stateful per-VM module cache:

- incompatible RobloxScript combinations fail first;
- `.config` modules cannot be required directly;
- loaded modules return the cached value;
- loading modules in a cycle produce the `export`-instead-of-`return`
  diagnostic;
- destroyed modules fail with `Requested module has been destroyed and can
  no longer be required`;
- load failures report `Requested module experienced an error while
  loading`;
- success must return exactly one value.

## Callbacks and coroutines

- `ram:0x101557e8c` resolves the implementing script through a weak
  reference. Destroyed scripts fail with `Script that implemented this
  callback has been destroyed while calling async callback.` Survivors get
  a `callAsyncCallback` scope, rebuilt arguments, and the same VM path as
  a start. All exits release shared references.
- `ram:0x1015977c1` only accepts suspended / awaiting coroutines.
  Anything else reports `ModuleScript cannot resume non-suspended
  awaiting coroutine`.

Behavior in code form:

```cpp
// Simplified illustration.
void resumeWaiting(WaitingQueue& q, Deadline d) {
  while (!q.tryClaim()) { /* bounded spin */ }
  int budget = q.budget();
  while (budget-- > 0) {
    auto w = q.popReady();
    if (!w) break;
    if (now() >= d) break;
    resumeOne(w);  // may yield again
  }
  q.release();
}
```

## ScriptContext facets

| Facet / job | Operations |
|---|---|
| `LoaderFacet` | `addScript`, `removeScript`, `requireModule`, `loadModuleScriptFrom*`, `handleModuleScriptResult`, `callAsyncCallbackFromSource`, `reloadModuleScriptInternal` |
| `SchedulerFacet` | `resumeWaitingScripts` |
| `ExecutorFacet` | `callInNewThread`, `callAsyncCallback` |
| `VmCollectionFacet` | pooled VM lanes |
| `CoverageFacet` | `getCoverageStats` |
| `RomarkProfilerFacet` | profile regions |
| Frame jobs | `WaitingHybridScriptsJob`, `GcJob`, `ClearUnusedLuaRefsJob`, `TelemetryJob`, `LuauHeapMemoryReportProcess` |

## Luau stages and counterparts

| Stage | Markers in this build | Public Luau counterpart |
|---|---|---|
| Values | `TValue`-shaped slots, `LuauBuffer`, `LuauInteger`, `LuauCollectionHandle` | `lobject.h`: `TValue`, `GCObject`, `TString`, `Table`, `Proto`, `Closure`, `UpVal` |
| Parse | `AstExpr*` / `AstStat*` families | parser |
| Compile | `LuauCompile*`, `BytecodeBuilder`, `JitInliner` | compiler, `BytecodeBuilder.h` |
| Check and load | bytecode authentication lines | `luau_load`, protected loader, encoder hook |
| Run | `lua_State` in signatures | `luau_execute`, protected call helpers |
| Native tier | `LuauCodegen*`, `SharedCodeGenContext`, native-execution events | code generator, native proto data |
| Types | type-check mode, solver, iteration limits | frontend type checker |
| `require` | navigator-shaped methods | `Require` configuration, backed here by DataModel |
| Coroutines | resume and yield lines | `lua_newthread`, resume, yield, `CallInfo` |
| GC | jobs and heap reports | tri-color collector, paged heap |

There is no general `luaL_*` surface and no `getgenv` in this build. The
engine talks to Luau through its own C++ layer.

## Protected calls

The public name `lua_pcall` is absent. The machinery is present:

| Address | Role |
|---|---|
| `ram:0x10298b913` | `luaB_pcall`: frame setup, protected call, status convert |
| `ram:0x10298b946` | `luaB_pcallrun`: stack-result continuation |
| `ram:0x102995084` | protected-execution boundary |
| `ram:0x102995125` | throw path, via C++ exception in this build |
| `ram:0x1029940f4` | error build and propagate |

ScriptContext paths use their own protected helpers as well.

## Split string pointers

Some Luau error paths do not reference their string directly. At
`ram:0x102988254`:

```asm
lea    rax, [rip+...]           ; dummy = 0x106c9c778
mov    [rbp-0x18], rax
mov    rsi, 0xffffffffffd06c00  ; delta = -0x2f9400
add    rsi, [rbp-0x18]          ; rsi = 0x1069a3378, "stack limit"
call   ram:0x1029940f4
```

```text
0x106c9c778 + (-0x2f9400) = 0x1069a3378
```

The dummy sits in `__unwind_info`. The result lands in `__cstring`. This
is selective. Ordinary direct string references still exist elsewhere.

## Luau function starts in this build

| Offset | Role |
|---|---|
| `0x299fd5b` | state create and init: allocator callback and context, then protected init |
| `0x29a9f36` | protected bytecode load: stack and memory checks, load, cursor restore |
| `0x29adfd3` | `luau_execute` entry; body is highly optimized and not detailed here |
| `0x15566d6` | resume: pending-name log, shutdown and C-stack checks, VM re-entry |
| `0x14cf74a` | `task.wait`: yieldable-thread check, mark waiting, timed resume |
| `0x55d5e74` | facet accessor: index below 3 returns `context+0xdc0`, else `Invalid Facet Access` |

Resume guards: pending-name log, C-stack depth above 200 becomes `C
stack overflow`, past-deadline shutdown refuses to run, execution
metadata can refresh from DataModel and context, first-resume and run
time are recorded.

`task.wait` never sleeps a worker. It marks the coroutine waiting,
packages a wake deadline, queues the resume, and returns to the VM.

### Task library

The `task` table is registered at `ram:0x1014cebe0`, which pairs each name
with its backing function:

| Lua name | Address | Behavior |
|---|---|---|
| `synchronize` | `ram:0x1014cedfe` | only from an Actor-descendant script, else `task.synchronize() may only be called from a script that is a descendant of an Actor` |
| `desynchronize` | `ram:0x1014ceec6` | same Actor gate |
| `defer` | `ram:0x1014cef92` | shared spawn/delay/wait core: Aurora rejection, re-entrancy cap (`Maximum re-entrancy depth (%i) exceeded calling task.defer`), deferred-growth guard (`Exponential deferred event growth detected calling task.defer`) |
| `spawn` | `ram:0x1014cf416` | Aurora-gated, serializes arguments, schedules a new thread |
| `delay` | `ram:0x1014cf56a` | spawn plus a wake time, then the same wait path |
| `wait` | `ram:0x1014cf74a` | marks the coroutine waiting, timed resume, no OS sleep |
| `cancel` | `ram:0x1014cf880` | `cannot cancel thread` unless it owns a live thread |

A label reading `task.spawn = 0x14cff67` is wrong for this build:
`ram:0x1014cff67` is a small error-string helper (returns `"unknown"` and
related names), not the spawn implementation.

State creation runs through `ram:0x1015b92cd`: allocator and context,
`lua_newstate` for `0x4d60` bytes, failure becomes `Failed to create Lua
state`, then hooks, libraries, Roblox bindings, `_G`, sandbox and config,
and the VM collection takes ownership. States belong to the collection.
Scripts borrow threads.

A label such as `GetGlobalState = 0x55d5e74` is better read as a facet
accessor here. A label reading `task.spawn = 0x14cff67` is wrong for this
build: `ram:0x1014cff67` is an error-string helper, while the real
`spawn` / `delay` / `defer` entries are `ram:0x1014cf416`,
`ram:0x1014cf56a`, and `ram:0x1014cef92` (see the task library table
above). Labels are an index, not a stable interface.

## Native to Lua events

- `notifyLuaFocusedIfNeeded` (`ram:0x10000cdd2`): builds
  `AppInput` / `""` / `"Focused"` (`0x100016179`, 0x58 bytes), fires via
  `0x102ec8158` / `0x102ec81a0`.
- `didReceiveScriptMessage` (`ram:0x10000e295`): message body to UTF-8 to
  engine string (SSO assign, `0x102d49d80`), lookup and dispatch
  (`0x104929a60`, `0x104f2965c`, global `0x10003ddee`), then
  `0x10520f486`.

## C++ companions

- `include/rbx/script_context.hpp`: facets, `ProtectedString`,
  `SecurityIdentity`, per-VM module state.
- `include/rbx/luau_bridge.hpp`: pooled lanes, check-then-load boundary,
  `require` navigator shape.
- `src/luau_embed_demo.cpp`: `luau_compile`, check, `luau_load`,
  `lua_resume` against real Luau headers.
