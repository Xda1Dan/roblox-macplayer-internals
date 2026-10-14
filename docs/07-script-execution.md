# Script execution, end to end

> What this is: the complete native path a Roblox script takes from
> “instance exists” to “Luau code is running, yielding, resuming, or dead”.
> If you only read one scripting doc, read this one.

```
Instance (Script / LocalScript / ModuleScript)
  │  ram:0x10142a7a6  admission predicate
  ▼
source (ProtectedString) or bytecode asset
  │  ram:0x101593650  compilation-cache boundary
  │  ram:0x10159502e  policy gate (restricted / malicious)
  ▼
execution record
  │  ram:0x10158cc58  init
  │  ram:0x10158ac46  start path (re-entrancy ≤ 3, "Script Start" scope)
  ▼
VM collection thread
  │  ram:0x1015b92cd  creates/owns lua_State, ram:0x10299fd5b lua_newstate
  ▼
Luau resume
  │  ram:0x1015566d6  ScriptContextResume (deadline, C-stack, timing)
  │  ram:0x101557e8c  async-callback entry (weak script check)
  ▼
yield / wait / complete / error
  │  ram:0x1014cf74a  task.wait (no OS sleep)
  │  ram:0x1015977c1  coroutine admission gate
  │  ram:0x10158dd88  require state machine
  ▼
WaitingHybridScriptsJob  ram:0x1015a8d94 / 0x1015a9249
  │  bounded drain, deadline, atomic resume flag
  ▼
teardown  ram:0x1015a7d1e → ram:0x1015bab74
```

---

## 1. Admission — `ram:0x10142a7a6`

**What it does:** answers “is this script allowed to start *here, now*?”
before any VM is touched. RunContext, capability bits, ancestry, DataModel
state, and plugin/core special cases all feed one boolean.

**Function card**

| | |
|---|---|
| entry | `ram:0x10142a7a6` |
| size | `0x3b9` bytes |
| callers | script-start path, plugin restart path |
| emits | `Cannot start {client,server} script '%s' (lacking capability …)` |
| confidence | high (predicate body inspected) |

**Step by step**

1. Load the script’s run-context field (`+0x110`). Values observed:
   `0 = legacy/generic`, `1 = server`, `2 = client`, `3 = plugin-ish`.
2. Fetch the native capability mask (`0x101540830`-family helper).
3. Server context requires the `RunServerScript` bit; client context
   requires `RunClientScript`. The failing helpers are tiny reporters —
   the decision lives here.
4. Even with the bit set, ancestry/container and DataModel state can veto:
   dynamic-cast walk over parents, identity/capability-holder checks,
   shutdown flags.
5. Plugin/core contexts take extra branches instead of a single bypass.

**Assembly pattern — capability dispatch prologue (excerpt, annotated)**

```asm
; rdi = script record
10142a7a6 PUSH RBP
10142a7af SUB RSP,0x38
10142a7b3 MOV RBX,RDI              ; keep script pointer
10142a7bd JNZ 0x10142a7e2          ; flag-gated fast path
10142a7bf CMP dword ptr [RBX+0x110],0x3
10142a7e2 MOV RDI,RBX
10142a7e5 CALL 0x101540830         ; -> capability mask in RAX
10142a7ea MOV R14,RAX
10142a7ed MOV EAX,[RBX+0x110]      ; run-context switch
10142a7f3 CMP EAX,0x1
10142a7f6 JZ  0x10142a821         ; server path
```

What to read here: one field load (`+0x110`), one helper call returning a
mask, then a small integer switch. That is the whole “level” system — there
is no Lua-visible integer you can just set higher.

**Simplified reconstruction (original, not engine source)**

```cpp
// Illustrative: the shape of the check, not the binary's code.
bool admitsScript(const ScriptRecord& s, Ctxt& c) {
  uint64_t caps = capabilitiesOf(c);
  switch (s.runContext) {
    case RunContext::Server: if (!(caps & kRunServer)) return deny("RunServerScript"); break;
    case RunContext::Client: if (!(caps & kRunClient)) return deny("RunClientScript"); break;
    case RunContext::Plugin: if (!pluginPathAllowed(s, c)) return false; break;
    default: break;
  }
  if (!ancestryOk(s)) return false;
  if (c.shuttingDown()) return false;
  return true;
}
```

---

## 2. Start — `ram:0x10158ac46`

**What it does:** turns an admitted script into a running VM thread:
log it, guard re-entrancy, open a profiling scope, build the VM-facing
objects, invoke the start helper, install result/error continuations.

**Function card**

| | |
|---|---|
| entry | `ram:0x10158ac46` |
| size | `0xc70` bytes |
| logs | `[FLog::ScriptContextAdd] LoaderFacet::addScript -- %s`, `Running script %p`, `Script Start` scope, `startScript re-entrancy has exceeded 3` |
| confidence | high (full body inspected) |

**Step by step**

1. Log the add. Copy the script/context records onto the stack with
   16-byte vector moves — this is why you see `MOVAPS` everywhere: these
   are small value-objects, not pointers.
2. Bump a shared ref on the context (`INC.LOCK [r+0x8]`). Everything from
   here on assumes shared ownership.
3. Resolve three sub-objects through the same idiom repeated three times:
   null-check → self-check → virtual call at `+0x10` (copy) or `+0x18`
   (in-place construct). This triplicated pattern is the visual fingerprint
   of this function.
4. Enforce `startDepth ≤ 3`, else fatal. Open the `Script Start` profile
   region.
5. Get/create the VM thread object, install thread state, call the VM
   start helper, then branch: success installs yield/completion
   continuation; failure publishes the script error.

**Assembly pattern — the triplicated resolver idiom (excerpt)**

```asm
10158acde MOV RDI,[RBP-0xa0]   ; candidate object
10158ace5 TEST RDI,RDI
10158ace8 JZ  0x10158acf7      ; null -> produce null
10158acea CMP RDI,RBX          ; self?
10158aced JZ  0x10158ad02      ; self -> construct in place
10158acef MOV RAX,[RDI]        ; vtable
10158acf2 CALL [RAX+0x10]      ; virtual copy
10158acf7 XOR EAX,EAX
10158acf9 MOV [RBP-0x160],RAX
; ... same shape repeats for [RBP-0x70] and [RBP-0x40] ...
10158ad8d MOV RDI,R15
10158ad90 CALL 0x10158afe8     ; re-entrancy + scope gate
```

Reading tip: `CALL [RAX+0x10]` vs `CALL [RAX+0x18]` is “copy” vs
“construct-in-place”. Once you see it, you can skim the whole function:
three resolves, one gate, one VM handoff, two continuations.

**Simplified reconstruction**

```cpp
void startScript(ScriptContext& ctx, ScriptRecord& s) {
  logAdd(s);
  auto held = ctx.shared_from_this();       // INC.LOCK
  auto a = resolveOrNull(s.slotA);
  auto b = resolveOrNull(s.slotB);
  auto c = resolveOrNull(s.slotC);
  if (!ctx.tryEnterScript())                // depth > 3 → fatal
    fatal("startScript re-entrancy has exceeded 3");
  auto scope = profile(ctx, "Script Start");
  auto thread = ctx.threadFor(s, a, b, c);
  int rc = ctx.invokeStart(thread, s);       // VM handoff
  if (rc == 0) ctx.onYieldOrComplete(thread);
  else ctx.onScriptError(thread, rc);
  ctx.leaveScript();
}
```

---

## 3. Resume — `ram:0x1015566d6` (`ScriptContextResume`)

**What it does:** the single funnel every resume passes through: refresh
metadata, check shutdown/deadline/C-stack, log the pending script, run the
VM, record timing.

**Function card**

| | |
|---|---|
| entry | `ram:0x1015566d6` |
| size | `0x814` bytes |
| logs | `Resuming script: %p`, `Pending script name: %s` |
| hard limits | C-stack depth `> 200` → `C stack overflow`; past shutdown deadline → refuse |
| confidence | high |

**Step by step**

1. Recompute a stack-budget field from the thread limits.
2. Optionally refresh execution metadata from DataModel/context.
3. Log resume + pending name (this is the log line to grep for when
   tracing “why did this script wake?”).
4. Read shutdown/deadline state; refuse with `Not running script because
   past shutdown deadline`.
5. Check C-stack depth; `> 200` → `C stack overflow` error path.
6. First-resume timestamp + execution-time accounting around the VM call.
7. Restore the depth counter and return the VM status code.

**Simplified reconstruction**

```cpp
int resumeScript(ResumeCtx& r) {
  r.recomputeBudget();
  r.maybeRefreshMetadata();
  log("Resuming script: %p", r.script);
  if (pastShutdownDeadline()) return err("past shutdown deadline");
  if (r.cstackDepth > 200) return err("C stack overflow");
  auto t0 = now();
  int st = vmResume(r.thread, r.args);   // the actual Luau re-entry
  r.recordTime(now() - t0);
  return st;
}
```

---

## 4. `task.wait` — `ram:0x1014cf74a`

**What it does:** yields the *current coroutine* until a deadline without
ever blocking the OS thread. This is the function that makes
`task.wait(t)` cooperative.

**Function card**

| | |
|---|---|
| entry | `ram:0x1014cf74a` |
| size | `0x246` bytes |
| rejects | `task.wait is not available for AuroraScripts`, `thread is not yieldable` |
| confidence | high |

**Step by step**

1. Reject AuroraScript contexts and non-yieldable threads up front.
2. Build the `task.wait(t)` call record and resolve the current thread.
3. Mark the coroutine waiting (`+0xac = 3`-shaped state write in the body).
4. Package wake-time + resume event into the ScriptContext waiting queue.
5. Return into the VM — the worker thread is immediately free.
6. Later, `WaitingHybridScriptsJob` pops the waiter and re-enters via
   `ScriptContextResume`.

**Why it matters:** if you model `task.wait` as `sleep()`, your
mental model of every Roblox scheduler/timing bug will be wrong. It is
`yield-until-deadline`, not `block`.

---

## 5. `require` — `ram:0x10158dd88`

**What it does:** stateful per-VM module cache with security checks,
cycle detection, destruction invalidation, and exactly-one-value enforcement.

**Function card**

| | |
|---|---|
| entry | `ram:0x10158dd88` |
| size | `0x80e` bytes |
| errors | RobloxScript↔non-RobloxScript mismatches, `.config` rejection, recursive-require diagnostic, `Requested module has been destroyed…`, `…experienced an error while loading`, `Module code did not return exactly one value`, `Unknown module state` |
| confidence | high |

**Step by step**

1. Check caller/module security contexts first — mismatches fail before
   any loading.
2. Reject `.config` modules from direct require.
3. Attach to the module’s per-VM state and branch on it:
   loaded → return cache; loading → cycle diagnostic (`export` vs
   `return`); destroyed → hard error; failed → load error.
4. Otherwise load, execute once, require exactly one return value, cache it.

```cpp
// Original illustration of the state machine.
Tuple require(Module& m, Vm& vm) {
  checkContexts(caller(), m);          // RobloxScript rules
  if (m.isConfig()) fail("cannot require .config directly");
  auto& st = vm.stateFor(m);
  switch (st.phase) {
    case Loaded: return st.value;
    case Loading: fail("required recursively; use export for cycles");
    case Destroyed: fail("has been destroyed and can no longer be required");
    case Failed: fail("experienced an error while loading");
    default: break;
  }
  Tuple v = loadAndRun(m, vm);          // exactly one value
  if (v.size() != 1) fail("did not return exactly one value");
  return st.cache(v);
}
```

---

## 6. Async callbacks + coroutine gate

**`ram:0x101557e8c` — what it does:** resolves the implementing script
through a *weak* reference first. Destroyed → `Script that implemented this
callback has been destroyed while calling async callback`. Alive → build the
`callAsyncCallback` scope, rebuild args/context, run the same VM machinery
as a start, release all shared refs on every exit path.

**`ram:0x1015977c1` — what it does:** admits only suspended/awaiting
coroutine states. Anything else →
`ModuleScript cannot resume non-suspended awaiting coroutine`. This is what
prevents double-resume of a running or finished coroutine.

---

## 7. Waiting drain + teardown

**Drain (`0x1015a8d94` / `0x1015a9249`) — what it does:** claim one atomic
“resuming” flag with a bounded spin, pop up to N ready waiters, stop at the
deadline, resume each, release each shared waiter, update scheduler
bookkeeping. The second body adds a context/queue predicate and can evict
via `0x1015a837c`.

**Teardown — what it does, in order:**

```
scheduler facet  ram:0x1015a7d1e
  YieldEvent->waiterCount → release resume claim →
  waitingThreads.size() → destroy queues/events → poison ~0x280 bytes
VM collection  ram:0x1015bab74
  per-slot thread counts → release state/allocators → poison ~0x5e0 bytes
DataModel close  ram:0x104542918
  per-service container clearing (StarterPack, StarterPlayer, Chat,
  Lighting, ReplicatedStorage/First, ServerStorage, …)
```

No raw VM, DataModel, or service pointer survives this boundary. Anything
that does is a use-after-free by construction.

---

## Unknowns (kept honest)

- Exact `lua_State` field offsets and helper names behind the VM calls.
- The `luau_execute` (`0x29adfd3`) body/call convention — entry confirmed,
  body not decompiled.
- `task.spawn` (`0x14cff67`) wrapper semantics — entry is a small
  task-library helper; caller trace still needed.
- Exact waiting-queue budget/deadline tunables per build.
