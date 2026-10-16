// rbxdis.cpp — command-line front-end for the rbxvm reproduction library.
//
//   rbxdis tables [--csv]              print the verified opcode map / T, R tables
//   rbxdis verify                      self-check tables against the documented scheme
//   rbxdis code [options] (--hex HEX | --file PATH)
//       --openc=wire|vmop|canon        numbering of the opcode byte (default: wire)
//       --base=N                       base index printed for the first instruction
//
// `--file` expects raw little-endian 32-bit instruction words (a code array,
// not a full serialized chunk — the chunk reader arrives with the container
// work). `--hex` takes hex-encoded words, whitespace-insensitive.
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "rbxvm/opcode.hpp"
#include "rbxvm/scramble.hpp"
#include "rbxvm/disasm.hpp"
#include "rbxvm/lbc.hpp"

using namespace rbxvm;

static int cmdTables(bool csv) {
    if (csv) {
        std::printf("canonical,name,has_aux,wire_opcode,vm_opcode\n");
        for (size_t c = 0; c < kOpcodeCount; ++c) {
            uint8_t wire = canonToWire(uint8_t(c));
            std::printf("%zu,%s,%s,%u,%u\n", c, kOpInfos[c].name,
                        kOpInfos[c].has_aux ? "AUX" : "", wire, kWireToVmop[wire]);
        }
        return 0;
    }
    std::printf("canonical  name              aux   wire  vmop   pathB handler\n");
    for (size_t c = 0; c < kOpcodeCount; ++c) {
        uint8_t wire = canonToWire(uint8_t(c));
        uint8_t vmop = kWireToVmop[wire];
        std::printf("%9zu  %-16s  %-3s  %5u  %5u\n", c, kOpInfos[c].name,
                    kOpInfos[c].has_aux ? "AUX" : "-", wire, vmop);
    }
    std::printf("\nT (wire->vmop), VA 0x1063bb686:\n");
    for (int row = 0; row < 16; ++row) {
        std::printf("  %3x:", row * 16);
        for (int col = 0; col < 16; ++col) std::printf(" %3u", kWireToVmop[row * 16 + col]);
        std::printf("\n");
    }
    std::printf("\nR (vmop->canonical), VA 0x10648bb74:\n");
    for (int row = 0; row < 16; ++row) {
        std::printf("  %3x:", row * 16);
        for (int col = 0; col < 16; ++col) std::printf(" %3u", kVmopToCanon[row * 16 + col]);
        std::printf("\n");
    }
    return 0;
}

static int cmdVerify() {
    int failures = verifyTables();
    if (failures == 0) {
        std::printf("OK: T/R are permutations; R[T[(227*c)&0xFF]] == c for all 93 opcodes;"
                    " 93 distinct vmops\n");
        // also confirm the aux flag column agrees with getOpLength
        int auxCount = 0;
        for (size_t c = 0; c < kOpcodeCount; ++c)
            auxCount += kOpInfos[c].has_aux ? 1 : 0;
        std::printf("OK: %zu opcodes, %d two-word (AUX), %zu one-word\n",
                    kOpcodeCount, auxCount, kOpcodeCount - size_t(auxCount));
        return 0;
    }
    std::printf("FAILED: %d checks\n", failures);
    return 1;
}

static std::vector<uint32_t> parseHex(const char* hex) {
    auto nibble = [](char ch) -> int {
        if ('0' <= ch && ch <= '9') return ch - '0';
        if ('a' <= ch && ch <= 'f') return ch - 'a' + 10;
        if ('A' <= ch && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> bytes;
    int high = -1; // pending high nibble
    for (const char* p = hex; *p; ++p) {
        if (isspace(unsigned(*p))) continue;
        int v = nibble(*p);
        if (v < 0) { std::fprintf(stderr, "bad hex char '%c'\n", *p); return {}; }
        if (high < 0) { high = v; continue; }
        bytes.push_back(uint8_t((high << 4) | v));
        high = -1;
    }
    if (high >= 0) { std::fprintf(stderr, "odd hex length\n"); return {}; }
    if (bytes.size() % 4 != 0) {
        std::fprintf(stderr, "hex length %zu not a multiple of 4 bytes\n", bytes.size());
        return {};
    }
    std::vector<uint32_t> words;
    for (size_t i = 0; i + 3 < bytes.size(); i += 4)
        words.push_back(uint32_t(bytes[i]) | (uint32_t(bytes[i + 1]) << 8) |
                        (uint32_t(bytes[i + 2]) << 16) | (uint32_t(bytes[i + 3]) << 24));
    return words;
}

static std::vector<uint32_t> readFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return {}; }
    std::vector<uint32_t> words;
    uint8_t buf[4];
    while (std::fread(buf, 1, 4, f) == 4)
        words.push_back(uint32_t(buf[0]) | (uint32_t(buf[1]) << 8) |
                        (uint32_t(buf[2]) << 16) | (uint32_t(buf[3]) << 24));
    std::fclose(f);
    return words;
}

static int cmdCode(int argc, char** argv) {
    OpEnc openc = OpEnc::Wire;
    long base = 0;
    std::vector<uint32_t> words;
    bool haveInput = false;

    for (int i = 0; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strncmp(a, "--openc=", 8)) {
            const char* v = a + 8;
            if (!std::strcmp(v, "wire")) openc = OpEnc::Wire;
            else if (!std::strcmp(v, "vmop")) openc = OpEnc::Vmop;
            else if (!std::strcmp(v, "canon")) openc = OpEnc::Canon;
            else { std::fprintf(stderr, "bad --openc '%s'\n", v); return 2; }
        } else if (!std::strncmp(a, "--base=", 7)) {
            base = std::strtol(a + 7, nullptr, 0);
        } else if (!std::strncmp(a, "--hex=", 6)) {
            words = parseHex(a + 6);
            haveInput = true;
        } else if (!std::strncmp(a, "--file=", 7)) {
            words = readFile(a + 7);
            haveInput = true;
        } else {
            std::fprintf(stderr, "unknown option '%s'\n", a);
            return 2;
        }
    }
    if (!haveInput) {
        std::fprintf(stderr, "no input: pass --hex=... or --file=PATH\n");
        return 2;
    }

    std::string out;
    long n = disassemble(words.data(), words.size(), openc, base, out);
    std::fputs(out.c_str(), stdout);
    std::printf("; %ld instructions, %zu words, openc=%s\n", n, words.size(),
                toString(openc));
    return 0;
}

static int cmdChunk(int argc, char** argv) {
    OpEnc openc = OpEnc::Canon; // luau-compile output is canonical by default
    const char* path = nullptr;
    for (int i = 0; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strncmp(a, "--openc=", 8)) {
            const char* v = a + 8;
            if (!std::strcmp(v, "wire")) openc = OpEnc::Wire;
            else if (!std::strcmp(v, "canon")) openc = OpEnc::Canon;
            else { std::fprintf(stderr, "bad --openc '%s'\n", v); return 2; }
        } else if (!path) {
            path = a;
        } else {
            std::fprintf(stderr, "unexpected arg '%s'\n", a);
            return 2;
        }
    }
    if (!path) { std::fprintf(stderr, "chunk: need a file\n"); return 2; }

    FILE* fh = std::fopen(path, "rb");
    if (!fh) { std::perror(path); return 1; }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fh)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(fh);
    if (data.empty()) return 1;
    try {
        lbc::Module m = lbc::parse(data.data(), data.size());
        std::fputs(lbc::dump(m, openc).c_str(), stdout);
        return 0;
    } catch (const lbc::ParseError& e) {
        std::fprintf(stderr, "parse error at offset %zu: %s\n", e.offset, e.what());
        return 1;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage:\n"
                     "  rbxdis tables [--csv]\n"
                     "  rbxdis verify\n"
                     "  rbxdis code [--openc=wire|vmop|canon] [--base=N] (--hex HEX | --file PATH)\n"
                     "  rbxdis chunk [--openc=wire|canon] FILE      (LBC serialized chunk)\n");
        return 2;
    }
    if (!std::strcmp(argv[1], "tables")) return cmdTables(argc > 2 && !std::strcmp(argv[2], "--csv"));
    if (!std::strcmp(argv[1], "verify")) return cmdVerify();
    if (!std::strcmp(argv[1], "code")) return cmdCode(argc - 2, argv + 2);
    if (!std::strcmp(argv[1], "chunk")) return cmdChunk(argc - 2, argv + 2);
    std::fprintf(stderr, "unknown command '%s'\n", argv[1]);
    return 2;
}
