#pragma once

// =============================================================================
// ByteIO.h -- little-endian byte packing, shared by vmcore's byte containers.
//
// WHY IT IS ITS OWN HEADER. Two unrelated containers pack values into a byte buffer:
// BytecodeIO.h's on-disk SKBC image, and ValueCodec.h's in-memory value graph. The
// framing, versioning and error models differ completely; the byte primitives do not.
// Keeping one copy here means the two can never drift on something as silently
// breakable as byte order.
//
// EVERYTHING IS EXPLICITLY LITTLE-ENDIAN AND WRITTEN BYTE-WISE -- never a memcpy of a
// wider integer -- so a buffer means the same thing regardless of the host's byte order
// or alignment rules. That is load-bearing for SKBC, which travels between toolchains;
// ValueCodec inherits the property for free.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace byteio {

// ----- write primitives ------------------------------------------------------
inline void put_u8 (std::vector<uint8_t>& o, uint8_t v)  { o.push_back(v); }
inline void put_u16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
inline void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8));
    o.push_back(uint8_t(v >> 16)); o.push_back(uint8_t(v >> 24));
}
inline void put_u64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back(uint8_t(v >> (8 * i)));
}

// ----- bounds-checked read cursor --------------------------------------------
// Templated on the exception it raises, and carrying its own short-read message,
// because each container has its own error type and wants a diagnostic that names it.
// The cursor itself is identical, so only the failure channel is parameterized.
template <class Err>
struct ReaderT {
    const uint8_t* p;
    size_t         n;
    const char*    eod;          // what to say on a short read -- names the container
    size_t         pos = 0;

    void need(size_t k) const {
        if (pos + k > n) throw Err(eod);
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

} // namespace byteio
