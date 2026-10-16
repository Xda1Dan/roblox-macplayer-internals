// disasm.hpp — bytecode disassembler API for the RobloxPlayer (macOS) build.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace rbxvm {

// Which numbering the low byte of each instruction word is in.
enum class OpEnc : uint8_t {
    Wire,  // bytes on disk / network stream: canon = R[T[byte]]
    Vmop,  // as loaded in memory after luau_load: canon = R[byte]
    Canon, // already canonical (our own tooling)
};

const char* toString(OpEnc e);

// Optional operand-resolution hooks for enriched disassembly (constant values,
// child proto names). Both may be null; `k` appends a comment for a K index,
// `child` returns a display name for a child proto id.
struct DisasmContext {
    std::function<void(long k, std::string& out)> kcomment;
    std::function<std::string(uint32_t)> child;
};

// Decodes one instruction at code[index] and appends a single line (no
// trailing blank lines; includes trailing '\n') to out. Returns words consumed
// (1 or 2), or 0 if the byte is invalid/unhandled in the chosen numbering.
unsigned disassembleOne(const uint32_t* code, size_t count, size_t index,
                        OpEnc openc, std::string& out);
unsigned disassembleOne(const uint32_t* code, size_t count, size_t index,
                        OpEnc openc, const DisasmContext* ctx, std::string& out);

// Disassembles a whole code array; returns number of instructions decoded.
// baseIndex is added to the printed instruction indices (use for nested protos
// or offsets within a larger blob).
long disassemble(const uint32_t* code, size_t count, OpEnc openc,
                 long baseIndex, std::string& out);
long disassemble(const uint32_t* code, size_t count, OpEnc openc,
                 long baseIndex, const DisasmContext* ctx, std::string& out);

} // namespace rbxvm
