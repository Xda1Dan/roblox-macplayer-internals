# Network layer — RakNet datagram crypto & framing
# RobloxPlayer (macOS, build f9247f9560044102)

> What this is: the encryption and framing applied to RakNet UDP datagrams in
> this build, recovered from the receive path (`RakNet::RecvFromLoop` →
> `ProcessNetworkPacket` → reliability layer) and the send-side encryptors.
> Every claim is from decompilation/disassembly of the named functions.
>
> Status: **crypto layer complete and consistent**; datagram-header bit layout
> partially mapped; reliable-frame/split/ACK formats not yet recovered (see
> Unknowns). Companion doc: `../roblox-macplayer-internals/docs/replication-*.md`
> covers the layers ABOVE this one (message dispatch ≥ 0x81).

---

## Layering

```
UDP datagram
  │
  ├─ client → server: [plaintext header (AAD)][ciphertext][8B seq+magic][4B magic2][16B tag]
  └─ server → client: [ciphertext = header+payload][2B seq echo][16B tag]
  │                    (after decrypt: [DHF][reliable frames…] — everything is inside the ct)
  ▼
RakNet reliability layer (DHF parse, 45KB core FUN_102d84b6e @ 0x102d84b6e)
  ▼
message-ID dispatch (Replicator::OnReceive 0x10561fb20 — 0x15/0x16/0x1b/0x81/0x83/0x86…)
```

Receive chain: `RakNet::RecvFromLoop` (`0x102d606da`, thread name literal
`"RakNet::RecvFromLoop"`) → socket recv → packet queue →
`ProcessNetworkPacket` (`0x102d6df78`, resolves the connection record —
stride `0x14a8`, reliability layer at record+0x1450, skips `param_9` bytes of
Rupp header when the socket flag bit0 is set) → reliability core
`FUN_102d84b6e`.

---

## Datagram encryption — ChaCha20-Poly1305, custom framing (verified)

> Correction history: this section initially said "AES-256-GCM" — the
> GHASH-shaped call structure suggested it, but the keystream generator's
> state setup loads `"expand 32-byte k"` (ChaCha20) and the "GHASH" init uses
> 26-bit limbs (`0x3ffffff` — poly1305-donna-32). Construction details below
> are from the actual implementations.

### Decrypt — server→client format (`FUN_102d94458` @ `0x102d94458`)

Gate: `len ≥ 0x12`, `relObj+0x2b == 0`, `relObj+0x2a == 1` (encryption enabled).

```
wire:  [ ciphertext (len−18) ][ 2B sequence-low16 echo ][ 16B tag ]
nonce buffer (12B): [ window-reconstructed 64-bit sequence ][ u32 0x5265626d ]
key:   32B at relObj+0x40 or +0x60 — selected by (bit relObj+0x28 XOR 1) << 5
       (directional keys: decrypt uses the PEER's key slot)
```

- The echo reconstructs the full sequence: `(local_counter & ~0xFFFF) | echo`,
  then a ±0x10000 wrap window against `local_counter` (thresholds
  `0x8000`/`0x8001`); the counter at `relObj+0x18` is committed on success.
  The counter therefore tracks the SENDER's *transformed* sequence (see the
  send nonce below).
- Two cipher implementations are selected by bit1 of `relObj+0x2d`
  (`FUN_101825740` / `FUN_101850d74`); both dispatch into the same primitives.
- Verify-then-decrypt: MAC over the wire ciphertext first, constant-time tag
  compare, then keystream XOR (encrypt-then-MAC ✓).

### Encrypt — client→server format (`FUN_102d93dba`/`FUN_102d93dc3` @ `0x102d93dba`, `FUN_102d93f84` @ `0x102d93f84`)

```
wire:  [ plaintext header (AAD) ][ ciphertext ][ 8B (seq + 0x754E657571696E55) ]
                                              [ 4B 0x7265626d ][ 16B tag ]
```

- Trailer is exactly 28 bytes (`0x1c`; `FUN_102d93f84` checks
  `payload + 0x1c == total_len`). The 12 bytes before the tag ARE the nonce,
  shipped on the wire.
- The u64 constant `0x754E657571696E55` is the LE bytes `"UniqueNu"`, and the
  u32 `0x7265626d` is `"mber"` — the nonce block literally spells
  **"UniqueNumber"** with the sequence counter *added* to the first 8 bytes.
- Send sequence counter: atomic increment at `relObj+8` (LOCK/XADD), old value
  used.
- Variant `FUN_102d93dba`/`dc3`: the first `header_len` bytes stay **plaintext
  and are used as AAD**; only the payload after the header is encrypted. Key
  comes from a **key bank** at `*(relObj+0x20)` with three slots selected by
  `+0x28`/`+0x2c`: `+0xc0` (bit +0x28 set), else `+0xe0`/`+0x120` (bit
  `+0x2c`) — consistent with handshake → session → rekey phases. Gate:
  `relObj+0x29 == 1`.
- Variant `FUN_102d93f84`: no AAD; key from `relObj+0x40/0x60` by `bit+0x28`
  (own-direction key); gate `relObj+0x2b == 0 && relObj+0x2a == 1`.
- A `std::string`-returning wrapper (`FUN_102d94082` → `FUN_102d94160`) is
  called from the replicator region (`FUN_105638ac6`, gated on a member at
  `+0x3898`) — used for secondary encrypted blobs, not the datagram path.

### The AEAD construction (from decompilation of the primitives)

```
init(ctx64, nonce12, key32):                       # FUN_10183bdf2 → FUN_10183bed2
    state = "expand 32-byte k" | key32 |
            words12-13 = 0 (64-bit counter 0) |
            words14-15 = nonce12[0..8]             # ONLY the first 8 nonce bytes
    ctx = ChaCha20_block(state)                    # 64 bytes at counter 0
    poly_key = ctx[0..32];  (ctx[32..64] unused)
    wipe state

payload keystream:                                 # FUN_10183be17 → FUN_10183bfc7
    state = "expand 32-byte k" | key32 |
            word12 = counter & 0xFFFFFFFF (starts at 1!) |
            word13 = counter >> 32 |
            words14-15 = nonce12[0..8]             # ONLY the first 8 nonce bytes
    XOR len bytes from that counter onward

tag = Poly1305(poly_key,
               aad || pad16 || ciphertext || pad16 ||
               len_aad (u64 LE) || len_ct (u64 LE))  # custom: GCM-style length blocks
```

- ChaCha20 core: RFC 8439 layout (`FUN_10183c095` = double-round/XOR loop).
- Poly1305: **poly1305-donna-32** (`FUN_101835319` init — standard donna
  26-bit limb masks, `s`-key stored at ctx+0x50; `FUN_10183539d` update with
  16-byte buffering; `FUN_10183546a` final — append 0x01, zero-pad,
  branchless ≥p reduce, add `s`, wipe 0x90-byte ctx). Multiply core:
  `FUN_101835607`.
- The 4-byte nonce suffix ("mbeR" recv / "mber" send) is **not consumed by
  either primitive**: the counter-0 block and the payload keystream both read
  only nonce bytes 0..8 (words 14-15). It is carried in the 12-byte nonce
  buffer on the wire but does not affect the ciphertext or tag. Directional
  separation comes from the key slots, not from the suffix. (Earlier notes
  incorrectly said the suffix enters the Poly1305 key.)
- Interface dispatch: Poly1305 thunks read a table at `0x1074d2438`
  (+0x10/+0x18/+0x20); the cipher dispatcher reads `0x1074d2480` (+0x08 init,
  +0x18 keystream, +0x20 variant).

---

## Session setup — the Rupp handshake

`RakPeer::processRbxOpenRequest2` (RTTI name present) is implemented by
`FUN_102d765f2` (**RbxOpenRequest2**, wire marker `0x78`) and `FUN_102d77722`
(**RbxOpenReply2**, marker `0x7d`), both logging under
`[DFLog::SessionCrypto]`:

- `FUN_102d5322a(keypair)` returns the local public key (32 bytes written —
  X25519-shaped).
- `FUN_102d93bfe(peer_public)` / `FUN_102d93c10(...)` return an **encryption
  suite id**; the request logs `RbxOpenRequest2 using encryption {}` with
  `suite+1`.
- The packet carries: marker byte, a 16-byte constant (`DAT_106431950`, sits
  directly before the RakNet RTTI name block — protocol/version magic),
  flags, random 16-bit values (written via a stack guard-clobber helper when
  `DAT_1074f8398 == 0` — an RNG fallback), the 32-byte public key, optional
  platform string (`DAT_10755f6e8` SSO string), an optional token blob
  (`param_1[0x118]`), the RakNet GUID (u64) and `~u32`-complemented address
  fields (anti-spoofing: written inverted, verified by re-inverting).
- The receive side of the handshake lives behind
  `processRbxOpenRequest2(..., RBX::StringView, Rupp::DeserializationResult)`
  (RTTI) with `TokenTlv::Token` TLVs.

**Key derivation** (handshake → the ChaCha20-Poly1305 keys at `relObj+0x40/0x60`
and the key-bank slots) has not been traced end-to-end yet — the X25519
shared-secret → key schedule step is the missing link (see Unknowns).

---

## Datagram header (DHF) — partially mapped

`FUN_102d82612` = `DatagramHeaderFormat::ParseBitStream` (Roblox-extended
signature: `(out, BitStream*, f3, f4, bit21, bit23)`), called by the
reliability core with `f4 = bit21`, `f5 = bit23` of the reliability-layer
flags dword at `relObj+0xe78`, and `f6 = !(sign bit of relObj+0xe78 qword)`.

Bit-level flow verified from assembly:

```
bit 1 → out+0x15   (checked by the caller as "isValid"; 0 → fatal path
                    "[DFLog::NetworkTrace] Incoming packet error: dhf.isValid==false")
bit 2 → out+0x10   (control/data selector)
  bit2==1 (control/ACK-NAK path): bit→+0x12, bit→+0x16, align,
          [f3: u32 → +0x00], [if +0x12: u32 → +0x0c], [f6: u16 → +0x18]
  bit2==0 (data path):            bit→+0x11, [f4: bit→+0x1a], [f5: bit→+0x1b],
          bit→+0x13, bit→+0x14, align,
          [f3: u32 → +0x00], 24-bit sequence → +0x08 (FUN_102d82b92),
          [f6: u16 → +0x18]
```

Caller consumption (same struct): `+0x10==1` → ACK/NAK branch, else data;
`+0x11==1` → secondary data branch; `+0x13` feeds the congestion manager's
`OnGotPacket(seq, flag, time, len, &out)` (vtable `+0x68`); the sequence
(+0x08) drives reliability bookkeeping. The two extra 32-bit fields on the
control path (+0x00/+0x0c) and the u16 (+0x18) are ACK/NAK payload candidates
but not yet pinned to roles.

---

## Failure / telemetry behavior

- Decryption failure rate: the reliability core tracks failures
  (`+0x90`/`+0x110` counters) and, past a threshold (`DAT_10756bed0`),
  logs `"[DFLog::RakNetMissingPingPong] Decryption failure rate: {}, failed to
  decrypt last {} packets"`, sets disconnect code `0x12f` (303) and reports
  via `FUN_102d86db2(..., rate%, ...)`.
- Other receive-error disconnect code: `300` (connection-state failure), also
  `0x12f` on the short/invalid paths. Error paths log under
  `[DFLog::NetworkTrace]` with the exact condition in the message
  (`length <= 2 || buffer == 0`, `dhf.isValid==false`,
  `congestionManager->OnGotPacket failed`).

### Key management

The encrypt/decrypt paths read 32-byte session keys from two locations:

**Key bank** — a 0x140-byte crypto context object, pointed to by
`relObj+0x20`, with nine 32-byte slots at `+0x20..+0x140` (zeroed by
`FUN_102d93065` at init). The encrypt path reads from three slots
selected by rekey flags at `relObj+0x28/+0x2c`:

```
+0x28 == 0: encrypt uses key bank +0xc0
+0x28 == 1, +0x2c == 0: encrypt uses key bank +0xe0
+0x28 == 1, +0x2c == 1: encrypt uses key bank +0x120
```

The decrypt path uses a rekey-aware wrapper (`FUN_102d93ea4`) that sets
`+0x2c = 1` when the bit at `+0x28` is set — switching from slot `+0xe0`
to `+0x100` — before calling the AEAD primitive.

**Direct keys** — 32-byte slots at `relObj+0x40` (encrypt) and
`relObj+0x60` (decrypt), used by the simpler wrappers
(`FUN_102d93f84` / `FUN_102d94016` / `FUN_102d94458`) when
`relObj+0x2b == 0` and `relObj+0x2a == 1`.

**Population** — `FUN_102d5368e` populates the key bank from four
sources: either hardcoded 32-byte keys (when no config is present) or
config-sourced keys via four named FVars. The hardcoded keys are
embedded in the function (constants at stack offsets). A helper
(`FUN_102d53e28` → `FUN_10253e5e`) copies two `std::string`-held keys
into the first two key bank halves.

**Crypto library** — libsodium-derived:
- X25519 scalar mult: `FUN_101f499e0` (CRYPTOGAMS x86-64 assembly at
  `FUN_101f49a20`, zero-output small-order check via `FUN_101fd63a0`),
  wrapped by OpenSSL's `ossl_ecx_compute_key` (`FUN_101f6ba90`)
- BLAKE2b IV constants at `0x1062512f0`
- AEAD primitives: `FUN_101825740` (decrypt) / `FUN_10182539e` (encrypt)
  with GHASH-style MAC (poly1305-donna-32 core `FUN_101835607`) and
  ChaCha20 keystream (`FUN_10183c095`, constants "expand 32-byte k")
- Algorithm names: `"curve25519"` (`FUN_101838aea`), `"x25519blake2b"`
  (`FUN_10183512a`)
- Init: `"LibsodiumDRG"` nonce in `FUN_10183c75e`

**The X25519 → session-key derivation call path is not traced.** The
EVP interface dispatches through function pointers, leaving no direct
call sites in the stripped binary. The likely construction (standard
libsodium `crypto_kx` pattern: BLAKE2b keyed with the shared secret,
hashing both public keys concatenated, producing 64 bytes = two
32-byte directional keys) is consistent with the dual-key layout but
not confirmed from static analysis.

---

## Unknowns (next session's targets)

1. **DHF bit-exact semantics** — partially resolved. The parser
   (`FUN_102d82612`) and the sequence reader (`FUN_102d82b92`, a 24-bit
   big-endian read at a byte-aligned position) are mapped. The remaining
   ambiguity is the *semantics* of the control-path fields (`+0x00`,
   `+0x0c`, `+0x18`) and the data-path flags (`+0x11/+0x13/+0x14/+0x1a/
   +0x1b`) — the bit positions are known but which stock-RakNet field
   each maps to needs the send-side writer confirmed.
1b. **Key-derivation chain** — partially traced (see the key-management
   section below). The X25519 → session-key path goes through OpenSSL's
   EVP function-pointer dispatch, which doesn't leave direct call sites
   in a stripped binary. The key bank structure, key slots, rekey
   mechanism, and crypto library identification are documented.
1c. **Key derivation KDF** — the X25519 scalar mult is confirmed
   (`FUN_101f499e0` calling CRYPTOGAMS x86-64 assembly at
   `FUN_101f49a20`, with a zero-output small-order check via
   `FUN_101fd63a0`). BLAKE2b is present (IV constants at
   `0x1062512f0`). The exact KDF construction (likely BLAKE2b keyed
   with the shared secret, hashing the two public keys — the standard
   libsodium `crypto_kx` pattern) is not confirmed from static
   analysis alone.
2. **Reliable-message frame header** — per-message flags (reliable/sequenced/
   ordered/split), 24-bit sequence numbers, split-packet header (count/index)
   inside the 45KB core (`FUN_102d84b6e`, 45,851-byte decompilation — needs
   targeted slicing).
3. **ACK/NAK wire format** — the compact cumulated-ACK encoding.
4. **Key-derivation chain** — partially traced. The X25519 scalar
   multiplication exists in the binary through two paths: an OpenSSL
   EVP interface (`ossl_ecx_compute_key` in `FUN_101f6ba90`, reached
   through EVP function pointers) and CRYPTOGAMS x86-64 assembly
   (`FUN_101f49a20`). The crypto library is libsodium-derived
   (`BLAKE2b` constants at `0x1062512f0`, `curve25519` and
   `x25519blake2b` algorithm names). The key bank is a 0x140-byte
   crypto context with nine 32-byte slots (`+0x20..+0x140`),
   initialized by `FUN_102d93065` and populated by `FUN_102d5368e`
   from config-sourced keys or four hardcoded defaults. The encrypt
   path reads from three key-bank slots (`+0xc0/+0xe0/+0x120`),
   selected by the rekey flags at `relObj+0x28/+0x2c`. **The exact
   X25519 → session-key derivation call path is not traced** — it goes
   through the EVP function-pointer dispatch, which doesn't leave
   direct call sites in a stripped binary. Dynamic analysis (running
   the binary under a debugger) would close this gap.
5. **The `0x12`-byte Rupp pre-header** skipped in `ProcessNetworkPacket`
   (`param_9`) for reverse-endpoint sockets — layout not yet dumped.
6. **Rupp TLV schema** (`TokenTlv::Token`) for the open-request/reply payloads.

C++ reproduction status: the AEAD layer is implemented and tested in
`../vm-repro/src/aead.cpp` (ChaCha20 + poly1305-donna-32 + the custom
framing: 8-byte-nonce payload state, counter=1 payload / counter=0 poly key,
GCM-style length blocks in the MAC, both trailer formats, and the exact
sequence-window reconstruction). Verified with RFC 8439 primitive vectors and
a differential test against a spec-faithful bignum reference. The
machine-code-extraction differential test (as done for xxHash32) remains an
optional extra hardening step.

## Addresses

| Address | Function |
|---------|----------|
| `0x102d606da` | `RakNet::RecvFromLoop` (thread named literal) |
| `0x102d6df78` | `ProcessNetworkPacket` (connection resolve, Rupp skip, dispatch) |
| `0x102d84b6e` | reliability-layer core (45KB; decrypt → DHF → frames) |
| `0x102d94458` | datagram decrypt (server→client format) |
| `0x102d93dba` / `0x102d93dc3` | datagram encrypt, AAD=header (client→server) |
| `0x102d93f84` | datagram encrypt, no AAD |
| `0x102d94082` / `0x102d94160` | std::string encrypt wrapper (replicator blobs) |
| `0x102d533b6` | send-encrypt helper (packet buffer + trailer room) |
| `0x102d82612` | DHF parse |
| `0x102d82b92` | DHF sequence parse |
| `0x101825740` / `0x101850d74` | AEAD decrypt wrappers (impl select by `+0x2d` bit1) |
| `0x10182539e` | AEAD encrypt primitive |
| `0x10183bdf2`/`0x10183bed2` init · `0x10183515a/16c/17e` poly thunks · `0x10183c4bf` tag cmp · `0x10183be17`/`0x10183bfc7` keystream · `0x10183c095` core |
| `0x102d765f2` / `0x102d77722` | RbxOpenRequest2 / RbxOpenReply2 (Rupp handshake) |
| `0x102d5322a` | keypair public-key accessor (32B) |
| `0x1062494f0` / `0x106251ca0` | zero pad block / ChaCha constants ("expand 32-byte k") |
| `0x107d7a668` / `0x107d7a674` | vtable slots for the encryptors |
