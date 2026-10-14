# Scripting: ScriptContext + Luau execution

## Script lifecycle, all ten stages

1. **Author** — `Script / LocalScript / ModuleScript / CoreScript` instances.
   `init.{luau,legacy,server,client,plugin,local}.luau` names mark the
   per-context bootstraps.
2. **Gate** — `RunContext` (`Server/Client/Plugin/Legacy`), `Run{Client,Server}Script`
   capabilities, `Security::Identity` + `VMClasses`. Rejections observed:
   `Cannot start {client,server} script '%s' (lacking capability …)`,
   `Can't resume script in this context`.

The concrete start-admission predicate is at `ram:0x10142a7a6`. It reads the
script's run-context field, obtains a native capability mask, and checks the
relevant bit before allowing the start path to continue. The observed mapping
is:

* server-context admission requires the `RunServerScript` capability;
* client-context admission requires the `RunClientScript` capability;
* plugin/core/special contexts take additional native branches rather than
  simply bypassing the check;
* ancestry/container and DataModel state can still veto admission after the
  capability bit is present.

The small functions containing the `Cannot start ... lacking capability`
strings are reporting helpers; `0x10142a7a6` is the function that computes the
decision. The binary does not show Lua code assigning its own security level.

The native capability vocabulary includes identities/classes such as `Plugin`,
`LocalUser`, `WritePlayer`, `RobloxScript`, `RobloxEngine`, `Unassigned`,
`InternalTest`, `PluginOrOpenCloud`, and `RemoteCommand`, plus capability
groups such as `ServerCommunication`, `Social`, `Teleport`, `AssetRead`,
`AssetManagement`, and `Logging`. These are policy categories used by native
checks, not a Lua mechanism for arbitrarily elevating a running script.
The identity is native execution-context metadata, while capabilities are the
permission mask used to admit a script.
3. **Wrap** — source becomes `ProtectedString`; never handled as raw `char*`
   in facet signatures.
4. **Load** — `LoaderFacet`: `addScript`, `requireModule`,
   `loadModuleScriptFrom{Instance,AssetId,RbxLibraryPath}`,
   `handleModuleScriptResult`, per-VM `ModuleScript::PerVMState`.
   Strict rules: `Cannot require a (non-)RobloxScript module from a
   (non-)RobloxScript (context)`.
5. **Verify** — `Bytecode Auth Check missing or wrong version`,
   `DebugDisableOptimizedBytecode`, `DebugSkipLoadAssetWithBytecodeVerification`.
   The check sits at the compile→load boundary (see below).
6. **Compile** — embedded Luau compiler (`LuauCompile*`, `BytecodeBuilder`,
   `JitInliner::RuntimeBytecodeBuilder`, `SharedCodeGenContext`).
7. **Schedule** — `ExecutorFacet::callInNewThread` / `callAsyncCallback`
   pushes closures onto `lua_State`s from `VmCollectionFacet`'s pool.
8. **Start and resume** — the inspected `LoaderFacet::addScript` execution
   body is at `ram:0x10158ac46` (this address is a call-site/function body in
   this build). It logs `Running script`, enforces a three-level
   `startScript` re-entrancy limit, establishes a `Script Start` profiling
   scope, obtains a VM/thread-facing object, and invokes the VM start helper.
   The call keeps script/context shared ownership alive while callbacks and
   result/error paths are installed. `SchedulerFacet::resumeWaitingScripts`
   has two observed bodies at `ram:0x1015a8d94` and `ram:0x1015a9249`.
   Both take an atomic resumption flag, drain a bounded number of waiting
   entries, stop at a deadline, release each shared waiter, and finish with
   scheduler bookkeeping. The second body has an additional context/queue
   predicate and can remove an entry through `0x1015a837c`.
   Yielding APIs suspend instead of blocking (`BoundYieldFuncDesc`,
   `CallYieldHelper`, `YieldFunctionState`).
9. **Parallel** — `ParallelLuauWorkQueue::executePendingWork`,
   `ParallelLuauHybridCaller::callInParallel` (errors if the VM lane is
   inactive or the instance isn't queued). Actor rule, observed verbatim:
   a `ModuleScript` may only use parallel APIs if required by an Actor
   script (`BindToMessage{,Parallel}` enforce descendant-of-Actor).
10. **Collect** — `GcJob`, `ClearUnusedLuaRefsJob`,
    `LuauHeapMemoryReport::{aggregate,collectInstances,linkHeap,runGc}`,
    `ScriptScopeTokenPool` occupancy stats.

Teardown reverses it: `LoaderFacet::disassociateState` → scheduler drain
(`waitingThreads`, `YieldEvent->waiterCount`) → `VmCollectionFacet` free
(`state thread count %zu`).

The destructor bodies make that ordering concrete. The scheduler teardown at
`ram:0x1015a7d1e` reports `YieldEvent->waiterCount`, releases the waiting
thread claim/lock, reports `waitingThreads.size()`, and then destroys its
queue/event/ownership members before poison-filling roughly `0x280` bytes.
The VM collection teardown at `ram:0x1015bab74` walks multiple fixed-size VM
slots, reports each state's thread count, releases per-slot resources and
allocators, then poison-fills roughly `0x5e0` bytes. These are lifetime
operations, not merely diagnostic logging.

### Script-start pseudocode (simplified from the observed body)

```cpp
void startScript(ScriptContext& context, ScriptRecord& script) {
    if (++context.startDepth > 3)
        fatal("startScript re-entrancy has exceeded 3");

    auto vmObject = context.prepareVmObject(script);
    auto thread = context.createOrSelectThread(vmObject);
    context.beginProfileRegion(thread, "Script Start");
    context.installThreadState(thread, script);
    int result = context.invokeLuauStart(thread, script);
    if (result == 0)
        context.publishYieldOrCompletion(thread);
    else
        context.publishScriptError(thread);
    --context.startDepth;
}
```

The helper names and exact `lua_State` offsets remain unresolved; the
important observed properties are the re-entrancy guard, VM/thread handoff,
profiling scope, and ownership/error cleanup.

### What “script execution level” means here

There are three related but different concepts:

1. **RunContext** — where the script is intended to run: server, client,
   plugin, or legacy.
2. **Security identity** — native context metadata carried into the script
   execution record and used by reflection/security checks.
3. **Capabilities** — the native permission set, including
   `RunClientScript` and `RunServerScript`, checked before the VM is entered.

Changing a `RunContext` property does not itself grant permission. The native
context must possess the corresponding capability, and the script must still
pass DataModel/ancestry/container checks. This is why a script can be present
in the correct service and still be refused before execution.

### Additional native boundaries

`ram:0x101593650` is a script compilation-cache/loader boundary, not the VM
resume itself. It constructs a Luau-facing value, reads compact or
out-of-line string storage, calls a helper supplying cached or compiled data,
and has an explicit `Unexpected type in script compilation cache.` failure
path. This places cache lookup between source/policy handling and execution.

`ram:0x10159502e` is a native pre-run policy gate. It builds diagnostic data
including `scriptClass` and `script`, and contains the messages `Script "{}"
was blocked from being ran under a restricted container.` and `detected as
malicious. ... will not run.` A script can therefore be rejected before the
VM start path, rather than only failing inside Luau.

`ram:0x10158cc58` initializes an execution record: it copies compact fields,
retains shared references, derives execution flags from the script and current
context, and stores inherited limits/identity data. The exact field meanings
are not stable enough to publish as fixed offsets.

### Native `require` behavior

The body at `ram:0x10158dd88` is a real ModuleScript require path. Before
loading it checks the caller/module security contexts and emits the observed
errors for incompatible RobloxScript/non-RobloxScript combinations. It also
handles special `.config` modules, attaches the module to the per-VM state,
and branches on module state rather than blindly executing every call.

The state handling includes:

* already-loaded modules returning their cached result;
* loading modules participating in a cycle, with a diagnostic requiring
  `export` instead of `return` for circular dependencies;
* destroyed module references failing with `Requested module has been
  destroyed and can no longer be required`;
* load failures producing `Requested module experienced an error while
  loading`;
* successful modules being required to return exactly one value.

This is a stateful per-VM module cache, not a simple filesystem-style
`require(path)` operation.

### Async callbacks and coroutine state

The callback body at `ram:0x101557e8c` resolves the implementing script from a
weak reference before doing any VM work. If that script has been destroyed it
raises `Script that implemented this callback has been destroyed while calling
async callback`. Otherwise it creates a `callAsyncCallback` execution/profiling
scope, rebuilds the callback's argument/context record, and routes the result
through the same execution-object and VM-facing machinery used by script
starts. Its normal, deferred, and error paths all release shared references.

The small state gate at `ram:0x1015977c1` accepts only the VM states represented
by suspended/awaiting coroutine states. Any other state produces
`ModuleScript cannot resume non-suspended awaiting coroutine`; this prevents a
second resume of a running or already-completed coroutine.

### Waiting-script pseudocode

```cpp
void resumeWaitingScripts(WaitingQueue& q, Deadline deadline) {
    while (!q.tryClaimResumeFlag()) { /* bounded spin */ }
    int budget = max(1, configuredWaitingScriptBudget());
    while (budget-- > 0) {
        auto waiter = q.popReady();
        if (!waiter) break;
        if (clock::now() >= deadline) break;
        resumeOne(waiter);             // may remain/yield again
    }
    q.releaseResumeFlag();
    schedulerBookkeeping(q, deadline);
}
```

## The host: ScriptContext facets

`RBX::ScriptContext` is a service in DataModel. Its facets, with observed
operations:

| Facet / job | Operations |
|---|---|
| `LoaderFacet` | `addScript/removeScript`, `requireModule`, `loadModuleScriptFrom*`, `handleModuleScriptResult`, `callAsyncCallbackFromSource`, `reloadModuleScriptInternal` |
| `SchedulerFacet` | `resumeWaitingScripts` |
| `ExecutorFacet` | `callInNewThread`, `callAsyncCallback` |
| `VmCollectionFacet` | VM-lane pool (`acquireVm/releaseVm` pattern) |
| `CoverageFacet` | `getCoverageStats` |
| `RomarkProfilerFacet` | profile-region annotations |
| `GcJob`, `TelemetryJob`, `ClearUnusedLuaRefsJob`, `WaitingHybridScriptsJob`, `LuauHeapMemoryReportProcess`, `LuauRootObjectHeapProcess` | frame-scheduled maintenance |

## The Luau pipeline and its OSS counterparts

| Stage | In-binary markers | Luau OSS (`github.com/luau-lang/luau`) |
|---|---|---|
| Values | `TValue`-shaped slots, `LuauBuffer/Integer/CollectionHandle` | `VM/src/lobject.h`: `TValue`, `GCObject`, `TString`, `Table`, `Proto`, `Closure`, `UpVal` |
| Parse | `AstExpr*/AstStat*` families | `Ast/.../Parser.h: parse` |
| Compile | `LuauCompile*`, `BytecodeBuilder::emit*`, `JitInliner` | `Compiler.cpp: compile`, `BytecodeBuilder.h` |
| Verify/load | auth-check strings above, `luau-telemetry.*` | `lvmload.cpp: luau_load → loadsafe`, `BytecodeEncoder::encode` hook |
| Execute | `lua_State` in signatures, `lua_vm_init`, `lua_container_init` | `lvmexecute.cpp: luau_execute`, `ldo: luaD_call/pcall` |
| Native tier | `LuauCodegen*`, `CodeGen::SharedCodeGenContext(+Deleter)`, `LuauNativeExecutionEvent` | `CodeGen.h: compile`, `NativeProtoExecData` |
| Types | `LuauTypeCheckMode`, `LuauTypeSolver`, `LuauTypeInferIterationLimit`, `Luau-Reflection-Type-Error` | `Frontend/` type checker |
| `require` | `Require::RuntimeNavigationContext`, navigator-shaped methods | `Require.h: luarequire_Configuration`, `RequireNavigator` (here backed by DataModel, not the filesystem) |
| Coroutines | resume/yield strings above | `lua_newthread/resume/yield`, `CallInfo` |
| GC | jobs + heap-report above | `lgc.h` tri-color, `lua_Page` heap |

No vanilla `luaL_*` surface and zero `getgenv` occurrences: the engine speaks
to Luau through its own C++ API.

### Is `lua_pcall` present?

The exact public symbol/name `lua_pcall` is not present in the shipped Mach-O
symbol/string inventory. That does **not** mean protected calls were removed.
The supplied Luau map resolves these exact function starts in this build:

| Address | Finding |
|---|---|
| `ram:0x10298b913` | `luaB_pcall`-shaped builtin wrapper; prepares the call frame, invokes the protected call machinery, and converts protected statuses/results |
| `ram:0x10298b946` | `luaB_pcallrun`-shaped continuation/stack-result helper |
| `ram:0x102995084` | `luaD_rawrunprotected`-shaped protected-execution boundary |
| `ram:0x102995125` | `luaD_throw`-shaped exception path; throws a C++ exception in this build |
| `ram:0x1029940f4` | `luaG_runerror`-shaped error construction/propagation path |

So the accurate statement is: **the exported/public `lua_pcall` API name is
stripped or not used, while the implementation's protected-call machinery and
the Luau `pcall` builtin remain present.** Roblox's native ScriptContext paths
also use their own protected VM helpers rather than requiring a public
`lua_pcall` symbol.

### Split RIP-pointer construction in selected Luau paths

The MacPlayer also contains the selective pointer construction described in
the accompanying offset research. A verified example is at
`ram:0x102988254`, inside a Luau error path:

```asm
lea    rax, [rip+...]          ; dummy = 0x106c9c778
mov    [rbp-0x18], rax
mov    rsi, 0xffffffffffd06c00 ; signed delta = -0x2f9400
add    rsi, [rbp-0x18]         ; rsi = 0x1069a3378, "stack limit"
call   ram:0x1029940f4         ; luaG_runerror-shaped path
```

The dummy address is in `__unwind_info`; the reconstructed pointer lands in
`__cstring`. The arithmetic is:

```text
0x106c9c778 + (-0x2f9400) = 0x1069a3378
```

This is distinct from ordinary compiler output such as `lea rsi,[rip+string]`.
It can break naive string-reference scanners because the instruction that
mentions the real string address is replaced by an immediate delta and a
stack-mediated add. The pattern is selective: direct RIP-relative references
still exist elsewhere, including ordinary Luau/engine strings, so it should be
treated as a deliberate per-site pointer transformation rather than a global
compiler ABI rule.

## Luau-side findings from the supplied function map

The supplied RVAs were checked against the binary's linker function-start
table. These entries land exactly on function starts for this build:

| Supplied RVA | Verified role |
|---|---|
| `0x299fd5b` | `lua_newstate`-shaped state allocation/initialization; accepts an allocator callback and allocator context, initializes the state, then runs protected initialization |
| `0x29a9f36` | `loadsafe`-shaped protected bytecode load; checks stack/memory state, calls the protected loader, restores the stack cursor, and reports memory failure |
| `0x29adfd3` | `luau_execute` entry exists at the exact address, but this build's decompiler could not recover its body; do not infer its calling convention from the label alone |
| `0x15566d6` | `ScriptContextResume`; logs `Resuming script`, checks shutdown/deadline and C-stack limits, then performs the VM resume path |
| `0x14cf74a` | `task.wait`; rejects non-yieldable threads and AuroraScript contexts, marks the coroutine waiting, schedules a timed resume, and returns through the VM resume machinery |
| `0x55d5e74` | ScriptContext facet accessor: if the facet index is below 3 it returns `context+0xdc0`; otherwise it raises `Invalid Facet Access` |

The `ScriptContextResume` body exposes several real VM guardrails:

* It logs the pending script name before resume.
* It rejects a C stack depth above 200 with `C stack overflow`.
* It refuses to run after the shutdown deadline.
* It can refresh execution metadata from the DataModel/context before resume.
* It records first-resume timing and script execution time.

`task.wait` is not implemented as a blocking OS sleep. Its body marks the
current coroutine as waiting, packages a resume event/deadline, schedules it
through ScriptContext's waiting machinery, and returns control to the VM. This
is the native reason a Luau `task.wait()` yields without blocking the worker
thread.

### Lua state creation in this player

The exact caller of `lua_newstate` is `ram:0x1015b92cd`. Its sequence is
visible in the body:

```text
allocator callback + allocator context
  → lua_newstate (allocates a 0x4d60-byte state)
  → fail hard if state creation returns null
  → attach allocator/configuration data to a ScriptContext VM wrapper
  → install panic/interrupt/error callbacks
  → initialize Luau/standard library entry points
  → register Roblox-specific libraries and globals
  → establish _G and sandbox/configuration handlers
  → retain the wrapper in the VM collection
```

The state initializer at `ram:0x10299fd5b` accepts a function pointer and
context, calls that allocator for `0x4d60` bytes, initializes the global-state
and main-thread pointers, stack/GC fields, and runs a protected initialization
routine. The caller immediately reports `Failed to create Lua state` if it
returns null.

The caller then installs callbacks and invokes a long series of library
registration functions. The names in the offset header map to several of the
same routines (`luaopen_math`, `luaopen_os`, `luaopen_base`, and others), but
the engine does not simply expose an unrestricted stock Lua environment: the
library set, globals, Roblox bindings, sandbox configuration, identity, and
capabilities are applied around the VM before scripts run.

The VM is therefore not created per individual script. A ScriptContext owns a
VM collection; scripts obtain execution threads/stacks from that collection,
and `task.wait`/resume operate on those threads. Teardown releases the VM
collection only after waiting scripts and yield waiters have been drained.

### Correct interpretation of the offset header

The header is a useful symbol/offset index, but its names are analyst labels,
not proof of wrapper semantics. In particular:

* `GetGlobalState = 0x55d5e74` should be renamed to something like
  `ScriptContextFacetAccessor` for this binary.
* `task.wait = 0x14cf74a` is behaviorally confirmed.
* `task.spawn = 0x14cff67` currently lands in a small task-library setup/helper
  body; its exact public wrapper relationship still needs a caller trace.
* `luau_execute` is an exact function start, but its calling convention/body
  is unresolved because the decompiler failed on that highly optimized VM
  routine.

This distinction matters when reconstructing a legitimate in-process engine:
use public Luau APIs and engine-owned ScriptContext interfaces for execution;
do not treat a label in an offset header as a stable ABI or as permission to
inject into or bypass another process.

## Native → Lua event paths (observed bodies)

* `EmbeddedWebView.notifyLuaFocusedIfNeeded` (`ram:0x10000cdd2`): builds
  `AppInput/""/"Focused"` (`0x100016179`, 0x58 bytes) → fires via
  `0x102ec8158/0x102ec81a0`. Telling Lua = engine event dispatch.
* `EmbeddedWebView userContentController:didReceiveScriptMessage:`
  (`ram:0x10000e295`): `WKScriptMessage.body` → `UTF8String` → libc++ string
  (SSO assign, `0x102d49d80`) → engine lookup + dispatch (`0x104929a60`,
  `0x104f2965c`, seqlock global `0x10003ddee`) → `0x10520f486`.

## C++ material

* `include/rbx/script_context.hpp` — `ScriptContext : Service` with concrete
  facet interfaces (`ProtectedString`, `SecurityIdentity`, per-VM state).
* `include/rbx/luau_bridge.hpp` — pooled `lua_State` lanes, verified
  compile→load boundary with the auth hook, `require` navigator shape.
* `src/luau_embed_demo.cpp` — `luau_compile` → auth → `luau_load` →
  `lua_resume` against real Luau headers (`make LUAU_INCLUDE=… luau_demo`).
