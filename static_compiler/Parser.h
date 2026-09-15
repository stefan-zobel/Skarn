#pragma once

// =============================================================================
// Parser.h -- token stream -> AST for the Skarn front end.
//
// A recursive-descent parser whose expression layer is a Pratt (precedence-
// climbing) parser: prefix handlers produce the leaves and prefix forms, a
// binding-power loop folds the infix operator ladder, and postfix handlers pick
// up call `(...)`, index `[...]`, field `.name` / `.N`, and the try operator `?`.
// Statement/item/pattern/type grammar is ordinary recursive descent on top.
//
// Adapted from the former dynamic parser. Static deltas: the
// case-split lexer removes pattern lookahead (LIdent = binding, UIdent = ctor);
// `?` builds a first-class TryExpr (no desugar); `.[k]` map-index and `#[`/`#v[`
// literals are gone; generics-with-bounds are parsed on fn/struct/enum/impl +
// method; param + return types are mandatory; `self` is the KwSelf keyword; an
// impl target is a type. See docs/skarn_grammar.ebnf. Namespace svc.
//
// Input is the Lexer's token vector (already carrying ASI `StmtEnd` markers and
// ending in Eof). The parser treats `StmtEnd`, `}`, and Eof as statement
// terminators. Throws ParseError on malformed input (fail-fast).
// =============================================================================

#include "Ast.h"
#include "Token.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace svc {

// Thrown on a syntax error. Carries the 1-based source position of the offending
// token so callers can report `line:col`.
class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& msg, uint32_t line, uint32_t col);
    uint32_t line() const noexcept { return line_; }
    uint32_t col()  const noexcept { return col_; }
private:
    uint32_t line_;
    uint32_t col_;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    // Parse a whole compilation unit (zero or more top-level items) up to Eof.
    Program parse_program();

    // Parse exactly one expression, then require Eof. Convenience for tests.
    ExprPtr parse_expression();

private:
    // ----- cursor -----
    const Token& peek(size_t off = 0) const noexcept;
    const Token& cur() const noexcept { return peek(0); }
    bool at_end() const noexcept;
    const Token& advance() noexcept;
    bool check(TokKind k) const noexcept { return cur().kind == k; }
    bool accept(TokKind k) noexcept;                       // consume iff it matches
    const Token& expect(TokKind k, const char* what);      // consume or throw
    [[noreturn]] void error(const std::string& msg) const; // throw at cur()

    void skip_terminators();  // consume any run of StmtEnd tokens

    // ----- items -----
    ItemPtr parse_item();
    ItemPtr parse_fn_item();
    ItemPtr parse_struct_item();
    ItemPtr parse_enum_item();
    ItemPtr parse_const_item();
    ItemPtr parse_trait_item();
    ItemPtr parse_impl_item();
    ItemPtr parse_import_item();
    ItemPtr parse_use_item();
    // One `fn …` method; require_body=true forces a `{ … }` block (impl), false
    // allows a bodyless signature (a required trait method).
    Method  parse_method(bool require_body);

    // ----- statements -----
    StmtPtr parse_stmt();
    StmtPtr parse_let();

    // ----- expressions (Pratt) -----
    ExprPtr parse_expr(int min_bp);
    ExprPtr parse_prefix();
    ExprPtr parse_interp_string();  // `"…${e}…"` -> desugars to a `toString`/`+` chain (no new AST node)
    void validate_format_spec(const std::string& spec) const;  // `${e:spec}` grammar check (throws on bad)
    ExprPtr parse_if();
    ExprPtr parse_if_let(const Token& kw);   // `if let PAT = E {…} else {…}` -> desugars to a 2-arm match
    ExprPtr parse_else_branch();             // an `else` tail: `else if …` chain or `else { … }` block (tailored diag)
    ExprPtr parse_match();
    ExprPtr parse_while();
    ExprPtr parse_while_let(const Token& kw); // `while let PAT = E { … }` -> desugars to loop + match + break
    ExprPtr parse_for();
    ExprPtr parse_loop();                   // `loop { … }` -- the condition-less infinite loop
    ExprPtr parse_lambda();
    ExprPtr parse_block();
    ExprPtr parse_paren();     // grouping / tuple / unit `()`
    ExprPtr parse_list();      // `[...]` sequence (list) literal
    ExprPtr parse_map_lit();   // `#{k => v, ...}` map literal
    ExprPtr parse_struct_lit(std::string qualifier, std::string name, uint32_t line, uint32_t col);
    std::vector<ExprPtr> parse_arg_list();

    // ----- patterns -----
    // A match-arm / if-let head: one or more `parse_pattern()` alternatives joined by `|`
    // (with an optional leading `|`). A single alternative returns that pattern verbatim; two
    // or more wrap into an `OrPat`. Alternatives may also be written PARENTHESIZED anywhere a
    // pattern is allowed -- `n @ (1..10 | 20..30)`, `Some(A | B)` -- so or-patterns are no longer
    // top-level only. Alternatives MAY bind, provided they all bind the same set (checker rule).
    PatPtr parse_arm_pattern();
    // Collect `first | a | b | ...` into an OrPat, splicing nested OrPats flat. Shared by
    // parse_arm_pattern, the parenthesized form, and parse_nested_pattern.
    PatPtr parse_or_tail(PatPtr first, uint32_t line, uint32_t col);
    // A pattern in a nested position whose terminator is unambiguous (ctor / tuple / list element,
    // struct field value, map value), so `|` needs no parentheses there.
    PatPtr parse_nested_pattern();
    PatPtr parse_pattern();
    // A pattern LITERAL: a number / string / bool literal, optionally negated -- and NOTHING else.
    // Deliberately not `parse_prefix()`, which only LOOKS restricted here: its unary arm parses the
    // operand with `parse_expr(UNARY_BP)`, and `POSTFIX_BP` (100) exceeds `UNARY_BP` (11), so the
    // Pratt loop happily consumes a call / index / field access -- `-f(3)` used to parse as a
    // "literal" pattern and `1..(g(1,2,3,4))` as a range bound. That is not merely surprising: a
    // call in a bound allocates argument temps that `measure_pattern` never budgeted (its Literal
    // and Range cases both return a hardcoded `{1, 0}` and never walk the bounds), so it is a frame
    // under-measure -- either a `CodegenError` or a silently wrong register read. This is a LEAF
    // parser: it consumes at most a sign and one literal token and can never re-enter the loop.
    ExprPtr parse_pattern_literal();
    // A range-pattern BOUND: a pattern literal, or a (possibly qualified) NAME the checker must
    // resolve to a scalar `const`. A name is unambiguous HERE and nowhere else in a pattern --
    // a bound is never a binder -- which is why `LO..HI` is expressible while a bare `MAX` as a
    // whole pattern is not. A qualified bound's tail must be UPPERCASE (`util::LO`), matching what
    // the pattern grammar already allows after `mod::`, so both bounds accept the same spellings.
    ExprPtr parse_range_bound();
    // `lo` is parsed and the cursor sits on `..` / `..<`: build the RangePat. One place, because
    // three copies of "which separator did I see" is exactly how the exclusivity flag would drift
    // out of the Maranget con id, where it is load-bearing.
    PatPtr finish_range_pattern(ExprPtr lo, uint32_t line, uint32_t col);
    // A binding name has been consumed: `name @ sub` -> BindPat, a bare `name` -> IdentPat.
    // Shared by the `lident` and `mut lident` branches of parse_pattern.
    PatPtr finish_binding_pattern(std::string name, bool is_mut, uint32_t line, uint32_t col);
    // Ctor / struct pattern body past the (optional-qualified) head name: `Name { ... }`,
    // `Name(p, ...)`, or a bare nullary `Name`. `qualifier` is a module path (empty = bare).
    PatPtr parse_ctor_or_struct_pattern(std::string name, std::string qualifier,
                                        uint32_t line, uint32_t col);

    // ----- types -----
    TypePtr parse_type();
    TypePtr parse_dyn_type();                          // `dyn Trait` / `dyn Trait[A]` / `dyn mod::Trait`
    void parse_type_args(std::vector<TypePtr>& out);   // the optional `[T, U]` tail (Named or dyn)

    // ----- helpers -----
    Param parse_param();                          // `lident : type` (type mandatory)
    std::vector<Param> parse_param_list();        // `(p0, p1, …)` incl. the parentheses
    std::vector<GenericParam> parse_generic_params();  // `[T: A + B, U]` (empty if absent)
    std::vector<Field> parse_positional_fields();      // `( T0, T1 )` -> `_0:T0 _1:T1` (past `(`)

    std::vector<Token> toks_;
    size_t pos_ = 0;
    // When true, a bare `Name { ... }` is NOT parsed as a struct literal -- set
    // while parsing the header expression of if/match/while/for, exactly like Rust,
    // to keep the `{` for the following block.
    bool no_struct_lit_ = false;
};

} // namespace svc
