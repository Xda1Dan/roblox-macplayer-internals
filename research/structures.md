# Key Structures — RobloxPlayer (macOS)

Field layouts recovered from decompilation. All offsets are from the object base.

> **Corrections (2026-10):** three fields in this file were corrected by the
> ScriptContext deep dive (`vm-research/01-scriptcontext.md`):
> 1. The "lifecycle level ≥ 3" facet gates are the **reverse**: the facet
>    (`ctx−0x888`) and scheduler queue (`lane+0xac0`) are valid while the phase
>    counter (`ctx+0xa18` / `lane+0x12a0`) is **< 3**; `≥ 3` is a fatal
>    `"Invalid Facet Access"`.
> 2. ScriptContext `+0x28` is a **double execution-time accumulator**, not a
>    C-stack base/limit.
> 3. The shutdown deadline lives in a **sub-object at `ctx+0x4b8`** (`+0x38`
>    deadline, `+0x40` bit0 shutdown-started), i.e. `ctx+0x4f0`/`ctx+0x4f8`
>    context-relative — the `+0x38/+0x40` rows below were that sub-object's
>    fields.

## ScriptContext / VM lane

| Offset | Type | Meaning |
|--------|------|---------|
| `+0x00` | int | C-stack depth counter |
| `+0x08` | uint64 | C-stack high-water mark |
| `+0x18` | ptr | **vtable** (execution hooks `+0x640`, `+0x6a0`) |
| `+0x20` | long | C-stack pointer |
| `+0x28` | long | C-stack base / limit |
| `+0x38` | double | shutdown deadline (wall clock) |
| `+0x40` | byte | bit0 = shutdown started |
| `+0x58` | byte | script state |
| `+0x100` | ptr | parallel / VM-lane sub-object (state flag at `+0x515`) |
| `+0xa18` | int | lifecycle level (must be ≥ 3 for facet access) |
| `+0xac0` | ptr | **scheduler queue** (time-ordered coroutine queue) |
| `+0x12a0` | int | lifecycle level (queue access gate, must be ≥ 3) |
| `-0x888` | ptr | facet sub-object base |

## Scheduler queue (at `lane + 0xac0`)

A time-ordered priority queue. Entries are 8 bytes (1 pointer each).

| Offset | Type | Meaning |
|--------|------|---------|
| `+0x68` | ptr | queue begin |
| `+0x70` | ptr | queue end |

Each entry: `{scheduled_time (double), closure, args, flags}`. Insert computes
`scheduled_time = delay + now` and inserts in time order.

## TaskScheduler Job object

| Offset | Type | Meaning |
|--------|------|---------|
| `+0x00` | ptr | job vtable |
| `+0x18` | std::string | **job name** (libc++ SSO: inline at `+0x19` if flag `&1==0`, else heap at `+0x28`) |
| `+0x48` | double | accumulated / last time in job |
| `+0x58` | byte | **state** |
| `+0xe0` | int | timing / profiling counter |

Job list (on the TaskScheduler object): begin `+0xd0`, end `+0xd8`, 16-byte entries.

## Coroutine / thread state byte (`thread + 0xac`)

| Value | Meaning |
|-------|---------|
| `0` | running |
| `1` | yielded / suspended |
| `2` | deferred |
| `3` | waiting |

## TaskScheduler FVars (selection)

`TaskSchedulerInit`, `TaskSchedulerRun`, `TaskSchedulerJobInitThreads`,
`TaskSchedulerJobInGameThreads`, `TaskSchedulerJobDeadlineMs`, `TaskSchedulerMaxNumOfJobs`,
`TaskSchedulerMaxNumOfArbiters`, `TaskSchedulerMaxStackSizeBytes`,
`TaskSchedulerTaskMaxStackSizeBytes`, `TaskSchedulerTasksDefaultMaxCountPerCycle`,
`TaskSchedulerTasksDefaultMaxTimeMsPerCycle`, `TaskSchedulerAutoThreadLimit`,
`TaskSchedulerThreadMin`, `TaskSchedulerTargetFps`, `TaskSchedulerBackgroundCycleRateMs`,
`TaskSchedulerWatchThreads`.

## Known TaskScheduler jobs

- **Heartbeat** (`RunService.Heartbeat`) — `prof/Jobs/Heartbeat`
- **Render** (`RenderJob`, `PreRenderJob`) — `Render Duty`, `Render FPS`, `Render Nominal FPS`
- **Physics** (`StepPhysics`, `WorldRoot::StepPhysics`) — `PreSimulation`, `PostSimulation`, `PreAnimation`
- **DataModel jobs** — `TextScraper…stepDataModelJob`, `DynamicTranslation…stepDataModelJob`,
  `DataModelRecorderJob`, `LoadTestCoordinator`

## RunService frame-loop events (ordered)

```
PreRender → RenderStepped → PreSimulation → Stepped (simulation) → PostSimulation → PreAnimation → Heartbeat
```

## DataModel services (cleared on close)

`Workspace`, `World`, `Lighting`, `Teams`, `ChatService`, `TextChatService`, `StarterGui`,
`CoreGui`, `StarterPack`, `StarterPlayer`, `JointsService`, `ChangeHistoryService`,
`ReplicatedFirst`, `ReplicatedStorage`, `RobloxReplicatedStorage`, `ServerStorage`,
`ServerScriptService`, `BackendServerScriptService`, `BackendReplicatedStorage`,
`BackendServerStorage`, `LocalizationService`, `TestService`

## FVar / FLog systems

- **FVar register:** `func_0x000102f4a716(uint64_t* out_flag, int kind, char* name)`
- **FVar registry init:** `sub_104c18388` (table at `0x10789f860`–`0x10789fa30`)
- **Static name pointer table:** `0x1073b8040`
- **FLog:** `func_0x000102f6fdf0(level, channel, format, …)`; fatal `func_0x000102f6d5d1(level, msg)`
- **Opcode dispatch table:** `0x1072d9230` (256 entries)

## ObjC startup

`RobloxPlayerAppDelegate` / `RBXAppDelegate` (class-name strings `0x106bbdc6e` / `0x106bbdcc2`).
`applicationDidFinishLaunching` → skip update → parse placeId → "Initializing lua app" →
"Bind to services in game-engine" → "Launching game from web" → machine-id check →
`reportClientStatus`.
