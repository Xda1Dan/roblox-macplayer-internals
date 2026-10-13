# 02 — Scripting: ScriptContext + Luau execution

## The host: ScriptContext

`RBX::ScriptContext` is a **service inside DataModel**
(`DescribedCreatable<ScriptContext, ServiceInstance>`). It owns script
execution through facets (exact names from mangled-name metadata):

| Facet / job | Role (from signatures + logs) |
|---|---|
| `LoaderFacet` | `addScript/removeScript`, `requireModule`, `loadModuleScriptFrom{Instance,AssetId,RbxLibraryPath}`, `handleModuleScriptResult`, `callAsyncCallbackFromSource`, `reloadModuleScriptInternal` |
| `SchedulerFacet` | `resumeWaitingScripts`, `YieldEvent`, `waitingThreads` |
| `ExecutorFacet` | `callInNewThread`, `callAsyncCallback` |
| `VmCollectionFacet` | owns `lua_State`s (`state thread count %zu` at teardown) |
| `CoverageFacet` | `getCoverageStats` |
| `RomarkProfilerFacet` | `popProfilerUserActiveRegion` |
| `GcJob` / `TelemetryJob` / `ClearUnusedLuaRefsJob` / `WaitingHybridScriptsJob` / `LuauHeapMemoryReportProcess` / `LuauRootObjectHeapProcess` | per-frame scheduler jobs |
| `ParallelLuauWorkQueue` / `ParallelLuauHybridCaller/Queue` | Actor-parallel dispatch (`callInParallel called in noopt mode`, `executePendingWork start/finish`) |

Close order is logged: `LoaderFacet::disassociateState` →
Scheduler drain → `VmCollectionFacet` free.

## Scripts as data

`Script / LocalScript / ModuleScript / CoreScript / BaseScript` instances carry
source wrapped in `ProtectedString` (never raw `char*` in facet signatures).
Gates, all observed in strings:

* `RunContext = Server/Client/Plugin/Legacy` (`RunContext is Plugin…`,
  `RunContext is not Legacy…`).
* `lacking capability Run{Client,Server}Script`.
* `Security::Identity` + `VMClasses` on every facet call.
* Strict `require` rules: `Cannot require a (non-)RobloxScript module from a
  (non-)RobloxScript (context)`.

## The pipeline (binary ↔ Luau OSS)

Luau counts in `__cstring`: `Luau` 511, `Lua` 2576, `Bytecode` 59,
`init.{luau,legacy,server,client,plugin,local}.luau` bootstraps,
`LuauBuffer/Integer/CollectionHandle`, `LuauTypeCheckMode`, `LuauCodegen*/LuauCompile*`
flags, `LuauHeapMemoryReport`, `LuauJitInliner`, `SharedCodeGenContext`.

| Stage | Binary evidence | Luau OSS counterpart |
|---|---|---|
| Parse | `Ast*` families (`AstExprCall`, `AstStatFunction`, …) | `Ast/.../Parser.h: parse` |
| Compile | `LuauCompile*`, `BytecodeBuilder`, `JitInliner::RuntimeBytecodeBuilder` | `Compiler.cpp: compile`, `BytecodeBuilder::emit*` |
| Verify at load | `Bytecode Auth Check missing or wrong version`, `DebugDisableOptimizedBytecode`, `DebugSkipLoadAssetWithBytecodeVerification`, `luau-telemetry.*` | `lvmload.cpp: loadsave` + `BytecodeEncoder::encode` hook |
| Execute | `lua_State` in facet signatures, `VmCollectionFacet`, `lua_vm_init`, `lua_container_init` | `lvmexecute.cpp: luau_execute`, `ldo: luaD_call/pcall` |
| Native tier | `LuauCodegen*`, `CodeGen::SharedCodeGenContext(+Deleter)`, `LuauNativeExecutionEvent`, `--!native`-style flags | `CodeGen.h: compile`, `NativeProtoExecData` (`Proto.execdata`) |
| `require` | `Require::RuntimeNavigationContext`, `reset/to_child/is_module_present/load` shapes | `Require.h: luarequire_Configuration`, `RequireNavigator` (Roblox plugs a DataModel-backed navigator) |
| Coroutines | `resumeWaitingScripts`, `Resuming delayed/waiting threads`, `ModuleScript cannot resume non-suspended awaiting coroutine` | `lua_newthread/resume/yield`, `CallInfo` flags |
| GC | `GcJob`, `LuauHeapMemoryReport::{aggregate,collectInstances,linkHeap,runGc}`, `ClearUnusedLuaRefsJob`, `ScriptScopeTokenPool stats` | `lgc.h` incremental tri-color, `lua_Page` heap, `luaC_step/fullgc` |

No vanilla `luaL_*` C-API surface was found — the engine talks to Luau through
its own C++ API. `getgenv` appears 0 times (no executor backdoor strings).

## Native → Lua event paths (decompiled)

* `notifyLuaFocusedIfNeeded` (`ram:0x10000cdd2`): builds `AppInput/""/"Focused"`
  (`0x100016179`) → fires via `0x102ec8158/0x102ec81a0`. Telling Lua = engine
  event dispatch, which script connections receive.
* `userContentController:didReceiveScriptMessage:` (`ram:0x10000e295`):
  `WKScriptMessage.body` → `UTF8String` → libc++ string (`0x102d49d80`, SSO
  assign — see `src/sso_string_demo.cpp`) → engine lookup + dispatch
  (`0x104929a60`, `0x104f2965c`, `0x10003ddee` — a seqlock-guarded lazy global
  resolved once via vtable `+0x30`) → `0x10520f486`.

## C++ sketches

* `include/rbx/script_context.hpp` — `ScriptContext : Service` with facet
  interfaces mirroring the observed signatures (`ProtectedString`,
  `Security::Identity`, per-VM state).
* `include/rbx/luau_bridge.hpp` — `lua_State` pool, `require` navigator
  shaped like `luarequire_Configuration`, protected-call + coroutine resume
  flow.
* `src/luau_embed_demo.cpp` — needs Luau OSS headers (`make
  LUAU_INCLUDE=… luau_demo`); compiles `= return 1` equivalent through
  `luau_compile` → `luau_load` → `lua_resume`, the same boundary the player
  uses.
