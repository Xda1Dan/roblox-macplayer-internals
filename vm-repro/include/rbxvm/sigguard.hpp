// sigguard.hpp — reproduction of the RobloxPlayer (macOS) bytecode
// authentication gate ("SigGuard", FUN_1014bb465 @ 0x1014bb465).
//
// The gate runs inside luau_load (call site 0x1029aa242) on the already
// decoded LBC blob. It is the only check between bytes and a runnable
// closure. The algorithm below is transcribed from the decompilation of:
//
//   FUN_1014bb64e   verifier: mode chain, checkword, payload threshold,
//                   SHA-256 digest, mode-1 table MAC, verdict codes
//   FUN_1014bc1a8   mode detection (telemetry codes 901/902/903)
//   FUN_1014bc7d4   v>0 trailer magic: (t[-0x18]^t[-4], t[-0x17]^t[-3]) ==
//                   C432, (t[-0x16]^t[-2]) == 6A, version = t[-0x15]^t[-1]
//   FUN_1014bc832   v0 secondary magic: u32(t-0x20) ^ u32(t-0x28) == 946AC432
//   FUN_1014bc174   trailer location: -0x20 (v0) / -0x18 (v>0)
//   FUN_1014bc4aa   mode-0 verdict (unreachable in this build: mode 0 is
//                   always rejected by the threshold check)
//   FUN_1014bc5c8   mode-1 verdict (table MAC over the 16 trailer bytes,
//                   compared with sha256_hex(payload)[4..16))
//   FUN_1014bd3e4/60c/6aa/73a/81c  lazy table expansion
//   FUN_1014bb465   verdict-code mapping + bit-30 clear
//
// Tables: 8257 packed 12-byte records at VA 0x106207760 (0x1830C bytes,
// header "GYM\x14"), expanded to 16-byte rows. Expansion builds three
// lookup tables: A/B (0x1000 rows each) and C (0x3C000 rows). The selector
// is the first 16 bytes of the trailer; each byte indexes a row.
//
// The gate's return convention (wrapper side):
//   gate returns 0 -> luau_load proceeds; 1 -> reject ("bytecode corrupted").
// Verdict codes 0/1/2 map to accept, 3..9 to reject; codes 3,4,6,7,9 also
// clear bit 30 of *(L->global + 0x630).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rbxvm {
namespace sigguard {

constexpr size_t kPackedTableBytes = 0x1830C;
constexpr size_t kRecordCount = 0x1830C / 12; // 8257
constexpr uint8_t kPackedMagic[4] = {'G', 'Y', 'M', 0x14};

using Record = std::array<uint8_t, 16>; // 12 meaningful bytes + 4 padding

struct Tables {
    std::array<Record, kRecordCount> t1{};
    std::vector<Record> a; // 0x1000 rows
    std::vector<Record> b; // 0x1000 rows
    std::vector<Record> c; // 0x3C000 rows
    bool expanded = false;
};

// Load the packed 12-byte records from the binary's blob. The blob is
// validated by size + header magic. Returns false on a bad fixture.
bool loadPackedTables(Tables& out, const uint8_t* blob, size_t len);

// Build A/B/C from t1. Deterministic; call once after loadPackedTables.
void expandTables(Tables& tables);

// 12-byte mode-1 MAC state for a 16-byte selector (FUN_1014bc5c8).
std::array<uint8_t, 12> computeMac(const Tables& tables, const uint8_t selector[16]);

// Verdict-code mapping from FUN_1014bb465.
bool codeRejects(int code);   // true for 3..9
bool codeClearsBit30(int code); // true for 3,4,6,7,9

struct Config {
    int max_mode = 1;          // DAT_10752be38
    int threshold_flag = 1;    // DAT_10752be20 (>=2 forces mode 1 to code 6)
    int force_signed = 0;      // DAT_10752be68
    int min_payload = 0x100;   // DAT_10752be08 (clamped to [0x100,0x400])
    bool verify_mac = true;    // DAT_10752be50 (LsbOptimizationDetail)
};

enum class Mode {
    ForcedReject = -1,
    V0 = 0,
    V1 = 1,
    Other = 2,
};

struct Result {
    int code = 1;        // verifier code (0..9)
    bool reject = false; // codeRejects(code)
    bool clears_bit30 = false;
    Mode mode = Mode::V0;
    uint32_t checkword = 0;
    size_t trailer_offset = 0;
    size_t payload_size = 0;
};

// Run the verifier on a decoded LBC blob. `len` is the full blob size.
// This mirrors FUN_1014bb64e + the gate's code mapping; it does not touch
// process-global FFlags or telemetry.
Result verify(const Tables& tables, const uint8_t* data, size_t len,
              const Config& cfg = Config{});

// Standard SHA-256 (the binary's FUN_102f8fa5c/8fc08/9109d are OpenSSL
// SHA-256; the gate requests a 32-byte digest). Returns lowercase hex.
std::string sha256Hex(const uint8_t* data, size_t len);

} // namespace sigguard
} // namespace rbxvm
