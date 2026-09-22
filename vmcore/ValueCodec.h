#pragma once

// =============================================================================
// ValueCodec.h -- a value graph to a flat byte buffer and back.
//
// WHY A BUFFER AND NOT A DIRECT HEAP-TO-HEAP COPY. Copying straight from one heap into
// another means allocating in the destination while holding pointers into both, so any
// collection there moves what has already been copied and invalidates the half-built
// graph. Two single-heap passes over a byte buffer remove that problem instead of
// managing it: encode() never allocates, and decode() never looks at the source.
// (Cheney's own forwarding records cannot stand in for this: they are destructive on the
// source header and their offsets are relative to the owning semispace base.)
//
// WHAT THE FORMAT DELIBERATELY DOES NOT HAVE. No magic, no version, no CRC. A buffer is
// produced and consumed by the same process running the same image, and never persists.
// That is the whole difference from BytecodeIO.h's SKBC container, which exists precisely
// because an image travels between runs and toolchains. Do not "harden" this into a
// second on-disk format; if a value ever has to survive a process, that is a new
// container with its own versioning, not a tweak to this one.
//
// FORMAT
//   u32 node_count
//   node_count x descriptor, in first-visit order:
//       u8 kind                                   (GcObject::KIND_*)
//       KIND_STRING  : u32 byte_len, then byte_len raw bytes (no NUL -- alloc adds it)
//       every other  : u32 slot_count
//       KIND_OBJECT  : + u16 struct type id
//   node_count x payload, same order, slot-bearing kinds only:
//       per slot: u8 Value::Type, then that type's payload (a Pointer's payload is a
//                 u32 node index; Nil / Undefined / Tombstone carry none)
//   root slot, encoded the same way
//
// SLOTS ARE ENCODED SEMANTICALLY, NOT AS RAW BITS -- and that is a design choice, not a
// workaround for `Value::bits` being private. An immediate is written as its TYPE plus
// that type's payload and rebuilt through the ordinary public factory, so a decoded
// immediate CANNOT be a pointer: the decoder never constructs a Value from an address.
// It also makes the switch exhaustive, so an immediate kind nobody anticipated fails
// loudly here instead of travelling as opaque bits -- the same discipline as the
// switch rot-guards elsewhere in vmcore.
//
// Encoder and decoder are always the same binary (the buffer never leaves the process),
// so using Value::Type's own numbering as the tag is safe: a reordering of that enum
// moves both sides together.
//
// SHARING AND CYCLES FALL OUT OF THE NODE TABLE. Every heap object gets an index on
// first visit and is referred to by that index everywhere else, so a shared subgraph is
// encoded once and rebuilt once, and a cycle terminates because the second visit finds
// an index instead of recursing. Both are day-one requirements, not hardening: two lines
// of ordinary Skarn build a self-referential object.
//
// WHAT IT REFUSES, AND THE ONE IT CANNOT. A KIND_CLOSURE is refused: its captures could
// be anything and its function id belongs to the sender's image (EQ_DEEP refuses closures
// for the same reason). A raw native function pointer (TAG_FUNCPTR) is refused. A `Func`
// IMMEDIATE passes -- it is a 16-bit id into the image, the same category as an Int, not
// a heap object.
//   But the codec CANNOT refuse a native resource handle. A TcpConn is, at this level, a
// KIND_OBJECT holding an Int; nothing in the value distinguishes it from any other
// struct, because only the checker knows nominal types. Copying one would hand a second
// owner a descriptor that means something different -- or nothing -- in the other heap's
// registry. Keeping handles out is therefore a CHECKER obligation (a `Sendable` rule),
// and this codec must not be described as making a value safe to send.
// =============================================================================

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ByteIO.h"
#include "Heap.h"
#include "StructType.h"
#include "Value.h"
#include "opcodes/op_bytes.h"   // the header/backing slot layouts decode() validates
#include "opcodes/op_map.h"
#include "opcodes/op_vec.h"

namespace vcodec {

// Thrown on anything the codec will not do: a refused value on the way out, a malformed
// or truncated buffer on the way in, or a destination heap that cannot fit the graph.
class ValueCodecError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Reader = byteio::ReaderT<ValueCodecError>;
inline constexpr const char* EOD_MSG = "value codec: buffer ended mid-value";

// Structural deep equality, forwarded to the interpreter's own EQ_DEEP worker so a test
// can use the REAL comparison rather than one written beside the code under test. Works
// across two heaps: the compare only dereferences pointers and performs no allocation,
// so neither graph can move under it. Defined in vmcore.cpp.
//
// It answers on a CYCLIC value too, but only about the VALUE, not the graph's SHAPE: two
// values are equal when they unfold alike, so a copy that duplicated a shared subgraph or
// unrolled a cycle into a longer ring still compares equal. Check sharing and cycle
// structure by pointer identity instead.
[[nodiscard]] bool deep_eq(Value a, Value b, Context* ctx);

// =============================================================================
// encode -- value graph -> bytes. Allocates nothing on the GC heap, so the graph
// cannot move underneath it (the property EQ_DEEP relies on for the same reason).
// =============================================================================
[[nodiscard]] inline std::vector<uint8_t> encode(Value root) {
    std::unordered_map<const void*, uint32_t> index;   // payload pointer -> node index
    std::vector<GcObject*>                    nodes;   // by index

    auto intern = [&](Value v) -> uint32_t {
        void* p = v.asPtr();
        if (auto it = index.find(p); it != index.end())
            return it->second;                          // shared or cyclic: reuse the index
        GcObject* o = GcObject::from_slots(p);
        if (o->kind == GcObject::KIND_CLOSURE)
            throw ValueCodecError("value codec: a closure cannot be copied "
                                  "(its captures and its function id belong to one image)");
        const uint32_t idx = static_cast<uint32_t>(nodes.size());
        index.emplace(p, idx);
        nodes.push_back(o);
        return idx;
    };

    // Reachability walk. The worklist IS `nodes`: interning appends, and the loop
    // re-reads the growing size. `o` is a copy of the element, not a reference into the
    // vector, so a reallocation inside intern() cannot invalidate it -- and heap objects
    // themselves never move here, because nothing allocates.
    if (root.isPtr()) intern(root);

    for (size_t i = 0; i < nodes.size(); ++i) {
        GcObject* o = nodes[i];
        if (o->kind == GcObject::KIND_STRING) continue;       // raw bytes, no references
        Value* s = o->slots();
        // Every slot within slot_count is a live Value, Nil or Undefined -- never a stale
        // pointer: vec_pop clears the slot it vacates ("unpin from GC", op_vec.h), and a
        // fresh backing is Undefined-filled by the allocator. So a container's whole
        // backing may be walked and copied, tail included, without chasing garbage.
        for (uint32_t k = 0, n = o->slot_count(); k < n; ++k)
            if (s[k].isPtr()) intern(s[k]);
    }

    std::vector<uint8_t> out;
    byteio::put_u32(out, static_cast<uint32_t>(nodes.size()));

    for (GcObject* o : nodes) {
        byteio::put_u8(out, o->kind);
        if (o->kind == GcObject::KIND_STRING) {
            const uint32_t n = static_cast<uint32_t>(o->string_length());
            byteio::put_u32(out, n);
            out.insert(out.end(), o->bytes(), o->bytes() + n);   // the NUL is re-added on alloc
        } else {
            byteio::put_u32(out, o->slot_count());
            if (o->kind == GcObject::KIND_OBJECT)
                byteio::put_u16(out, o->object_type_id());
        }
    }

    auto put_slot = [&](Value v) {
        const Value::Type t = v.type();
        byteio::put_u8(out, static_cast<uint8_t>(t));
        switch (t) {
        case Value::Type::Pointer: byteio::put_u32(out, index.at(v.asPtr()));                    break;
        case Value::Type::Integer: byteio::put_u64(out, static_cast<uint64_t>(v.asSigned48()));  break;
        // The IEEE bit pattern, not the decimal form: exact, and no formatting anywhere.
        case Value::Type::Double:  byteio::put_u64(out, std::bit_cast<uint64_t>(v.asDouble()));  break;
        case Value::Type::Bool:    byteio::put_u8 (out, v.asBool() ? 1 : 0);                     break;
        case Value::Type::Error:   byteio::put_u32(out, v.asErrorCode());                        break;
        case Value::Type::Atom:    byteio::put_u32(out, v.asAtomId());                           break;
        case Value::Type::Func:    byteio::put_u32(out, v.asFuncId());                           break;
        case Value::Type::Nil:
        case Value::Type::Undef:
        case Value::Type::Tomb:    /* the tag alone carries it */                                break;
        case Value::Type::FuncPtr:
            throw ValueCodecError("value codec: a native function pointer cannot be copied");
        case Value::Type::Unknown:
        default:
            throw ValueCodecError("value codec: unsupported value kind");
        }
    };

    for (GcObject* o : nodes) {
        if (o->kind == GcObject::KIND_STRING) continue;
        Value* s = o->slots();
        for (uint32_t k = 0, n = o->slot_count(); k < n; ++k) put_slot(s[k]);
    }
    put_slot(root);
    return out;
}

// =============================================================================
// decode -- bytes -> a value graph in `dst`.
//
// The caller supplies a FRESH RootedValuePool and must keep it alive for as long as the
// result is needed: every rebuilt node lives in one of its slots, and those slots are the
// only GC roots holding the graph. The pool is also the relocation table -- a node index
// names an object no matter how often the collector moves it, which is what lets the
// patch pass wire up objects that were allocated before their referents existed.
//
// `ctx` MUST be the context that roots `dst`, or null. It is used only by the
// allocator's collect-and-retry ladder -- but a collection forwards whatever that
// context names, so the rule cuts both ways and both halves matter:
//   * passing a context belonging to a DIFFERENT heap is a bug. Its register window
//     would be scanned as if those pointers were dst's, and the collector would copy
//     another heap's objects into this one.
//   * passing null when `dst` DOES have other live objects is equally a bug: a
//     collection would then scan only the heap's registered roots and reclaim
//     everything the omitted context was holding.
// Null is correct exactly when dst has no live values besides the ones being rebuilt --
// the standalone case, and the one the tests use.
//
// `types` / `type_count` are the image's struct-type table (what VM::struct_types holds);
// every KIND_OBJECT is checked against it.
//
// WHAT A MALFORMED BUFFER CAN AND CANNOT DO. Immediates are rebuilt through their public
// factories, so a bad slot yields a wrong VALUE, never a forged pointer. Heap objects need
// more than that, because the interpreter trusts their shape: it indexes a struct by its
// type's field count, a Vec/Bytes by its count, and probes a Map until it meets an empty
// slot. So decode() also refuses any object the VM could not have built:
//   * a struct whose type id is outside the table or whose slot count is not that
//     type's field count;
//   * a Vec / Bytes / Map header of the wrong size, whose backing is not a node of the
//     right kind, or whose count is not an Int within the backing's capacity;
//   * a Map whose backing is not 2 * a power of two, whose count / used disagree with
//     the keys actually in the backing, that leaves no empty slot (a probe would never
//     end), or that holds a pointer key that is not a string;
//   * a backing referenced from anywhere but its one header. The VM never shares a
//     backing, and a shared one would let a Vec write straight into a Map's key slots.
// What remains is a wrong value inside a well-formed object -- a key at the wrong probe
// position, a duplicate key, an odd immediate in a field. Those produce wrong answers,
// not out-of-bounds accesses.
// =============================================================================
[[nodiscard]] inline Value decode(const uint8_t* data, size_t size,
                                  Heap& dst, Context* ctx, RootedValuePool& pool,
                                  const StructType* types, std::size_t type_count) {
    // A zeroed Context scans nothing: an empty frame, an empty return stack, and no VM
    // (forward_vm_external_roots returns at once on a null vm). So the collector sees
    // exactly the heap's registered roots, which at that point is this pool.
    Context standalone{};
    if (!ctx) ctx = &standalone;

    Reader r{ data, size, EOD_MSG };
    const uint32_t count = r.u32();

    struct Desc {
        uint8_t     kind    = 0;
        uint32_t    n       = 0;   // slot count, or byte length for a string
        uint16_t    type_id = 0;
        const char* str     = nullptr;
    };
    std::vector<Desc> descs;
    descs.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Desc d;
        d.kind = r.u8();
        switch (d.kind) {
        case GcObject::KIND_STRING:
            d.n = r.u32();
            r.need(d.n);
            d.str = reinterpret_cast<const char*>(r.p + r.pos);
            r.pos += d.n;
            break;
        case GcObject::KIND_ARRAY:
            d.n = r.u32();
            break;
        case GcObject::KIND_VEC:
        case GcObject::KIND_BYTES:
        case GcObject::KIND_MAP:
        {
            d.n = r.u32();
            const uint32_t want = d.kind == GcObject::KIND_MAP ? MAP_HDR_SLOTS
                                : d.kind == GcObject::KIND_VEC ? VEC_HDR_SLOTS
                                                               : BYTES_HDR_SLOTS;
            if (d.n != want)
                throw ValueCodecError("value codec: container header has the wrong size");
            break;
        }
        case GcObject::KIND_OBJECT:
            d.n       = r.u32();
            d.type_id = r.u16();
            if (!types || d.type_id >= type_count)
                throw ValueCodecError("value codec: struct type id not in this image");
            if (d.n != types[d.type_id].field_count)
                throw ValueCodecError("value codec: struct slot count does not match its type");
            break;
        case GcObject::KIND_CLOSURE:
            throw ValueCodecError("value codec: a closure cannot be rebuilt");
        default:
            throw ValueCodecError("value codec: unknown object kind in buffer");
        }
        descs.push_back(d);
    }

    // Size the pool ONCE and root every slot BEFORE the first allocation: the addresses
    // handed to add_root must stay stable, and from here on any allocation may collect.
    pool.heap = &dst;
    pool.slots.resize(count);
    for (Value& s : pool.slots) dst.add_root(&s);

    // --- allocation pass. May collect; everything already built is rooted. -----------
    // Slot-bearing objects are left with the Undefined the allocator wrote. That is
    // load-bearing: Debug's verify_heap asserts after every collection that each interior
    // pointer slot lies inside the active semispace, so an object must never hold a
    // source-heap pointer, not even briefly.
    for (uint32_t i = 0; i < count; ++i) {
        const Desc& d = descs[i];
        GcObject*   o = nullptr;
        if (d.kind == GcObject::KIND_STRING)
            o = dst.alloc_string_gc(std::string_view(d.str, d.n), ctx);
        else if (d.kind == GcObject::KIND_OBJECT)
            o = dst.alloc_object_gc(d.type_id, d.n, ctx);
        else
            o = dst.alloc_slots_gc(d.kind, d.n, ctx);
        if (!o)
            throw ValueCodecError("value codec: destination heap cannot hold the value");
        pool.slots[i] = Value::fromPtr(o->payload());
    }

    // Every immediate is rebuilt through its ordinary public factory, so a bad slot is at
    // worst a wrong VALUE -- never a forged pointer, and never an address the collector
    // would then try to follow. Each node reference is counted, and the index of the one
    // read last is kept, for the backing checks after the patch pass.
    constexpr uint32_t   NO_NODE = UINT32_MAX;
    std::vector<uint32_t> refs(count, 0);
    uint32_t              last_node = NO_NODE;
    auto read_slot = [&]() -> Value {
        last_node = NO_NODE;
        switch (static_cast<Value::Type>(r.u8())) {
        case Value::Type::Pointer: {
            const uint32_t idx = r.u32();
            if (idx >= count)
                throw ValueCodecError("value codec: node reference out of range");
            ++refs[idx];
            last_node = idx;
            return pool.slots[idx];
        }
        case Value::Type::Integer: return Value::fromSigned48(static_cast<int64_t>(r.u64()));
        case Value::Type::Double:  return Value::fromDouble(std::bit_cast<double>(r.u64()));
        case Value::Type::Bool:    return Value::fromBool(r.u8() != 0);
        case Value::Type::Error:   return Value::fromError(r.u32());
        case Value::Type::Atom:    return Value::fromAtom(r.u32());
        case Value::Type::Func:    return Value::fromFunc(r.u32());
        case Value::Type::Nil:     return Value::fromNil();
        case Value::Type::Undef:   return Value::fromUndefined();
        case Value::Type::Tomb:    return Value::tombstone();
        default:
            throw ValueCodecError("value codec: bad value tag in buffer");
        }
    };

    // --- patch pass. Allocates nothing, therefore collects nothing, therefore every
    // pointer written here stays valid for the rest of the pass. -----------------------
    // A header's backing is its slot 0; remember which node that was.
    std::vector<uint32_t> backing_of(count, NO_NODE);
    for (uint32_t i = 0; i < count; ++i) {
        if (descs[i].kind == GcObject::KIND_STRING) continue;
        Value* s = GcObject::from_slots(pool.slots[i].asPtr())->slots();
        for (uint32_t k = 0; k < descs[i].n; ++k) {
            s[k] = read_slot();
            if (k == 0) backing_of[i] = last_node;
        }
    }
    const Value root = read_slot();

    // --- shape checks. Every slot is written now, so a Map's keys can be counted. ----
    auto count_in = [](Value v, uint64_t cap) {
        return v.isInt() && v.asSigned48() >= 0 && static_cast<uint64_t>(v.asSigned48()) <= cap;
    };
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t kind = descs[i].kind;
        if (kind != GcObject::KIND_VEC && kind != GcObject::KIND_BYTES && kind != GcObject::KIND_MAP)
            continue;
        const uint32_t b = backing_of[i];
        const uint8_t  want = kind == GcObject::KIND_BYTES ? GcObject::KIND_STRING : GcObject::KIND_ARRAY;
        if (b == NO_NODE || descs[b].kind != want)
            throw ValueCodecError("value codec: container backing has the wrong kind");
        if (refs[b] != 1)
            throw ValueCodecError("value codec: container backing is shared");
        const Value* h = GcObject::from_slots(pool.slots[i].asPtr())->slots();
        const uint64_t cap = descs[b].n;   // slots, or bytes for a Bytes backing
        if (kind == GcObject::KIND_VEC) {
            if (!count_in(h[VEC_SLOT_COUNT], cap))
                throw ValueCodecError("value codec: vector count out of range");
        } else if (kind == GcObject::KIND_BYTES) {
            if (!count_in(h[BYTES_SLOT_COUNT], cap))
                throw ValueCodecError("value codec: byte buffer count out of range");
        } else {
            const uint64_t pairs = cap / 2;
            if (cap % 2 != 0 || pairs == 0 || !std::has_single_bit(pairs))
                throw ValueCodecError("value codec: map backing is not a power-of-two table");
            const Value* kv   = GcObject::from_slots(pool.slots[b].asPtr())->slots();
            uint64_t     live = 0, tombs = 0;
            for (uint64_t p = 0; p < pairs; ++p) {
                const Value k = kv[2 * p];
                if (k.isUndefined()) continue;
                if (k.isTombstone()) { ++tombs; continue; }
                if (k.isPtr() && GcObject::from_slots(k.asPtr())->kind != GcObject::KIND_STRING)
                    throw ValueCodecError("value codec: map key is neither a string nor an immediate");
                ++live;
            }
            if (!count_in(h[MAP_SLOT_COUNT], pairs) || !count_in(h[MAP_SLOT_USED], pairs) ||
                static_cast<uint64_t>(h[MAP_SLOT_COUNT].asSigned48()) != live ||
                static_cast<uint64_t>(h[MAP_SLOT_USED].asSigned48()) != live + tombs)
                throw ValueCodecError("value codec: map count / used disagree with its keys");
            if (live + tombs >= pairs)
                throw ValueCodecError("value codec: map has no empty slot");
        }
    }
    return root;
}

} // namespace vcodec
