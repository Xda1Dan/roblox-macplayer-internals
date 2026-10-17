# Luau VM lifecycle in this player

> What this is: how a `lua_State` comes into existence, what Roblox puts
> around stock Luau, how code runs protected, and where strings get
> deliberately hard to trace by string reference.

```
allocator callback + context
  |  lua_newstate (alloc 0x4d60, init, protected init) — 0x10299fd5b
  v
ScriptContext VM wrapper
  |  caller: hooks + libraries + _G + sandbox + retain — 0x1015b92cd
  v
compile / load boundary
  |  loadsafe-shaped protected load — 0x1029a9f36
  |  bytecode auth + "Invalid Bytecode." guards
  v
protected execution
  |  luaB_pcall wrapper — 0x10298b913
  |  luaD_rawrunprotected boundary — 0x102995084
  |  luaD_throw (C++ exception in this build) — 0x102995125
  |  luaG_runerror — 0x1029940f4
  |  luau_execute dispatch stub (singlestep check + jump table) — 0x1029adfd3
  v
selective pointer hardening (string-reference hardening)
```

---

## The load boundary in detail

The wrapper (`LuaVMLoad`, `0x1029a9f36`) runs a GC step if memory is tight,
then **suspends the GC entirely** (`GCthreshold = -1`) for the duration of
the load — a chunk can never be interrupted by a collection mid-deserialize.
The deserializer (`luau_load`, `0x1029aa05a`) then runs protected via
`luaD_rawrunprotected`, and the threshold is restored afterwards.

Inside `luau_load`, after the version checks and the integrity gate
(`FUN_1014bb465`), each proto's code words are bulk-copied from the stream
and then rewritten **wire → vmop in place** (table `T` at `0x1063bb686`,
stepping by `getOpLength(R[vmop])`). After all protos are loaded, a single
pass walks every proto's instructions and swaps table ops to their userdata
variants when the referenced string constant has an assigned **atom**:
`GETTABLEKS→GETUDATAKS`, `SETTABLEKS→SETUDATAKS`, `NAMECALL→NAMECALLUDATA`
(vmop swaps `0x78→0x8d` (141), `0xec→0x58` (88), `0xba→0x7f` (127)). The atom is fetched
through a hook at `global+0x658` and cached in the string's `+0x06` field.
This is what makes userdata field access faster than a generic table lookup.

The full deserialization sequence (string table, proto fields, constants,
line info, feedback slots) matches the reference `lvmload.cpp` shape; the
research notes cover the Roblox deviations (integrity gate, wire→vmop
rewrite, GC suspension). The authentication internals have their own
section below.

## Bytecode authentication — SigGuard

The integrity gate (`FUN_1014bb465`, called at `0x1029aa242` before
deserialization) is not an opaque predicate — it is a signature verifier,
and its module registers itself at runtime under the literal name
**SigGuard** (once-init `FUN_105a4ee65` runs
`FUN_103209804(&DAT_107a70110, "SigGuard")`). On rejection the loader
pushes `"%s: bytecode corrupted"`; the gate also reports telemetry and
clears bit 30 of `global+0x630`.

### What a loadable blob looks like

| Requirement | Detail |
|---|---|
| Version gate | byte 0 in `3..14` (or `100`); type version `1..3` for v ≥ 4 |
| Auth trailer (v1) | declared version **1**, 24 bytes at end−`0x18`; detected by `trailer[0..4]⊕trailer[20..24] == C4 32 6A <ver>` |
| Legacy trailer (v0) | detected by the secondary magic `u32[len−0x28]⊕u32[len−0x20] == 0x946AC432`; always rejected by the threshold |
| Checkword | for the v1 trailer, `u32(trailer[0..4]) ⊕ u32(trailer[4..8])` must equal `0x74171870` |
| Signature | the trailer's first 16 bytes, mixed through the table MAC, must equal bytes `[4..16)` of `sha256hex(payload)` |

Unauthenticated and legacy-v0 blobs take the verifier's "simple path" and
are rejected **by design** (result code 6) — only v1-trailer blobs are ever
fully verified. Trailer versions ≥ 2 are also rejected (max is the `be38`
flag), while blobs ≤ `0x117` bytes or with `data[0] == 0` pass through
untouched (those are the loader's error-message chunks).

### How the signature is checked

1. The payload (blob minus trailer) is digested by SHA-256
   (`FUN_102f486bc` → OpenSSL `SHA256_Init/Update/Final`, 32-byte state)
   and hex-encoded (64 characters).
2. The trailer's first 16 bytes select rows from a table set packed in the
   `0x1830c`-byte binary blob at `0x106207760` (**8257** 12-byte records,
   header `GYM\x14`, expanded lazily on first use). Expansion builds T1
   (`0x20410` bytes of 16-byte rows), two `0x1000`-row tables A/B, and a
   `0x3C000`-row cross table C. The result is a fixed degree-2 function of
   the 16 selector bytes; reproduction in `vm-repro/src/sigguard.cpp`.
3. The mixed 12-byte state must equal `sha256hex(payload)[4..16)`.

A failed or weak blob is not silently dropped: the gate fires a telemetry
event (fields `placeid`, `version`, `code`; the mode chain uses events
901/902/903, `decrcc`, `declocal`), runs an anti-tamper fence, and clears
bit 30 of `global+0x630` (a 28-bit counter plus four flag bits, so the
clear is observable by other subsystems). The clear applies to verifier
codes 3,4,6,7,9; codes 5 and 8 do not clear it.

### Configuration

The policy flags are FFlag-backed descriptors of the form
`{int value; const char* name}`, initialized by per-global stubs, all
defaulting to 1:

| Global | Role | Note |
|---|---|---|
| `DAT_10752be50` | mode-1 MAC verification enable | named `LsbOptimizationDetail`; **0 auto-passes every signed blob** — the signature kill-switch |
| `DAT_10752be38` | max supported trailer version | ≥ 2 rejected |
| `DAT_10752be20` | verification threshold | ≥ 2 also rejects mode 1 (total lockdown) |
| `DAT_10752be68` | force-signed | rejects v0 blobs outright |
| `DAT_10752be08` / `DAT_10752bdf0` | payload length gate / compare length | |

!!! note
    The check binds the trailer to the payload digest with key material
    baked into the binary, so forging bytecode offline requires the
    extracted table set. Patching the gate call site (`0x1029aa242`) or
    flipping `LsbOptimizationDetail` remains the practical bypass.

Full step-by-step notes: `research/vm-research/luau-load.md`.

## 1. lua_newstate - 0x10299fd5b

**What it does:** allocates a `0x4d60`-byte Luau state through the
caller-supplied allocator, lays out global-state / main-thread / stack / GC
fields, then runs protected init. Null means the caller reports
`Failed to create Lua state`.

**Function card**

| | |
|---|---|
| entry | 0x10299fd5b |
| size | 0x4a8 bytes |
| args | (allocatorFn, allocatorCtx) - RDI = fn, RSI = ctx |
| covered here | entry, inputs, and exits |

**Step by step**

1. `CALL R15` with `(ctx, NULL, 0, 0x4d60)` - the allocator is the first
   argument. Roblox owns allocation policy; Luau just calls back.
   `0x4d60` is `sizeof(LG)` (`lua_State` + `global_State`).
2. Branch on a global flag: two init layouts (fast vs full zeroing). This
   is `FFlag::LuauEasyStateInit` straight from Luau source — the fast
   path zeroes one range and writes a handful of fields, the full path
   initializes every field individually.
3. The tag write `*L = 0x90a` decodes against source too: low byte `0x0A`
   is `LUA_TTHREAD`, high byte `0x09` is the initial `marked` bits
   (`L->tt = LUA_TTHREAD; L->marked = ...`). Allocator fn/ctx land at
   `+0x5c/+0x60` (`g->frealloc`, `g->ud`), stack caps read back as
   `0x4d60`.
4. `bzero` large ranges - a fresh state has no stale stack / GC / upvalue
   data by construction.
5. Protected init call; nonzero means free state and return null.

**Assembly - allocator call plus tag init (excerpt)**

```asm
10299fd66 MOV R14,RSI           ; allocator ctx
10299fd69 MOV R15,RDI           ; allocator fn
10299fd73 MOV ECX,0x4d60        ; state size
10299fd78 MOV RDI,RSI
10299fd7b XOR ESI,ESI
10299fd7d XOR EDX,EDX
10299fd7f CALL R15              ; state = alloc(ctx,0,0,0x4d60)
10299fd82 MOV RBX,RAX
10299fda6 LEA RAX,[RBX+0x90]
10299fdad MOV word ptr [RBX],0x90a    ; state tag
10299fdb2 MOV byte ptr [RBX+0x90],0x9 ; main-thread tag
10299fdb9 MOV [RBX+0x18],RAX
10299fdc4 MOV [RBX+0xb8],R15   ; allocator fn
10299fdcb MOV [RBX+0xc0],R14   ; allocator ctx
```

Takeaway: "how does Roblox create a Lua state" starts outside Luau -
find the allocator callback and its context, not just `lua_newstate`.

**Simplified reconstruction (original, not engine source)**

```cpp
lua_State* newState(AllocFn alloc, void* ctx) {
  auto* L = (lua_State*)alloc(ctx, nullptr, 0, 0x4d60);
  if (!L) return nullptr;
  L->tag = 0x90a;
  L->mainThreadTag = 0x9;
  L->alloc = alloc; L->allocCtx = ctx;
  L->stackCap = L->gcCap = 0x4d60;
  zeroRanges(L);  // stacks, GC lists, upvalues
  if (!protectedInit(L)) { alloc(ctx, L, 0x4d60, 0); return nullptr; }
  return L;
}
```

---

## 2. Wrapper - 0x1015b92cd

**What it does:** turns a raw state into a Roblox VM: install panic /
interrupt / error hooks, open the restricted standard libraries, register
Roblox bindings and globals, establish `_G`, attach sandbox and config
handlers, retain in the VM collection. Failure at any point unwinds;
there is never a half-owned state.

The `luaopen_base`, `luaopen_math`, and `luaopen_os` names point into this
registration sequence - but "opens base" does not mean stock unrestricted
base. Policy, identity, and capabilities are applied around the VM before
any script runs, and scripts get threads from the collection, not fresh
states.

---

## 3. Protected execution (pcall question, settled)

**Finding:** the public names `lua_pcall` / `lua_pcallk` are absent from
the shipped symbol and string inventory. The machinery is present:

| address | role |
|---|---|
| 0x10298b913 | luaB_pcall builtin: frame prep, protected call, status convert |
| 0x10298b946 | luaB_pcallrun stack-result continuation |
| 0x102995084 | luaD_rawrunprotected boundary |
| 0x102995125 | luaD_throw via C++ exception |
| 0x1029940f4 | luaG_runerror build and propagate |
| 0x1029adfd3 | luau_execute dispatch stub (see below) |

So: name stripped, implementation present. Engine paths also use their own
ScriptContext protected helpers rather than needing a public `lua_pcall`.

Checked against Luau source, the labels hold up:

- `luaD_throw` uses the C++ `lua_exception` path (not setjmp) — the
  binary body contains `___cxa_throw`, and the string table holds
  `lua_exception: not enough memory`, which is source's `LUA_MEMERRMSG`.
- `luaB_pcall` matches source's `lua_pcallyieldable` wrapper shape, and
  the neighboring `luaB_pcallrun` matches the `luaB_pcallcont` result
  shuffling (shift results right to make room for the status boolean).

### `luau_execute` is a dispatch stub

The stub (`0x1029adfd3`, 0xfb bytes) is not the interpreter loop — it is the
entry that selects *which* loop to enter. Cross-checked against Luau
source (`lvmexecute.cpp`, `lstate.h`):

1. Test `[RDI+0x6]`. The `lua_State` header is 3 bytes
   (`tt`/`marked`/`memcat`), then `status` (+3), `activememcat` (+4),
   `isactive` (+5), `singlestep` (+6) — so this byte is `singlestep`,
   **not** thread status as an earlier revision of these notes claimed.
   Value `1` tail-jumps to the single-step loop at `0x1029adfe9`;
   anything else falls into the normal loop at `0x1029b359c`. This
   matches source exactly: `if (L->singlestep) luau_execute<true>(L);
   else luau_execute<false>(L);`
2. The normal path tests a global hook flag and runs the hook
   (`CALL [RAX+0x6f0]`) when set.
3. It reads the first bytecode byte, loads the opcode jump table at
   `0x1072d9230`, and enters the interpreter with `JMP [table + byte*8]`
   — a computed goto, which is what defeated the decompiler. Calling
   convention is `RDI` = thread.

```asm
1029adfd3 PUSH RBP
1029adfd7 CMP byte ptr [RDI+0x6],0x1   ; singlestep?
1029adfdb JNZ 0x1029adfe3       ; normal loop
1029adfdd POP RBP
1029adfde JMP 0x1029adfe9       ; single-step loop
; normal path at 0x1029b359c:
1029b35b8 TEST byte ptr [RAX+0x2c],0x4   ; hook flag?
1029b35ca CALL qword ptr [RAX+0x6f0]     ; hook
1029b3609 MOVZX EAX,byte ptr [R11]       ; first opcode byte
1029b360d LEA R15,[0x1072d9230]          ; opcode jump table
1029b3614 JMP qword ptr [R15+RAX*8]     ; enter interpreter loop
```

### Twin opcode tables, shuffled bytes

There are **two** adjacent 256-entry tables in `__DATA:__const`:

| Table | Address | Targets |
|---|---|---|
| A (single-step loop) | `0x1072d8a30` | `0x1029ae03e–0x1029b34fc` |
| B (normal loop) | `0x1072d9230` | `0x1029b361a–0x1029b8b9f` |

Both hold exactly 93 non-zero slots with bit-identical occupancy masks
but different handler addresses — one handler copy per loop, back to
back in `__text` (combined span `0x1029ae03e–0x1029b8b9f`).

Stock Luau has exactly 93 opcodes (`LOP_NOP` = 0 … `LOP_FINCONSTRUCT` =
92), yet the non-zero slots scatter across indices 0–255 (slot 0 empty,
slot 2 empty, slots 32–39 all empty, and so on). The table does **not**
follow stock `LOP_*` order.

The permutation is exact, not approximate. The byte on disk is `wire =
(227 * canonical) & 0xFF`, and the byte the interpreter dispatches on is
`T[wire]`, for the 256-byte table `T` at `0x1063bb686`; the binary ships
the inverse too (`R` at `0x10648bb74`, with `R[T[wire]] == canonical`).
All 93 slots were resolved by this round trip and cross-checked against
their handler bodies. The full table and the arithmetic live in
[Luau VM encodings](luau-vm-encodings.md). Stock-Luau bytecode still
will not execute here even where the VM logic is identical.

---

### Loop-1 (single-step) mirrors loop-2 per slot

Spot-checked through raw disassembly: table-A handlers for slots 3, 12,
and 20 implement the same semantics as their table-B twins. Slots 3 and
20 match inline (same `addsd`/`mulsd` fast paths, byte-identical
prologues apart from the pc register). Slot 12 differs in layout only:
loop-2 keeps the POWK `2.0`/`0.5`/`3.0` fast paths inline, loop-1 calls
an outlined helper at `0x105a78166` holding the identical constant
compares and the same `_pow` callee.

Loop-1's dispatch and frame differ systematically from loop-2's:

| | Loop 1 (single-step) | Loop 2 (normal) |
|---|---|---|
| Table | A at `0x1072d8a30` | B at `0x1072d9230` |
| Central dispatch | `0x1029ae667`: index, load handler, stash pc, jump via `0x1029ae038` trampoline | `0x1029b360d`: index, jump straight through |
| pc register | `r8` | `r11` |
| Constant base | `[rbp-0x50]` | `[rbp-0x48]` |
| Hook trampoline | `0x1029ae031` → `0x1029ae615` debug check | inline hook flag test |

All handler tails converge on the loop's dispatch point, so each loop is
self-contained. The constant-base difference is why K-operand addressing
must be read relative to each loop's own frame, not as an absolute
offset.

## 4. Split pointers (why some string references miss)

**What it does:** selected sites replace `lea reg,[rip+string]` with
`lea dummy + mov imm(delta) + add`, where `dummy` is an unrelated address
already in `__TEXT` and `real = dummy + delta`. Simple string-reference scans that
only match RIP-relative references to `__cstring` miss these.

**Verified example - 0x102988254 (Luau error path)**

```asm
lea    rax, [rip+...]           ; dummy = 0x106c9c778 (__unwind_info)
mov    [rbp-0x18], rax
mov    rsi, 0xffffffffffd06c00  ; delta = -0x2f9400
add    rsi, [rbp-0x18]          ; rsi = 0x1069a3378
call   0x1029940f4          ; luaG_runerror-shaped
; [0x1069a3378] = "stack limit" (__cstring)
```

```text
0x106c9c778 + (-0x2f9400) = 0x1069a3378
```

Selective hardening, not a universal compiler rule: direct RIP-relative
string references still exist elsewhere.

### Slots identified

Every slot is resolved by the derived numbering above (the round trip is
93/93 against the binary's own `T` / `R` tables), not by sampling. Handler
bodies were used to confirm the hard cases against source semantics rather
than to name the rest:

- `COVERAGE` (vmop 9, `0x1029b52ab`) increments bits 8..30 of its own
  instruction word, saturating at `0x7fffff` — the stock coverage counter,
  which reads like a loop-prep patch but is not.
- `CMPPROTO` (vmop 22, `0x1029b361a`) requires a function register, reads
  the Lua closure's `Proto`, compares `Proto.funid` (`+0xBC`) to the AUX
  word, and skips on mismatch.
- `GETUDATAKS` (vmop 141, `0x1029b48d3`) falls back by rewriting its own
  opcode byte to `0x78` (120 = `GETTABLEKS`) and jumping into that handler.
- `NAMECALL` (vmop 186, `0x1029b4a5f`) writes `R(A+1) = R(B)`, then
  `R(A) = R(B)[K(AUX)]`; `GETTABLEKS` (vmop 120, `0x1029b411c`) writes only
  `R(A)`.

Store opcodes are not duplicated: `SETGLOBAL` (vmop 74, `0x1029b6653`)
takes its value from `A` and its key from `AUX` against the environment,
and never reads the `B` register, while `SETTABLEKS` (vmop 236,
`0x1029b5301`) reads `B` as the table. They are two opcodes, not one
twice. The opcode byte values, the two loops' separate handler copies, and
the parity between them are tabulated in
[Luau VM encodings](luau-vm-encodings.md).

## Unknowns

- Exact helper names behind VM start / resume calls.
- `CAPTURE` is consumed inline by the closure handlers and its table slot
  is a `UD2` trap; all other opcodes are mapped, with the full table in
  [Luau VM encodings](luau-vm-encodings.md).
- Index-to-builtin-name assignment for the two `FASTCALL` tables.
- Full library open list vs flag-gated subsets per build.

## A note on the address list

30 sampled offsets from the address list (memory, GC, table, string,
type, value, load, library, and state functions) were checked against
this build's function-start table: every one lands exactly on a function
entry. The list is a reliable *index* — the remaining work is confirming
what each entry does, as done above for `spawn`, `luau_execute`, and
the facet accessor. Names are still not a stable interface across builds.
