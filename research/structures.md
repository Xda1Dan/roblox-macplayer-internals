# Structures — verified fields (RobloxPlayer macOS, build f9247f9560044102)

All offsets are object-relative and belong to **this build only**. The
load-bearing VM layouts (`lua_State`, `CallInfo`, `Proto`, `Closure`,
`TString`, `LuaTable`) are in `vm-research/luau-vm.md` and are mirrored as
compile-time constants in `vm-repro/include/rbxvm/layouts.hpp`; they were
recovered from `luaF_newproto`/`luaF_freeproto`, `lua_resetthread`,
`luaS_newlstr`, and the interpreter entry sequence.

This page only lists the ScriptContext/scheduler fields that the reproduction
or the ScriptContext notes actually rely on. Fields marked *(narrative)* come
from the broader site docs and were not re-verified in the cleanup pass.

## ScriptContext (ctx)

| Offset | Type | Meaning | Evidence |
|--------|------|---------|----------|
| `+0x28` | double | execution-time accumulator (outermost resumes) | `FUN_101556cb4` |
| `+0x38` | u32 | `startScript` re-entrancy counter (>3 fatal) | `FUN_10158afe8` |
| `+0x1a8` | u32 | startScript flags word (OR 4 when in the telemetry set) | `FUN_10158afe8` |
| `+0x208` | queue | scheduler enqueue target | `FUN_10158afe8` → `sub_1015a81a8` |
| `+0x488` | sub-object | permission sub-object; predicate pointer at `+0x60` | `FUN_1015c4980` |
| `+0x4b8` | sub-object | shutdown state: `+0x38` deadline (double), `+0x40` bit0 started | `FUN_1015c486e` |
| `+0x560` | set | script telemetry/registration set | `FUN_10158afe8` |
| `+0x608` | map | script-name map | `FUN_10158afe8` |
| `+0x8a8` | set | script registration set | `FUN_10158afe8` |
| `+0xa18` | int | facet phase; `< 3` valid, `>= 3` fatal "Invalid Facet Access" | `FUN_101556664` |
| `-0x888` | ptr | facet sub-object base (returned while phase `< 3`) | `FUN_101556664` |

The old `+0x38`/`+0x40` deadline rows were actually the `+0x4b8` sub-object's
fields; the old `+0x28` "C-stack base/limit" row was the execution-time
accumulator. The phase gates are valid while the counter is **below** 3, not
above it.

## VM lane / scheduler

| Offset | Type | Meaning | Evidence |
|--------|------|---------|----------|
| `+0x8b8` | facet | facet object returned by `FUN_101543ea6` (fatal at phase `>= 3`) | `FUN_101543ea6` |
| `+0xac0` | queue | scheduler queue (accessor gated the same way) | `FUN_1040d49da` |
| `+0x12a0` | int | lane facet phase (`< 3` valid) | `FUN_1040d49da` |

The queue's `+0x68`/`+0x70` begin/end pointers and the 8-byte entry shape are
*(narrative)* from `docs/scheduler-internals.md`.

## Thread status byte (`thread + 0xac`)

Used by the scheduler layer (not part of `lua_State`):

| Value | Meaning |
|-------|---------|
| `0` | running |
| `1` | yielded / suspended |
| `2` | deferred |
| `3` | waiting |

## TaskScheduler job *(narrative)*

| Offset | Type | Meaning |
|--------|------|---------|
| `+0x00` | ptr | job vtable |
| `+0x18` | std::string | job name (libc++ SSO) |
| `+0x48` | double | accumulated / last time in job |
| `+0x58` | byte | state |
| `+0xe0` | int | timing/profiling counter |

Job list begin/end at `+0xd0`/`+0xd8` (16-byte entries) is *(narrative)*.

## Scheduler FVars *(narrative)*

`TaskSchedulerInit`, `TaskSchedulerRun`, `TaskSchedulerJobInitThreads`,
`TaskSchedulerJobInGameThreads`, `TaskSchedulerJobDeadlineMs`,
`TaskSchedulerMaxNumOfJobs`, `TaskSchedulerMaxNumOfArbiters`,
`TaskSchedulerMaxStackSizeBytes`, `TaskSchedulerTaskMaxStackSizeBytes`,
`TaskSchedulerTasksDefaultMaxCountPerCycle`,
`TaskSchedulerTasksDefaultMaxTimeMsPerCycle`, `TaskSchedulerAutoThreadLimit`,
`TaskSchedulerThreadMin`, `TaskSchedulerTargetFps`,
`TaskSchedulerBackgroundCycleRateMs`, `TaskSchedulerWatchThreads`.

## RunService event order *(narrative)*

```
PreRender → RenderStepped → PreSimulation → Stepped (simulation)
          → PostSimulation → PreAnimation → Heartbeat
```

## DataModel services cleared on close *(narrative)*

`Workspace`, `World`, `Lighting`, `Teams`, `ChatService`, `TextChatService`,
`StarterGui`, `CoreGui`, `StarterPack`, `StarterPlayer`, `JointsService`,
`ChangeHistoryService`, `ReplicatedFirst`, `ReplicatedStorage`,
`RobloxReplicatedStorage`, `ServerStorage`, `ServerScriptService`,
`BackendServerScriptService`, `BackendReplicatedStorage`,
`BackendServerStorage`, `LocalizationService`, `TestService`.

## FVar / FLog *(narrative)*

- FVar register: `0x102f4a716`; registry init: `sub_104c18388`; name table at
  `0x1073b8040`.
- FLog: `0x102f6fdf0`; fatal: `0x102f6d5d1`.
- Opcode dispatch tables: `0x1072d8a30` (Path A) / `0x1072d9230` (Path B).
