# Open items — RobloxPlayer (macOS) build f9247f9560044102

Companion to `research/README.md` (what is verified), `research/vm-research/*`
(the evidence), and `vm-repro/` (the reproduction). Checked boxes are done in
this repository; unchecked boxes are genuinely open.

## 1. Luau VM core

- [ ] **Path A interpreter** (`0x1029adfe9`) does not decompile cleanly
  ("backward normalization not implemented"); only the dispatch table and
  handler addresses are verified. Handler-by-handler assembly review remains.
- [x] **Userdata `metatable` read site** — located: `Udata+0x08`,
  Add-encoded (`real = stored + (u+8)`); read per type in the builtin
  fast path `0x10298f40a` (tag 7 → `[t+0x20]`, tag 9 → the encoded field,
  else `global->mt[tag]`). Mirrored in `layouts.hpp` (`Udata_`).
- [x] **`CallInfo+0x28`** — resolved: a 32-bit `nresults` dword
  (`MOV dword [ci+0x28], ecx` with `ecx = ~(int)nargs >> 1`, the
  MULTRET convention), written by the call helper `0x1029adc1a`; no
  packing. Bonus: the same function re-confirms the `nCcalls` guard
  (`word [L+0x58]`, limit `0xc8` = 200).
- [x] **Alternate container branch** `FUN_1014b96b3` — resolved: a
  flag-gated (`DAT_10752c0a0`) *pure* string→string decode of the same
  RSB1 pipeline; never calls `LuaVMLoad`; returns `{payload, ok}`.
  Wrapped by `FUN_1014b969b`.
- [x] **`FUN_1014bb465` integrity gate** — fully deobfuscated as SigGuard;
  mode chain, SHA-256 digest, table MAC, verdict mapping, and bit-30 clear
  are documented in `luau-load.md` §3.2 and reproduced in
  `vm-repro/src/sigguard.cpp`.
- [x] **Roblox-added builtins 54–59** — closed as *not a thing*: the
  FASTCALL table (`0x1072d74e0`) has ids **1..196**; ids 1..132 are exactly
  the upstream 0.742 `LBF_*` set (each with a distinct impl in
  `0x10298dd90..0x102990f57`), and ids **133..196 all share one stub**
  (`0x102990fa7`, returns -1 → slow path). The old "ids 54–59" reading
  stopped reading the table too early.
- [ ] **Type tag 5** (`LUA_TVECTOR`) semantics in this build — the tag is
  known; the exact handler set for vector arithmetic is not fully walked.
- [ ] **`+0xa18` phase-counter writer** — same state-machine family as
  `+0x12a0`; the **lane** machine is proven (`sub_101545276`: 1→2→3 with a
  fatal if `>2` is observed mid-transition); the ScriptContext-side writer
  function itself was not individually named.

## 2. ScriptContext layer

- [x] **Admission polarity** — corrected: predicate 0 = allowed, 1 = blocked;
  no predicate is fail-closed; `predicate+0x1a8` is a blocked set. The model
  now mirrors `FUN_10144ef5e` exactly and the second `DAT_10752c840` window.
- [x] **Require cross-context** — both directions implemented; the outer
  `FUN_101543e94` skip byte and the name-property bit-8 check are documented.
- [x] **`require` write sites** — resolved (see `scriptcontext.md`):
  `FUN_101593956` funnels every post-run outcome into
  `FUN_1014d19c4` (state 2 + sub-kind) or `FUN_1014d19d6`/`FUN_101593d13`
  (state 3 + result slot); yields install a continuation at
  `*(identity+0x78)`. ModuleRecord layout recovered.
- [ ] **Identity getters** (vtable `+0x2c0`/`+0x2c8`) and the
  `DAT_10752bfe8` pair variant — not followed into implementing classes.
- [ ] **`ScriptContextResume` slot `[7]`** — used as the argument count of
  `lua_resume(L, 0, nargs)` (negative = MULTRET, seen in the call helper
  `0x1029adc1a`); whether anything else reads it as `nresults` unconfirmed.
- [ ] **ScriptContext construction call-site** — operator-new size and the
  facet-constructor caller still unmapped.
- [ ] **GC tunables application point** — `LuauGcGoal*`, `LuauGcStepMul`,
  `LuauGcStepSizeKb` values have no direct code refs.

## 3. Networking

- [ ] **DHF bit-exact semantics** — bit positions known; the send-side writer
  is needed to pin the control/data flag roles.
- [ ] **Key-derivation chain / KDF** — X25519 scalar mult confirmed; BLAKE2b
  present; the exact KDF (likely libsodium `crypto_kx`) is not confirmed.
- [ ] **Reliable-message frame header** — flags, 24-bit seq, split-packet
  header inside `FUN_102d84b6e` (needs targeted slicing).
- [ ] **ACK/NAK wire format** — compact cumulated-ACK encoding.
- [ ] **`0x12`-byte Rupp pre-header** — layout not dumped.
- [ ] **Rupp TLV schema** — `TokenTlv::Token` payloads.

## 4. Scheduler / replication / engine

- [ ] Waiting-queue budget/deadline tunables per build.
- [ ] Worker-thread entry / steal policy between shards.
- [ ] Arbiter priority formula/weights; per-class cycle budgets.
- [ ] Replication message payloads behind ids `0x81/0x83/0x86`.
- [ ] Inner replicator addresses (no whole-program call graph).
- [ ] Factory vs manager identity behind `&DAT_107510308`.
- [ ] ObjC bridge: service internals not deeply analyzed.

## 5. Reproduction gaps

- [x] SigGuard verifier + table expansion + MAC.
- [x] Binary audit tool (`make audit BINARY=...`).
- [x] LBC header-only opcode conversion (AUX words are never rewritten).
- [x] Disassembler operand semantics aligned with the reference
  `BytecodeBuilder::dumpInstruction`.
- [x] AEAD poly-key state corrected (8-byte nonce; suffix unused).
- [x] ScriptContext admission/require models corrected.
- [x] **`luau_load`-equivalent loader** (`src/loader.cpp`): version gates,
  per-proto wire→vmop rewrite, atom pass (hook injectable), line/debug/
  feedback fields, main-proto closure; round-trips canonical and wire
  inputs to identical VMOP code + disassembly.
- [ ] Interpreter core (Path B first).
- [x] **Machine-code differential test for the AEAD's ChaCha core** — the
  binary's own `FUN_10183c095` is extracted (code + 4 rip-relative shuffle
  constants + a 6-entry rip-relative patch table) into
  `tests/fixtures/chacha_core.bin` and executed in
  `tests/test_chacha_binary.cpp`; it matches the reproduction across a
  key/nonce/length sweep, across calls (counter continuity), and in-place.
  Pinned contract: `core(state, a, b, len)` writes `b = a ^ ks(state)` and
  advances the state's 64-bit counter by `ceil(len/64)`.
- [ ] Poly1305 machine-code differential test (same trick; primitives are
  `FUN_101835319/39d/46a/607` — more pieces, same pattern).
- [ ] IMPORT constant evaluation against a synthetic environment (the
  loader keeps import ids; resolution needs the embedder's `_G`).

## Notes

- Ghidra project: `tmp-rea-ghidra/ghidra-proj`, program `RobloxPlayer`,
  base `0x100000000`.
- Addresses are absolute VMAs. For `__TEXT`/`__DATA`,
  `VA = file offset + 0x100000000`; `__LINKEDIT` differs.
