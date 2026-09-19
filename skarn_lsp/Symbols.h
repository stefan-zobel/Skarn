#pragma once

// =============================================================================
// Symbols.h -- the outline of one file (textDocument/documentSymbol), and the
// source-like rendering of declarations that the outline and hover share.
//
// The outline lists the file's declarations: fn, struct, enum (with its variants),
// trait and impl (with their methods), const. Imports, `use` and top-level statements
// are left out. Names are shown unmangled.
//
// The front end records where a node STARTS, never where it ends, so a symbol's range
// runs from its first line to the line before the next declaration (the last one to the
// end of the file). The selection range -- what the editor highlights -- is the name.
// =============================================================================

#include "Analyze.h"

#include <string>
#include <vector>

namespace svc {
struct GenericParam;
struct Item;
struct Method;
struct Type;
}

namespace lsp {

// LSP SymbolKind values.
enum class SymbolKind {
    Namespace = 3, Class = 5, Method = 6, Enum = 10, Interface = 11, Function = 12,
    Constant = 14, EnumMember = 22, Struct = 23,
};

struct Symbol {
    std::string         name;
    std::string         detail;      // a signature line, or empty
    SymbolKind          kind = SymbolKind::Function;
    Range               range;
    Range               selection;
    std::vector<Symbol> children;
};

// The outline of the file at `path`, which must be the entry or one of the modules of
// `analysis`. Empty when the analysis has no program.
std::vector<Symbol> document_symbols(const AnalysisResult& analysis, const std::string& path);

// `Vec[Int]`, `fn(Int) -> Bool`, `(Int, String)`, `dyn Show` -- a written type as the
// programmer would write it, module prefixes dropped.
std::string render_type(const svc::Type& t);

// A type or signature text with every module path dropped, as in a declaration:
// `Vec[dyn geometry::shapes::Shape]` -> `Vec[dyn Shape]`, `std::core::Result[Int, String]` ->
// `Result[Int, String]`. A module segment is a lowercase identifier followed by `::`.
std::string drop_module_paths(const std::string& s);

// The one-line declaration of an item or a method: `fn add(a: Int, b: Int) -> Int`,
// `struct Point`, `enum Shape`, `trait Show`, `const LIMIT: Int`, `impl Show for Point`.
std::string render_item(const svc::Item& item);
std::string render_method(const svc::Method& m);

// A generic parameter list with its bounds, `[T: Show + Eq, U]`; empty without parameters.
std::string render_generics(const std::vector<svc::GenericParam>& gs);

} // namespace lsp
