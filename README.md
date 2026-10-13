# Roblox Mac Player Internals

Architecture notes + illustrative C++ reconstructions for the
Roblox macOS player (`RobloxPlayer.app`, `0.742.0.7421053`,
`MacPlayer-version-f9247f9560044102`, x86_64 Mach-O, 120 MB).

Three subsystems, traced from the binary outward:

| # | Subsystem | Doc |
|---|-----------|-----|
| 1 | Scripting + Luau execution | [docs/02-scripting-luau.md](docs/02-scripting-luau.md) |
| 2 | DataModel (root, services, lifecycle) | [docs/01-datamodel.md](docs/01-datamodel.md) |
| 3 | Networking + replication | [docs/03-replication-net.md](docs/03-replication-net.md) |

Supporting docs: [job system](docs/04-jobscheduler.md), [ObjC bridge](docs/05-objc-bridge.md),
[loader/layout](docs/00-overview.md), [evidence ledger](docs/06-evidence-ledger.md)
(every claim mapped to a binary address, log tag, or header).

## Sources

* The player binary itself: version, SHA-256, and every address in these docs
  (`ram:` = file offset + `0x100000000`).
* Luau OSS (`github.com/luau-lang/luau`) for the VM-side reference table.
* Roblox public API documentation for service membership, cited where used.

## C++ reconstructions

`include/rbx/` + `src/` are **original illustrative reconstructions** of the
observed shapes (reflection descriptors, job queue, SSO strings, Luau embedding
pattern). They are not copied engine code.

```sh
make            # builds src/* demos with g++ -std=c++17
./build/reflection_demo
./build/jobqueue_demo
./build/sso_string_demo
```

`src/luau_embed_demo.cpp` additionally needs Luau OSS headers
(`LUAAU_INCLUDE`); it is excluded from the default build.

## Ground rules used throughout

* Observation vs inference vs unknown are kept separate.
* Every address is `ram:` = file offset + `0x100000000` (image base).
* No proprietary code is reproduced; behavioral findings are paraphrased.

Target SHA-256 (`Contents/MacOS/RobloxPlayer`):
`ac9ad59b3aa3805bf2d1c89fb6ff9c7941bf2e57af17a81cbc26abe94a022c47`
