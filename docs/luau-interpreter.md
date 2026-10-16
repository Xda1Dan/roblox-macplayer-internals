# Inside the Luau interpreter

> What this is: a guided tour of the bytecode interpreter that actually runs
> every script in the player — `luau_execute`, the "Path B" interpreter loop.
> We follow one real script through the machine, instruction by instruction,
> and document the machinery it touches: the register file, the value format,
> call frames, method-call caches and the inline caches that make table
> access fast.
>
> Companion pages: [Script execution](script-execution.md) (how a script gets
> *to* the interpreter), [Luau VM encodings](luau-vm-encodings.md) (the
> opcode shuffle), [Luau VM lifecycle](luau-vm-lifecycle.md) (state
> creation/teardown).

---

## The 30-second version

Luau is a **register machine**. A compiled function ("proto") is a list of
32-bit instructions that move values between numbered slots of a stack frame
("registers"), call functions, and jump around. The interpreter is one giant
loop:

```text
fetch the opcode byte of the current instruction
jump into the handler for that opcode (a table of 256 function pointers)
the handler does the work, advances the instruction pointer,
  and jumps straight into the next handler — no call/return overhead
```

Everything else — calls, loops, closures, tables — is "just" handlers in that
loop. The rest of this page walks through them.

## Two interpreters, and which one runs

The binary contains **two complete copies** of the interpreter because Luau
compiles it twice from a C++ template: a debug variant (single-step, for
debuggers) and the normal one. `luau_execute` (`0x1029adfd3`) only picks:

```c
if (L->singlestep)   // lua_State +0x06
    run execute<true>;   // 0x1029adfe9, dispatch table 0x1072d8a30
else
    run execute<false>;  // 0x1029b359c, dispatch table 0x1072d9230
```

Everything below is the **normal** interpreter (Path B), which is what runs
99.99% of the time.

## The machine state

When the interpreter starts (or resumes after a call), it loads a fixed set of
registers from the current call frame:

```asm
; entry: RDI = lua_State* L
mov  rax, [rdi + 0x8]        ; ci  = L->ci          (current CallInfo)
mov  r10, [rcx + 0x28]       ; R10 = L->base        (the register file!)
mov  r11, [rax + 0x20]       ; R11 = ci->savedpc    (the instruction pointer)
mov  rcx, [rax + 0x10]       ; ci->func — the TValue holding the called function
mov  rax, [rax + 0x18]       ; ci->p — the running Proto
mov  rax, [rcx]              ; cl  = *(ci->func)    (the Closure object)
mov  rax, [rdx + 0x20]       ; k   = p->k           (the constant table)
movzx eax, byte [r11]        ; the opcode byte of the first instruction
lea  r15, [0x1072d9230]      ; the Path B dispatch table
jmp  [r15 + opbyte*8]        ; go
```

| Register | Holds | Meaning |
|---|---|---|
| `R10` | `L->base` | the register file of the current frame — register `N` lives at `base + N*16` |
| `R11` | `pc` | points at the *current* instruction word |
| `R15` | dispatch table | `0x1072d9230`, indexed by the raw opcode byte |
| stack | `cl`, `k`, `L` | the Closure, the constant table, and the `lua_State` |

A "register" is a **`TValue`** — 16 bytes, and this build lays it out as:

```text
offset  0        8        12   13      14
        +--------+--------+----+-------+----+
        | value  | (vec z)| tt | marked| mem|
        | 8 bytes|        |    |       |    |
        +--------+--------+----+-------+----+
```

- `value` is either a `double`, a heap pointer (`GCObject*`), or three
  floats — this build uses **3-component vectors**, so a vector value uses
  bytes 0–11 and the type tag sits at byte 12.
- The type tag is read and written as a **dword at +12** (type, then
  `marked`/`memcat` bytes). For example `LOADN` writes the number and tags it:

```asm
; LOADN A, D  →  registers[A] = (double)D
movsd [r10 + rcx], xmm0        ; the double, at offset 0
mov  dword [r10 + rcx + 0xc], 0x3   ; tt = LUA_TNUMBER
```

### A type-tag surprise

The type tags are **renumbered** in this build compared to open-source Luau.
Confirmed from handlers:

| Type | open-source | this build |
|---|---|---|
| `NIL` | 0 | 0 |
| `BOOLEAN` | 1 | 1 |
| `NUMBER` | 3 | 3 |
| `STRING` | 5 | **6** |
| `TABLE` | 6 | **7** |
| `FUNCTION` | 7 | **8** |

(low types match; the shift starts at `STRING`. The exact reason — likely a
new internal type inserted before it — is not confirmed yet.) Handlers also
mask tags with `& 0xf` before comparing, so the upper nibble of the tag byte
carries flags, not type.

## Dispatch

Every handler ends the same way — read the next opcode byte, jump:

```asm
movzx eax, byte [r11]          ; next instruction's opcode byte
lea  rax, [r15 + rax*8]
jmp  qword ptr [rax]           ; tail-jump into the next handler
```

Handlers are *not* functions — they're blocks of one huge function that
permanently live in the registers above. That's why a script running millions
of instructions never touches the C stack between them.

"Protected" actions (anything that can raise a Lua error or run the GC) take a
slower path: they save `pc` into `ci->savedpc` and re-enter the dispatch
through a shared helper, so the error handler can find where execution was.

## Following one script

Take the smallest interesting program:

```lua
print("hello", 1 + 2)
```

compiles to (canonical numbering, from our disassembler):

```text
0000  PREPVARARGS 0
0001  GETIMPORT R0 K1 ; import [K0 ; "print"]
0002  LOADK R1 K2 ; "hello"
0003  LOADN R2 3                  ; 1+2, folded by the compiler
0004  CALL R0 2 0
0005  RETURN R0 0
```

`PREPVARARGS 0` says the chunk takes no fixed arguments. `GETIMPORT R0 K1`
loads the global `print` — see [Imports](#getimport-cached-global-access).
`LOADK`/`LOADN` fill registers 1 and 2 with the arguments (`CALL`'s callee
convention: the function in `R0`, arguments right after it). `CALL R0 2 0`
calls with `B = 2` (one function + two args ⇒ `B-1 = 2` arguments) and
`C = 0` (`C-1 = -1` — discard all results... here `0` means "keep
nothing"). `RETURN R0 0` ends the chunk.

## GETIMPORT — cached global access

`GETIMPORT A D` references constant `D`, an "import" constant that packs up to
three string indices (`AUX`: length in the top 2 bits, then three 10-bit
constant indices — e.g. `game`, `GetService`, `Workspace` for
`game:GetService("Workspace")`).

The clever part: **the resolved value is cached in the constant table**.

```asm
; fast path — the constant is already a real value:
cmp  dword [k + D*16 + 0xc], 0     ; is the import constant still NIL?
jz   slow_path                     ; NIL = never resolved
mov  rdx, [cl + 0x10]              ; cl->env (the function's environment table)
cmp  byte [rdx + 7], 0             ; env->safeenv — only "safe" envs may cache
jz   slow_path
movdqu xmm0, [k + D*16]            ; copy the resolved 16-byte value...
movdqu [base + A*16], xmm0         ; ...into the target register
add  r11, 8                        ; pc += 2 (GETIMPORT is two words)
jmp  dispatch
```

First execution takes the slow path (a resolver call at `0x1029a9e6c` that
walks the import chain through the environment) and **writes the result into
`k[D]`** — every later execution is a single 16-byte copy. The `safeenv` check
guards against scripts swapping the environment to redirect imports.

## CALL — building a new frame

`CALL A B C`: the function is in register `A`, arguments in `A+1 .. A+B-1`
(`B = 0` means "everything up to top"), results go back into `A` onward.
The handler (`0x1029b4f33`) does, in order:

1. **Interrupt check** — if `global->cb.interrupt` is set, save `pc`,
   advance it past the `CALL`, and invoke the callback (the debugger/profiler
   hook); then *re-execute* the `CALL`.
2. **Locate the callee** — `RBX = &reg[A]`, and the argument end:
   `R15 = reg[A] + B*16` (or `L->top` for `B = 0`).
3. **Check the type** — the callee's tag must be `FUNCTION` (8 in this
   build). If not, a helper (`0x1029add42`) looks for the `__call`
   metamethod, rewrites register `A` with it, shifts the arguments by one,
   and the handler *retries*.
4. **Push a CallInfo** — the 48-byte frame record (stride `0x30`):

```asm
mov  [rax + 0x20], r11        ; caller's ci->savedpc = return address
mov  r14, [rdi + 0x8]         ; L->ci
cmp  r14, [rdi + 0x70]        ; ci array full? (end_ci)
jne  push
call 0x10299544e              ; grow the CallInfo array
push:
add  r14, 0x30                ; next CallInfo slot
mov  [rdi + 0x8], r14         ; L->ci = new frame
mov  [r14 + 0x18], rbx        ; ci->func   = &reg[A]
```

5. **Fill in the new frame** (Lua closure case):

```asm
mov  rax, [r12 + 0x18]        ; cl->p  (the Proto)
mov  ebx, c - 1               ; wanted results
lea  rcx, [r10 + r13 + 0x10]  ; &reg[A+1] — first argument
mov  [r14 + 0x10], rax        ; ci->p
mov  [r14],      rcx          ; ci->base  = arguments start
mov  eax, [r12 + 5]           ; cl->stacksize  (copied from the Proto)
shl  eax, 4
lea  rax, [rax + r15]         ; func + stacksize*16
mov  [r14 + 8], rax           ; ci->top
mov  qword [r14 + 0x20], 0    ; ci->savedpc = none yet
mov  dword [r14 + 0x2c], 0    ; ci->flags = 0
mov  [r14 + 0x28], rbx        ; ci->nresults
mov  [rdi + 0x28], rcx        ; L->base = the callee's registers
mov  [rdi + 0x20], r15        ; L->top   = one past the last argument
```

6. **Grow the stack if needed** — the callee's `stacksize` is decoded from
   the pointer-encoding scheme (see
   [encodings](luau-vm-encodings.md)): the field at `lua_State+0x40` is
   XOR-decoded with its own address, *in vivo*:

```asm
mov  ecx, [rdi + 0x40]        ; encoded stacksize
xor  ecx, [rbp - 0x7c]        ; ^ (uint32)&L->stacksize  → the real value
```

   If `stack_last - top` doesn't cover the new frame, a realloc helper
   (`0x10299516c`) grows the stack (typically doubling).
7. **Nil-fill the frame** — every register between `top` and
   `base + stacksize*16` gets `tt = NIL`, so the callee never sees stale
   values.
8. **Jump into the callee** — `pc = p->codeentry`, `k = p->k`, and the
   dispatch runs the callee's first instruction. No C call, no return — the
   "call" is just swapping the frame registers and continuing.

Vararg callees (`p->is_vararg`) skip the `top = ci->top` step so `GETVARARGS`
can later spread the extra arguments.

### C functions

If the callee is a C function (`cl->isC`), the handler simply does
`call rax` with the standard C convention (`L` in `rdi`), then:

- a negative return = error → leave the interpreter loop;
- results are moved down onto `func`, padded with `nil` to `nresults`,
  `L->ci` pops back (`ci - 0x30`), `L->base` restores from the caller's
  `ci->base`, and execution continues after the `CALL`.

### RETURN

`RETURN A B` (`0x1029b88f9`) is the mirror image: results from `reg[A]`
(`B = 0` means "up to `L->top`") are moved down onto `func`, the count is
padded/truncated to the caller's `nresults`, `L->ci` pops, and the
interpreter restores the **caller's** frame registers:

```asm
lea  rsi, [rax - 0x30]        ; the previous CallInfo
mov  [rdi + 8], rsi           ; L->ci = caller
mov  r10, [rsi]               ; R10 = caller's ci->base  (register file)
mov  r11, [rax - 0x10]        ; R11 = caller's savedpc   (resume point)
mov  rax, [rsi + 0x20]        ; caller's p->k
jmp  dispatch
```

One special bit: before popping, the handler tests `ci->flags & 1`
(`RETURN` bit). The *outermost* CallInfo — the one the VM was entered with —
has that bit set, and seeing it means the Lua program finished: the
interpreter loop exits back to `lua_resume`.

## NAMECALL — `obj:Method()` and its three caches

`obj:Method(args)` compiles to `NAMECALL A B C` + `AUX` (the method *name*
string). `A` receives the function, `A+1` receives `self`, then the call
proceeds as if `CALL A B+1` followed. The handler (`0x1029b4a5f`) is a
cache-finding machine with three levels:

1. **The table hash, with the inline slot cache.** The instruction's `C`
   field remembers the hash bucket where this method was found last time:

```asm
mov  rdx, [r15]               ; the method-name TString
lea  rdi, [rdx + 0x10]
add  edi, [rdx + 0x10]        ; hash = field_addr + stored  ← the encoded hash!
mov  cl,  [rax + 5]           ; table->lsizenode
mov  esi, 0xffffffff
shl  esi, cl
not  esi                      ; bucket mask
mov  rcx, [rax + 0x10]        ; table->node
shl  esi, 5                   ; * sizeof(LuaNode) = 32
cmp  dword [rcx + rsi + 0x1c], 6   ; node key tag == STRING?
```

   Note two things: the string's `hash` field is stored **encoded**
   (`real = field_addr + stored`, one of the four pointer encodings) and gets
   decoded right here in the hot path; and `LuaNode` is laid out
   **`val` first, `key` second** in this build (32-byte nodes).

2. **Key match → hit.** If the node's key pointer equals the name string and
   the value is non-nil, the handler copies `node.val` into `reg[A]` and the
   object into `reg[A+1]`, then jumps straight into the `CALL` continuation.

3. **Miss → resolver.** On a miss (or a nil value, or a metatable in the
   way) the handler consults the metatable chain and calls the namecall
   resolver (`0x1029a8a2e`, hooked through `global+0x398`), which writes the
   found function into the table's node so the *next* execution hits level 1.

## FASTCALL — builtins that skip the call machinery

`FASTCALL A C` (+ `FASTCALL1/2/2K/3` variants) is the fast path for common
library functions (`math.floor`, `table.insert`, `string.len`, ...). The
instruction's `A` is a **builtin id**, and `C` is the *distance to the CALL
that follows* the argument-setup instructions. The handler (`0x1029b4014`)
reads that future `CALL` word *now*, takes its operands apart, and calls the
builtin implementation directly — before the interpreter ever reaches it:

```asm
mov  r14d, [r11 + 3]            ; C — the skip distance
mov  r15d, [r11 + r14*4 + 4]    ; the CALL word, read ahead!
mov  ebx, (r15d >> 4) & 0xff0   ; the CALL's A = func/target register
mov  r9d, (r15d >> 16) & 0xff   ; the CALL's B = argument count + 1
...
lea  r10, [0x1072d74e0]         ; the builtin dispatch table
call qword [r10 + A*8]          ; builtin(L, funcTValue, arg0, arg1)
```

The builtin returns the number of results, or a negative value to *give up*:

- **Success**: the interpreter skips the argument-setup instructions *and* the
  `CALL` (`pc += C*4 + 4`) and fixes up `L->top` — the frame top for a fixed
  result count, or `target + results` when the `CALL` was multret (`C = 0`).
  The compare that decides this is elegant: the `CALL` word itself exceeds
  `0xFFFFFF` exactly when its result-count byte is non-zero.
- **Failure**: execution resumes at the instruction right after `FASTCALL` —
  the fallback setup (`GETIMPORT`/`MOVE`/`GETUPVAL`) runs, and the real
  `CALL` performs the call through the generic path. Nothing is lost.

Two gates apply: the current environment must be "safe" (`env->safeenv` —
fastcalls bypass metamethods, so a tampered environment disables them), and
the stack base is saved/restored around the builtin because a builtin may
reallocate the stack.

The builtin table at `0x1072d74e0` is a third dispatch table, indexed by the
`LuauBuiltinFunction` id baked into the instruction at compile time.

### The builtin id table

The ids match the reference Luau enum exactly (verified by compiling
one-liners with the reference compiler and reading the emitted `A` field).
The implementations live contiguously at `0x10298dd90–0x10298f3b3`:

| id | builtin | impl | id | builtin | impl |
|----|---------|------|----|---------|------|
| 1 | ASSERT | `0x10298dd90` | 25 | MATH_SQRT | `0x10298e516` |
| 2 | MATH_ABS | `0x10298ddbd` | 29 | BIT32_BAND | `0x10298e62a` |
| 9 | MATH_COS | `0x10298df9e` | 30 | BIT32_BNOT | `0x10298e6a6` |
| 11 | MATH_EXP | `0x10298e023` | 41 | STRING_BYTE | `0x10298eb3b` |
| 12 | MATH_FLOOR | `0x10298e069` | 43 | STRING_LEN | `0x10298ecba` |
| 13 | MATH_FMOD | `0x10298e0a2` | 44 | TYPEOF | `0x10298ecf5` |
| 17 | MATH_LOG | `0x10298e1f3` | 46 | MATH_CLAMP | `0x10298edcb` |
| 18/19 | MAX/MIN | `0x10298e29c`/`e319` | 49/50 | RAWSET/RAWGET | `0x10298eee7`/`efbd` |
| 21 | MATH_POW | `0x10298e3f8` | 52 | TABLE_INSERT | `0x10298f042` |

(the remaining ids follow the reference `LuauBuiltinFunction` order:
3–8 acos/asin/atan2/atan/ceil/cosh, 10 deg, 14 frexp, 16 log10, 20 modf,
22 rad, 23 sinh, 26 tanh, 27 tan, 28/31–39 the rest of bit32, 40 type,
42 char, 45 string.sub, 47 sign, 48 round, 51 rawequal, 53 table.unpack)

**Entries 54–59 exist in the binary but are past the reference enum** — they
are Roblox-added builtins (implementations at `0x10298f1cf`–`0x10298f3b3`).
Their names are not yet identified; the same compile-and-read method with
Roblox-only functions (`tick()`, `os.*`, buffer ops) will map them.

## Loops and the garbage collector

`FORNLOOP A D` (numeric `for`) keeps its three variables in registers
`A..A+2` — limit, step, index. The handler (`0x1029b69b7`):

```asm
movsd xmm0, [r10 + rcx]        ; limit
movsd xmm2, [r10 + rcx + 0x10] ; step
movsd xmm1, [r10 + rcx + 0x20] ; index
addsd xmm1, xmm2               ; index += step
movsd [r10 + rcx + 0x20], xmm1
mov  dword [r10 + rcx + 0x2c], 3    ; retag as NUMBER
; continue iff (step > 0 ? index <= limit : index >= limit),
; by jumping D instructions; otherwise fall through past the loop
```

Before any of that, a flag-gated block can call `luaC_step`
(`0x102996c06`) — **the garbage collector takes its slices at loop
backedges**, which is why long-running loops are where GC pauses appear.

## Closures and upvalues

`NEWCLOSURE A D` instantiates child proto `D` (from our fixture):

```text
0001  NEWCLOSURE R1 proto=0 ; proto#0 <counter>
0002  CAPTURE REF 0           ; capture register 0 by reference
0003  CLOSEUPVALS R0          ; close upvalues pointing at register 0
```

`CAPTURE` marks what the new closure captures (`VAL` = copy now, `REF` =
share the register box, `UPVAL` = copy from the parent's upvalue). `REF`
captures stay heap-boxed only when the register leaves scope — that's what
`CLOSEUPVALS` forces: it migrates any open upvalue pointing at register ≥ A
onto the heap so the value survives the frame.

## The inline caches, summarized

This build's interpreter aggressively caches into the instruction stream
itself:

| Instruction | Cached in | What |
|---|---|---|
| `GETTABLEKS` / `SETTABLEKS` | `C` field | hash-slot index of the last table accessed with this instruction |
| `NAMECALL` | `C` field | hash-slot index of the last method lookup |
| `GETIMPORT` | the constant table | the resolved import value itself |
| `DUPCLOSURE` | the constant table | the shared closure template |

The `GETTABLEKS` slow path shows the write-back explicitly — after a real
hash lookup succeeds, the discovered slot is stored back:

```asm
mov  ecx, [rbx + 0x10]        ; table->node
mov  edx, eax                 ; the found LuaNode*
sub  edx, ecx
shr  edx, 5                   ; (node - nodes) / 32 = slot index
mov  [r12 + 3], dl            ; write it into the instruction's C field
```

which is why two occurrences of the *same source line* in a proto carry
independent caches — each instruction site learns its own table.

## Addresses

| Address | What |
|---------|------|
| `0x1029adfd3` | `luau_execute` — Path A/B dispatcher |
| `0x1029b359c` | Path B entry: load frame registers, first dispatch |
| `0x1072d9230` | Path B dispatch table (256 × 8-byte handler pointers, indexed by vmop byte) |
| `0x1072d74e0` | builtin dispatch table (indexed by `FASTCALL`'s builtin id) |
| `0x1029b4f33` | `CALL` handler |
| `0x1029b88f9` | `RETURN` handler |
| `0x1029b4a5f` | `NAMECALL` handler |
| `0x1029b411c` | `GETTABLEKS` handler |
| `0x1029b3f8d` | `GETIMPORT` handler |
| `0x1029b69b7` | `FORNLOOP` handler |
| `0x1029a9e6c` | import resolver (slow path) |
| `0x1029a8a2e` | namecall resolver (slow path) |
| `0x1029add42` | `__call` metamethod dispatch |
| `0x10299544e` | CallInfo array growth |
| `0x10299516c` | stack growth (`luaD_reallocstack`) |
| `0x102996c06` | `luaC_step` (GC, at loop backedges) |
| `global+0x690` | interrupt callback (debugger/profiler hooks) |
| `global+0x6f0` | native-code entry callback (`ecb.enter`) |

## Evidence and limits

Every behavior above was read from the machine code of the named handlers
(disassembly, not guesses), and cross-checked against the reference Luau
sources where both exist. Values confirmed against the binary: the `TValue`
layout (via `LOADN`/`GETTABLEKS`), the `CallInfo` stride and fields (via
`CALL`/`RETURN`), the shifted type tags (via three independent handlers), the
pointer-encoded hash decode inside `NAMECALL`, and the inline-cache write-back
into instruction bytes.

Not yet covered handler-by-handler: the full `SETLIST`/`CONCAT`/string
builders, the vector arithmetic paths, `FASTCALL`'s builtin dispatch tables,
and the debug interpreter (Path A). The type that occupies tag 5 in this
build's enum (between `VECTOR` and `STRING`) is not yet identified.

## Do it yourself

Every claim above is reproducible with the tooling in `vm-repro/`
(see the repository root's `README` for the build):

```sh
# compile Lua with the version-matched reference compiler
luau-compile --binary script.lua > script.lbc

# disassemble with constants resolved (canonical numbering)
tools/rbxdis chunk script.lbc

# or run the full delivery pipeline: canonical → wire ops → RSB1 container
tools/rbxrsb1 encode --size0 --wire script.lbc -o script.container
tools/rbxrsb1 decode script.container -o script.wire.lbc
tools/rbxdis chunk --openc=wire script.wire.lbc   # identical output
```

`tools/rbxdis code --openc=vmop` accepts raw instruction words in the
interpreter's own numbering, which is what the handlers above consume.
