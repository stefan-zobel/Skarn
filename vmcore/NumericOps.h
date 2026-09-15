#pragma once

#include <cmath>
#include "Windows.h"
#include "Value.h"

// =============================================================================
// NumericOps.h -- shared inline helpers for the generic (promoting) numeric
// opcodes (SUB / MUL / DIV / MOD / NEG), factored out of the switch interpreter
// (Interpreter.h) so the int->double promotion rule lives in ONE place.
//
// The int->double promotion rule they implement: when BOTH operands are Ints the
// result is a 48-bit Int (wrapping, via
// fromSigned48); as soon as a Double is involved, both operands are promoted to
// double (numAsDouble) and the result is a Double. No non-numeric type check on
// this fast path -- operands are assumed numeric (Int or Double); well-typedness
// is the compiler/linter's job. See "Numbers" in docs/VirtualMachine.md.
//
// ADD is deliberately NOT here: it is overloaded with string concatenation (a GC
// safepoint) and is handled inline in the switch. num_div / num_mod raise the same
// VM-level "Division by zero" SEH trap as DIV_INT/MOD_INT on a zero int divisor
// (a double operand takes the IEEE / std::fmod path and never traps); the raise is
// caught by run_switch's __except, exactly like the tail-call handlers.
// =============================================================================

[[nodiscard]] [[msvc::forceinline]] inline Value num_sub(Value a, Value b) noexcept {
    a.assertNumeric(); b.assertNumeric();
    if (a.isInt() && b.isInt())
        return Value::fromSigned48(a.asSigned48() - b.asSigned48());
    return Value::fromDouble(a.numAsDouble() - b.numAsDouble());
}

[[nodiscard]] [[msvc::forceinline]] inline Value num_mul(Value a, Value b) noexcept {
    a.assertNumeric(); b.assertNumeric();
    if (a.isInt() && b.isInt())
        return Value::fromSigned48(a.asSigned48() * b.asSigned48());
    return Value::fromDouble(a.numAsDouble() * b.numAsDouble());
}

[[nodiscard]] [[msvc::forceinline]] inline Value num_div(Value a, Value b) {
    a.assertNumeric(); b.assertNumeric();
    if (a.isInt() && b.isInt()) {
        const int64_t vb = b.asSigned48();
        if (vb == 0) [[unlikely]] {
            RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr);
        }
        return Value::fromSigned48(a.asSigned48() / vb);
    }
    return Value::fromDouble(a.numAsDouble() / b.numAsDouble());
}

[[nodiscard]] [[msvc::forceinline]] inline Value num_mod(Value a, Value b) {
    a.assertNumeric(); b.assertNumeric();
    if (a.isInt() && b.isInt()) {
        const int64_t vb = b.asSigned48();
        if (vb == 0) [[unlikely]] {
            RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr);
        }
        return Value::fromSigned48(a.asSigned48() % vb);
    }
    return Value::fromDouble(std::fmod(a.numAsDouble(), b.numAsDouble()));
}

[[nodiscard]] [[msvc::forceinline]] inline Value num_neg(Value a) noexcept {
    a.assertNumeric();
    return a.isInt() ? Value::fromSigned48(-a.asSigned48())
                     : Value::fromDouble(-a.asDouble());
}
