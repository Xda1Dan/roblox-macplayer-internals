# DataModel: what it is and what it contains

## What it is

`RBX::DataModel` is the **root `Instance`** of one game session and the
`ServiceProvider` everything resolves against (230 references). Code paths
uniformly assume "current DataModel or nothing": `AssetService::* … DataModel
is null`, `createSessionFromAssetID: DataModel not available`,
`Cannot predict instance {}: Only instances under Workspace can be predicted`.

It is **per-session, not a global singleton**: created on join
(`BringUpDataModel`), destroyed on leave/teleport (`CloseDataModel` →
`Closing Script Context` → scheduler drain → VM pool free,
`LogOnDMSetShuttingDown`). The nearby singletons are a factory/manager
(`0x105235e96` → `0x107510308`, call-once guarded) and a global manager
(`0x10118fab6` → `0x107a61588` + destructor at exit). Session lookup for
deeplinks and leave-game goes through `0x1051e5369` (live lookup via
`0x100043b24` + virtual `0x1051e9b6a`, or locator call with key
`0x107510170`).

## Service catalog (338 names found in the binary)

Player-facing services (present in a running game):

*World*: `Workspace`, `Lighting`, `MaterialService`,
`Teams`, `StarterPack`, `StarterGui`,
`StarterPlayer`, `ReplicatedFirst`, `ReplicatedStorage`, `ServerStorage`,
`ServerScriptService`, `SoundService`, `TweenService`, `DebrisService`,
`CollectionService`, `ProximityPromptService`, `PathfindingService`,
`ContextActionService`, `UserInputService`, `GamepadService`, `HapticService`,
`VRService`, `GuiService`, `TextService`, `TextChatService`, `ChatService`,
`VoiceChatService`, `RunService`, `TimerService`.

*Players & social*: `PlayersService`/`Players`, `PlayerDataService`,
`FriendService`, `GroupService`, `SocialService`, `ChatService`,
`UGCAvatarService`, `AvatarChatService`, `AvatarCreationService`,
`AvatarEditorService`, `ThirdPartyUserService`, `UserService`,
`PlayerEmulatorService`, `PartyEmulatorService`.

*Persistence & commerce*: `DataStoreService`, `MemoryStoreService`,
`MarketplaceService`, `BadgeService`, `GamePassService`, `PointsService`,
`CommerceService`, `OpenCloudService` — plus
`AssetService`, `InsertService`, `PackageService`, `UGCValidationService`,
`PublishService`, `VersionControlService`, `TeamCreateService`.

*Scripting support*: `ScriptContext` (a service itself),
`CoreScriptSyncService`, `ScriptDebuggerService`, `ScriptProfilerService`,
`ScriptRegistrationService`, `ScriptService`, `ServerScriptService`,
`ModuleScript` hosting, `LuauExpressionService`, `LuauScriptAnalyzerService`,
`LiveScriptingService`, `TestService`, `ScriptChangeService`.

*Media/capture*: `CaptureService`, `VideoCaptureService`,
`VideoScreenCaptureService`, `SoundService`.

*Platform/shell*: `AppUpdateService`, `AppStorageService`,
`LocalStorageService`, `UserStorageService`, `MemStorageService`,
`PlatformCloudStorageService`, `LogService`, `NotificationService`,
`ToastNotificationService`, `PermissionsService`, `PolicyService`,
`BrowserService`, `WebViewService`, `HttpService`, `CookiesService`,
`GuidRegistryService`, `UniqueIdLookupService`, `DeviceIdService`,
`SessionService`, `LoginService`, `TeleportService`, `MatchmakingService`.

*Engine-internal*: `PhysicsService`, `ProcessInstancePhysicsService`,
`CSGDictionaryService`, `NonReplicatedCSGDictionaryService`,
`JointsService`, `GeometryService`, `HeightmapImporterService`,
`Mesh` pipeline (`FileMeshDataChunkReader`), `TextureGenerationService`,
`MaterialGenerationService`, `ModelTranscoder` (`ModelDeliveryTrace`),
`SlimReplicationService`, `SlimAnimationReplicationService`,
`FileSyncService`, `LiveSyncService`, `ReflectionService`,
`InstanceRegistryService`, `ChangeHistoryService`, `RunService`,
`ProceduralBehaviorSchedulerService`, `SceneAnalysisService`,
`Aerodynamics` (logs), `HeatmapService`, `RbxAnalyticsService`,
`TelemetryService`, `EventIngestService`, `IXPService` (experiments),
`LocalizationService`, `ModerationService`, `SafetyService`,
`CrossExperienceVoiceController`, `ExperienceAuthService`,
`ExperienceNotificationService`, `ExperienceService`,
`RtcMessaging`/`RtMessagingService`, `MessageBusService`,
`InternalMessagingService`, `RequestOrchestratorService`,
`CloudCRUDService`, `CloudExecutionService`, `OpenCloudService`.

Studio-only / test / mock / flag-gated (shipped in the player binary but not
active in games): the `Studio*` family, `Enable*`/`DebugEnable*` toggles,
`Mock*`, `TestService*`, `DummyService`, `NoService`, `ExampleV2Service`,
`ReflectionService` (test helper).

## How Lua sees it: Reflection

Descriptor counts: `PropDescriptor` 3089, `BoundFuncDesc` 1692, `EventDesc`
649, `EnumDesc` 588, `TypedPropertyDescriptor` 505, `BoundYieldFuncDesc` 413,
plus `ClassDescriptor`, `DescribedBase`, `FunctionDescriptor::Arguments`,
`ValueTable`, `Variant`, `Tuple`, `RefPropertyDescriptor`,
`TypedPropertyDescriptor<T>` per engine type. Values cross as `Variant`
(`LuauBuffer`, `LuauInteger`, `LuauCollectionHandle` bridge types).
`include/rbx/reflection.hpp` + `src/reflection_demo.cpp` model registration,
get/set dispatch, method calls, and event connections.

## Players service

`Players` is a reflected DataModel service with a larger surface than simple
player storage. The service registration body at `ram:0x100bae0c0` declares
player lifecycle events including `PlayerAdded`, `PlayerRemoving`,
`PlayerConnecting`, `PlayerDisconnecting`, `PlayerRejoining`,
`PlayerMembershipChanged`, and `PlayerChatted`. The larger registration body
at `ram:0x100bac24b` exposes operations such as `getPlayers`,
`playerFromCharacter`, `ResetLocalPlayer`, user/name/thumbnail requests,
character-description creation, moderation/ban APIs, and chat helpers.

The reflected properties include `CharacterAutoLoads`, `MaxPlayers`,
`MaxPlayersInternal`, `PreferredPlayers`, `PreferredPlayersInternal`,
`RespawnTime`, `LocalPlayer`, `localPlayer`, `NumPlayers`, and `numPlayers`.
These are descriptor registrations; they do not by themselves prove that a
client can mutate every property. Native security/capability and server/client
context checks still apply.

The player container is also an execution/replication boundary. The binary
contains checks such as `InvokeClient: player %s must be a child of game.Players`
and `Player:Kick() can only be called on a Player under Players`. Thus a
`Player` instance's parent/service membership is semantically significant,
not cosmetic.

## Lifecycle state machine

`NoSession → JoinRequested (deeplink/protocol) → BringUpDataModel →
AppUpdateAdapter::init("MacDesktopClient") → async scheduler handoff →
Running (frame jobs) → Leaving (session reports, telemetry) →
CloseDataModel → SchedulerFacet drain → VmCollectionFacet free → NoSession`,
with teleport as `Running → Leaving → JoinRequested`.

The inspected close routine at `ram:0x104542918` performs more than a root
state transition. It iterates service/container lists and clears selected
contents in a defined teardown sequence, with log points for
`StarterPack`, `StarterPlayer`, `TextChatService`, `StarterGui`, `CoreGui`,
`Teams`, `Lighting`, `JointsService`, `TestService`,
`ServerScriptService`, `ReplicatedStorage`, `ReplicatedFirst`,
`ServerStorage`, `ChatService`, and `LocalizationService` among others.
The exact list is build-specific and some entries are flag-gated. This is why
holding an `Instance` or service pointer past close is unsafe even before the
VM pool is destroyed.

The DataModel is therefore best modeled as an ownership/lifetime boundary:
network and script work carries weak/shared references to it, close marks the
boundary and drains work, then child service contents and script state are
released in stages.
`C++`: `include/rbx/datamodel.hpp`, demo in `src/datamodel_demo.cpp`.
