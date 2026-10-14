# Luau VM lifecycle in this player

> What this is: how a `lua_State` comes into existence, what Roblox puts
> around stock Luau, how code runs protected, and where strings get
> deliberately hard to trace by string reference.

```
allocator callback + context
  |  0x10299fd5b  lua_newstate (alloc 0x4d60, init, protected init)
  v
ScriptContext VM wrapper
  |  0x1015b92cd  caller: hooks + libraries + _G + sandbox + retain
  v
compile / load boundary
  |  0x1029a9f36  loadsafe-shaped protected load
  |  bytecode auth + "Invalid Bytecode." guards
  v
protected execution
  |  0x10298b913  luaB_pcall wrapper
  |  0x102995084  luaD_rawrunprotected boundary
  |  0x102995125  luaD_throw (C++ exception in this build)
  |  0x1029940f4  luaG_runerror
  |  0x1029adfd3  luau_execute dispatch stub (singlestep check + jump table)
  v
selective pointer hardening (string-reference hardening)
```

---

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

`0x1029adfd3` (0xfb bytes) is not the interpreter loop — it is the
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

The striking part: stock Luau has exactly 93 opcodes (`LOP_NOP` = 0 …
`LOP_FINCONSTRUCT` = 92), yet the non-zero slots scatter across indices
0–255 (slot 0 empty, slot 2 empty, slots 32–39 all empty, and so on).
The table does **not** follow stock `LOP_*` order — the opcode byte
values in this build's bytecode are shuffled relative to public Luau.
That count match (93 slots = 93 opcodes) is observed; the shuffle itself
is the natural reading, and it means stock-Luau bytecode will not execute
here even where the VM logic is identical.

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

### Shuffle proven: six handlers identified by behavior

Table slots were identified by what their handler bodies do, checked
against Luau source semantics (tag constants match source exactly:
`3` = number, `5` = vector, `8` = function, `9` = userdata):

| Table slot | Handler | Behavior | Stock slot |
|---|---|---|---|
| 1 | `0x1029b3d0d` | copies argument TValues, grows the stack (`luaD_growstack`-shaped call), sets the new frame top — call setup | 21 |
| 3 | `0x1029b4ed7` | number plus constant-table number, slow-path fallback — ADDK (second operand addressed off the constant base, not the stack) | 39 |
| 9 | `0x1029b52ab` | patches the loop-counter field of its own instruction — FOR-loop prep | 56 |
| 12 | `0x1029b5485` | constant-exponent `x*x`, `sqrt(x)`, `x*x*x` fast paths, `_pow()` fallback — POWK | 44 |
| 14 | `0x1029b7909` | number divided by constant-table number, vector path, userdata-metatable fallback with tag-method `0xb` (DIV) — DIVK | 42 |
| 16 | `0x1029b58bf` | plain `_pow()` on two stack numbers — POW | 38 |
| 17 | `0x1029b5934` | number/number or string/string less-than, skip-on-false — JUMPIFLT | 29 |
| 18 | `0x1029b5863` | number register vs constant-table number compare, conditional skip with flag bit — JUMPXEQKN family | 79 |
| 20 | `0x1029b79a5` | number×number, vector paths, userdata-metatable fallback, slow path — MUL | 35 |
| 22 | `0x1029b361a` | function-tag check plus inline-cache compare before dispatch — cached call dispatch | 21-ish |
| 25 | `0x1029b5735` | `a - round(a/b)*b` on stack numbers — MOD | 37 |
| 28 | `0x1029b786d` | `floor(a/b)` on stack-over-constant numbers, vector paths, tag-method `0xc` (IDIV) fallback — IDIVK | 82 |
| 40 | `0x1029b367b` | two-word instruction with AUX feedback slot, seal-then-helper sequence — CALLFB | 87 |
| 46 | `0x1029b55c5` | safeenv + table + nil triple check, iterator setup — FORGPREP_NEXT | 61 |
| 52 | `0x1029b4105` | 3 instructions: advance one word, zero state change — no-op family (NOP/BREAK indistinguishable in production) | 0 |
| 55 | `0x1029b5607` | truthy test, truthy selects constant operand — ANDK | 47 |
| 56 | `0x1029b551d` | table and string fast paths, `__len` fallback, number result — LENGTH | 52 |
| 57 | `0x1029b598e` | hash/array sizes, table allocate, GC check — NEWTABLE | 53 |
| 58 | `0x1029b39cd` | string-keyed global lookup with cache check and slow-path patch — GETGLOBAL | 7 |
| 61 | `0x1029b56bf` | table + number + exactly-0.0 checks, iterator setup — FORGPREP_INEXT | 59 |
| 63 | `0x1029b54e1` | jump iff truthy (nil/false fall through) — JUMPIF | 25 |
| 65 | `0x1029b57a3` | register-register equality, tag-switched, no-jump-on-mismatch — JUMPIFEQ | 27 |
| 66 | `0x1029b5a1b` | unconditional constant-to-register copy across two instruction words — LOADKX | 66 |
| 69 | `0x1029b5688` | 4-instruction unconditional 16-bit jump, no interrupt check — JUMP | 23 |
| 71 | `0x1029b563b` | register truthy wins, else constant operand — ORK (pairs with slot 55 ANDK; slot 111 OR below confirms the plain shape) | 48 |
| 74 | `0x1029b6653` | method-name string check, inline-cache compare, slow lookup fallback — NAMECALL | 20 |
| 78 | `0x1029b3a82` | dispatches through a third table at `0x1072d74e0` indexed by builtin ID — FASTCALL | 68 |
| 79 | `0x1029b3b7c` | multi-value copy plus SIMD tag validation, top adjust — unidentified (return/vararg-list shaped) | — |
| 80 | `0x1029b66fb` | nil-or-false test, skip-or-fall-through — JUMPIFNOT | 26 |
| 83 | `0x1029b6314` | number negate (sign-flip via per-build masks), tag-method `0xf` (UNM) fallback — MINUS | 51 |
| 107 | `0x1029b5583` | writes nil tag to the destination slot, single-word — LOADNIL | 2 |
| 109 | `0x1029b6275` | upvalue-table source with closed-over dereference, 16-byte copy — GETUPVAL | 9 |
| 111 | `0x1029b67ec` | falsy first operand wins, else second (inverse of ANDK) — OR | 46 |
| 112 | `0x1029b692e` | triple number-tag check, step/limit/index setup, conditional first jump — FORNPREP | 56 |
| 114 | `0x1029b644a` | number and string less-or-equal fast paths, generic fallback, skip-on-true — JUMPIFLE | 28 |
| 115 | `0x1029b8231` | cached-closure reuse with capture loop (value/upvalue only, no by-ref) — DUPCLOSURE | 64 |
| 116 | `0x1029b4014` | builtin-ID dispatch via `0x1072d74e0` with frame args — FASTCALL family | 68-ish |
| 117 | `0x1029b69b7` | interrupt hook + GC step, then numeric step/limit/exit — FORNLOOP | 57 |
| 118 | `0x1029b8b9f` | 2 bytes: trap only — permanently invalid slot, not an opcode | — |
| 120 | `0x1029b411c` | table fast path plus full slow path with userdata/object handling (`luaV_gettable`) — GETTABLE | 13 |
| 125 | `0x1029b45da` | builtin-ID dispatch with constant-table args and arg-count 2 — FASTCALL2K-leaning | 75 |
| 127 | `0x1029b46b0` | per-tag method table (`global+0x960`, `0x48` stride) userdata call with marshaling — exact opcode unidentified, possibly engine-extended fast path | — |

Every identified slot differs from its stock position (exact stock:
CALL 21, ADD 33, MUL 35, POW 38, FORNPREP 56), so the shuffle is
established by behavior, not just by table sparsity. All handlers end by
jumping back to the central dispatch (`0x1029b3618` / `0x1029b360d`
region), confirming the single-loop structure.

## Unknowns

- Exact helper names behind VM start / resume calls.
- Interpreter loop bodies behind the `0x1072d9230` opcode table.
  Twin tables exist (`0x1072d8a30` single-step, `0x1072d9230` normal),
  each with 93 non-zero slots in identical positions but different
  handlers; the slot order does not match stock `LOP_*` order.
- Full library open list vs flag-gated subsets per build.

## A note on the address list

30 sampled offsets from the address list (memory, GC, table, string,
type, value, load, library, and state functions) were checked against
this build's function-start table: every one lands exactly on a function
entry. The list is a reliable *index* — the remaining work is confirming
what each entry does, as done above for `spawn`, `luau_execute`, and
the facet accessor. Names are still not a stable interface across builds.
