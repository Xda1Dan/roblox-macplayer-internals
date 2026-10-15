# Roblox macOS Player Internals

Notes on how the Roblox macOS player is put together, with small C++
examples that show the same shapes.

Documentation site: https://xda1dan.github.io/roblox-macplayer-internals/

The focus is on the parts that matter if you want to understand the engine:

```text
platform / bootstrap
        |
        v
DataModel -- services -- ScriptContext -- Luau VMs
    |              |             |
    |              |             +- waiting / resume jobs
    |              +- reflection / events
    |
    +- NetworkClient / RakNet -> packet queues -> deserializer -> replicator

almost everything asynchronous runs as a job in the TaskScheduler
```

## Target

| Property | Value |
|---|---|
| Application | `RobloxPlayer.app` |
| Player build | `0.742.0.7421053` |
| Target name | `MacPlayer-version-f9247f9560044102` |
| Format | 64-bit little-endian x86_64 Mach-O executable |
| Approximate size | 120 MB |
| Image base | `0x100000000` |
| SHA-256 | `ac9ad59b3aa3805bf2d1c89fb6ff9c7941bf2e57af17a81cbc26abe94a022c47` |

Addresses in these notes are virtual addresses. For this build,
file offset + `0x100000000` = virtual address.

## How to read these notes

Suggested reading order:

1. [Binary overview](docs/overview.md) — layout, entry, app boundary.
2. [Script execution, end to end](docs/script-execution.md) — start here
   for scripting: permission check, start, resume, wait, require, shutdown.
3. [TaskScheduler internals](docs/scheduler-internals.md) — how jobs are
   queued, stored in rings, and run on workers.
4. [Luau VM lifecycle](docs/luau-vm-lifecycle.md) — how a `lua_State` is
   created, wrapped, protected, and hardened.
5. [Replication deserialize path](docs/replication-deserialize.md) — how
   received packets become typed work without blocking the socket thread.
6. [DataModel](docs/datamodel.md) — the per-session root, services,
   ownership, and shutdown order.
7. [Scripting reference](docs/scripting-luau.md) — facets, policy words,
   module states, teardown order.
8. [Networking reference](docs/replication-net.md) — transport, queues,
   replicator items, DataModel apply rules.
9. [Scheduler reference](docs/job-scheduler.md) — submit path, node pool,
   frame jobs, timing.
10. [Objective-C bridge](docs/objc-bridge.md) — app callbacks, web views,
    native-to-engine events.
11. [Addresses and limits](docs/evidence-ledger.md) — where each address
    lives and what is still unknown.

The raw working notes behind these write-ups — the offset reference, structure layouts, the
early report, and the per-symbol dump cross-check — are in
[`research/`](research/README.md). They are versioned here but not published on the site.

Each walkthrough and reference page follows the same shape:

```text
What it does
  -> Big picture
  -> Step by step
  -> Behavior in code form
  -> What the machine code looks like
  -> Addresses in this build
  -> What is not claimed
```

Short machine-code excerpts are facts about the binary. Longer C++
listings are original illustrations, not player source.

## The important ideas

### DataModel is an ownership boundary

The DataModel is the root of one game session. Join creates it. Network
and script work hold weak or shared references to it. Close drains that
work first, then clears service containers one by one, then releases
script state. The close routine (`CloseDataModel`, `0x104542918`) clears StarterPack,
StarterPlayer, TextChatService, StarterGui, CoreGui, Teams, Lighting,
ReplicatedStorage, ReplicatedFirst, ServerStorage, ServerScriptService,
ChatService, and others.

Holding an `Instance` or service pointer across close is unsafe by design,
even before the VM pool is gone.

### Scripts run in stages

```text
Script instance
  -> permission check (RunContext + capabilities + identity + ancestry)
  -> source or bytecode lookup
  -> compile cache or compile
  -> bytecode check and load
  -> execution record
  -> VM thread
  -> Luau run
  -> finish, error, or yield
  -> waiting queue
  -> later resume
```

The start path (`0x10158ac46`) allows at most three levels of
re-entrant start, opens a `Script Start` region, hands off to a VM thread,
and keeps shared ownership alive across callbacks. The waiting paths
(`WaitingHybridScriptsJob` drain, `0x1015a8d94` / `0x1015a9249`) claim one resume flag, drain a
bounded number of waiters, respect a deadline, and release each waiter.

Permission is decided by the admission predicate (`0x10142a7a6`). Server scripts need the native
`RunServerScript` capability. Client scripts need `RunClientScript`.
Service location, ancestry, DataModel state, and plugin rules are checked
as well. Lua code does not grant itself a higher level.

On the Luau side, `lua_newstate` (`0x10299fd5b`), the protected load
boundary (`0x1029a9f36`), `ScriptContextResume` (`0x1015566d6`), and
`task.wait` (`0x1014cf74a`) are real function starts. `task.wait` marks the
coroutine waiting and schedules a later resume. It does not sleep a worker
thread. `ScriptContextResume` checks pending state, shutdown deadline, and
a C-stack limit before re-entering.

State creation runs through the VM-creation path (`0x1015b92cd`): allocator and context,
`lua_newstate`, VM wrapper, error hooks, libraries and Roblox bindings,
`_G` and sandbox setup, then the VM collection owns it. States belong to
the collection. Scripts borrow threads from it.

Execution enters through a dispatch stub (`0x1029adfd3`) that checks the
`singlestep` flag and jumps into one of two opcode tables —
`0x1072d8a30` (single-step loop) or `0x1072d9230` (normal loop) — each
with 93 handler slots in identical positions but separate handler copies.
The slots do not follow public Luau `LOP_*` order. The permutation is
exact: the byte on disk is `wire = (227 * canonical) & 0xFF`, and the
dispatched byte is `T[wire]`, for the 256-byte table `T` at `0x1063bb686`
(the binary ships the inverse `R` at `0x10648bb74`). All 93 opcodes were
resolved this way and cross-checked against their handler bodies. See
[Luau VM encodings](https://xda1dan.github.io/roblox-macplayer-internals/luau-vm-encodings/)
for the full map and for the address-relative pointer encodings applied to
some VM object fields.

The public name `lua_pcall` is absent, but protected execution is present:
`luaB_pcall` (`0x10298b913`), `luaB_pcallrun` (`0x10298b946`),
`luaD_rawrunprotected` (`0x102995084`), `luaD_throw`, and `luaG_runerror`
are all present as Luau function starts.

Some Luau error paths build strings with a split pointer. At
`0x102988254`, a dummy pointer plus signed delta `-0x2f9400`
reconstructs `0x1069a3378` (`"stack limit"`) before the
`luaG_runerror`-shaped call. This is selective, not a general compiler
rule. Direct string references still exist elsewhere.

Module `require` at `0x10158dd88` checks security context, tracks
per-VM module state, reports circular dependencies, rejects destroyed
modules, and requires exactly one return value.

Async callbacks at `0x101557e8c` resolve the implementing script
through a weak reference first. Destroyed scripts are rejected before any
VM work. Coroutine resume at `0x1015977c1` only accepts
suspended / awaiting states.

### RakNet moves bytes, the replicator interprets them

```text
RakNet::RecvFromLoop
  -> packet-return queue
  -> DeserializePacketsJob / buffered deserializer
  -> replicator receive path
  -> typed Deserialized*Item
  -> DataModel change
```

The buffered deserializer at `0x10564dd3c` drains queued records,
updates receive timing and queue state, builds typed temporary records,
and hands them to replication processing. It only ever sees typed records
because `Replicator::OnReceive` (`0x10561fb20`) dispatches the
message-ID byte first: `0x15`/`0x16` connection teardown, `0x1b` physics
(dropped when the receiver is not ready, basis-index ordered otherwise),
`0x81`/`0x83`/`0x86` replication queued via the item enqueue
(`0x1055fa738`). The exact wire
schema is not claimed here.

### The scheduler carries everything

Objective-C callbacks, network receives, Lua callbacks, frame work, and
maintenance all become scheduled work:

```text
job build (0x10521311e) -> dispatch (0x104f54dc6) -> enqueue (0x104f54c96)
                                       -> ring insert (0x104f54876) -> record ring (0x104545ba0)
                                       -> wakeup helper (0x102ee57de)
```

The record ring (`0x104545ba0`) grows and copies a circular 0x80-byte record buffer. Job
start (`0x102eec6da`) marks a job active and calls its step function.
Job stop clears the active state, calls the stop hook, records elapsed
time, and updates the arbiter. Node allocation (`0x102ee3136`) uses a
bounded pool. Running out of nodes is fatal. Workers pop from the same
queue record enqueue writes to, promote the queued weak reference back to
shared (`0x102eeac76`), and dispatch through the worker dispatch stub
(`0x102ee3332`) — a job
that dies while queued simply never runs.

Shutdown drains waiting threads and yield waiters before destroying
scheduler members. The VM collection then releases per-slot state. Work
must not keep raw VM, DataModel, or service pointers across that boundary.

The `Players` service is a full reflected service. It declares
`PlayerAdded`, `PlayerRemoving`, connecting, disconnecting, rejoining,
membership, and chat events, plus `LocalPlayer`, `MaxPlayers`,
`CharacterAutoLoads`, `RespawnTime`, `NumPlayers`, and related operations.
A `Player` used by client calls must live under `game.Players`, so
parenting is part of security and replication, not decoration.

Player join and removal cross the network and DataModel boundary:
replicated bookkeeping resolves membership under `game.Players`, then
lifecycle events go out. Removal does the reverse before dependent
references are released.

The native script path can be summarized as:

```text
permission -> source / bytecode -> compile / cache -> check / load
-> execution record -> VM thread -> Luau run
-> finish / error / yield -> scheduler -> resume
```

`task.wait` and async engine calls yield resumable VM state. Native
`loadstring` is policy-gated and can be unavailable in RobloxScript
contexts.

## C++ examples

`include/rbx/` and `src/` are original, simplified illustrations. They are
not player source and are not wire-compatible with the player.

| Example | Shows |
|---|---|
| `reflection_demo` | Descriptor registration, property and method dispatch, events |
| `jobqueue_demo` | Mutex-protected queues, worker wakeup, shutdown |
| `sso_string_demo` | Small-string-shaped ownership behavior |
| `datamodel_demo` | DataModel bring-up, services, world instances, event job, close |
| `luau_embed_demo` | Optional Luau compile, load, and resume boundary |

Build and run the default examples:

```sh
make
./build/reflection_demo
./build/jobqueue_demo
./build/sso_string_demo
./build/datamodel_demo
```

The optional Luau example needs Luau headers and libraries. It stays out
of the default build because Luau is an external dependency:

```sh
make LUAU_INCLUDE=/path/to/luau/VM/include
```

## Using these notes

This is a study of one build. Treat the following as unstable across
player versions:

- native addresses;
- inferred object offsets;
- vtable positions;
- flag-gated service lists;
- scheduler queue layout;
- replication and RakNet integration details.

Behavior listings are illustrations. Validate ownership, threading, and
error behavior independently before using them in production code.

## License and boundaries

The C++ examples and notes in this repository are original material. No
proprietary source is reproduced. Roblox, Roblox Player, Luau, and related
names belong to their respective owners. Use these notes for learning,
compatibility research, and independent implementation work.
