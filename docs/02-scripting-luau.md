# Scripting: ScriptContext + Luau execution

## Script lifecycle, all ten stages

1. **Author** — `Script / LocalScript / ModuleScript / CoreScript` instances.
   `init.{luau,legacy,server,client,plugin,local}.luau` names mark the
   per-context bootstraps.
2. **Gate** — `RunContext` (`Server/Client/Plugin/Legacy`), `Run{Client,Server}Script`
   capabilities, `Security::Identity` + `VMClasses`. Rejections observed:
   `Cannot start {client,server} script '%s' (lacking capability …)`,
   `Can't resume script in this context`.
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
