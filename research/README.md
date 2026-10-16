# RobloxPlayer (macOS) — Reverse Engineering Project

> Working notes for this reverse-engineering effort. They live in this repository but are kept
> out of the published site build; the curated write-ups are in [`docs/`](../docs/index.md).

Reverse engineering of the macOS Roblox player binary, focused on how the engine executes
scripts and manages its internals: the Luau VM, ScriptContext, TaskScheduler, DataModel,
RunService, and the RbxTransport networking layer.

## Target

| | |
|---|---|
| **Binary** | `MacPlayer-version-f9247f9560044102/RobloxPlayer.app/Contents/MacOS/RobloxPlayer` |
| **Format** | Mach-O (x86-64), Objective-C++ / C++ (libc++), stripped |
| **Image base** | `0x100000000` (all VAs in these docs are absolute; subtract the base for a file offset) |
| **Size** | ~127 MB mapped (`133,177,600` bytes across 43 memory blocks) |
| **Luau** | `0.742.0.7421053` |
| **Analysis tool** | Ghidra (headless MCP), project `tmp-rea-ghidra/ghidra-proj`, program `RobloxPlayer` |
| **Functions identified** | ~549,782 (auto-analysis; the deep dives below cover a targeted subset) |

## Documents

| File | Contents |
|------|----------|
| **`vm-research/luau-vm.md`** | **The Luau VM deep dive** — opcode encoding, pointer-encoding scheme, interpreter internals, verified object layouts, and the full script-execution address map. Start here for anything VM-related. |
| **`vm-research/scriptcontext.md`** | **The ScriptContext layer** — resume pipeline and guard (200-limit, shutdown deadline, time accounting), result-code mapping, facet gates, `startScript` admission chain, the compilation-cache load step, and the `require` module state machine. |
| **`vm-research/networking.md`** | **RakNet datagram crypto & framing** — ChaCha20-Poly1305 (custom framing: 8-byte-nonce payload state, GCM-style length blocks in the MAC), the "UniqueNumber" nonce, asymmetric trailer formats per direction, directional keys, the Rupp handshake (RbxOpenRequest2/Reply2), DHF partial layout, and the open wire-format targets. |
| **[`vm-repro/`](../vm-repro/)** | **C++ reproduction** of the verified internals — opcode tables, disassembler, RSB1 container pipeline, ScriptContext state machines, RakNet AEAD, LBC chunk reader, each with tests. See the [tooling page](../docs/repro-tooling.md). |
| **`vm-research/opcode-map.csv`** | All 93 opcodes, verified: canonical / wire / vmop numbers, aux flag, and both interpreter handler addresses. |
| **`vm-research/dump-crosscheck.txt`** | Per-symbol verification status of the community (`Pibbly`) offset dump. |
| **`RobloxPlayer_RE_Report.md`** | The original broad reverse-engineering report (12 sections). |
| **`offsets.md`** | Memory-offset reference (RVA + VA), with community-dump corrections. |
| **`structures.md`** | ScriptContext / TaskScheduler / DataModel structure layouts. |
| **[`docs/`](../docs/index.md)** | Curated subsystem write-ups (script execution, scheduler, networking, …). |

## TL;DR — execution architecture

```
Mach-O entry (0x100041fe2)
  └─ NSApplicationMain
      └─ RobloxPlayerAppDelegate.applicationDidFinishLaunching
          └─ "Initializing lua app" → "Bind to services" → game launch
              └─ DataModel created (services bound)
                  └─ RunService frame loop
                      └─ ScriptContextResume          0x1015566d6
                          └─ C-stack / deadline guard 0x101556cb4   (limit 200)
                              └─ lua_resume           0x102995af5
                                  └─ luaD_rawrunprotected 0x102995084
                                      └─ luau_execute 0x1029adfd3   ← bytecode interpreter
                                          └─ Path B (normal)  0x1029b359c  — table 0x1072d9230
                                          └─ Path A (debug)   0x1029adfe9  — table 0x1072d8a30
```

Scripts reach the VM through a longer pipeline — asset decode → compilation cache →
`LuaVMLoad`/`luau_load` → scheduler enqueue → resume. The complete annotated path is in
`vm-research/luau-vm.md`.

## What this build does differently (verified)

Two obfuscations distinguish this Luau from upstream, and both are fully reverse-engineered in
`vm-research/luau-vm.md`:

1. **Opcodes are double-scrambled.** Each instruction exists as three different byte values —
   `canonical` (the real ID, `0..92`), `wire` (`= (227 × canonical) & 0xFF`, what ships on
   disk), and `vmop` (`= T[wire]`, what the interpreter dispatches on, `T` at `0x1063bb686`).
   A reverse map back to canonical lives at `0x10648bb74`.

2. **Some object fields are pointer-encoded.** Debug/source metadata in `Proto`, plus
   `TString.hash`, userdata metatables and `lua_State.stacksize`, are stored scrambled relative
   to **their own field address** (four variants: add / subtract / reverse-subtract / XOR).
   There is no secret key — but reading these fields without decoding returns garbage. The
   hot-path pointers (`Proto.code`, `Proto.k`, `Proto.p`, `Closure.p`) are **not** encoded.

## Corrections to the community offset dump

The community (`Pibbly`) offsets header is a **merge from multiple builds**, so most entries
resolve but a few point at the wrong thing. Confirmed errors for *this* binary:

| Symbol | Dump value | Correct |
|--------|-----------|---------|
| `OpcodeLookupTable` | `0x63bb680` | `0x63bb686` (off by 6 — it's the wire→vmop scramble table `T`) |
| `task.spawn` | `0x14cff67` | `0x14cf416` |
| `task.delay` | `0x14cf880` | `0x14cf56a` (dump reused the `task.cancel` address) |
| `lua_rawcheckstack` | `0x2995b7f` | mislabeled — that address is the resume / nCcalls guard |

Note the dump's `OpcodeLookupTable` and the interpreter **dispatch** jump table are two
different tables. The scramble table `T` is `0x1063bb686`; the Path A/B dispatch tables are
`0x1072d8a30` / `0x1072d9230`. Earlier notes conflated these.

The `Input.Fire*`, `RakNet.*`, `GetGlobalState` and `PushInstance` entries previously marked
"out of range" **do** resolve to valid functions in this build; the real issue with the dump is
the mislabels above, not out-of-range addresses.

## Methodology

1. String cross-references (`[FLog::*]` channels) to locate subsystems.
2. Byte-pattern scans for name and dispatch tables.
3. On-demand decompilation of the functions Ghidra identified.
4. Cross-validation against the open-source Luau tree this build derives from, plus the
   community offset dump — every structural claim in `vm-research/` was confirmed first-hand in
   the disassembler rather than taken from the dump.
