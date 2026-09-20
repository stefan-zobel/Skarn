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
// terminators. Throws ParseError on malformed input (fail-fast); the editor-tooling
// entry parse_program_tolerant records and recovers instead.
// =============================================================================

#include "Ast.h"
#include "Lexer.h"
#include "Token.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
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

// Editor tooling (the formatter): how the parser read the token stream, by token index into the
// vector the Parser was given. `starts` holds the first token of every top-level item or statement,
// block statement, match arm and impl/trait member -- the grammar separates statements, which may
// share a line with nothing between them, so only the parser knows where one begins. `braces` says
// what each `{` it recorded opens; a `{` not recorded there opens a comma list (a struct literal or
// pattern, a map literal, a `use` list, a record variant's fields).
struct ParseLayout {
    enum class Brace : uint8_t {
        Block,     // statements: a fn / lambda / if / loop body or a block expression
        Match,     // match arms
        Members,   // impl / trait methods
        Fields,    // a struct declaration's fields, an enum's variants
    };
    std::vector<size_t> starts;
    std::vector<std::pair<size_t, Brace>> braces;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    // Editor tooling: record the layout of what parse_program() reads into `layout` (null = off).
    void set_layout(ParseLayout* layout) { layout_ = layout; }

    // Parse a whole compilation unit (zero or more top-level items) up to Eof.
    Program parse_program();

    // Parse exactly one expression, then require Eof. Convenience for tests.
    ExprPtr parse_expression();

    // Editor tooling: never throws a ParseError. Each syntax error is appended to `errors` and parsing
    // resumes at the next statement, match arm, impl/trait member, field, variant or item; the
    // construct that failed is dropped whole, so the tree holds complete nodes only. A token that can
    // only start an item (`fn name`, `struct`, `impl`, `pub`, ...) closes every open body -- the
    // missing-`}` case -- and the innermost open `{` is reported. An item that lost part of itself, or
    // whose text holds one of the `lex_errors`, gets `Item::has_syntax_error`. On valid input the tree
    // equals parse_program()'s.
    Program parse_program_tolerant(std::vector<ParseError>& errors, const std::vector<LexError>& lex_errors);

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

    // ----- error recovery (parse_program_tolerant only; recover_ false = every catch rethrows) -----
    enum class Sync { Item, Stmt, Arm, Member, Field };
    bool at_item_start() const noexcept;           // a token only an item can start with
    void note(const ParseError& e);                // record once per position, at most MAX_ERRORS
    // Record `e`, then skip to the sync point of `where` (counting `( [ {` depth); `start` is the
    // token index the failed construct began at, to guarantee progress; `open` is the `{` of the
    // body the construct sits in (none at item level).
    void recover(const ParseError& e, Sync where, size_t start, const Token* open = nullptr);
    uint32_t line_indent(const Token& t) const;    // column of the first token on t's line
    // In recover mode: does the body opened at `open` end here without its closer (Eof, or an item
    // start; for an impl/trait body a token indented past `owner_col` is a member, not an item)? If
    // so the innermost such body reports it once.
    bool body_unclosed(const Token& open, uint32_t owner_col = 0, bool members_are_fns = false);
    // Consume the `}` of the body opened at `open`. In recover mode it also remembers the first
    // `}` of the item that does not line up with the indentation of its opener's line: when a
    // body later turns out unclosed, that opener is where the `}` is most likely missing (the
    // braces paired up wrongly after it), not the outermost `{` left over.
    void close_body(const Token& open);

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
    ExprPtr parse_braceless_block();   // recover mode: a block whose `{` is missing (see Parser.cpp)
    ExprPtr parse_paren();     // grouping / tuple / unit `()`
    ExprPtr parse_list();      // `[...]` sequence (list) literal
    ExprPtr parse_map_lit();   // `#{k => v, ...}` map literal
    ExprPtr parse_struct_lit(std::string qualifier, std::string name, uint32_t line, uint32_t col,
                             uint32_t name_line, uint32_t name_col);
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
                                        uint32_t line, uint32_t col, uint32_t qual_line, uint32_t qual_col);

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
    ParseLayout* layout_ = nullptr;   // set_layout: where statements start and what each `{` opens
    void layout_start() { if (layout_) layout_->starts.push_back(pos_); }
    void layout_brace(ParseLayout::Brace b) { if (layout_) layout_->braces.emplace_back(pos_, b); }
    // When true, a bare `Name { ... }` is NOT parsed as a struct literal -- set
    // while parsing the header expression of if/match/while/for, exactly like Rust,
    // to keep the `{` for the following block.
    bool no_struct_lit_ = false;

    // ----- the nesting-depth limit -------------------------------------------------------
    // Parsing is recursive descent, so source nesting costs STACK and deep enough input used to
    // overflow it -- the process died with no diagnostic (5 000 nested parentheses still do, in a
    // Debug build). The limit turns that into an ordinary syntax error, which the tolerant parser
    // then recovers from like any other. It matches the checker's `MAX_EXPR_DEPTH`; parentheses
    // build no AST node, so this counter is the only thing that bounds them.
    static constexpr int MAX_NESTING = 200;
    int depth_ = 0;
    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& parser) : p(parser) {
            if (++p.depth_ > MAX_NESTING) { --p.depth_; p.error("expression nested too deeply (more than " +
                                                                std::to_string(MAX_NESTING) + " levels)"); }
        }
        ~DepthGuard() { --p.depth_; }
    };
    // The Pratt loop builds a LEFT-NESTED chain (`a + b + c`, `x |> f |> g`, `a.b().c()`) without
    // recursing, so `DepthGuard` alone does not see how deep the TREE it returns is -- and depth the
    // parser never counted is depth that has to be walked later: the checker's own limit rejects such a
    // chain, but simply DESTROYING it recurses once per link (`~BinaryExpr` -> `~unique_ptr` -> ...),
    // which overflowed the stack on a 20 000-term chain after everything else was already guarded.
    // Counting each wrap keeps the tree bounded by construction. Releases exactly what it added.
    struct ChainDepth {
        Parser& p;
        int n = 0;
        explicit ChainDepth(Parser& parser) : p(parser) {}
        void add() {
            ++p.depth_; ++n;
            if (p.depth_ > MAX_NESTING)
                p.error("expression nested too deeply (more than " + std::to_string(MAX_NESTING) + " levels)");
        }
        ~ChainDepth() { p.depth_ -= n; }
    };

    bool recover_ = false;
    std::vector<ParseError>* errors_ = nullptr;
    size_t unclosed_at_ = SIZE_MAX;   // token index where an unclosed body was last reported
    size_t syntax_events_ = 0;        // errors noted so far, including those past the cap
    uint32_t item_col_ = 0;           // column of the current item's first token (`pub` included)
    const Token* first_mismatch_ = nullptr;   // see close_body; reset per item
};

} // namespace svc
