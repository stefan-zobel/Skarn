#pragma once

// =============================================================================
// Lexer.h -- source text -> token stream for the Skarn front end.
//
// The stream always ends with a single Eof token. Statement boundaries are
// inserted automatically (Swift/Go style ASI) and surface as StmtEnd tokens;
// the exact rule lives in Lexer.cpp. The lexer is self-contained (no vmcore
// dependency) -- only codegen, later, will touch the value layer.
//
// Adapted from the former dynamic lexer; see Token.h for the
// static-grammar deltas (case-based uident/lident, no nil, `self` keyword,
// Double instead of Float). Namespace `svc`.
// =============================================================================

#include "Token.h"
#include <string>
#include <vector>
#include <stdexcept>

namespace svc {

// Thrown on a lexical error (bad character, unterminated string/comment,
// out-of-range numeric literal). Carries the 1-based source position.
class LexError : public std::runtime_error {
public:
    LexError(const std::string& msg, uint32_t line, uint32_t col);
    uint32_t line() const noexcept { return line_; }
    uint32_t col()  const noexcept { return col_; }
private:
    uint32_t line_;
    uint32_t col_;
};

class Lexer {
public:
    explicit Lexer(std::string source);

    // Tokenize the whole input. Throws LexError on malformed input.
    std::vector<Token> tokenize();

private:
    // scanning primitives
    bool at_end() const noexcept { return pos_ >= src_.size(); }
    char peek(size_t off = 0) const noexcept;
    char cur() const noexcept { return peek(0); }
    void advance() noexcept;

    bool  skip_trivia();                              // returns true iff a newline was crossed
    Token scan_token();                               // interpolation-aware wrapper over scan_token_raw
    Token scan_token_raw();                           // the ordinary one-token dispatcher
    Token scan_number(uint32_t line, uint32_t col);
    // Consume one run of digits (per `is_digit_of`) allowing `_` as a DIGIT SEPARATOR. Java's rule:
    // a `_` must sit BETWEEN two digits of the run, so a leading, trailing, or doubled-at-the-edge
    // separator is a LexError rather than being silently mis-lexed as an adjacent identifier. Shared
    // by all four digit runs (decimal integer, fraction, exponent, radix digits) so the rule is
    // stated once. The separators are stripped before `std::from_chars`, never in the token text.
    void  scan_digit_run(bool (*is_digit_of)(char), uint32_t line, uint32_t col);
    Token scan_ident(uint32_t line, uint32_t col);
    Token scan_string(uint32_t line, uint32_t col);
    // Raw string literal `r"…"` / `r#"…"#` (Rust-style hash counting for embedded `"`). A separate
    // scan path that bypasses scan_interp_text/interp_stack_ entirely: NO backslash escapes and NO
    // `${…}` interpolation -- the bytes are taken verbatim (so `\n`/`${x}` are literal). Multi-line
    // is allowed. Emits a plain TokKind::Str, so the parser treats it like any other string literal.
    Token scan_raw_string(uint32_t line, uint32_t col);
    // Scan one string TEXT chunk from pos_ (already past the opening `"` or a hole's closing `}`) up
    // to the next `${` hole or the closing `"`, honoring backslash escapes. `continuation` is false for
    // the leading chunk (emits Str on close, InterpStrBegin on a hole) and true for a chunk after a hole
    // (emits InterpStrEnd on close, InterpStrMid on a hole); it also drives the interp-context stack.
    Token scan_interp_text(uint32_t line, uint32_t col, bool continuation);
    Token scan_char(uint32_t line, uint32_t col);    // char literal '\''X'\'' -> Int (single byte 0..255)
    char  decode_escape();                           // one backslash escape (shared by string/char)
    Token scan_colon(uint32_t line, uint32_t col);
    Token emit(TokKind k, size_t len, uint32_t line, uint32_t col);

    std::vector<Token> apply_asi(const std::vector<Token>& raw);

    std::string src_;
    size_t   pos_  = 0;
    uint32_t line_ = 1;
    uint32_t col_  = 1;
    // True iff the previous real token was a '.' (member access). A number scanned
    // right after a member dot is a TUPLE INDEX -- an integer -- so scan_number must
    // NOT treat a following `.digit` as a decimal point (else `t.0.1` would lex the
    // `.1` into a Double and break the nested tuple access `(t.0).1`).
    bool prev_was_dot_ = false;

    // String-interpolation context stack. One frame per interpolated string whose hole
    // we are currently lexing (a nested `"…${ "…${…}" }…"` pushes a second frame). A
    // non-empty stack at scan_token entry means "inside a `${ … }` hole": `brace_depth`
    // counts `{`/`}` in that hole so the hole-closing `}` (depth 0) is told apart from a
    // struct-literal / block `}` inside it. Empty in ordinary (non-interpolated) code.
    // `pending_spec`/`pending_has_spec` stage a `${e:spec}` format specifier: when a depth-0 single
    // `:` closes the hole expression, scan_token scans the raw spec text (up to the hole-closing `}`)
    // into these, and scan_interp_text moves them onto the InterpStrMid/End token it then emits.
    struct InterpFrame {
        uint32_t    brace_depth = 0;
        uint32_t    paren_depth = 0;    // `(`/`)` inside the current hole
        uint32_t    bracket_depth = 0;  // `[`/`]` inside the current hole
        bool        pending_has_spec = false;
        std::string pending_spec;
    };
    std::vector<InterpFrame> interp_stack_;
};

} // namespace svc
