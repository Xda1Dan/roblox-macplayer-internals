# The reproduction tooling (`vm-repro/`)

> What this is: the working C++ reproduction of everything this project has
> reverse engineered — not illustrations, but tested implementations of the
> real formats and state machines. Every piece ships with a test suite that
> verifies it against ground truth: bytes dumped from the player binary,
> official cipher test vectors, and the reference Luau loader.
>
> Everything here is built from first-hand analysis documented across the
> site; nothing is copied from proprietary source.

## What is implemented

| Layer | Status |
|---|---|
| Opcode triple-numbering (canonical / wire / vmop) + the `T`/`R` scramble tables | done, tested — tables dumped from the binary |
| Disassembler for all 93 opcodes, with constant resolution | done, tested |
| Struct layouts (`lua_State`, `CallInfo`, `Proto`, `Closure`, `TString`, `Table`) | done — offset tables with static asserts |
| RSB1 script-container pipeline (scramble, key recovery, XXH32 integrity, zstd) | done, tested — checksum verified against the binary's *own machine code*, executed in the test |
| ScriptContext state machines (resume guard, admission, `require`) | done, tested |
| RakNet datagram AEAD (ChaCha20-Poly1305, custom framing) | done, tested — RFC 8439 vectors + differential vs a bignum reference |
| LBC serialized-chunk reader + constant-aware disassembly | done, tested — validated against the reference `luau_load` |
| `luau_load`-equivalent loader, interpreter core | planned |

## The full delivery pipeline, end to end

This is the same journey a script takes from the compiler to the
interpreter, reproduced with the tooling:

```sh
# 1. compile Lua with the version-matched reference compiler
luau-compile --binary script.lua > script.lbc

# 2. scramble the opcode bytes (canonical -> wire) and wrap in an
#    RSB1 container — what the server ships
tools/rbxrsb1 encode --size0 --wire script.lbc -o script.container

# 3. decode on the "client" side
tools/rbxrsb1 decode script.container -o script.wire.lbc

# 4. parse the chunk and disassemble — the wire numbering resolves back
#    to identical canonical output
tools/rbxdis chunk --openc=wire script.wire.lbc
```

The test suite asserts that step 4's output is byte-identical to
disassembling the original canonical chunk.

## How things are verified

A few highlights of the test discipline in `tests/`:

- **The binary certifies the checksum.** The container-integrity checksum
  (`FUN_103222533`) is pure computation, so its 318 bytes of machine code
  were extracted from the player, mapped at runtime, and *executed* against
  our implementation across a 1000+-case sweep.
- **Official vectors for the ciphers.** The datagram AEAD decomposes into
  RFC 8439 test vectors plus a differential test against a spec-faithful
  bignum reference (the framing itself is Roblox-custom and documented in
  the research notes).
- **The reference loader as ground truth.** The chunk reader's output was
  validated by linking the reference `luau_load` and diffing the parsed
  constants — which caught three format subtleties (1-based string ids,
  proto size fields, import ids).

## Building

```sh
cd vm-repro
make test      # builds and runs the six test suites
make demo      # disassemble a wire-encoded sample program
```

Requires only a C++20 compiler, `make`, and (optionally) `libzstd` for the
compressed-container path — loaded at runtime, no headers needed.

## Layout

```text
include/rbxvm/   opcode.hpp, scramble.hpp, insn.hpp, ptrenc.hpp,
                 layouts.hpp, disasm.hpp, rsb1.hpp, zstd_loader.hpp,
                 scriptctx.hpp, aead.hpp, lbc.hpp
src/             implementations of the above
tools/           rbxdis (tables/verify/code/chunk), rbxrsb1 (containers)
tests/           six suites + fixtures (compiled Lua, binary ground truth)
```

Per-area provenance (which binary address every claim came from) lives in
the header comments of each `include/rbxvm/*.hpp` and in the research notes
under `research/vm-research/`.
