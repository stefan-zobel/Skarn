#pragma once

// =============================================================================
// Check.h -- the public entry of the Skarn TYPECHECKER (`svc::check`).
//
// The checker is the ONLY line of defense: types are erased at runtime (the Gleam
// model), so a checker bug is an unsound program. It therefore rejects anything it
// cannot give a definite type -- there is no `Unknown`/`any` escape hatch (that is
// what set the static checker apart from the former dynamic *linter*).
//
// It runs in the two-pass shape the grammar enables: (1) collect every top-level
// signature, then (2) check each body locally against its declared signature
// (bidirectional-local type checking; no global Hindley-Milner). This header
// exposes only the result surface; the machinery lives in Check.cpp.
//
// Diagnostics are COLLECT-ALL: `check` never throws on a type error, it returns
// every error it found (poisoning a bad subtree with `Ty::Error` and continuing).
//
// Namespace `svc`.
// =============================================================================

#include "Ast.h"

#include <cstdint>
#include <string>
#include <vector>

namespace svc {

// A secondary "note" caret: a related source position plus an explanatory label, drawn
// UNDER the primary error's caret (e.g. "expected because of this type annotation" pointing
// at the annotation while the primary caret points at the offending value). Carries its own
// `module` so the two carets can even land in different files (routed like the primary one).
struct DiagLabel {
    std::string text;
    uint32_t line = 0;
    uint32_t col = 0;
    std::string module;
};

// A single type error, with its 1-based source position (from the offending node) and the
// MODULE it originated in (the mangle prefix: "" = the entry program, "util"/"net::http" = an
// imported module, "$prelude" = the prelude). A multi-file driver uses `module` to render the
// caret against the RIGHT file's source. Empty for a single-file program (the entry).
//
// `labels` are OPTIONAL secondary "note" carets (dual expected/found reporting). An empty
// vector == the historical single-caret behavior; it is a TRAILING, defaulted member, so the
// aggregate init `TypeError{msg, line, col, module}` at the emit sites stays valid unchanged.
struct TypeError {
    std::string message;
    uint32_t line = 0;
    uint32_t col = 0;
    std::string module;
    std::vector<DiagLabel> labels;
};

// The outcome of checking a whole program. `ok()` iff no errors were reported.
//
// `warnings` is an ADVISORY tier (must-use Result/Option + unused bindings): a logic-bug
// nudge, never a soundness signal, so it does NOT affect `ok()`. A future `static_vmrun
// --strict` can escalate a non-empty `warnings` list to fatal (mirroring `static_vmrun --strict`).
struct CheckResult {
    std::vector<TypeError> errors;
    std::vector<TypeError> warnings;
    bool ok() const { return errors.empty(); }
};

// Typecheck a parsed program. Fills each `Expr::ty` the checker reaches with a
// resolved `svc::Ty` and returns all diagnostics found. The program is taken by
// non-const reference because the checker annotates the tree (the `ty` slots).
CheckResult check(Program& program);

} // namespace svc
