#pragma once

// =============================================================================
// Token.h -- token kinds and the Token value type for the Skarn front end.
//
// The lexer (Lexer.h/.cpp) turns source text into a stream of these. Adapted from
// the former dynamic front end, with the changes the static
// grammar (docs/skarn_grammar.ebnf) forces:
//
//   * Case-based identifiers: the dynamic single `Ident` is split into two token
//     kinds decided IN THE LEXER -- `UIdent` (first char A..Z: types, constructors,
//     enum variants, traits, type variables) and `LIdent` (first char a..z or `_`,
//     but not a lone `_`: values, functions, methods, params, fields). This removes
//     the constructor-vs-binding ambiguity (`None`) so the parser never consults a
//     namespace.
//   * No `nil`: the language has no null (unit is `()`, absence is `Option[T]`), so
//     there is no `KwNil`.
//   * `self` is a reserved keyword (`KwSelf`), not a plain lident.
//   * The 64-bit float type is `Double` (there is no `Float`): the numeric literal
//     token is `Double` with a `double_val` payload.
//
// Namespace `svc` (Skarn compiler), kept distinct from the dynamic `vc`.
// The lexer is self-contained (no vmcore dependency).
// =============================================================================

#include <string>
#include <cstdint>

namespace svc {

enum class TokKind : uint8_t {
    // literals
    Int, Double, Str, UIdent, LIdent,

    // string interpolation: a `"…${e}…"` literal lexes to a sequence
    //   InterpStrBegin(text0) <e0-tokens> ( InterpStrMid(text_i) <e_i-tokens> )* InterpStrEnd(text_n)
    // where each `text` is a (possibly empty) literal chunk carried in `Token.text`. A string with
    // no `${` hole stays a single plain `Str`. Nesting (`"a ${ "b ${x}" } c"`) falls out of the lexer's
    // interpolation-context stack. The parser desugars the sequence to a `toString`/`+` chain.
    InterpStrBegin, InterpStrMid, InterpStrEnd,

    // keywords
    KwLet, KwMut, KwFn, KwStruct, KwMatch, KwIf, KwElse, KwWhile, KwReturn,
    KwTrue, KwFalse,
    KwFor, KwIn, KwLoop, KwBreak, KwContinue,
    KwTrait, KwImpl, KwEnum, KwSelf, KwDyn,
    KwImport, KwUse, KwPub, KwConst, KwTransparent,

    // grouping / punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Colon, ColonColon, Dot, DotDot, DotDotLess, Arrow, FatArrow, Underscore,
    Hash,      // '#' -- collection-literal sigil; '#{' opens a map literal
    At,        // '@' -- pattern binding (`n @ 1..100`): bind the whole value AND match a sub-pattern
    Question,  // '?' -- postfix try operator (`e?`): unwrap-or-early-return on Result/Option

    // operators
    Assign, Pipe,
    OrOr, AndAnd,
    EqEq, NotEq, Lt, Le, Gt, Ge,
    BitOr, BitXor, BitAnd,
    Shl, Shr, UShr,
    Plus, Minus, Star, Slash, Percent,
    // compound assignment `a op= b`; see AssignStmt::op. The arithmetic five are numeric
    // (Int / Double); the bitwise/shift six are integer-only, exactly like their plain
    // operators (the VM's bitwise handlers do no coercion).
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    BitAndEq, BitOrEq, BitXorEq, ShlEq, ShrEq, UShrEq,
    Not, Tilde,

    // structural
    Newline,   // lexer-internal only: a raw line break, never in the final stream
    StmtEnd,   // statement terminator (from ASI over Newline, or an explicit ';')
    Eof,
};

struct Token {
    TokKind     kind = TokKind::Eof;
    std::string text;             // lexeme (ident/keyword/operator) or decoded string
    uint32_t    line = 0;         // 1-based
    uint32_t    col  = 0;         // 1-based, column of the first character
    // The token's source bytes [begin, end): its spelling, which `text` is not for a decoded string
    // (editor tooling -- the formatter copies literals verbatim). A StmtEnd from a line break and Eof
    // are empty.
    uint32_t    begin = 0, end = 0;
    int64_t     int_val    = 0;   // valid iff kind == Int
    double      double_val = 0.0; // valid iff kind == Double
    // Format specifier of the hole that a `InterpStrMid`/`InterpStrEnd` token CLOSES: for `${e:spec}`,
    // `has_spec` is set and `spec` carries the raw text after the `:` (empty spec => a parse error). A
    // bare `${e}` leaves `has_spec` false. See Lexer::scan_token / Parser::parse_interp_string.
    bool        has_spec = false;
    std::string spec;
};

// Human-readable name of a token kind (diagnostics / tests).
const char* to_string(TokKind k);

} // namespace svc
