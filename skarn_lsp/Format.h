#pragma once

// =============================================================================
// Format.h -- textDocument/formatting: Skarn's one fixed format.
//
// The format (no options): every body -- fn, method, impl, trait, struct, enum, statement block --
// on its own lines with K&R braces and 4 spaces; fields, variants and match arms one per line, a
// comma after each but the last; one blank line between items, two after the use/import block,
// blank lines inside bodies and between methods kept as written (at most one in a row); `name: Type`.
// A line may hold 80 columns: a group that does not fit breaks -- a call, parameter list or literal
// one element per line with the closer on its own line, an operator chain before each operator
// (`|>`, `&&`, `+`, ...). A single-expression lambda or `if` expression, and a struct literal, stay
// on one line when they fit.
//
// The formatter reprints the TOKENS with new whitespace; it never prints the AST, which is lossy
// (interpolation is desugared, parentheses and comments are gone). Every token is kept in its source
// spelling -- strings, raw strings, whole interpolated strings, chars, numbers and comments are
// copied byte for byte -- except commas, which are normalized, and statement ends, which follow
// the layout. Which tokens start a statement, an arm or a member, and what each `{` opens, the
// parser reports (svc::ParseLayout), since statements may share a line with nothing between them.
//
// Line breaks are only placed where Skarn's automatic statement separation cannot cut a
// statement: after `(`, `[`, `{` and `,`, before a closer, and before an operator or `.`.
//
// Before answering, the result is checked: it must parse, its structural AST (svc::dump, without
// positions) must equal the input's, its tokens must equal the input's apart from commas and
// statement ends, and its comments must be the same. Otherwise nothing is changed and the reason
// is returned. A file with a syntax error is not formatted.
// =============================================================================

#include <string>

namespace lsp {

struct FormatResult {
    bool        ok = false;
    std::string text;    // the formatted source (ok)
    std::string error;   // why it was not formatted (!ok)
};

FormatResult format_source(const std::string& source);

} // namespace lsp
