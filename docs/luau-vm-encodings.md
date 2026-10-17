# Luau VM encodings

> What this is: the two byte-level encodings this player layers over stock
> Luau — a permutation applied to all 93 opcode bytes, and a family of
> address-relative pointer encodings for selected object fields — plus the
> object layouts those encodings live in.

Both are read directly out of `__DATA:__const` and the accessor code, and
checked against stock Luau (`Common/include/Luau/Bytecode.h`,
`CodeGen/src/IrUtils.cpp`, `VM/src/lvmload.cpp`). Where the
[Luau VM lifecycle](luau-vm-lifecycle.md) walkthrough identifies
handlers by *behavior*, the opcode mapping here is *derived* from the
binary's own tables and reproduces all 93 opcodes, not a sample.

---

## 1. Opcode numbering

Stock Luau numbers its 93 opcodes densely `0..92` (`LOP_NOP` = 0 …
`LOP_FINCONSTRUCT` = 92). A module in this build moves the same
instruction between **three** different byte values:

| Name | Range | Where | Meaning |
|---|---|---|---|
| `canonical` | `0..92` | compiler / `CodeGen` / disassembler | stock `Bytecode.h` order — the real id |
| `wire` | `0..255` | bytes on disk, over the network | scrambled canonical |
| `vmop` | `0..255` | the running interpreter | scrambled wire; what dispatch switches on |

```text
canonical --×227 mod 256--> wire --table T--> vmop
```

- `wire = (227 * canonical) & 0xFF`
- `vmop = T[wire]` — `T` is a 256-byte permutation at `0x1063bb686`
- `canonical = R[vmop]` — the reverse map `R` is shipped too, at `0x10648bb74`

**Verified two independent ways, both 93/93, from the binary's raw bytes:**

1. the round trip `R[T[(227 * canonical) & 0xFF]] == canonical` holds for
   every opcode;
2. handler bodies match stock semantics at the slots that are easiest to
   misread:
   - `COVERAGE` (69 → wire 47 → vmop 9): increments bits 8..30 of its own
     instruction word, saturating at `0x7fffff` — the stock coverage count.
   - `CMPPROTO` (88 → 8 → 22): requires a function register, reads the Lua
     closure's `Proto`, compares `Proto.funid` (`+0xBC`) against the AUX
     word, and skips on mismatch.
   - `GETUDATAKS` (83 → 153 → 141): on a per-tag miss it rewrites its own
     opcode byte to `0x78` (= 120 = `GETTABLEKS`) and jumps into that
     handler — the fallback documents itself.
   - `NAMECALL` (20 → 188 → 186) writes `R(A+1) = R(B)` (self), then
     `R(A) = R(B)[K(AUX)]`; `GETTABLEKS` (15 → 77 → 120) writes only `R(A)`.

Because `T` is a fixed permutation, bytecode written by this player's
compiler does not run under stock Luau, and stock bytecode does not run
here — even though the handler bodies are stock-shaped.

### The full map

`wire = (227 * canonical) & 0xFF`, `vmop = T[wire]`. `AUX` marks the 31
two-word opcodes (checked against `IrUtils.cpp::getOpLength`). Handler
addresses are the normal-loop (table B) and single-step-loop (table A)
targets.

| # | opcode | AUX | wire | vmop | handler (B) | handler (A) |
|---:|---|:--:|---:|---:|---|---|
| 0 | `NOP` | · | 0 | 52 | `0x1029b4105` | `0x1029b02da` |
| 1 | `BREAK` | · | 227 | 85 | `0x1029b3daf` | `0x1029aec49` |
| 2 | `LOADNIL` | · | 198 | 107 | `0x1029b5583` | `0x1029b0a75` |
| 3 | `LOADB` | · | 169 | 163 | `0x1029b635b` | `0x1029b1321` |
| 4 | `LOADN` | · | 140 | 140 | `0x1029b5599` | `0x1029b02e3` |
| 5 | `LOADK` | · | 111 | 204 | `0x1029b569a` | `0x1029b0ddd` |
| 6 | `MOVE` | · | 82 | 160 | `0x1029b5a48` | `0x1029b10ce` |
| 7 | `GETGLOBAL` | AUX | 53 | 58 | `0x1029b39cd` | `0x1029b1188` |
| 8 | `SETGLOBAL` | AUX | 24 | 74 | `0x1029b6653` | `0x1029ae8d5` |
| 9 | `GETUPVAL` | · | 251 | 109 | `0x1029b6275` | `0x1029b0926` |
| 10 | `SETUPVAL` | · | 222 | 144 | `0x1029b5d80` | `0x1029af4c1` |
| 11 | `CLOSEUPVALS` | · | 193 | 219 | `0x1029b5abe` | `0x1029aef67` |
| 12 | `GETIMPORT` | AUX | 164 | 92 | `0x1029b3f8d` | `0x1029b0b2c` |
| 13 | `GETTABLE` | · | 135 | 159 | `0x1029b6c2a` | `0x1029aee47` |
| 14 | `SETTABLE` | · | 106 | 246 | `0x1029b5c54` | `0x1029af1d1` |
| 15 | `GETTABLEKS` | AUX | 77 | 120 | `0x1029b411c` | `0x1029ae557` |
| 16 | `SETTABLEKS` | AUX | 48 | 236 | `0x1029b5301` | `0x1029ae08c` |
| 17 | `GETTABLEN` | · | 19 | 164 | `0x1029b5de3` | `0x1029aeace` |
| 18 | `SETTABLEN` | · | 246 | 86 | `0x1029b6386` | `0x1029aeb37` |
| 19 | `NEWCLOSURE` | · | 217 | 255 | `0x1029b5af8` | `0x1029ae95d` |
| 20 | `NAMECALL` | AUX | 188 | 186 | `0x1029b4a5f` | `0x1029ae1fc` |
| 21 | `CALL` | · | 159 | 207 | `0x1029b4f33` | `0x1029af53c` |
| 22 | `RETURN` | · | 130 | 190 | `0x1029b88f9` | `0x1029b3275` |
| 23 | `JUMP` | · | 101 | 69 | `0x1029b5688` | `0x1029b0633` |
| 24 | `JUMPBACK` | · | 72 | 150 | `0x1029b6784` | `0x1029b05e5` |
| 25 | `JUMPIF` | · | 43 | 63 | `0x1029b54e1` | `0x1029b0b78` |
| 26 | `JUMPIFNOT` | · | 14 | 80 | `0x1029b66fb` | `0x1029b0c57` |
| 27 | `JUMPIFEQ` | AUX | 241 | 65 | `0x1029b57a3` | `0x1029b0c8a` |
| 28 | `JUMPIFLE` | AUX | 212 | 114 | `0x1029b644a` | `0x1029b0833` |
| 29 | `JUMPIFLT` | AUX | 183 | 17 | `0x1029b5934` | `0x1029b0782` |
| 30 | `JUMPIFNOTEQ` | AUX | 154 | 216 | `0x1029b5e8c` | `0x1029b06ee` |
| 31 | `JUMPIFNOTLE` | AUX | 125 | 198 | `0x1029b60fa` | `0x1029b0889` |
| 32 | `JUMPIFNOTLT` | AUX | 96 | 152 | `0x1029b6b82` | `0x1029b09a7` |
| 33 | `ADD` | · | 67 | 222 | `0x1029b7b3c` | `0x1029ae863` |
| 34 | `SUB` | · | 38 | 251 | `0x1029b7a9e` | `0x1029af582` |
| 35 | `MUL` | · | 9 | 20 | `0x1029b79a5` | `0x1029b0a01` |
| 36 | `DIV` | · | 236 | 106 | `0x1029b7ae6` | `0x1029b0ad2` |
| 37 | `MOD` | · | 207 | 25 | `0x1029b5735` | `0x1029aebda` |
| 38 | `POW` | · | 178 | 16 | `0x1029b58bf` | `0x1029b0313` |
| 39 | `ADDK` | · | 149 | 3 | `0x1029b4ed7` | `0x1029b1250` |
| 40 | `SUBK` | · | 120 | 235 | `0x1029b6731` | `0x1029b0505` |
| 41 | `MULK` | · | 91 | 220 | `0x1029b79fb` | `0x1029b0d4e` |
| 42 | `DIVK` | · | 62 | 14 | `0x1029b7909` | `0x1029b04a5` |
| 43 | `MODK` | · | 33 | 158 | `0x1029b682c` | `0x1029b1206` |
| 44 | `POWK` | · | 4 | 12 | `0x1029b5485` | `0x1029b0290` |
| 45 | `AND` | · | 231 | 201 | `0x1029b6cca` | `0x1029b10f7` |
| 46 | `OR` | · | 202 | 111 | `0x1029b67ec` | `0x1029b0d9d` |
| 47 | `ANDK` | · | 173 | 55 | `0x1029b5607` | `0x1029b0549` |
| 48 | `ORK` | · | 144 | 71 | `0x1029b563b` | `0x1029b0d31` |
| 49 | `CONCAT` | · | 115 | 240 | `0x1029b5f46` | `0x1029af2ac` |
| 50 | `NOT` | · | 86 | 210 | `0x1029b600e` | `0x1029b05b0` |
| 51 | `MINUS` | · | 57 | 83 | `0x1029b6314` | `0x1029b0fd7` |
| 52 | `LENGTH` | · | 28 | 56 | `0x1029b551d` | `0x1029b068c` |
| 53 | `NEWTABLE` | AUX | 255 | 57 | `0x1029b598e` | `0x1029b0e06` |
| 54 | `DUPTABLE` | · | 226 | 168 | `0x1029b68a9` | `0x1029aea7c` |
| 55 | `SETLIST` | AUX | 197 | 87 | `0x1029b3e2b` | `0x1029aecc4` |
| 56 | `FORNPREP` | · | 168 | 112 | `0x1029b692e` | `0x1029b129f` |
| 57 | `FORNLOOP` | · | 139 | 117 | `0x1029b69b7` | `0x1029ae2ef` |
| 58 | `FORGLOOP` | AUX | 110 | 183 | `0x1029b6150` | `0x1029b0ec1` |
| 59 | `FORGPREP_INEXT` | · | 81 | 61 | `0x1029b56bf` | `0x1029b0401` |
| 60 | `FASTCALL3` | AUX | 52 | 78 | `0x1029b3a82` | `0x1029aefa4` |
| 61 | `FORGPREP_NEXT` | · | 23 | 46 | `0x1029b55c5` | `0x1029b08df` |
| 62 | `NATIVECALL` | · | 250 | 176 | `0x1029b8a33` | `0x1029b3399` |
| 63 | `GETVARARGS` | · | 221 | 147 | `0x1029b64a0` | `0x1029af426` |
| 64 | `DUPCLOSURE` | · | 192 | 115 | `0x1029b8231` | `0x1029b1354` |
| 65 | `PREPVARARGS` | · | 163 | 1 | `0x1029b3d0d` | `0x1029b0372` |
| 66 | `LOADKX` | AUX | 134 | 66 | `0x1029b5a1b` | `0x1029b0588` |
| 67 | `JUMPX` | · | 105 | 185 | `0x1029b605b` | `0x1029b07d8` |
| 68 | `FASTCALL` | · | 76 | 116 | `0x1029b4014` | `0x1029aeeb6` |
| 69 | `COVERAGE` | · | 47 | 9 | `0x1029b52ab` | `0x1029b0758` |
| 70 | `CAPTURE` | · | 18 | 118 | `0x1029b8b9f` | `0x1029b34fc` |
| 71 | `SUBRK` | · | 245 | 145 | `0x1029b5d3c` | `0x1029b063d` |
| 72 | `DIVRK` | · | 216 | 209 | `0x1029b5fc8` | `0x1029b0bb4` |
| 73 | `FASTCALL1` | · | 187 | 227 | `0x1029b5216` | `0x1029ae15f` |
| 74 | `FASTCALL2` | AUX | 158 | 151 | `0x1029b6a71` | `0x1029af094` |
| 75 | `FASTCALL2K` | AUX | 129 | 125 | `0x1029b45da` | `0x1029aed04` |
| 76 | `FORGPREP` | · | 100 | 142 | `0x1029b62a5` | `0x1029b0c05` |
| 77 | `JUMPXEQKNIL` | AUX | 71 | 238 | `0x1029b5d10` | `0x1029b0e95` |
| 78 | `JUMPXEQKB` | AUX | 42 | 206 | `0x1029b60be` | `0x1029b1141` |
| 79 | `JUMPXEQKN` | AUX | 13 | 18 | `0x1029b5863` | `0x1029b0454` |
| 80 | `JUMPXEQKS` | AUX | 240 | 179 | `0x1029b6d12` | `0x1029b0956` |
| 81 | `IDIV` | · | 211 | 253 | `0x1029b7820` | `0x1029af15c` |
| 82 | `IDIVK` | · | 182 | 28 | `0x1029b786d` | `0x1029b106e` |
| 83 | `GETUDATAKS` | AUX | 153 | 141 | `0x1029b48d3` | `0x1029ae67f` |
| 84 | `SETUDATAKS` | AUX | 124 | 88 | `0x1029b3f18` | `0x1029ae7e3` |
| 85 | `NAMECALLUDATA` | AUX | 95 | 127 | `0x1029b46b0` | `0x1029af33c` |
| 86 | `NEWCLASSMEMBER` | AUX | 66 | 231 | `0x1029b6bdc` | `0x1029b1023` |
| 87 | `CALLFB` | AUX | 37 | 40 | `0x1029b367b` | `0x1029b0a8f` |
| 88 | `CMPPROTO` | AUX | 8 | 22 | `0x1029b361a` | `0x1029ae03e` |
| 89 | `FASTPCALL` | · | 235 | 149 | `0x1029b4942` | `0x1029ae38e` |
| 90 | `NEWCLASS` | AUX | 206 | 157 | `0x1029b6ae9` | `0x1029aedcd` |
| 91 | `CONSTRUCT` | AUX | 177 | 247 | `0x1029b5e4e` | `0x1029b0ce9` |
| 92 | `FINCONSTRUCT` | · | 148 | 79 | `0x1029b3b7c` | `0x1029ae529` |

`CAPTURE`'s table slot (`vmop` 118, `0x1029b8b9f`) is a bare `UD2` trap: it
is never dispatched as a standalone instruction, because the closure
handlers (`NEWCLOSURE` / `DUPCLOSURE`) consume the following `CAPTURE`
words inline. The slot exists only to keep the table dense.

Two `FASTCALL` helper tables sit in `__DATA:__const`: `0x1072d74e0` holds
code pointers into the Luau builtin region (`0x10298…`) — the
`luauF_table`-shaped fast-function table — while `0x1072d7ce0` interleaves
code pointers with non-code values, so it is a separate builtin / name
structure, not the same direct table. The opcode map above does not depend
on the index-to-builtin-name assignment.

---

## 2. The loader rewrites opcodes in place

`luau_load` (`0x1029aa05a`) does not keep a private copy of the code. After
the `Invalid Bytecode.` guards it walks the instruction stream and replaces
each wire opcode byte with its `vmop` in place, so the array the interpreter
runs is already `T[wire]`. `getOpLength` is used only to step between
instructions:

```text
byte = T[byte]                ; wire -> vmop
len  = getOpLength(R[byte])   ; vmop -> canonical -> word count
```

Operands are never touched. `getOpLength` is byte-for-byte the upstream
`IrUtils.cpp::getOpLength`, and **31 of the 93** opcodes are two-word.

---

## 3. Pointer encoding

Several object fields do **not** hold plain pointers — read naively they
return garbage. This is the scheme community offset dumps label
`VMValue1..4` without explaining. It is not cryptographic; there is no
secret key. Each field is encoded relative to **its own address**,
`field_addr = object_base + offset`.

Let `stored` be the raw value in memory and `field_addr` the field's
address:

| Scheme | Decode | Fields |
|---|---|---|
| `VMValue1` | `real = stored − field_addr` | `Proto.abslineinfo` |
| `VMValue2` | `real = field_addr − stored` | `Proto.source`, `.lineinfo`, `.debugname`, `.debuginsn`; `Closure.cont` |
| `VMValue3` | `real = field_addr ^ stored` | `Proto.upvalues`, `.userdata`; `lua_State.stacksize` |
| `VMValue4` | `real = field_addr + stored` | `Proto.locvars`, `.typeinfo`; `TString.hash`; userdata `metatable` |

**Worked example — `Proto.source` (VMValue2, at `Proto+0x30`).** The
decoder at `0x102993f3c`:

```asm
MOV  RAX, [RSI]           ; RAX = Proto*
LEA  RCX, [RAX + 0x30]    ; field_addr
SUB  RCX, [RAX + 0x30]    ; field_addr - stored -> real TString*   (text at +0x18)
```

**Proof.** `luaF_freeproto` (`0x102996918`) must un-encode every pointer
before freeing, so one function shows all four schemes at once (`−`,
reversed-`−`, `^`, `+`), and it frees `code` (`+0x28`), `p` (`+0x48`) and
`k` (`+0x20`) raw, marking those as unencoded. Independently,
`luaF_newproto` (`0x1029964dd`) seeds each encoded field with a
self-referential empty sentinel, and all nine decode back to exactly `0`.

**What is not encoded** — this is the whole hot path:

| Object | Plain fields |
|---|---|
| `Proto` | `k` (+0x20), `code` (+0x28), `p` (+0x48) |
| `Closure` | `p`/`f` (+0x18), `debugname` (+0x28, C variant) |

So reading or patching bytecode needs no decoding; reading debug metadata
(source name, line tables, upvalue/local names, type info) plus
`TString.hash`, userdata metatables, and `stacksize` does. Because the key
is the field's own address, a wrong struct offset produces a wrong
`field_addr` and therefore garbage — the offsets below are load-bearing,
not cosmetic.

Separately, `global_State` carries a keyed pointer-encryption table —
`ptrenckey[4]` at `global+0x06B8`, `ptrenckeynew[8]` at `+0x49F0`,
`ptrencactive` at `+0x4A30`. `lua_State.stacksize` participates in it; the
`Proto` debug fields above do not.

---

## 4. Layouts

Offsets verified against the accessor code above. "Enc" names the pointer
scheme; blank means stored plain.

### `lua_State` — a Lua thread

| Off | Field | Enc | Off | Field | Enc |
|---|---|---|---|---|---|
| `+0x00` | `tt` | | `+0x40` | `stacksize` | VMValue3 |
| `+0x01` | `marked` | | `+0x44` | `size_ci` | |
| `+0x02` | `memcat` | | `+0x48` | `openupval` | |
| `+0x03` | `status` | | `+0x50` | `gclist` | |
| `+0x04` | `activememcat` | | `+0x58` | `nCcalls` (u16) | |
| `+0x05` | `isactive` | | `+0x5A` | `baseCcalls` (u16) | |
| `+0x06` | `singlestep` | | `+0x5C` | `cachedslot` | |
| `+0x08` | `ci` | | `+0x60` | `namecall` | |
| `+0x10` | `stack_last` | | `+0x68` | `gt` | |
| `+0x18` | `global` | | `+0x70` | `end_ci` | |
| `+0x20` | `top` | | `+0x78` | `base_ci` | |
| `+0x28` | `base` | | `+0x80` | `userdata` | |
| `+0x30` | `stack` | | | | |
| `+0x38` | `finalizers` | | | | |

`status`: `0 = OK, 1 = YIELD, 6 = error`. `singlestep` (+0x06) selects the
debug interpreter. `stack` at `+0x30` and `base_ci` at `+0x78` were
confirmed against `lua_resetthread` (`0x10299fb71`).

### `CallInfo` — one stacked call, stride `0x30`

| Off | Field |
|---|---|
| `+0x00` | `base` |
| `+0x08` | `top` (= `L->top + 0x140`, `LUA_MINSTACK`) |
| `+0x10` | `p` (Proto*) |
| `+0x18` | `func` (StkId) |
| `+0x20` | union `savedpc` / `errfunc` |
| `+0x28` | `nresults` |
| `+0x2C` | `flags` |

`flags`: `RETURN = 1, HANDLE = 2, NATIVE = 4, OPYIELD = 8, PCALL = 16`.
`& 4` is tested at interpreter entry; `& 2` in `lua_resumeerror`.

### `Proto` — one compiled function, size `0xD8`

| Off | Field | Enc | Off | Field | Enc |
|---|---|---|---|---|---|
| `+0x00` | `tt, marked, memcat` | | `+0x68` | `execdata` | |
| `+0x03` | `nups` | | `+0x70` | `exectarget` | |
| `+0x04` | `flags` | | `+0x78` | `codeentry` | |
| `+0x05` | `numparams` | | `+0x80` | `debuginsn` | VMValue2 |
| `+0x06` | `maxstacksize` | | `+0x88` | `linedefined` | |
| `+0x07` | `is_vararg` | | `+0x8C` | `sizetypeinfo` | |
| `+0x08` | `locvars` | VMValue4 | `+0x90` | `sizeupvalues` | |
| `+0x10` | `lineinfo` | VMValue2 | `+0x94` | `linegaplog2` | |
| `+0x18` | `abslineinfo` | VMValue1 | `+0x98` | `sizelocvars` | |
| `+0x20` | `k` | — | `+0x9C` | `bytecodeid` | |
| `+0x28` | `code` | — | `+0xA0` | `sizep` | |
| `+0x30` | `source` | VMValue2 | `+0xA4` | `sizelineinfo` | |
| `+0x38` | `gclist` | — | `+0xA8` | `sizecode` | |
| `+0x40` | `userdata` | VMValue3 | `+0xAC` | `sizek` | |
| `+0x48` | `p` | — | `+0xB0` | `feedbackvec` | |
| `+0x50` | `debugname` | VMValue2 | `+0xB8` | `feedbackvecsize` | |
| `+0x58` | `upvalues` | VMValue3 | `+0xBC` | `funid` | |
| `+0x60` | `typeinfo` | VMValue4 | `+0xC0` | `optimized` | |
| | | | `+0xC8` | `deoptimized` | |
| | | | `+0xD0` | `cost` | |

`k`, `code`, `p` are the three hot-path pointers and are plain. `funid`
(`+0xBC`) is what `CMPPROTO` compares against.

### `Closure` — a function value

| Off | Field |
|---|---|
| `+0x00` | `tt, marked, memcat` |
| `+0x03` | `isC` (0 = Lua, 1 = C) |
| `+0x04` | `preload` |
| `+0x05` | `stacksize` |
| `+0x06` | `nupvalues` |
| `+0x08` | `gclist` |
| `+0x10` | `env` |
| `+0x18` | union — Lua: `p` (Proto*) · C: `f` (lua_CFunction) |
| `+0x20` | union — Lua: first upvalue TValue · C: `cont` (VMValue2) |
| `+0x28` | (C only) `debugname` |
| `+0x30` | (C only) first upvalue TValue |

Upvalues are 16-byte TValues. Alloc size: Lua `nupvalues*16 + 0x20`,
C `nupvalues*16 + 0x30`.

### `TString` — a string

| Off | Field | Enc |
|---|---|---|
| `+0x00` | `tt, marked, memcat` | |
| `+0x04` | `trap` (i16) | |
| `+0x06` | `atom` (i16) | |
| `+0x08` | `next` | |
| `+0x10` | `hash` (u32) | VMValue4 |
| `+0x14` | `len` (u32) | |
| `+0x18` | `data[]` | |

Alloc size `len + 0x19`. `luaS_newlstr` (`0x1029a06d3`) shows the hash
encoding directly: `stored = hash − (ts + 0x10)`.

### `TValue` — a single Lua value (16 bytes)

```text
offset  0        8        12
        value(8) | extra  | tt
```

The value slot holds a `double`, a heap pointer, or the first two floats of a
3-component vector; the third component lives in `extra`, which is why the
type tag sits at byte 12. Handlers read and write the tag as a **dword at
`+0xc`**. The tags match the reference tree at this configuration:
`STRING = 6`, `TABLE = 7`, `FUNCTION = 8` (with `INTEGER = 4` and
`VECTOR = 5` before them); `NIL/BOOLEAN/NUMBER` are 0/1/3. Handlers mask
tags with `& 0xf` before comparing, so the high nibble carries flags.
Verified from the `LOADN`, `GETTABLEKS` and `NAMECALL` handlers — see
[Inside the Luau interpreter](luau-interpreter.md).

### `LuaTable` — size `0x30`

| Off | Field |
|---|---|
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

---

## Unknowns (kept honest)

- The single read site for userdata `metatable` (VMValue4) was not
  isolated; the assignment comes from the dump plus consistency with the
  other VMValue4 fields.
- `CallInfo+0x28` is read as `nresults`; whether it packs extra state is
  unconfirmed.
- The index-to-builtin-name assignment for the two `FASTCALL` tables is
  not recovered. The opcode map above does not depend on it.
- These offsets and the opcode permutation belong to this build only.
