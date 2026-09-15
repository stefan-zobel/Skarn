#pragma once

#include <string>
#include <cstdint>

// =============================================================================
// gen -- a well-typed Skarn PROGRAM GENERATOR for property testing.
//
// generate(seed) emits a random but TYPE-CORRECT-BY-CONSTRUCTION program as source
// text (type-directed, top-down: an expression is only ever assembled from productions
// whose result type is the demanded type and whose sub-parts are generated at the types
// that production requires). Deterministic per `seed` (a seeded std::mt19937), so any
// failure reproduces from its integer seed.
//
// The generated program ends in a scalar expression -- its value is what the differential
// harness (run_diff) compares between the real bytecode VM and the reference interpreter.
// The scalar core covers Int / Double / Bool / String: literals, in-scope variables,
// arithmetic / bitwise / shift, boolean logic, comparisons, string concat, and `if`; later
// slices widened it to control flow, calls, closures, containers, structs / enums / newtypes,
// `?`, structural `==` over composites (gen_compare's EQ_DEEP branch), `@`-bindings over a range
// (gen_at_match), and a transparent-newtype pattern NESTED in a tuple pattern (gen_newtype_match --
// added after that exact shape turned out to hide a real miscompile from 100 000 generated programs).
// Calls are spelled the ways a user writes them: prefix `m(x)`, pipe `x |> m`, method syntax
// `x.m()`, and qualified `Tr::m(x)` / `S::m(x)` / `x |> S::m`; traitless `impl S { .. }` blocks add
// an associated `S::new(..)`, plain and one-argument methods, a `mut self` mutator, and the
// associated-fn-then-method chain `S::new(..).m()`. The prelude combinators appear in method form
// too (`o.unwrapOr(d)`, `r.unwrap()`, `o.expect(..)`, `a.lessThan(b)`).
// It stays inside the reference interpreter's supported, deterministic subset -- only the
// STATELESS, message-independent natives (getEnv / fileExists / readFile on known-absent inputs,
// `args`), no Map `toString` (hash order) -- so a run is signal, not skips.
//
// NOTE the exclusions above are the CURRENT ones. This list was read as authoritative
// once when it was already stale ("no heap-object `==`" long after gen_compare grew that
// branch), so keep it in step with the productions rather than treating it as history.
// =============================================================================
namespace gen {

// Generate a well-typed program from `seed`. Same seed -> same program. When `with_prelude` is
// set, the generator also emits Option/Result productions (Some/None/Ok/Err, `?`, and the prelude
// combinators unwrap/unwrapOr/isSome/isNone/optionMap/resultAndThen) -- valid only when the program
// is compiled WITH the static prelude (run_diff(src, /*with_prelude=*/true)).
std::string generate(uint32_t seed, bool with_prelude = false);

}  // namespace gen
