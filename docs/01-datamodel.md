# 01 — DataModel: what it is and what it contains

## What it is

`RBX::DataModel` is the **root `Instance`** of one game session and the
`ServiceProvider` everything resolves against. Observed facts:

* 230 `RBX::DataModel` references; code paths uniformly assume "current
  DataModel or nothing": `AssetService::* … DataModel is null`,
  `createSessionFromAssetID: DataModel not available`,
  `Cannot predict instance {}: Only instances under Workspace can be predicted`.
* It is **per-session, not a global singleton**. Created on join
  (`BringUpDataModel`), destroyed on leave/teleport
  (`[FLog::CloseDataModel] Closing Script Context`,
  `onTeleportStarted/Succeeded/Failed`). The actual singletons nearby are a
  factory/manager (`0x105235e96` → `&DAT_107510308`, call-once guarded, with a
  secondary `0x107c55298` path) and a global manager (`0x10118fab6` →
  `0x107a61588` + `cxa_atexit`).
* Session lookup used by deeplink + leave-game:
  `0x1051e5369` — live lookup (`0x100043b24` + virtual `0x1051e9b6a` on session
  field `+0x50`) or service-locator call through `_DAT_107c53d10` with key
  `0x107510170`. Returns session pointer or null.

## What it contains

Children = services. 100+ `*Service` names observed in strings, including:

`Workspace, Players, Lighting, ReplicatedFirst, ReplicatedStorage,
ServerScriptService, ServerStorage, StarterGui, StarterPack, StarterPlayer,
Teams, SoundService, Chat, TextChatService, HttpService, MarketplaceService,
DataStoreService, MemoryStoreService, CollectionService, BadgeService,
Avatar*, SoundService, TestService, ScriptContext` (itself a service:
`DescribedCreatable<ScriptContext, ServiceInstance>`).

Mac-shell services around it: `InputCapture`, `EmbeddedWebView`,
`RBMessageBus` protocols, `MainAppBridge/MenuBarBridge`.

World content lives under `Workspace` (`BasePart`, `Model`, `Terrain` voxels,
`MeshPart`, constraints/assemblies). Replication, prediction, and physics are
all gated on DataModel/Workspace membership (see doc 03).

## How Lua sees it

Through `RBX::Reflection` descriptors, not raw pointers: `PropDescriptor`
3089 refs, `BoundFuncDesc` 1692, `EventDesc` 649, `EnumDesc` 588,
`TypedPropertyDescriptor` 505, `BoundYieldFuncDesc` 413, plus
`ClassDescriptor` / `DescribedBase` / `FunctionDescriptor::Arguments` /
`ValueTable` / `Variant` / `Tuple`. Each Instance class, property, method,
event, and enum is a descriptor row; the Luau bridge types
(`LuauBuffer`, `LuauInteger`, `LuauCollectionHandle`) carry values across.
`include/rbx/reflection.hpp` reconstructs the pattern.

## Lifecycle (observed order)

1. Join (deeplink/protocol URL → `handleDeeplinkUrlV2` `0x1000376fd` →
   `RBLinkingProtocol detectURL`, session via `0x1051e5369`).
2. `AppUpdateAdapter::init` (`0x1051efaa2`, `[FLog::AppUpdate]`) registers
   `"MacDesktopClient"` + DataModel pointer; the `+0x108` "initialized" flag
   (`0x1051efba0` reads it) guards re-entry.
3. Async handoff: job object (0x30 bytes) onto the scheduler queue
   (`0x10521311e` → `0x104f54dc6` → `0x104f54c96` → `0x104f54876` →
   `0x104545ba0`, wake `0x102ee57de`). Doc 04 has the mechanics.
4. Run: `TaskScheduler` jobs — `Heartbeat/Stepped/RenderStepped/PreRender/
   PostSimulation`, `DeserializePacketsJob`, `WaitingHybridScriptsJob`,
   `GcJob`, `TelemetryJob` (`FLog::DataModelJobs`, 20 refs).
5. Leave/teleport/suspend: `leaveGameWithCallback:isShuttingDown:`
   (`0x10002f6b6`, session reports + telemetry), `ugcGameWillClose/DidClose`,
   `CloseDataModel` → SchedulerFacet drain
   (`waitingThreads`, `YieldEvent->waiterCount`) →
   `VmCollectionFacet` free (`state thread count %zu`).

## C++ sketch

`include/rbx/datamodel.hpp` — `DataModel : ServiceProvider` with typed
service accessors, session factory singleton, and the join/leave lifecycle
as observed. `src/reflection_demo.cpp` shows descriptor registration +
property get/set dispatch in the observed style.
