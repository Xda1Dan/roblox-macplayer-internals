# luau_load / luau_loadsafe — bytecode loader, reverse engineered
# RobloxPlayer (macOS, build f9247f9560044102 / Luau 0.742)

> What this is: a first-hand decompilation of `LuaVMLoad` (`0x1029a9f36`) and
> `luau_load` (`0x1029aa05a`), cross-checked line-by-line against the
> open-source `luau-oss/VM/src/lvmload.cpp`. Every claim cites the binary
> address or the upstream source line it mirrors. Companion: `luau-vm.md`
> (pipeline + layouts) and `../vm-repro/` (reproduction tooling).

Binary: `MacPlayer-version-f9247f9560044102`, Mach-O x86-64, image base
`0x100000000`. All addresses below are absolute VMAs.

---

## 1. Two functions, two jobs

| Function | VA | Role |
|---|---|---|
| `LuaVMLoad` | `0x1029a9f36` | thin GC-managing wrapper (matches upstream `luau_load` public entry) |
| `luau_loadsafe` | `0x1029aa05a` | deserializer run under `luaD_rawrunprotected`; name verified via the project rename map (the notes' `luau_load`) |

`LuaVMLoad` signature (decompiled): `sub_1029a9f36(L, chunkname, data, size, env)` —
5 params, returns `undefined4` (status). `param_2` = chunkname (char*), `param_3` = data,
`param_4` = size, `param_5` = env (0 = current globals, else a stack index).

---

## 2. `LuaVMLoad` — the wrapper (`0x1029a9f36`)

Decompiled body maps 1:1 to upstream `luau_load` (lines 842–888 of `lvmload.cpp`):

```c
int LuaVMLoad(L, chunkname, data, size, env) {
    global = L->global;                         // lVar2 = *(L+0x18)
    savedThreshold = global->GCthreshold;        // uStack_38 = global[+0x08]
    if (savedThreshold <= global->totalbytes) {  // if (savedThreshold <= global[+0x10])
        luaC_step(L, 1);                        // FUN_102996c06(L,1)
        global = L->global;                     // re-read after step
        savedThreshold = global->GCthreshold;    // (re-snapshot, upstream does this too)
    }
    global->GCthreshold = -1;                   // suspend GC for the whole load
    // ... set up the LoadContext {strings, protos, chunkname, data, size, env, result=0}
    // ... (uStack_68..uStack_50)
    status = luaD_rawrunprotected(L, luau_loadsafe, &ctx);   // FUN_102995084(L, FUN_1029aa05a, &uStack_98)
    result = ctx.result;                               // uStack_4c
    if (status == 4) {                                 // LUA_ERRMEM
        FUN_102986d86(L, "not enough memory");         // pusherror
        result = 1;
    }
    free temp buffers: FUN_1029abcee(&uStack_80);      // free proto table
                   FUN_1029abd1a(&uStack_98);          // free string table
    global->GCthreshold = savedThreshold;               // restore
    return result;
}
```

Upstream deviations: **none functional.** The Roblox build keeps the GC-suspension trick
(`GCthreshold = -1` / `SIZE_MAX`) and the single pre-load `luaC_step`. This is stock Luau
behavior, not a Roblox addition. (The `LuaVMLoad` name in the project docs is the wrapper;
`luau_load` is the internal `loadsafe`.)

---

## 3. `luau_load` — the deserializer (`0x1029aa05a`)

Signature: `void FUN_1029aa05a(L, ctx)` where `ctx` (`param_2`) is the `LoadContext*`
passed to `luaD_rawrunprotected` — i.e. `L` = thread, and the args live in `ctx`
(`+0x30` = chunkname char*, `+0x38` = data byte*, `+0x40` = size, `+0x48` = env int).
`param_1` is saved as `lStack_148 = L`.

The flow, in the order the decompiler shows it:

### 3.1 Chunkname / version gate (lines 78–108 of decomp)
- `pcVar2 = ctx->chunkname` (`+0x30`), `pbVar42 = ctx->data` (`+0x38`), `uVar10 = ctx->size` (`+0x40`), `bVar20 = *data` (version byte).
- `bVar20 == 0` → the rest of the blob is an error message: `luaG_runerror`-shaped push
  `"%s%.*s"` (chunkid + the bytes). Returns (via `uVar25 = 1` → result 1). **Upstream
  `loadsafe` version==0 branch, identical.**
- `bVar20 == 100 || bVar20 - 0x0f > 0xf3` (i.e. version 100 = `LBC_VERSION_CLASSES`, or
  version ≥ 15) → version check. If `bVar20 < 4` (versions 0..3 handled above / 1..3 allowed
  as min), `typesversion = 0`; else read `typesversion = data[1]` and validate it is in
  `[1..3]` (else push `"%s: bytecode type version mismatch (expected [1..3], got %d)"` and
  jump to the error exit `LAB_1029aa2a1`).
  - `LBC_VERSION_MIN = 3`, `LBC_VERSION_MAX = 14`, `LBC_VERSION_CLASSES = 100` (matches
    upstream `lbytecode.h`).

### 3.2 Integrity gate — `FUN_1014bb465` ("SigGuard") (line 109)
```c
iVar7 = FUN_1014bb465(L, chunkname, data, size);   // 0x1014bb465
if (iVar7 != 1) {   // proceed only when the gate returns 0 (false)
    ... deserialize ...
} else {
    // gate returned 1 (true) → reject
    push "%s: bytecode corrupted";   // line 1185
    uVar25 = 1;
}
```
Call site (`0x1029aa242`): `CALL FUN_1014bb465; CMP EAX,1; JNZ <continue>`. Return 1 →
reject via `chunkid` (`0x10299f01f`) + `lua_pushfstringL` (`0x102986e1a`) with
`"%s: bytecode corrupted"` (literal at `0x106a89e7e`), `ctx->status = 1` (args+0x4c),
plain return (no `lua_error`).

The subsystem is **literally named `SigGuard`**: its once-initializer `FUN_105a4ee65`
(tail-called by the fence stubs `FUN_1014bb630` / `FUN_1014bb642`) runs
`FUN_103209804(&DAT_107a70110, "SigGuard")`, seeds a float `1.0f` at `+0x108`, zeroes the
`0x107a700a8..0x107a70110` state and registers `__cxa_atexit(FUN_1014bc0c4, ...)`. The
gate packs {chunkname, data, size, arg5} into a stack struct and calls the verifier
`FUN_1014bb64e(&DAT_107a700a8, &in, &out)` — the ctx pointer is passed but never
dereferenced (pure obfuscation plumbing).

Verifier `FUN_1014bb64e`, step by step (decompile + raw disassembly, machine-verified):

1. **Trivial pass:** size ≤ `0x117` or `data == NULL` or `data[0] == 0` → code **1**
   (accept; upstream-semantic: tiny blobs are error strings anyway).
2. **Mode detection** (`FUN_1014bc1a8`): reads the trailer's declared version via
   `FUN_1014bc7d4` (`trailer[0..4] ⊕ trailer[20..24] == C4 32 6A <ver>`) and the
   secondary magic via `FUN_1014bc832` (`u32[len-0x28] ⊕ u32[len-0x20] == 0x946AC432`);
   the trailer sits at −`0x20` (v0) / −`0x18` (v>0) from the end (`FUN_1014bc174`).
   Mapping, with SigGuard telemetry on the weak paths (fields `placeid`, `version`,
   `code`, `decrcc`, `declocal`; sink `PTR_PTR_1072db2e8`; codes 901/902/903):
   no trailer → **mode 0** (event 901); v0 trailer → mode 0 (event 902), but **forced
   to −1 when `DAT_10752be68 > 0`** (force-signed) → reject; v>0 → mode = v (event 903).
3. **Mode gates:** mode < 0 → code **3**; mode > `DAT_10752be38` (max supported) → **4**;
   mode ≥ 2 → **5**. Only modes 0/1 are processed further.
4. **Checkword:** `out[1] = u32[trailer+0] ⊕ u32[trailer + (mode==0 ? 0x10 : 4)]`
   (mode 0: words 0⊕4; mode 1: words 0⊕1). Mode 1 additionally requires
   `out[1] == 0x74171870` (code 9 otherwise).
5. **Threshold branch** (`CMP [0x10752be20],2; SETGE; INC` → threshold = (be20≥2)+1):
   mode < threshold → *simple path* = hex(trailer) stashed into the out-record string,
   code **6** (reject). mode ≥ threshold → *full path*. Since threshold ≥ 1 always,
   **mode 0 always takes the simple path** — the code-6 reject is deliberate:
   unauthenticated and legacy-v0 blobs must not load. The full path's mode-0 arm
   (`bc4aa`, called at `0x1014bb82a`) is therefore unreachable from the verifier
   (it remains referenced from the `0x107d527xx` table). Only mode 1 is ever fully
   verified; be20 ≥ 2 additionally rejects mode 1 (total-lockdown switch).
6. **Full path:** hex(`trailer[0..0x18)`) stashed into the out-record; requires payload ≥
   `max(0x100, min(0x400, DAT_10752be08))` else code 1. Then `FUN_102f486bc(data,
   payload_len, 0x20)` — SHA-256 digest of the payload **without the trailer**
   (the wrappers at `0x102f8fa5c/8fc08/9109d` are OpenSSL SHA-256),
   hex-encoded (64 chars, builder `FUN_102f476ef`). Mode 0 skips the `0x74171870` word
   check (`CMP dword[RBX],0; JLE` before it); mode 1 requires `out[1] == 0x74171870`,
   else code **9**.
7. **Verdicts:**
   - **Mode 0 → `FUN_1014bc4aa`:** derive 32 bytes
     `roll8(~(hex[i & 0x1f] ^ cw[i & 3]), rot(i))` (four rotation variants, switch table
     `0x1014bc5b8`) and `memcmp` against `trailer[8..40)` — the expected pointer is
     `trailer + (mode==0)*8`, computed from the saved `RDI = (mode==0)` (not from the ctx),
     length `min(0x20, DAT_10752bdf0)` → 0 (pass) or **7**.
   - **Mode 1 → `FUN_1014bc5c8`** (only when `DAT_10752be50 == 1`; otherwise the arm
     leaves code 0 — automatic pass, i.e. this flag is the kill-switch for the whole
     signature check):
     table MAC. Selector = `trailer[0..16)`. Each selector byte indexes one row in
     two `0x1000`-row tables (A/B), and set-bit pairs across selector positions
     index a third `0x3C000`-row table (C); the mixed 12-byte state is compared
     against `sha256hex(payload)[4..16)`. The packed `0x1830c`-byte blob at
     `0x106207760` is **8257** 12-byte records (header `47 59 4D 14`); it expands
     to `0x20410` bytes of 16-byte rows (T1), then A/B (`0x10000` bytes each) and
     C (`0x3C0000` bytes) are built from it (`FUN_1014bd3e4` →
     `bd60c/bd6aa/bd73a/bd81c`). → 0 or **7**. Construction: A rows are linear
     in the bits of one selector byte (basis records `T1[1+8i+j]`); B rows are
     within-byte pairwise terms; C rows are cross-position pairwise terms. The
     result is a fixed degree-2 function of the 16 selector bytes. Reproduction:
     `vm-repro/src/sigguard.cpp` (`loadPackedTables` / `expandTables` /
     `computeMac`).
8. **Code → verdict mapping** (opaque arithmetic over three tables):
   `pa9c=[9,9,1,0,2,5,2,0,5,2]@0x10621fa9c`,
   `paa6=[8,3,4,9,1,5,2,7,0,6]@0x10621faa6`,
   `fab0=[0x44FD,0x405E,0x945,0x4A6,0x4558,0x40B9,0x44B,0x40B9,0x4558,0x8EA]@0x10621fab0`;
   `uv5 = fab0[...]/7 − 0x9d`; the gate returns **1 (reject)** iff
   `⌊uv5/2197⌋ ≠ 13·⌊uv5/28561⌋`. Per code: **0,1,2 → accept; 3..9 → reject**.
   Codes 3,4,6,7,9 additionally clear bit 30 of `global+0x630` via
   `&= ~0x40000000` (the only `and dword[reg+0x630], 0xbfffffff` in the binary,
   at `0x1014bb542`); codes 5 and 8 do not. `global+0x630` is a 28-bit counter (low bits) plus four flag bits (28–31); the
   flag bits are compared across global_States (e.g. `FUN_101544a36`), so the clear is
   observable elsewhere in the process.
9. **Telemetry** `FUN_1014bbe64(ctx, code, bool)`: atomic counters in the SigGuard state
   (total, failures, per-code slots via pointer table `DAT_10621fae0`, valid codes =
   bits of `0x3fb`).

Config globals (`DAT_10752bdf0/be08/be20/be38/be50/be68`) are FFlag-backed, initialized
by per-global stubs (`FUN_1014bb3c4/3da/3f0/406/41c/439`) that call
`FUN_102f604b0(&global, 1)` / `FUN_102f604f8(name, &global, 1)` — each descriptor is
`{int value; const char* name;}` (name pointer at +8, resolved by `FUN_102f4d58a`).
Named so far: `DAT_10752be50` = **`LsbOptimizationDetail`** (default 1 = mode-1 MAC
verification ON; 0 auto-passes mode 1). The other five stubs pass no name string —
they are resolved through the obfuscated reference table at `0x107d527xx`; all six
default to 1. The stubs themselves are referenced from that same table, as are the
wrappers, the gate, and the mode-chain/verdict functions.

**Wrapper callers** `FUN_1014baaac` / `FUN_1014bac62` (driven by `FUN_1014b8f74` and an
obfuscated pointer table at `0x107d527xx`): check the start marker `0xE009325B4A1007A2`,
locate the trailer with `FUN_1014bc174`, then re-encrypt through `FUN_102f732f6`
(+0x10/0x18 extra bytes — the RSB1 container step). `FUN_1014bac62`'s malformed path
pushes `"Invalid Bytecode."` (literal `0x106b7b236`). Compiler-side, the strings
`Bytecode Auth Check missing or wrong version.` / `Invalid Bytecode.` are handed out by
obfuscated-string thunks (`FUN_102f6f63d(rva)`, e.g. `sub_1054c8245`) that have no direct
xrefs — they are referenced through the same tables.

Upstream has **no such gate** — this is a pure Roblox addition, and unlike an
opaque-predicate check it is cryptographically bound to the payload: forging a trailer
requires the table set baked into the binary.

### 3.3 Deserialization (lines 111–~1179)
When the gate passes, the body follows upstream `loadsafe` exactly:
- `env` resolution: `env == 0 ? L->gt : hvalue(luaA_toobject(L, env))` (line 111–116,
  `iVar26 = ctx->env` at `+0x48`).
- `source = luaS_new(L, chunkname)` (`FUN_1029a06d3`).
- String table: varint count → `luaM_newarray` → each string via `luaS_newlstr`.
- Userdata remapping table when `typesversion == 3` (line 153): fills `uStack_138..` with
  `0x707070707070707` (sentinel) and calls a type-mapping callback `*(global + 0x708)` if
  present — matches upstream `remapUserdataTypes` / `ecb.gettypemapping`.
- Proto loop (varint count), per proto:
  - version ≥ 12: varint `protoSize`, then seek to `protoStart + protoSize` at the end.
  - `maxstacksize`, `numparams`, `nups`, `is_vararg` (4 bytes).
  - version ≥ 4: `flags` byte + type info (v1 transform / v2 / v3 remap).
  - code array: varint `sizecode`, bulk copy u32 words; `p->codeentry = p->code`.
  - constants: varint `sizek`, init to nil, then per-constant switch (NIL/BOOLEAN/NUMBER/
    VECTOR/VECTORD/STRING/IMPORT/TABLE/TABLE_WITH_CONSTANTS/CLOSURE/CLASS_SHAPE/INTEGER).
    - `LBC_CONSTANT_IMPORT` calls `resolveImportSafe` (protected `luaV_getimport` with nil
      propagation) — matches upstream.
  - **Atom rewrite pass** (lines 194–183): walks the proto's code with
    `getOpLength(R[vmop])` steps; for `GETTABLEKS`/`SETTABLEKS`/`NAMECALL` (vmop 120/236/186)
    with `AUX < 0x10000`, looks up the string constant, calls
    `luaS_updateatom` (`*(global + 0x658)` hook), and if `atom >= 0` rewrites the opcode byte
    in place to `GETUDATAKS`/`SETUDATAKS`/`NAMECALLUDATA` (vmop 141/88/127). Matches upstream
    `luaS_updateatom` + the `SHRT_MIN` unassigned sentinel logic described in `luau-vm.md`.
  - child proto ids, `linedefined`, `debugname` (string id).
  - line info (gap-log2 + deltas + absolute intervals) when flag set.
  - debug info (locals, upvalue names) when flag set.
  - version ≥ 11: feedback vector slots.
  - version ≥ 12 + `LPF_INLINABLE`: cost (varint64).
- Main proto id → `luaF_newLclosure(L, 0, env, main)` pushed onto the stack.
- `luaC_threadbarrier(L)` before closure creation.

### 3.4 Wire → vmop rewrite — RESOLVED: it *is* in `luau_load`
The earlier decompiler pass hid it behind the register-forwarding quirk (functions that
forward RSI/RDX untouched decompile with no visible args); raw disassembly confirms the
rewrite runs per proto, immediately after the code words are bulk-copied and before the
constants are read:

- translation table `T` at `0x1063bb686` (wire → vmop, `vmop = T[wire]`),
  reverse table `R` at `0x10648bb74`;
- instruction-length helper `getOpLength` = `FUN_1029abbe3` (reused by the atom pass).

So the container decoder hands over **wire**-encoded opcodes and `luau_load` rewrites
them in place; nothing upstream of `LuaVMLoad` does it. (Pass order in `luau-vm.md`.)

### 3.5 Roblox deviations from upstream (observed)
| Upstream (`lvmload.cpp`) | This build |
|---|---|
| No integrity gate | `FUN_1014bb465` called on the raw LBC blob before deserialization; return 1 → reject with "bytecode corrupted" + clear bit 30 of `global+0x630`. |
| `luaC_checkGC` + `ScopedSetGCThreshold(SIZE_MAX)` | Identical (GC suspended during load). |
| `luaD_rawrunprotected` wrapping `loadsafe` | Identical. |
| OOM → push `LUA_MEMERRMSG` | Identical (status 4 path in the wrapper). |
| Atom rewrite via `luaS_updateatom` + `SHRT_MIN` sentinel | Identical shape; hook at `global+0x658`. |
| Type version 3 userdata remap via `ecb.gettypemapping` | Identical. |
| Version gates `[3..14]` / `[1..3]` / class version 100 | Identical constants. |
| `luaC_threadbarrier` before closure creation | Identical. |
| Wire→vmop rewrite | **Present** (in-place, per proto; see §3.4 — `T` @ `0x1063bb686`). |

### 3.6 Security / executor-relevant findings
1. **Single chokepoint, real crypto:** `FUN_1014bb465` is the only gate between raw bytes
   and a runnable closure, and it is cryptographically bound to the payload: mode 0
   verifies a rolled-XOR signature of `sha256(payload)` in the trailer, mode 1 a table-MAC
   of the same digest (§3.2). Feeding arbitrary bytecode requires forging a trailer (needs
   the table set baked into the binary) or patching the call site at `0x1029aa242` /
   the verdict mapping tables.
2. **No runtime re-validation:** once `luau_load` succeeds, the resulting closure is trusted;
   there is no per-execution bytecode check. The atom rewrite only swaps table ops for
   userdata variants based on `luaS_updateatom` — it does not re-authenticate.
3. **Version gates are the only structural checks** beyond the gate: version byte must be
   `3..14` or `100`, type version `1..3`. A loader feeding version 0 gets its payload
   treated as an error string (not executed) — a useful quirk for probing.
4. **`env` parameter:** `0` = current globals (`L->gt`), else a stack index. This is how
   restricted containers inject a different environment; the admission predicate
   (`sub_10159502e`) decides whether a script runs at all, but `luau_load` itself only
   consumes whatever `env` it is given.

---

## 4. Open follow-ups
- [x] Wire→vmop rewrite — **resolved:** it is in `luau_load`, in place, per proto (§3.4).
- [x] Deobfuscate `FUN_1014bb465` — **done:** full SigGuard pipeline in §3.2 (mode chain
  `bc1a8`/`bc7d4`/`bc832`/`bc174`, verdicts `bc4aa`/`bc5c8`, lazy-expanded table MAC,
  telemetry `bbe64`, module name "SigGuard" via `FUN_105a4ee65`).
- [x] Atom-rewrite vmop values cross-checked against `opcode-map.csv`
  (120→141, 236→88, 186→127); `luau-vm.md` had the 0xec/0xba targets swapped — fixed.
- [x] SigGuard config FFlags — partially resolved: `be50` = `LsbOptimizationDetail`
  (default 1); all six default to 1 via stubs `FUN_1014bb3c4/3da/3f0/406/41c/439`;
  the remaining flag names live behind the obfuscated table at `0x107d527xx`.
- [ ] Map `FUN_102985294` (env stack resolver), `FUN_1029abb5c` / `FUN_1029abba0`
  (string-table / proto registration helpers), `FUN_1029964dd` (luaF_newproto),
  `FUN_10299d387` (typeinfo buffer alloc) — to complete the field-level mapping.

---

## 5. Addresses (luau_load subsystem)
| Address | Function |
|---|---|
| `0x1029a9f36` | `LuaVMLoad` (GC wrapper) |
| `0x1029aa05a` | `luau_load` (deserializer / `loadsafe`) |
| `0x102995084` | `luaD_rawrunprotected` (protected boundary) |
| `0x102996c06` | `luaC_step` (GC) |
| `0x1029964dd` | `luaF_newproto` |
| `0x1029965b9` | `luaF_newLclosure` |
| `0x1029a06d3` | `luaS_newlstr` |
| `0x10299f01f` | `luaO_chunkid` |
| `0x102986e1a` | push-error (luaG_runerror-shaped) |
| `0x102985294` | env stack resolver |
| `0x1029abb5c` / `0x1029abba0` | string-table / proto registration helpers |
| `0x1029abcee` / `0x1029abd1a` | free temp buffers (proto table / string table) |
| `0x10299d387` | typeinfo buffer allocator |
| `0x1014bb465` | integrity gate (bytecode auth) |
| `0x1014bb64e` | verifier (mode chain + threshold + digest) |
| `0x1014bc1a8` / `0x1014bc7d4` / `0x1014bc832` / `0x1014bc174` | mode detect / trailer magics / trailer locator |
| `0x1014bc4aa` / `0x1014bc5c8` | mode-0 / mode-1 verdicts |
| `0x1014bd3e4` (→ `bd60c/bd6aa/bd73a/bd81c`) | SigGuard table expander: 8257 packed 12-byte records → T1 (`0x20410` bytes), A/B (`0x10000` each), C (`0x3C0000`) |
| `0x106207760` | packed signature tables (8257 × 12 B = `0x1830C`, header `47 59 4D 14`) |
| `0x102f486bc` / `0x102f476ef` | SHA-256 digest → hex / hex builder |
| `0x105a4ee65` | SigGuard once-init (registers the "SigGuard" singleton) |
| `0x1014bbe64` | telemetry counters |
| `0x1014bb3c4/3da/3f0/406/41c/439` | per-global FFlag init stubs (defaults = 1) |
| `0x102f604b0` / `0x102f604f8` / `0x102f4d58a` | FFlag setters (`be50` name: `LsbOptimizationDetail`) |
| `0x1029ec8ca` / `0x102d49b50` / `PTR_PTR_1072db2e8` | telemetry field add / event fire / sink |
| `0x107d527xx` | obfuscated reference table wiring the whole SigGuard cluster |
| `0x1014baaac` / `0x1014bac62` / `0x1014b8f74` | RSB1 re-encrypt wrappers / driver |
| `0x1014bb630/64e/8f0/902/b8f0/bbe64` | gate helpers |
| `0x10621fa9c` / `0x10621faa6` / `0x10621fab0` | gate lookup tables |
| `0x107a700a8` | gate state object (passed, never read) |
