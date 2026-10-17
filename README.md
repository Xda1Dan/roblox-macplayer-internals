# Roblox macOS Player — internals, verified

Reverse engineering of the macOS Roblox player build
`MacPlayer-version-f9247f9560044102` (Luau `0.742.0.7421053`), with a tested
C++ reproduction of the parts that were pinned down first-hand.

The project is deliberately split into two layers:

- **[`research/`](research/README.md)** — evidence-first notes. Every
  load-bearing claim cites a function/table address in the target binary.
  **This is the source of truth.**
- **[`vm-repro/`](vm-repro/README.md)** — the reproduction: opcode tables,
  disassembler, RSB1 containers, LBC reader, ScriptContext models, RakNet
  AEAD, SigGuard bytecode auth, plus a binary audit tool. `make test` runs
  eight suites (including a test that executes the binary's
  own ChaCha20 core).

The site pages in [`docs/`](docs/index.md) are narrative write-ups. Where
they disagree with `research/`, the research notes win.

## Target

| Property | Value |
|---|---|
| Application | `RobloxPlayer.app` |
| Build | `0.742.0.7421053` (`MacPlayer-version-f9247f9560044102`) |
| Format | Mach-O x86-64, stripped |
| Size | `125,285,200` bytes |
| SHA-256 | `ac9ad59b3aa3805bf2d1c89fb6ff9c7941bf2e57af17a81cbc26abe94a022c47` |
| Image base | `0x100000000` |

The binary itself is not in this repository.

## Start here

| Document | What it answers |
|---|---|
| [`research/vm-research/luau-vm.md`](research/vm-research/luau-vm.md) | How opcodes are numbered, how objects are laid out, where the interpreter lives |
| [`research/vm-research/luau-load.md`](research/vm-research/luau-load.md) | How bytecode becomes a closure; the SigGuard gate |
| [`research/vm-research/scriptcontext.md`](research/vm-research/scriptcontext.md) | Resume guard, shutdown/facet gates, `startScript`, `require` |
| [`research/vm-research/networking.md`](research/vm-research/networking.md) | RakNet datagram crypto/framing |
| [`research/vm-research/opcode-map.csv`](research/vm-research/opcode-map.csv) | All 93 opcodes with both handler addresses |
| [`research/offsets.md`](research/offsets.md) / [`structures.md`](research/structures.md) | Address and structure references |
| [`OPEN-ITEMS.md`](OPEN-ITEMS.md) | What is open vs resolved |
| [`vm-repro/README.md`](vm-repro/README.md) | What the reproduction covers and how to run it |

## What was verified (and how)

- **Opcodes:** `wire = 227*canonical mod 256`, `vmop = T[wire]`,
  `canonical = R[vmop]`; `T`/`R` dumped from the binary, permutations,
  93/93 round-trip. Both 256-entry dispatch tables have exactly 93 non-null
  entries and match `opcode-map.csv` byte-for-byte.
- **Layouts:** `lua_State`, `CallInfo`, `Proto`, `Closure`, `TString`,
  `LuaTable` offsets recovered from constructors/free paths/accessors and
  encoded as compile-time constants.
- **Containers:** RSB1 scramble/checksum/zstd; the checksum is executed from
  the binary's own machine code in the test suite.
- **Loader:** `LuaVMLoad`/`luau_load` version gates, wire→vmop rewrite, atom
  pass, and the SigGuard gate.
- **SigGuard:** packed tables extracted, A/B/C expansion reproduced, 12-byte
  table MAC cross-checked against an independent model, verdict mapping.
- **AEAD:** ChaCha20 + poly1305-donna-32 with the binary's custom framing;
  independent known-answer test.
- **ScriptContext guard:** 200-depth limit, shutdown deadline, facet phase,
  time accounting, result mapping.

## What is not claimed

- The X25519 → session-key KDF.
- Reliable-frame/ACK/NAK wire formats and full DHF field semantics.
- The `+0xa18` phase-counter writer and identity getter classes.
- `require`'s state *writes* during a run.
- Scheduler/DataModel/ObjC details in the narrative site pages were not
  re-verified in this pass.
- Cross-build stability: addresses and offsets move.

## Reproduce

```sh
cd vm-repro
make test
make audit BINARY=/path/to/RobloxPlayer
```

`make audit` checks the binary hash, `T`/`R`, both dispatch tables, the
SigGuard blob, the checksum machine code, and the `luau_load → SigGuard`
call site. The test suite is self-contained (fixtures are committed).

## Repository layout

```
research/            evidence-first notes, offsets, structures
  vm-research/       VM, loader/SigGuard, ScriptContext, networking
  archive/           early broad report (superseded)
vm-repro/            C++ reproduction + tests + CLI tools
docs/                narrative site pages (MkDocs)
include/, src/       small illustrative C++ examples (not player source)
mkdocs.yml           site configuration
```

## Boundaries

The C++ code and notes are original material. No proprietary source is
reproduced. Roblox, Roblox Player, Luau, and related names belong to their
respective owners. Use this for learning, compatibility research, and
independent implementation work.
