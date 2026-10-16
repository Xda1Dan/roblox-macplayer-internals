# Luau VM & script execution — RobloxPlayer (macOS, Luau 0.742)

> What this is: how this build's Luau VM decodes opcodes, how it stores its objects
> (including the pointer-encoding scheme), and where every core function and table lives.
> Every address was verified by hand against the binary; where a community dump disagrees,
> the corrected value is given.

Binary `MacPlayer-version-f9247f9560044102`, Mach-O x86-64, image base `0x100000000`
(all addresses below are absolute VMAs; subtract the base for a file offset).
Reference for names/semantics: the open-source Luau tree this build derives from.

Companion files: `opcode-map.csv` (all 93 opcodes, verified) · `dump-crosscheck.txt`
(per-symbol status of the community offset dump).

---

## Execution path

```
asset bytes ──▶ FUN_1014b91b5        container decoder (RSB1; see the section below)
                 sub_101593650        compilation cache ("Compilation_Cache_Key_Address")
                 LuaVMLoad 0x1029a9f36 ──▶ luaD_rawrunprotected 0x102995084
                 luau_load 0x1029aa05a     ──▶ FUN_1014bb465 integrity gate (on the
                         │                     decoded LBC blob, see below)
                         │                 decode opcodes (see below) ──▶ Proto + Closure
                         │
                 ScriptContext::startScript 0x10158afe8   (policy gate sub_10159502e,
                         │    re-entrancy ≤ 3, "Script Start" scope, record sub_10158cc58)
                 sub_1015a81a8  enqueue into scheduler
                         │
                 SchedulerFacet::resumeWaitingScripts 0x1015a8d94   (lock R15+0xf8,
                         │    pop from queue R15+0xd8, deadline 0x102f7eb5e)
                 FUN_101555d98  per-script resume (alive-check 0x10299fd1e, scope 0x1015c6549)
                 ScriptContextResume 0x1015566d6   (result 0=done, 1=yield, 6=error)
                 FUN_101556cb4  C-stack guard ≤200, shutdown-deadline check, CPU accounting
                         │
                 ┌─ fresh ─▶ lua_resume 0x102995af5        (pre-hook global+0x6a0,
                 │             luaD_rawrunprotected(…, FUN_102995c18, L->top−nresults*16),
                 │             post-hook global+0x640)
                 └─ error ─▶ lua_resumeerror 0x102995dc4   (walks ci chain for flags&2)
                         │
                 resume loop 0x102996328 ──▶ luau_execute 0x1029adfd3
                                             └─ Path A or Path B (see Interpreter)
```

**Pipeline correction (was wrong here earlier):** the "integrity gate"
`FUN_1014bb465` is NOT a pre-decoder step — its only caller is `luau_load`
(`0x1029aa05a`), which invokes it as `FUN_1014bb465(L, chunkname, data, size)`
on the already-decoded LBC blob *before* deserialization, and only proceeds
when it does not return 1. It is an obfuscated predicate (VMP-style opaque
arithmetic with table lookups at `DAT_10621fa9c`/`DAT_10621faa6`); on its
failure path it clears bit 30 of `*(L + 0x18) + 0x630`. The container decode
itself happens in `FUN_1014b91b5` before `LuaVMLoad` is ever called.

`startScript` also logs `[FLog::ScriptContext] Running script %p`; `ScriptContextResume`
logs `Resuming script: %p` and `Pending script name: %s`. On load failure the error goes
through `sub_101552b94`.

---

## Opcodes: three numberings

This build uses **three different byte values** for the same instruction. Mixing them up is
the most common way to mis-read the VM, so they are worth separating cleanly.

| Name | Range | Where it appears | Meaning |
|------|-------|------------------|---------|
| `canonical` | `0..92` | compiler, disassembler, CodeGen | the real ID; same order as the upstream `Bytecode.h` enum |
| `wire` | `0..255` | bytes on disk / over the network | scrambled canonical |
| `vmop` | `0..255` | the running interpreter | scrambled wire; what dispatch switches on |

```
canonical ──×227 mod 256──▶ wire ──table T──▶ vmop
```

- `wire      = (227 * canonical) & 0xFF`
- `vmop      = T[wire]` — `T` is a 256-byte permutation at **`0x1063bb686`**
- `canonical = R[vmop]` — reverse map at **`0x10648bb74`** (the binary ships both directions)

`luau_load` (`0x1029aa05a`) rewrites every instruction byte in place from `wire` to `vmop`,
then sizes the instruction with `getOpLength(R[vmop])`:

```
byte = T[byte]                       ; wire  → vmop
len  = getOpLength(R[byte])          ; vmop  → canonical → word count
```

`getOpLength` is identical to the upstream `CodeGen/src/IrUtils.cpp::getOpLength`. **31 of the
93** opcodes are two-word (carry an auxiliary operand); the rest are one-word.

**Verified** two independent ways, both 93/93: (1) the round-trip
`R[T[(227*c)&0xFF]] == c` holds for every opcode; (2) handler bodies were read and match
upstream semantics — `CMPPROTO`, `GETIMPORT`, `NEWTABLE`, `JUMPXEQKNIL`, `FASTCALL`, `CALL`,
`POW`, `POWK`. The full table (canonical, name, aux, wire, vmop, both handler addresses) is in
`opcode-map.csv`.

---

## Pointer encodings

Certain object fields are **not stored as plain pointers** — reading them naively yields
garbage. This is the scheme the community dump labels `VMValue1..4` without explaining.

It is **not cryptographic.** There is no secret key: each field is encoded relative to **its own
address** (`field_addr = object_base + offset`). Decoding is one arithmetic instruction.

Let `stored` = the raw value in memory and `field_addr` = the field's address:

| Scheme | Decode | Fields |
|--------|--------|--------|
| VMValue1 | `real = stored − field_addr` | `Proto.abslineinfo` |
| VMValue2 | `real = field_addr − stored` | `Proto.source`, `.lineinfo`, `.debugname`, `.debuginsn`; `Closure.cont` |
| VMValue3 | `real = field_addr XOR stored` | `Proto.upvalues`, `.userdata`; `lua_State.stacksize` |
| VMValue4 | `real = field_addr + stored` | `Proto.locvars`, `.typeinfo`; `TString.hash`; userdata `metatable` |

**Worked example — `Proto.source` (VMValue2, at `Proto+0x30`).** The decoder at `0x102993f3c`:

```asm
MOV  RAX, [RSI]           ; RAX = Proto*
LEA  RCX, [RAX + 0x30]    ; field_addr
SUB  RCX, [RAX + 0x30]    ; minus stored → real TString*   (text at +0x18)
```

**Proof.** `luaF_freeproto` (`0x102996918`) must un-encode every pointer before freeing, so it
exhibits all four schemes in one function (`−`, reversed-`−`, `^`, `+` respectively).
Independently, `luaF_newproto` (`0x1029964dd`) initializes each encoded field to a
self-referential "empty" sentinel, and every one of those sentinels decodes back to exactly
`0` under the table above (9/9 fields). The scheme is therefore pinned down, not inferred.

**What is *not* encoded** (read directly — this covers the whole hot path):

| Object | Plain fields |
|--------|--------------|
| `Proto` | `k` (+0x20), `code` (+0x28), `p` (+0x48) |
| `Closure` | `p`/`f` (+0x18), `debugname` (+0x28, C variant) |

So reading/patching **bytecode needs no decoding**; reading **debug metadata** (source name,
line tables, upvalue/local names, type info) plus `TString.hash`, userdata metatables and
`stacksize` **does**. Because the "key" is the field's own address, an incorrect struct offset
produces a wrong `field_addr` and therefore garbage — the offsets below are verified.

Separately, `global_State` carries a keyed pointer-encryption table — `ptrenckey[4]` at
`global+0x06B8` (plus `ptrenckeynew[8]` at `+0x49F0`, `ptrencactive` at `+0x4A30`).
`lua_State.stacksize` participates in it; the `Proto` debug fields above do not.

---

## The RSB1 script container (`FUN_1014b91b5` @ `0x1014b91b5`)

The decoder takes a `std::string` (Apple libc++ layout: size/flag byte `@0`,
inline data `@+1`, size `@+8`, heap ptr `@+0x10`), requires length ≥ 9, and
chooses one of three paths. Callers: `FUN_1014c83e8` (`require`),
`sub_101593650` (compilation cache), `FUN_1015890ee`, `FUN_10158d468`,
`FUN_101549288`, `FUN_1015925f4`. The result always goes to `LuaVMLoad`
(`0x1029a9f36`) as `(L, chunkname, bytes, size, env)`.

**Path A — plain bytecode blob.** Raw `W[0..7] == 0xE009325B4A107A52`
(= the K=0 scramble of `"RSB1"+0xFFFFFFFF`, byte-verified) → load `W[8..N)`
verbatim. The binary additionally gates the direct branch on `DAT_10752bd48`,
but both sub-paths load the same bytes.

**Path B — scrambled container.**

```
key[i]      = (W[i] ^ "RSB1"[i]) - i*0x29          (mod 256; SIMD: PXOR 0x106207660,
                                                    PADDB 0x106207670 = (0,-0x29,-0x52,-0x7B))
plain[p]    = W[p] ^ (uint8)(key[p & 3] + p*0x29)  (whole buffer, descrambled in place)
checksum    = XXH32(plain, seed 0x2A)              (FUN_103222533 — plain standard xxHash32)
verify      checksum == key as LE u32              (CMP EAX, R14D @ 0x1014b9413)
plain[0..3] = "RSB1"                               (0x31425352; re-written by the code)
plain[4..7] = u32 LE decompressed size
  size == 0                        → payload = plain[8..N) (plain bytecode)
  u32 at plain[8..11] == 0xFD2FB528 → payload = zstd_decompress(plain[8..N), expect size)
  otherwise                        → error 3
```

The descrambler `FUN_1014bae23` is expressed in the binary as: build a
16-byte keystream `ks[j] = key[j&3] + j*0x29`, XOR 16-byte blocks, bump every
keystream byte by `+0x90` per block (16·0x29 ≡ 0x90 mod 256) — algebraically
identical to the scalar formula above; the tail uses the same formula.

**The security trick:** the scramble key IS the xxHash32 checksum of the
plaintext. The key is recoverable from the scrambled bytes alone because the
first four plaintext bytes are the known constant `"RSB1"` — the derivation is
a tautology that exactly reconstructs the embedded key for any container. No
secret, nothing to brute-force; the checksum then authenticates the whole
payload. (This is the "shuffle/encryption" of script bytecode in this build.)

**zstd wrapper** `FUN_1032844e2` (`0x1032844e2`): create-dctx → decompress →
free-dctx; success requires the decompressed size to equal the `plain[4..7]`
field exactly. Allocation for the output happens in the caller via
`FUN_10064f530`.

Reproduced and tested in `../vm-repro/` (tools: `rbxrsb1 info|key|decode|encode`);
the checksum was verified by executing the binary's own machine code for
`FUN_103222533` over a 1000+-case sweep.

---

## Interpreter: two copies

Luau is a C++ template on a `SingleStep` flag, so the binary contains **two** full interpreters.
`luau_execute` (`0x1029adfd3`) is just the branch:

```c
if (L->singlestep)   // lua_State +0x06
    Path A;          // execute<true>  — 0x1029adfe9  — jump table 0x1072d8a30
else
    Path B;          // execute<false> — 0x1029b359c  — jump table 0x1072d9230
```

| | Path A | Path B |
|---|---|---|
| trigger | `L->singlestep == 1` (debugger attached) | everything else (the normal case) |
| entry | `0x1029adfe9` | `0x1029b359c` |
| jump table | `0x1072d8a30` | `0x1072d9230` |
| per-instruction | calls `global+0x668` (`cb.debugstep`), skipping `vmop` 1 (`PREPVARARGS`) and 85 (`BREAK`) — upstream `luau_skipstep` | none |
| prologue | — | if `ci->flags & 4` (NATIVE) call `global+0x6f0` (`ecb.enter`); return if it yields 0 |

Both tables are 256 entries; the valid index set is the 93 `vmop` values (index 0 invalid).
Interpreter entry registers: `R11 = ci->savedpc (+0x20)`, `cl = **(ci+0x18)`,
`p = *(ci+0x10)`, `k = *(p+0x20)`, `R10 = L->base`.

Prefer **Path B** when reading the interpreter — it is what actually runs and decompiles
cleanly; Path A does not decompile ("backward normalization not implemented") and must be read
as assembly.

---

## Object layouts

Offsets verified against the accessor code cited above. "Enc." column names the pointer-encoding
scheme from the previous section; blank means the field is stored plain.

### `lua_State` — a Lua thread / coroutine

| Off | Field | Enc. | | Off | Field | Enc. |
|-----|-------|------|-|-----|-------|------|
| `+0x00` | `tt` | | | `+0x40` | `stacksize` | VMValue3 |
| `+0x01` | `marked` | | | `+0x44` | `size_ci` | |
| `+0x02` | `memcat` | | | `+0x48` | `openupval` | |
| `+0x03` | `status` | | | `+0x50` | `gclist` | |
| `+0x04` | `activememcat` | | | `+0x58` | `nCcalls` (u16, limit 200) | |
| `+0x05` | `isactive` | | | `+0x5A` | `baseCcalls` (u16) | |
| `+0x06` | `singlestep` | | | `+0x5C` | `cachedslot` | |
| `+0x08` | `ci` | | | `+0x60` | `namecall` | |
| `+0x10` | `stack_last` | | | `+0x68` | `gt` | |
| `+0x18` | `global` | | | `+0x70` | `end_ci` | |
| `+0x20` | `top` | | | `+0x78` | `base_ci` | |
| `+0x28` | `base` | | | `+0x80` | `userdata` | |
| `+0x30` | `stack` | | | | | |
| `+0x38` | `finalizers` | | | | | |

`status`: `0=OK, 1=YIELD, 6=error`. `singlestep` (+0x06) is the Path A/B selector.
Note `stack` sits at `+0x30` and `base_ci` at `+0x78` — confirmed by `lua_resetthread`
(`0x10299fb71`); earlier notes placing them at `0x78`/`0x70` were wrong.

### `CallInfo` — one stacked call, stride `0x30`

| Off | Field |
|-----|-------|
| `+0x00` | `base` |
| `+0x08` | `top` (= `L->top + 0x140`, LUA_MINSTACK) |
| `+0x10` | `p` (Proto*) |
| `+0x18` | `func` (StkId) |
| `+0x20` | union `savedpc` / `errfunc` |
| `+0x28` | `nresults` |
| `+0x2C` | `flags` |

`flags`: `RETURN=1, HANDLE=2, NATIVE=4, OPYIELD=8, PCALL=16`. `& 4` (NATIVE) is tested at
interpreter entry; `& 2` (HANDLE) in `lua_resumeerror`. Derived from the call helper
`FUN_1029adc1a` (`func = L->top − (nargs+1)*16`, push ci, call, pop ci).

### `Proto` — one compiled function, size `0xD8`

| Off | Field | Enc. | | Off | Field | Enc. |
|-----|-------|------|-|-----|-------|------|
| `+0x00` | `tt, marked, memcat` | | | `+0x68` | `execdata` | |
| `+0x03` | `nups` | | | `+0x70` | `exectarget` | |
| `+0x04` | `flags` | | | `+0x78` | `codeentry` | |
| `+0x05` | `numparams` | | | `+0x80` | `debuginsn` | VMValue2 |
| `+0x06` | `maxstacksize` | | | `+0x88` | `linedefined` | |
| `+0x07` | `is_vararg` | | | `+0x8C` | `sizetypeinfo` | |
| `+0x08` | `locvars` | VMValue4 | | `+0x90` | `sizeupvalues` | |
| `+0x10` | `lineinfo` | VMValue2 | | `+0x94` | `linegaplog2` | |
| `+0x18` | `abslineinfo` | VMValue1 | | `+0x98` | `sizelocvars` | |
| `+0x20` | `k` | — | | `+0x9C` | `bytecodeid` | |
| `+0x28` | `code` | — | | `+0xA0` | `sizep` | |
| `+0x30` | `source` | VMValue2 | | `+0xA4` | `sizelineinfo` | |
| `+0x38` | `gclist` | — | | `+0xA8` | `sizecode` | |
| `+0x40` | `userdata` | VMValue3 | | `+0xAC` | `sizek` | |
| `+0x48` | `p` | — | | `+0xB0` | `feedbackvec` | |
| `+0x50` | `debugname` | VMValue2 | | `+0xB8` | `feedbackvecsize` | |
| `+0x58` | `upvalues` | VMValue3 | | `+0xBC` | `funid` | |
| `+0x60` | `typeinfo` | VMValue4 | | `+0xC0` | `optimized` | |
| | | | | `+0xC8` | `deoptimized` | |
| | | | | `+0xD0` | `cost` | |

`k`, `code`, `p` are the three hot-path pointers and are stored plain.

### `Closure` — a function value (Lua or C)

| Off | Field |
|-----|-------|
| `+0x00` | `tt, marked, memcat` |
| `+0x03` | `isC` (0 = Lua, 1 = C) |
| `+0x04` | `preload` |
| `+0x05` | `stacksize` |
| `+0x06` | `nupvalues` |
| `+0x08` | `gclist` |
| `+0x10` | `env` |
| `+0x18` | union — Lua: `p` (Proto*) · C: `f` (lua_CFunction) |
| `+0x20` | union — Lua: first upvalue `TValue` · C: `cont` (VMValue2) |
| `+0x28` | (C only) `debugname` |
| `+0x30` | (C only) first upvalue `TValue` |

Upvalues are 16-byte `TValue`s. Alloc size: Lua `nupvalues*16 + 0x20`, C `nupvalues*16 + 0x30`.
`luaF_newLclosure` = `0x1029965b9`, `luaF_newCclosure` = `0x10299673d`.

### `TString` — a string

| Off | Field | Enc. |
|-----|-------|------|
| `+0x00` | `tt, marked, memcat` | |
| `+0x04` | `trap` (i16) | |
| `+0x06` | `atom` (i16) | |
| `+0x08` | `next` | |
| `+0x10` | `hash` (u32) | VMValue4 |
| `+0x14` | `len` (u32) | |
| `+0x18` | `data[]` | |

Alloc size `len + 0x19` (`luaS_newlstr` = `0x1029a06d3`, which also shows the `hash` encoding:
`stored = hash − (ts+0x10)`).

### `LuaTable` — size `0x30`

| Off | Field |
|-----|-------|
| `+0x00` | `tt, marked, memcat` |
| `+0x03` | `tmcache` |
| `+0x04` | `readonly` |
| `+0x05` | `lsizenode` |
| `+0x06` | `nodemask8` |
| `+0x07` | `safeenv` |
| `+0x08` | `sizearray` |
| `+0x0C` | union `lastfree` / `aboundary` |
| `+0x10` | `node` (initially `luaH_dummynode` = `0x1063bb640`) |
| `+0x18` | `array` |
| `+0x20` | `metatable` |
| `+0x28` | `gclist` |

`luaH_new` = `0x1029a4b19`.

### Others

| Struct | Layout |
|--------|--------|
| `stringtable` | `size +0x00`, `nuse +0x04`, `hash +0x08` |
| `lua_Debug` | `nupvals +0x00`, `isvararg +0x01`, `nparams +0x02`, `linedefined +0x04`, `protoid +0x08`, `currentline +0x0C`, `bytecodeid +0x10`, `name +0x18`, `what +0x20`, `short_src +0x28`, `source +0x30`, `userdata +0x38`, `ssbuf[0x100] +0x40` |
| `lua_Page` | `listprev +0x00`, `listnext +0x08`, `prev +0x10`, `next +0x18`, `pageSize +0x20`, `blockSize +0x24`, `freeList +0x28`, `freeNext +0x30`, `busyBlocks +0x34`, `padding[8] +0x38`, `data +0x40` |

---

## Address map

### Core VM

| Address | Function |
|---------|----------|
| `0x1029a9f36` | `LuaVMLoad` |
| `0x1029aa05a` | `luau_load` (opcode decode loop) |
| `0x1029adfd3` | `luau_execute` (Path A/B dispatcher) |
| `0x1029adfe9` | Path A interpreter (debug) |
| `0x1029b359c` | Path B interpreter (normal) |
| `0x1029adc1a` | call / precall helper |
| `0x102995084` | `luaD_rawrunprotected` |
| `0x102995125` | `luaD_throw` |
| `0x102995af5` | `lua_resume` |
| `0x102995dc4` | `lua_resumeerror` |
| `0x102996328` | resume loop |
| `0x102992a58` | `lua_yield` |
| `0x1029940f4` | `luaG_runerror` |
| `0x1029964dd` | `luaF_newproto` |
| `0x102996918` | `luaF_freeproto` (decodes all fields) |
| `0x1029965b9` | `luaF_newLclosure` |
| `0x10299673d` | `luaF_newCclosure` |
| `0x1029967b1` | `luaF_newUpval` |
| `0x102996c06` | `luaC_step` (GC) |
| `0x1029a4b19` | `luaH_new` (table) |
| `0x1029a63f4` | `luaH_clone` |
| `0x1029a06d3` | `luaS_newlstr` (string) |
| `0x10299fb71` | `lua_resetthread` |
| `0x10299fd5b` | `lua_newstate` |
| `0x1029abe86` | `luaV_gettable` |
| `0x1029ac376` | `luaV_settable` |
| `0x10299ebe4` | `luaV_equal` |
| `0x1029ac854` | `luaV_lessthan` |
| `0x1029a8a5d` | GETIMPORT resolver |
| `0x10319633c` | in-binary disassembler |
| `0x103176e64` | Proto serializer |
| `0x103183106` | whole-buffer opcode transform |

### Roblox script layer

| Address | Function |
|---------|----------|
| `0x1014bb465` | pre-load integrity gate |
| `0x1014b91b5` | container decoder (RSB1 / decompress) |
| `0x101593650` | compilation cache |
| `0x10158afe8` | `ScriptContext::startScript` |
| `0x1015a8d94` | `SchedulerFacet::resumeWaitingScripts` |
| `0x1015a8050` | scheduler core — time-ordered queue insert |
| `0x101555d98` | per-script resume |
| `0x1015566d6` | `ScriptContextResume` |
| `0x101556cb4` | C-stack / shutdown guard |

### Data tables

| Address | Contents |
|---------|----------|
| `0x1063bb686` | `T` — wire→vmop permutation (256 bytes) |
| `0x10648bb74` | `R` — vmop→canonical reverse map |
| `0x1072d8a30` | Path A jump table (256) |
| `0x1072d9230` | Path B jump table (256) |
| `0x103196fbc` | disassembler format-string table |
| `0x1063bb640` | `luaH_dummynode` |
| `0x1063bb4e0` | `luaO_nilobject` |

---

## Interpreter internals (Path B) — verified from the handlers

The register-machine loop was walked handler-by-handler; the full guided tour
lives in `../roblox-macplayer-internals/docs/luau-interpreter.md`. Findings
that correct or extend the layouts above:

### TValue layout (correction — not previously documented)

```text
offset  0        8        12   13      14
        value(8) | vec z  | tt | marked| memcat     (16 bytes total)
```

- `value` = double, GC pointer, or **three floats** (this build uses
  3-component vectors — `LUA_VECTOR_SIZE = 3` — which is why the tag sits at
  byte 12).
- The tag is read/written as a **dword at +0xc** (`LOADN` writes `0x3`;
  `GETTABLEKS`/`NAMECALL` compare the whole dword).
- Type tags are **renumbered**: `STRING = 6`, `TABLE = 7`, `FUNCTION = 8`
  (open-source: 5/6/7). `NIL/BOOLEAN/NUMBER` match upstream (0/1/3).
  Handlers mask tags with `& 0xf` — the tag byte's high nibble carries flags.
- `LuaNode` = `{ TValue val @0; TValue key @16 }` — **val first** (upstream is
  key-first), 32-byte stride confirmed by the slow-path slot arithmetic
  (`(node - nodes) >> 5`).

### Register map (Path B)

| Reg | Holds |
|-----|-------|
| `R10` | `L->base` (register file; reg N at `base + N*16`) |
| `R11` | `pc` (current instruction word) |
| `R15` | dispatch table `0x1072d9230` |
| stack | `cl` (`[rbp-0x60]`), `k` (`[rbp-0x48]`), `L` (`[rbp-0x30]`) |

Dispatch = `jmp [table + opbyte*8]`; handlers tail-jump to the next handler
(no C stack use between instructions). Protected operations save `pc` to
`ci->savedpc` and re-enter through shared helpers.

### Confirmed in vivo

- **Pointer encodings in the hot path**: `NAMECALL` decodes the encoded
  `TString.hash` (`real = field_addr + stored`) on every execution; `CALL`
  decodes the encoded `lua_State.stacksize` (`stored ^ (uint32)&field`).
- **Inline caches write back into the instruction stream**: the `GETTABLEKS`
  slow path stores the found slot into the instruction's `C` byte
  (`mov [r12+3], dl` where `r12 = pc`); `NAMECALL` does the same. `GETIMPORT`
  caches the resolved import **in the Proto's constant table** (first
  execution resolves, later ones are a 16-byte copy guarded by `env->safeenv`).
- **CALL/RETURN** confirm the `CallInfo` layout above (stride `0x30`,
  `base/top/p/func/savedpc/nresults/flags` at `0/8/0x10/0x18/0x20/0x28/0x2c`),
  the frame nil-fill between `top` and `base + stacksize*16`, the
  `flags & RETURN(1)` VM-exit boundary, and the `__call` retry path
  (`0x1029add42` rewrites reg[A] and the handler re-executes).
- **GC at loop backedges**: `FORNLOOP` calls `luaC_step` (`0x102996c06`)
  behind a flag before running the iteration.

---

## `task.*` library

Registered by `task_lib_init` (`0x1014cebe0`). These are the corrected addresses (the community
dump's `spawn` and `delay` are wrong):

| Member | Address |
|--------|---------|
| `synchronize` | `0x1014cedfe` |
| `desynchronize` | `0x1014ceec6` |
| `defer` | `0x1014cef92` |
| `spawn` | `0x1014cf416` |
| `delay` | `0x1014cf56a` |
| `wait` | `0x1014cf74a` |
| `cancel` | `0x1014cf880` |

`task.wait` (`0x1014cf74a`) yields through the scheduler (`0x1015a8050`), sets `thread+0xac = 3`,
and rejects AuroraScripts / non-yieldable threads. `task.cancel` (`0x1014cf880`) rejects the
current or dead thread (`cannot cancel thread`), else `lua_resetthread` or scheduler route.

---

## Unknowns (kept honest)

- **Path A interpreter** (`0x1029adfe9`) does not decompile; treated as assembly-only.
- **Userdata `metatable`** is taken as VMValue4 by the community dump and is consistent with the
  other VMValue4 fields, but the individual read site was not isolated.
- **`ci+0x28`** is read as `nresults`; whether it packs additional state is unconfirmed.
- **Alternate decoder branch** (`FUN_1014b96b3`, taken when `DAT_10752c0a0 != 0`) not yet
  analyzed; the main paths above are fully characterized.
- **`FUN_1014bb465`** is characterized functionally (obfuscated predicate called from
  `luau_load` on the LBC blob; failure clears bit 30 at `*(L+0x18)+0x630`) but the obfuscated
  arithmetic (opaque expressions over `DAT_10621fa9c`/`DAT_10621faa6` lookups) is not deobfuscated.
- ~~Asset-pipeline callers of the container decoder~~ resolved: `FUN_1014c83e8` (`require`),
  `sub_101593650` (compilation cache), `FUN_1015890ee`, `FUN_10158d468`, `FUN_101549288`,
  `FUN_1015925f4` (confirmed via xrefs).
