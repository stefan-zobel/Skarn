#pragma once

// =============================================================================
// Ast.h -- abstract syntax tree for the Skarn front end.
//
// The parser (later) builds these out of the Lexer's token stream. The tree is a
// tagged class hierarchy: every node carries a `kind` discriminant and its 1-based
// source position, and concrete node types derive from one of five bases -- Expr,
// Stmt, Pattern, Type, Item. Ownership is by std::unique_ptr; a null child means
// "absent" (an `if` with no `else`, a `let` with no type annotation). `dump()`
// renders a stable S-expression string used by the tests.
//
// Adapted from the former dynamic AST. Two things are DIFFERENT
// because this language is statically type-checked (types checked then ERASED --
// the Gleam model):
//
//   1. Types are FIRST-CLASS, RETAINED, and MANDATORY at the boundaries the
//      grammar requires (fn/method params + return, struct/enum fields). They are
//      NOT parsed-and-ignored as in the former dynamic front end.
//   2. Every `Expr` reserves a `ty` slot (a semantic svc::Ty, Types.h) that the
//      parser leaves null and the TYPECHECKER fills with the resolved type. dump()
//      ignores it, so structural dumps stay stable once the checker starts writing.
//
// Grammar: docs/skarn_grammar.ebnf. Static deltas vs. the dynamic AST:
// no `nil`; `Double` not `Float`; `self` is a keyword; generics-with-bounds on
// fn/struct/enum/impl (+ method); postfix `?` is a first-class `TryExpr`; a uident
// constructor pattern (`None`/`Some(x)`) is a first-class `CtorPat`; only one index
// form `a[i]`; `[...]` is the sole sequence literal (array/vec are ordinary calls).
//
// Namespace `svc`.
// =============================================================================

#include "Naming.h"
#include "Token.h"
#include "Types.h"

#include <cstdint>
#include <functional>   // for_each_expr
#include <memory>
#include <string>
#include <string_view>   // BUILTIN_FN_NAMES -- the enumerable builtin-name table
#include <vector>

namespace svc {

// ----- base nodes -----------------------------------------------------------

enum class ExprKind : uint8_t {
    IntLit, DoubleLit, StrLit, BoolLit,
    Ident, Unary, Binary, Pipe, Call, Field, Index, Try,
    If, Match, While, For, Loop, Break, Continue, Return, Block, Lambda, StructLit, Tuple, ListLit, MapLit
};

enum class StmtKind : uint8_t { Let, Assign, Expr };
enum class PatKind  : uint8_t { Wildcard, Literal, Ident, Ctor, Tuple, List, Struct, Map, Or, Range, Bind };
// Appending to any of the three enums above is a BUILD BREAK, by design, in two independent places:
// the `kind_name` switches in static_compiler_tests/main.cpp (4062-as-error, they prove the SET is
// complete) and the ordinal `static_assert`s beside the coverage loops there (they prove which
// enumerator is LAST -- a bound the switches cannot check). Follow both to what else must move.
enum class TypeKind : uint8_t { Named, Fn, Tuple, Dyn };
enum class ItemKind : uint8_t { Fn, Struct, Stmt, Trait, Impl, Enum, Import, Use, Const };

// The Expr base carries the resolved-type slot `ty` (null until the checker runs) and `widen_double`,
// the CHECKER'S RECORD that this expression was accepted as an `Int` where a `Double` was expected --
// i.e. that `subsumes` used the `Int <: Double` edge right here. Codegen OBEYS it (emit an `I2D`)
// instead of re-deriving "does this position want a Double?" per call site, which is what let the
// coercion go missing at nine of twenty-six sites and silently compute integer division for
// `fn dv(a: Double, b: Double)` called as `dv(1, 2)`. Same idiom, same reason, as `IdentExpr::ambient`
// and `IdentExpr::inherent`. `compile_expr` (with `compile_into`) is the ONE site that honors it;
// a site deriving `want_double` itself compiles through `compile_expr_raw` instead, because `I2D`
// asserts its source is an Int and a second application reads double bits through `asSigned48()` --
// a silent `0.0` in Release. Never combine the flag with a locally derived `want_double`.
// A DELEGATING construct (Block/If/Match/Loop) is never marked: it pushes the expected type into its
// leaves, they carry the flag, and `settle_delegated` re-types the construct to what they produce.
struct Expr    { ExprKind kind; uint32_t line = 0, col = 0; TyPtr ty; bool widen_double = false; virtual ~Expr() = default; protected: explicit Expr(ExprKind k) : kind(k) {} };
struct Stmt    { StmtKind kind; uint32_t line = 0, col = 0; virtual ~Stmt() = default; protected: explicit Stmt(StmtKind k) : kind(k) {} };
struct Pattern { PatKind  kind; uint32_t line = 0, col = 0; virtual ~Pattern() = default; protected: explicit Pattern(PatKind k) : kind(k) {} };
struct Type    { TypeKind kind; uint32_t line = 0, col = 0; virtual ~Type() = default; protected: explicit Type(TypeKind k) : kind(k) {} };
// `module_prefix` is the owning module's mangle prefix (Slice 2b): "" for the root program
// and the prelude, "util" / "net::http" for an imported module. The checker mangles this
// module's declared names to `prefix::name` and resolves references against the module's
// import environment. Empty prefix = identity, so single-module programs are unaffected.
// `is_pub` (visibility, stdlib split S4): a `pub`-marked item is exported from its module and
// reachable cross-module (via `use` / glob / the auto-import ring / a `mod::name` qualifier). The
// DEFAULT is PRIVATE (Rust model) -- a bare item is visible only inside its own module. Fields and
// enum VARIANTS inherit their container's visibility (no per-field `pub`). Moot for a single-module
// program (same-module access is always allowed); every prelude item is auto-`pub` (Compiler.cpp).
// `name_line`/`name_col` anchor the declared NAME of a fn / struct / enum / trait / const (`line`/`col`
// are its keyword). Read by editor tooling only; 0 for the item kinds that declare no name.
// `has_syntax_error`: set only by Parser::parse_program_tolerant, when a syntax error was recovered
// inside this item (a statement, arm or member of it was dropped). Never read by the compiler.
struct Item    { ItemKind kind; uint32_t line = 0, col = 0; std::string module_prefix; bool is_pub = false;
                 uint32_t name_line = 0, name_col = 0; bool has_syntax_error = false;
                 virtual ~Item() = default; protected: explicit Item(ItemKind k) : kind(k) {} };

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using PatPtr  = std::unique_ptr<Pattern>;
using TypePtr = std::unique_ptr<Type>;
using ItemPtr = std::unique_ptr<Item>;

// ----- shared aggregates ----------------------------------------------------

// A single trait bound reference: a trait name plus optional type arguments --
// `Display` (non-parametric) or `Iterable[T]` (parametric, args reference the
// enclosing generic scope). The args reuse the ordinary type grammar.
struct BoundRef { std::string trait; std::vector<TypePtr> args; uint32_t line = 0, col = 0; };

// A generic type parameter with optional trait bounds: `T` or `T: Display + Clone`
// or `I: Iterable[T]`. The names are UPPERCASE (uident); bounds are trait refs.
struct GenericParam { std::string name; std::vector<BoundRef> bounds; };

// A function/method/lambda parameter: `[mut] name: Type`. `type` is MANDATORY for
// fn / struct / method params, OPTIONAL (null) for a lambda param (supplied by the
// checker's expected type). The invariant is enforced by the parser/checker.
// `is_mut` (from a leading `mut`) permits in-place mutation of the binding; the
// checker restricts it to aggregate types (structs/tuples/Array/Vec/Bytes/Map) and
// binds a mutable scope var. `line`/`col` anchor the `mut` token for diagnostics (0 without `mut`);
// `name_line`/`name_col` anchor the name itself, always set by the parser, for editor tooling.
struct Param { std::string name; TypePtr type; bool is_mut = false; uint32_t line = 0, col = 0;
               uint32_t name_line = 0, name_col = 0; };

// A struct/enum-variant *definition* field: `name: Type` (type mandatory).
struct Field { std::string name; TypePtr type; };

// A struct *literal* field initializer: `name: expr`, or shorthand `name` (value null).
struct FieldInit { std::string name; ExprPtr value; };

// A struct *pattern* field: `name` (shorthand, pat == null) or `name: subpat`.
struct FieldPat { std::string name; PatPtr pat; };

// One arm of a `match`: `pattern [if guard] => body`.
struct MatchArm { PatPtr pat; ExprPtr guard; ExprPtr body; };

// A method inside a `trait` (body null = required, non-null = default) or an `impl`
// (body always present). `has_self` marks a leading `self` receiver (the `KwSelf`
// token; its type is the receiver `Self`, resolved by the checker). `self_mut`
// marks a `mut self` receiver (a binding-local qualifier, NOT part of the method
// signature); the checker binds `self` mutable and gates it to aggregate receivers.
// `self_line`/`self_col` anchor the `mut`/`self` token for diagnostics. Methods may
// carry their own generics (`fn m[T](...)`). `line`/`col` anchor the method NAME (editor tooling).
struct Method {
    std::string name;
    uint32_t line = 0, col = 0;
    std::vector<GenericParam> generics;
    bool has_self = false;
    bool self_mut = false;
    uint32_t self_line = 0, self_col = 0;
    std::vector<Param> params;   // the non-self params
    TypePtr ret;
    ExprPtr body;                // null = required trait method
};

// ----- expression nodes -----------------------------------------------------

struct IntLit    : Expr { int64_t value = 0;        IntLit()    : Expr(ExprKind::IntLit)    {} };
struct DoubleLit : Expr { double  value = 0.0;      DoubleLit() : Expr(ExprKind::DoubleLit) {} };
struct StrLit    : Expr { std::string value;        StrLit()    : Expr(ExprKind::StrLit)    {} };
struct BoolLit   : Expr { bool    value = false;    BoolLit()   : Expr(ExprKind::BoolLit)   {} };
// A bare name (`upper=false`, an lident: variable / function), a bare constructor or
// type used as a value (`upper=true`, a uident: `None`, `Point`), or a qualified
// trait-method path `Trait::method` (qualifier = "Trait", name = "method", upper=true).
// `upper` records the head token's case so the checker need not re-lex to tell a
// constructor from a variable. A non-empty qualifier is only a call / pipe target.
//
// `ambient` is the checker's WRITTEN-BACK routing decision for a bare name that a user
// function shadows: true means "resolved PAST that user fn to the ambient namespace
// (builtin / native / trait method)". Only the checker knows the referring module, so it
// decides once and codegen + the RefEval oracle obey -- three independent lookups of the
// same question is exactly what let a user `fn toInt` hijack the name inside `std::math`.
// Default false = the historical routing, so any path that bypasses the checker is unchanged.
//
// `name_line`/`name_col` anchor the NAME token -- the tail of a qualified path (the node itself sits at
// the head), the name itself otherwise. Editor tooling only.
//
// `inherent` is the same kind of written-back decision for an UPPERCASE qualifier: true means
// "`Head::method` where Head is a TYPE with an inherent method", false means the trait path.
// The two live in separate tables (`traits_` vs `structs_`/`enums_`) under the SAME mangled key
// -- `trait Foo` and `struct Foo` may coexist -- so re-deriving the answer in codegen could pick
// the other one and silently call the wrong function. `qualifier` then holds the MANGLED head.
//
// `resolved_trait` is the third one, for an unqualified trait-method call `m(x)` / `x |> m`: the
// MANGLED trait the checker dispatched it to (empty = not such a call). Several traits may declare
// `m`; the checker picks by the receiver, so codegen and the oracle must not re-derive it by name.
struct IdentExpr : Expr { std::string name; std::string qualifier; bool upper = false;
                          bool ambient = false; bool inherent = false;
                          std::string resolved_trait;
                          uint32_t name_line = 0, name_col = 0;
                                                    IdentExpr() : Expr(ExprKind::Ident)     {} };

struct UnaryExpr : Expr { TokKind op = TokKind::Eof; ExprPtr operand;        UnaryExpr() : Expr(ExprKind::Unary) {} };
struct BinaryExpr: Expr { TokKind op = TokKind::Eof; ExprPtr lhs, rhs;       BinaryExpr(): Expr(ExprKind::Binary) {} };
struct PipeExpr  : Expr { ExprPtr lhs, rhs;                                  PipeExpr()  : Expr(ExprKind::Pipe) {} };
struct CallExpr  : Expr { ExprPtr callee; std::vector<ExprPtr> args;         CallExpr()  : Expr(ExprKind::Call) {} };
// `obj.name` -- struct field access. `tuple_index` marks the `t.N` tuple-index form
// (`name` is then the synthetic `_N`, `index` the numeric slot); the checker resolves
// it against the receiver's static tuple type instead of a declared field name. The node sits at the
// `.`; `name_line`/`name_col` anchor the member token after it (editor tooling). As the callee of
// `recv.m(..)` resolved to a trait method, `resolved_trait` holds the MANGLED trait the checker
// dispatched it to (see IdentExpr::resolved_trait); empty for a field, an inherent method, or unchecked.
struct FieldExpr : Expr { ExprPtr obj; std::string name; bool tuple_index = false; uint32_t index = 0;
                          std::string resolved_trait;
                          uint32_t name_line = 0, name_col = 0; FieldExpr() : Expr(ExprKind::Field) {} };
// `a[i]` -- the sole index form (bounds-checked, total-ish). Map lookup is the
// ordinary call `get(m, k) -> Option[V]`, so there is no map-index flag.
struct IndexExpr : Expr { ExprPtr obj; ExprPtr index;                        IndexExpr() : Expr(ExprKind::Index) {} };
// `e?` -- postfix try: unwrap Ok/Some, or early-return Err/None from the enclosing
// function. A first-class node (the checker types it + reports propagation; codegen
// desugars it to match + return during lowering).
struct TryExpr   : Expr { ExprPtr operand;                                   TryExpr()   : Expr(ExprKind::Try) {} };

struct IfExpr    : Expr { ExprPtr cond; ExprPtr then_blk; ExprPtr else_blk;  IfExpr()    : Expr(ExprKind::If) {} };
struct MatchExpr : Expr { ExprPtr scrut; std::vector<MatchArm> arms; bool from_if_let = false; MatchExpr() : Expr(ExprKind::Match) {} };
struct WhileExpr : Expr { ExprPtr cond; ExprPtr body;                        WhileExpr() : Expr(ExprKind::While) {} };
// `for pat in iter { body }` -- external iteration (snapshot semantics), yields unit.
// `pat` is an irrefutable binding pattern (ident / `_` / tuple of those).
struct ForExpr    : Expr { PatPtr pat; ExprPtr iter; ExprPtr body;           ForExpr()   : Expr(ExprKind::For) {} };
// `loop { body }` -- the condition-less infinite loop. Unlike `while`/`for` it has NO
// fall-through exit, so its type is purely the join of its `break` values (`Never` when
// no `break` is reachable). This is what makes `break value` sound here and nowhere else.
struct LoopExpr   : Expr { ExprPtr body;                                     LoopExpr()  : Expr(ExprKind::Loop) {} };
// `break [value]` / `continue` -- loop control, valid only inside a `for`/`while`/`loop` body.
// A `value` is `loop`-only (a `while`/`for` always exits with unit); null = valueless `break`.
struct BreakExpr    : Expr { ExprPtr value;                                  BreakExpr()    : Expr(ExprKind::Break) {} };
struct ContinueExpr : Expr {                                                 ContinueExpr() : Expr(ExprKind::Continue) {} };
// `return [value]` -- diverges out of the enclosing function, leaving `value` (or
// unit if absent). An expression (like break/continue) that yields nothing in place.
struct ReturnExpr   : Expr { ExprPtr value;                                  ReturnExpr()   : Expr(ExprKind::Return) {} };
struct BlockExpr : Expr { std::vector<StmtPtr> stmts;                        BlockExpr() : Expr(ExprKind::Block) {} };
struct LambdaExpr: Expr { std::vector<Param> params; TypePtr ret; ExprPtr body; LambdaExpr() : Expr(ExprKind::Lambda) {} };
// `Name { f: v, ... }` or module-qualified `mod::Name { ... }`. A non-empty `qualifier`
// is resolved against the named module by the checker, which writes the mangled name back
// into `name` and clears `qualifier` (so codegen reads only `name`, exactly as bare form).
// `base` (record update `Name { f: v, ..base }`) is null unless a trailing `..expr` is present; the unlisted
// fields are then copied from it (same struct type; evaluated once). Front-end only -- no VM change.
// `name_line`/`name_col`: the name token (the tail of `mod::Name`); editor tooling only.
struct StructLit : Expr { std::string name; std::string qualifier; std::vector<FieldInit> fields; ExprPtr base;
                          uint32_t name_line = 0, name_col = 0; StructLit() : Expr(ExprKind::StructLit) {} };
struct TupleExpr : Expr { std::vector<ExprPtr> elems;                        TupleExpr() : Expr(ExprKind::Tuple) {} };
// `[a, b, c]` -- the sequence (list) literal. No sigil variants: `array([...])` and
// `vec([...])` are ordinary calls on this literal.
struct ListLit   : Expr { std::vector<ExprPtr> elems;                        ListLit()   : Expr(ExprKind::ListLit) {} };
// `#{k => v, ...}` -- an ordered list of key/value expression pairs (empty allowed).
struct MapLit    : Expr { std::vector<std::pair<ExprPtr, ExprPtr>> entries;  MapLit()    : Expr(ExprKind::MapLit) {} };

// ----- statement nodes ------------------------------------------------------

// `let [mut] pattern [: Type] = init`
struct LetStmt    : Stmt { bool is_mut = false; PatPtr pat; TypePtr type; ExprPtr init; LetStmt() : Stmt(StmtKind::Let) {} };
// `target = value` (reassignment of a `mut` binding; target is an lvalue expr), or a COMPOUND
// assignment `target op= value` when `op` is one of Plus/Minus/Star/Slash/Percent.
//
// `op` carries the operator instead of the parser desugaring to `target = target op value`, for one
// reason: the place must be evaluated exactly ONCE. A desugar would need a deep copy of the lvalue
// (there is no AST clone here) and would then evaluate it twice -- `v[next()] += 1` would call
// `next()` two times, silently, which is precisely the shape the feature is for. Codegen instead
// computes the place once and does a read-modify-write.
//
// Default `Assign` = the plain form, so every pre-existing path (checker, codegen, oracle,
// tree-shaker, measure walkers) is unchanged for ordinary assignments.
struct AssignStmt : Stmt { ExprPtr target; ExprPtr value; TokKind op = TokKind::Assign;
                                                                            AssignStmt() : Stmt(StmtKind::Assign) {} };
// a bare expression used as a statement (its value is the block value if last)
struct ExprStmt   : Stmt { ExprPtr expr;                                    ExprStmt()   : Stmt(StmtKind::Expr) {} };

// ----- pattern nodes --------------------------------------------------------

struct WildcardPat : Pattern { WildcardPat() : Pattern(PatKind::Wildcard) {} };
struct LiteralPat  : Pattern { ExprPtr lit;                                 LiteralPat() : Pattern(PatKind::Literal) {} };
// A fresh binding from an lident: `x` / `mut x`.
struct IdentPat    : Pattern { bool is_mut = false; std::string name;       IdentPat()   : Pattern(PatKind::Ident) {} };
// A uident constructor pattern: nullary `None` (has_parens=false, elems empty) or a
// tuple-struct / enum-variant `Some(x)` (has_parens=true). The case-split lexer makes
// this unambiguous vs. a binding IdentPat.
// A qualified pattern sits at its TAIL; `qual_line`/`qual_col`
// anchor the head (`Shape` in `Shape::Square(w)`), 0 for a bare name. Editor tooling only.
struct CtorPat     : Pattern { std::string name; std::string qualifier; std::vector<PatPtr> elems; bool has_parens = false;
                               uint32_t qual_line = 0, qual_col = 0; CtorPat() : Pattern(PatKind::Ctor) {} };
// `(p0, p1)` -- an anonymous tuple pattern (matches the tuple type of the scrutinee).
struct TuplePat    : Pattern { std::vector<PatPtr> elems;                   TuplePat() : Pattern(PatKind::Tuple) {} };
// `[a, b, ..rest]` -- `rest` is null when there is no `..` tail.
struct ListPat     : Pattern { std::vector<PatPtr> elems; PatPtr rest;      ListPat()    : Pattern(PatKind::List) {} };
struct StructPat   : Pattern { std::string name; std::string qualifier; std::vector<FieldPat> fields;
                               uint32_t qual_line = 0, qual_col = 0; StructPat() : Pattern(PatKind::Struct) {} };
// `#{ key => pat, .. }` -- a map pattern (match-only, partial). Keys are literal exprs.
struct MapPat      : Pattern { std::vector<std::pair<ExprPtr, PatPtr>> entries; MapPat() : Pattern(PatKind::Map) {} };
// `A | B | C` -- an or-pattern. Allowed anywhere a pattern is (bare where the terminator is
// unambiguous, parenthesized elsewhere); nested OrPats are SPLICED FLAT by the parser, so an
// alternative is never itself an OrPat. Alternatives MAY bind, but every one of them must bind the
// same names at the same types with the same mutability -- the checker enforces that, and codegen
// gives the shared names canonical registers each alternative copies into.
struct OrPat       : Pattern { std::vector<PatPtr> alts;                    OrPat() : Pattern(PatKind::Or) {} };
// `lo..hi` (INCLUSIVE, `lo <= v <= hi`) or `lo..<hi` (EXCLUSIVE upper bound, `lo <= v < hi`) --
// a numeric range pattern. Binds nothing; never completes a signature (a `_` is still required).
//
// A bound is a numeric LITERAL (optionally negated) or the NAME of a scalar `const`, possibly
// qualified (`LO..HI`, `util::LO..9`) -- and nothing else. An identifier is unambiguous HERE and
// nowhere else in a pattern, because a bound is never a binder; that is why `LO..HI` is expressible
// while a bare `MAX` as a whole pattern is not (a lowercase name there would have to stop meaning
// "fresh binding"). Arbitrary EXPRESSIONS are a deliberate NON-GOAL: they would need a purity rule
// nothing else in the pattern language has, and would leave the Maranget con id unable to name the
// set the range denotes. Both bounds go through Parser::parse_range_bound, a leaf parser -- see
// Parser.h for why `parse_prefix` was not restrictive enough despite appearances.
//
// Against a `Char` scrutinee a pair of byte-range Int LITERALS (`'a'..'z'`, equivalently `97..122`)
// retypes to Char, mirroring the literal-pattern rule; a NAMED bound does not (Check.cpp says why).
//
// `..` is the inclusive form -- NOT Rust's exclusive `..`. The exclusive spelling is Swift's `..<`,
// deliberately not Rust's `..=`-vs-`..`: adopting those would have silently changed the meaning of
// every existing `lo..hi` in the tree, with no diagnostic. For Int bounds the two forms are
// interchangeable (`0..<10` == `0..9`); for DOUBLE bounds only `..<` can express a half-open range,
// since there is no writable largest double below a given one.
struct RangePat    : Pattern { ExprPtr lo, hi; bool exclusive = false;      RangePat() : Pattern(PatKind::Range) {} };
// `name @ sub` / `mut name @ sub` -- bind the WHOLE value at this position to `name` AND require
// `sub` to match. The one pattern node that both binds and tests, which is why it is the only
// WRAPPER: `name` takes the type of the position, not of `sub`, and `sub` is checked against that
// same type. Refutability is entirely `sub`'s, so a binder position (`let` / `for`) rejects it
// outright rather than asking whether `sub` happens to be irrefutable -- an `@` there would be
// pure noise (`let n @ p = e` binds exactly what `let n = e` does).
struct BindPat     : Pattern { bool is_mut = false; std::string name; PatPtr sub; BindPat() : Pattern(PatKind::Bind) {} };

// ----- type nodes (syntactic -- retained and CHECKED) -----------------------

// `Name`, `Name[T,...]`, or module-qualified `mod::Name[...]`. A non-empty `qualifier`
// (only set for `mod::Name`, never for a primitive/generic/builtin) is resolved against
// the named module by the checker's resolve_type.
// The node sits at the head; `name_line`/`name_col`
// anchor the name token (editor tooling).
struct NamedType : Type { std::string name; std::string qualifier; std::vector<TypePtr> args;
                          uint32_t name_line = 0, name_col = 0; NamedType() : Type(TypeKind::Named) {} };
struct FnType    : Type { std::vector<TypePtr> params; TypePtr ret;         FnType()    : Type(TypeKind::Fn) {} };
struct TupleType : Type { std::vector<TypePtr> elems;                       TupleType() : Type(TypeKind::Tuple) {} };
// `dyn Trait` / `dyn Trait[A, ...]` / `dyn mod::Trait` -- a TRAIT OBJECT (existential):
// "some type implementing Trait, not named here". Unlike a generic param it is a real type,
// so it can sit in a field / container / return position and each value may hide a DIFFERENT
// concrete type -- that is what makes `Vec[dyn Show]` heterogeneous. `trait` is the written
// name (the checker mangles it); `args` are the parametric trait's arguments, which must be
// written in full (there is no impl here to solve them from -- see resolve_type).
// The node sits at `dyn`; `name_line`/`name_col` anchor the trait name (editor tooling).
struct DynType   : Type { std::string trait; std::string qualifier; std::vector<TypePtr> args;
                          uint32_t name_line = 0, name_col = 0; DynType() : Type(TypeKind::Dyn) {} };

// ----- item nodes (top level) -----------------------------------------------

struct FnItem     : Item { std::string name; std::vector<GenericParam> generics; std::vector<Param> params; TypePtr ret; ExprPtr body; FnItem() : Item(ItemKind::Fn) {} };
// A record struct `struct N[..] { f: T, .. }` (is_tuple=false) OR a tuple struct
// `struct N[..](f0, ..)` / nullary `struct N` (is_tuple=true, positional fields).
// `is_transparent` (only a single-field tuple struct over an Int/Double/Bool field qualifies,
// checked in resolve_struct): the value ERASES to its underlying immediate at runtime -- no
// heap KIND_OBJECT, construction/`.0`/pattern lowered to no-ops. Set by `transparent struct`.
struct StructItem : Item { std::string name; std::vector<GenericParam> generics; std::vector<Field> fields; bool is_tuple = false; bool is_transparent = false; StructItem() : Item(ItemKind::Struct) {} };
struct StmtItem   : Item { StmtPtr stmt;                                     StmtItem()   : Item(ItemKind::Stmt) {} };
// `[pub] const NAME: Type = <literal>` -- a compile-time module constant (inlined at each use site;
// no runtime representation). `value` is restricted (parser) to a single literal (Int/Double/Bool/
// String/char, optionally a negated number). Type annotation is REQUIRED (no inference).
struct ConstItem  : Item { std::string name; TypePtr type; ExprPtr value;   ConstItem()  : Item(ItemKind::Const) {} };
// One variant of an `enum`, mirroring the three struct forms:
//   `V`            nullary  -> is_tuple = true,  0 fields
//   `V(T0, T1)`    tuple    -> is_tuple = true,  positional fields, names synthesized `_i`
//   `V { f: T }`   record   -> is_tuple = false, named fields
// `has_disc`/`disc`: an EXPLICIT discriminant `V = <intlit>` -- valid ONLY on a nullary variant of an
// int-backed enum (`enum E : Int`); checked in resolve_enum. `disc` holds the pinned 48-bit value.
struct EnumVariant { std::string name; bool is_tuple = false; std::vector<Field> fields; bool has_disc = false; int64_t disc = 0; uint32_t line = 0, col = 0; };
// `enum Name [generic_params] [: Repr] { variant, .. }`. Generic params are CHECKED then erased.
// `is_int_backed` (opt-in via a `: Int` repr annotation; every variant must be nullary, checked in
// resolve_enum): the enum ERASES to a bare Int discriminant at runtime -- no heap KIND_OBJECT,
// construction/pattern lowered to Int-immediate ops. `repr` is the annotation's type name (must be `Int`).
struct EnumItem   : Item { std::string name; std::vector<GenericParam> generics; std::vector<EnumVariant> variants; bool is_int_backed = false; std::string repr; EnumItem() : Item(ItemKind::Enum) {} };

// ----- module items (parsed here; resolved by the loader/checker in later slices) --
//
// `import a::b::c` -- declare a dependency on module `a::b::c` and make its members
// reachable via a qualified path (`c::name`). `path` holds the `::`-separated
// segments (lowercase module names); the loader maps them to `a/b/c.skn`.
struct ImportItem : Item { std::vector<std::string> path; ImportItem() : Item(ItemKind::Import) {} };
// `use a::b::{x, Y}` (explicit list) / `use a::b::*` (glob) / `use a::b::name` (single) --
// bring names from module `a::b` into unqualified scope. `path` = the module path
// segments; `names` = the imported leaf names (one element for the `::name` form,
// empty for a glob); `glob` = the `::*` wildcard. A glob binds WEAKLY (a local def or
// an explicit `use` overrides it silently); resolution + shadowing land in Slice 2.
// `name_pos` holds each imported name's position, parallel to `names` (editor tooling).
struct UseItem    : Item { std::vector<std::string> path; std::vector<std::string> names; bool glob = false;
                           std::vector<std::pair<uint32_t, uint32_t>> name_pos; UseItem() : Item(ItemKind::Use) {} };

// `trait Name [generic_params] [: Super1, Super2] { fn m(self) -> T [{ default }] … }`.
// Trait names + supertraits are UPPERCASE. `generics` are the trait's own type params
// (`trait Iterable[T]`), distinct from `Self` and from method-level generics; they may
// appear in method signatures (`fn iter(self) -> Vec[T]`).
// `supertrait_pos` is parallel to `supertraits` (editor tooling).
struct TraitDecl  : Item { std::string name; std::vector<GenericParam> generics;
                           std::vector<std::string> supertraits;
                           std::vector<std::pair<uint32_t, uint32_t>> supertrait_pos;
                           std::vector<Method> methods;                     TraitDecl() : Item(ItemKind::Trait) {} };
// `impl [generic_params] TraitName [trait_args] for TargetType { fn m(self){…} … }`.
// `target` is a NamedType (`List[T]`), so generic impls (`impl[T] Trait for List[T]`) are
// expressible; `trait_args` are the trait's type arguments (`impl[X] Iterable[X] for …`).
// A traitless `impl [G] Target { … }` (inherent methods) sets `is_inherent` + empty `trait_name`.
// `trait_line`/`trait_col` anchor the trait name of a trait impl (editor tooling).
struct ImplDecl   : Item { std::vector<GenericParam> generics; std::string trait_name;
                           std::vector<TypePtr> trait_args; TypePtr target; bool is_inherent = false;
                           uint32_t trait_line = 0, trait_col = 0;
                           std::vector<Method> methods;                     ImplDecl()  : Item(ItemKind::Impl) {} };

// A whole compilation unit: an ordered list of top-level items.
//
// `prelude_item_count` marks the prelude/user boundary for future tree-shaking:
// items[0 .. prelude_item_count) come from the standard prelude, the rest from the
// user program. 0 = no prelude.
struct Program { std::vector<ItemPtr> items; size_t prelude_item_count = 0; };

// Module prefixes + the mangling rule now live in Naming.h -- Types.cpp needs them too and
// must not depend on the AST. Ast.h re-exports them by including it.

// ----- std / prelude name centralization (stdlib-split Step 1) ---------------
//
// The compiler refers to a handful of prelude ENTITIES by name from its own code
// (not from user source): the core enums/variants Option/Result/Some/None/Ok/Err,
// the iteration traits Iterator/IntoIterator/Iterable, and the process struct
// ProcessOutput. These names are consumed at three points that must all agree on
// the SAME (mangled) string: the checker builds/compares the types (make_named,
// `ty->name == ...`), codegen looks them up (find_struct / resolve_method), and the
// tree-shaker seeds them (enum_idx.find / names.insert).
//
// Today every prelude item lives under PRELUDE_MODULE_PREFIX, which mangle_name()
// special-cases to a BARE name -- so all three see e.g. "Option". When the stdlib is
// split into real ring modules, `Option` will live in `std::core` and mangle to
// "std::core::Option"; the bare literals scattered across the compiler would then
// silently miss. Routing them through the helpers below -- the ONE source of truth --
// keeps the current behavior byte-identical (the STD_* prefixes are the prelude
// prefix today, so std_Option() == "Option") AND makes the eventual split a single
// edit: reassign STD_CORE / STD_ITER / STD_PROCESS to the real module paths and
// every internal lookup follows. NOTE: this covers only the prelude ENTITIES above;
// the builtin container TYPES (Array/Vec/Map/List/Bytes/String), builtin fn names,
// native names, and trait METHOD names (next/iter/intoIter) are AMBIENT and stay
// bare literals -- they are never module-scoped.

// `mangle_name` / `bare_scope` / `short_name` / `module_prefix_of` / `display_name` and the two
// module-prefix constants live in Naming.h (included above) -- the semantic type layer needs them
// too and must not depend on the AST.

// The AMBIENT builtin function names -- opcode-backed calls that belong to no module and are
// therefore never mangled. The single source of truth: the checker consults it to decide whether
// a user fn may shadow the name outside its own scope, and `Codegen::is_builtin_name` delegates
// here (the two lists silently drifting apart would resurrect the routing split this closes).
// A TABLE rather than a `||` chain so the population is ENUMERABLE: the differential suite's
// builtin-coverage assertion iterates it to require that every one of these was actually exercised
// against the VM (a string population has no C4062-style compile-time guard, so an iterable source of
// truth is what keeps the test from having to duplicate this list -- exactly the drift warned about
// above). Callers are compile-time only (checker + codegen), so the scan costs what the chain cost.
inline constexpr std::string_view BUILTIN_FN_NAMES[] = {
    // Containers: fixed/growable construction, append, length
    "array", "emptyArray", "vec", "push", "len",
    // Universal / opcode-backed builtins: stringify, output, abort
    "toString", "print", "println", "panic",
    // Map builtins: read / membership / removal / snapshot
    "has", "delete", "get", "keys", "values",
    // Internal live-map cursor primitives (behind the prelude MapCursor)
    "mapIterNext", "mapKeyAt", "mapValAt",
    // Vec pop + Bytes conversions
    "pop", "toBytes", "fromBytes", "bytes",
    // Bulk byte-blit: BYTES_APPEND (whole) / BYTES_APPEND_RANGE (sub-range, internal)
    "appendBytes", "_appendBytesRange",
    // Double <-> Int conversions + Double rounding (DROUND)
    "toInt", "toDouble",
    "floor", "ceil", "trunc", "round", "roundHalfToEven",
    // Erasure projection: an int-backed enum -> its Int discriminant. The one builtin here backed by
    // NO opcode -- the value already IS that Int, so codegen is an identity.
    "ordinal",
};

inline bool is_builtin_fn_name(const std::string& name) {
    for (std::string_view n : BUILTIN_FN_NAMES)
        if (n == name) return true;
    return false;
}


// The ring module each entity will live in after the split. ALL equal to the prelude
// prefix today => mangle_name() returns the bare name => byte-identical behavior. The
// stdlib split flips these three to "std::core" / "std::iter" / "std::process".
constexpr const char* STD_CORE    = "std::core";    // Option/Result + variants + combinators   (ring)
constexpr const char* STD_ITER    = "std::iter";    // Iterator/IntoIterator/Iterable + combinators (ring)
constexpr const char* STD_STRING  = "std::string";  // charStr/slice (+ future string helpers)    (ring)
constexpr const char* STD_PROCESS = "std::process"; // ProcessOutput + run/sh/... + the rawRun native (opt-in)
// Opt-in modules. STD_IO and STD_ENV have NO prelude source of their own -- they exist purely to gate
// the ambient natives, which is why `use std::env::*` works with nothing to import. STD_BYTES DOES have
// a source file (std/bytes.skn, in std/modules.manifest): the LE binary reader/writer shipped.
constexpr const char* STD_IO      = "std::io";      // file/stdin natives (readFile/writeFile/...)  (gate only)
constexpr const char* STD_ENV     = "std::env";     // env/time natives (getEnv/args/nanoTime/...)  (gate only)
constexpr const char* STD_BYTES   = "std::bytes";   // LE binary reader/writer + ByteReader          (opt-in)
constexpr const char* STD_MATH    = "std::math";    // math natives (sqrt/pow/... ) + toIntChecked   (opt-in)
constexpr const char* STD_NET     = "std::net";     // TCP networking natives (tcpConnect/tcpSend/...) (opt-in)
constexpr const char* STD_HASH    = "std::hash";    // hashing: crc32 (prelude) + the sha256 native       (opt-in)
constexpr const char* STD_POLL    = "std::poll";    // non-blocking I/O + readiness (rawPoll/rawRecvNb/...) (opt-in)
constexpr const char* STD_TASK    = "std::task";    // fork-join tasks: Task[R] + spawn + join (rawSpawn/rawJoin/...) (opt-in)
constexpr const char* STD_ACTOR   = "std::actor";   // actors: Pid/Inbox/Mail + spawnActor/send/receive (rawSpawnActor/...) (opt-in)

// Canonical (currently-mangled) names of the compiler-referenced prelude entities.
inline std::string std_Option()       { return mangle_name(STD_CORE, "Option"); }
inline std::string std_Result()       { return mangle_name(STD_CORE, "Result"); }
inline std::string std_Some()         { return mangle_name(STD_CORE, "Some"); }
inline std::string std_None()         { return mangle_name(STD_CORE, "None"); }
inline std::string std_Ok()           { return mangle_name(STD_CORE, "Ok"); }
inline std::string std_Err()          { return mangle_name(STD_CORE, "Err"); }
inline std::string std_Iterator()     { return mangle_name(STD_ITER, "Iterator"); }
inline std::string std_IntoIterator() { return mangle_name(STD_ITER, "IntoIterator"); }
inline std::string std_Iterable()     { return mangle_name(STD_ITER, "Iterable"); }
inline std::string std_Eq()           { return mangle_name(STD_CORE, "Eq"); }        // the sealed structural-== marker
inline std::string std_Hashable()     { return mangle_name(STD_CORE, "Hashable"); }  // the map-key marker
inline std::string std_MustUse()      { return mangle_name(STD_CORE, "MustUse"); }   // the open must-use marker
inline std::string std_Sendable()     { return mangle_name(STD_CORE, "Sendable"); }  // the sealed can-be-sent marker
inline std::string std_ProcessOutput(){ return mangle_name(STD_PROCESS, "ProcessOutput"); }
inline std::string std_Char()          { return mangle_name(STD_STRING, "Char"); }
inline std::string std_codePointToStr(){ return mangle_name(STD_STRING, "codePointToStr"); }
inline std::string std_Task()         { return mangle_name(STD_TASK, "Task"); }       // a fork-join task handle
inline std::string std_spawn()        { return mangle_name(STD_TASK, "spawn"); }      // its one constructor
inline std::string std_spawnActor()   { return mangle_name(STD_ACTOR, "spawnActor"); }
inline std::string std_mainInbox()    { return mangle_name(STD_ACTOR, "mainInbox"); }
inline std::string std_spawnActorBounded() { return mangle_name(STD_ACTOR, "spawnActorBounded"); }
inline std::string std_newInbox()     { return mangle_name(STD_ACTOR, "newInbox"); }
inline std::string std_newBoundedInbox() { return mangle_name(STD_ACTOR, "newBoundedInbox"); }
inline std::string std_ask()          { return mangle_name(STD_ACTOR, "ask"); }
inline std::string std_Pid()          { return mangle_name(STD_ACTOR, "Pid"); }
inline std::string std_Inbox()        { return mangle_name(STD_ACTOR, "Inbox"); }
// The checked function values: made only by `actorFn(f)` / `taskFn(f)` with a named function.
inline std::string std_ActorFn()      { return mangle_name(STD_ACTOR, "ActorFn"); }
inline std::string std_actorFn()      { return mangle_name(STD_ACTOR, "actorFn"); }
inline std::string std_TaskFn()       { return mangle_name(STD_TASK, "TaskFn"); }
inline std::string std_taskFn()       { return mangle_name(STD_TASK, "taskFn"); }

// ----- debug rendering ------------------------------------------------------

// Stable S-expression rendering of a subtree (used by the tests and diagnostics).
// Structural only -- dispatches on `kind`, and does NOT render the resolved `ty`
// slot (which is null until the checker runs). Keep in sync with the node set.
std::string dump(const Program& p);
// Like dump(), but each Expr node is suffixed `:<describe(e.ty)>` (the resolved type the checker
// filled) -- the view that answers "what did the checker actually infer HERE?". Run svc::check /
// svc::parse_check(_modules) first to populate the ty slots; an unfilled slot renders
// describe(nullptr) == "?".
//
// Prelude items are SKIPPED by default (Program::prelude_item_count marks the boundary): with the
// embedded std that is thousands of items, which would bury the program under inspection. Pass
// include_prelude=true to render the whole combined program.
//
// Reached from `static_vmrun --dump-ast`. Its original consumer was the skarnc_diff differential,
// deleted with the self-hosting experiment; the driver flag replaced it so this path
// keeps being executed instead of rotting uncalled.
std::string dump_typed(const Program& p, bool include_prelude = false);
std::string dump_expr(const Expr& e);
std::string dump_stmt(const Stmt& s);
std::string dump_pat(const Pattern& p);
std::string dump_type(const Type& t);
std::string dump_item(const Item& i);

// Visit every expression of `item` in pre-order: fn / method / lambda bodies, match guards, the
// literal, range-bound and map-key expressions inside patterns, and const initializers. Types are
// syntax (`Type`), not expressions, and are not visited.
void for_each_expr(Item& item, const std::function<void(Expr&)>& fn);

} // namespace svc
