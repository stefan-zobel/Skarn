#pragma once

// =============================================================================
// Query.h -- position queries over a checked program: hover, go-to-definition,
// find-references, document highlight and rename.
//
// The front end records where a node STARTS, and the parser additionally records the
// token of every name -- the tail of a qualified path (`util::double`, `Shape::Square`,
// `Tr::m`), a member after `.`, a declared name. A query maps the cursor to the
// identifier or number token under it and looks for the name recorded there.
//
// What a name resolves to:
//   * a use of a top-level fn / const / struct / enum / variant / trait: the checker
//     writes the MANGLED name back into the use (`util::f`, `$entry::Point`), which is
//     also the declaration's name -- an exact match across all modules;
//   * `Trait::m` / `Type::m`: the mangled head plus the method name; the head itself
//     refers to the trait / type, and the head of `Enum::V` to the enum;
//   * `recv.m(..)` and a bare `m(x)`: the trait the checker dispatched the call to
//     (`resolved_trait`), else through the receiver's static type -- the inherent method
//     of that type, else the trait method its impl provides (for `dyn Tr` or a bounded
//     type parameter, the trait's method); a trait impl's method refers to the trait's;
//   * `recv.field`: the field in the receiver's struct declaration;
//   * a local (let, parameter, match / for / lambda binding): the query walks the file
//     with a scope stack that mirrors the checker's, since the checker keeps no link;
//   * a type name in an annotation is not written back: this module's declaration of
//     that name, else the only user declaration with that name;
//   * a name in a `use` list: the declaration it imports.
// Not resolved: a call the checker could not resolve (an ambiguous `m(x)` records no
// trait), and anything declared in the std, which has no file to open.
//
// Hover shows a declaration's signature for a declared name, and otherwise the type
// the checker inferred for the expression. A builtin such as `len` has no signature of
// its own (it is typed per call), so its hover shows what that call produces:
// `len(..) -> Int`. Nothing is shown where the checker has no type at all.
//
// Rename never guesses. It refuses what it cannot rename completely (a field, anything
// in the std, a trait method while an UNRESOLVED call `m(x)` of that name exists, a
// type name the annotation lookup could not decide), and before answering it applies
// the edits in memory, re-checks every affected program and requires that no new error
// appears and that exactly the edited names -- no more, no fewer -- now refer to the
// renamed declaration. That rules out shadowing capture and collisions.
// =============================================================================

#include "Analyze.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace svc { struct Ty; struct CallExpr; }

namespace lsp {

struct Location {
    std::string path;
    Range       range;
};

// Markdown hover text for the token at `pos` in the file at `path`, or nothing.
std::optional<std::string> hover(const AnalysisResult& analysis, const std::string& path, Position pos);

// Where the name at `pos` in the file at `path` is declared, or nothing.
std::optional<Location> definition(const AnalysisResult& analysis, const std::string& path, Position pos);

// Every place that refers to what the name at `pos` refers to, across every file of every
// analysis in `analyses` that contains `path`; with `include_declaration`, the declaration
// too. Sorted by path, line, column.
std::vector<Location> references(const std::vector<const AnalysisResult*>& analyses, const std::string& path,
                                 Position pos, bool include_declaration);

// The same, restricted to the file at `path`. `write` marks a declaration or an
// assignment target (LSP DocumentHighlightKind Write), the rest are reads.
struct Highlight {
    Range range;
    bool  write = false;
};
std::vector<Highlight> highlights(const AnalysisResult& analysis, const std::string& path, Position pos);

struct TextEdit {
    Range       range;
    std::string new_text;
};

// Either the range and current text of the name at `pos`, or why it cannot be renamed.
struct PrepareRename {
    Range       range;
    std::string placeholder;
    std::string error;   // non-empty: refused
};
PrepareRename prepare_rename(const std::vector<const AnalysisResult*>& analyses, const std::string& path, Position pos);

// Either the edits per file (normalized path -> edits) or why the rename is refused.
struct RenameResult {
    std::map<std::string, std::vector<TextEdit>> changes;
    std::string error;   // non-empty: refused
};
RenameResult rename(const std::vector<const AnalysisResult*>& analyses, const std::string& path, Position pos,
                    const std::string& new_name);

// ---- completion -------------------------------------------------------------------------

// What the name starting at 1-based `line`/`col` of the file at `path` is: completion inserts
// a probe identifier at the cursor, re-analyzes, and asks here what the probe became.
//   * Member: the member of `recv.<probe>`; `receiver` is the type the checker gave `recv`.
//   * Value:  an expression name; `locals` are the bindings in scope there.
//   * Type:   a type name in an annotation.
// When no node sits there (the statement around the cursor was dropped by the error-tolerant
// parser), the answer is Value with `exact` false and the locals of the innermost `{ }` block
// around the position -- those of the statements that start before it. `type_params` are the
// generic parameters in scope there (and `Self` in a trait or impl). `qualified`: the name is the
// tail of a qualified path (`util::<probe>`, `let x: geo::<probe>`).
struct ProbeLocal {
    std::string name;
    std::string type;   // empty when unknown
};
struct Probe {
    enum class Kind { None, Member, Value, Type } kind = Kind::None;
    bool exact = false;
    bool qualified = false;
    std::shared_ptr<const svc::Ty> receiver;
    std::vector<ProbeLocal> locals;   // innermost first, one per name
    std::vector<std::string> type_params;
};
Probe probe(const AnalysisResult& analysis, const std::string& path, uint32_t line, uint32_t col);

// ---- signature help ---------------------------------------------------------------------

// The call whose `(` is at 1-based `line`/`col` of the file at `path` (the parser anchors a call
// there), or null. `piped`: it is the right-hand side of `x |> f(a)`, so `x` is its first
// argument. The node belongs to `analysis`'s program.
struct CallSite {
    const svc::CallExpr* call = nullptr;
    bool piped = false;
};
CallSite call_at(const AnalysisResult& analysis, const std::string& path, uint32_t line, uint32_t col);

} // namespace lsp
