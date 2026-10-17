# Open items — RobloxPlayer (macOS) build f9247f9560044102

Companion to `research/README.md` (what is verified), `research/vm-research/*`
(the evidence), and `vm-repro/` (the reproduction). Checked boxes are done in
this repository; unchecked boxes are genuinely open.

## 1. Luau VM core

- [ ] **Path A interpreter** (`0x1029adfe9`) does not decompile cleanly
  ("backward normalization not implemented"); only the dispatch table and
  handler addresses are verified. Handler-by-handler assembly review remains.
- [ ] **Userdata `metatable`** read site — the VMValue4 scheme is consistent
  with the other fields, but the individual accessor was not isolated.
- [ ] **`CallInfo+0x28`** — read as `nresults`; whether it packs additional
  state is unconfirmed.
- [ ] **Alternate container branch** `FUN_1014b96b3` (taken when
  `DAT_10752c0a0 != 0`) — not analyzed.
- [x] **`FUN_1014bb465` integrity gate** — fully deobfuscated as SigGuard;
  mode chain, SHA-256 digest, table MAC, verdict mapping, and bit-30 clear
  are documented in `luau-load.md` §3.2 and reproduced in
  `vm-repro/src/sigguard.cpp`.
- [ ] **Roblox-added builtins** ids 54–59 (impls `0x10298f1cf`–`0x10298f3b3`)
  — names unidentified.
- [ ] **Type tag 5** (`LUA_TVECTOR`) semantics in this build — the tag is
  known; the exact handler set for vector arithmetic is not fully walked.
- [ ] **`+0xa18` phase-counter writer** — same state-machine family as
  `+0x12a0`, not individually confirmed.

## 2. ScriptContext layer

- [x] **Admission polarity** — corrected: predicate 0 = allowed, 1 = blocked;
  no predicate is fail-closed; `predicate+0x1a8` is a blocked set. The model
  now mirrors `FUN_10144ef5e` exactly and the second `DAT_10752c840` window.
- [x] **Require cross-context** — both directions implemented; the outer
  `FUN_101543e94` skip byte and the name-property bit-8 check are documented.
- [ ] **Identity getters** (vtable `+0x2c0`/`+0x2c8`) and the
  `DAT_10752bfe8` pair variant — not followed into implementing classes.
- [ ] **`require` write sites** — state reads verified; who moves a module
  3 → 1/2 in `sub_10158dd88` / `FUN_10158ec34` is not traced.
- [ ] **`ScriptContextResume` slot `[7]`** — argcount vs nresults inferred.
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
- [ ] `luau_load`-equivalent loader (Proto/Closure construction).
- [ ] Interpreter core (Path B first).
- [ ] Machine-code differential test for the AEAD primitives (the xxHash32
  test already executes the binary's own code; AEAD currently uses an
  independent model + RFC vectors).

## Notes

- Ghidra project: `tmp-rea-ghidra/ghidra-proj`, program `RobloxPlayer`,
  base `0x100000000`.
- Addresses are absolute VMAs. For `__TEXT`/`__DATA`,
  `VA = file offset + 0x100000000`; `__LINKEDIT` differs.
