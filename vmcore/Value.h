/*
 * Copyright 2026 Stefan Zobel
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <format>
#include "Inline.h"   // SKARN_FORCEINLINE -- the hints on the NaN-boxing predicates below

union Value {

    enum class Type : uint64_t {
        Nil,
        Integer,
        Double,
        Pointer,
        FuncPtr,  // new
        Func,     // script function immediate (fn_id into the function table)
        Bool,
        Error,
        Atom,
        Undef,
        Tomb,
        Unknown
    };

    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isDouble()  const noexcept { return bits < TAG_FUNCPTR; }
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isInt()    const noexcept { return (bits & ~PAYLOAD_MASK) == TAG_INT; }
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isPtr()     const noexcept { return (bits & ~PAYLOAD_MASK) == TAG_PTR;     }
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isBool()   const noexcept { return (bits & ~PAYLOAD_MASK) == TAG_BOOL; }
    // Basic check: Is it even the "Special"-Space (0xFFFA)?
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isSpecial() const noexcept {
        return (bits & ~PAYLOAD_MASK) == TAG_SPECIAL;
    }
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isNil()    const noexcept { return bits == TAG_SPECIAL; } // (TAG_SPECIAL | 0)
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isUndefined() const noexcept {
        return bits == (TAG_SPECIAL | 1ULL);
    }
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isError() const noexcept {
        return (bits & ~PAYLOAD_MASK) == TAG_SPECIAL && (bits & 0xFFULL) == static_cast<uint64_t>(SpecialType::Error);
    }
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isAtom() const noexcept {
        // Check the Special-Tag (0xFFFA) AND Sub-Tag 3 in the lowest byte
        return (bits & ~PAYLOAD_MASK) == TAG_SPECIAL && (bits & 0xFFULL) == static_cast<uint64_t>(SpecialType::Atom);
    }
    // A first-class SCRIPT function value: a GC-invisible immediate carrying a
    // 16-bit function-table id (Sub-Tag 5). This is the capture-FREE function
    // representation (a named fn or a non-capturing lambda referenced as a value);
    // capturing closures are a heap KIND_CLOSURE instead. Distinct from TAG_FUNCPTR,
    // which stays reserved for NATIVE function pointers.
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isFunc() const noexcept {
        return (bits & ~PAYLOAD_MASK) == TAG_SPECIAL && (bits & 0xFFULL) == static_cast<uint64_t>(SpecialType::Func);
    }

    [[nodiscard]] SKARN_FORCEINLINE
    constexpr uint32_t asErrorCode() const noexcept {
        // Since the lower 8 Bit are reserved fo the Sub-Tag (2), 
        // we shift to the right by 8 bit to get to the code
        return static_cast<uint32_t>((bits & PAYLOAD_MASK) >> 8);
    }

    [[nodiscard]] SKARN_FORCEINLINE
    constexpr uint32_t asAtomId() const noexcept {
        assert(isAtom() && "Value is not an atom!");
        // move the lower 8 bit (Sub-Tag) away, to get to the ID
        return static_cast<uint32_t>((bits & PAYLOAD_MASK) >> 8);
    }

    // Function-table id of a Func immediate (the low 16 bits after the sub-tag).
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr uint32_t asFuncId() const noexcept {
        assert(isFunc() && "Value is not a function!");
        // move the lower 8 bit (Sub-Tag) away, to get to the id
        return static_cast<uint32_t>((bits & PAYLOAD_MASK) >> 8);
    }

    [[nodiscard]] SKARN_FORCEINLINE
    constexpr double asDouble() const noexcept { return dbl; }

    // Numeric value as a double, applying the int->double promotion rule: an Int
    // is converted through its signed 48-bit value, a Double is returned as-is.
    // For non-numeric Values the result is unspecified (this is a fast-path helper
    // used by the promoting arithmetic ops, which assume numeric operands).
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr double numAsDouble() const noexcept {
        return isInt() ? static_cast<double>(asSigned48()) : dbl;
    }

    // Debug-only operand type checks for the type-check-free fast-path opcodes
    // (the typed int family and the generic promoting numeric family). In Release
    // (NDEBUG) assert() vanishes, so these are ZERO-cost; in Debug they turn a
    // mis-typed operand -- mis-emitted bytecode / a compiler bug -- into a loud
    // failure at the source instead of a silent wrong result read out of the raw
    // tagged bits downstream.
    SKARN_FORCEINLINE
    void assertInt() const noexcept {
        assert(isInt() && "typed int fast-path opcode: operand must be Int");
    }
    SKARN_FORCEINLINE
    void assertNumeric() const noexcept {
        assert((isInt() || isDouble()) &&
            "numeric fast-path opcode: operand must be Int or Double");
    }

    // Truthiness: coerce any Value to a truth value. The falsy set is exactly
    // { false, Int(0), Double(0.0) incl. the normalized -0.0, Nil, Undefined }; EVERY
    // other value is truthy -- including NaN, heap objects, and the empty string. This
    // differs from JavaScript, where "" and NaN are falsy.
    //
    // Reachable from exactly TWO opcodes, TO_BOOL and LNOT. NOT from BT / BF, which
    // call asBool() and require a real Bool. And the Skarn front end emits neither
    // TO_BOOL nor LNOT: its checker requires Bool at every conditional position, so this
    // is a facility for hand-written bytecode only.
    // See "The instruction set" in docs/VirtualMachine.md.
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isTruthy() const noexcept {
        if (isBool())   return (bits & 1ULL) != 0;
        if (isInt())    return asRaw48() != 0;      // Int(0) is the only falsy int
        if (isDouble()) return dbl != 0.0;          // 0.0/-0.0 falsy; NaN (!= 0.0) truthy
        // Nil and Undefined are the only falsy specials; Ptr / FuncPtr / Atom / Error
        // are all truthy.
        return !(isNil() || isUndefined());
    }

    // Falsy-ness: the exact negation of isTruthy(), factored out so the LNOT opcode
    // (`!x` with truthiness) can share TYPE_CHECK_BODY exactly as TO_BOOL shares it
    // with isTruthy(). rd = Bool(isFalsy(ra)) is `!x` for any Value.
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isFalsy() const noexcept { return !isTruthy(); }

    [[nodiscard]] SKARN_FORCEINLINE
    constexpr int64_t asSigned48() const noexcept {
        // Extract 48 bit and do the Sign-Extension to 64 bit
        return (static_cast<int64_t>(bits & PAYLOAD_MASK) << 16) >> 16;
    }

    [[nodiscard]] SKARN_FORCEINLINE
    constexpr uint64_t asRaw48() const noexcept {
        return bits & PAYLOAD_MASK;
    }

    // can't be constexpr because of reinterpret_cast
    [[nodiscard]] SKARN_FORCEINLINE
    static Value fromPtr(void* p) noexcept {
        return Value{ reinterpret_cast<uint64_t>(p) | TAG_PTR };
    }

    // Native C++ function pointer -- tagged separately so GC can skip it.
    // Must NOT be treated as a heap object.
    [[nodiscard]] SKARN_FORCEINLINE
    static Value fromFuncPtr(void* p) noexcept {
        return Value{ reinterpret_cast<uint64_t>(p) | TAG_FUNCPTR };
    }

    // Works for both TAG_PTR and TAG_FUNCPTR -- strips the tag, returns the raw address.
    [[nodiscard]] SKARN_FORCEINLINE
    void* asPtr() const noexcept {
        return reinterpret_cast<void*>(bits & PAYLOAD_MASK);
    }

    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isFuncPtr() const noexcept {
        // No special case needed anymore -- TAG_FUNCPTR | 0x1 no longer exists
        return (bits & ~PAYLOAD_MASK) == TAG_FUNCPTR;
    }

    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool asBool() const noexcept {
        assert(isBool() && "Value is not a boolean!");
        return (bits & 1ULL) != 0;
    }

    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool operator==(const Value& other) const noexcept {

        // Fast path: identical bit patterns are equal for Int, Bool, Ptr, and
        // canonical-QNaN-normalized Doubles.
        // Exception: IEEE-754 NaN != NaN.
        if (bits == other.bits) [[likely]] {
            return bits != CANONICAL_QNAN; // NaN != NaN, everything else: equal
        }

        // The "Double path"
        // When the bits are different it could nevertheless be numerically
        // equal doubles (e.g., 0.0 == -0.0)
        // We don't need this when we are mapping -0.0 to 0.0
        /*
        if (isDouble() && other.isDouble()) {
            return dbl == other.dbl;
        }
        */

        // Mixed types (e.g., Int 1 == Double 1.0)
        if (isInt() && other.isDouble()) {
            return static_cast<double>(asSigned48()) == other.dbl;
        }
        if (isDouble() && other.isInt()) {
            return dbl == static_cast<double>(other.asSigned48());
        }

        // Everything else (either different types and/or different pointers) is unequal
        return false;
    }

    // For arithmetic: Takes signed 64-bit, truncates to 48-bit and tags it as Int
    [[nodiscard]] SKARN_FORCEINLINE
    static constexpr Value fromSigned48(int64_t i) noexcept {
        // The masking ' & PAYLOAD_MASK' is essential here, when i < 0 
        // or a 64-bit overflow occurred.
        return Value{ (static_cast<uint64_t>(i) & PAYLOAD_MASK) | TAG_INT };
    }

    // For logic shifts: Takes uint64_t (must be already < 2^48 !) and tag it as Int
    [[nodiscard]] SKARN_FORCEINLINE
    static constexpr Value fromRaw48(uint64_t r) noexcept {
        return Value{ (r & PAYLOAD_MASK) | TAG_INT };
    }

    [[nodiscard]] SKARN_FORCEINLINE
    static constexpr Value fromDouble(double d) noexcept {
        if (d == -0.0) {
            // map -0.0 to 0.0 to avoid the problem that
            // 0.0 == -0.0 but their bit patterns are different.
            return Value{ 0x0000'0000'0000'0000ULL };
        }
        // Get the bit pattern for the check
        const uint64_t d_bits = std::bit_cast<uint64_t>(d);

        // IEEE-754 NaN Check: Exponent (Bits 52-62) are all 1.
        // That corresponds to the mask 0x7FF0000000000000
        // When the mantissa (lower 52 Bits) are all > 0 in addition, it's a NaN.
        if ((d_bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
            (d_bits & 0x000F'FFFF'FFFF'FFFFULL) != 0)
        {
            return Value{ CANONICAL_QNAN };
        }
        return Value{ d_bits };
    }

    [[nodiscard]] SKARN_FORCEINLINE
    static constexpr Value fromBool(bool b) noexcept {
        // Take the TAG_BOOL and set the lowest bit to 0 or 1
        return Value{ TAG_BOOL | (b ? 1ULL : 0ULL) };
    }

    [[nodiscard]] SKARN_FORCEINLINE
    static constexpr Value fromNil() noexcept {
        return Value{ TAG_SPECIAL | static_cast<uint64_t>(SpecialType::Nil) };
    }

    // Construct a Undefined
    [[nodiscard]] SKARN_FORCEINLINE
    static constexpr Value fromUndefined() noexcept {
        return Value{ TAG_SPECIAL | static_cast<uint64_t>(SpecialType::Undefined) };
    }

    // Create a specific Error-Code (e.g., NotFound = 404)
    [[nodiscard]] SKARN_FORCEINLINE
    static constexpr Value fromError(uint32_t errorCode) noexcept {
        // Set the Error-Flag and then the code in the remaining bits
        return Value{ TAG_SPECIAL | static_cast<uint64_t>(SpecialType::Error) | (static_cast<uint64_t>(errorCode) << 8) };
    }

    [[nodiscard]] SKARN_FORCEINLINE
    static constexpr Value fromAtom(uint32_t atomId) noexcept {
        // Sub-Tag 3 for Atoms, shift the ID by 8 Bits to the left
        return Value{ TAG_SPECIAL | static_cast<uint64_t>(SpecialType::Atom) | (static_cast<uint64_t>(atomId) << 8) };
    }

    // Construct a first-class script function value from a function-table id.
    // Sub-Tag 5; the id is shifted left by 8 bits, exactly like an Atom.
    [[nodiscard]] SKARN_FORCEINLINE
    static constexpr Value fromFunc(uint32_t fnId) noexcept {
        return Value{ TAG_SPECIAL | static_cast<uint64_t>(SpecialType::Func) | (static_cast<uint64_t>(fnId) << 8) };
    }

    [[nodiscard]] SKARN_FORCEINLINE
    constexpr uint32_t hash() const noexcept {
        uint64_t x = bits;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        x = x ^ (x >> 31);
        return static_cast<uint32_t>(x);
    }

    [[nodiscard]] SKARN_FORCEINLINE
    static constexpr Value tombstone() noexcept {
        // Encoded as TAG_SPECIAL | SpecialType::Tombstone (sub-tag 4).
        // Completely separate from TAG_FUNCPTR -- no overlap, no special cases needed.
        return Value{ TAG_SPECIAL | static_cast<uint64_t>(SpecialType::Tombstone) };
    }

    static constexpr Value nan() noexcept {
        return Value{ CANONICAL_QNAN };
    }

    // Helper method for an open addressing HashTable (since 'bits' is private)
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr bool isTombstone() const noexcept {
        return bits == (TAG_SPECIAL | static_cast<uint64_t>(SpecialType::Tombstone));
    }

    // Only used for debugging
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr Type type() const noexcept {
        if (isSpecial()) {
            if (isNil()) {
                return Type::Nil;
            }
            if (isUndefined()) {
                return Type::Undef;
            }
            if (isTombstone()) {
                return Type::Tomb;
            }
            if (isError()) {
                return Type::Error;
            }
            if (isAtom()) {
                return Type::Atom;
            }
            if (isFunc()) {
                return Type::Func;
            }
        }
        if (isFuncPtr()) {
            return Type::FuncPtr;
        }
        if (isPtr()) {
            return Type::Pointer;
        }
        if (isInt()) {
            return Type::Integer;
        }
        if (isDouble()) {
            return Type::Double;
        }
        if (isBool()) {
            return Type::Bool;
        }
        return Type::Unknown;
    }

    // default constructor with Undefined as standard value
    [[nodiscard]] SKARN_FORCEINLINE
    constexpr Value() noexcept : bits(TAG_SPECIAL | static_cast<uint64_t>(SpecialType::Undefined)) {}

private:
    uint64_t bits;
    double dbl;

    [[nodiscard]] SKARN_FORCEINLINE
    constexpr explicit Value(uint64_t value) noexcept : bits(value) {}

    enum class SpecialType : uint64_t {
        Nil       = 0,
        Undefined = 1,
        Error     = 2, // 0xFFFA...02 (Payload: ErrorCode)
        Atom      = 3, // 0xFFFA...03 (Payload: AtomID), 40-Bit ID space
        Tombstone = 4, // 0xFFFA...04 (HashTable sentinel)
        Func      = 5, // 0xFFFA...05 (Payload: function-table id), GC-invisible
        // 6 up to 255 are still free
    };

    static constexpr uint64_t PAYLOAD_MASK = 0x0000'FFFF'FFFF'FFFFULL;

    // Tags are all in the upper range (NaN-Space)
    static constexpr uint64_t TAG_FUNCPTR = 0xFFF6'0000'0000'0000ULL; // native fn ptr, GC-invisible
    static constexpr uint64_t TAG_PTR     = 0xFFF7'0000'0000'0000ULL; // heap object, GC-visible
    static constexpr uint64_t TAG_INT     = 0xFFF8'0000'0000'0000ULL;
    static constexpr uint64_t TAG_BOOL    = 0xFFF9'0000'0000'0000ULL;
    static constexpr uint64_t TAG_SPECIAL = 0xFFFA'0000'0000'0000ULL;

    static constexpr uint64_t CANONICAL_QNAN = 0x7FF8'0000'0000'0000ULL;
};


// Ensure that the Union is exactly 8 Bytes
static_assert(sizeof(Value) == 8, "Value union must be exactly 8 bytes (64 bits)!");

// Ensure that the Union is trivially copyable (performance, can use memcpy)
#include <type_traits>
static_assert(std::is_trivially_copyable_v<Value>, "Value must be trivially copyable for performance!");

static_assert(Value::Type::Nil     == Value::fromNil().type());
static_assert(Value::Type::Undef   == Value::fromUndefined().type());
static_assert(Value::Type::Tomb    == Value::tombstone().type()); // Tombstone is now in SPECIAL
static_assert(Value::Type::Error   == Value::fromError(404).type());
static_assert(Value::Type::Atom    == Value::fromAtom(25).type());
static_assert(Value::Type::Func    == Value::fromFunc(25).type());
static_assert(Value::Type::Bool    == Value::fromBool(true).type());
static_assert(Value::Type::Integer == Value::fromSigned48(-101).type());
static_assert(Value::Type::Integer == Value::fromRaw48(222'222).type());
static_assert(Value::Type::Double  == Value::fromDouble(0.0).type());
static_assert(Value::Type::Double  == Value::fromDouble(0.23455).type());
static_assert(Value::Type::Double  == Value::nan().type());
// Note: numAsDouble() and the mixed int/double path of operator== read the union's
// `dbl` member, which is never the active member in a constant expression (the
// constexpr constructor always initializes `bits`). They therefore cannot be
// static_assert-ed under MSVC -- like asDouble(), they are constexpr but only
// evaluable at run time. Their behaviour is covered by the runtime tests
// (test_num_promotion, test_double_arith).
// Truthiness falsy/truthy set (non-double cases -- the Double path reads the union's
// `dbl`, which is not the active member in a constant expression, so 0.0/NaN are
// covered by the runtime test test_to_bool instead).
static_assert(!Value::fromBool(false).isTruthy());
static_assert(Value::fromBool(true).isTruthy());
static_assert(!Value::fromSigned48(0).isTruthy());
static_assert(Value::fromSigned48(1).isTruthy());
static_assert(!Value::fromNil().isTruthy());
static_assert(!Value::fromUndefined().isTruthy());
static_assert(Value::fromAtom(3).isTruthy());
static_assert(Value::fromFunc(0).isTruthy());   // a function value is always truthy
// A Func immediate round-trips its id and never collides with the neighbouring
// SPECIAL sub-tags (Atom) or the pointer tags -- it is GC-invisible.
static_assert(Value::fromFunc(1234).isFunc());
static_assert(Value::fromFunc(1234).asFuncId() == 1234);
static_assert(Value::fromFunc(0xFFFF).asFuncId() == 0xFFFF);
static_assert(!Value::fromFunc(3).isAtom());
static_assert(!Value::fromAtom(3).isFunc());
static_assert(!Value::fromFunc(3).isPtr());
static_assert(!Value::fromFunc(3).isFuncPtr());
static_assert(!Value::fromFunc(3).isNil());
// isFalsy() is the exact negation of isTruthy() (the LNOT / `!x` predicate).
static_assert(Value::fromBool(false).isFalsy());
static_assert(!Value::fromBool(true).isFalsy());
static_assert(Value::fromSigned48(0).isFalsy());
static_assert(!Value::fromSigned48(1).isFalsy());
static_assert(Value::fromNil().isFalsy());
static_assert(Value::fromUndefined().isFalsy());
static_assert(!Value::fromAtom(3).isFalsy());
// Nan != NaN
static_assert(Value::nan() != Value::nan());
// hash(-0.0) == hash(0.0)
static_assert(Value::fromDouble(-0.0).hash() == Value::fromDouble(0.0).hash());


// fromFuncPtr is non-constexpr (reinterpret_cast), so we verify isFuncPtr()
// via the tombstone's tag neighbourhood -- tombstone is now in TAG_SPECIAL.
static_assert(!Value::tombstone().isFuncPtr());                // no overlap between tombstone and function pointer tags
static_assert(!Value::tombstone().isPtr());                    // clear separation between tombstone and pointer tags


template <>
struct std::formatter<Value> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const Value& v, std::format_context& ctx) const {
        auto type = v.type();
        switch (type) {
            // Double: everything below TAG_FUNCPTR 0xFFF6...
            case Value::Type::Double  : return std::format_to(ctx.out(), "Dbl({:g})", v.asDouble());
            // Pointer (0xFFF7...)
            case Value::Type::Pointer : return std::format_to(ctx.out(), "Ptr({})", v.asPtr());
            // Integer (0xFFF8...)
            case Value::Type::Integer : return std::format_to(ctx.out(), "Int({})", v.asSigned48());
            // Boolean (0xFFF9...)
            case Value::Type::Bool    : return std::format_to(ctx.out(), "bool({})", v.asBool() ? "true" : "false");
            // Special Types (0xFFFA...)
            case Value::Type::Nil     : return std::format_to(ctx.out(), "nil");
            case Value::Type::Undef   : return std::format_to(ctx.out(), "undefined");
            // Error 0xFFFA...02
            case Value::Type::Error   : return std::format_to(ctx.out(), "Error({})", v.asErrorCode());
            // Atom 0xFFFA...03
            case Value::Type::Atom    : return std::format_to(ctx.out(), "Atom(#{})", v.asAtomId());
            // Func 0xFFFA...05 (script function immediate)
            case Value::Type::Func    : return std::format_to(ctx.out(), "Func(#{})", v.asFuncId());
            // Tombstone
            case Value::Type::Tomb    : return std::format_to(ctx.out(), "tombstone");
            // Function Pointer
            case Value::Type::FuncPtr : return std::format_to(ctx.out(), "FuncPtr({})", v.asPtr());
            // Something went wrong
            default                   : return std::format_to(ctx.out(), "unknown({:g})", v.asDouble());
        }
    }
};
