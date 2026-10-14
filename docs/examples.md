# C++ examples

The `include/rbx/` headers and `src/` programs are original, simplified
illustrations. They are **not** player source and are not wire-compatible
with the player. They exist to make the documented shapes (reflection
descriptors, job queues, SSO strings, DataModel bring-up, Luau embed
boundary) concrete and runnable.

## Build

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

## What each example shows

| Example | Shows |
|---|---|
| `reflection_demo` | Descriptor registration, property and method dispatch, events |
| `jobqueue_demo` | Mutex-protected queues, worker wakeup, shutdown |
| `sso_string_demo` | Small-string-shaped ownership behavior |
| `datamodel_demo` | DataModel bring-up, services, world instances, event job, close |
| `luau_embed_demo` | Optional Luau compile, load, and resume boundary |

## Source index

- `include/rbx/datamodel.hpp` — DataModel, services, worlds, instances
- `include/rbx/reflection.hpp` — descriptors, properties, methods, events
- `include/rbx/scheduler.hpp` — job queue and worker wakeup
- `include/rbx/script_context.hpp` — script execution contexts
- `include/rbx/luau_bridge.hpp` — Luau VM wrapper boundary
- `include/rbx/replicator.hpp` / `include/rbx/instance.hpp` — replication + instance shapes
- `src/datamodel_demo.cpp`, `src/jobqueue_demo.cpp`, `src/reflection_demo.cpp`
- `src/sso_string_demo.cpp`, `src/luau_embed_demo.cpp`, `src/descriptors.cpp`

See the repository on GitHub for the full source.
