// aead.hpp — the RakNet datagram AEAD of RobloxPlayer (macOS) build
// f9247f9560044102: ChaCha20 keystream + poly1305-donna-32 MAC with the
// binary's custom framing.
//
// Construction (verified against the binary's primitives; see
// ../research/vm-research/networking.md for addresses):
//
//   init:    state = "expand 32-byte k" | key32 |
//                    words12-13 = 0 (64-bit counter) |
//                    words14-15 = nonce[0..8]
//            poly_key = ChaCha20_block(state)[0..32]
//   payload: state = "expand 32-byte k" | key32 |
//                    word12 = counter & 0xFFFFFFFF (starts at 1) |
//                    word13 = counter >> 32 |
//                    words14-15 = nonce[0..8]
//            keystream = ChaCha20 blocks from that counter
//   mac:     Poly1305(poly_key, aad||pad16||ct||pad16||len_aad(u64le)||len_ct(u64le))
//
// Wire integration (RakNet layer):
//   server→client: [ct][2B seq-low16 echo][16B tag]     (18B trailer)
//                  nonce buffer = [reconstructed seq u64][0x5265626d]
//   client→server: [header = AAD][ct][8B seq+0x754E657571696E55][4B 0x7265626d][16B tag]
//                  (28B trailer; the 12B before the tag are the nonce, on the wire)
//
// IMPORTANT: only nonce bytes 0..8 are read by the primitives. The 4-byte
// suffix ("mbeR"/"mber") is carried in the nonce buffer but never reaches the
// ChaCha state (both the counter-0 block and the payload blocks take an
// 8-byte nonce). It therefore does NOT affect the ciphertext or the tag; the
// per-direction separation comes from the key slots, not from the suffix.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rbxvm {
namespace aead {

// The "UniqueNumber" nonce constants.
constexpr uint64_t kSendNonceAdd = 0x754E657571696E55ull; // LE "UniqueNu" (u64 added to seq)
constexpr uint32_t kSendNonceSuffix = 0x7265626du;        // LE "mber"
constexpr uint32_t kRecvNonceSuffix = 0x5265626du;        // LE "mbeR"

constexpr size_t kKeySize = 32;
constexpr size_t kNonceSize = 12;
constexpr size_t kTagSize = 16;

// --- nonce builders -------------------------------------------------------
// recv: [seq u64 LE][0x5265626d]        send: [(seq + kSendNonceAdd) u64 LE][0x7265626d]
void makeRecvNonce(uint8_t out[12], uint64_t seq);
void makeSendNonce(uint8_t out[12], uint64_t seq);

// --- primitives -----------------------------------------------------------
// RFC 8439 block: state = consts | key | counter | nonce12 (words 13-15).
void chacha20Block(uint8_t out[64], const uint8_t key[32], uint32_t counter,
                   const uint8_t nonce[12]);

// RakNet payload keystream: state = consts | key | counter lo | counter hi |
// nonce[0..8]; XORs len bytes into dst (dst may alias src).
void keystream(uint8_t* dst, const uint8_t* src, size_t len, const uint8_t key[32],
               uint64_t counterStart, const uint8_t nonce12[12]);

// poly1305-donna-32 over an arbitrary message (no length blocks).
void poly1305(uint8_t tag[16], const uint8_t key[32], const uint8_t* msg, size_t len);

// --- AEAD (the wrapper-level construction) --------------------------------
// tag = Poly1305(poly_key, aad||pad||ct||pad||len_aad||len_ct); ct = pt XOR keystream.
void aeadEncrypt(uint8_t* dst, uint8_t tag[16], const uint8_t* pt, size_t ptlen,
                 const uint8_t* aad, size_t aadlen, const uint8_t nonce12[12],
                 const uint8_t key[32]);

// Returns true and writes pt to dst iff the tag matches (constant-time compare).
bool aeadDecrypt(uint8_t* dst, const uint8_t* ct, size_t ctlen, const uint8_t tag[16],
                 const uint8_t* aad, size_t aadlen, const uint8_t nonce12[12],
                 const uint8_t key[32]);

// --- receive-side sequence reconstruction (FUN_102d94458's window logic) ---
// candidate = (local & ~0xFFFF) | echo, then:
//   cand < local  and (local-cand) >= 0x8001 -> candidate += 0x10000 (and counter)
//   cand >= local and (cand-local)  <  0x8001 -> keep candidate, commit counter
//   cand >= local and (cand-local) >= 0x8001  -> candidate -= 0x10000 (counter unchanged)
// Returns the candidate; `local` is updated only when the counter commits
// (the real code commits after successful decryption — pass the result on).
uint64_t reconstructSequence(uint64_t& local, uint16_t echo);

} // namespace aead
} // namespace rbxvm
