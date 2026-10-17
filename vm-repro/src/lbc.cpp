// lbc.cpp — LBC serialized-chunk reader + pretty printer.
// Format provenance: include/rbxvm/lbc.hpp header comment.
#include "rbxvm/lbc.hpp"

#include "rbxvm/opcode.hpp"
#include "rbxvm/scramble.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace rbxvm {
namespace lbc {

namespace {

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t off = 0;

    uint8_t u8() {
        if (off >= size) throw ParseError("unexpected end of stream", off);
        return data[off++];
    }

    uint32_t varint() {
        uint32_t result = 0;
        unsigned shift = 0;
        for (;;) {
            uint8_t b = u8();
            result |= uint32_t(b & 0x7f) << shift;
            if ((b & 0x80) == 0) return result;
            shift += 7;
            if (shift > 28) throw ParseError("varint too long", off);
        }
    }

    uint64_t varint64() {
        uint64_t result = 0;
        unsigned shift = 0;
        for (;;) {
            uint8_t b = u8();
            result |= uint64_t(b & 0x7f) << shift;
            if ((b & 0x80) == 0) return result;
            shift += 7;
            if (shift > 63) throw ParseError("varint64 too long", off);
        }
    }

    void bytes(void* dst, size_t n) {
        if (off + n > size) throw ParseError("unexpected end of stream", off);
        std::memcpy(dst, data + off, n);
        off += n;
    }

    uint32_t u32() {
        uint8_t b[4];
        bytes(b, 4);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    }

    int32_t i32() { return int32_t(u32()); }

    double f64() {
        uint8_t b[8];
        bytes(b, 8);
        double v;
        std::memcpy(&v, b, 8);
        return v;
    }

    float f32() {
        uint8_t b[4];
        bytes(b, 4);
        float v;
        std::memcpy(&v, b, 4);
        return v;
    }

    uint32_t stringIdx() { return varint(); }
};

Constant readConstant(Reader& r) {
    Constant c;
    c.kind = Constant::Kind(r.u8());
    switch (c.kind) {
    case Constant::Kind::Nil: break;
    case Constant::Kind::Boolean: c.b = r.u8() != 0; break;
    case Constant::Kind::Number: c.num = r.f64(); break;
    case Constant::Kind::String: c.index = r.varint(); break;
    case Constant::Kind::Import: c.index = r.u32(); break;
    case Constant::Kind::Table: {
        uint32_t keys = r.varint();
        for (uint32_t i = 0; i < keys; ++i) c.tableKeys.push_back(r.varint());
        break;
    }
    case Constant::Kind::Closure: c.index = r.varint(); break;
    case Constant::Kind::Vector:
        for (int i = 0; i < 4; ++i) c.vec[i] = r.f32();
        break;
    case Constant::Kind::TableWithConstants: {
        uint32_t keys = r.varint();
        for (uint32_t i = 0; i < keys; ++i) {
            c.tableKeys.push_back(r.varint());
            c.tableConstIdx.push_back(int32_t(r.u32()));
        }
        break;
    }
    case Constant::Kind::Integer:
        c.neg = r.u8() != 0;
        c.integer = r.varint64();
        break;
    case Constant::Kind::ClassShape: {
        c.index = r.varint(); // class name string index
        c.classProps = r.varint();
        c.classMethods = r.varint();
        for (uint32_t i = 0; i < c.classProps + c.classMethods; ++i)
            c.classMembers.push_back(r.varint());
        break;
    }
    case Constant::Kind::VectorD:
        for (int i = 0; i < 4; ++i) c.vecd[i] = r.f64();
        break;
    default:
        throw ParseError("unknown constant kind", r.off);
    }
    return c;
}

} // namespace

namespace {

// Canonical opcode for a stored opcode byte in the given numbering. Returns
// kOpcodeCount when the byte is not a valid opcode.
uint8_t canonicalOp(uint8_t byte, OpEnc openc) {
    uint8_t canon = 0;
    switch (openc) {
    case OpEnc::Canon: canon = byte; break;
    case OpEnc::Wire: canon = vmopToCanon(wireToVmop(byte)); break;
    case OpEnc::Vmop: canon = vmopToCanon(byte); break;
    }
    return canon;
}

Module parseImpl(Reader& r, OpEnc openc, std::vector<size_t>* codeOffsets) {
    Module m;
    m.version = r.u8();
    if (m.version == 0) throw ParseError("bytecode carries an error message (version 0)", r.off);
    if ((m.version < 3 || m.version > 14) && m.version != 100)
        throw ParseError("bytecode version mismatch", r.off);
    if (m.version >= 4) {
        m.typesversion = r.u8();
        if (m.typesversion < 1 || m.typesversion > 3)
            throw ParseError("type version mismatch", r.off);
    }

    const uint32_t stringCount = r.varint();
    m.strings.resize(stringCount);
    for (uint32_t i = 0; i < stringCount; ++i) {
        uint32_t len = r.varint();
        if (r.off + len > r.size) throw ParseError("string overruns stream", r.off);
        m.strings[i].assign(reinterpret_cast<const char*>(r.data + r.off), len);
        r.off += len;
    }

    if (m.typesversion == 3) {
        uint8_t index = r.u8();
        while (index != 0) {
            uint32_t nameIdx = r.stringIdx();
            (void)nameIdx; // userdata type remapping (engine-side callback)
            index = r.u8();
        }
    }

    const uint32_t protoCount = r.varint();
    m.protos.resize(protoCount);
    for (uint32_t i = 0; i < protoCount; ++i) {
        uint32_t protoSize = 0;
        if (m.version >= 12) protoSize = r.varint();
        const size_t protoStart = r.off; // AFTER the size field (reference semantics)

        Proto& p = m.protos[i];

        p.maxstacksize = r.u8();
        p.numparams = r.u8();
        p.nups = r.u8();
        p.is_vararg = r.u8();
        if (m.version >= 4) {
            p.flags = r.u8();
            uint32_t typesize = r.varint();
            if (typesize) {
                p.typeinfo.resize(typesize);
                r.bytes(p.typeinfo.data(), typesize);
            }
        }

        uint32_t sizecode = r.varint();
        const size_t codeStart = r.off;
        p.code.resize(sizecode);
        for (uint32_t j = 0; j < sizecode; ++j) p.code[j] = r.u32();

        if (codeOffsets) {
            // Walk instruction headers using the numbering stored in the blob;
            // AUX words are skipped, never treated as headers.
            uint32_t pc = 0;
            while (pc < sizecode) {
                codeOffsets->push_back(codeStart + size_t(pc) * 4);
                uint8_t canon = canonicalOp(uint8_t(p.code[pc] & 0xff), openc);
                uint32_t words = (canon < kOpcodeCount) ? getOpLength(canon) : 1;
                if (words == 0) words = 1;
                pc += words;
            }
        }

        uint32_t sizek = r.varint();
        p.k.resize(sizek);
        for (uint32_t j = 0; j < sizek; ++j) p.k[j] = readConstant(r);

        uint32_t sizep = r.varint();
        p.children.resize(sizep);
        for (uint32_t j = 0; j < sizep; ++j) p.children[j] = r.varint();

        p.linedefined = r.varint();
        p.debugnameIdx = r.stringIdx();

        p.hasLineInfo = r.u8() != 0;
        if (p.hasLineInfo) {
            p.linegaplog2 = r.u8();
            p.lineinfo.resize(sizecode);
            uint8_t last = 0;
            for (uint32_t j = 0; j < sizecode; ++j) {
                last = uint8_t(last + r.u8());
                p.lineinfo[j] = last;
            }
            uint32_t intervals = sizecode ? ((sizecode - 1) >> p.linegaplog2) + 1 : 0;
            p.abslineinfo.resize(intervals);
            int32_t lastline = 0;
            for (uint32_t j = 0; j < intervals; ++j) {
                lastline += r.i32();
                p.abslineinfo[j] = lastline;
            }
        }

        p.hasDebugInfo = r.u8() != 0;
        if (p.hasDebugInfo) {
            uint32_t nloc = r.varint();
            p.locvars.resize(nloc);
            for (uint32_t j = 0; j < nloc; ++j) {
                p.locvars[j].nameIdx = r.stringIdx();
                p.locvars[j].startpc = r.varint();
                p.locvars[j].endpc = r.varint();
                p.locvars[j].reg = r.u8();
            }
            uint32_t nups = r.varint();
            p.upvalueNames.resize(nups);
            for (uint32_t j = 0; j < nups; ++j) p.upvalueNames[j] = r.stringIdx();
        }

        if (m.version >= 11) {
            uint32_t nfb = r.varint();
            p.feedback.resize(nfb);
            for (uint32_t j = 0; j < nfb; ++j) {
                p.feedback[j].kind = r.u8();
                if (p.feedback[j].kind == 0 || p.feedback[j].kind == 1) // CALLTARGET / CONSTRUCT
                    p.feedback[j].target = r.varint();
            }
        }

        if (m.version >= 12 && (p.flags & 1) != 0) p.cost = r.varint64();

        if (m.version >= 12) {
            // skip any unknown tail bytes of this proto section
            if (protoStart + protoSize > r.size) throw ParseError("proto size overruns stream", r.off);
            r.off = protoStart + protoSize;
        }
    }

    m.mainProto = r.varint();
    return m;
}
} // namespace

Module parse(const uint8_t* data, size_t size) {
    Reader r{data, size, 0};
    return parseImpl(r, OpEnc::Canon, nullptr);
}

std::vector<size_t> codeWordOffsets(const uint8_t* data, size_t size, OpEnc openc) {
    Reader r{data, size, 0};
    std::vector<size_t> offsets;
    parseImpl(r, openc, &offsets);
    return offsets;
}

namespace {
uint8_t encodeOp(uint8_t canonical, OpEnc openc) {
    switch (openc) {
    case OpEnc::Canon: return canonical;
    case OpEnc::Wire: return canonToWire(canonical);
    case OpEnc::Vmop: return canonToVmop(canonical);
    }
    return canonical;
}
} // namespace

void convertOps(uint8_t* data, size_t size, OpEnc from, OpEnc to) {
    if (from == to) return;
    std::vector<size_t> offsets = codeWordOffsets(data, size, from);
    for (size_t off : offsets) {
        uint8_t canon = canonicalOp(data[off], from);
        if (canon >= kOpcodeCount) continue; // leave unknown bytes untouched
        data[off] = encodeOp(canon, to);
    }
}

// ---------------------------------------------------------------------------
// dump
// ---------------------------------------------------------------------------
namespace {

std::string escape(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:
            if (unsigned(ch) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\x%02x", unsigned(ch) & 0xff);
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
    return out;
}

std::string constText(const Module& m, const Constant& c) {
    char buf[160];
    switch (c.kind) {
    case Constant::Kind::Nil: return "nil";
    case Constant::Kind::Boolean: return c.b ? "true" : "false";
    case Constant::Kind::Number: {
        std::string s = std::to_string(c.num);
        return s;
    }
    case Constant::Kind::String:
        return escape(m.str(c.index));
    case Constant::Kind::Import: {
        std::snprintf(buf, sizeof buf, "import#%u", c.index);
        return buf;
    }
    case Constant::Kind::Table: {
        std::snprintf(buf, sizeof buf, "table[%u keys]", unsigned(c.tableKeys.size()));
        return buf;
    }
    case Constant::Kind::Closure: {
        std::string s = "closure proto#";
        std::snprintf(buf, sizeof buf, "%u", c.index);
        s += buf;
        if (c.index < m.protos.size()) {
            const std::string& n = m.str(m.protos[c.index].debugnameIdx);
            if (!n.empty()) s += " <" + n + ">";
        }
        return s;
    }
    case Constant::Kind::Vector: {
        std::snprintf(buf, sizeof buf, "vector(%.6g, %.6g, %.6g, %.6g)", c.vec[0], c.vec[1],
                      c.vec[2], c.vec[3]);
        return buf;
    }
    case Constant::Kind::VectorD: {
        std::snprintf(buf, sizeof buf, "vectord(%.17g, %.17g, %.17g, %.17g)", c.vecd[0],
                      c.vecd[1], c.vecd[2], c.vecd[3]);
        return buf;
    }
    case Constant::Kind::TableWithConstants: {
        std::snprintf(buf, sizeof buf, "table[%u keys w/ constants]", unsigned(c.tableKeys.size()));
        return buf;
    }
    case Constant::Kind::Integer: {
        // Two's-complement negation, matching lvmload.cpp (safe for 2^63).
        const int64_t v = c.neg ? int64_t(~c.integer + 1) : int64_t(c.integer);
        std::snprintf(buf, sizeof buf, "i64 %lld", (long long)v);
        return buf;
    }
    case Constant::Kind::ClassShape: {
        std::string s = "class ";
        s += m.str(c.index);
        std::snprintf(buf, sizeof buf, " (%u props, %u methods)", c.classProps, c.classMethods);
        s += buf;
        return s;
    }
    }
    return "?";
}

} // namespace

std::string dump(const Module& m, OpEnc openc) {
    std::string out;
    char buf[128];

    std::snprintf(buf, sizeof buf, "; LBC version %u, typesversion %u, %zu strings, %zu protos, main=#%u\n",
                  m.version, m.typesversion, m.strings.size(), m.protos.size(), m.mainProto);
    out += buf;

    for (uint32_t pi = 0; pi < m.protos.size(); ++pi) {
        const Proto& p = m.protos[pi];
        std::string name = m.str(p.debugnameIdx);
        std::snprintf(buf, sizeof buf, "\n; --- proto #%u: %s (line %u) params=%u upvals=%u %s ---\n",
                      pi, name.c_str(), p.linedefined, p.numparams, p.nups,
                      p.is_vararg ? "vararg" : "");
        out += buf;

        // child proto names for NEWCLOSURE comments
        DisasmContext ctx;
        ctx.child = [&m](uint32_t id) -> std::string {
            if (id >= m.protos.size()) return "";
            const Proto& cp = m.protos[id];
            std::string n = "proto#" + std::to_string(id);
            if (cp.debugnameIdx < m.strings.size()) n += " <" + m.strings[cp.debugnameIdx] + ">";
            return n;
        };

        std::string line;
        line.reserve(64);
        long idx = 0;
        size_t i = 0;
        while (i < p.code.size()) {
            line.clear();
            std::snprintf(buf, sizeof buf, "%04ld  ", idx);
            line += buf;

            // per-proto constant resolution
            DisasmContext pctx;
            pctx.child = ctx.child;
            pctx.kcomment = [&m, &p](long k, std::string& o) {
                if (k < 0 || size_t(k) >= p.k.size()) return;
                o += " ; ";
                o += constText(m, p.k[size_t(k)]);
            };
            // Line number: lineinfo is indexed by WORD position, not by
            // instruction count (AUX words have their own entry).
            int lineNo = -1;
            if (p.hasLineInfo && i < p.lineinfo.size()) {
                uint32_t interval = p.linegaplog2 < 32 ? uint32_t(i) >> p.linegaplog2 : 0;
                if (interval < p.abslineinfo.size())
                    lineNo = p.abslineinfo[interval] + int(p.lineinfo[i]);
            }

            unsigned words = disassembleOne(p.code.data(), p.code.size(), i, openc, &pctx, line);
            if (words == 0) {
                char raw[48];
                std::snprintf(raw, sizeof raw, "; raw %08x\n", p.code[i]);
                line += raw;
                i += 1;
                idx += 1;
                continue;
            }
            if (lineNo >= 0) {
                char ln[32];
                std::snprintf(ln, sizeof ln, " ; line %d", lineNo);
                if (!line.empty() && line.back() == '\n') line.pop_back();
                line += ln;
            }
            line += '\n';
            out += line;
            i += words;
            idx += 1;
        }
    }

    return out;
}

} // namespace lbc
} // namespace rbxvm
