# ScriptContext — resume pipeline, guards, startScript, require
# RobloxPlayer (macOS, build f9247f9560044102 / Luau 0.742)

> What this is: the ScriptContext layer that sits between the scheduler and the
> raw Luau VM — who may run, how a script is started, how resumes are guarded,
> and how modules are tracked. Every claim below was verified by decompiling
> the named function; correction notes mark places where earlier notes in this
> repo had the polarity or offset wrong.
>
> Companion: `../vm-repro/include/rbxvm/scriptctx.hpp` (C++ reproduction) and
> its tests.

---

## The resume pipeline

### `ScriptContextResume` (`sub_1015566d6` @ `0x1015566d6`)

Not a plain function — it operates on a ~10-slot task/args object (the
decompiler shows it as `param_1[N]` indexing). Reconstructed field map:

| Slot | Meaning |
|------|---------|
| `[0]` | pointer to the guard-counter struct `{int32 count; double start_time}` |
| `[1]` | ScriptContext object |
| `[2]` | pointer → `+0x18` → DataModel-ish → `+0x28` → … → `+0x100` = parallel-lane sub-object (state flag at `+0x515`); `+0x78` = hooks object with callback vtables at `+0x20` (done) and `+0x50` (other) |
| `[3]` | script object — vtable methods `+0x2c0`/`+0x2c8` (identity getters), `+0xb8` (GetName); name `std::string` at `+0x70`; VM ref at `+0x148` |
| `[4]` | profiler/job object (`+0x228` flag, `+0xd0` double time accumulator) |
| `[5]` | result out-struct `{int result; double time; int code}` |
| `[6]` | thread-wrapper object (`+0x28` = `lua_State*`) |
| `[7]` | u32 — argument/result count for the resume |
| `[8]` | char — "error resume" flag |
| `[9]` | error message (`std::string*`, Apple libc++ layout) |

Sequence:

1. Stats counter update: `*(*ctx+0x10) = (*(*ctx+0x20) − *(*ctx+0x18)) >> 6`.
2. **Identity save/restore** — only when *not* running on a parallel lane
   (`lane == 0 || lane->+0x515 == 0`) and `FUN_1015b0efc()` (a byte flag on the
   caller object) and the script is non-null: the VM object's identity string
   (`std::string` at VM+0x30, Apple SSO layout) is saved, cleared, replaced
   with the script's identity (vtable `+0x2c0`, or the `+0x2c8` pair-variant
   when `DAT_10752bfe8 == 1`), and restored after the resume. This is what
   makes error reports name the right script.
3. FLog: `[FLog::ScriptContext] Resuming script: %p`, `Pending script name: %s`.
4. Calls the **guard** (below) with `(ctx, L, argcount, is_error, errmsg,
   counter*)`; stores `{result, time}` into slot `[5]`.
5. Result handling — `hooks = *(*(slot[2]) + 0x78)`:
   - `result == 0` (done) → call `hooks->+0x20` vtable `+0x30`.
   - `result != 1 && result != 6` → call `hooks->+0x50` vtable `+0x30`.
6. If `DAT_10752c310 && DAT_107562320` and result ∉ {1, 6}: compares the raw
   VM status (`FUN_1029883b6(L)`) and on match calls `FUN_101558e38(ctx, L, 0)`
   (script-finished notification).
7. Restores the identity string, `FUN_1015a51d8(ctx+0x728, vm)`, profiler
   updates (`*(slot[4]+0xd0) += elapsed`), `*(slot[4]+200) = *(vm+8)`.

### The guard (`FUN_101556cb4` @ `0x101556cb4`)

```
int guard(ctx, L, argcount, is_error, errmsg, Counter* c):
    n = c->count
    if n > 200:                                     # re-entrancy limit (mirrors nCcalls)
        ok = luaD_checkstack(L, 1)                  # FUN_10298534b — see below
        if ok:  push_error(L, "C stack overflow");  return 2
        else:   return 5                            # cannot even report
    c->count = n + 1
    if shutdown_deadline_passed(ctx):               # FUN_1015c486e(ctx + 0x4b8)
        facet = facet_access(ctx)                   # fatal if phase >= 3
        (two pointer fields at facet+0xb8 / +0xc0 checked)
        ok = luaD_checkstack(L, 1)
        if ok:  push_error(L, "Not running script because past shutdown deadline"); r = 2
        else:   r = 5
    else:
        if c->count == 1: c->t0 = now()             # outermost resume: start clock
        if !is_error:
            r = lua_resume(L, 0, argcount)          # FUN_102995af5
        else:
            if errmsg: checkstack + push_error(L, errmsg)   # else fall through
            r = lua_resumeerror(L, 0)               # FUN_102995dc4
        if c->count == 1:                           # outermost: account time
            if not flag at *(*(*L->userdata?…) +0x228):
                ctx->+0x28 (double) += now() − t0
    c->count = n                                    # always restored
    return r
```

Corrections and specifics established here:

- **`ctx+0x28` is a double execution-time accumulator** (sum of wall time
  spent in outermost resumes), *not* "C-stack base/limit" as `structures.md`
  claimed.
- The **deadline sub-object lives at `ctx+0x4b8`**: `+0x38` = deadline double,
  `+0x40` bit0 = "shutdown started". Context-relative: `+0x4f0`/`+0x4f8`.
  (`structures.md`'s `+0x38/+0x40` were this sub-object's offsets.)
- The 200 limit is checked against the **task's counter struct** (`{int
  count; double t0}` at slot `[0]`), not `lua_State.nCcalls` directly — a
  ScriptContext-level mirror of the C-stack guard.
- `FUN_10298534b` = **`luaD_checkstack(L, n)`**: requires
  `(top − base)/16 + n < 8001` (0x1f41 slots), grows the stack through
  `luaD_rawrunprotected(L, grow)` on demand, and raises `ci->top` to cover the
  requested range. Returns 0 if the growth fails — the guard then returns 5
  ("error occurred but can't be reported") instead of 2.
- **Result codes** (raw VM statuses flow through the guard):
  `0` = LUA_OK (done) · `1` = LUA_YIELD · `2` = error with message on the
  stack · `5` = error without message · `6` = LUA_BREAK / error-with-payload
  in the wrapper mapping. The two execute wrappers
  (`sub_101556235` / `sub_1015562c1`, differing only in one telemetry call)
  map: `0 → 0`, `1 → 1`, `6 → (1, payload, code 6)`, anything else → `2`
  after a `FUN_101552990` notification.

### Facet gates — polarity correction

Both accessors have the **same** (previously mis-stated) polarity:

```c
long facet_access(ctx)        { if (*(int*)(ctx+0xa18)  < 3) return ctx - 0x888;  fatal("Invalid Facet Access"); }
long queue_access(lane)       { if (*(int*)(lane+0x12a0) < 3) return lane + 0xac0; fatal("Invalid Facet Access"); }
```

The facet sub-object (`ctx − 0x888`) and the scheduler queue (`lane + 0xac0`)
are valid **while the phase counter is below 3**; at `≥ 3` access is a fatal
error. The counters read as a shutdown phase/generation marker (writer not yet
identified — see Unknowns).

---

## `startScript` (`FUN_10158afe8` @ `0x10158afe8`)

Request struct: `{script, script_ptr2, shared_ref, extra}` (4 words) plus
trailing fields (`+0xc`, `+0x14`, `+0x20` — optional identity payload).

1. Take a ref on the request's shared member; `FUN_101594f1e` builds a copy.
2. **Policy gate** `sub_10159502e(ctx, request)` — false → return (nothing runs).
3. Script vtable `+0x320` (0x800/8) returns a bool — script-level run check.
4. **Re-entrancy counter** at `ctx+0x38` (u32): `> 3` → fatal
   `"startScript re-entrancy has exceeded 3"`; then increment (restored at the
   end from the saved copy).
5. FLog `Running script %p`; optional `Running Script "{}"` (name via vtable `+0xb8`).
6. Opens the `Script Start` scope (`sub_10159543e` / `FUN_101542a52`).
7. Registers the script in a set (`sub_101544972`), creates the execution
   record (`sub_10158cc58`), registers the script name in a map at `ctx+0x608`.
8. Creates the VM-thread ref (`FUN_1015f2110`) and stores it at `script+0x148`
   (previous value released).
9. Registers into `ctx+0x8a8`; `FUN_10298781a(L, LUA_GLOBALSINDEX(-10002), 1)`;
   optionally attaches a 0x60-byte identity object to `vm+0x78`.
10. Builds the **chunkname** `"=" + script name` (Luau `=`-prefixed chunkname)
    for the load step.
11. Two version-negotiation checks against global config singletons
    (`FUN_1004c9be8` / `FUN_100537686`): u16 schema version at `obj+0x1b8`,
    limit at `+0x1ba`; produces a flag byte (`FUN_1014d24d4` = "is RobloxScript
    context"). Flag 4 ORed in from a set membership check at `ctx+0x560`
    when `DAT_10752c220` is on.
12. **Load step** `sub_101593650(vm, L, chunkname, …)` (below):
    - returns 0 → success path: telemetry inserts into `ctx+0x560`,
      **`FUN_10155445e(script+0x150, &L)`** (binds thread to script),
      **`sub_1015a81a8(&out, ctx+0x208, &Lref, 0, 0)`** — scheduler enqueue
      (queue object at `ctx+0x208`, no deadline, no flags).
    - returns non-zero → failure path: `sub_101552b94(ctx−0x738, L, request)`
      error handling, `hooks->+0x50 vtable +0x30` if present, and
      `FUN_102985711(L, −2)` thread teardown.
13. Restores the re-entrancy counter; releases refs.

### The load step / compilation cache (`sub_101593650`)

A Lua-registry-backed cache, not a bytecode cache:

```
key  = FUN_101543e4c(script_ctx, chunkname)          # 24-byte key
table = registry["Compilation_Cache_Key_Address"]     # literal registry field name
type = rawget(table, key) via lua_type
  LUA_TNIL (0)      → miss: run the script's Bytecode property through the
                      RSB1 container decoder FUN_1014b91b5(L, chunkname, bytes, 0, flags)
                      → LuaVMLoad inside; on success store the loaded value
                      (as LUA_TUSERDATA) into the cache table
  LUA_TUSERDATA (8) → hit: reuse the cached module value
  anything else     → fatal "Unexpected type in script compilation cache."
```

Pseudo-index constants observed: `−10000` (registry), `−10002` (globals),
`−1/−2/−3` — the standard Luau ABI. Consequence: **this build executes
pre-compiled bytecode shipped in the `Bytecode` property (RSB1 containers);
source compilation is not part of this path.**

### The admission predicate (`sub_10159502e` + `FUN_1015c4980` + `FUN_10144ef5e`)

```
sub_10159502e(ctx, request):
  1. if script->+0x110 (RunContext enum) == 3:
         re-resolve the script through a service singleton (vtable +0x58);
         the resolved object's +0x1f0 becomes the "script" for the next step
  2. container check: normalize(script_owner) == normalize(ctx_owner) else DENY
  3. capability check FUN_1015c4980(ctx+0x488, request):
         perm sub-object at ctx+0x488; predicate object at +0x60;
         NO predicate  -> ALLOW (returns 1)
         predicate set -> FUN_10144ef5e(predicate, request)
  4. on deny: optional telemetry (sampled: rand()%10000 < DAT_10752c870;
     fields "scriptClass"/"script") and, if DAT_10752c858,
     log "Script "{}" was blocked from being ran under a restricted container."

FUN_10144ef5e(predicate, request):
  version gates against two config singletons (u16 version at +0x1b8,
  limit at +0x1ba: deny if (script_ver − cfg_ver) > cfg_limit)
  deny if script is a RobloxScript-class module (FUN_1014d24d4)
  otherwise ALLOW iff the script key is in the allowlist at predicate+0x1a8
```

So the "restricted container" story is: predicate object present → deny
RobloxScript-class scripts and anything not in the allowlist, subject to
schema-version gates. Scripts in unrestricted containers run with no
predicate installed.

---

## `require` / the module state machine (`sub_10158dd88` @ `0x10158dd88`)

Per-VM module state (tracked via `FUN_10158eb34`/`FUN_1014d19f8`), states:

| State | Meaning | Behavior on require |
|-------|---------|---------------------|
| 1 | ready | If the requiring script is part of a cycle (`FUN_1014d17fc`/`FUN_1014d1d36` over the require chain held weakly at `vm+0x88/+0x90`): allowed **only** for export-based modules (`FUN_1014d190e`); otherwise fatal `"Requested module was required recursively. To allow circular dependencies, all modules in the cycle must use `export` instead of `return`"`. Returns the module value. |
| 2 | error | fatal: sub-kind `FUN_1014d19ee` → `"Requested module experienced an error while loading"` or `"Module code did not return exactly one value"` |
| 3 | pending | `FUN_1014d18fa(state) == −1` → resolve the weak reference: its target must be type `LUA_TFUNCTION` (7) else fatal; call it; a `nil` result means `FUN_10158dae2` cleanup + fatal `"Requested module has been destroyed and can no longer be required"`; otherwise the value is returned. `!= −1` → value already materialized. |
| other | — | fatal `"Unknown module state: %d"` |

Additional rules verified in the entry path:

- **`.config` modules**: a module literally named `.config` (byte-compared
  `".con"`/`"nfig"`) cannot be required directly —
  `"cannot require '{}': .config modules are configuration scripts and cannot
  be required directly"` (gated by `DAT_10752c0a0`; telemetry via
  `DAT_10752c828`).
- **RobloxScript context isolation** (only when the requiring script is not
  itself RobloxScript-class per `FUN_101543e94`): requiring a RobloxScript
  module from a non-RobloxScript context — and vice versa — is a fatal error
  (`"Cannot require a non-RobloxScript module from a RobloxScript"` /
  `"Cannot require a RobloxScript module from a non RobloxScript context"`),
  gated by a name-property check (`FUN_1016fb2ae(name, 8)`).
- The requiring script's name is taken from the `std::string` at `script+0x70`
  (same field the resume FLog reads).

---

## Address map (ScriptContext layer)

| Address | Function |
|---------|----------|
| `0x1015566d6` | `ScriptContextResume` (task-object driven) |
| `0x101556cb4` | resume guard (200 limit, deadline, time accounting) |
| `0x101556235` / `0x1015562c1` | execute wrappers (result mapping) |
| `0x101556664` | facet accessor (`ctx − 0x888`, fatal at phase ≥ 3) |
| `0x1040d49da` | scheduler queue accessor (`lane + 0xac0`, same gate) |
| `0x1015566ae` | push-error-message helper |
| `0x1015c486e` | shutdown-deadline check (sub-object `ctx+0x4b8`) |
| `0x10298534b` | `luaD_checkstack(L, n)` (limit 8001 slots) |
| `0x10298877e` | `L->userdata` reader (Roblox VM context) |
| `0x10158afe8` | `startScript` |
| `0x10159502e` | policy gate (container match + capabilities) |
| `0x1015c4980` | capability dispatch (predicate at `ctx+0x488+0x60`) |
| `0x10144ef5e` | restricted-container predicate (version gates + allowlist) |
| `0x101593650` | load step / compilation cache (registry `Compilation_Cache_Key_Address`) |
| `0x1015a81a8` | scheduler enqueue (queue at `ctx+0x208`) |
| `0x10155445e` | bind thread to script (`script+0x150`) |
| `0x101552b94` | load-failure handler (`ctx − 0x738`) |
| `0x10158dd88` | `require` / module state machine |
| `0x10158eb34` / `0x1014d19f8` | module-state lookup / state read |
| `0x1014d190e` | export-based-cycle check |
| `0x10158dae2` | destroyed-module cleanup |

## Unknowns (kept honest)

- **Who writes the phase counters** `ctx+0xa18` / `lane+0x12a0` (the facet
  gates) — a shutdown-phase writer exists but was not located.
- The **identity getters** (vtable `+0x2c0`/`+0x2c8`) and the `DAT_10752bfe8`
  "new mode" pair variant were not followed into the implementing classes.
- `require`'s state transitions during a *run* (who moves a module 3 → 1/2)
  live in the truncated middle of `sub_10158dd88` and its helpers
  (`FUN_10158ec34`) — state *reads* are fully verified, the write sites are not.
- The exact meaning of slot `[7]` (argcount vs nresults) in
  `ScriptContextResume` is inferred from the `lua_resume(L, 0, n)` call shape.
