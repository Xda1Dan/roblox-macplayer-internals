# rbxvm — C++ reproduction of RobloxPlayer (macOS) Luau VM internals

Reproduction of verified internals of the macOS Roblox player binary
`MacPlayer-version-f9247f9560044102` (Luau `0.742.0.7421053`), as researched in
[`../research/vm-research/luau-vm.md`](../research/vm-research/luau-vm.md). Everything in
this repo is derived from first-hand binary analysis (Ghidra) and the reference
Luau tree (`../luau-oss`), **not** from any proprietary source.

## What is implemented today

| Piece | Status |
|---|---|
| Opcode triple-numbering (canonical / wire / vmop) + tables `T`, `R` | **done, tested** — data dumped from the binary |
| Instruction word accessors + encoders (ABC / AD / E, AUX layouts) | **done, tested** |
| Jump-target arithmetic incl. AUX-skip semantics | **done, tested** (verified vs `lvmexecute.cpp`) |
| Disassembler for wire/vmop/canonical code arrays, all 93 opcodes | **done, tested** |
| Address-relative pointer encodings (VMValue1..4) encode/decode | **done, tested** (sentinel property) |
| Verified struct offset tables (`lua_State`, `CallInfo`, `Proto`, `Closure`, `TString`, `Table`) | **done** (offset constants + static asserts) |
| RSB1 script-container pipeline: scramble, key recovery, xxHash32 integrity, zstd | **done, tested** — checksum verified against the binary's own machine code |
| ScriptContext layer: resume guard, result mapping, facet gates, startScript admission, require state machine | **done, tested** (see `../research/vm-research/scriptcontext.md`) |
| RakNet datagram AEAD: ChaCha20-Poly1305 with the custom framing, nonce builders, sequence-window reconstruction | **done, tested** — RFC 8439 primitive vectors + differential vs a bignum spec reference (see `../research/vm-research/networking.md`) |
| LBC serialized-chunk reader: strings, constants, protos, line/debug info + constant-aware disassembly + wire-op conversion | **done, tested** — validated against the reference loader (`luau_load`); full pipeline: source -> compile -> wire ops -> RSB1 container -> decode -> identical disassembly |
| `luau_load`-equivalent loader (wire→vmop rewrite, Proto/Closure build) | next |
| Interpreter core | planned |

## Layout

```
include/rbxvm/opcode.hpp    canonical opcode enum, names, getOpLength
include/rbxvm/scramble.hpp  T/R tables + wire/vmop/canonical transforms
include/rbxvm/insn.hpp      32-bit instruction word accessors/encoders
include/rbxvm/ptrenc.hpp    the four address-relative pointer encodings
include/rbxvm/layouts.hpp   verified struct offset tables
include/rbxvm/disasm.hpp    disassembler API
include/rbxvm/rsb1.hpp      script-container pipeline (scramble/decode)
include/rbxvm/zstd_loader.hpp  dlopen'd libzstd access
include/rbxvm/scriptctx.hpp ScriptContext state machines (guard/admission/require)
include/rbxvm/aead.hpp     RakNet datagram AEAD (ChaCha20-Poly1305, custom framing)
include/rbxvm/lbc.hpp      LBC serialized-chunk reader (strings/constants/protos)
src/tables.cpp              binary-dumped T (VA 0x1063bb686) and R (VA 0x10648bb74)
src/disasm.cpp              93-opcode disassembler
src/rsb1.cpp                RSB1 key/scramble/checksum/decode
src/zstd_loader.cpp         runtime libzstd binding
src/scriptctx.cpp           ScriptContext guard/admission/require models
src/aead.cpp                ChaCha20/Poly1305 + the RakNet AEAD construction
src/lbc.cpp                 chunk parser + constant-aware dumper
tools/rbxdis.cpp            CLI: tables / verify / code / chunk
tools/rbxrsb1.cpp           CLI: info / key / decode / encode (script containers)
tests/test_core.cpp         verification vs vm-research ground truth
tests/test_rsb1.cpp         RSB1 verification incl. executing the binary's own checksum code
tests/test_scriptctx.cpp    ScriptContext decision-logic verification
tests/test_aead.cpp         AEAD verification (RFC vectors + differential + wire formats)
tests/test_lbc.cpp          chunk reader + full pipeline (compile->wire->container->decode->disasm)
tests/fixtures/rich.lbc     compiled fixture (closures/tables/loops, from the reference compiler)
tests/opmap_expected.inc    per-opcode fixture (generated from vm-research/opcode-map.csv)
tests/ground_truth.inc      binary fixtures: FUN_103222533 machine code + a real zstd frame
```

## Build and run

```sh
make
make test        # verification suite (core + RSB1)
make demo        # disassemble a wire-encoded sample program
./tools/rbxdis tables        # opcode map + T/R dumps
./tools/rbxdis verify        # table self-checks
./tools/rbxdis code --openc=wire --hex="..."   # disassemble raw words

./tools/rbxrsb1 info FILE     # inspect a script container
./tools/rbxrsb1 decode FILE -o bytecode.lbc     # container -> Luau bytecode
./tools/rbxrsb1 encode --zstd bytecode.bin -o container.bin     # inverse
```

## The RSB1 script-container format (reproduced in `src/rsb1.cpp`)

Wire bytes `W`, length `N` (all first-hand verified; see the header comment in
`include/rbxvm/rsb1.hpp` for every code address):

```
Path A: W[0..7] == 0xE009325B4A107A52      -> payload = W[8..N) plain bytecode
Path B: key[i]  = (W[i] ^ "RSB1"[i]) - i*0x29          (mod 256)
        plain[p] = W[p] ^ (uint8)(key[p&3] + p*0x29)   (whole buffer, in place)
        verify: XXH32(plain, seed 0x2A) == key (LE u32)   else reject
        plain[0..3] = "RSB1"
        plain[4..7] = u32 LE decompressed size
          size == 0                  -> payload = plain[8..N) plain bytecode
          u32 at plain[8..11] == 0xFD2FB528 (zstd) -> payload = zstd(plain[8..N))
```

The security trick: **the scramble key IS the xxHash32 checksum of the
plaintext** (seed `0x2A`). The encoder hashes the plain container, scrambles
with that value, and the key is recoverable from the scrambled bytes alone
because the first four plaintext bytes are the known constant `"RSB1"` — the
derivation is a tautology that exactly reconstructs the embedded key. There is
no secret and nothing to brute-force; the checksum then authenticates the
whole payload.

The "plain bytecode magic" is the K=0 scramble of `"RSB1"+0xFFFFFFFF` —
byte-verified. `FUN_1014bb465` (the "integrity gate") is NOT part of this
pipeline: it is called from inside `luau_load` (`0x1029aa05a`) on the already-
decoded LBC blob.

## Key facts encoded here (with provenance)

- `wire = (227 * canonical) & 0xFF`; `vmop = T[wire]`; `canonical = R[vmop]`.
  `T` at VA `0x1063bb686`, `R` at VA `0x10648bb74` (the community dump's
  `OpcodeLookupTable = 0x63bb680` is off by 6 and is this `T`, not the
  dispatch table).
- The interpreter dispatch jump tables (`0x1072d8a30` Path A single-step,
  `0x1072d9230` Path B normal) are *different* from `T`/`R`.
- 31 of 93 opcodes carry an AUX word (`getOpLength`), identical to upstream.
- Jump targets: `target = i + 1 + offset` — for two-word jumps this skips the
  AUX word (VM reads AUX without advancing `pc`; not-taken advances one more).
- Pointer encodings are relative to the field's own address; the four schemes
  and their fields are documented in `include/rbxvm/ptrenc.hpp`. Hot-path
  pointers (`Proto.k/.code/.p`, `Closure.p/f`) are **plain**.
- `GETIMPORT` AUX: `len(1..3) << 30 | comp0 << 20 | comp1 << 10 | comp2`,
  each component a 10-bit constant index.

## Next steps

1. Serialized-chunk reader (LBC format, versions 3..14/100) — unblocks
   disassembling real game bytecode once the RSB1 container decoder
   (`FUN_1014b91b5`) is reproduced.
2. Loader equivalent of `luau_load` (in-place wire→vmop rewrite + Proto build).
3. Interpreter core (start with Path B semantics).
