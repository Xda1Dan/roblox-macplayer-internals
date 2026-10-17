# Research notes — RobloxPlayer (macOS) build f9247f9560044102

These are the **evidence-first** notes. Every load-bearing claim cites a
function address in the target binary; the reproduction in
[`../vm-repro/`](../vm-repro/) turns the formats and state machines into
tested code, and `rbxaudit` re-checks the constants against the actual
Mach-O.

The curated site pages in [`../docs/`](../docs/index.md) are narrative
write-ups. Where they disagree with `research/vm-research/`, the research
notes win.

## Target

| | |
|---|---|
| Binary | `MacPlayer-version-f9247f9560044102/RobloxPlayer.app/Contents/MacOS/RobloxPlayer` |
| Format | Mach-O x86-64, stripped |
| Size | `125,285,200` bytes |
| SHA-256 | `ac9ad59b3aa3805bf2d1c89fb6ff9c7941bf2e57af17a81cbc26abe94a022c47` |
| Image base | `0x100000000` |
| Luau | `0.742.0.7421053` |
| Reference tree | `luau-oss/` at tag `0.742` |

VAs are absolute (`0x100000000 + RVA`). For the `__TEXT` and `__DATA`
segments, `VA = file offset + 0x100000000`; `__LINKEDIT` does not follow that
rule.

## Documents

| File | Contents | Evidence |
|------|----------|----------|
| **`vm-research/luau-vm.md`** | Opcode triple-numbering, pointer encodings, interpreter paths, verified object layouts, address map | First-hand (Ghidra + binary bytes) |
| **`vm-research/luau-load.md`** | `LuaVMLoad` / `luau_load` end to end, version gates, wire→vmop rewrite, atom pass, **SigGuard** auth gate | First-hand |
| **`vm-research/scriptcontext.md`** | Resume guard, deadline/facet gates, `startScript` admission (polarity-corrected), `require` state machine | First-hand; some admission fields modeled |
| **`vm-research/networking.md`** | RakNet ChaCha20-Poly1305 framing, nonces, sequence window, Rupp handshake, DHF | First-hand; KDF open |
| **`vm-research/opcode-map.csv`** | 93 opcodes: canonical/wire/vmop, AUX flag, Path A/B handler addresses | Verified against both dispatch tables |
| **`vm-research/dump-crosscheck.txt`** | Per-symbol status of the community offset dump | First-hand |
| `offsets.md` | RVA/VA reference and dump corrections | First-hand, targeted |
| `structures.md` | ScriptContext/scheduler fields actually used by the reproduction | First-hand + marked narrative |
| [`../vm-repro/`](../vm-repro/) | C++ reproduction + tests + `rbxaudit` | Tests pass; audit checks the binary |
| [`archive/RobloxPlayer_RE_Report.md`](archive/RobloxPlayer_RE_Report.md) | Early broad report, kept for history | **Superseded — do not cite** |

## What is actually verified

- **Opcode numbering:** `wire = 227*canonical mod 256`, `vmop = T[wire]`,
  `canonical = R[vmop]`; `T` at `0x1063bb686`, `R` at `0x10648bb74`. Both
  tables are permutations; all 93 opcodes round-trip. `getOpLength`
  (`0x1029abbe3`) agrees with the 31 AUX opcodes.
- **Dispatch:** both 256-entry tables (`0x1072d8a30` Path A,
  `0x1072d9230` Path B) have exactly 93 non-null entries; every entry
  matches `opcode-map.csv`.
- **Layouts:** `lua_State`, `CallInfo`, `Proto`, `Closure`, `TString`,
  `LuaTable` offsets in `vm-repro/include/rbxvm/layouts.hpp`, recovered from
  `luaF_newproto`/`luaF_freeproto`, `lua_resetthread`, `luaS_newlstr`, and
  the interpreter entry.
- **Pointer encodings:** four self-address-relative schemes; hot-path
  pointers (`Proto.k/.code/.p`, `Closure.p/f`) are plain.
- **RSB1 containers:** scramble key derivation, xxHash32 checksum (executed
  from the binary's own machine code in tests), zstd payload, magic paths.
- **AEAD:** ChaCha20 + poly1305-donna-32 with the custom framing. The
  counter-0 poly key and payload keystream both read only nonce bytes 0..8;
  the 4-byte suffix is carried but not consumed.
- **SigGuard:** mode chain, SHA-256 payload digest, 8257-record packed
  table set, A/B/C expansion, 12-byte table MAC, verdict mapping. The
  reproduction includes the table blob as a fixture and cross-checks the MAC
  against an independent Python transcription.
- **ScriptContext guard:** 200-deep limit, shutdown deadline
  (`ctx+0x4b8`), facet phase (`<3` valid), time accounting, result mapping.
- **LBC:** strings/constants/protos/line/debug/feedback parsing, header-only
  opcode conversion (AUX words untouched), full compile→wire→container→
  decode→disassemble pipeline.

## What is not claimed

- The X25519 → session-key KDF (EVP dispatch; dynamic analysis needed).
- Reliable-frame/ACK/NAK wire formats and the full DHF field semantics.
- The `+0xa18` phase-counter writer and the identity getter classes.
- `require`'s state *writes* during a run (state reads are verified).
- The broad site pages' scheduler/DataModel/ObjC details are narrative;
  they were not re-verified in this pass.
- Anything about other builds. Offsets and addresses move.

## Method

1. String/xref anchors (`[FLog::*]`, registry keys) to find subsystems.
2. Byte-pattern scans for tables and constants.
3. On-demand Ghidra decompilation/disassembly of the named functions.
4. Cross-check against `luau-oss` 0.742 for semantics, and against the
   actual Mach-O bytes for every table/constant via `rbxaudit`.
5. Anything that could not be pinned down is listed as an open item, not
   presented as fact.
