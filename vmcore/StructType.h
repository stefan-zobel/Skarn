#pragma once

#include <cstdint>
#include <string>
#include <vector>

// =============================================================================
// StructType -- descriptor for a fixed-shape struct (KIND_OBJECT).
//
// `field_count` is the arity NEW_STRUCT reads to size the KIND_OBJECT payload,
// so it is the single source of truth for a struct type's arity: an object can
// never be allocated with the wrong number of fields. The registry is a flat
// array indexed by the 16-bit type id stored in the object header's `_pad` (see
// GcObject::object_type_id in Heap.h).
//
// `name` + `field_names` are the printable shape, used ONLY by TO_STRING's
// default field dump (Point { x: 1, y: 2 }). They live host-side in the registry
// (this struct is never on the VM heap -- the runtime object carries only the id),
// so carrying std::string / std::vector here is free of any object-layout cost.
// NEW_STRUCT and the GC still touch only `field_count` / the id.
//
// `dump_style` picks how TO_STRING renders an object of this type: an ordinary
// `Struct` (the field dump), a `Tuple` (positional `(e0, e1)`), or a cons-list
// `List` cell (spine walk `[e0, e1, ...]`). The compiler tags its two internal
// fiction types -- `$TupleN` -> Tuple, `$List` -> List -- so tuples and lists
// print in surface syntax instead of leaking their struct encoding. Host-side
// only, like name/field_names: NEW_STRUCT / the GC never read it.
//
// Room for more per-type data (protocol/trait tables, method dispatch) later,
// again WITHOUT touching object layout.
// =============================================================================
enum class DumpStyle : uint8_t {
    Struct     = 0,   // Name { f0: v0, f1: v1 }  (the default field dump)
    Tuple      = 1,   // (e0, e1)  -- 1-tuple prints as (e0,)
    List       = 2,   // [e0, e1, ...]  -- $List cons-cell spine walk
    NamedTuple = 3,   // Name(e0, e1)  -- tuple-struct; nullary prints as just `Name`
};

struct StructType {
    uint32_t                 field_count;
    std::string              name;         // printable type name (TO_STRING dump)
    std::vector<std::string> field_names;  // ordered field names (TO_STRING dump)
    DumpStyle                dump_style = DumpStyle::Struct;  // TO_STRING render style
};
