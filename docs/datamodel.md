# DataModel

In one paragraph: the DataModel is the root of one game session. Every
service hangs off it. Join creates it. Leave, teleport, or shutdown drains
dependent work first, then clears services one by one, then releases script
state. If you keep a raw pointer across close, it will betray you.

## Big picture

```text
JoinRequested (deeplink / protocol)
  -> BringUpDataModel
  -> AppUpdateAdapter init ("MacDesktopClient")
  -> scheduler handoff
  -> Running (frame jobs)
  -> Leaving (session reports, telemetry)
  -> CloseDataModel
  -> scheduler drain
  -> VM pool release
  -> NoSession
```

Teleport is `Running -> Leaving -> JoinRequested`.

Session lookup for deeplinks and leave-game goes through the session
accessor (`0x1051e5369`).
Nearby singletons are a factory/manager (`0x105235e96` → `0x107510308`)
and a global manager (`0x10118fab6` → `0x107a61588`).

## Services

338 service-like names appear in the binary. The player-facing set in a
running game includes:

**World:** `Workspace`, `Lighting`, `MaterialService`, `Teams`,
`StarterPack`, `StarterGui`, `StarterPlayer`, `ReplicatedFirst`,
`ReplicatedStorage`, `ServerStorage`, `ServerScriptService`,
`SoundService`, `TweenService`, `DebrisService`, `CollectionService`,
`ProximityPromptService`, `PathfindingService`, `ContextActionService`,
`UserInputService`, `GamepadService`, `HapticService`, `VRService`,
`GuiService`, `TextService`, `TextChatService`, `ChatService`,
`VoiceChatService`, `RunService`, `TimerService`.

**Players and social:** `Players` / `PlayersService`, `PlayerDataService`,
`FriendService`, `GroupService`, `SocialService`, `ChatService`,
`UGCAvatarService`, `AvatarChatService`, `AvatarCreationService`,
`AvatarEditorService`, `ThirdPartyUserService`, `UserService`,
`PlayerEmulatorService`, `PartyEmulatorService`.

**Saving and commerce:** `DataStoreService`, `MemoryStoreService`,
`MarketplaceService`, `BadgeService`, `GamePassService`, `PointsService`,
`CommerceService`, `OpenCloudService`, plus `AssetService`,
`InsertService`, `PackageService`, `UGCValidationService`,
`PublishService`, `VersionControlService`, `TeamCreateService`.

**Scripting:** `ScriptContext` (itself a service),
`CoreScriptSyncService`, `ScriptDebuggerService`, `ScriptProfilerService`,
`ScriptRegistrationService`, `ScriptService`, `ServerScriptService`,
`ModuleScript` hosting, `LuauExpressionService`,
`LuauScriptAnalyzerService`, `LiveScriptingService`, `TestService`,
`ScriptChangeService`.

**Media and capture:** `CaptureService`, `VideoCaptureService`,
`VideoScreenCaptureService`, `SoundService`.

**Platform:** `AppUpdateService`, `AppStorageService`,
`LocalStorageService`, `UserStorageService`, `MemStorageService`,
`PlatformCloudStorageService`, `LogService`, `NotificationService`,
`ToastNotificationService`, `PermissionsService`, `PolicyService`,
`BrowserService`, `WebViewService`, `HttpService`, `CookiesService`,
`GuidRegistryService`, `UniqueIdLookupService`, `DeviceIdService`,
`SessionService`, `LoginService`, `TeleportService`, `MatchmakingService`.

**Engine-internal:** `PhysicsService`, `ProcessInstancePhysicsService`,
`CSGDictionaryService`, `NonReplicatedCSGDictionaryService`,
`JointsService`, `GeometryService`, `HeightmapImporterService`,
`FileMeshDataChunkReader`, `TextureGenerationService`,
`MaterialGenerationService`, `ModelTranscoder`, `SlimReplicationService`,
`SlimAnimationReplicationService`, `FileSyncService`, `LiveSyncService`,
`ReflectionService`, `InstanceRegistryService`, `ChangeHistoryService`,
`RunService`, `ProceduralBehaviorSchedulerService`,
`SceneAnalysisService`, `HeatmapService`, `RbxAnalyticsService`,
`TelemetryService`, `EventIngestService`, `IXPService`,
`LocalizationService`, `ModerationService`, `SafetyService`,
`CrossExperienceVoiceController`, `ExperienceAuthService`,
`ExperienceNotificationService`, `ExperienceService`,
`RtMessagingService`, `MessageBusService`, `InternalMessagingService`,
`RequestOrchestratorService`, `CloudCRUDService`,
`CloudExecutionService`, `OpenCloudService`.

Studio-only, test, mock, and flag-gated names ship in the player but do
not run in games: `Studio*`, `Enable*` / `DebugEnable*`, `Mock*`,
`TestService*`, `DummyService`, `NoService`, `ExampleV2Service`.

## How Lua sees it

Every class, property, method, and event is described by a descriptor row.
Script access dispatches through those rows.

Descriptor families in this build:

| Family | Count |
|---|---|
| `PropDescriptor` | 3089 |
| `BoundFuncDesc` | 1692 |
| `EventDesc` | 649 |
| `EnumDesc` | 588 |
| `TypedPropertyDescriptor` | 505 |
| `BoundYieldFuncDesc` | 413 |

Plus `ClassDescriptor`, `DescribedBase`, `FunctionDescriptor::Arguments`,
`ValueTable`, `Variant`, `Tuple`, `RefPropertyDescriptor`, and typed
property descriptors per engine type. Values cross as `Variant` values.

Example — reading a property the way the engine does:

```cpp
// Simplified illustration.
Variant v = instance.getProperty("Name");
if (v.type == Variant::Type::String)
  print(v.str);
```

Example — firing an event:

```cpp
// Simplified illustration.
plate.touched.connections.push_back(
  [&](const Tuple&) { print("touched"); });
plate.touched.connections[0]({});
```

See `include/rbx/reflection.hpp` and `src/reflection_demo.cpp`.

## Players

`Players` is a full service, not just a list.

Events declared at the Players event-descriptor table (`0x100bae0c0`) include `PlayerAdded`,
`PlayerRemoving`, `PlayerConnecting`, `PlayerDisconnecting`,
`PlayerRejoining`, `PlayerMembershipChanged`, and `PlayerChatted`.
Operations declared at the Players function-descriptor table (`0x100bac24b`) include `getPlayers`,
`playerFromCharacter`, `ResetLocalPlayer`, user and thumbnail lookups,
character-description creation, moderation and ban calls, and chat helpers.

Properties include `CharacterAutoLoads`, `MaxPlayers`,
`MaxPlayersInternal`, `PreferredPlayers`, `PreferredPlayersInternal`,
`RespawnTime`, `LocalPlayer`, `localPlayer`, `NumPlayers`, and
`numPlayers`.

A `Player` used by client calls must live under `game.Players`:

```text
InvokeClient: player must be a child of game.Players
Player:Kick() can only be called on a Player under Players
```

Parenting is part of security and replication, not decoration.

Lifecycle notifications stay separate: connecting, disconnecting,
rejoining, membership changes, public `PlayerAdded` / `PlayerRemoving`,
and local-player transitions. Some surfaces expose `PlayerRemoved`
separately.

## Shutdown order

Step by step, close does this:

1. Mark the DataModel shutting down.
2. Drain scheduler work: waiting threads, then yield waiters.
3. Clear service containers in a fixed order. Log points include
   `StarterPack`, `StarterPlayer`, `TextChatService`, `StarterGui`,
   `CoreGui`, `Teams`, `Lighting`, `JointsService`, `TestService`,
   `ServerScriptService`, `ReplicatedStorage`, `ReplicatedFirst`,
   `ServerStorage`, `ChatService`, and `LocalizationService`.
4. Release VM state only after the drains above.
5. Release the DataModel itself.

The close routine (`CloseDataModel`) lives at `0x104542918`. The exact service list is
build-specific and partly flag-gated.

Behavior in code form:

```cpp
// Simplified illustration.
void close(DataModel& dm) {
  dm.setShuttingDown();      // stop new work
  drainScheduler();          // waiting threads, yield waiters
  clearServicesInOrder(dm);   // StarterPack, StarterGui, Lighting, ...
  releaseVms();              // VM collection last
}
```

C++ model: `include/rbx/datamodel.hpp`, demo in
`src/datamodel_demo.cpp`.

## Addresses in this build

| Place | Address |
|---|---|
| Session accessor | `0x1051e5369` |
| Factory singleton | `0x105235e96` → `0x107510308` |
| Global manager | `0x10118fab6` → `0x107a61588` |
| Close routine | `0x104542918` |
| Players events | `0x100bae0c0` |
| Players operations | `0x100bac24b` |

## What is not claimed

- Exact service offsets and vtable layouts across versions.
- The full flag-gated service list for other builds.
- Wire formats for replication (see the [Networking reference](replication-net.md)).
