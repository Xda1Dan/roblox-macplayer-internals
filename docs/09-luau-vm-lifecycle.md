# Luau VM lifecycle in this player

> What this is: how a `lua_State` comes into existence, what Roblox puts
> around stock Luau, how code runs protected, and where strings get
> deliberately hard to xref.

```
allocator callback + context
  |  ram:0x10299fd5b  lua_newstate (alloc 0x4d60, init, protected init)
  v
ScriptContext VM wrapper
  |  ram:0x1015b92cd  caller: hooks + libraries + _G + sandbox + retain
  v
compile / load boundary
  |  ram:0x1029a9f36  loadsafe-shaped protected load
  |  bytecode auth + "Invalid Bytecode." guards
  v
protected execution
  |  ram:0x10298b913  luaB_pcall wrapper
  |  ram:0x102995084  luaD_rawrunprotected boundary
  |  ram:0x102995125  luaD_throw (C++ exception in this build)
  |  ram:0x1029940f4  luaG_runerror
  |  ram:0x1029adfd3  luau_execute (entry confirmed, body not decompiled)
  v
selective pointer hardening (string xref obfuscation)
```

---

## 1. lua_newstate - ram:0x10299fd5b

**What it does:** allocates a `0x4d60`-byte Luau state through the
caller-supplied allocator, lays out global-state / main-thread / stack / GC
fields, then runs protected init. Null means the caller reports
`Failed to create Lua state`.

**Function card**

| | |
|---|---|
| entry | ram:0x10299fd5b |
| size | 0x4a8 bytes |
| args | (allocatorFn, allocatorCtx) - RDI = fn, RSI = ctx |
| confidence | high |

**Step by step**

1. `CALL R15` with `(ctx, NULL, 0, 0x4d60)` - the allocator is the first
   argument. Roblox owns allocation policy; Luau just calls back.
2. Branch on a global flag: two init layouts (fast vs full zeroing). Both
   write the same logical fields: type tag `0x90a`, thread byte `0x9`,
   allocator fn/ctx at `+0x5c/+0x60`, list links, stack caps `0x4d60`.
3. `bzero` large ranges - a fresh state has no stale stack / GC / upvalue
   data by construction.
4. Protected init call; nonzero means free state and return null.

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

## 2. Wrapper - ram:0x1015b92cd

**What it does:** turns a raw state into a Roblox VM: install panic /
interrupt / error hooks, open the restricted standard libraries, register
Roblox bindings and globals, establish `_G`, attach sandbox and config
handlers, retain in the VM collection. Failure at any point unwinds;
there is never a half-owned state.

The offset header's `luaopen_base / math / os` entries point into this
registration sequence - but "opens base" does not mean stock unrestricted
base. Policy, identity, and capabilities are applied around the VM before
any script runs, and scripts get threads from the collection, not fresh
states.

---

## 3. Protected execution (pcall question, settled)

**Finding:** the public names `lua_pcall` / `lua_pcallk` are absent from
the shipped symbol and string inventory. The machinery is present:

| ram: | role |
|---|---|
| 0x10298b913 | luaB_pcall builtin: frame prep, protected call, status convert |
| 0x10298b946 | luaB_pcallrun stack-result continuation |
| 0x102995084 | luaD_rawrunprotected boundary |
| 0x102995125 | luaD_throw via C++ exception |
| 0x1029940f4 | luaG_runerror build and propagate |
| 0x1029adfd3 | luau_execute entry confirmed; body not decompiled |

So: name stripped, implementation present. Engine paths also use their own
ScriptContext protected helpers rather than needing a public `lua_pcall`.

---

## 4. Split pointers (why some xrefs miss)

**What it does:** selected sites replace `lea reg,[rip+string]` with
`lea dummy + mov imm(delta) + add`, where `dummy` is an unrelated address
already in `__TEXT` and `real = dummy + delta`. Naive xref scanners that
only match RIP-relative references to `__cstring` miss these.

**Verified example - ram:0x102988254 (Luau error path)**

```asm
lea    rax, [rip+...]           ; dummy = 0x106c9c778 (__unwind_info)
mov    [rbp-0x18], rax
mov    rsi, 0xffffffffffd06c00  ; delta = -0x2f9400
add    rsi, [rbp-0x18]          ; rsi = 0x1069a3378
call   ram:0x1029940f4          ; luaG_runerror-shaped
; [0x1069a3378] = "stack limit" (__cstring)
```

```text
0x106c9c778 + (-0x2f9400) = 0x1069a3378
```

Selective hardening, not a universal compiler rule: direct RIP-relative
string references still exist elsewhere.

## Unknowns

- Exact helper names behind VM start / resume calls.
- `luau_execute` body and calling convention.
- Full library open list vs flag-gated subsets per build.
