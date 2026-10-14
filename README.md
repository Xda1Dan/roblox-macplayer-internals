# Roblox macOS Player Internals

Architecture notes and original C++ reconstructions for the Roblox macOS
player binary examined in this repository.

The documentation focuses on the boundaries that matter when rebuilding a
small compatible engine architecture:

```text
platform/bootstrap
        │
        ▼
DataModel ── services ── ScriptContext ── Luau VMs
    │              │             │
    │              │             └─ waiting/resume jobs
    │              └─ reflection/events
    │
    └─ NetworkClient/RakNet → packet queues → deserializer → replicator

all asynchronous work enters the TaskScheduler/job system
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

Addresses in the documentation use `ram:` addresses. For this build, a file
offset is converted to a virtual address by adding `0x100000000`.

## Read the documentation in this order

1. **[Overview and binary layout](docs/00-overview.md)** — target facts,
   entry points, Objective-C boundary, and address conventions.
2. **[Script execution, end to end](docs/07-script-execution.md)** — start here
   for scripting: admission → start → resume → wait → require → teardown,
   each with what-it-does, step-by-step, pseudocode, and assembly.
3. **[TaskScheduler internals](docs/08-scheduler-internals.md)** — enqueue
   assembly walkthrough, ring buffer, virtual step/stop, node pool.
4. **[Luau VM lifecycle](docs/09-luau-vm-lifecycle.md)** — `lua_newstate`,
   VM wrapper, protected calls, split-pointer hardening.
5. **[Replication deserialize path](docs/10-replication-deserialize.md)** —
   queue drain loop, temp records, time budgets, handoff to the replicator.
5. **[DataModel](docs/01-datamodel.md)** — the per-session root, services,
   lifetime states, reflection, and teardown ordering.
6. **[Scripting and Luau reference](docs/02-scripting-luau.md)** — full
   facet/method catalog, policy vocabulary, module states, teardown order.
7. **[Networking and replication](docs/03-replication-net.md)** — RakNet,
   packet ownership, deserialize jobs, typed replication items, and DataModel
   application.
8. **[TaskScheduler reference](docs/04-jobscheduler.md)** — job submission,
   ring queues, wakeups, frame jobs, and shutdown.
9. **[Objective-C bridge](docs/05-objc-bridge.md)** — application callbacks,
   web messages, and native-to-engine event delivery.
10. **[Evidence ledger and limits](docs/06-evidence-ledger.md)** — addresses,
   confidence boundaries, and unresolved details.

### How to read a deep-dive doc

Each of `07`, `08`, `09`, `10` follows the same shape:

```text
What it does (plain English, 3-5 lines)
  → Function card (address, size, logs, confidence)
  → Step by step (numbered, in execution order)
  → Assembly excerpt (short, annotated, factual)
  → Simplified reconstruction (original C++, not engine source)
  → Patterns / pitfalls
  → Unknowns
```

Short assembly excerpts are factual instruction bytes; longer
reconstructions are original illustrations. Full decompiler dumps are not
reproduced.

## The most important findings

### DataModel is a lifetime boundary

The DataModel is a per-session root and service provider, not merely a global
table of services. Join creates it; network and script work retain or weakly
reference it; close drains dependent work before freeing script state. The
close path at `ram:0x104542918` clears service containers individually,
including StarterPack, StarterPlayer, TextChatService, StarterGui, CoreGui,
Teams, Lighting, ReplicatedStorage, ReplicatedFirst, ServerStorage,
ServerScriptService, ChatService, and others.

### Script execution is staged native work

The native path is approximately:

```text
Script instance
  → context/run-policy gate
  → source or compilation-cache handling
  → execution-record construction
  → Luau VM/thread selection
  → start/resume
  → yield, completion, or error
  → waiting-script queue
  → later scheduler resume
```

The inspected start path at `ram:0x10158ac46` has a three-level
`startScript` re-entrancy limit, a `Script Start` profiling scope, VM/thread
handoff, and shared ownership across callbacks. The waiting-script bodies at
`ram:0x1015a8d94` and `ram:0x1015a9249` claim an atomic resumption flag, drain
a bounded number of waiters, observe a deadline, and release each waiter.

The native admission predicate at `ram:0x10142a7a6` separates **RunContext**,
**security identity**, and **capabilities**. Server scripts require the native
`RunServerScript` capability; client scripts require `RunClientScript`. The
script's service/ancestry, DataModel state, and special plugin/core branches
are checked as well. Lua does not assign itself a higher execution level.

On the Luau side, the supplied map was validated against exact function
starts. `lua_newstate` (`0x299fd5b`), the protected load boundary
(`0x29a9f36`), `ScriptContextResume` (`0x15566d6`), and `task.wait`
(`0x14cf74a`) all match real entry points. `task.wait` marks the coroutine
waiting and schedules a timed resume; it does not block an OS thread.
`ScriptContextResume` checks pending-script state, shutdown deadlines, and a
C-stack limit before resuming. The header's `GetGlobalState` label at
`0x55d5e74` is more accurately a ScriptContext facet accessor in this build.

The state-creation caller is `ram:0x1015b92cd`: allocator/context →
`lua_newstate` → ScriptContext VM wrapper → panic/interrupt hooks → library and
Roblox binding registration → `_G`/sandbox configuration → VM collection.
The state is created once per VM collection, not once per script; scripts use
execution threads from that collection.

The `Players` service is a real reflected service rather than just a vector of
players. Its registration declares `PlayerAdded`, `PlayerRemoving`, connecting,
disconnecting, rejoining, membership, and chat events, plus properties such as
`LocalPlayer`, `MaxPlayers`, `CharacterAutoLoads`, `RespawnTime`, and
`NumPlayers`. Player parentage under `game.Players` is checked by APIs such as
`InvokeClient` and `Player:Kick`, so service membership is part of the engine's
security/replication model.

Player join/removal crosses the network and DataModel boundaries: replicated
player bookkeeping resolves membership under `game.Players`, then lifecycle
events are delivered to service consumers; removal performs the inverse before
dependent references are released.

The script path is now summarized as:

```text
admission → source/bytecode lookup → compile/cache → auth/load
→ ScriptContext execution record → VM thread → Luau execute
→ completion/error/yield → TaskScheduler → ScriptContextResume
```

`task.wait` and async engine calls yield resumable VM state; they do not sleep
the worker thread. Native `loadstring` is also policy-gated and can be
unavailable in RobloxScript contexts.

The public `lua_pcall` name is absent from the binary, but protected execution
is not absent: `luaB_pcall` (`0x10298b913`), `luaB_pcallrun`
(`0x10298b946`), `luaD_rawrunprotected` (`0x102995084`), `luaD_throw`, and
`luaG_runerror` are all present as exact Luau function starts.

Selected Luau error paths also reconstruct strings through a split pointer.
At `ram:0x102988254`, a dummy pointer in `__unwind_info` plus the signed delta
`-0x2f9400` reconstructs `__cstring` address `0x1069a3378` (`"stack limit"`)
before the `luaG_runerror`-shaped call. This is selective pointer
transformation, not a universal compiler rule; direct RIP-relative string
references remain elsewhere.

ModuleScript `require` is stateful as well. The native path at
`ram:0x10158dd88` checks security context, tracks per-VM module state, detects
circular dependencies, rejects destroyed modules, and requires exactly one
returned value.

Async callbacks first resolve the implementing script through a weak reference.
The native callback path at `ram:0x101557e8c` rejects destroyed scripts before
creating callback execution state. Coroutine resumes are admitted only for
suspended/awaiting states; `ram:0x1015977c1` rejects other states.

### RakNet is transport, not the replication model

The player contains RakNet types such as `RakPeer`, `RakPeerInterface`,
`RakNetSocket`, `BitStream`, and `CCRakNetSlidingWindow`. The observed flow is:

```text
RakNet::RecvFromLoop
  → packet-return queue
  → DeserializePacketsJob / buffered deserializer
  → Replicator receive path
  → typed Deserialized*Item
  → DataModel mutation
```

The buffered deserializer at `ram:0x10564dd3c` drains queued records, updates
receive timing and queue state, constructs typed temporary records, and hands
them to replication-side processing. The exact wire schema is intentionally
not claimed here.

### The scheduler is the common crossing point

Objective-C callbacks, network receives, Lua callbacks, frame work, and
maintenance all become scheduled work. The observed submission chain is:

```text
0x10521311e → 0x104f54dc6 → 0x104f54c96 → 0x104f54876
                                      └→ 0x104545ba0
                                      └→ 0x102ee57de wakeup
```

`0x104545ba0` grows and copies a circular 0x80-byte record buffer; it is not
an open-addressing hash table.

Job execution is polymorphic: `ram:0x102eec6da` marks a job active and calls
its virtual step function. `ram:0x102ee3136` allocates bounded job nodes and
publishes them atomically; node exhaustion is a fatal scheduler diagnostic.
The matching stop path clears the active state, calls the virtual stop hook,
records elapsed time, updates the arbiter, and emits job timing diagnostics.

ScriptContext teardown drains waiting threads and yield waiters before
destroying scheduler members; VM collection then releases per-slot state and
poison-fills the old object storage. Work must not retain raw VM or
DataModel/service pointers across this boundary.

## Original C++ examples

The `include/rbx/` and `src/` files are original, deliberately simplified
reconstructions. They are not Roblox source and are not wire-compatible with
the player.

| Example | Demonstrates |
|---|---|
| `reflection_demo` | Descriptor registration, property/method dispatch, events |
| `jobqueue_demo` | Mutex-protected queues, worker wakeup, shutdown |
| `sso_string_demo` | Small-string optimization-shaped ownership behavior |
| `datamodel_demo` | DataModel bring-up, services, world instances, event job, close |
| `luau_embed_demo` | Optional Luau compile/load/resume boundary |

Build and run the default examples:

```sh
make
./build/reflection_demo
./build/jobqueue_demo
./build/sso_string_demo
./build/datamodel_demo
```

The optional Luau example requires Luau OSS headers and libraries. It is kept
outside the default build because Luau is an external dependency:

```sh
make LUAU_INCLUDE=/path/to/luau/VM/include
```

## How to use these notes responsibly

This is a build-specific architectural study. Treat the following as
unstable across player versions:

- native addresses;
- inferred object offsets;
- vtable positions;
- flag-gated service lists;
- scheduler queue layout;
- replication and RakNet integration details.

The documents distinguish observed behavior, derived structure, illustrative
inference, and unresolved questions. Decompiled pseudocode is not source and
should not be copied into a production implementation without independently
validating its ownership, threading, and error behavior.

## License and boundaries

The C++ examples and documentation in this repository are original material.
No proprietary Roblox source is reproduced. Roblox, Roblox Player, Luau, and
related names remain the property of their respective owners. Use these notes
for learning, compatibility research, and independent implementation work.
