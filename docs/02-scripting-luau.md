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
8. **Resume** — `SchedulerFacet::resumeWaitingScripts` drives coroutines
   (`Resuming delayed/waiting threads`, `Pending script name: %s`).
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
