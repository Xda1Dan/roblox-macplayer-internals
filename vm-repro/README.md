# rbxvm — reproduction of RobloxPlayer (macOS) internals

C++ reproduction of the formats and state machines documented in
[`../research/vm-research/`](../research/vm-research/) for the macOS player
build `MacPlayer-version-f9247f9560044102` (Luau `0.742.0.7421053`).
Everything here is derived from first-hand binary analysis (Ghidra + raw
Mach-O bytes) and the reference Luau tree (`luau-oss` 0.742); no proprietary
source is reproduced.

## What is implemented

| Piece | Status | Notes |
|---|---|---|
| Opcode triple-numbering + `T`/`R` tables | **done, tested** | bytes dumped from the binary; permutations + 93/93 round-trip |
| Instruction accessors + `getOpLength` | **done, tested** | AUX set matches the binary's `0x1029abbe3` |
| Jump arithmetic incl. AUX-skip | **done, tested** | vs `lvmexecute.cpp` |
| Disassembler (wire/vmop/canon, 93 opcodes) | **done, tested** | operand semantics aligned with `BytecodeBuilder::dumpInstruction` |
| Address-relative pointer encodings (VMValue1..4) | **done, tested** | sentinel property |
| Verified struct offsets (`lua_State`, `CallInfo`, `Proto`, `Closure`, `TString`, `LuaTable`) | **done** | constants + static asserts |
| RSB1 container: scramble, key recovery, xxHash32, zstd | **done, tested** | checksum executed from the binary's own machine code |
| ScriptContext guard/admission/require models | **done, tested** | guard is exact; admission polarity corrected; `require` is a behavioral model |
| RakNet AEAD (ChaCha20-Poly1305, custom framing) | **done, tested** | RFC 8439 primitive vectors + independent KAT; suffix is not consumed |
| LBC reader + constant-aware disassembly | **done, tested** | full compile→wire→container→decode→disassemble pipeline |
| **SigGuard** bytecode auth (mode chain, SHA-256, table MAC) | **done, tested** | packed tables extracted from the binary; MAC cross-checked against an independent Python model |
| Binary audit tool | **done** | re-checks tables/handlers/blob/checksum/gate call site against a real Mach-O |
| `luau_load`-equivalent loader (Proto/Closure build) | **done, tested** (`src/loader.cpp` + `tests/test_loader.cpp`) — version gates, per-proto wire→vmop rewrite, injectable atom pass |
| Interpreter core | planned | start with Path B |

## Layout

```
include/rbxvm/opcode.hpp     canonical enum, names, getOpLength
include/rbxvm/scramble.hpp   T/R tables + wire/vmop/canonical transforms
include/rbxvm/insn.hpp       32-bit instruction accessors/encoders
include/rbxvm/ptrenc.hpp     four address-relative pointer encodings
include/rbxvm/layouts.hpp    verified struct offsets
include/rbxvm/disasm.hpp     disassembler API
include/rbxvm/rsb1.hpp       RSB1 container pipeline
include/rbxvm/zstd_loader.hpp dlopen'd libzstd access
include/rbxvm/scriptctx.hpp  ScriptContext decision logic
include/rbxvm/aead.hpp       RakNet AEAD construction
include/rbxvm/lbc.hpp        LBC serialized-chunk reader
include/rbxvm/sigguard.hpp   SigGuard verifier + table expansion
src/tables.cpp               binary-dumped T/R + opcode metadata
src/disasm.cpp               disassembler
src/rsb1.cpp                 RSB1 key/scramble/checksum/decode
src/scriptctx.cpp            guard/admission/require models
src/aead.cpp                 ChaCha20/Poly1305 + framing
src/lbc.cpp                  chunk parser + header-aware op conversion
src/sigguard.cpp             SHA-256, table expansion, MAC, verdicts
src/zstd_loader.cpp          runtime libzstd binding
tools/rbxdis.cpp             CLI: tables / verify / code / chunk
tools/rbxrsb1.cpp            CLI: container info/decode/encode
tools/rbxaudit.cpp           CLI: verify constants against a RobloxPlayer Mach-O
tests/                       6 suites + fixtures + ground-truth machine code
```

## Build, test, audit

```sh
make                 # tools + tests
make test            # all six suites
make demo            # wire-encoded disassembly sample
./tools/rbxdis tables
./tools/rbxdis verify
./tools/rbxdis code --openc=wire --hex="..."
./tools/rbxdis chunk tests/fixtures/rich.lbc

./tools/rbxrsb1 info FILE
./tools/rbxrsb1 decode FILE -o bytecode.lbc
./tools/rbxrsb1 encode --zstd bytecode.bin -o container.bin

# Re-check the load-bearing constants against the target binary:
make audit BINARY=/path/to/RobloxPlayer
```

`make audit` verifies: the binary SHA-256, the `T`/`R` bytes, both dispatch
tables (93 handlers each, matching `tests/handler_addresses.inc`), the
SigGuard blob hash/header, the checksum machine-code hash, and the
`luau_load → SigGuard` call site.

## Fixtures and provenance

- `tests/fixtures/rich.lbc` — compiled by the reference `luau-compile`
  (Luau 0.742), 4 protos; used for the LBC/pipeline tests.
- `tests/fixtures/sigguard_tables.bin` — the binary's `0x1830C` packed
  table blob at VA `0x106207760` (8257 × 12-byte records, header
  `GYM\x14`). SHA-256 `0b6cdf37…f42c3c`.
- `tests/ground_truth.inc` — the actual 318-byte machine code of the
  container checksum (`FUN_103222533`), executed by the RSB1 test.
- `tests/handler_addresses.inc` — Path A/B handler addresses generated
  from `research/vm-research/opcode-map.csv`.

## The RSB1 container (reproduced)

```
Path A: W[0..7] == 0xE009325B4A107A52  -> payload = W[8..N) plain LBC
Path B: key[i] = (W[i] ^ "RSB1"[i]) - i*0x29
        plain[p] = W[p] ^ (key[p&3] + p*0x29)
        verify XXH32(plain, seed 0x2A) == key (LE u32)
        plain[0..3] = "RSB1"; plain[4..7] = u32 decompressed size
        size == 0 or zstd magic 0xFD2FB528 -> payload
```

The key is the checksum of the plaintext, and the first four plaintext bytes
are the known constant `"RSB1"`, so the key is recoverable from the wire
bytes. This is obfuscation/integrity, not a secret-key cipher.

## SigGuard (reproduced)

The gate runs in `luau_load` on the decoded LBC blob. The reproduction
covers the live mode-1 path:

- trailer mode detection (`C4 32 6A` + version, secondary magic
  `0x946AC432`);
- checkword (`trailer[0] ^ trailer[4]` for mode 1, must equal
  `0x74171870`);
- SHA-256 of the payload, compared with the 12-byte table MAC over the
  16-byte selector;
- verdict mapping: codes 0/1/2 accept, 3..9 reject; bit 30 clear for
  3,4,6,7,9.

Mode 0 is always rejected by the threshold check in this build; the mode-0
verdict function is therefore documented but not on the live path. A valid
mode-1 trailer cannot be forged in a unit test without solving the table
MAC, so the accept path is tested with `verify_mac=false` and the MAC path
is tested through its rejection verdict. The MAC itself is cross-checked
against an independent Python transcription for fixed selectors.

## Known limitations

- The interpreter is not implemented; loader IMPORT constants are kept as
  ids (resolution needs the embedder's environment).
- The interpreter is not implemented.
- The AEAD key-derivation chain (X25519 → session keys) is not traced; the
  reproduction takes the 32-byte key as input.
- Reliable-frame/ACK/NAK wire formats are not reproduced.
- ScriptContext admission/require models are decision-logic reproductions,
  not full object-graph simulations; see the headers for the exact
  abstractions.
- Offsets/addresses are for this build only.
