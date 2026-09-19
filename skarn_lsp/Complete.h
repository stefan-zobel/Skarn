#pragma once

// =============================================================================
// Complete.h -- textDocument/completion: the members after `recv.`, and the names in
// scope elsewhere (types in a type annotation); and textDocument/signatureHelp, which
// shares the probe, the re-analysis and the member resolution (see the end).
//
// At `p.` the text does not parse, so the tree has no node at the cursor. Completion
// therefore inserts a PROBE identifier at the cursor (`p.` -> `p.<probe>`, `pri` ->
// `pri<probe>`; capitalized where nothing is typed yet and a name is completed, because a
// type position parses only an uppercase name), re-analyzes the program with that text, and
// asks what the probe became (Query.h, probe()): the member of a field access -> the members
// of the receiver's type; an expression name -> the locals in scope there plus the global
// names; a type name -> the types. A line still being typed often fails to parse even with
// the probe -- `push(v, x.` lacks its `)`, `if p.` its block -- so a second attempt gives an
// unfinished `if` / `while` / `match` / `for` head an empty `{}`, closes every bracket, brace
// and `${` hole the line opened, and completes a `let` without its value (` = 0`) and a `fn`
// header without its body (` {}`). When the statement around the cursor was dropped all
// the same, the names come from the innermost block around it, types included, since the
// position is unknown.
//
// Whether the cursor is in code at all is decided by the lexer: the text up to the cursor
// plus the probe must end in an identifier token that holds the probe -- inside a string or
// a comment it does not. Nothing is offered where a new name is declared (after `let`,
// `fn`, `struct`, ...), in an `import` path, and after a number's `.`.
//
// After `::` the path before it decides, read from the tokens; an expression or type path has
// one segment (the checker's rule), a `use` path any number. `Type::` (the head resolved as the
// checker's `mangle_ref` does: this module, its `use` items, the ring): an enum's variants and
// the fns of the type's inherent impls, or a trait's methods. `module::`: the `pub` items of a
// module this file imports, or of the ring for `std::`; only here is the probe asked, whether
// the position is a type. `use a::b::` and `use a::b::{x, `: the modules below `a::b`, its `pub`
// items and the natives it gates, or the variants of enum `b` in module `a`. These answers come
// from the current tree -- its items survive a line that does not parse.
//
// Members: the fields of a struct, the methods with `self` of its inherent impls, and the
// methods of every trait the type implements -- by an impl on its head, or by a blanket
// impl whose bounds the head meets (checked one level deep); for `dyn Tr` and a bounded
// type parameter, the traits' methods and their supertraits'. An impl's own bounds are not
// checked, so `Timed[Int]` may be offered a method only `Timed[T: Named]` has.
//
// Names: the locals, the generic parameters in scope (types only), this file's declarations,
// the `pub` items of the ring modules (`std::core`, `std::iter`, `std::string`), what this
// file's `use` items bring in, the modules it imports (for `m::name`), the trait methods
// (callable bare: `area(s)`), the builtins and the natives reachable from here, and the
// keywords.
//
// Signature help: the lexer finds the call the cursor is in -- the innermost open `(` after
// a name, `)` or `]`, not crossing a `{` or `${` -- and the commas at its depth give the
// active argument. The call node is looked up by the position of that `(` (Query.h,
// call_at()) in the current tree; while the line does not parse, in a re-analysis with the
// probe where an argument is missing, then once more with the line finished. The callee:
// a fn; `recv.m` through the receiver's type, with `self` shown but no argument; `Type::m`
// / `Trait::m` with `self` as the first argument; a tuple variant or tuple struct; a local of
// `fn` type; a builtin or native from its ambient signature, one signature per line; for a
// bare trait-method call `m(x)`, every trait's `m`. The right side of `x |> f(a)` counts `x`.
// =============================================================================

#include "Analyze.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lsp {

// LSP CompletionItemKind values.
enum class CompletionKind {
    Method = 2, Function = 3, Field = 5, Variable = 6, Interface = 8, Keyword = 14,
    Module = 9, Enum = 13, EnumMember = 20, Constant = 21, Struct = 22, TypeParameter = 25,
};

struct CompletionItem {
    std::string    label;
    CompletionKind kind = CompletionKind::Variable;
    std::string    detail;   // a signature or a type, or empty
    std::string    sort;     // the group, then the label: locals and members first, keywords last
};

// The candidates at `pos` in the file at `path`, which `analysis` must cover. The client
// filters them by the word typed so far.
std::vector<CompletionItem> complete(const AnalysisResult& analysis, const std::string& path, Position pos);

// ---- signature help ---------------------------------------------------------------------

struct SignatureInfo {
    std::string label;                                    // `fn add(a: Int, b: Int) -> Int`
    std::vector<std::pair<uint32_t, uint32_t>> params;    // each parameter's [start, end) in the label
    bool variadic = false;                                // the last parameter is `...`
};

struct SignatureHelp {
    std::vector<SignatureInfo> signatures;
    int active_signature = 0;
    int active_parameter = 0;   // may lie past the last parameter: too many arguments
};

// The signature of the call the cursor at `pos` is in, and which argument it is at; nothing
// outside a call (and inside a lambda body or a string among its arguments).
std::optional<SignatureHelp> signature_help(const AnalysisResult& analysis, const std::string& path, Position pos);

} // namespace lsp
