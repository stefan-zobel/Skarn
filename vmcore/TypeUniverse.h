#pragma once

#include <cstdint>

// =============================================================================
// TypeUniverse.h -- the shared trait-dispatch type universe.
//
// Trait dispatch (PROTO_RESOLVE, Interpreter.h) is single-dispatch on the first
// argument: given a method id and the receiver value, it looks up the concrete
// implementation's function id. The lookup key must span THREE otherwise-separate
// "type worlds" -- immediates (Int/Double/Bool/Nil/Atom/Func, a NaN-box tag),
// heap non-structs (String/Array/Map/Closure, a GcObject kind), and structs (a
// 16-bit struct type id) -- folded into ONE dense integer index.
//
// The fold: a small FIXED block of built-in type ids [0, BUILTIN_COUNT), then the
// struct type ids offset above it -- dense_id(struct) = BUILTIN_COUNT + type_id.
// The VM (which computes the dense id from a live Value) and the compiler (which
// fills the trait table indexed by that dense id) MUST agree on this layout, so
// it lives in one header both sides include. The VM needs only the constant
// BUILTIN_COUNT; the compiler already owns the struct type ids and just adds it.
//
// This header is pure constants -- no Value / GcObject dependency -- so the
// compiler can include it via its ..\vmcore include path without pulling in the
// runtime. The Value->dense-id mapping itself lives VM-side in Interpreter.h.
// =============================================================================
enum BuiltinTid : uint16_t {
    TID_INT     = 0,
    TID_DOUBLE  = 1,
    TID_BOOL    = 2,
    TID_NIL     = 3,
    TID_ATOM    = 4,
    TID_FUNC    = 5,   // first-class function value (a Func immediate)
    TID_STRING  = 6,
    TID_ARRAY   = 7,
    TID_MAP     = 8,
    TID_CLOSURE = 9,
    TID_VEC     = 10,  // growable vector (KIND_VEC)
    TID_BYTES   = 11,  // growable byte buffer (KIND_BYTES)
    BUILTIN_COUNT = 12 // struct dense ids start here: BUILTIN_COUNT + struct_type_id
};

// Trait-table cell sentinel: no implementation of this method for this type.
// The table is fn_id[method_id * width + dense_id]; a cell holding this value
// means the receiver's type does not implement the method -> PROTO_RESOLVE traps.
inline constexpr uint16_t TRAIT_METHOD_NONE = 0xFFFFu;
