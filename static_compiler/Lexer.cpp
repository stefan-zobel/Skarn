// =============================================================================
// Lexer.cpp -- implementation of the Skarn front-end lexer.
//
// Two passes: a raw scan that emits real tokens interleaved with internal
// Newline markers, then an ASI pass (apply_asi) that turns qualifying line
// breaks into StmtEnd terminators and drops the rest.
//
// Automatic statement termination (ASI), Swift/Go flavoured: a line break
// becomes a StmtEnd iff the previous real token can END a statement AND the
// next real token does NOT CONTINUE one. Consequences worth knowing:
//   * an opening brace must sit on the same line as its header (`if c {`), like
//     Go -- a `{` on the next line is not a continuation token and would be cut
//     off by an inserted StmtEnd;
//   * a leading infix operator / `|>` / `.` on the next line continues the
//     previous line, which is what makes multi-line pipe chains read well.
//
// Identifiers are case-split IN THE LEXER: an uppercase-initial identifier is a
// UIdent (type / constructor / trait / type var), a lowercase-or-`_`-initial one
// is an LIdent (value / function / field). A lone `_` is the wildcard. Keywords
// (all lowercase) are matched first, so they never reach the case test.
//
// `:` is only ever an annotation / field separator (`x: Int`, `Point { x: 1 }`) or
// half of `::`. Atoms (`:ok`) were removed with the dynamic front end, so the
// context-sensitive "is this a value-ending char?" test that used to disambiguate
// them is gone too.
// =============================================================================

#include "Lexer.h"

#include <charconv>
#include <string_view>

namespace svc {

// ---- character classes ------------------------------------------------------

static bool is_digit(char c)       { return c >= '0' && c <= '9'; }
static bool is_hex_digit(char c)   { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static bool is_octal_digit(char c) { return c >= '0' && c <= '7'; }
static bool is_binary_digit(char c){ return c == '0' || c == '1'; }
static bool is_upper(char c)       { return c >= 'A' && c <= 'Z'; }
static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_ident_cont(char c)  { return is_ident_start(c) || is_digit(c); }

// Returns the keyword kind for `s`, Underscore for a lone `_`, or TokKind::LIdent
// as a sentinel meaning "not a keyword". Keywords are all lowercase, so a genuine
// UIdent (uppercase-initial) never matches here -- scan_ident promotes the LIdent
// sentinel to UIdent by first-character case afterwards.
static TokKind keyword_kind(const std::string& s) {
    if (s == "_")      return TokKind::Underscore;
    if (s == "let")    return TokKind::KwLet;
    if (s == "mut")    return TokKind::KwMut;
    if (s == "fn")     return TokKind::KwFn;
    if (s == "struct") return TokKind::KwStruct;
    if (s == "enum")   return TokKind::KwEnum;
    if (s == "match")  return TokKind::KwMatch;
    if (s == "if")     return TokKind::KwIf;
    if (s == "else")   return TokKind::KwElse;
    if (s == "while")  return TokKind::KwWhile;
    if (s == "for")    return TokKind::KwFor;
    if (s == "in")     return TokKind::KwIn;
    if (s == "loop")   return TokKind::KwLoop;
    if (s == "break")  return TokKind::KwBreak;
    if (s == "continue") return TokKind::KwContinue;
    if (s == "trait")  return TokKind::KwTrait;
    if (s == "dyn")    return TokKind::KwDyn;
    if (s == "impl")   return TokKind::KwImpl;
    if (s == "import") return TokKind::KwImport;
    if (s == "use")    return TokKind::KwUse;
    if (s == "pub")    return TokKind::KwPub;
    if (s == "const")  return TokKind::KwConst;
    if (s == "transparent") return TokKind::KwTransparent;
    if (s == "return") return TokKind::KwReturn;
    if (s == "true")   return TokKind::KwTrue;
    if (s == "false")  return TokKind::KwFalse;
    if (s == "self")   return TokKind::KwSelf;
    return TokKind::LIdent;   // sentinel: not a keyword
}

// ---- ASI predicates ---------------------------------------------------------

// A token that can legitimately be the last token of a statement.
static bool ends_value(TokKind k) {
    switch (k) {
        case TokKind::Int:      case TokKind::Double:  case TokKind::Str:
        case TokKind::UIdent:   case TokKind::LIdent:
        case TokKind::Underscore:
        case TokKind::RParen:   case TokKind::RBracket:case TokKind::RBrace:
        case TokKind::KwTrue:   case TokKind::KwFalse: case TokKind::KwSelf:
        case TokKind::KwReturn:
        case TokKind::KwBreak:  case TokKind::KwContinue:
        case TokKind::Question:                        // `e?` completes a value
        case TokKind::InterpStrEnd:                    // a closed `"…${…}…"` completes a value
            return true;
        default:
            return false;
    }
}

// A token that, appearing first on the next line, continues the previous one.
static bool starts_continuation(TokKind k) {
    switch (k) {
        case TokKind::Pipe:    case TokKind::Dot:     case TokKind::DotDot:
        case TokKind::DotDotLess:
        case TokKind::OrOr:    case TokKind::AndAnd:
        case TokKind::EqEq:    case TokKind::NotEq:
        case TokKind::Lt:      case TokKind::Le:      case TokKind::Gt:  case TokKind::Ge:
        case TokKind::BitOr:   case TokKind::BitXor:  case TokKind::BitAnd:
        case TokKind::Shl:     case TokKind::Shr:     case TokKind::UShr:
        case TokKind::Plus:    case TokKind::Minus:   case TokKind::Star:
        case TokKind::Slash:   case TokKind::Percent:
        case TokKind::FatArrow:case TokKind::Arrow:
        case TokKind::Comma:   case TokKind::Colon:   case TokKind::ColonColon:
        // `@` binds a pattern to a name; a line opening with `@ 1..100` continues the `n` above it.
        // Deliberately NOT in `ends_value` -- that asymmetry is what lets `n @` end a line.
        case TokKind::At:
        case TokKind::Assign:
        // The compound forms follow `Assign`: a line opening with `= 5` already continues the
        // previous one, so `+= 5` must too, or the two spellings would disagree on line breaks.
        case TokKind::PlusEq:  case TokKind::MinusEq: case TokKind::StarEq:
        case TokKind::SlashEq: case TokKind::PercentEq:
        case TokKind::BitAndEq:case TokKind::BitOrEq: case TokKind::BitXorEq:
        case TokKind::ShlEq:   case TokKind::ShrEq:   case TokKind::UShrEq:
        case TokKind::KwElse:
        case TokKind::RParen:  case TokKind::RBracket:case TokKind::RBrace:
        // A `}`-close of an interpolation hole on the previous line lexes to InterpStrMid/End: it
        // continues the same interpolated-string value (`"a ${ x\n}b"`), so never cut before it.
        case TokKind::InterpStrMid: case TokKind::InterpStrEnd:
            return true;
        default:
            return false;
    }
}

// ---- LexError ---------------------------------------------------------------

LexError::LexError(const std::string& msg, uint32_t line, uint32_t col)
    : std::runtime_error(msg + " at line " + std::to_string(line) + ":" + std::to_string(col)),
      line_(line), col_(col) {}

// ---- Lexer ------------------------------------------------------------------

Lexer::Lexer(std::string source) : src_(std::move(source)) {}

char Lexer::peek(size_t off) const noexcept {
    size_t i = pos_ + off;
    return i < src_.size() ? src_[i] : '\0';
}

void Lexer::advance() noexcept {
    if (at_end()) return;
    if (src_[pos_] == '\n') { ++line_; col_ = 1; }
    else                    { ++col_; }
    ++pos_;
}

bool Lexer::skip_trivia() {
    bool saw_newline = false;
    for (;;) {
        char c = cur();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else if (c == '\n') {
            saw_newline = true;
            advance();
        } else if (c == '/' && peek(1) == '/') {
            advance(); advance();
            while (!at_end() && cur() != '\n') advance();
        } else if (c == '/' && peek(1) == '*') {
            uint32_t sl = line_, sc = col_;
            advance(); advance();
            int depth = 1;
            while (depth > 0) {
                if (at_end()) throw LexError("unterminated block comment", sl, sc);
                if (cur() == '/' && peek(1) == '*')      { advance(); advance(); ++depth; }
                else if (cur() == '*' && peek(1) == '/') { advance(); advance(); --depth; }
                else { if (cur() == '\n') saw_newline = true; advance(); }
            }
        } else {
            break;
        }
    }
    return saw_newline;
}

Token Lexer::emit(TokKind k, size_t len, uint32_t line, uint32_t col) {
    Token t;
    t.kind = k;
    t.text = src_.substr(pos_, len);
    t.line = line;
    t.col  = col;
    for (size_t i = 0; i < len; ++i) advance();
    return t;
}

// Strip digit separators so `std::from_chars` sees only digits. Called on the literal's own span,
// which the scanner has already validated -- so this is a copy, not a parse.
static std::string without_separators(std::string_view digits) {
    std::string out;
    out.reserve(digits.size());
    for (char c : digits)
        if (c != '_') out.push_back(c);
    return out;
}

void Lexer::scan_digit_run(bool (*is_digit_of)(char), uint32_t line, uint32_t col) {
    while (is_digit_of(cur()) || cur() == '_') {
        if (cur() == '_') {
            // Every caller enters this run ON a digit, so "a digit precedes the separator" is given
            // and only the forward side can fail. Skipping the whole `_` run first is what accepts
            // Java's (and Rust's) `1__0` while still rejecting `1_`, `1__`, `1_x` and `1_.5`.
            // Rejecting rather than just stopping is the point: `1_x` used to lex as `1` followed by
            // an identifier `_x` -- always a parse error later, but with a mystifying message.
            size_t k = 1;
            while (peek(k) == '_') ++k;
            if (!is_digit_of(peek(k)))
                throw LexError("digit separator '_' must be between two digits", line, col);
        }
        advance();
    }
}

Token Lexer::scan_number(uint32_t line, uint32_t col) {
    size_t start = pos_;

    // Radix-prefixed integer literals: 0x/0X hex, 0o/0O octal, 0b/0B binary. Option B -- the digits
    // are a BIT PATTERN up to the full 48-bit width, sign-extended from bit 47, so `0xFFFFFFFFFFFF`
    // is -1 (a full-width all-ones mask) and `0x800000000000` is MIN_48. This differs from a decimal
    // literal (a signed magnitude that must be < 2^47), as in Rust/Java/C. Guarded on a valid first
    // digit so a bare `0` / `0.5` / `0e5` still take the decimal/double path; guarded on !prev_was_dot_
    // so a tuple index `t.0` stays decimal; a malformed `0xG` falls through to `0` + an identifier.
    if (!prev_was_dot_ && cur() == '0') {
        const char p1 = peek(1);
        // A digit separator may not open the digit run. Caught HERE rather than left to the
        // fall-through: `0x_FF` would otherwise lex as `0` plus an identifier `x_FF`, which reports
        // an unknown variable instead of the actual mistake.
        if ((p1 == 'x' || p1 == 'X' || p1 == 'o' || p1 == 'O' || p1 == 'b' || p1 == 'B') && peek(2) == '_')
            throw LexError("digit separator '_' must be between two digits", line, col);
        int radix = 0;
        if      ((p1 == 'x' || p1 == 'X') && is_hex_digit(peek(2)))    radix = 16;
        else if ((p1 == 'o' || p1 == 'O') && is_octal_digit(peek(2)))  radix = 8;
        else if ((p1 == 'b' || p1 == 'B') && is_binary_digit(peek(2))) radix = 2;
        if (radix != 0) {
            advance(); advance();                                     // consume the "0x"/"0o"/"0b" prefix
            const size_t dstart = pos_;
            if      (radix == 16) scan_digit_run(is_hex_digit,    line, col);
            else if (radix == 8)  scan_digit_run(is_octal_digit,  line, col);
            else                  scan_digit_run(is_binary_digit, line, col);
            // Separators are stripped for the value; `t.text` below keeps the original spelling, so a
            // diagnostic quotes what was written.
            const std::string digits = without_separators(
                std::string_view(src_).substr(dstart, pos_ - dstart));
            uint64_t uval = 0;
            const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), uval, radix);
            if (r.ec != std::errc{} || uval > 0xFFFFFFFFFFFFull)      // 2^48 - 1 = 281474976710655
                throw LexError("integer literal out of range for a 48-bit Int", line, col);
            constexpr uint64_t BIT48 = 1ull << 48, SIGN = 1ull << 47;
            Token t;
            t.line = line;
            t.col  = col;
            t.text = src_.substr(start, pos_ - start);
            t.kind = TokKind::Int;
            t.int_val = (uval & SIGN) ? static_cast<int64_t>(uval - BIT48)   // sign-extend from 48 bits
                                      : static_cast<int64_t>(uval);
            return t;
        }
    }

    scan_digit_run(is_digit, line, col);

    bool is_double = false;
    // After a member dot (`t.0`), the digits are a tuple index -- an integer -- so a
    // following `.digit` is the NEXT member access (`t.0.1` == `(t.0).1`), not a decimal
    // point. Only treat `.` as a decimal point when this number did not follow a dot.
    // (`1_.5` therefore never reaches here: the separator run above rejects the trailing `_`.
    // `1._5` stays a member access on `1`, since `.` needs a DIGIT after it -- as in Java.)
    if (!prev_was_dot_ && cur() == '.' && is_digit(peek(1))) {  // `.` is a decimal point only before a digit
        is_double = true;
        advance();                                 // consume '.'
        scan_digit_run(is_digit, line, col);
    }
    // Optional exponent: `e`/`E`, an optional sign, then >= 1 digit. An exponent makes the number a
    // Double even without a fractional part (`1e10`). Only consumed when a digit actually follows, so
    // `1e` / `t.0e` leave `e` as a separate identifier token. Guarded by `!prev_was_dot_` for the same
    // reason as the decimal point (a tuple index like `t.0` is always a plain integer). std::from_chars
    // parses the scientific form directly, so no extra value handling is needed.
    if (!prev_was_dot_ && (cur() == 'e' || cur() == 'E')) {
        const int digit_off = (peek(1) == '+' || peek(1) == '-') ? 2 : 1;
        if (is_digit(peek(digit_off))) {
            is_double = true;
            advance();                                        // consume 'e'/'E'
            if (cur() == '+' || cur() == '-') advance();      // optional sign
            scan_digit_run(is_digit, line, col);
        }
    }

    Token t;
    t.line = line;
    t.col  = col;
    t.text = src_.substr(start, pos_ - start);

    // The token text keeps the separators (diagnostics quote the source); the VALUE is parsed from a
    // stripped copy, because from_chars would stop dead at the first `_`.
    const std::string value_text = without_separators(t.text);
    const char* first = value_text.data();
    const char* last  = first + value_text.size();
    if (is_double) {
        t.kind = TokKind::Double;
        auto r = std::from_chars(first, last, t.double_val);
        if (r.ec != std::errc{}) throw LexError("malformed floating-point literal", line, col);
    } else {
        t.kind = TokKind::Int;
        auto r = std::from_chars(first, last, t.int_val);
        if (r.ec != std::errc{}) throw LexError("integer literal out of range", line, col);
    }
    return t;
}

Token Lexer::scan_ident(uint32_t line, uint32_t col) {
    size_t start = pos_;
    while (is_ident_cont(cur())) advance();

    Token t;
    t.text = src_.substr(start, pos_ - start);
    t.kind = keyword_kind(t.text);
    // keyword_kind returns the LIdent sentinel for a non-keyword. Case-split it:
    // an uppercase-initial identifier is a UIdent (type / constructor / trait). A
    // lone `_` already came back as Underscore, and every keyword is lowercase, so
    // only genuine identifiers reach this promotion.
    if (t.kind == TokKind::LIdent && is_upper(t.text[0]))
        t.kind = TokKind::UIdent;
    t.line = line;
    t.col  = col;
    return t;
}

Token Lexer::scan_string(uint32_t line, uint32_t col) {
    advance();                                     // consume opening '"'
    return scan_interp_text(line, col, /*continuation=*/false);
}

// Raw string literal: `r"…"` or `r#"…"#` / `r##"…"##` / … . The number of `#`s after `r` fixes the
// delimiter: the string ends at the first `"` followed by exactly that many `#`s, so a `"` with fewer
// following `#`s (or none, in the bare `r"…"` form only if no `"` at all) is part of the content --
// this is what lets a raw string carry embedded `"` (regex, JSON/HTML templates). No backslash escapes
// and no `${…}` interpolation are processed: every byte between the delimiters is taken verbatim,
// including newlines (multi-line is allowed; `advance()` keeps line/col correct across them). The
// result is an ordinary TokKind::Str, so the parser/checker/codegen see a plain string literal.
Token Lexer::scan_raw_string(uint32_t line, uint32_t col) {
    advance();                                     // consume the 'r' prefix
    size_t hashes = 0;
    while (cur() == '#') { advance(); ++hashes; }  // count the opening '#'s
    advance();                                     // consume the opening '"' (guaranteed by the guard)

    std::string value;
    for (;;) {
        if (at_end())
            throw LexError("unterminated raw string literal", line, col);
        if (cur() == '"') {                        // candidate close: '"' followed by `hashes` '#'s
            bool close = true;
            for (size_t k = 0; k < hashes; ++k)
                if (peek(1 + k) != '#') { close = false; break; }
            if (close) {
                advance();                         // consume the closing '"'
                for (size_t k = 0; k < hashes; ++k) advance();   // consume the closing '#'s
                Token t;
                t.kind = TokKind::Str;             // a plain string literal -- never interpolated
                t.text = std::move(value);
                t.line = line;
                t.col  = col;
                return t;
            }
        }
        value.push_back(cur());                    // verbatim byte (incl. '\n' and a non-closing '"')
        advance();
    }
}

// Scan one string TEXT chunk from pos_ up to a `${` hole or the closing `"`. `$` is special ONLY
// immediately before `{` (a lone `$` is literal, no escape needed); a literal `${` is written `\$`
// (the new escape below). On a hole it emits InterpStrBegin (leading chunk) / InterpStrMid
// (continuation) and keeps/enters an interp frame; on the closing `"` it emits a plain Str (leading,
// no hole seen) / InterpStrEnd (continuation) and pops the frame. See scan_token for the hole-body
// handoff and the `}`-close interception.
Token Lexer::scan_interp_text(uint32_t line, uint32_t col, bool continuation) {
    std::string value;
    for (;;) {
        if (at_end() || cur() == '\n')
            throw LexError("unterminated string literal", line, col);
        char c = cur();
        if (c == '"') {                            // closing quote -- end of the whole string
            advance();
            Token t;
            t.kind = continuation ? TokKind::InterpStrEnd : TokKind::Str;
            t.text = std::move(value);
            t.line = line;
            t.col  = col;
            if (continuation) {                    // this token closes the preceding hole: carry its spec
                t.has_spec = interp_stack_.back().pending_has_spec;
                t.spec     = std::move(interp_stack_.back().pending_spec);
                interp_stack_.pop_back();          // leave this interpolation
            }
            return t;
        }
        if (c == '$' && peek(1) == '{') {          // a `${ … }` hole opens here
            advance(); advance();                  // consume '${'
            Token t;
            t.kind = continuation ? TokKind::InterpStrMid : TokKind::InterpStrBegin;
            t.text = std::move(value);
            t.line = line;
            t.col  = col;
            if (continuation) {                    // closes the preceding hole (carry its spec), opens the next
                t.has_spec = interp_stack_.back().pending_has_spec;
                t.spec     = std::move(interp_stack_.back().pending_spec);
                interp_stack_.back().brace_depth = 0;   // reset all delimiter depths for the next hole
                interp_stack_.back().paren_depth = 0;
                interp_stack_.back().bracket_depth = 0;
                interp_stack_.back().pending_has_spec = false;
                interp_stack_.back().pending_spec.clear();
            } else {
                interp_stack_.push_back(InterpFrame{});  // enter the interpolation
            }
            return t;
        }
        if (c == '\\') {
            advance();
            value.push_back(decode_escape());
            advance();
        } else {
            value.push_back(c);
            advance();
        }
    }
}

// Decode one backslash escape. `pos_` is on the char AFTER the backslash on entry
// (the caller advances past it). Shared by scan_string and scan_char; both quote
// escapes (`\'` and `\"`) are accepted in both contexts (forgiving). A `\uXXXX` /
// `\xNN` is deliberately NOT supported -- the language is byte-oriented, so there is
// no code-point escape (a code point > 255 could not be a single byte anyway).
char Lexer::decode_escape() {
    switch (cur()) {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '\\': return '\\';
        case '"':  return '"';
        case '\'': return '\'';
        case '$':  return '$';   // `\$` -- a literal '$' (lets a string carry a literal `${`)
        case '0':  return '\0';
        default:   throw LexError("unknown escape sequence", line_, col_);
    }
}

// A character literal is pure lexer sugar for an Int: `'A'` lexes to an Int token
// whose value is the single BYTE 0..255. There is NO scalar Char type -- Int already
// covers a byte (bytes read as Int), matching the byte-string model. So everything
// downstream (parser/checker/codegen/VM) sees an ordinary Int literal, and char
// literals work in expressions, patterns, and match arms for free. A multi-byte
// literal (e.g. 'é' = 2 UTF-8 bytes) is rejected: the value must equal a byte read
// from toBytes(s) / b[i] for the byte-scanner use case to be sound.
Token Lexer::scan_char(uint32_t line, uint32_t col) {
    advance();                                     // consume opening '\''
    std::string bytes;
    for (;;) {
        if (at_end() || cur() == '\n')
            throw LexError("unterminated character literal", line, col);
        char c = cur();
        if (c == '\'') { advance(); break; }
        if (c == '\\') {
            advance();
            bytes.push_back(decode_escape());
            advance();
        } else {
            bytes.push_back(c);
            advance();
        }
    }
    if (bytes.empty())
        throw LexError("empty character literal", line, col);
    if (bytes.size() != 1)
        throw LexError("character literal must be a single byte", line, col);

    Token t;
    t.kind    = TokKind::Int;                          // a char literal IS an Int
    t.int_val = static_cast<unsigned char>(bytes[0]);  // 0..255
    t.text    = bytes;                                 // cosmetic (diagnostics only)
    t.line = line;
    t.col  = col;
    return t;
}

Token Lexer::scan_colon(uint32_t line, uint32_t col) {
    if (peek(1) == ':')                              // '::' path separator (Trait::method)
        return emit(TokKind::ColonColon, 2, line, col);

    // A single `:` at the TOP level of a `${ … }` hole (all of braces/parens/brackets balanced) is the
    // `${e:spec}` format-specifier separator (reached only here: `::` is already handled above; a struct/map/
    // block colon is at brace_depth > 0; a LAMBDA param annotation `fn(x: Int)`
    // is at paren_depth > 0). Scan the raw spec up to the hole-closing `}`, stage it on the frame, and let
    // scan_interp_text emit the InterpStrMid/End that carries it (see Parser::parse_interp_string).
    if (!interp_stack_.empty() && interp_stack_.back().brace_depth == 0
        && interp_stack_.back().paren_depth == 0 && interp_stack_.back().bracket_depth == 0) {
        advance();                                 // consume ':'
        std::string spec;
        while (cur() != '}') {
            if (at_end() || cur() == '\n')
                throw LexError("unterminated format specifier", line, col);
            spec.push_back(cur());
            advance();
        }
        interp_stack_.back().pending_has_spec = true;
        interp_stack_.back().pending_spec = std::move(spec);
        uint32_t l = line_, c = col_;              // position of the hole-closing '}'
        advance();                                 // consume it
        return scan_interp_text(l, c, /*continuation=*/true);
    }
    return emit(TokKind::Colon, 1, line, col);
}

// Interpolation-aware dispatch: inside a `${ … }` hole (interp_stack_ non-empty), a depth-0 `}`
// closes the hole -- consume it and resume text scanning (InterpStrMid/InterpStrEnd) instead of
// emitting an RBrace token; otherwise track `{`/`}` depth of the enclosing hole so struct-literal /
// block braces inside it are not mistaken for the closer. Outside any hole this is a thin pass-through.
Token Lexer::scan_token() {
    if (!interp_stack_.empty() && cur() == '}' && interp_stack_.back().brace_depth == 0) {
        uint32_t line = line_, col = col_;
        advance();                                 // consume the hole-closing '}'
        return scan_interp_text(line, col, /*continuation=*/true);
    }
    Token t = scan_token_raw();
    if (!interp_stack_.empty()) {                  // a `"…"` in the raw scan may have pushed a nested
        InterpFrame& f = interp_stack_.back();     // frame; adjust the top (current) hole's delimiter depths
        if      (t.kind == TokKind::LBrace)   f.brace_depth++;
        else if (t.kind == TokKind::RBrace)   f.brace_depth--;
        else if (t.kind == TokKind::LParen)   f.paren_depth++;
        else if (t.kind == TokKind::RParen)   f.paren_depth--;
        else if (t.kind == TokKind::LBracket) f.bracket_depth++;
        else if (t.kind == TokKind::RBracket) f.bracket_depth--;
    }
    return t;
}

Token Lexer::scan_token_raw() {
    uint32_t line = line_, col = col_;
    char c = cur();

    if (is_digit(c))       return scan_number(line, col);
    // Raw-string prefix: a lowercase `r` immediately followed by `"` or by one-or-more `#` then `"`
    // (`r"…"` / `r#"…"#` / `r##"…"##` …). The lookahead (r, 0..N `#`, a mandatory `"`) makes this
    // unambiguous -- an identifier `r` directly abutting a string literal is never valid Skarn, and a
    // `red`/`range`/`r#{…}` fails the `"` test and falls through to scan_ident. Uppercase `R` is NOT a
    // raw prefix (lowercase-only, as in Rust).
    if (c == 'r') {
        size_t k = 1;
        while (peek(k) == '#') ++k;
        if (peek(k) == '"') return scan_raw_string(line, col);
    }
    if (is_ident_start(c)) return scan_ident(line, col);
    if (c == '"')          return scan_string(line, col);
    if (c == '\'')         return scan_char(line, col);
    if (c == ':')          return scan_colon(line, col);

    switch (c) {
        case '(': return emit(TokKind::LParen,   1, line, col);
        case ')': return emit(TokKind::RParen,   1, line, col);
        case '{': return emit(TokKind::LBrace,   1, line, col);
        case '}': return emit(TokKind::RBrace,   1, line, col);
        case '[': return emit(TokKind::LBracket, 1, line, col);
        case ']': return emit(TokKind::RBracket, 1, line, col);
        case ',': return emit(TokKind::Comma,    1, line, col);
        case ';': return emit(TokKind::StmtEnd,  1, line, col);
        case '?': return emit(TokKind::Question, 1, line, col);
        case '#': return emit(TokKind::Hash,     1, line, col);
        // `@` shares no prefix with any other lexeme, so unlike every multi-character family below
        // its position in this switch carries no longest-match obligation.
        case '@': return emit(TokKind::At,       1, line, col);
        // Arithmetic, each with its compound-assignment form `op=` (comments are consumed before
        // this switch, so `/` is safely a plain operator here).
        case '+': return peek(1) == '=' ? emit(TokKind::PlusEq,    2, line, col)
                                        : emit(TokKind::Plus,      1, line, col);
        case '*': return peek(1) == '=' ? emit(TokKind::StarEq,    2, line, col)
                                        : emit(TokKind::Star,      1, line, col);
        case '/': return peek(1) == '=' ? emit(TokKind::SlashEq,   2, line, col)
                                        : emit(TokKind::Slash,     1, line, col);
        case '%': return peek(1) == '=' ? emit(TokKind::PercentEq, 2, line, col)
                                        : emit(TokKind::Percent,   1, line, col);
        case '^': return peek(1) == '=' ? emit(TokKind::BitXorEq, 2, line, col)
                                        : emit(TokKind::BitXor,   1, line, col);
        case '~': return emit(TokKind::Tilde,    1, line, col);
        case '.':                                     // `..<` (exclusive range) before `..` before `.`
            if (peek(1) == '.') return peek(2) == '<' ? emit(TokKind::DotDotLess, 3, line, col)
                                                      : emit(TokKind::DotDot,     2, line, col);
            return emit(TokKind::Dot, 1, line, col);
        case '-':
            if (peek(1) == '>') return emit(TokKind::Arrow,   2, line, col);
            if (peek(1) == '=') return emit(TokKind::MinusEq, 2, line, col);
            return emit(TokKind::Minus, 1, line, col);
        case '|':
            if (peek(1) == '>') return emit(TokKind::Pipe,     2, line, col);
            if (peek(1) == '|') return emit(TokKind::OrOr,     2, line, col);
            if (peek(1) == '=') return emit(TokKind::BitOrEq,  2, line, col);
            return emit(TokKind::BitOr, 1, line, col);
        case '&':
            if (peek(1) == '&') return emit(TokKind::AndAnd,   2, line, col);
            if (peek(1) == '=') return emit(TokKind::BitAndEq, 2, line, col);
            return emit(TokKind::BitAnd, 1, line, col);
        case '=':
            if (peek(1) == '=') return emit(TokKind::EqEq,     2, line, col);
            if (peek(1) == '>') return emit(TokKind::FatArrow, 2, line, col);
            return emit(TokKind::Assign, 1, line, col);
        case '!':
            if (peek(1) == '=') return emit(TokKind::NotEq, 2, line, col);
            return emit(TokKind::Not, 1, line, col);
        // The shift family is matched LONGEST-FIRST: `>>>=` (the language's only four-character
        // token) before `>>>`, and each `op=` before its bare `op`, or the shorter form would
        // swallow the prefix. There is no closing-angle-bracket ambiguity to worry about here --
        // Skarn spells type arguments with brackets (`Vec[T]`), so `>` is always an operator.
        case '<':
            if (peek(1) == '<' && peek(2) == '=') return emit(TokKind::ShlEq, 3, line, col);
            if (peek(1) == '<')                   return emit(TokKind::Shl,   2, line, col);
            if (peek(1) == '=')                   return emit(TokKind::Le,    2, line, col);
            return emit(TokKind::Lt, 1, line, col);
        case '>':
            if (peek(1) == '>' && peek(2) == '>' && peek(3) == '=') return emit(TokKind::UShrEq, 4, line, col);
            if (peek(1) == '>' && peek(2) == '>')                   return emit(TokKind::UShr,   3, line, col);
            if (peek(1) == '>' && peek(2) == '=')                   return emit(TokKind::ShrEq,  3, line, col);
            if (peek(1) == '>')                                     return emit(TokKind::Shr,    2, line, col);
            if (peek(1) == '=')                                     return emit(TokKind::Ge,     2, line, col);
            return emit(TokKind::Gt, 1, line, col);
        default:
            break;
    }
    throw LexError(std::string("unexpected character '") + c + "'", line, col);
}

std::vector<Token> Lexer::apply_asi(const std::vector<Token>& raw) {
    std::vector<Token> out;
    out.reserve(raw.size());

    TokKind prev_kind = TokKind::Eof;
    bool    has_prev  = false;

    auto push_stmt_end = [&](uint32_t line, uint32_t col) {
        if (!out.empty() && out.back().kind == TokKind::StmtEnd) return;
        Token t;
        t.kind = TokKind::StmtEnd;
        t.text = ";";
        t.line = line;
        t.col  = col;
        out.push_back(std::move(t));
    };

    size_t i = 0;
    while (i < raw.size()) {
        const Token& t = raw[i];

        if (t.kind == TokKind::Newline) {
            size_t j = i + 1;
            while (j < raw.size() && raw[j].kind == TokKind::Newline) ++j;
            const TokKind next = j < raw.size() ? raw[j].kind : TokKind::Eof;
            if (has_prev && ends_value(prev_kind) && !starts_continuation(next))
                push_stmt_end(t.line, t.col);
            i = j;
            continue;
        }

        if (t.kind == TokKind::StmtEnd) {          // explicit ';'
            push_stmt_end(t.line, t.col);
            has_prev = false;                      // nothing left to terminate
            ++i;
            continue;
        }

        out.push_back(t);
        prev_kind = t.kind;
        has_prev  = true;
        ++i;
    }
    return out;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> raw;
    for (;;) {
        bool nl = skip_trivia();
        if (at_end()) {
            if (nl) {
                Token n; n.kind = TokKind::Newline; n.line = line_; n.col = col_;
                raw.push_back(std::move(n));
            }
            break;
        }
        if (nl) {
            Token n; n.kind = TokKind::Newline; n.line = line_; n.col = col_;
            raw.push_back(std::move(n));
        }
        Token tk = scan_token();
        // Track member dots so scan_number can distinguish a tuple index from a Double.
        // A newline push (above) leaves the flag untouched, so a dot may legitimately
        // lead the next line (`t\n.0`).
        prev_was_dot_ = (tk.kind == TokKind::Dot);
        raw.push_back(std::move(tk));
    }
    Token eof; eof.kind = TokKind::Eof; eof.line = line_; eof.col = col_;
    raw.push_back(std::move(eof));
    return apply_asi(raw);
}

// ---- diagnostics ------------------------------------------------------------

const char* to_string(TokKind k) {
    switch (k) {
        case TokKind::Int:        return "Int";
        case TokKind::Double:     return "Double";
        case TokKind::Str:        return "Str";
        case TokKind::InterpStrBegin: return "InterpStrBegin";
        case TokKind::InterpStrMid:   return "InterpStrMid";
        case TokKind::InterpStrEnd:   return "InterpStrEnd";
        case TokKind::UIdent:     return "UIdent";
        case TokKind::LIdent:     return "LIdent";
        case TokKind::KwLet:      return "let";
        case TokKind::KwMut:      return "mut";
        case TokKind::KwFn:       return "fn";
        case TokKind::KwStruct:   return "struct";
        case TokKind::KwEnum:     return "enum";
        case TokKind::KwMatch:    return "match";
        case TokKind::KwIf:       return "if";
        case TokKind::KwElse:     return "else";
        case TokKind::KwWhile:    return "while";
        case TokKind::KwFor:      return "for";
        case TokKind::KwIn:       return "in";
        case TokKind::KwLoop:     return "loop";
        case TokKind::KwBreak:    return "break";
        case TokKind::KwContinue: return "continue";
        case TokKind::KwTrait:    return "trait";
        case TokKind::KwDyn:      return "dyn";
        case TokKind::KwImpl:     return "impl";
        case TokKind::KwImport:   return "import";
        case TokKind::KwUse:      return "use";
        case TokKind::KwPub:      return "pub";
        case TokKind::KwConst:    return "const";
        case TokKind::KwTransparent: return "transparent";
        case TokKind::KwReturn:   return "return";
        case TokKind::KwTrue:     return "true";
        case TokKind::KwFalse:    return "false";
        case TokKind::KwSelf:     return "self";
        case TokKind::LParen:     return "(";
        case TokKind::RParen:     return ")";
        case TokKind::LBrace:     return "{";
        case TokKind::RBrace:     return "}";
        case TokKind::LBracket:   return "[";
        case TokKind::RBracket:   return "]";
        case TokKind::Comma:      return ",";
        case TokKind::Colon:      return ":";
        case TokKind::ColonColon: return "::";
        case TokKind::Dot:        return ".";
        case TokKind::DotDot:     return "..";
        case TokKind::DotDotLess: return "..<";
        case TokKind::Arrow:      return "->";
        case TokKind::FatArrow:   return "=>";
        case TokKind::Underscore: return "_";
        case TokKind::Hash:       return "#";
        case TokKind::At:         return "@";
        case TokKind::Question:   return "?";
        case TokKind::Assign:     return "=";
        case TokKind::Pipe:       return "|>";
        case TokKind::OrOr:       return "||";
        case TokKind::AndAnd:     return "&&";
        case TokKind::EqEq:       return "==";
        case TokKind::NotEq:      return "!=";
        case TokKind::Lt:         return "<";
        case TokKind::Le:         return "<=";
        case TokKind::Gt:         return ">";
        case TokKind::Ge:         return ">=";
        case TokKind::BitOr:      return "|";
        case TokKind::BitXor:     return "^";
        case TokKind::BitAnd:     return "&";
        case TokKind::Shl:        return "<<";
        case TokKind::Shr:        return ">>";
        case TokKind::UShr:       return ">>>";
        case TokKind::Plus:       return "+";
        case TokKind::Minus:      return "-";
        case TokKind::Star:       return "*";
        case TokKind::Slash:      return "/";
        case TokKind::Percent:    return "%";
        case TokKind::PlusEq:     return "+=";
        case TokKind::MinusEq:    return "-=";
        case TokKind::StarEq:     return "*=";
        case TokKind::SlashEq:    return "/=";
        case TokKind::PercentEq:  return "%=";
        case TokKind::BitAndEq:   return "&=";
        case TokKind::BitOrEq:    return "|=";
        case TokKind::BitXorEq:   return "^=";
        case TokKind::ShlEq:      return "<<=";
        case TokKind::ShrEq:      return ">>=";
        case TokKind::UShrEq:     return ">>>=";
        case TokKind::Not:        return "!";
        case TokKind::Tilde:      return "~";
        case TokKind::Newline:    return "<newline>";
        case TokKind::StmtEnd:    return "<stmt-end>";
        case TokKind::Eof:        return "<eof>";
    }
    return "<?>";
}

} // namespace svc
