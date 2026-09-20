#pragma once

// =============================================================================
// BytecodeIO.h -- a versioned, chunked on-disk container ("SKBC") for a compiled
// bytecode image, plus its writer/reader. This is P0 of the North Star: persist a
// compiled program and re-run it without recompiling.
//
// SCOPE: a single, already-linked bytecode image (the flattened output of the
// compiler; modules are mangled away before this point). It carries EXACTLY the
// fields execute() consumes -- so a deserialized image runs by handing its vectors
// straight to execute(); there is NO new runtime path.
//
// LAYERING: vmcore must not depend on the compiler, so this header operates on a
// vmcore-local `ModuleImage` (the execution-relevant subset of the compiler's
// svc::Module -- no compile-time `warnings`). The compiler supplies a trivial
// svc::Module -> ModuleImage field copy (svc::module_to_image).
//
// FORMAT (all multi-byte integers little-endian, written byte-wise so the container
// metadata is endian-portable by construction):
//
//   Header:  magic "SKBC" | endianness u8(0=LE) | container_version u16 |
//            isa_version u16 | layout_sentinel u32 | flags u16 | chunk_count u16
//   Chunks:  tag[4] | cflags u8 (bit0 = critical) | length u32 | payload
//   Footer:  crc32 u32 over [magic .. last chunk]
//
// Chunk tags: CODE CPOL CARR STRC STRL FNTB TRTB META (critical) + DBGL (ancillary,
// strippable debug info: line/column tables + function names).
//
// ENCODING CHOICE (owner-approved "Option A"): the bytecode travels as the raw
// 32-bit instruction words, packed the way the writing toolchain lays bitfields out
// -- fast, and correct wherever reader and writer agree, which the two supported
// toolchains (MSVC/x64 and Clang/arm64) do. The `layout_sentinel` (a known packed Instruction word the
// reader recomputes and compares) turns a mismatched-bitfield-ABI read into a clean
// REJECT rather than a silent misread. A portable field-by-field re-encode would be a
// non-breaking `container_version` bump later.
//
// VERSIONING: `isa_version` = BYTECODE_ISA_VERSION (opcode SEMANTICS -- bumped on any
// ISA change); `container_version` = CONTAINER_FORMAT_VERSION (this file structure).
// Both live in Opcodes.h. A reader rejects a newer container_version, an isa_version
// it does not support, an unknown CRITICAL chunk, or a bad sentinel/CRC; it SKIPS an
// unknown ANCILLARY chunk (forward-compat: a future field = a new ancillary chunk).
// =============================================================================

#include <cstdint>
#include <cstring>
#include <bit>
#include <string>
#include <vector>
#include <stdexcept>

#include "Value.h"
#include "StructType.h"
#include "FunctionTable.h"
#include "Instruction.h"
#include "Opcodes.h"   // BYTECODE_ISA_VERSION / CONTAINER_FORMAT_VERSION + op()/OpCode

namespace bcio {

// The execution-relevant subset of the compiler's svc::Module: exactly the fields
// execute() reads. Keep in lockstep with svc::Module (svc::module_to_image copies
// field-for-field); the round-trip test + the 50k differential sweep catch drift.
struct ModuleImage {
    std::vector<uint32_t>    bytecode;
    std::vector<Value>       constants;
    // Const-array literals (LOAD_CONST_ARRAY): one inner vector per `const NAME: Array[T] =
    // [...]`, holding its scalar element immediates. Pointer-free by contract (like constants).
    std::vector<std::vector<Value>> const_arrays;
    std::vector<StructType>  struct_types;
    std::vector<std::string> string_literals;
    std::vector<FnInfo>      function_table;
    std::vector<uint16_t>    trait_table;
    uint32_t                 trait_table_width  = 0;
    uint32_t                 trait_method_count = 0;
    uint8_t                  top_frame_size     = 0;
    // Strippable debug info (the DBGL chunk). Empty is legal: execute() then renders
    // faults without a source caret.
    std::vector<uint32_t>    line_table;
    std::vector<uint32_t>    column_table;
    std::vector<std::string> function_names;
    std::vector<std::string> function_modules;   // parallel to function_names; owning module prefix (fault caret)
};

// Thrown on any malformed / incompatible image: bad magic, unsupported version,
// bitfield-layout mismatch, unknown critical chunk, CRC mismatch, or truncation.
class BytecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ----- format constants ------------------------------------------------------
inline constexpr uint8_t  ENDIAN_LE       = 0;
inline constexpr uint8_t  CHUNK_CRITICAL  = 0x01;   // cflags bit0
inline constexpr uint16_t FLAG_HAS_DEBUG  = 0x0001; // header flags bit0
inline constexpr size_t   HEADER_SIZE     = 17;     // magic4+endian1+cver2+isa2+sentinel4+flags2+count2
inline constexpr size_t   FOOTER_SIZE     = 4;      // crc32

// A known packed Instruction word. Recomputed identically by writer and reader; a
// differing bitfield ABI (or a different Instruction layout) yields a different word,
// so the reader rejects instead of misreading the raw code stream. Exercises the
// signed 16-bit `cnst` bitfield + LSB-first packing of the c2 layout.
inline uint32_t layout_sentinel() {
    return Instruction::C2(op(OpCode::LOAD_CONST), 42, -3).raw;
}

// ----- CRC32 (IEEE 802.3, poly 0xEDB88320), table built once -----------------
inline uint32_t crc32(const uint8_t* data, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ----- little-endian write primitives ----------------------------------------
namespace detail {

inline void put_u8 (std::vector<uint8_t>& o, uint8_t v)  { o.push_back(v); }
inline void put_u16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
inline void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8));
    o.push_back(uint8_t(v >> 16)); o.push_back(uint8_t(v >> 24));
}
inline void put_u64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
inline void put_str(std::vector<uint8_t>& o, const std::string& s) {
    put_u32(o, uint32_t(s.size()));
    o.insert(o.end(), s.begin(), s.end());
}
inline void emit_chunk(std::vector<uint8_t>& o, const char tag[4], uint8_t cflags,
                       const std::vector<uint8_t>& payload) {
    o.insert(o.end(), tag, tag + 4);
    put_u8(o, cflags);
    put_u32(o, uint32_t(payload.size()));
    o.insert(o.end(), payload.begin(), payload.end());
}

// ----- little-endian read cursor (bounds-checked) ----------------------------
struct Reader {
    const uint8_t* p;
    size_t         n;
    size_t         pos = 0;

    void need(size_t k) const {
        if (pos + k > n) throw BytecodeError("bytecode: unexpected end of data");
    }
    uint8_t  u8()  { need(1); return p[pos++]; }
    uint16_t u16() { need(2); uint16_t v = uint16_t(p[pos]) | uint16_t(uint16_t(p[pos + 1]) << 8); pos += 2; return v; }
    uint32_t u32() {
        need(4);
        uint32_t v = uint32_t(p[pos]) | (uint32_t(p[pos + 1]) << 8)
                   | (uint32_t(p[pos + 2]) << 16) | (uint32_t(p[pos + 3]) << 24);
        pos += 4; return v;
    }
    uint64_t u64() {
        need(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(p[pos + i]) << (8 * i);
        pos += 8; return v;
    }
    std::string str() {
        uint32_t len = u32();
        need(len);
        std::string s(reinterpret_cast<const char*>(p + pos), len);
        pos += len; return s;
    }
};

inline bool tag_eq(const char t[4], const char (&lit)[5]) {
    return std::memcmp(t, lit, 4) == 0;
}

} // namespace detail

// =============================================================================
// serialize -- ModuleImage -> bytes.
// =============================================================================
inline std::vector<uint8_t> serialize(const ModuleImage& m, bool include_debug = true) {
    using namespace detail;
    std::vector<uint8_t> out;

    // header
    const char magic[4] = { 'S', 'K', 'B', 'C' };
    out.insert(out.end(), magic, magic + 4);
    put_u8 (out, ENDIAN_LE);
    put_u16(out, CONTAINER_FORMAT_VERSION);
    put_u16(out, BYTECODE_ISA_VERSION);
    put_u32(out, layout_sentinel());
    put_u16(out, include_debug ? FLAG_HAS_DEBUG : uint16_t(0));
    const uint16_t chunk_count = uint16_t(8 + (include_debug ? 1 : 0));   // CODE CPOL CARR STRC STRL FNTB TRTB META (+ DBGL)
    put_u16(out, chunk_count);

    std::vector<uint8_t> pl;   // reused payload scratch

    // CODE -- raw instruction words
    pl.clear();
    put_u32(pl, uint32_t(m.bytecode.size()));
    for (uint32_t w : m.bytecode) put_u32(pl, w);
    emit_chunk(out, "CODE", CHUNK_CRITICAL, pl);

    // CPOL -- constant pool (raw 64-bit Value bit patterns; pointer-free by contract)
    pl.clear();
    put_u32(pl, uint32_t(m.constants.size()));
    for (const Value& v : m.constants) put_u64(pl, std::bit_cast<uint64_t>(v));
    emit_chunk(out, "CPOL", CHUNK_CRITICAL, pl);

    // CARR -- const-array literals (per array: count + raw Value bit patterns; pointer-free)
    pl.clear();
    put_u32(pl, uint32_t(m.const_arrays.size()));
    for (const std::vector<Value>& arr : m.const_arrays) {
        put_u32(pl, uint32_t(arr.size()));
        for (const Value& v : arr) put_u64(pl, std::bit_cast<uint64_t>(v));
    }
    emit_chunk(out, "CARR", CHUNK_CRITICAL, pl);

    // STRC -- struct types
    pl.clear();
    put_u32(pl, uint32_t(m.struct_types.size()));
    for (const StructType& st : m.struct_types) {
        put_u32(pl, st.field_count);
        put_u8 (pl, static_cast<uint8_t>(st.dump_style));
        put_str(pl, st.name);
        put_u32(pl, uint32_t(st.field_names.size()));
        for (const std::string& fn : st.field_names) put_str(pl, fn);
    }
    emit_chunk(out, "STRC", CHUNK_CRITICAL, pl);

    // STRL -- string literals
    pl.clear();
    put_u32(pl, uint32_t(m.string_literals.size()));
    for (const std::string& s : m.string_literals) put_str(pl, s);
    emit_chunk(out, "STRL", CHUNK_CRITICAL, pl);

    // FNTB -- function table
    pl.clear();
    put_u32(pl, uint32_t(m.function_table.size()));
    for (const FnInfo& f : m.function_table) {
        put_u32(pl, f.code_offset);
        put_u8 (pl, f.frame_size);
        put_u8 (pl, f.arity);
        put_u8 (pl, f.ncaptures);
        put_u8 (pl, f.self_slot);
    }
    emit_chunk(out, "FNTB", CHUNK_CRITICAL, pl);

    // TRTB -- trait dispatch table
    pl.clear();
    put_u32(pl, uint32_t(m.trait_table.size()));
    for (uint16_t w : m.trait_table) put_u16(pl, w);
    emit_chunk(out, "TRTB", CHUNK_CRITICAL, pl);

    // META -- scalars
    pl.clear();
    put_u8 (pl, m.top_frame_size);
    put_u32(pl, m.trait_table_width);
    put_u32(pl, m.trait_method_count);
    emit_chunk(out, "META", CHUNK_CRITICAL, pl);

    // DBGL -- strippable debug info (ancillary)
    if (include_debug) {
        pl.clear();
        put_u32(pl, uint32_t(m.line_table.size()));
        for (uint32_t v : m.line_table)   put_u32(pl, v);
        put_u32(pl, uint32_t(m.column_table.size()));
        for (uint32_t v : m.column_table) put_u32(pl, v);
        put_u32(pl, uint32_t(m.function_names.size()));
        for (const std::string& s : m.function_names) put_str(pl, s);
        // function_modules (parallel to function_names) -- appended last so an older reader that
        // stops after function_names simply ignores the trailing bytes (the chunk length skips them).
        put_u32(pl, uint32_t(m.function_modules.size()));
        for (const std::string& s : m.function_modules) put_str(pl, s);
        emit_chunk(out, "DBGL", /*ancillary*/ 0, pl);
    }

    // footer: CRC over everything so far
    put_u32(out, crc32(out.data(), out.size()));
    return out;
}

// =============================================================================
// deserialize -- bytes -> ModuleImage. Throws BytecodeError on any problem.
// =============================================================================
inline ModuleImage deserialize(const uint8_t* data, size_t size) {
    using namespace detail;
    if (size < HEADER_SIZE + FOOTER_SIZE)
        throw BytecodeError("bytecode: image too small");

    // integrity first: CRC over [0, size-4) vs the stored trailing u32
    const uint32_t stored = uint32_t(data[size - 4]) | (uint32_t(data[size - 3]) << 8)
                          | (uint32_t(data[size - 2]) << 16) | (uint32_t(data[size - 1]) << 24);
    if (crc32(data, size - FOOTER_SIZE) != stored)
        throw BytecodeError("bytecode: CRC mismatch (corrupt image)");

    Reader r{ data, size };

    // header
    r.need(4);
    if (data[0] != 'S' || data[1] != 'K' || data[2] != 'B' || data[3] != 'C')
        throw BytecodeError("bytecode: bad magic (not an SKBC image)");
    r.pos = 4;
    if (r.u8() != ENDIAN_LE)
        throw BytecodeError("bytecode: unsupported endianness");
    const uint16_t cver = r.u16();
    if (cver > CONTAINER_FORMAT_VERSION)
        throw BytecodeError("bytecode: container version too new");
    const uint16_t isa = r.u16();
    if (isa != BYTECODE_ISA_VERSION)
        throw BytecodeError("bytecode: incompatible ISA version");
    if (r.u32() != layout_sentinel())
        throw BytecodeError("bytecode: instruction layout mismatch (wrong toolchain/ABI)");
    (void)r.u16();  // flags (informational; DBGL presence is chunk-driven)
    const uint16_t chunk_count = r.u16();

    ModuleImage m;
    for (uint16_t i = 0; i < chunk_count; ++i) {
        r.need(4 + 1 + 4);
        char tag[4];
        std::memcpy(tag, r.p + r.pos, 4);
        r.pos += 4;
        const uint8_t  cflags = r.u8();
        const uint32_t len    = r.u32();
        r.need(len);
        // sub-cursor bounded to this chunk so a lying length can't over-read
        Reader pr{ r.p + r.pos, len };
        r.pos += len;

        if (tag_eq(tag, "CODE")) {
            uint32_t k = pr.u32();
            m.bytecode.resize(k);
            for (uint32_t j = 0; j < k; ++j) m.bytecode[j] = pr.u32();
        } else if (tag_eq(tag, "CPOL")) {
            uint32_t k = pr.u32();
            m.constants.resize(k);
            for (uint32_t j = 0; j < k; ++j) m.constants[j] = std::bit_cast<Value>(pr.u64());
        } else if (tag_eq(tag, "CARR")) {
            uint32_t k = pr.u32();
            m.const_arrays.resize(k);
            for (uint32_t j = 0; j < k; ++j) {
                uint32_t n = pr.u32();
                m.const_arrays[j].resize(n);
                for (uint32_t x = 0; x < n; ++x) m.const_arrays[j][x] = std::bit_cast<Value>(pr.u64());
            }
        } else if (tag_eq(tag, "STRC")) {
            uint32_t k = pr.u32();
            m.struct_types.resize(k);
            for (uint32_t j = 0; j < k; ++j) {
                StructType st;
                st.field_count = pr.u32();
                st.dump_style  = static_cast<DumpStyle>(pr.u8());
                st.name        = pr.str();
                uint32_t fn = pr.u32();
                st.field_names.resize(fn);
                for (uint32_t x = 0; x < fn; ++x) st.field_names[x] = pr.str();
                m.struct_types[j] = std::move(st);
            }
        } else if (tag_eq(tag, "STRL")) {
            uint32_t k = pr.u32();
            m.string_literals.resize(k);
            for (uint32_t j = 0; j < k; ++j) m.string_literals[j] = pr.str();
        } else if (tag_eq(tag, "FNTB")) {
            uint32_t k = pr.u32();
            m.function_table.resize(k);
            for (uint32_t j = 0; j < k; ++j) {
                FnInfo f;
                f.code_offset = pr.u32();
                f.frame_size  = pr.u8();
                f.arity       = pr.u8();
                f.ncaptures   = pr.u8();
                f.self_slot   = pr.u8();
                m.function_table[j] = f;
            }
        } else if (tag_eq(tag, "TRTB")) {
            uint32_t k = pr.u32();
            m.trait_table.resize(k);
            for (uint32_t j = 0; j < k; ++j) m.trait_table[j] = pr.u16();
        } else if (tag_eq(tag, "META")) {
            m.top_frame_size     = pr.u8();
            m.trait_table_width  = pr.u32();
            m.trait_method_count = pr.u32();
        } else if (tag_eq(tag, "DBGL")) {
            uint32_t nl = pr.u32();
            m.line_table.resize(nl);
            for (uint32_t j = 0; j < nl; ++j) m.line_table[j] = pr.u32();
            uint32_t nc = pr.u32();
            m.column_table.resize(nc);
            for (uint32_t j = 0; j < nc; ++j) m.column_table[j] = pr.u32();
            uint32_t nf = pr.u32();
            m.function_names.resize(nf);
            for (uint32_t j = 0; j < nf; ++j) m.function_names[j] = pr.str();
            if (pr.pos < pr.n) {                 // optional: absent in pre-function_modules images
                uint32_t nm = pr.u32();
                m.function_modules.resize(nm);
                for (uint32_t j = 0; j < nm; ++j) m.function_modules[j] = pr.str();
            }
        } else if (cflags & CHUNK_CRITICAL) {
            throw BytecodeError("bytecode: unknown critical chunk");
        }
        // else: unknown ancillary chunk -> skip (forward-compat)
    }
    return m;
}

inline ModuleImage deserialize(const std::vector<uint8_t>& bytes) {
    return deserialize(bytes.data(), bytes.size());
}

} // namespace bcio
