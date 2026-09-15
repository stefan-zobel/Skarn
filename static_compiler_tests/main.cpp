// =============================================================================
// static_compiler_tests -- test executable for the Skarn front end.
//
// Links the `static_compiler` static library. For now it exercises the lexer
// (Lexer.h): the case-based uident/lident split, keyword classification (incl.
// `self`, and NO `nil`), numeric/string literals, longest-match operators, and
// Swift/Go automatic statement termination (ASI) -- then the parser, typed AST,
// typechecker, codegen, the RefEval differential and the program generator. The
// front end is feature-complete.
// =============================================================================

#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>   // std::function -- the shrinker's failure predicate
#include <filesystem>   // temp dir for the native file-I/O tests
#include <cstdlib>      // _putenv_s -- set a deterministic var for the getEnv test
#include <chrono>       // steady_clock -- Phase-0 GC benchmark wall-clock timing
#include <format>       // std::format -- Phase-0 GC benchmark table formatting
#include <algorithm>    // std::fill -- Phase-0 barrier probe state reset

#include "Lexer.h"
#include "Token.h"
#include "Ast.h"
#include "Types.h"
#include "Parser.h"
#include "Loader.h"     // svc::load_modules / ModuleSet / LoadError -- the multi-module loader
#include "Solver.h"
#include "Check.h"
#include "Compiler.h"   // svc::compile -> Module, CheckFailure
#include "Codegen.h"    // svc::CodegenError (inspect codegen rejections)
#include "Execute.h"    // execute() + Heap / StringInterner / Value / GcObject
#include "Natives.h"    // build_native_table() -- exercise the file/time/env/stdin natives
#include "RefEval.h"    // refeval:: -- the tree-walking oracle for the differential harness
#include "Generator.h"  // gen::generate -- the well-typed program generator (property testing)

namespace {

int g_pass = 0;
int g_fail = 0;

// Space-joined token-kind names for a source string (includes the final Eof).
// Skarn has no atoms, but execute() gates the true/false/nil TO_STRING pool on a NON-NULL
// atom_names table (vmcore is unchanged). Pass this EMPTY one everywhere so that pool builds.
static const std::vector<std::string> kNoAtoms;

std::string kinds_str(const std::string& src) {
    svc::Lexer lex(src);
    auto toks = lex.tokenize();
    std::string s;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (i) s += ' ';
        s += svc::to_string(toks[i].kind);
    }
    return s;
}

void check_str(const char* name, const std::string& got, const std::string& expect) {
    if (got == expect) {
        ++g_pass;
        std::cout << "  CORRECT!  " << name << "\n";
    } else {
        ++g_fail;
        std::cout << "  WRONG!    " << name << "\n"
                  << "     expected: " << expect << "\n"
                  << "     got:      " << got << "\n";
    }
}

void check_true(const char* name, bool cond) {
    if (cond) {
        ++g_pass;
        std::cout << "  CORRECT!  " << name << "\n";
    } else {
        ++g_fail;
        std::cout << "  WRONG!    " << name << "\n";
    }
}

// True iff lexing `src` throws a LexError.
bool lex_throws(const std::string& src) {
    try {
        svc::Lexer lex(src);
        lex.tokenize();
        return false;
    } catch (const svc::LexError&) {
        return true;
    }
}

// First token of `src` (or Eof if the stream is somehow empty of real tokens).
svc::Token first_tok(const std::string& src) {
    svc::Lexer lex(src);
    auto toks = lex.tokenize();
    return toks.empty() ? svc::Token{} : toks.front();
}

// ---- test groups ------------------------------------------------------------

void test_case_classification() {
    std::cout << "[case classification]\n";
    check_str("uident",        kinds_str("Foo"),  "UIdent <eof>");
    check_str("lident",        kinds_str("foo"),  "LIdent <eof>");
    check_str("lident_uscore", kinds_str("_x"),   "LIdent <eof>");
    check_str("lident_digit",  kinds_str("x1"),   "LIdent <eof>");
    check_str("wildcard",      kinds_str("_"),    "_ <eof>");
    // A capitalized non-keyword is a UIdent (`Self` the type), while the lowercase
    // `self` is the reserved keyword.
    check_str("Self_is_uident", kinds_str("Self"), "UIdent <eof>");
    check_str("self_is_kw",     kinds_str("self"), "self <eof>");
}

void test_keywords() {
    std::cout << "[keywords]\n";
    check_str("all_keywords",
              kinds_str("let mut fn struct enum match if else while return "
                        "true false for in break continue trait impl self"),
              "let mut fn struct enum match if else while return "
              "true false for in break continue trait impl self <eof>");
    // `nil` is NOT a keyword in the static language -- it lexes as a plain lident.
    check_str("nil_is_lident", kinds_str("nil"), "LIdent <eof>");
}

void test_numbers() {
    std::cout << "[numbers]\n";
    check_str("int",    kinds_str("42"),  "Int <eof>");
    check_str("double", kinds_str("3.5"), "Double <eof>");
    check_true("int_val",    first_tok("42").int_val == 42);
    check_true("double_val", first_tok("3.5").double_val == 3.5);
    // A number after a member dot is a tuple index (integer), so `t.0.1` is the
    // nested access `(t.0).1`, NOT `t . (0.1)`.
    check_str("tuple_index", kinds_str("t.0.1"), "LIdent . Int . Int <eof>");
    // Exponent notation -> Double (even with an integer mantissa). e/E, optional sign, digits.
    check_str("exp_int_mant",  kinds_str("1e10"),    "Double <eof>");
    check_str("exp_frac",      kinds_str("2.5e3"),   "Double <eof>");
    check_str("exp_upper_neg", kinds_str("6.022E-2"),"Double <eof>");
    check_str("exp_plus",      kinds_str("1e+5"),    "Double <eof>");
    check_true("exp_val",      first_tok("1e3").double_val == 1000.0);
    check_true("exp_neg_val",  first_tok("2.5e-1").double_val == 0.25);
    // A trailing `e` with no following digit is NOT part of the number: `1e` -> Int `1`, ident `e`.
    check_str("exp_bare_e",    kinds_str("1e"),      "Int LIdent <eof>");
    check_str("exp_no_digit",  kinds_str("1e+"),     "Int LIdent + <eof>");
    // A tuple index stays an integer -- no exponent there either.
    check_str("exp_after_dot", kinds_str("t.0e5"),   "LIdent . Int LIdent <eof>");
    // Radix-prefixed integer literals (Option B: a bit pattern up to 48 bits, sign-extended from bit 47).
    check_str("hex_kind",    kinds_str("0xFF"), "Int <eof>");
    check_true("hex_val",    first_tok("0xFF").int_val == 255);
    check_true("hex_u32",    first_tok("0xFFFFFFFF").int_val == 4294967295LL);
    check_true("hex_neg1",   first_tok("0xFFFFFFFFFFFF").int_val == -1);            // full-width all-ones
    check_true("hex_min48",  first_tok("0x800000000000").int_val == -140737488355328LL);  // sign bit -> MIN_48
    check_true("hex_upper",  first_tok("0XaB").int_val == 171);                     // case-insensitive
    check_true("oct_val",    first_tok("0o17").int_val == 15);
    check_true("oct_upper",  first_tok("0O777").int_val == 511);
    check_true("bin_val",    first_tok("0b1010").int_val == 10);
    check_true("bin_upper",  first_tok("0B101").int_val == 5);
    // A radix prefix with no valid digit is NOT a number prefix: `0xG` -> Int 0, ident `xG`.
    check_str("radix_baddigit", kinds_str("0xG"), "Int LIdent <eof>");
    // A tuple index stays plain decimal: `t.0x1` -> (t.0) then ident `x1`.
    check_str("radix_after_dot", kinds_str("t.0x1"), "LIdent . Int LIdent <eof>");
    // Beyond 2^48 is a lex-time range error (hex, octal, binary).
    check_true("hex_oob", lex_throws("0x1000000000000"));                           // 2^48
    check_true("oct_oob", lex_throws("0o10000000000000000"));                       // 2^48
    check_true("bin_oob", lex_throws("0b1" + std::string(48, '0')));               // 2^48

    // ---- digit separators: `_` between digits, in EVERY numeric form -----------------------------
    // The value must equal the un-separated literal exactly; the token TEXT keeps the separators
    // (diagnostics quote the source), which is why the value is parsed from a stripped copy.
    check_str ("sep_kind",      kinds_str("1_000"), "Int <eof>");
    check_true("sep_dec",       first_tok("1_000_000").int_val == 1000000);
    check_true("sep_text_kept", first_tok("1_000_000").text == "1_000_000");
    check_true("sep_hex",       first_tok("0xFF_FF").int_val == 0xFFFF);
    check_true("sep_hex_mask",  first_tok("0xFFFF_FFFF_FFFF").int_val == -1);   // the guide's all-ones mask
    check_true("sep_oct",       first_tok("0o1_7").int_val == 15);
    check_true("sep_bin",       first_tok("0b1010_1010").int_val == 170);
    check_true("sep_frac",      first_tok("3.141_592").double_val == 3.141592);
    check_true("sep_exp",       first_tok("1e1_0").double_val == 1e10);
    check_true("sep_everywhere",first_tok("1_2.3_4e0_1").double_val == 123.4);
    // Java's rule, so a RUN of separators between two digits is fine (`1__0` == 10).
    check_true("sep_run",       first_tok("1__0").int_val == 10);
    check_true("sep_run_hex",   first_tok("0xF__F").int_val == 255);
    // ... but the run must be flanked by digits on BOTH sides. Each of these used to lex as a number
    // followed by an identifier (`1_x` -> `1` + `_x`), reporting an unknown variable instead of the
    // actual mistake -- the reason this rejects rather than merely stopping the run.
    check_true("sep_trailing",   lex_throws("1_"));
    check_true("sep_trailing2",  lex_throws("1__"));
    check_true("sep_before_dot", lex_throws("1_.5"));
    check_true("sep_before_id",  lex_throws("1_x"));
    check_true("sep_exp_trail",  lex_throws("1e5_"));
    // A separator may not OPEN the digit run either. Without the explicit guard, `0x_FF` would slip
    // through the radix prefix check (which requires a digit at peek(2)) into the `0` + identifier
    // fall-through that `0xG` deliberately takes -- a confusing "unknown variable 'x_FF'".
    check_true("sep_after_prefix_hex", lex_throws("0x_FF"));
    check_true("sep_after_prefix_oct", lex_throws("0o_7"));
    check_true("sep_after_prefix_bin", lex_throws("0b_1"));
    // Unchanged neighbours: a leading `_` is an ordinary identifier, `1._5` is a member access (`.`
    // is a decimal point only before a DIGIT -- Java rejects it too), and `0xG` still falls through.
    check_str("sep_leading_is_ident", kinds_str("_1"),    "LIdent <eof>");
    check_str("sep_dot_then_sep",     kinds_str("1._5"),  "Int . LIdent <eof>");
    check_str("sep_radix_baddigit",   kinds_str("0xG"),   "Int LIdent <eof>");
    // A tuple index shares the digit run, so it accepts separators too (`t.1_0` == index 10).
    // Harmless and needs no rule of its own.
    check_str("sep_tuple_index",      kinds_str("t.1_0"), "LIdent . Int <eof>");
}

void test_char_literals() {
    std::cout << "[char literals]\n";
    // A char literal lexes to an ordinary Int token (single byte 0..255).
    check_str ("char_lit_kind",       kinds_str("'A'"), "Int <eof>");
    check_true("char_lit_val",        first_tok("'A'").int_val == 65);
    check_true("char_lit_digit",      first_tok("'0'").int_val == 48);
    check_true("char_lit_brace",      first_tok("'{'").int_val == 123);
    // Escapes: \n \t \r \\ \0 \' \" (same table as strings, plus \').
    check_true("char_lit_newline",    first_tok("'\\n'").int_val == 10);   // '\n'
    check_true("char_lit_tab",        first_tok("'\\t'").int_val == 9);    // '\t'
    check_true("char_lit_squote",     first_tok("'\\''").int_val == 39);   // '\''
    check_true("char_lit_backslash",  first_tok("'\\\\'").int_val == 92);  // '\\'
    check_true("char_lit_nul",        first_tok("'\\0'").int_val == 0);    // '\0'
    // A bare double-quote inside a char literal needs no escape.
    check_true("char_lit_dquote",     first_tok("'\"'").int_val == 34);
    // Errors (D4/D2): empty, multi-char, multi-byte UTF-8, unterminated, bad escape.
    check_true("char_empty_err",        lex_throws("''"));
    check_true("char_multi_err",        lex_throws("'ab'"));
    check_true("char_multibyte_err",    lex_throws("'\xc3\xa9'"));         // 'é' = 2 UTF-8 bytes
    check_true("char_unterminated_err", lex_throws("'A"));
    check_true("char_bad_escape_err",   lex_throws("'\\q'"));             // '\q'
}

void test_asi() {
    std::cout << "[ASI]\n";
    // A leading `|>` on the next line continues the previous one (no StmtEnd).
    check_str("pipe_continues", kinds_str("x\n|> f"), "LIdent |> LIdent <eof>");
    // `{` is not a value-ender, so the newline before `}` inserts no StmtEnd.
    check_str("if_header_brace", kinds_str("if c {\n}"), "if LIdent { } <eof>");
    // Two statements on separate lines get a StmtEnd between them.
    check_str("two_statements", kinds_str("a\nb"), "LIdent <stmt-end> LIdent <eof>");
    // An explicit `;` is a StmtEnd.
    check_str("explicit_semi", kinds_str("a; b"), "LIdent <stmt-end> LIdent <eof>");
    // `return x` completes a value; the following line is a new statement.
    check_str("return_then_stmt", kinds_str("return x\ny"),
              "return LIdent <stmt-end> LIdent <eof>");
}

void test_colons() {
    std::cout << "[colons]\n";
    check_str("annotation_colon", kinds_str("x: Int"),    "LIdent : UIdent <eof>");
    check_str("qualified_path",   kinds_str("Trait::m"),  "UIdent :: LIdent <eof>");
    // Atoms were removed from Skarn: a leading `:name` is now just a colon + ident (no Atom token).
    check_str("no_atom_token",    kinds_str("f(:ok)"),    "LIdent ( : LIdent ) <eof>");
}

void test_operators() {
    std::cout << "[operators]\n";
    check_str("longest_match",
              kinds_str("-> => |> >>> <= == .. ? #"),
              "-> => |> >>> <= == .. ? # <eof>");
    check_str("shifts_and_bits",
              kinds_str("<< >> >>> & | ^ ~"),
              "<< >> >>> & | ^ ~ <eof>");
    check_str("compare",
              kinds_str("< <= > >= == !="),
              "< <= > >= == != <eof>");
}

void test_strings() {
    std::cout << "[strings]\n";
    check_str("string_kind", kinds_str("\"hi\""), "Str <eof>");
    check_true("string_text",   first_tok("\"hi\"").text == "hi");
    check_true("string_escape", first_tok("\"a\\nb\"").text == "a\nb");
}

// Raw string literals `r"…"` / `r#"…"#` -- a plain Str token, taken byte-for-byte (no escapes, no
// `${…}` interpolation), Rust-style hash counting for embedded `"`, multi-line allowed. Lexer-only.
void test_raw_strings() {
    std::cout << "[raw strings]\n";
    // A raw string lexes to an ordinary Str token, same as a normal string literal.
    check_str ("raw_kind",       kinds_str("r\"hi\""), "Str <eof>");
    check_true("raw_text",       first_tok("r\"hi\"").text == "hi");
    // No backslash escapes: `\n` is two bytes (backslash, n), not a newline.
    check_true("raw_no_escape",  first_tok("r\"a\\nb\"").text == "a\\nb");   // a \ n b (4 bytes)
    // No `${…}` interpolation: the bytes are literal.
    check_true("raw_no_interp",  first_tok("r\"${x}\"").text == "${x}");
    // Empty raw strings, both forms.
    check_true("raw_empty",      first_tok("r\"\"").text.empty());
    check_true("raw_hash_empty", first_tok("r#\"\"#").text.empty());
    // Hash form carries an embedded `"` (closes only on `"#`).
    check_true("raw_hash_quote", first_tok("r#\"a\"b\"#").text == "a\"b");
    // Double-hash: a `"#` inside is content; only `"##` closes.
    check_true("raw_hash2",      first_tok("r##\"a\"# b\"##").text == "a\"# b");
    // Multi-line: a real newline is part of the content.
    check_true("raw_multiline",  first_tok("r\"a\nb\"").text == "a\nb");
    // A `}` inside a raw string is not special (the scan tracks no braces).
    check_true("raw_brace",      first_tok("r\"a}b\"").text == "a}b");
    // Disambiguation: `r` NOT abutting a quote stays an ordinary identifier.
    check_str ("raw_ident_r",    kinds_str("r"),        "LIdent <eof>");
    check_str ("raw_ident_word", kinds_str("range"),    "LIdent <eof>");   // starts with r, not a raw string
    check_str ("raw_ident_red",  kinds_str("red"),      "LIdent <eof>");
    check_str ("raw_ident_expr", kinds_str("r + 1"),    "LIdent + Int <eof>");
    check_str ("raw_let_r",      kinds_str("let r = 5"),"let LIdent = Int <eof>");
    // `r#{` is ident `r` then a `#`-hash token (a map literal open), NOT a raw string (no closing `"`).
    check_str ("raw_r_hash_map", kinds_str("r#{"),      "LIdent # { <eof>");
    // Unterminated raw strings throw.
    check_true("raw_unterminated",      lex_throws("r\"abc"));
    check_true("raw_hash_unterminated", lex_throws("r#\"abc\""));   // needs `"#`, only `"` present
}

void test_errors() {
    std::cout << "[errors]\n";
    check_true("unterminated_string",  lex_throws("\"abc"));
    check_true("unterminated_comment", lex_throws("/* abc"));
    // `@` used to be the stray character here; it is a real token now (pattern binding), so the
    // test moved to one that is still genuinely unlexable.
    check_true("stray_char",           lex_throws("$"));
    check_true("stray_char_backtick",  lex_throws("`"));
    check_true("at_lexes",            !lex_throws("@"));
    check_true("bad_escape",           lex_throws("\"a\\qb\""));
}

// ---- typed AST --------------------------------------------------------------
// No parser yet, so trees are hand-built with small factory helpers and asserted
// via dump(). This exercises the node set + the static deltas (DoubleLit, no nil,
// TryExpr, CtorPat, generics-with-bounds, retained/mandatory types, impl target).

svc::ExprPtr e_int(int64_t v)  { auto n = std::make_unique<svc::IntLit>();    n->value = v; return n; }
svc::ExprPtr e_dbl(double v)   { auto n = std::make_unique<svc::DoubleLit>(); n->value = v; return n; }
svc::ExprPtr e_str(std::string s) { auto n = std::make_unique<svc::StrLit>(); n->value = std::move(s); return n; }
svc::ExprPtr e_id(std::string nm, bool up = false, std::string q = "") {
    auto n = std::make_unique<svc::IdentExpr>(); n->name = std::move(nm); n->upper = up; n->qualifier = std::move(q); return n;
}
svc::ExprPtr e_block(std::vector<svc::StmtPtr> stmts) {
    auto n = std::make_unique<svc::BlockExpr>(); n->stmts = std::move(stmts); return n;
}
svc::StmtPtr s_expr(svc::ExprPtr e) { auto n = std::make_unique<svc::ExprStmt>(); n->expr = std::move(e); return n; }

svc::TypePtr t_named(std::string nm) { auto n = std::make_unique<svc::NamedType>(); n->name = std::move(nm); return n; }
svc::TypePtr t_named1(std::string nm, svc::TypePtr a) {
    auto n = std::make_unique<svc::NamedType>(); n->name = std::move(nm); n->args.push_back(std::move(a)); return n;
}

svc::PatPtr p_id(std::string nm) { auto n = std::make_unique<svc::IdentPat>(); n->name = std::move(nm); return n; }
svc::PatPtr p_ctor(std::string nm, bool parens, std::vector<svc::PatPtr> el = {}) {
    auto n = std::make_unique<svc::CtorPat>(); n->name = std::move(nm); n->has_parens = parens; n->elems = std::move(el); return n;
}

// Build the item `fn[T: Display] id (x: T) -> T { x }`.
svc::ItemPtr build_generic_fn() {
    auto fn = std::make_unique<svc::FnItem>();
    fn->name = "id";
    svc::GenericParam gp_t; gp_t.name = "T"; gp_t.bounds.push_back(svc::BoundRef{"Display"});
    fn->generics.push_back(std::move(gp_t));
    fn->params.push_back(svc::Param{"x", t_named("T")});
    fn->ret = t_named("T");
    std::vector<svc::StmtPtr> body;
    body.push_back(s_expr(e_id("x")));
    fn->body = e_block(std::move(body));
    return fn;
}

// Build `enum Option[T] { None, Some(T) }`.
svc::ItemPtr build_enum() {
    auto en = std::make_unique<svc::EnumItem>();
    en->name = "Option";
    en->generics.push_back(svc::GenericParam{"T", {}});
    svc::EnumVariant none; none.name = "None"; none.is_tuple = true;
    svc::EnumVariant some; some.name = "Some"; some.is_tuple = true;
    some.fields.push_back(svc::Field{"_0", t_named("T")});
    en->variants.push_back(std::move(none));
    en->variants.push_back(std::move(some));
    return en;
}

// Build `impl[T] Show for List[T] { fn show(self) -> String { "x" } }`.
svc::ItemPtr build_impl() {
    auto im = std::make_unique<svc::ImplDecl>();
    im->generics.push_back(svc::GenericParam{"T", {}});
    im->trait_name = "Show";
    im->target = t_named1("List", t_named("T"));
    svc::Method m;
    m.name = "show";
    m.has_self = true;
    m.ret = t_named("String");
    std::vector<svc::StmtPtr> body;
    body.push_back(s_expr(e_str("x")));
    m.body = e_block(std::move(body));
    im->methods.push_back(std::move(m));
    return im;
}

void test_typed_ast() {
    std::cout << "[typed ast]\n";

    // literals -- DoubleLit renders (there is deliberately no NilLit in the enum).
    check_str("double_lit", svc::dump_expr(*e_dbl(3.5)), "3.5");
    check_str("int_lit",    svc::dump_expr(*e_int(42)),  "42");

    // a generic fn with a bound, a typed param, and a return type -- all retained.
    check_str("generic_fn", svc::dump_item(*build_generic_fn()),
              "(fn-item id[T:Display] (x:T) -> T (block x))");

    // record struct + tuple struct, fields carry mandatory types.
    {
        auto s = std::make_unique<svc::StructItem>();
        s->name = "Point";
        s->fields.push_back(svc::Field{"x", t_named("Int")});
        s->fields.push_back(svc::Field{"y", t_named("Int")});
        check_str("struct_item", svc::dump_item(*s), "(struct-item Point (x:Int) (y:Int))");
    }
    {
        auto s = std::make_unique<svc::StructItem>();
        s->name = "Pair"; s->is_tuple = true;
        s->fields.push_back(svc::Field{"_0", t_named("Int")});
        s->fields.push_back(svc::Field{"_1", t_named("Int")});
        check_str("tuple_struct", svc::dump_item(*s), "(struct-item Pair tuple (_0:Int) (_1:Int))");
    }

    check_str("enum_item", svc::dump_item(*build_enum()), "(enum Option[T] (None) (Some _0:T))");

    // match with constructor patterns (None / Some(x)) vs a binding pattern.
    {
        auto m = std::make_unique<svc::MatchExpr>();
        m->scrut = e_id("o");
        svc::MatchArm a0; a0.pat = p_ctor("None", false);          a0.body = e_int(0);
        svc::MatchArm a1; std::vector<svc::PatPtr> el; el.push_back(p_id("x"));
        a1.pat = p_ctor("Some", true, std::move(el));              a1.body = e_id("x");
        m->arms.push_back(std::move(a0));
        m->arms.push_back(std::move(a1));
        check_str("match_ctor", svc::dump_expr(*m),
                  "(match o (arm (ctor None) 0) (arm (ctor Some x) x))");
    }

    // postfix `?` is a first-class TryExpr.
    {
        auto call = std::make_unique<svc::CallExpr>(); call->callee = e_id("foo");
        auto t = std::make_unique<svc::TryExpr>(); t->operand = std::move(call);
        check_str("try_expr", svc::dump_expr(*t), "(try (call foo))");
    }

    // let with and without the optional type annotation.
    {
        auto l = std::make_unique<svc::LetStmt>();
        l->pat = p_id("x"); l->type = t_named("Int"); l->init = e_int(5);
        check_str("let_typed", svc::dump_stmt(*l), "(let x (: Int) 5)");
    }
    {
        auto l = std::make_unique<svc::LetStmt>();
        l->pat = p_id("y"); l->init = e_int(5);
        check_str("let_untyped", svc::dump_stmt(*l), "(let y 5)");
    }

    // impl[T] Show for List[T] with a `self` method.
    check_str("impl_item", svc::dump_item(*build_impl()),
              "(impl Show[T] (List T) (method show (self) -> String (block (str x))))");

    // index a[i] and the sole list literal [1, 2] -- no dropped flags in the dump.
    {
        auto ix = std::make_unique<svc::IndexExpr>(); ix->obj = e_id("a"); ix->index = e_id("i");
        check_str("index", svc::dump_expr(*ix), "(index a i)");
    }
    {
        auto ls = std::make_unique<svc::ListLit>();
        ls->elems.push_back(e_int(1)); ls->elems.push_back(e_int(2));
        check_str("list_lit", svc::dump_expr(*ls), "(list 1 2)");
    }

    // Ty stub: default is Unresolved; a Named type describes with its args.
    {
        svc::Ty unresolved;
        check_str("ty_unresolved", svc::describe(unresolved), "?");
        svc::Ty named;
        named.kind = svc::TyKind::Named; named.name = "List";
        auto intTy = std::make_shared<svc::Ty>(); intTy->kind = svc::TyKind::Int;
        named.args.push_back(intTy);
        check_str("ty_named", svc::describe(named), "List[Int]");

        // A flexible (fresh) var has no name -> anonymous "_", never the raw var_id
        // (which would be meaningless and unstable across compiles). A rigid var keeps
        // the user's own parameter name.
        svc::Ty freshVar; freshVar.kind = svc::TyKind::Var; freshVar.var_id = 42;
        check_str("describe_fresh_var", svc::describe(freshVar), "_");
        svc::Ty namedVar; namedVar.kind = svc::TyKind::Var; namedVar.name = "T"; namedVar.rigid = true;
        check_str("describe_rigid_var", svc::describe(namedVar), "T");
    }

    // TypeRenderer: distinct anonymous flexible vars get stable per-message letters
    // (a, b, ...), so a var that recurs in one message reads the same both times --
    // unlike describe(), which collapses every anonymous var to `_`.
    {
        auto v1 = std::make_shared<svc::Ty>(); v1->kind = svc::TyKind::Var; v1->var_id = 100;
        auto v2 = std::make_shared<svc::Ty>(); v2->kind = svc::TyKind::Var; v2->var_id = 200;
        svc::Ty fn; fn.kind = svc::TyKind::Fn; fn.args = { v1, v2 }; fn.ret = v1;
        svc::TypeRenderer r;
        check_str("renderer_shared_var", r(fn), "fn(a, b) -> a");   // v1 -> a on BOTH sides
        auto tv = std::make_shared<svc::Ty>(); tv->kind = svc::TyKind::Var; tv->name = "T"; tv->rigid = true;
        svc::TypeRenderer r2;
        check_str("renderer_rigid_name", r2(tv), "T");              // rigid/named keeps its name
        check_str("describe_still_underscore", svc::describe(*v1), "_");   // describe() unchanged

        // Two DISTINCT rigid vars sharing a name (both `T`, different var_id) are
        // disambiguated within one message: `T`, then `T#2`. The SAME var (same var_id)
        // reappearing renders identically -- never a false `T#2`.
        auto t1 = std::make_shared<svc::Ty>(); t1->kind = svc::TyKind::Var; t1->name = "T"; t1->rigid = true; t1->var_id = 300;
        auto t2 = std::make_shared<svc::Ty>(); t2->kind = svc::TyKind::Var; t2->name = "T"; t2->rigid = true; t2->var_id = 400;
        svc::Ty fn2; fn2.kind = svc::TyKind::Fn; fn2.args = { t1, t2 }; fn2.ret = t1;
        svc::TypeRenderer r3;
        check_str("renderer_rigid_collision", r3(fn2), "fn(T, T#2) -> T");  // t1->T both sides, t2->T#2
    }
}

// ---- parser -----------------------------------------------------------------
// Assert on the S-expression dump of a parsed program / expression.

std::string prog_dump(const std::string& src) {
    svc::Lexer lex(src);
    svc::Parser p(lex.tokenize());
    return svc::dump(p.parse_program());
}
std::string expr_dump(const std::string& src) {
    svc::Lexer lex(src);
    svc::Parser p(lex.tokenize());
    return svc::dump_expr(*p.parse_expression());
}
// True iff lexing+parsing `src` throws (a ParseError or a LexError).
bool parse_throws(const std::string& src) {
    try {
        svc::Lexer lex(src);
        svc::Parser p(lex.tokenize());
        p.parse_program();
        return false;
    } catch (const svc::ParseError&) { return true; }
      catch (const svc::LexError&)   { return true; }
}

void test_parser_expr() {
    std::cout << "[parser: expressions]\n";
    // precedence + associativity
    check_str("prec_mul",   expr_dump("1 + 2 * 3"),   "(+ 1 (* 2 3))");
    check_str("pipe_lassoc", expr_dump("a |> f |> g"), "(|> (|> a f) g)");
    check_str("unary_prec", expr_dump("-a + b"),       "(+ (- a) b)");
    check_str("cmp_and",    expr_dump("a == b && c"),  "(&& (== a b) c)");
    check_str("shift_chain", expr_dump("1 << 2 >> 3"), "(>> (<< 1 2) 3)");
    // postfix
    check_str("try",    expr_dump("x?"),      "(try x)");
    check_str("call",   expr_dump("f(a, b)"), "(call f a b)");
    check_str("index",  expr_dump("a[i]"),    "(index a i)");
    check_str("field",  expr_dump("p.x"),     "(. p x)");
    check_str("tuple_index", expr_dump("t.0"), "(. t _0)");
    // case-based prefix
    check_str("lident",   expr_dump("foo"),  "foo");
    check_str("uident",   expr_dump("Foo"),  "Foo");
    check_str("bare_ctor",expr_dump("None"), "None");
    check_str("self",     expr_dump("self"), "self");
    check_str("qualified",expr_dump("Trait::method"), "Trait::method");
    check_str("mod_qual_call", expr_dump("util::helper(x)"), "(call util::helper x)");
    check_str("mod_qual_ctor_path", expr_dump("geo::Point"), "geo::Point");
    check_str("struct_lit", expr_dump("Point { x: 1, y: 2 }"),
              "(struct-lit Point (x 1) (y 2))");
    // Record update `Name { f: v, ..base }` -- the `..base` renders as a trailing `(.. base)`.
    check_str("record_update", expr_dump("Point { x: 5, ..p }"),
              "(struct-lit Point (x 5) (.. p))");
    check_str("record_update_copy", expr_dump("Point { ..p }"),
              "(struct-lit Point (.. p))");
    check_true("record_update_base_not_last", parse_throws("Point { ..p, y: 2 }"));
    // Qualified `mod::Name` in the three positions that previously did not parse.
    check_str("qual_struct_lit", expr_dump("geo::Point { x: 1, y: 2 }"),
              "(struct-lit geo::Point (x 1) (y 2))");
    check_str("qual_ctor_pat", expr_dump("match p { geo::Wrap(n) => n, _ => 0 }"),
              "(match p (arm (ctor geo::Wrap n) n) (arm _ 0))");
    check_str("qual_struct_pat", expr_dump("match p { geo::Point { x, y } => x, _ => 0 }"),
              "(match p (arm (struct-pat geo::Point (x) (y)) x) (arm _ 0))");
    check_str("qual_nullary_pat", expr_dump("match c { pal::Red => 1, _ => 0 }"),
              "(match c (arm (ctor pal::Red) 1) (arm _ 0))");
    check_true("qual_no_tail_throws", parse_throws("let x = geo::5\n x"));
    check_str("unit",     expr_dump("()"),     "(tuple)");
    check_str("list",     expr_dump("[1, 2]"), "(list 1 2)");
    check_str("map",      expr_dump("#{ 1 => 2 }"), "(map (1 2))");
}

void test_parser_items() {
    std::cout << "[parser: items]\n";
    check_str("let",       prog_dump("let x = 5"),        "(let x 5)");
    check_str("let_typed", prog_dump("let x: Int = 5"),   "(let x (: Int) 5)");
    check_str("let_mut",   prog_dump("let mut y = 1"),    "(let-mut y 1)");
    check_str("assign",    prog_dump("x = 2"),            "(= x 2)");
    check_str("generic_fn", prog_dump("fn id[T: Display](x: T) -> T { x }"),
              "(fn-item id[T:Display] (x:T) -> T (block x))");
    check_str("record_struct", prog_dump("pub struct Point { x: Int, y: Int }"),
              "(struct-item Point (x:Int) (y:Int))");
    check_str("tuple_struct", prog_dump("struct Pair(Int, Int)"),
              "(struct-item Pair tuple (_0:Int) (_1:Int))");
    check_str("nullary_struct", prog_dump("struct Unit"),
              "(struct-item Unit tuple)");
    check_str("enum", prog_dump("enum Option[T] { None, Some(T) }"),
              "(enum Option[T] (None) (Some _0:T))");
    check_str("enum_record", prog_dump("enum Shape { Circle { r: Int } }"),
              "(enum Shape (Circle r:Int))");
    check_str("trait", prog_dump("trait Ord: Eq { fn cmp(self, other: Self) -> Int }"),
              "(trait Ord (: Eq) (method cmp (self other:Self) -> Int _))");
    check_str("impl", prog_dump("impl[T] Show for List[T] { fn show(self) -> String { \"x\" } }"),
              "(impl Show[T] (List T) (method show (self) -> String (block (str x))))");
    // Parametric traits + parametric bounds + impl trait args (generic-traits feature).
    check_str("trait_generic", prog_dump("trait Iterable[T] { fn iter(self) -> Vec[T] }"),
              "(trait Iterable[T] (method iter (self) -> (Vec T) _))");
    check_str("parametric_bound",
              prog_dump("fn toVec[T, I: Iterable[T]](it: I) -> Vec[T] { it }"),
              "(fn-item toVec[T,I:Iterable[T]] (it:I) -> (Vec T) (block it))");
    check_true("impl_trait_args_parses",
               !parse_throws("impl[X] Iterable[X] for List[X] { fn iter(self) -> Vec[X] { self } }"));
    // ASI + header disambiguation
    check_str("pipe_multiline", prog_dump("x\n|> f"), "(|> x f)");
    check_str("if_header", prog_dump("if c { 1 } else { 2 }"),
              "(if c (block 1) (block 2))");
    // Modules (Slice 1a): import + use parsing. Structure only -- resolution is Slice 2.
    check_str("import_single",  prog_dump("import util"),          "(import util)");
    check_str("import_path",    prog_dump("import net::http"),     "(import net::http)");
    check_str("use_single",     prog_dump("use util::helper"),     "(use util {helper})");
    check_str("use_single_up",  prog_dump("use grafik::Color"),    "(use grafik {Color})");
    check_str("use_list",       prog_dump("use util::{a, B}"),     "(use util {a B})");
    check_str("use_list_path",  prog_dump("use net::http::{get, post}"),
              "(use net::http {get post})");
    check_str("use_glob",       prog_dump("use util::*"),          "(use util *)");
    check_str("use_deep_name",  prog_dump("use net::http::Client"),"(use net::http {Client})");
    check_str("use_trailing_comma", prog_dump("use util::{a, b,}"), "(use util {a b})");
    // Malformed module decls fail-fast.
    check_true("use_needs_name",   parse_throws("use util"));       // no `::name`/`::{}`/`::*`
    check_true("import_needs_name", parse_throws("import"));        // bare keyword
    check_true("use_needs_path",   parse_throws("use"));           // bare keyword
    check_true("import_upper_rejected", parse_throws("import Foo")); // module names are lowercase
}

void test_parser_patterns() {
    std::cout << "[parser: patterns]\n";
    check_str("ctor_pats", expr_dump("match o { None => 0, Some(x) => x }"),
              "(match o (arm (ctor None) 0) (arm (ctor Some x) x))");
    check_str("struct_wild", expr_dump("match p { Point { x, y } => x, _ => 0 }"),
              "(match p (arm (struct-pat Point (x) (y)) x) (arm _ 0))");
    check_str("tuple_pat", expr_dump("match t { (a, b) => a }"),
              "(match t (arm (tuple-pat a b) a))");
    check_str("list_pat", expr_dump("match xs { [h, ..t] => h, [] => 0 }"),
              "(match xs (arm (list-pat h (.. t)) h) (arm (list-pat) 0))");
    check_str("literal_neg", expr_dump("match n { -1 => 0, 1 => 2 }"),
              "(match n (arm (- 1) 0) (arm 1 2))");
    check_str("guard", expr_dump("match n { x if x > 0 => 1 }"),
              "(match n (arm x (guard (> x 0)) 1))");
    check_str("map_pat", expr_dump("match m { #{ 1 => x } => x }"),
              "(match m (arm (map-pat (1 => x)) x))");
}

void test_parser_errors() {
    std::cout << "[parser: errors]\n";
    check_true("missing_ret_type",  parse_throws("fn f() { 1 }"));
    check_true("untyped_param",      parse_throws("fn f(x) -> Int { x }"));
    check_true("unterminated_block", parse_throws("fn f() -> Int { 1"));
    check_true("hash_not_brace",     parse_throws("#[1]"));
    check_true("stray_token",        parse_throws("@"));
    check_true("unterminated_struct_lit", parse_throws("Point { x: 1"));
}

// ---- multi-module loader (Slice 1b) -----------------------------------------
// The loader is filesystem-free: it takes a resolver `path segments -> source`. These
// tests drive it with an in-memory module map, so the DAG / cycle / topological-order
// logic is exercised without touching disk. static_vmrun's real FS resolver is Slice 6.

svc::ModuleResolver mem_resolver(const std::unordered_map<std::string, std::string>& mods) {
    return [mods](const std::vector<std::string>& path) -> std::optional<std::string> {
        auto it = mods.find(svc::join_module_path(path));
        if (it == mods.end()) return std::nullopt;
        return it->second;
    };
}

// Space-joined module names in load order ("<root>" = the entry), for order assertions.
std::string load_order(const char* entry, const std::unordered_map<std::string, std::string>& mods) {
    svc::ModuleSet set = svc::load_modules(entry, mem_resolver(mods));
    std::string s;
    for (size_t i = 0; i < set.modules.size(); ++i) {
        if (i) s += ' ';
        s += set.modules[i].name.empty() ? "<root>" : set.modules[i].name;
    }
    return s;
}

// True iff loading throws a LoadError (missing module / import cycle).
bool load_throws(const char* entry, const std::unordered_map<std::string, std::string>& mods) {
    try { svc::load_modules(entry, mem_resolver(mods)); return false; }
    catch (const svc::LoadError&) { return true; }
}

// The type errors of compiling a whole module SET (prelude-free) -- the `check_errs` sibling for
// anything that must distinguish the declaring module from the referring one, e.g. which module a
// diagnostic's secondary caret belongs to. Empty if the set compiles.
std::vector<svc::TypeError> modules_check_errs(const std::string& entry,
                                               const std::unordered_map<std::string, std::string>& mods) {
    try {
        svc::ModuleSet set = svc::load_modules(entry.c_str(), mem_resolver(mods));
        svc::compile_modules(std::move(set));
        return {};
    } catch (const svc::CheckFailure& e) {
        return e.errors();
    }
}

void test_loader() {
    std::cout << "[loader: modules]\n";
    using M = std::unordered_map<std::string, std::string>;

    // No imports -> just the root.
    check_str("load_root_only", load_order("let x = 1", M{}), "<root>");
    // Single import: the dependency loads BEFORE the root (topological, entry last).
    check_str("load_single_import",
              load_order("import util\nlet x = 1", M{{"util", "let y = 2"}}),
              "util <root>");
    // Chain root -> util -> math: deepest first, entry last.
    check_str("load_chain",
              load_order("import util", M{{"util", "import math"}, {"math", "let m = 0"}}),
              "math util <root>");
    // `use` also creates a dependency edge.
    check_str("load_use_edge",
              load_order("use util::helper", M{{"util", "fn helper() -> Int { 1 }"}}),
              "util <root>");
    // Diamond: root -> a, b ; a -> c ; b -> c. `c` loaded ONCE, before a and b.
    check_str("load_diamond",
              load_order("import a\nimport b",
                         M{{"a", "import c"}, {"b", "import c"}, {"c", "let z = 0"}}),
              "c a b <root>");
    // Subdir path segments: `import net::http` depends on module "net::http".
    check_str("load_path_segments",
              load_order("import net::http", M{{"net::http", "let ok = 1"}}),
              "net::http <root>");
    // A shared dependency appears exactly once in the order.
    check_str("load_shared_once",
              load_order("import a\nimport util",
                         M{{"a", "import util"}, {"util", "let u = 0"}}),
              "util a <root>");
    // Missing module -> LoadError.
    check_true("load_missing", load_throws("import nope", M{}));
    // Direct cycle a <-> b.
    check_true("load_cycle_2", load_throws("import a", M{{"a", "import b"}, {"b", "import a"}}));
    // Self-import cycle.
    check_true("load_self_cycle", load_throws("import a", M{{"a", "import a"}}));
    // A parse error inside an imported module is WRAPPED in a LoadError carrying the module + the
    // 1-based position (so a driver can caret it against that module's source).
    check_true("load_bad_module_wrapped",
               []{ try { svc::load_modules("import bad",
                             mem_resolver(M{{"bad", "fn f() { 1 }"}}));  // missing ret type
                         return false; }
                   catch (const svc::LoadError& e) { return e.module == "bad" && e.line > 0; }
                   catch (...) { return false; } }());
}

// ---- typechecker: solver (Stage 0) ------------------------------------------
// The TypeContext is AST-free, so it is unit-tested directly on hand-built Ty's:
// variable binding, the `Int <: Double` subtyping edge, join, rigid skolems,
// structural unification, Error poison, and rigid-var substitution.

void test_check_solver() {
    std::cout << "[check: solver]\n";
    svc::TypeContext tc;

    // A flexible var binds under unify and resolves via apply.
    auto v = tc.fresh_var("A");
    check_true("unify_var_int", tc.unify(v, svc::ty_int()));
    check_true("apply_var_int", tc.apply(v)->kind == svc::TyKind::Int);
    // The occurs-check rejects an infinite type.
    auto w = tc.fresh_var("B");
    check_true("occurs_check", !tc.unify(w, svc::make_tuple({ w })));

    // The one subtyping edge: Int <: Double, directional.
    check_true("sub_int_double",  tc.subsumes(svc::ty_int(), svc::ty_double()));
    check_true("nsub_double_int", !tc.subsumes(svc::ty_double(), svc::ty_int()));
    check_true("sub_same",        tc.subsumes(svc::ty_string(), svc::ty_string()));
    check_true("nsub_string_int", !tc.subsumes(svc::ty_string(), svc::ty_int()));

    // join is an EXACT merge (Never drops out); it does NOT widen {Int,Double}
    // (that is a check-boundary coercion, ratified N1/N3), so a mixed numeric
    // inference merge poisons to Error, as does any genuine incompatibility.
    check_true("join_int_double", tc.join(svc::ty_int(), svc::ty_double())->kind == svc::TyKind::Error);
    check_true("join_int_int",    tc.join(svc::ty_int(), svc::ty_int())->kind == svc::TyKind::Int);
    check_true("join_never",      tc.join(svc::ty_never(), svc::ty_string())->kind == svc::TyKind::String);
    check_true("join_incompat",   tc.join(svc::ty_string(), svc::ty_int())->kind == svc::TyKind::Error);

    // A rigid skolem unifies only with itself (soundness of local checking).
    svc::TypeContext tc2;
    auto r = tc2.rigid_var("T");
    check_true("rigid_self",   tc2.unify(r, r));
    check_true("rigid_no_int", !tc2.unify(r, svc::ty_int()));

    // Structural unification recurses into type args; the flexible arg is solved.
    auto listX = svc::make_named("List", { tc2.fresh_var("X") });
    auto listI = svc::make_named("List", { svc::ty_int() });
    check_true("unify_named",    tc2.unify(listX, listI));
    check_str ("named_arg_solved", svc::describe(tc2.apply(listX)), "List[Int]");
    // Mismatched heads / arities fail.
    check_true("named_head_ne",  !tc2.unify(svc::make_named("List", {}), svc::make_named("Vec", {})));

    // Error is consistent with everything (no cascade after a reported error).
    check_true("error_unifies", tc2.unify(svc::ty_error(), svc::ty_bool()));

    // Instantiating a scheme replaces rigid ids with fresh vars; substitute
    // realizes `fn(T) -> T` at `T := Int`.
    svc::TypeContext tc3;
    auto T = tc3.rigid_var("T");
    auto scheme = svc::make_fn({ T }, T);       // fn(T) -> T
    std::unordered_map<uint32_t, svc::TyPtr> m;
    m[T->var_id] = svc::ty_int();
    check_str("substitute_fn", svc::describe(tc3.substitute(scheme, m)), "fn(Int) -> Int");
    // instantiate makes the two Ts share one fresh var: unifying the param with
    // Double forces the result to Double too.
    auto inst = tc3.instantiate(scheme, { T->var_id });
    check_true("instantiate_unify", tc3.unify(inst->args[0], svc::ty_double()));
    check_str ("instantiate_ret",   svc::describe(tc3.apply(inst->ret)), "Double");
}

// ---- typechecker: signatures + coherence (Stage 1) --------------------------
// Programs are syntactically well-formed (they test SEMANTIC errors), so parsing
// succeeds and svc::check reports the declaration-level diagnostics.

int check_errc(const std::string& src) {
    svc::Lexer lex(src);
    svc::Parser p(lex.tokenize());
    auto prog = p.parse_program();
    return static_cast<int>(svc::check(prog).errors.size());
}
bool check_has(const std::string& src, const std::string& substr) {
    svc::Lexer lex(src);
    svc::Parser p(lex.tokenize());
    auto prog = p.parse_program();
    for (const auto& e : svc::check(prog).errors)
        if (e.message.find(substr) != std::string::npos) return true;
    return false;
}
// Advisory-tier helpers (must-use / unused): warnings live in a SEPARATE list, so they
// never affect the error count above.
int check_warnc(const std::string& src) {
    svc::Lexer lex(src);
    svc::Parser p(lex.tokenize());
    auto prog = p.parse_program();
    return static_cast<int>(svc::check(prog).warnings.size());
}
bool check_warn_has(const std::string& src, const std::string& substr) {
    svc::Lexer lex(src);
    svc::Parser p(lex.tokenize());
    auto prog = p.parse_program();
    for (const auto& w : svc::check(prog).warnings)
        if (w.message.find(substr) != std::string::npos) return true;
    return false;
}
// PRELUDE-LINKED warning count: `check_warnc` above sees only the snippet, so it can never notice a
// new warning firing inside the sealed std. That matters because a warning in std would break
// `--strict` for every user at once, in code they cannot edit.
int check_warnc_p(const std::string& src) {
    return static_cast<int>(svc::compile(src.c_str(), svc::builtin_prelude()).warnings.size());
}

void test_check_signatures() {
    std::cout << "[check: signatures]\n";

    // A clean multi-item program has no declaration errors.
    check_true("clean_program", check_errc(
        R"(struct Point { x: Int, y: Int }
           enum Option[T] { None, Some(T) }
           fn add(a: Int, b: Int) -> Int { a }
           trait Show { fn show(self) -> String }
           impl Show for Point { fn show(self) -> String { "p" } })") == 0);

    // A trait DEFAULT method satisfies an otherwise-missing impl method.
    check_true("default_satisfies", check_errc(
        R"(struct P { x: Int }
           trait T { fn m(self) -> Int { 0 } }
           impl T for P { })") == 0);

    // resolve_type errors.
    check_true("unknown_type",  check_has("fn f(x: Nope) -> Int { 0 }", "unknown type"));
    check_true("generic_arity", check_has(
        "struct Box[T] { v: T }\nfn f(x: Box) -> Int { 0 }", "type argument"));
    check_true("self_out_of_scope", check_has("fn f(x: Self) -> Int { 0 }", "'Self'"));
    check_true("dup_type_param", check_has("fn f[T, T](x: T) -> T { x }", "duplicate type parameter"));

    // A lambda's explicitly-annotated return type is reconciled with the EXPECTED return (covariant).
    // Regression for the closed soundness hole: a `fn(Int)->Int` must NOT satisfy `fn(Int)->String`.
    check_true("lambda_ret_mismatch",
        check_has("let f: fn(Int) -> String = fn(x: Int) -> Int { x * 10 }", "lambda returns"));
    check_true("lambda_ret_mismatch_arg",
        check_has("fn ap(f: fn(Int) -> String) -> String { f(1) }\n ap(fn(x: Int) -> Int { x })",
                  "lambda returns"));
    // ...but a covariant annotated return (Int where Double is expected) is still ACCEPTED (Int <: Double).
    check_true("lambda_ret_covariant",
        !check_has("let f: fn(Int) -> Double = fn(x: Int) -> Int { x }", "lambda returns"));

    // duplicate definitions across the type / constructor / fn / trait namespaces.
    check_true("dup_type", check_has(
        "struct A { x: Int }\nstruct A { y: Int }", "duplicate type"));
    check_true("redefine_builtin", check_has("struct Int { x: Int }", "builtin type"));
    check_true("dup_ctor", check_has(
        "struct Foo { x: Int }\nenum E { Foo }", "duplicate constructor"));
    check_true("dup_fn", check_has(
        "fn f() -> Int { 0 }\nfn f() -> Int { 1 }", "duplicate function"));

    // trait graph.
    check_true("unknown_supertrait", check_has(
        "trait Ord: Nope { fn cmp(self) -> Int }", "unknown trait"));
    check_true("cyclic_traits", check_has(
        "trait A: B { fn a(self) -> Int }\ntrait B: A { fn b(self) -> Int }", "cyclic trait"));

    // impl coherence.
    check_true("unknown_impl_trait", check_has(
        "struct P { x: Int }\nimpl Nope for P { }", "unknown trait"));
    check_true("dup_impl", check_has(
        R"(struct P { x: Int }
           trait T { fn m(self) -> Int }
           impl T for P { fn m(self) -> Int { 0 } }
           impl T for P { fn m(self) -> Int { 1 } })", "duplicate impl"));
    check_true("missing_method", check_has(
        R"(struct P { x: Int }
           trait T { fn m(self) -> Int }
           impl T for P { })", "missing method"));
    check_true("extra_method", check_has(
        R"(struct P { x: Int }
           trait T { fn m(self) -> Int }
           impl T for P { fn m(self) -> Int { 0 } fn extra(self) -> Int { 0 } })",
        "not a member of trait"));
    check_true("supertrait_closure", check_has(
        R"(struct P { x: Int }
           trait Eq { fn eq(self) -> Bool }
           trait Ord: Eq { fn cmp(self) -> Int }
           impl Ord for P { fn cmp(self) -> Int { 0 } })", "supertrait"));
    check_true("self_ness_mismatch", check_has(
        R"(struct P { x: Int }
           trait T { fn m(self) -> Int }
           impl T for P { fn m() -> Int { 0 } })", "self"));
    check_true("method_arity_mismatch", check_has(
        R"(struct P { x: Int }
           trait T { fn m(self, a: Int) -> Int }
           impl T for P { fn m(self) -> Int { 0 } })", "wrong number of parameters"));
}

// ---- typechecker: bidirectional body checker (Stage 2) ----------------------
// Positive inference is exercised through the return-type boundary: `fn f() -> T
// { EXPR }` checks clean iff EXPR's inferred type is acceptable where T is
// expected (so it doubles as an assertion that EXPR : <: T).

void test_check_bodies() {
    std::cout << "[check: bodies]\n";

    // --- an UPPERCASE name in value position must be REPORTED by the checker ---
    // It used to return an error type without recording a diagnostic, so the program passed the
    // checker and died in codegen as an unlocated `CodegenError: undefined variable`. A
    // CodegenError carries no module id, so in a multi-module program its caret landed on the
    // WRONG FILE. Each shape gets the message that actually helps:
    check_true("upper_unknown_reported",
        check_has("println(toString(NOSUCHNAME))", "unknown constant or constructor 'NOSUCHNAME'"));
    check_true("upper_record_struct_needs_fields",
        check_has("struct P { x: Int }\nlet p = P\n", "needs field initializers"));
    check_true("upper_zero_field_struct_needs_fields",   // reachable in codegen, so it must be typed too
        check_has("struct E { }\nlet e = E\n", "needs field initializers"));
    check_true("upper_enum_type_is_not_a_value",
        check_has("enum C { Red, Green }\nlet c = C\n", "is an enum type, not a value"));
    // ...and the valid neighbours stay valid: a nullary variant and a tuple struct ARE values.
    check_true("upper_nullary_variant_ok",
        check_errc("enum C { Red, Green }\nlet c = Red\n") == 0);
    check_true("upper_tuple_struct_ok",
        check_errc("struct W(Int)\nlet w = W(1)\n") == 0);

    // --- arithmetic + Int<:Double widening ---
    check_true("int_arith",    check_errc("fn f() -> Int { 1 + 2 * 3 }") == 0);
    check_true("int_widens",   check_errc("fn f() -> Double { 1 + 2 }") == 0);
    check_true("double_arith", check_errc("fn f() -> Double { 1.5 * 2.0 }") == 0);
    check_true("no_narrow",    check_has("fn f() -> Int { 1.5 }", "type mismatch"));
    check_true("str_not_num",  check_has("fn f(s: String) -> String { s - 1 }", "numeric"));
    check_true("str_concat",   check_errc("fn f(a: String, b: String) -> String { a + b }") == 0);
    // Java-style `+`: a String concatenates with a scalar (Int/Double/Bool) on EITHER side -> String.
    check_true("concat_str_int",   check_errc("fn f() -> String { \"n=\" + 42 }") == 0);
    check_true("concat_int_str",   check_errc("fn f() -> String { 42 + \"!\" }") == 0);
    check_true("concat_str_dbl",   check_errc("fn f() -> String { \"x=\" + 3.5 }") == 0);
    check_true("concat_str_bool",  check_errc("fn f() -> String { \"b=\" + true }") == 0);
    check_true("concat_bool_str",  check_errc("fn f() -> String { true + \"!\" }") == 0);
    // ...but heap operands still need an explicit toString(x), and Bool+Bool (no String) stays an error.
    check_true("concat_reject_struct",
        check_has("struct P { x: Int }\nfn f(p: P) -> String { \"p=\" + p }", "concatenat"));
    check_true("concat_reject_list",
        check_has("fn f(xs: List[Int]) -> String { \"xs=\" + xs }", "concatenat"));
    check_true("bool_plus_bool_still_err", check_has("fn f() -> Bool { true + false }", "numeric"));

    // --- bitwise / shift are Int-only; comparisons / equality / logic -> Bool ---
    check_true("bitwise_int",  check_errc("fn f(a: Int, b: Int) -> Int { a & b | 1 << 2 }") == 0);
    check_true("bitwise_dbl",  check_has("fn f(x: Double) -> Int { x & 1 }", "Int operands"));
    check_true("compare_bool", check_errc("fn f(a: Int, b: Int) -> Bool { a < b }") == 0);
    check_true("cmp_mixed",    check_has("fn f(s: String, n: Int) -> Bool { s < n }", "two numbers or two strings"));
    check_true("logic_bool",   check_errc("fn f(a: Bool, b: Bool) -> Bool { a && b || !a }") == 0);
    check_true("logic_nonbool",check_has("fn f(n: Int) -> Bool { n && true }", "type mismatch"));
    check_true("eq_ok",        check_errc("fn f(a: Int, b: Int) -> Bool { a == b }") == 0);
    check_true("eq_incompat",  check_has("fn f(s: String, n: Int) -> Bool { s == n }", "cannot compare"));

    // --- B1: an operator error on a generic type parameter appends a framing hint
    //         ("...no numeric capability...") instead of the confusing bare "found T and T" ---
    check_true("generic_arith_hint",
        check_has("fn f[T](a: T, b: T) -> T { a + b }", "no numeric capability"));
    check_true("generic_bitwise_hint",
        check_has("fn f[T](a: T, b: T) -> T { a & b }", "no numeric capability"));
    check_true("generic_unary_hint",
        check_has("fn f[T](a: T) -> T { -a }", "no numeric capability"));
    // The hint fires only for a generic param; a concrete non-numeric operand does NOT get it.
    check_true("concrete_no_hint",
        !check_has("fn f(a: Bool, b: Bool) -> Bool { a & b }", "no numeric capability"));

    // --- variables, let (typed + inferred + tuple destructure), assign/mut ---
    check_true("let_infer",    check_errc("fn f() -> Int { let x = 1 + 2\n x }") == 0);
    check_true("let_typed_ok", check_errc("fn f() -> Double { let x: Double = 3\n x }") == 0);
    check_true("let_typed_bad",check_has("fn f() -> Int { let x: Int = \"s\"\n x }", "type mismatch"));
    check_true("unknown_var",  check_has("fn f() -> Int { y }", "unknown variable"));
    check_true("tuple_destr",  check_errc("fn f(p: (Int, Bool)) -> Int { let (a, b) = p\n a }") == 0);
    check_true("mut_assign",   check_errc("fn f() -> Int { let mut x = 1\n x = 2\n x }") == 0);
    check_true("immut_assign", check_has("fn f() -> Int { let x = 1\n x = 2\n x }", "immutable"));
    // H5: field/index assignment requires the ROOT binding to be `mut` (ratified M5).
    check_true("field_assign_immut", check_has(
        "struct P { x: Int }\nfn f() -> Int { let p = P { x: 0 }\n p.x = 1\n 0 }", "immutable binding 'p'"));
    check_true("field_assign_mut_ok", check_errc(
        "struct P { x: Int }\nfn f() -> Int { let mut p = P { x: 0 }\n p.x = 1\n 0 }") == 0);
    check_true("index_assign_immut", check_has(
        "fn f(a: Array[Int]) -> Int { a[0] = 9\n 0 }", "immutable binding 'a'"));
    check_true("assign_temporary", check_has(
        "struct P { x: Int }\nfn mk() -> P { P { x: 0 } }\nfn g() -> Int { mk().x = 1\n 0 }", "temporary"));
    // M5 walks the WHOLE place-path to its root: a chained lvalue `v[0].x = ..` needs `v` mut.
    check_true("chain_assign_immut", check_has(
        "struct P { x: Int }\nfn f(v: Vec[P]) -> Int { v[0].x = 1\n 0 }", "immutable binding 'v'"));
    check_true("chain_assign_mut_ok", check_errc(
        "struct P { x: Int }\nfn f(mut v: Vec[P]) -> Int { v[0].x = 1\n 0 }") == 0);
    check_true("chain_assign_temporary", check_has(   // root is a temporary (a call), not a binding
        "struct P { x: Int }\nfn mk() -> Vec[P] { vec() }\nfn g() -> Int { mk()[0].x = 1\n 0 }", "temporary"));

    // H6: an integer literal must fit the VM's 48-bit Int [-2^47, 2^47 - 1].
    check_true("int48_max_ok",   check_errc("fn f() -> Int { 140737488355327 }") == 0);      // 2^47 - 1
    check_true("int48_over",     check_has("fn f() -> Int { 140737488355328 }", "out of range")); // 2^47
    check_true("int48_min_ok",   check_errc("fn f() -> Int { -140737488355328 }") == 0);     // -2^47 = MIN_48
    check_true("int48_under",    check_has("fn f() -> Int { -140737488355329 }", "out of range")); // < MIN_48

    // --- if / while / return / break / continue ---
    check_true("if_join",      check_errc("fn f(c: Bool) -> Int { if c { 1 } else { 2 } }") == 0);
    check_true("if_nonbool",   check_has("fn f(n: Int) -> Int { if n { 1 } else { 2 } }", "type mismatch"));
    check_true("if_branch_ty", check_has("fn f(c: Bool) -> Int { if c { 1 } else { \"x\" } }", "type mismatch"));
    check_true("if_branch_join", check_has("fn f(c: Bool) -> Int { let x = if c { 1 } else { \"x\" }\n 0 }", "incompatible"));
    check_true("while_unit",   check_errc("fn f(c: Bool) -> () { while c { 1 } }") == 0);
    check_true("return_ok",    check_errc("fn f(c: Bool) -> Int { if c { return 1 }\n 2 }") == 0);
    check_true("return_bad",   check_has("fn f() -> Int { return \"s\" }", "type mismatch"));
    check_true("break_outside",check_has("fn f() -> Int { break }", "'break' outside"));

    // --- direct calls (arity + arg types + result) ---
    check_true("call_ok", check_errc(
        "fn add(a: Int, b: Int) -> Int { a + b }\nfn g() -> Int { add(1, 2) }") == 0);
    check_true("call_arity", check_has(
        "fn add(a: Int, b: Int) -> Int { a + b }\nfn g() -> Int { add(1) }", "wrong number of arguments"));
    check_true("call_argty", check_has(
        "fn add(a: Int, b: Int) -> Int { a + b }\nfn g() -> Int { add(1, \"x\") }", "type mismatch"));
    check_true("call_noncallable", check_has("fn g(x: Int) -> Int { x(1) }", "cannot call"));
    check_true("pipe_call", check_errc(
        "fn inc(n: Int) -> Int { n + 1 }\nfn g() -> Int { 5 |> inc }") == 0);

    // --- field access / index / struct literal / tuple ---
    check_true("field_ok", check_errc(
        "struct Point { x: Int, y: Int }\nfn g(p: Point) -> Int { p.x }") == 0);
    check_true("field_bad", check_has(
        "struct Point { x: Int, y: Int }\nfn g(p: Point) -> Int { p.z }", "no field"));
    check_true("struct_lit_ok", check_errc(
        "struct Point { x: Int, y: Int }\nfn g() -> Point { Point { x: 1, y: 2 } }") == 0);
    check_true("struct_lit_missing", check_has(
        "struct Point { x: Int, y: Int }\nfn g() -> Point { Point { x: 1 } }", "missing field"));
    check_true("struct_lit_badty", check_has(
        "struct Point { x: Int, y: Int }\nfn g() -> Point { Point { x: 1, y: \"s\" } }", "type mismatch"));
    // Record update `Name { …, ..base }`: base supplies the unlisted fields -> no missing-field error.
    check_true("record_update_ok", check_errc(
        "struct Point { x: Int, y: Int }\nfn g(p: Point) -> Point { Point { x: 5, ..p } }") == 0);
    check_true("record_update_copy_ok", check_errc(
        "struct Point { x: Int, y: Int }\nfn g(p: Point) -> Point { Point { ..p } }") == 0);
    check_true("record_update_wrong_base", check_has(
        "struct A { x: Int }\nstruct B { y: Int }\nfn g(b: B) -> A { A { ..b } }", "type mismatch"));
    check_true("record_update_unknown_field", check_has(
        "struct Point { x: Int, y: Int }\nfn g(p: Point) -> Point { Point { z: 1, ..p } }", "no field"));
    check_true("record_update_overridden_ty", check_has(
        "struct Point { x: Int, y: Int }\nfn g(p: Point) -> Point { Point { x: \"s\", ..p } }", "type mismatch"));
    // A generic struct: the base SOLVES the type argument the explicit fields don't pin.
    check_true("record_update_generic", check_errc(
        "struct Box[T] { v: T }\nfn g(b: Box[Int]) -> Box[Int] { Box { ..b } }") == 0);
    check_true("tuple_index", check_errc("fn g(t: (Int, Bool)) -> Bool { t.1 }") == 0);
    check_true("index_array", check_errc("fn g(a: Array[Int]) -> Int { a[0] }") == 0);
    check_true("index_nonint", check_has("fn g(a: Array[Int]) -> Int { a[\"k\"] }", "type mismatch"));
    // A DOUBLE index/size is the subtle case (Int<:Double coercion is one-way): must still be rejected,
    // so a Double never reaches the guardless VM asSigned48() index/size path.
    check_true("index_double",      check_has("fn g(a: Array[Int]) -> Int { a[2.0] }", "expected Int"));
    check_true("size_double_array", check_has("fn g() -> Int { let a = array(2.0, 0)\n len(a) }", "expected Int"));
    check_true("size_double_bytes", check_has("fn g() -> Int { let b = bytes(2.0)\n len(b) }", "expected Int"));
    check_true("nullary_variant", check_errc(
        "enum Color { Red, Green }\nfn g() -> Color { Red }") == 0);
}

// ---- typechecker: match + patterns + exhaustiveness (Stage 3) ---------------

void test_check_match() {
    std::cout << "[check: match]\n";

    const char* OPT = "enum Option[T] { None, Some(T) }\n";

    // --- pattern typing + binding ---
    check_true("variant_bind", check_errc(
        std::string(OPT) + "fn f(o: Option[Int]) -> Int { match o { None => 0, Some(x) => x } }") == 0);
    check_true("variant_bind_bad", check_has(
        std::string(OPT) + "fn f(o: Option[Int]) -> String { match o { None => \"n\", Some(x) => x } }",
        "type mismatch"));
    check_true("match_arm_join", check_has(
        std::string(OPT) + "fn f(o: Option[Int]) -> Int { let r = match o { None => 0, Some(x) => \"s\" }\n r }",
        "incompatible"));
    check_true("ctor_arity", check_has(
        std::string(OPT) + "fn f(o: Option[Int]) -> Int { match o { None => 0, Some(x, y) => x } }",
        "expects 1 field"));
    check_true("unknown_ctor", check_has(
        std::string(OPT) + "fn f(o: Option[Int]) -> Int { match o { Nope => 0, _ => 1 } }",
        "unknown constructor"));
    // --- a refutable pattern in a BINDER position names the construct it is in ---
    // `let` and `for` share bind_pattern, which used to hard-code "'let'" in all three of its
    // rejection messages -- so a bad `for` pattern advised the user about a construct they had not
    // written. Both directions are pinned, since the fix is a defaulted parameter and a missed call
    // site silently reverts to "let".
    check_true("refutable_in_let_says_let", check_has(
        std::string(OPT) + "fn f(o: Option[Int]) -> Int { let Some(x) = o\n x }",
        "not allowed in 'let'"));
    check_true("refutable_in_for_says_for", check_has(
        std::string(OPT) + "fn f(v: Vec[Option[Int]]) -> Int { for Some(x) in v { }\n 0 }",
        "not allowed in 'for'"));
    check_true("refutable_in_for_not_let", !check_has(
        std::string(OPT) + "fn f(v: Vec[Option[Int]]) -> Int { for Some(x) in v { }\n 0 }",
        "not allowed in 'let'"));
    // Nested, to prove the context is threaded through the recursion and not just set at the top.
    check_true("refutable_nested_for_says_for", check_has(
        std::string(OPT) + "fn f(v: Vec[(Int, Option[Int])]) -> Int { for (a, Some(x)) in v { }\n 0 }",
        "not allowed in 'for'"));

    check_true("wrong_scrut", check_has(
        std::string(OPT) + "fn f(n: Int) -> Int { match n { Some(x) => x, _ => 0 } }",
        "does not match"));
    check_true("literal_pat", check_errc(
        "fn f(n: Int) -> Int { match n { 0 => 10, _ => 20 } }") == 0);
    check_true("literal_pat_bad", check_has(
        "fn f(n: Int) -> Int { match n { \"s\" => 1, _ => 0 } }", "does not match"));
    check_true("struct_pat", check_errc(
        "struct Point { x: Int, y: Int }\nfn f(p: Point) -> Int { match p { Point { x, y } => x + y } }") == 0);
    check_true("tuple_pat", check_errc(
        "fn f(t: (Int, Bool)) -> Int { match t { (a, b) => a } }") == 0);
    check_true("nested_pat", check_errc(
        std::string(OPT) + "fn f(o: Option[(Int, Int)]) -> Int { match o { None => 0, Some((a, b)) => a + b } }") == 0);
    check_true("guard_typed", check_errc(
        "fn f(n: Int) -> Int { match n { x if x > 0 => 1, _ => 0 } }") == 0);
    check_true("guard_nonbool", check_has(
        "fn f(n: Int) -> Int { match n { x if x => 1, _ => 0 } }", "type mismatch"));

    // --- exhaustiveness ---
    check_true("enum_exhaustive", check_errc(
        std::string(OPT) + "fn f(o: Option[Int]) -> Int { match o { None => 0, Some(x) => x } }") == 0);
    check_true("enum_nonexhaustive", check_has(
        std::string(OPT) + "fn f(o: Option[Int]) -> Int { match o { Some(x) => x } }", "non-exhaustive"));
    check_true("bool_exhaustive", check_errc(
        "fn f(b: Bool) -> Int { match b { true => 1, false => 0 } }") == 0);
    check_true("bool_nonexhaustive", check_has(
        "fn f(b: Bool) -> Int { match b { true => 1 } }", "non-exhaustive"));
    check_true("wildcard_exhaustive", check_errc(
        "fn f(n: Int) -> Int { match n { 0 => 1, _ => 2 } }") == 0);
    check_true("int_needs_wildcard", check_has(
        "fn f(n: Int) -> Int { match n { 0 => 1, 1 => 2 } }", "non-exhaustive"));
    check_true("list_exhaustive", check_errc(
        "fn f(xs: List[Int]) -> Int { match xs { [] => 0, [h, ..t] => h } }") == 0);
    check_true("list_nonexhaustive", check_has(
        "fn f(xs: List[Int]) -> Int { match xs { [h, ..t] => h } }", "non-exhaustive"));
    // A list pattern matches a cons `List` only. The checker used to accept it on a Vec / Array too, where
    // codegen's cons-cell test can never succeed: `match toVec([1, 5]) { [1, d] => d, _ => 0 }` returned 0
    // with no diagnostic (the RefEval oracle matched positionally and would have said 5), and a second
    // same-length arm was reported "unreachable" because exhaustiveness modelled the Vec as a cons list.
    check_true("list_pattern_on_vec_rejected", check_has(
        "fn f(v: Vec[Int]) -> Int { match v { [1, d] => d, _ => 0 } }", "a list pattern matches a List"));
    check_true("list_pattern_on_array_rejected", check_has(
        "fn f(v: Array[Int]) -> Int { match v { [] => 0, _ => 1 } }", "a list pattern matches a List"));
    check_true("list_pattern_on_vec_one_error", check_errc(
        "fn f(v: Vec[String]) -> Int { match v { [\"go\", d] => len(d), [\"take\", i] => 2, _ => 0 } }") == 2);
    check_true("list_pattern_on_vec_names_fix", check_has(
        "fn f(v: Vec[Int]) -> Int { match v { [a, ..r] => a, _ => 0 } }", "index the Vec"));

    // --- usefulness (unreachable arms) ---
    check_true("redundant_wildcard", check_has(
        std::string(OPT) + "fn f(o: Option[Int]) -> Int { match o { _ => 0, None => 1 } }",
        "unreachable"));
    check_true("redundant_literal", check_has(
        "fn f(n: Int) -> Int { match n { 1 => 1, 1 => 2, _ => 0 } }", "unreachable"));
    check_true("guard_not_redundant", check_errc(
        "fn f(n: Int) -> Int { match n { x if x > 0 => 1, x => 0 } }") == 0);

    // --- record-style enum variants `V { f: T }` (construction + pattern + exhaustiveness) ---
    const char* SH = "enum Shape { Dot, Circle { r: Int }, Rect { w: Int, h: Int } }\n";
    check_true("rvariant_construct", check_errc(
        std::string(SH) + "fn f() -> Shape { Circle { r: 2 } }") == 0);
    check_true("rvariant_match", check_errc(
        std::string(SH) + "fn f(s: Shape) -> Int { match s { Dot => 0, Circle { r } => r, Rect { w, h } => w * h } }") == 0);
    check_true("rvariant_match_rename", check_errc(
        std::string(SH) + "fn f(s: Shape) -> Int { match s { Dot => 0, Circle { r: n } => n, Rect { w, h } => w + h } }") == 0);
    // A record variant completes the enum signature -> exhaustive WITHOUT a wildcard.
    check_true("rvariant_exhaustive", check_errc(
        std::string(SH) + "fn f(s: Shape) -> Int { match s { Dot => 0, Circle { r } => r, Rect { w, h } => w } }") == 0);
    check_true("rvariant_nonexhaustive", check_has(
        std::string(SH) + "fn f(s: Shape) -> Int { match s { Dot => 0, Circle { r } => r } }", "non-exhaustive"));
    // Construction errors.
    check_true("rvariant_missing_field", check_has(
        std::string(SH) + "fn f() -> Shape { Rect { w: 2 } }", "missing field"));
    check_true("rvariant_unknown_field", check_has(
        std::string(SH) + "fn f() -> Shape { Circle { r: 1, z: 2 } }", "has no field"));
    check_true("rvariant_field_type", check_has(
        std::string(SH) + "fn f() -> Shape { Circle { r: \"s\" } }", "type mismatch"));
    // Brace syntax on a tuple/nullary variant is rejected; `..base` on a variant is rejected.
    check_true("rvariant_brace_on_tuple", check_has(
        "enum E { V(Int) }\nfn f() -> E { V { x: 1 } }", "not a record variant"));
    check_true("rvariant_no_base", check_has(
        std::string(SH) + "fn f(c: Shape) -> Shape { Circle { r: 3, ..c } }", "record update"));
    // Generic record variant: T solved from the field value.
    check_true("rvariant_generic", check_errc(
        "enum Tree[T] { Leaf { val: T }, Node(Tree[T], Tree[T]) }\n"
        "fn f() -> Tree[Int] { Leaf { val: 42 } }") == 0);
}

// ---- typechecker: generics, bounds, lambdas, ctor calls (Stage 4) -----------

void test_check_generics() {
    std::cout << "[check: generics]\n";

    const char* OPT = "enum Option[T] { None, Some(T) }\n";

    // --- generic direct calls (instantiate + unify + result) ---
    check_true("id_int", check_errc("fn id[T](x: T) -> T { x }\nfn g() -> Int { id(5) }") == 0);
    check_true("id_str", check_errc("fn id[T](x: T) -> T { x }\nfn g() -> String { id(\"s\") }") == 0);
    check_true("const_fn", check_errc(
        "fn fst[A, B](a: A, b: B) -> A { a }\nfn g() -> Int { fst(1, \"x\") }") == 0);
    check_true("id_result_flows", check_has(
        "fn id[T](x: T) -> T { x }\nfn g() -> String { id(5) }", "type mismatch"));

    // --- constructor-as-function calls ---
    check_true("some_call", check_errc(std::string(OPT) + "fn g() -> Option[Int] { Some(5) }") == 0);
    check_true("some_call_bad", check_has(std::string(OPT) + "fn g() -> Option[Int] { Some(\"s\") }", "type mismatch"));
    check_true("tuple_struct_call", check_errc(
        "struct Pair(Int, Int)\nfn g() -> Pair { Pair(1, 2) }") == 0);
    check_true("ctor_call_arity", check_has(std::string(OPT) + "fn g() -> Option[Int] { Some(1, 2) }", "wrong number"));

    // --- trait bounds discharged at the call site ---
    const char* DISP =
        "trait Display { fn show(self) -> String }\n"
        "impl Display for Int { fn show(self) -> String { \"i\" } }\n"
        "fn p[T: Display](x: T) -> String { \"ok\" }\n";
    check_true("bound_ok",  check_errc(std::string(DISP) + "fn g() -> String { p(1) }") == 0);
    check_true("bound_bad", check_has(std::string(DISP) + "fn g() -> String { p(true) }", "does not satisfy the bound"));
    check_true("bound_forward", check_errc(   // a bounded param forwards to a bounded call
        std::string(DISP) + "fn q[U: Display](y: U) -> String { p(y) }") == 0);
    // H0 soundness: an UNBOUNDED generic forwarded to a Display-bound call cannot be
    // proven to satisfy the bound -> reject (the old leniency accepted this silently).
    check_true("bound_forward_unbounded", check_has(
        std::string(DISP) + "fn q[U](y: U) -> String { p(y) }", "does not satisfy the bound"));

    // H1: an enum/struct generic bound is discharged at CONSTRUCTION (a ctor call and
    // a struct literal), not just at a direct fn call.
    const char* BND =
        "trait Display { fn show(self) -> String }\n"
        "impl Display for Int { fn show(self) -> String { \"i\" } }\n"
        "enum Box[T: Display] { Wrap(T) }\n"
        "struct Holder[T: Display] { item: T }\n";
    check_true("ctor_bound_ok",  check_errc(std::string(BND) + "fn g() -> Box[Int] { Wrap(1) }") == 0);
    check_true("ctor_bound_bad", check_has(
        std::string(BND) + "fn g() -> Box[Bool] { Wrap(true) }", "does not satisfy the bound"));
    check_true("lit_bound_ok",   check_errc(
        std::string(BND) + "fn g() -> Holder[Int] { Holder { item: 1 } }") == 0);
    check_true("lit_bound_bad",  check_has(
        std::string(BND) + "fn g() -> Holder[Bool] { Holder { item: true } }", "does not satisfy the bound"));

    // H2: a BOUNDED generic fn used as a value in an INFER position (un-annotated `let`) is
    // rejected -- the bare `fn` type has no slot for the bound. But at a CONCRETE `fn(...)->R`
    // expected type it is now monomorphized + discharged (see test_check_bounded_fn_values), so
    // `ap(p, 1)` -- p flowing into a concrete `fn(Int)->String` param, Int: Display -- is CLEAN.
    check_true("bounded_fn_value_let", check_has(
        std::string(DISP) + "fn g() -> String { let f = p\n f(1) }", "first-class value"));
    check_true("bounded_fn_value_arg_ok", check_errc(
        std::string(DISP) +
        "fn ap(f: fn(Int) -> String, x: Int) -> String { f(x) }\nfn g() -> String { ap(p, 1) }") == 0);
    check_true("unbounded_fn_value_ok", check_errc(
        "fn id[T](x: T) -> T { x }\nfn g() -> Int { let f = id\n f(5) }") == 0);

    // H3: a trait METHOD's OWN generic bounds are discharged at the call site.
    const char* MB =
        "trait Display { fn show(self) -> String }\n"
        "impl Display for Int { fn show(self) -> String { \"i\" } }\n"
        "trait Sink { fn take[U: Display](self, u: U) -> String }\n"
        "impl Sink for Int { fn take[U: Display](self, u: U) -> String { \"t\" } }\n";
    check_true("method_bound_ok",  check_errc(std::string(MB) + "fn g() -> String { take(0, 1) }") == 0);
    check_true("method_bound_bad", check_has(
        std::string(MB) + "fn g() -> String { take(0, true) }", "does not satisfy the bound"));

    // --- lambdas in check position ---
    check_true("lambda_arg", check_errc(
        "fn ap(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\nfn g() -> Int { ap(fn(n) { n + 1 }, 5) }") == 0);
    check_true("lambda_body_bad", check_has(
        "fn ap(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\nfn g() -> Int { ap(fn(n) { \"s\" }, 5) }",
        "type mismatch"));
    check_true("lambda_unannotated_infer_fails", check_has(
        "fn g() -> Int { let f = fn(n) { n + 1 }\n 0 }", "cannot infer"));
    check_true("hof_generic_lambda", check_errc(
        std::string(OPT) +
        "fn mapo[A, B](o: Option[A], f: fn(A) -> B) -> Option[B] { match o { None => None, Some(x) => Some(f(x)) } }\n"
        "fn g() -> Option[Int] { mapo(Some(1), fn(n) { n + 1 }) }") == 0);

    // --- empty / directed container literals in check position ---
    check_true("empty_list_ok",  check_errc("fn g() -> List[Int] { [] }") == 0);
    check_true("list_elems_ok",  check_errc("fn g() -> List[Int] { [1, 2, 3] }") == 0);
    check_true("empty_list_ctx_bad", check_has("fn g() -> Int { [] }", "cannot infer"));
    check_true("empty_map_ok",   check_errc("fn g() -> Map[String, Int] { #{} }") == 0);
    check_true("list_if_branch", check_errc("fn g(c: Bool) -> List[Int] { if c { [] } else { [1] } }") == 0);
}

// ---- typechecker: traits at use sites + ? + method bodies (Stage 5) ---------

void test_check_traits_use() {
    std::cout << "[check: traits/?]\n";

    const char* SHOW =
        "trait Show { fn show(self) -> String }\n"
        "impl Show for Int { fn show(self) -> String { \"i\" } }\n"
        "struct Point { x: Int, y: Int }\n"
        "impl Show for Point { fn show(self) -> String { \"p\" } }\n";

    // --- trait method call / pipe / qualified ---
    check_true("method_call", check_errc(std::string(SHOW) + "fn g(p: Point) -> String { show(p) }") == 0);
    check_true("method_pipe", check_errc(std::string(SHOW) + "fn g() -> String { 5 |> show }") == 0);
    check_true("method_no_impl", check_has(
        std::string(SHOW) + "fn g(b: Bool) -> String { show(b) }", "does not implement trait 'Show'"));
    check_true("method_qualified", check_errc(std::string(SHOW) + "fn g() -> String { Show::show(5) }") == 0);
    check_true("method_result_ty", check_has(
        std::string(SHOW) + "fn g(p: Point) -> Int { show(p) }", "type mismatch"));

    // impl body typing: `self` is the target; can call trait methods on self
    check_true("impl_self_field", check_errc(
        "trait Area { fn area(self) -> Int }\n"
        "struct Rect { w: Int, h: Int }\n"
        "impl Area for Rect { fn area(self) -> Int { self.w * self.h } }") == 0);
    check_true("impl_body_bad", check_has(
        "trait Area { fn area(self) -> Int }\n"
        "struct Rect { w: Int, h: Int }\n"
        "impl Area for Rect { fn area(self) -> Int { self.w - \"x\" } }", "numeric"));  // `-` stays numeric-only

    // impl signature must MATCH the trait (soundness: a caller resolves via the
    // trait signature, so the impl cannot return / accept a different type)
    check_true("impl_ret_conformance", check_has(
        "trait Show { fn show(self) -> String }\nimpl Show for Int { fn show(self) -> Int { 0 } }",
        "requires String"));
    check_true("impl_param_conformance", check_has(
        "trait Eq { fn eq(self, other: Self) -> Bool }\nstruct P { x: Int }\n"
        "impl Eq for P { fn eq(self, other: Int) -> Bool { true } }", "requires P"));
    check_true("impl_conformance_ok", check_errc(
        "trait Eq { fn eq(self, other: Self) -> Bool }\nstruct P { x: Int }\n"
        "impl Eq for P { fn eq(self, other: P) -> Bool { true } }") == 0);

    // H4: conformance now also checks methods with their OWN generics (was skipped).
    check_true("mgen_ret_conformance", check_has(   // impl returns U, trait promises List[U]
        "trait Wrap { fn wrap[U](self, u: U) -> List[U] }\n"
        "impl Wrap for Int { fn wrap[U](self, u: U) -> U { u } }", "requires List"));
    check_true("mgen_param_conformance", check_has(  // impl param Int, trait param U
        "trait Foo { fn f[U](self, u: U) -> Int }\n"
        "impl Foo for Int { fn f[U](self, u: Int) -> Int { 0 } }", "requires U"));
    check_true("mgen_arity_conformance", check_has(  // impl has 2 type params, trait 1
        "trait Ar { fn g[U](self) -> Int }\n"
        "impl Ar for Int { fn g[U, V](self) -> Int { 0 } }", "type parameter"));
    check_true("mgen_extra_bound", check_has(        // impl assumes Ord the trait never guarantees
        "trait Display { fn show(self) -> String }\ntrait Ord { fn cmp(self, o: Self) -> Int }\n"
        "impl Display for Int { fn show(self) -> String { \"i\" } }\n"
        "trait Sink { fn take[U: Display](self, u: U) -> String }\n"
        "impl Sink for Int { fn take[U: Display + Ord](self, u: U) -> String { \"t\" } }",
        "does not require"));
    check_true("mgen_conformance_ok", check_errc(    // a matching generic method conforms
        "trait Wrap { fn wrap[U](self, u: U) -> U }\n"
        "impl Wrap for Int { fn wrap[U](self, u: U) -> U { u } }") == 0);

    // ambiguous unqualified method (declared by two traits)
    check_true("ambiguous_method", check_has(
        "trait A { fn m(self) -> Int }\ntrait B { fn m(self) -> Int }\n"
        "struct P { x: Int }\nimpl A for P { fn m(self) -> Int { 1 } }\nimpl B for P { fn m(self) -> Int { 2 } }\n"
        "fn g(p: P) -> Int { m(p) }", "ambiguous"));

    // --- generic body dispatching on a bounded type var ---
    check_true("bounded_body", check_errc(
        "trait Show { fn show(self) -> String }\n"
        "fn describe[T: Show](x: T) -> String { show(x) }") == 0);
    check_true("unbounded_body_bad", check_has(
        "trait Show { fn show(self) -> String }\n"
        "fn describe[T](x: T) -> String { show(x) }", "does not implement trait 'Show'"));

    // --- default method body checked; sub-trait dispatch on self ---
    check_true("default_body", check_errc(
        "trait Greet { fn name(self) -> String\n fn hello(self) -> String { name(self) } }") == 0);

    // --- the ? operator ---
    const char* RES = "enum Result[T, E] { Ok(T), Err(E) }\nenum Option[T] { None, Some(T) }\n";
    check_true("try_result_ok", check_errc(std::string(RES) +
        "fn h() -> Result[Int, String] { Err(\"boom\") }\n"
        "fn g() -> Result[Int, String] { let x = h()?\n Ok(x) }") == 0);
    check_true("try_option_ok", check_errc(std::string(RES) +
        "fn h() -> Option[Int] { None }\n"
        "fn g() -> Option[Int] { let x = h()?\n Some(x) }") == 0);
    check_true("try_wrong_ctx", check_has(std::string(RES) +
        "fn h() -> Result[Int, String] { Ok(1) }\n"
        "fn g() -> Int { let x = h()?\n x }", "requires the enclosing function to return a Result"));
    check_true("try_err_mismatch", check_has(std::string(RES) +
        "fn h() -> Result[Int, String] { Ok(1) }\n"
        "fn g() -> Result[Int, Int] { let x = h()?\n Ok(x) }", "error type"));
    check_true("try_non_result", check_has(std::string(RES) +
        "fn g() -> Result[Int, String] { let x = 5?\n Ok(x) }", "can only be applied to a Result or Option"));
    check_true("try_payload_flows", check_has(std::string(RES) +
        "fn h() -> Result[Int, String] { Ok(1) }\n"
        "fn g() -> Result[String, String] { let x = h()?\n Ok(x) }", "type mismatch"));
}

// ---- typechecker: for-loops, corpus, collect-all diagnostics (Stage 6) ------

std::vector<svc::TypeError> check_errs(const std::string& src) {
    svc::Lexer lex(src);
    svc::Parser p(lex.tokenize());
    auto prog = p.parse_program();
    return svc::check(prog).errors;
}
bool errs_sorted(const std::vector<svc::TypeError>& es) {
    for (size_t i = 1; i < es.size(); ++i)
        if (es[i].line < es[i - 1].line ||
            (es[i].line == es[i - 1].line && es[i].col < es[i - 1].col))
            return false;
    return true;
}

// A realistic multi-feature program that must check clean.
static const char* CORPUS = R"(
enum Option[T] { None, Some(T) }
enum Result[T, E] { Ok(T), Err(E) }
enum Color { Red, Green, Blue }

struct Point { x: Int, y: Int }
struct Pair(Int, Int)

trait Show { fn show(self) -> String }
impl Show for Int { fn show(self) -> String { "int" } }
impl Show for Point { fn show(self) -> String { "point" } }

fn origin() -> Point { Point { x: 0, y: 0 } }
fn manhattan(p: Point) -> Int { p.x + p.y }

fn id[T](x: T) -> T { x }
fn describe[T: Show](x: T) -> String { show(x) }

fn first[T](o: Option[T]) -> Result[T, String] {
    match o {
        None => Err("empty"),
        Some(v) => Ok(v)
    }
}

fn use_try() -> Result[Int, String] {
    let p = origin()
    let v = first(Some(manhattan(p)))?
    Ok(v)
}

fn classify(c: Color) -> String {
    match c { Red => "r", Green => "g", Blue => "b" }
}

fn apply_twice(f: fn(Int) -> Int, x: Int) -> Int { f(f(x)) }
fn compute() -> Int {
    let inc = fn(n: Int) -> Int { n + 1 }
    apply_twice(inc, 10)
}

fn sum_list(xs: List[Int]) -> Int {
    let mut total = 0
    for x in xs { total = total + x }
    total
}

fn count_map(m: Map[String, Int]) -> Int {
    let mut n = 0
    for (k, v) in m { n = n + v }
    n
}

let base = origin()
let d = manhattan(base)
)";

void test_check_corpus() {
    std::cout << "[check: corpus]\n";

    // --- for-loops ---
    check_true("for_list",  check_errc("fn f(xs: List[Int]) -> Int { let mut s = 0\n for x in xs { s = s + x }\n s }") == 0);
    check_true("for_map",   check_errc("fn f(m: Map[String, Int]) -> Int { let mut s = 0\n for (k, v) in m { s = s + v }\n s }") == 0);
    check_true("for_break", check_errc("fn f(xs: List[Int]) -> Int { for x in xs { break }\n 0 }") == 0);
    check_true("for_noniter", check_has("fn f(n: Int) -> Int { for x in n { }\n 0 }", "cannot iterate"));

    // --- the whole corpus checks clean ---
    check_true("corpus_clean", check_errc(CORPUS) == 0);

    // --- ratified N3: a mixed numeric container literal is a type error (no
    // inference-position widening; widening is a check-BOUNDARY coercion only) ---
    check_true("mixed_num_list", check_has("fn f() -> Int { let xs = [1, 2.0]\n 0 }", "incompatible"));
    check_true("widen_boundary_ok", check_errc("fn f() -> List[Double] { [1.0, 2.0] }") == 0);
    check_true("scalar_widen_ok",  check_errc("fn f() -> Double { 3 }") == 0);

    // --- collect-all: multiple distinct errors, reported in source order ---
    auto es = check_errs(
        "struct Point { x: Int, y: Int }\n"
        "fn bad(p: Point) -> Int {\n"
        "    let a = \"s\" - 1\n"       // String - Int: still a numeric-op error (`+` now concatenates)
        "    let b = p.z\n"
        "    zzz\n"
        "}\n");
    check_true("multi_error_count",  es.size() == 3);
    check_true("multi_error_sorted", errs_sorted(es));
}

// ---- structured diagnostics: per-message a/b/c naming + dual expected/found carets ----
// The a/b/c variable naming is exercised as a unit test in test_typed_ast (TypeRenderer);
// here we assert the SECONDARY-LABEL (dual-caret) plumbing on real programs: the right sites
// attach a note, and -- crucially -- the wrong sites (call args) do NOT (origin is explicit,
// never ambient).
void test_structured_diagnostics() {
    std::cout << "[check: structured diagnostics]\n";

    // (1) `let x: T = v` mismatch -> a note at the type annotation, left of the value caret.
    {
        auto es = check_errs("fn f() -> Int { let x: String = 42\n 0 }");
        check_true("let_ann_has_label", es.size() >= 1 && es[0].labels.size() == 1);
        if (!es.empty() && es[0].labels.size() == 1) {
            check_true("let_ann_label_text",
                       es[0].labels[0].text.find("annotation") != std::string::npos);
            check_true("let_ann_label_before_value",
                       es[0].labels[0].line == es[0].line && es[0].labels[0].col < es[0].col);
        }
    }

    // (2) declared return-type mismatch -> a note citing the `-> T`.
    {
        auto es = check_errs("fn f() -> String { 42 }");
        check_true("ret_type_has_label", es.size() >= 1 && es[0].labels.size() == 1);
        if (!es.empty() && es[0].labels.size() == 1)
            check_true("ret_type_label_text",
                       es[0].labels[0].text.find("return type") != std::string::npos);
    }

    // (3) incompatible if-branches -> a note on the first branch.
    {
        auto es = check_errs("fn f(c: Bool) -> Int { let x = if c { 1 } else { \"s\" }\n 0 }");
        check_true("if_branch_has_label", es.size() >= 1 && es[0].labels.size() == 1);
    }

    // (4) a plain operator mismatch carries NO secondary label (single caret unchanged).
    {
        auto es = check_errs("fn f() -> Int { \"s\" - 1 }");
        check_true("op_mismatch_no_label", es.size() >= 1 && es[0].labels.empty());
    }

    // (5) a call-ARGUMENT mismatch notes the PARAMETER that demanded the type -- and specifically not
    //     the enclosing `let` annotation, which is the point of passing origin explicitly rather than
    //     letting it be ambient. The note must therefore sit on line 1 (the declaration), not line 2.
    {
        auto es = check_errs("fn g(n: Int) -> Int { n }\n"
                             "fn f() -> Int { let x: Int = g(true)\n x }");
        check_true("arg_mismatch_notes_the_parameter", es.size() >= 1 && es[0].labels.size() == 1);
        if (!es.empty() && es[0].labels.size() == 1) {
            check_true("arg_note_names_the_param",
                       es[0].labels[0].text.find("parameter 'n'") != std::string::npos);
            check_true("arg_note_not_the_annotation",
                       es[0].labels[0].text.find("annotation") == std::string::npos);
            check_true("arg_note_at_declaration",
                       es[0].labels[0].line == 1 && es[0].line == 2);
        }
    }

    // (6) the same note for a METHOD and for an ASSOCIATED FUNCTION. `MethodSig::origin` holds
    //     `Method::params`, which excludes `self`, so it stays index-aligned with the argument list
    //     after the receiver is dropped -- an off-by-one here would cite the wrong parameter.
    {
        auto es = check_errs("struct Box { n: Int }\n"
                             "impl Box { fn grow(self, by: Int) -> Int { self.n + by } }\n"
                             "fn f(b: Box) -> Int { b.grow(\"x\") }");
        check_true("method_arg_notes_param",
                   es.size() >= 1 && es[0].labels.size() == 1 &&
                   es[0].labels[0].text.find("parameter 'by'") != std::string::npos);
    }
    {
        auto es = check_errs("struct P { x: Int }\n"
                             "impl P { fn make(x: Int) -> P { P { x: x } } }\n"
                             "fn f() -> P { P::make(true) }");
        check_true("assoc_fn_arg_notes_param",
                   es.size() >= 1 && es[0].labels.size() == 1 &&
                   es[0].labels[0].text.find("parameter 'x'") != std::string::npos);
    }
    // A TRAIT method is the receiver-offset case: the free form `m(recv, a, b)` drops arg 0 before
    // checking, so citing the SECOND non-self parameter is what proves there is no off-by-one. Both
    // spellings must agree, and both point at the trait's declaration (the signature of record).
    {
        const char* T = "trait Scaler { fn scaleBy(self, factor: Int, label: String) -> Int }\n"
                        "struct Box { n: Int }\n"
                        "impl Scaler for Box { fn scaleBy(self, factor: Int, label: String) -> Int"
                        " { self.n * factor } }\n"
                        "fn f(b: Box) -> Int { ";
        for (const char* form : { "scaleBy(b, 3, 99) }", "b.scaleBy(3, 99) }" }) {
            auto es = check_errs(std::string(T) + form);
            const bool ok = es.size() >= 1 && es[0].labels.size() == 1 &&
                            es[0].labels[0].text.find("parameter 'label'") != std::string::npos &&
                            es[0].labels[0].line == 1;   // the trait declaration
            check_true(form[0] == 's' ? "trait_free_form_notes_param" : "trait_dot_form_notes_param", ok);
        }
    }

    // (7) a BUILTIN has no source parameters to point at, so it keeps the historical single caret
    //     rather than inventing a position. Same for a first-class function VALUE, which has only a
    //     `Fn` type and no declaration at all.
    {
        auto es = check_errs("fn f() -> Int { len(42) }");
        check_true("builtin_arg_no_note", es.size() >= 1 && es[0].labels.empty());
    }
    {
        auto es = check_errs("fn g(n: Int) -> Int { n }\n"
                             "fn f() -> Int { let h: fn(Int) -> Int = g\n h(true) }");
        check_true("fn_value_arg_no_note", es.size() >= 1 && es[0].labels.empty());
    }

    // (8) CROSS-MODULE routing: the note must carry the DECLARING module, not the calling one, or the
    //     driver would caret it against the wrong file. Asserted on a module set rather than on std,
    //     so it does not depend on any line number in the prelude.
    {
        using Mods = std::unordered_map<std::string, std::string>;
        auto errs = modules_check_errs("use lib::twice\n twice(\"x\")",
                                       Mods{{"lib", "pub fn twice(n: Int) -> Int { n * 2 }"}});
        check_true("xmod_arg_note", errs.size() >= 1 && errs[0].labels.size() == 1);
        if (!errs.empty() && errs[0].labels.size() == 1) {
            check_true("xmod_arg_note_names_param",
                       errs[0].labels[0].text.find("parameter 'n'") != std::string::npos);
            check_true("xmod_arg_note_declaring_module",
                       errs[0].labels[0].module == "lib" && errs[0].labels[0].module != errs[0].module);
        }
    }
}

// ---- typechecker: advisory warnings (must-use + unused bindings) ------------
// These are the WARNING tier: they populate CheckResult::warnings, never ::errors,
// and never affect ok(). A future `static_vmrun --strict` escalates them to fatal.
void test_check_warnings() {
    std::cout << "[check: warnings]\n";
    const char* RES = "enum Result[T, E] { Ok(T), Err(E) }\nenum Option[T] { None, Some(T) }\n";
    const char* H   = "fn h() -> Result[Int, String] { Ok(1) }\n";   // a Result-returning helper

    // --- must-use: a discarded Result/Option is a warning ---
    check_true("mustuse_discarded",
        check_warn_has(std::string(RES) + H + "fn g() -> Int { h()\n 0 }", "unused Result"));
    // `let _ = e` is the explicit discard escape -> silent (Wildcard binds nothing, not an ExprStmt).
    check_true("mustuse_let_wildcard_silent",
        check_warnc(std::string(RES) + H + "fn g() -> Int { let _ = h()\n 0 }") == 0);
    // Consuming it (match) silences must-use AND the unused-binding check.
    check_true("mustuse_consumed_silent",
        check_warnc(std::string(RES) + H +
            "fn g() -> Int { let x = h()\n match x { Ok(v) => v, Err(_) => 0 } }") == 0);
    // A discarded non-carrier (plain Int) is quiet.
    check_true("mustuse_noncarrier_quiet",
        check_warnc("fn h() -> Int { 1 }\nfn g() -> Int { h()\n 0 }") == 0);
    // A loop body's value is always discarded -> a Result there warns (loop var `_` to isolate it).
    check_true("mustuse_loop_body",
        check_warn_has(std::string(RES) + H +
            "fn g(xs: List[Int]) -> Int { for _ in xs { h() }\n 0 }", "unused Result"));
    // A Result as a unit-returning function's TAIL is a type ERROR, not a must-use warning.
    check_true("mustuse_tail_is_error",
        check_has(std::string(RES) + H + "fn g() -> () { h() }", "type mismatch"));

    // --- unused bindings: let / match / for, params exempt ---
    check_true("unused_let",
        check_warn_has("fn f() -> Int { let x = 1\n 0 }", "unused variable 'x'"));
    check_true("unused_let_read_quiet",
        check_warnc("fn f() -> Int { let x = 1\n x }") == 0);
    check_true("unused_underscore_quiet",
        check_warnc("fn f() -> Int { let _x = 1\n 0 }") == 0);
    check_true("unused_match_binding",
        check_warn_has(std::string(RES) +
            "fn g(o: Option[Int]) -> Int { match o { Some(x) => 0, None => 1 } }",
            "unused variable 'x'"));
    check_true("unused_param_exempt",
        check_warnc("fn f(x: Int) -> Int { 0 }") == 0);

    // --- carrier vs generic message: an unread let-bound Result gets the must-use
    //     message (once, not doubled with the generic unused-variable one) ---
    check_true("unused_carrier_message",
        check_warn_has(std::string(RES) + H + "fn g() -> Int { let r = h()\n 0 }",
                       "unused Result 'r'"));
    check_true("unused_carrier_single",
        check_warnc(std::string(RES) + H + "fn g() -> Int { let r = h()\n 0 }") == 1);
}

// ---- typechecker: duplicate literal key in a map literal --------------------
// `#{1 => 7, 1 => 8}` keeps ONE entry and drops the `7`. That is the standard dict-literal last-wins
// rule (Python and JS do the same), so the RUNTIME behaviour is not the bug and does not change -- this
// is advisory only, and `--strict` escalates it. What makes it worth saying at all is the interaction
// with a second, individually correct rule: map keys canonicalize, and `Int <: Double` widens at check
// boundaries, so `Map[Double, Int] = #{1 => 7, 1.0 => 8}` merges two keys that do not LOOK equal.
// Every case below counts warnings, because a diagnostic that fires twice is its own defect.
void test_check_duplicate_map_key() {
    std::cout << "[check: duplicate literal key in a map literal]\n";

    // --- positive: one warning per duplicate, for each literal key kind ---
    check_true("dupkey_int",    check_warn_has("let m = #{1 => 7, 1 => 8}\n println(len(m))",
                                               "duplicate key in map literal"));
    check_true("dupkey_int_once",  check_warnc("let m = #{1 => 7, 1 => 8}\n println(len(m))") == 1);
    check_true("dupkey_str",       check_warnc("let m = #{\"k\" => 7, \"k\" => 8}\n println(len(m))") == 1);
    check_true("dupkey_bool",      check_warnc("let m = #{true => 7, true => 8}\n println(len(m))") == 1);
    check_true("dupkey_double",    check_warnc("let m = #{1.5 => 7, 1.5 => 8}\n println(len(m))") == 1);
    // A NEGATED numeric literal counts (`-1` is a UnaryExpr over an IntLit, not a lexed literal).
    check_true("dupkey_negative",  check_warnc("let m = #{-1 => 7, -1 => 8}\n println(len(m))") == 1);
    // ...but `-1` and `1` are different keys, and the unary arm must not conflate them.
    check_true("dupkey_negative_vs_positive",
        check_warnc("let m = #{-1 => 7, 1 => 8}\n println(len(m))") == 0);

    // Two duplicates of the same key report TWICE, each against the FIRST occurrence -- the `seen` map
    // deliberately keeps the first entry rather than advancing to the latest.
    check_true("dupkey_triple",
        check_warnc("let m = #{1 => 7, 1 => 8, 1 => 9}\n println(len(m))") == 2);

    // --- negatives, each a distinct rule ---
    check_true("dupkey_distinct_quiet",
        check_warnc("let m = #{1 => 7, 2 => 8}\n println(len(m))") == 0);
    check_true("dupkey_distinct_str_quiet",
        check_warnc("let m = #{\"a\" => 7, \"b\" => 8}\n println(len(m))") == 0);
    // A NON-LITERAL key is a runtime matter and out of scope -- silent even when the two bindings
    // provably hold the same value.
    check_true("dupkey_nonliteral_quiet",
        check_warnc("let a = 1\n let b = 1\n let m = #{a => 7, b => 8}\n println(len(m))") == 0);
    // An un-annotated `#{1 => 7, 1.0 => 8}` is already a hard type ERROR (`join` stays exact), so no
    // warning is piled on top of it.
    check_true("dupkey_mixed_is_error_not_warning",
        check_has("let m = #{1 => 7, 1.0 => 8}\n println(len(m))", "map keys have incompatible types"));
    check_true("dupkey_mixed_no_warning",
        check_warnc("let m = #{1 => 7, 1.0 => 8}\n println(len(m))") == 0);
    // The whole sealed std must stay warning-free, or `--strict` breaks for every user at once, in
    // code they cannot edit. This compiles the ENTIRE embedded prelude, so it covers every std map
    // literal at once -- the check `check_warnc` structurally cannot make.
    check_true("dupkey_std_quiet", check_warnc_p("println(1)") == 0);

    // --- the widened pair: the case that started this, and the only one needing `widen_double` ---
    // WITH the annotation the keys go through `subsumes`, both become the double 1.0, and the map
    // really does end up with one entry -- so `1` and `1.0` must compare EQUAL here even though they
    // are different literals of different kinds.
    check_true("dupkey_widened",
        check_warnc("let m: Map[Double, Int] = #{1 => 7, 1.0 => 8}\n println(len(m))") == 1);
    // The message must name both spellings; without that it reads as a compiler bug.
    check_true("dupkey_widened_message",
        check_warn_has("let m: Map[Double, Int] = #{1 => 7, 1.0 => 8}\n println(len(m))",
                       "1.0 and 1 are the same key"));
    // The plain same-spelling case keeps the SHORT message -- the explanatory clause is not noise
    // people have to read every time.
    check_true("dupkey_same_spelling_short_message",
        check_warn_has("let m = #{1 => 7, 1 => 8}\n println(len(m))",
                       "this entry replaces the earlier one"));
    // Widened but genuinely DISTINCT keys stay silent.
    check_true("dupkey_widened_distinct_quiet",
        check_warnc("let m: Map[Double, Int] = #{1 => 7, 2.0 => 8}\n println(len(m))") == 0);
    // Negation negates the VALUE, so signed zero collides exactly as it does at run time.
    check_true("dupkey_negative_zero",
        check_warnc("let m = #{-0.0 => 7, 0.0 => 8}\n println(len(m))") == 1);
    // A Char literal IS an Int literal retyped, and a Char erases to its code point -- so these two
    // really are one key.
    check_true("dupkey_char_vs_code_point",
        check_warnc("let m: Map[Char, Int] = #{'a' => 7, 97 => 8}\n println(len(m))") == 1);
}

// ---- typechecker: blanket impls (`impl[T: bounds] Tr for T`) ----------------
// The category-B gap: a concrete type must be able to satisfy a trait THROUGH a
// blanket impl, with concrete impls winning and the blanket's bounds covering the
// trait's supertrait closure (soundness).
void test_check_blanket() {
    std::cout << "[check: blanket]\n";
    const char* SHOWB =
        "trait Show { fn show(self) -> String }\n"
        "impl[T] Show for T { fn show(self) -> String { \"x\" } }\n";

    // An unbounded blanket satisfies the trait for a concrete type AND an unbounded generic.
    check_true("blanket_discharges_concrete",
        check_errc(std::string(SHOWB) + "fn f() -> String { show(5) }") == 0);
    check_true("blanket_discharges_generic",
        check_errc(std::string(SHOWB) + "fn g[T](x: T) -> String { show(x) }") == 0);

    // A bounded blanket `impl[T: Show] Debug for T`: a type WITH Show gains Debug; one WITHOUT
    // Show does not.
    const char* DBGB =
        "trait Show { fn show(self) -> String }\n"
        "trait Debug: Show { fn debug(self) -> String }\n"
        "impl[T: Show] Debug for T { fn debug(self) -> String { show(self) } }\n";
    check_true("blanket_bounded_ok",
        check_errc(std::string(DBGB) +
            "struct P {}\nimpl Show for P { fn show(self) -> String { \"p\" } }\n"
            "fn f(p: P) -> String { debug(p) }") == 0);
    check_true("blanket_bounded_bad",
        check_has(std::string(DBGB) + "struct Q {}\nfn f(q: Q) -> String { debug(q) }",
            "does not implement trait 'Debug'"));

    // A concrete impl coexists with the blanket (no overlap error) and wins.
    check_true("concrete_overrides_blanket",
        check_errc(std::string(SHOWB) +
            "impl Show for Int { fn show(self) -> String { \"int\" } }\n"
            "fn f() -> String { show(5) }") == 0);

    // At most one blanket per trait.
    check_true("duplicate_blanket",
        check_has(std::string(SHOWB) + "impl[U] Show for U { fn show(self) -> String { \"b\" } }",
            "duplicate blanket impl"));

    // Soundness: the blanket's bounds must cover the trait's supertrait closure.
    check_true("blanket_supertrait_uncovered",
        check_has(
            "trait Eq { fn eq(self) -> Bool }\ntrait Ord: Eq { fn lt(self) -> Bool }\n"
            "impl[T] Ord for T { fn lt(self) -> Bool { true } }",
            "requires its supertrait 'Eq'"));
    check_true("blanket_supertrait_covered",
        check_errc(
            "trait Eq { fn eq(self) -> Bool }\ntrait Ord: Eq { fn lt(self) -> Bool }\n"
            "impl[T: Eq] Ord for T { fn lt(self) -> Bool { true } }") == 0);

    // A method-less marker trait blanket-impl'd over a base bound -- the closest analogue of
    // Rust's `Numeric: Arithmetic` marker.
    check_true("marker_blanket",
        check_errc(
            "trait Arithmetic { }\ntrait Numeric { }\n"
            "impl Arithmetic for Int { }\nimpl[T: Arithmetic] Numeric for T { }\n"
            "fn needs[T: Numeric](x: T) -> Int { 0 }\nfn f() -> Int { needs(5) }") == 0);

    // A blanket does NOT bypass method coverage.
    check_true("blanket_missing_method",
        check_has("trait Debug { fn debug(self) -> String }\nimpl[T] Debug for T { }",
            "is missing method 'debug'"));
}

// ---- typechecker: bounded generic fn as a first-class value -----------------
// A bounded generic fn (`describe[T: Display]`) is usable where a CONCRETE `fn(...)->R`
// is expected -- the bound is monomorphized + discharged against that concrete type.
void test_check_bounded_fn_values() {
    std::cout << "[check: bounded-fn-value]\n";
    const char* DISP =
        "trait Display { fn show(self) -> String }\n"
        "impl Display for Int { fn show(self) -> String { \"i\" } }\n"
        "fn describe[T: Display](x: T) -> String { show(x) }\n";

    // Passed to a concrete `fn(Int)->String` parameter.
    check_true("arg_concrete_param",
        check_errc(std::string(DISP) +
            "fn ap(f: fn(Int) -> String, x: Int) -> String { f(x) }\n"
            "fn g() -> String { ap(describe, 1) }") == 0);
    // A generic combinator with the element parameter FIRST: `A` is pinned to Int by `5`
    // before `describe` is checked, so its `T` resolves concrete.
    check_true("arg_via_generic_combinator",
        check_errc(std::string(DISP) +
            "fn call1[A, B](x: A, f: fn(A) -> B) -> B { f(x) }\n"
            "fn g() -> String { call1(5, describe) }") == 0);
    // Annotated let (concrete expected type).
    check_true("annotated_let_ok",
        check_errc(std::string(DISP) + "fn g() -> String { let f: fn(Int) -> String = describe\n f(5) }") == 0);
    // Annotated at a type that does NOT satisfy the bound.
    check_true("annotated_let_bad",
        check_has(std::string(DISP) + "fn g() -> String { let f: fn(Bool) -> String = describe\n \"x\" }",
            "does not satisfy the bound 'Display'"));
    // Return position with a concrete fn return type.
    check_true("return_position_ok",
        check_errc(std::string(DISP) + "fn h() -> fn(Int) -> String { describe }") == 0);
    // Un-annotated `let f = describe` (infer position) is STILL rejected (H2 boundary).
    check_true("unannotated_still_rejected",
        check_has(std::string(DISP) + "fn g() -> String { let f = describe\n \"x\" }",
            "first-class value"));
    // Documented safe limitation: a combinator with the fn parameter BEFORE the element
    // parameter leaves `A` unsolved when `describe` is checked -> over-rejected.
    check_true("reversed_order_rejected",
        check_has(std::string(DISP) +
            "fn ap2[A, B](f: fn(A) -> B, x: A) -> B { f(x) }\n"
            "fn g() -> String { ap2(describe, 5) }",
            "is not determined by the expected type"));
}

// ---- typechecker: parametric traits + bounds with OUTPUT inference ----------
// `trait Iterable[T]` + a bound `[I: Iterable[T]]`: the element type `T` is SOLVED
// from the matching impl (Rust's functional-dependency / associated-type-lite rule).
void test_check_generic_traits() {
    std::cout << "[check: generic traits]\n";
    // A concrete `impl Iterable[Int] for Bag`: a bare `iter(b)` resolves to Vec[Int].
    const char* BAG =
        "trait Iterable[T] { fn iter(self) -> Vec[T] }\n"
        "struct Bag { items: Vec[Int] }\n"
        "impl Iterable[Int] for Bag { fn iter(self) -> Vec[Int] { self.items } }\n";
    check_true("concrete_impl_iter",
        check_errc(std::string(BAG) + "fn f(b: Bag) -> Vec[Int] { iter(b) }") == 0);
    // toVec: the bound's `T` is INFERRED from the impl (the headline win).
    check_true("toVec_infers_element",
        check_errc(std::string(BAG) +
            "fn toVec[T, I: Iterable[T]](it: I) -> Vec[T] { iter(it) }\n"
            "fn f(b: Bag) -> Vec[Int] { toVec(b) }") == 0);
    // A generic impl `impl[X] Iterable[X] for MyBox[X]` -- element solved per instantiation.
    const char* BOX =
        "trait Iterable[T] { fn iter(self) -> Vec[T] }\n"
        "struct MyBox[X] { one: X }\n"
        "impl[X] Iterable[X] for MyBox[X] { fn iter(self) -> Vec[X] { vec() } }\n";
    check_true("generic_impl_iter",
        check_errc(std::string(BOX) + "fn f(b: MyBox[Int]) -> Vec[Int] { iter(b) }") == 0);
    check_true("generic_impl_toVec",
        check_errc(std::string(BOX) +
            "fn toVec[T, I: Iterable[T]](it: I) -> Vec[T] { iter(it) }\n"
            "fn f(b: MyBox[Bool]) -> Vec[Bool] { toVec(b) }") == 0);

    // NEGATIVE: wrong element type -- bound Iterable[String] on a receiver whose element is Int.
    check_true("wrong_element_type",
        check_has(std::string(BAG) +
            "fn needsStr[I: Iterable[String]](x: I) -> Int { 0 }\n"
            "fn f(b: Bag) -> Int { needsStr(b) }",
            "does not satisfy the bound"));
    // NEGATIVE: no impl of Iterable for the receiver.
    check_true("no_impl_for_receiver",
        check_has(
            "trait Iterable[T] { fn iter(self) -> Vec[T] }\nstruct Bag { items: Vec[Int] }\n"
            "fn needs[T, I: Iterable[T]](x: I) -> Int { 0 }\n"
            "fn f(b: Bag) -> Int { needs(b) }",
            "does not satisfy the bound"));
    // NEGATIVE: an undetermined element type is rejected (I fixed by no argument -> Vec[T] leaks).
    check_true("unsolved_output_rejected",
        check_errc(std::string(BAG) +
            "fn mk[T, I: Iterable[T]]() -> Vec[T] { vec() }\n"
            "fn f() -> Vec[Int] { mk() }") > 0);
    // NEGATIVE: impl provides the wrong number of trait type args.
    check_true("trait_arg_arity",
        check_has(
            "trait Iterable[T] { fn iter(self) -> Vec[T] }\nstruct Bag { items: Vec[Int] }\n"
            "impl Iterable for Bag { fn iter(self) -> Vec[Int] { self.items } }",
            "type argument"));
    // NEGATIVE: impl method's element type disagrees with the trait.
    check_true("conformance_mismatch",
        check_has(
            "trait Iterable[T] { fn iter(self) -> Vec[T] }\nstruct Bag { items: Vec[Int] }\n"
            "impl Iterable[Int] for Bag { fn iter(self) -> Vec[String] { vec() } }",
            "but trait 'Iterable' requires"));
    // NEGATIVE: a blanket impl of a PARAMETRIC trait is not supported (decision 5).
    check_true("parametric_blanket_rejected",
        check_has(
            "trait Iterable[T] { fn iter(self) -> Vec[T] }\n"
            "impl[X] Iterable[X] for X { fn iter(self) -> Vec[X] { vec() } }",
            "blanket impl of the parametric trait"));
    // A parametric supertrait cannot even be written (grammar rejects it) -- decision 4.
    check_true("parametric_supertrait_rejected",
        parse_throws("trait X: Iterable[T] { fn m(self) -> Int }"));
}

// ---- codegen (P0 + P1): compile -> run -> read register 0 -------------------

// The empty prelude (the core-language path) -- the default for the harness helpers below.
static const std::vector<svc::PreludeModule> kNoPrelude{};

// Compile `src`, run it on a fresh heap, and return register 0. `prelude` empty means no prelude
// (the core-language path); svc::builtin_prelude() pulls in Option/Result + combinators.
Value cg_run(const std::string& src, const std::vector<svc::PreludeModule>& prelude = kNoPrelude) {
    svc::Module m = svc::compile(src.c_str(), prelude);
    Heap heap;
    StringInterner interner;
    // Wire the native registry so a program that calls a native (parseInt / parseDouble / …)
    // resolves the NativeId instead of misreading it as a baked pointer. Harmless for programs
    // that never call a native (the table is simply available).
    static const std::vector<NativeFunc> natives = build_native_table();
    auto res = execute(m.bytecode, &heap, nullptr, &interner, m.top_frame_size,
                       &m.constants, &m.struct_types, &m.string_literals, &kNoAtoms,
                       &m.function_table, /*out=*/nullptr,
                       &m.trait_table, m.trait_table_width, m.trait_method_count,
                       &m.line_table, &m.function_names, &m.column_table,
                       &natives, /*script_args=*/nullptr, /*in=*/nullptr,
                       /*function_modules=*/nullptr, &m.const_arrays);
    return res.get_reg_base()[0];
}

// Convenience: a synthetic monolithic "$prelude" source string (a test that supplies its own
// small prelude). Wraps it in a one-module list and forwards to the vector form above.
Value cg_run(const std::string& src, const char* prelude) {
    std::vector<svc::PreludeModule> mods;
    if (prelude && *prelude) mods.push_back({ "$prelude", prelude });
    return cg_run(src, mods);
}

// Compile+run `src` (with the prelude), capturing the print/println sink into a string.
std::string cg_run_out(const std::string& src) {
    svc::Module m = svc::compile(src.c_str(), svc::builtin_prelude());
    Heap heap;
    StringInterner interner;
    std::ostringstream oss;
    execute(m.bytecode, &heap, nullptr, &interner, m.top_frame_size,
            &m.constants, &m.struct_types, &m.string_literals, &kNoAtoms,
            &m.function_table, /*out=*/&oss,
            &m.trait_table, m.trait_table_width, m.trait_method_count);
    return oss.str();
}

// True iff running `src` (with the prelude) throws at RUNTIME (a VM fault, e.g. a panic/abort).
bool cg_faults(const std::string& src) {
    try { cg_run(src, svc::builtin_prelude()); return false; }
    catch (const std::exception&) { return true; }
}

// True iff running `src` throws at runtime with a message CONTAINING `needle` (e.g. the
// located "stack overflow" abort at the reserved-stack cap).
bool cg_faults_msg(const std::string& src, const char* needle) {
    try { cg_run(src, svc::builtin_prelude()); return false; }
    catch (const std::exception& e) { return std::string(e.what()).find(needle) != std::string::npos; }
}

// Compile+run WITH the prelude AND the real native registry, capturing the print/println sink.
// Optional command-line `args` (-> args()) and piped `stdin_text` (-> readLine / readAllStdin) let
// the file/time/env/stdin natives be exercised exactly as static_vmrun drives them.
std::string cg_run_native(const std::string& src,
                          const std::vector<std::string>& args = {},
                          const std::string& stdin_text = "") {
    // The I/O / env / process natives are now opt-in modules (stdlib split S3). A native-exercising
    // test program pulls all three in; an unused `use` is harmless.
    const std::string full = "use std::io::*\nuse std::env::*\nuse std::process::*\n" + src;
    svc::Module m = svc::compile(full.c_str(), svc::builtin_prelude());
    Heap heap;
    StringInterner interner;
    std::ostringstream out;
    std::istringstream in(stdin_text);
    static const std::vector<NativeFunc> natives = build_native_table();
    execute(m.bytecode, &heap, nullptr, &interner, m.top_frame_size,
            &m.constants, &m.struct_types, &m.string_literals, &kNoAtoms,
            &m.function_table, &out,
            &m.trait_table, m.trait_table_width, m.trait_method_count,
            &m.line_table, &m.function_names, &m.column_table,
            &natives, &args, &in);
    return out.str();
}

void check_int(const char* name, const std::string& src, int64_t expect) {
    try {
        Value v = cg_run(src);
        const int64_t got = v.asSigned48();
        check_true(name, v.isInt() && got == expect);
        if (!(v.isInt() && got == expect))
            std::cout << "     expected Int " << expect << ", got " << got << "\n";
    } catch (const std::exception& e) {
        ++g_fail;
        std::cout << "  WRONG!    " << name << "   (threw: " << e.what() << ")\n";
    }
}

void check_bool(const char* name, const std::string& src, bool expect) {
    try {
        Value v = cg_run(src);
        check_true(name, v.isBool() && v.asBool() == expect);
        if (!(v.isBool() && v.asBool() == expect))
            std::cout << "     expected Bool " << (expect ? "true" : "false") << "\n";
    } catch (const std::exception& e) {
        ++g_fail;
        std::cout << "  WRONG!    " << name << "   (threw: " << e.what() << ")\n";
    }
}

void check_dbl(const char* name, const std::string& src, double expect) {
    try {
        Value v = cg_run(src);
        check_true(name, v.isDouble() && v.asDouble() == expect);
        if (!(v.isDouble() && v.asDouble() == expect))
            std::cout << "     expected Double " << expect << "\n";
    } catch (const std::exception& e) {
        ++g_fail;
        std::cout << "  WRONG!    " << name << "   (threw: " << e.what() << ")\n";
    }
}

// Prelude-linked variants: compile `src` WITH the static prelude (Option/Result + combinators).
void check_int_p(const char* name, const std::string& src, int64_t expect) {
    try {
        Value v = cg_run(src, svc::builtin_prelude());
        const int64_t got = v.asSigned48();
        check_true(name, v.isInt() && got == expect);
        if (!(v.isInt() && got == expect))
            std::cout << "     expected Int " << expect << ", got " << got << "\n";
    } catch (const std::exception& e) {
        ++g_fail;
        std::cout << "  WRONG!    " << name << "   (threw: " << e.what() << ")\n";
    }
}
void check_bool_p(const char* name, const std::string& src, bool expect) {
    try {
        Value v = cg_run(src, svc::builtin_prelude());
        check_true(name, v.isBool() && v.asBool() == expect);
        if (!(v.isBool() && v.asBool() == expect))
            std::cout << "     expected Bool " << (expect ? "true" : "false") << "\n";
    } catch (const std::exception& e) {
        ++g_fail;
        std::cout << "  WRONG!    " << name << "   (threw: " << e.what() << ")\n";
    }
}

// Prelude-linked error-substring check: compile `src` WITH the static prelude and return whether the
// thrown diagnostic mentions `substr`. Needed for errors that reference a real prelude entity (e.g. the
// sealed std::core::Eq marker or `dyn Eq`), which the no-prelude check_has cannot see.
bool check_has_p(const std::string& src, const std::string& substr) {
    try { cg_run(src, svc::builtin_prelude()); return false; }
    catch (const std::exception& e) { return std::string(e.what()).find(substr) != std::string::npos; }
}

// True iff compiling `src` throws a CodegenError (a not-yet-supported construct).
bool cg_rejects(const std::string& src) {
    try { svc::compile(src.c_str()); return false; }
    catch (const svc::CodegenError&) { return true; }
    catch (...) { return false; }
}

// True iff compiling `src` throws a CheckFailure (a type error -> no bytecode emitted).
bool cg_check_fails(const std::string& src) {
    try { svc::compile(src.c_str()); return false; }
    catch (const svc::CheckFailure&) { return true; }
    catch (...) { return false; }
}

// Prelude-linked cg_check_fails: compile `src` WITH the std prelude. A feature gated on a
// prelude-declared trait -- e.g. the `Hashable` map-key bound -- is only active when the prelude is
// present (no trait in scope => gating off), so its negative tests MUST link the prelude.
bool cg_check_fails_p(const std::string& src) {
    try { svc::compile(src.c_str(), svc::builtin_prelude()); return false; }
    catch (const svc::CheckFailure&) { return true; }
    catch (...) { return false; }
}

// --- multi-module compile+run (Slice 2a) ---
// Load `entry` + the in-memory `mods` map into a ModuleSet, compile the whole set (with the
// prelude, if any) and run it, returning register 0 -- the module-set sibling of cg_run.
Value cg_run_modules(const std::string& entry,
                     const std::unordered_map<std::string, std::string>& mods,
                     const std::vector<svc::PreludeModule>& prelude = kNoPrelude) {
    svc::ModuleSet set = svc::load_modules(entry.c_str(), mem_resolver(mods));
    svc::Module m = svc::compile_modules(std::move(set), prelude);
    Heap heap;
    StringInterner interner;
    auto res = execute(m.bytecode, &heap, nullptr, &interner, m.top_frame_size,
                       &m.constants, &m.struct_types, &m.string_literals, &kNoAtoms,
                       &m.function_table, /*out=*/nullptr,
                       &m.trait_table, m.trait_table_width, m.trait_method_count);
    return res.get_reg_base()[0];
}

// True iff loading+compiling the module set throws a CheckFailure (a type/resolution error).
bool cg_modules_check_fails(const std::string& entry,
                            const std::unordered_map<std::string, std::string>& mods) {
    try {
        svc::ModuleSet set = svc::load_modules(entry.c_str(), mem_resolver(mods));
        svc::compile_modules(std::move(set), nullptr);
        return false;
    } catch (const svc::CheckFailure&) { return true; }
    catch (...) { return false; }
}

void check_int_modules(const char* name, const std::string& entry,
                       const std::unordered_map<std::string, std::string>& mods,
                       int64_t expect, const std::vector<svc::PreludeModule>& prelude = kNoPrelude) {
    try {
        Value v = cg_run_modules(entry, mods, prelude);
        const int64_t got = v.asSigned48();
        check_true(name, v.isInt() && got == expect);
        if (!(v.isInt() && got == expect))
            std::cout << "     expected Int " << expect << ", got " << got << "\n";
    } catch (const std::exception& e) {
        ++g_fail;
        std::cout << "  WRONG!    " << name << "   (threw: " << e.what() << ")\n";
    }
}

// The import MATRIX: every way a newcomer can try to reach an item, for every kind of item, pinned in one
// table so a rule that holds for one kind but not another shows up as one wrong cell. (It did: until
// earlier `use std::math::sqrt` was rejected while `use std::math::PI` was fine, and any use of a module
// unlocked all its natives.) The rule it pins: an item is reachable bare through `use m::*`, `use m::name`
// or `use m::{name, ..}` -- never through `import m` alone or a `use` of another name; a user module's pub
// item is also reachable qualified as `m::name` once `m` is imported; a private item through nothing. The
// std rows cover a native per gated module, a Skarn fn, a const, a type with an associated fn, and an enum.
// A cell only says ACCEPT (with the value) or REJECT; the wording of each rejection is the diagnostics'
// business (tier2/newcomer_* claims), so the matrix does not break when a message improves.
void test_import_matrix() {
    std::cout << "[test_import_matrix: item kind x import form]\n";
    using M = std::unordered_map<std::string, std::string>;
    const int REJECT = -999;
    auto std_cell = [&](const std::string& name, const std::string& header, const std::string& body, int expect) {
        const std::string src = (header.empty() ? "" : header + "\n") + body;
        if (expect == REJECT) check_true(name.c_str(), cg_check_fails_p(src));
        else                  check_int_p(name.c_str(), src, expect);
    };
    struct StdRow { const char* id; const char* mod; const char* item; const char* other; const char* body; int value; };
    const StdRow std_rows[] = {
        { "native_math", "std::math",    "sqrt",         "PI",           "toInt(sqrt(9.0))", 3 },
        { "native_io",   "std::io",      "fileExists",   "readTextFile", "if fileExists(\"no/such/dir.xyz\") { 1 } else { 0 }", 0 },
        { "native_env",  "std::env",     "nanoTime",     "getEnv",       "let _ = nanoTime()\n 1", 1 },
        { "native_hash", "std::hash",    "sha256",       "crc32Str",     "len(sha256(bytes()))", 32 },
        { "std_fn",      "std::math",    "toIntChecked", "PI",           "match toIntChecked(3.9) { Ok(n) => n, Err(_) => 0 }", 3 },
        { "std_const",   "std::math",    "PI",           "E",            "toInt(PI)", 3 },
        { "std_type",    "std::random",  "Rng",          nullptr,        "let mut r = Rng::fromSeed(1)\n r.nextInt(5, 6)", 5 },
        { "std_enum",    "std::json",    "Json",         "JsonEntry",    "match Json::Integer(3) { Json::Integer(n) => n, _ => 0 }", 3 },
    };
    for (const auto& r : std_rows) {
        const std::string id = std::string("imx_") + r.id;
        std_cell(id + "_none",   "",                                              r.body, REJECT);
        std_cell(id + "_glob",   std::string("use ") + r.mod + "::*",             r.body, r.value);
        std_cell(id + "_name",   std::string("use ") + r.mod + "::" + r.item,     r.body, r.value);
        std_cell(id + "_import", std::string("import ") + r.mod,                  r.body, REJECT);
        if (r.other) {
            std_cell(id + "_braces", std::string("use ") + r.mod + "::{" + r.item + ", " + r.other + "}", r.body, r.value);
            std_cell(id + "_other",  std::string("use ") + r.mod + "::" + r.other, r.body, REJECT);
        }
    }

    // A user module: every item kind once. `score` (not `get`) as the trait method: a bare `get(x)` is
    // the ambient Map/Vec builtin, which wins over a trait method of that name (a known limitation).
    const M LIB{{"m",
        "pub fn pubFn() -> Int { 1 }\n"
        "fn privFn() -> Int { 2 }\n"
        "pub struct P { x: Int }\n"
        "impl P { fn new(x: Int) -> P { P { x: x } } }\n"
        "pub trait T { fn score(self) -> Int }\n"
        "impl T for P { fn score(self) -> Int { self.x } }\n"
        "pub const K: Int = 7\n"
        "pub enum Color { Red, Green }\n"
        "pub fn other() -> Int { 0 }\n"}};
    auto user_cell = [&](const std::string& name, const std::string& header, const std::string& body, int expect) {
        const std::string src = header + "\n" + body;
        if (expect == REJECT) check_true(name.c_str(), cg_modules_check_fails(src, LIB));
        else                  check_int_modules(name.c_str(), src, LIB, expect);
    };
    struct UserRow { const char* id; const char* item; const char* body; const char* qualified; int value; };
    const UserRow user_rows[] = {
        { "pub_fn",  "pubFn",  "pubFn()",   "m::pubFn()", 1 },
        { "priv_fn", "privFn", "privFn()",  "m::privFn()", REJECT },
        { "struct",  "P",      "P::new(3).x", nullptr, 3 },
        { "trait",   "T",      "struct Q { y: Int }\n impl T for Q { fn score(self) -> Int { self.y } }\n score(Q { y: 4 })", nullptr, 4 },
        { "const",   "K",      "K",         "m::K", 7 },
        { "enum",    "Color",  "match Color::Red { Color::Red => 1, Color::Green => 2 }", nullptr, 1 },
    };
    for (const auto& r : user_rows) {
        const std::string id = std::string("imx_user_") + r.id;
        user_cell(id + "_import_only", "import m",                                          r.body, REJECT);
        user_cell(id + "_glob",        "import m\nuse m::*",                                 r.body, r.value);
        user_cell(id + "_name",        std::string("import m\nuse m::") + r.item,            r.body, r.value);
        user_cell(id + "_braces",      std::string("import m\nuse m::{") + r.item + ", other}", r.body, r.value);
        user_cell(id + "_other",       "import m\nuse m::other",                             r.body, REJECT);
        if (r.qualified) {
            user_cell(id + "_qualified",          "import m", r.qualified, r.value);
            user_cell(id + "_qualified_noimport", "",         r.qualified, REJECT);
        }
    }
    // Variants: through the enum (glob or braces) or the module glob, never bare without them.
    const char* VARIANTS = "match Red { Red => 1, Green => 2 }";
    user_cell("imx_user_variants_none",        "import m",                             VARIANTS, REJECT);
    user_cell("imx_user_variants_module_glob", "import m\nuse m::*",                   VARIANTS, 1);
    user_cell("imx_user_variants_enum_glob",   "import m\nuse m::Color::*",            VARIANTS, 1);
    user_cell("imx_user_variants_enum_braces", "import m\nuse m::Color::{Red, Green}", VARIANTS, 1);
}

// Newcomer diagnostics that a message-substring claim cannot pin: the COUNT of errors. A follow-on error
// printed before (or instead of) the real one sends a newcomer after the wrong line -- earlier a
// match over an unknown name opened with "non-exhaustive match", and `Json::Integer(3)` without its `use`
// produced four errors, none naming the missing import. The wording itself is pinned by the
// tier2/newcomer_* claims.
void test_newcomer_diagnostics() {
    std::cout << "[test_newcomer_diagnostics: one mistake, one error]\n";
    auto errc_p = [](const std::string& src) -> size_t {
        try { svc::compile(src.c_str(), svc::builtin_prelude()); return 0; }
        catch (const svc::CheckFailure& e) { return e.errors().size(); }
        catch (...) { return 999; }
    };
    auto first_error_p = [](const std::string& src) -> std::string {
        try { svc::compile(src.c_str(), svc::builtin_prelude()); return ""; }
        catch (const svc::CheckFailure& e) { return e.errors().empty() ? "" : e.errors().front().message; }
        catch (...) { return "?"; }
    };
    // A match whose scrutinee failed to type: no exhaustiveness verdict on a type nobody knows.
    check_true("newcomer_match_unknown_scrutinee_one_error",
        errc_p("match toIntChecked(3.9) { Ok(n) => n, Err(_) => 0 }") == 1);
    check_true("newcomer_match_unknown_scrutinee_no_prelude", check_errc("match nope(1) { 1 => 1 }") == 1);
    // A qualified variant of an enum that is not in scope: one error, naming the type -- not a second
    // "is not an enum" for the pattern, "unknown constructor in pattern" and "unreachable match arm".
    check_true("newcomer_enum_without_use_one_error_per_site",
        errc_p("let j = Json::Integer(3)\n match j { Json::Integer(n) => n, _ => 0 }") == 2);
    check_true("newcomer_enum_without_use_names_type",
        first_error_p("let j = Json::Integer(3)\n 0").find("unknown type 'Json'") != std::string::npos);
    check_true("newcomer_unknown_enum_pattern_no_cascade",
        check_errc("match 1 { Nope::Bar(n) => n, _ => 0 }") == 1);
    // A known struct is still "not an enum" (the head exists, it is just the wrong kind).
    check_true("newcomer_struct_is_not_enum", check_has("struct S { x: Int }\n let v = S::Bar(1)\n 0", "'S' is not an enum"));
    // A failed pipe argument must not add an operator error about the lambda's unsolved parameter.
    check_true("newcomer_pipe_without_intoiter_one_error",
        errc_p("let xs = toVec([1, 2, 3])\n let ys = xs |> map(fn(x) { x * 2 }) |> collect\n len(ys)") == 1);
    // `return` belongs to a function; at the top level it used to compile and crash the VM.
    check_true("newcomer_return_toplevel_rejected", check_has("return\n 1", "'return' outside of a function"));
    check_true("newcomer_return_in_toplevel_block_rejected", check_has("if true { return }\n 1", "'return' outside of a function"));
    check_int("newcomer_return_in_toplevel_lambda_ok",
        "let f = fn(x: Int) -> Int { if x > 0 { return 1 }\n 0 }\n f(5)", 1);
    check_int("newcomer_return_in_fn_ok", "fn f(x: Int) -> Int { if x > 0 { return 1 }\n 0 }\n f(5)", 1);
    // A private item reached by a glob or by nothing: say it exists but is private.
    {
        using M = std::unordered_map<std::string, std::string>;
        const M LIB{{"m", "pub fn shown() -> Int { 1 }\nfn secret() -> Int { 2 }"}};
        auto modules_error_has = [](const std::string& entry, const M& mods, const std::string& substr) {
            try {
                svc::compile_modules(svc::load_modules(entry.c_str(), mem_resolver(mods)), nullptr);
                return false;
            } catch (const std::exception& e) { return std::string(e.what()).find(substr) != std::string::npos; }
        };
        check_true("newcomer_private_via_glob_hint",
            modules_error_has("import m\nuse m::*\nsecret()", LIB, "'secret' is declared in 'm' but is not `pub`"));
        check_true("newcomer_private_without_use_hint",
            modules_error_has("import m\nsecret()", LIB, "'secret' is declared in 'm' but is not `pub`"));
    }

    // `pub fn` on a method (the Rust habit) used to be a bare "expected 'fn'".
    {
        auto parse_msg = [](const std::string& src) -> std::string {
            try { svc::Lexer lex(src); svc::Parser p(lex.tokenize()); p.parse_program(); return ""; }
            catch (const svc::ParseError& e) { return e.what(); }
        };
        check_true("newcomer_pub_on_inherent_method",
            parse_msg("struct P { x: Int }\nimpl P { pub fn new(x: Int) -> P { P { x } } }").find(
                "'pub' is not allowed on a method") != std::string::npos);
        check_true("newcomer_pub_on_trait_method",
            parse_msg("trait T { pub fn m(self) -> Int }").find("'pub' is not allowed on a method") != std::string::npos);
        check_true("newcomer_method_without_pub_ok",
            parse_msg("struct P { x: Int }\nimpl P { fn new(x: Int) -> P { P { x } } }").empty());
    }

    // A builtin / native passed as a function value is call-only; it used to say "unknown variable".
    check_true("newcomer_builtin_as_value",
        check_has_p("let f = toString\n 0", "built-in function 'toString' can only be called"));
    check_true("newcomer_builtin_as_map_arg",
        check_has_p("toVec([1, 2]) |> intoIter |> map(toString) |> collect |> len", "wrap it in a lambda"));
    check_true("newcomer_native_as_value",
        check_has_p("use std::math::*\n let f = sqrt\n 0", "built-in function 'sqrt' can only be called"));
    check_true("newcomer_native_as_value_one_error",
        errc_p("use std::math::*\n let f = sqrt\n 0") == 1);
    // A prelude fn is an ordinary fn value, and a user fn of a builtin's name shadows it (both unchanged).
    check_int_p("newcomer_prelude_fn_as_value_ok",
        "toVec([\" a\", \"bb \"]) |> intoIter |> map(trim) |> map(fn(s: String) -> Int { len(s) }) |> sum", 3);
    check_int_p("newcomer_user_fn_shadowing_builtin_as_value_ok",
        "fn len(x: Int) -> Int { x * 2 }\n let f = len\n f(4)", 8);
}

void test_modules() {
    std::cout << "[modules: 2a compile+run]\n";
    using M = std::unordered_map<std::string, std::string>;

    // Behavior-preserving: a single root module (no imports) == the plain compile() path.
    check_int_modules("mod_root_only", "let x = 40\n x + 2", M{}, 42);
    // `use` brings a fn in unqualified (and pulls the module in). (Plain `import` only LOADS
    // the module -- 2b removes 2a's flat cross-module visibility; qualified `mod::x` is 2c.)
    check_int_modules("mod_use_fn",
                      "use util::add\n add(40, 2)",
                      M{{"util", "pub fn add(a: Int, b: Int) -> Int { a + b }"}}, 42);
    // A struct brought in via `use` is usable (constructor + field access).
    check_int_modules("mod_use_struct",
                      "use geo::Point\n Point { x: 40, y: 2 }.x",
                      M{{"geo", "pub struct Point { x: Int, y: Int }"}}, 40);
    // Per-module top-level scoping: a module's top-level `let` is PRIVATE -- NOT visible to the entry.
    check_true("mod_toplevel_private",
               cg_modules_check_fails("import util\n g + 1", M{{"util", "let g = 41"}}));
    // A module may still carry top-level init (a `let`) alongside its exported fn; it compiles + runs.
    check_int_modules("mod_toplevel_runs",
                      "use util::helper\n helper()",
                      M{{"util", "let g = 41\n pub fn helper() -> Int { 42 }"}}, 42);
    // Transitive load (entry -> a -> b): `use` chains across modules; loaded topologically.
    check_int_modules("mod_chain",
                      "use a::go\n go()",
                      M{{"a", "use b::f\n pub fn go() -> Int { f(42) }"},
                        {"b", "pub fn f(n: Int) -> Int { n }"}}, 42);
    // The prelude still flows through the module path (Option/Result available).
    check_int_modules("mod_prelude_flows",
                      "match Some(42) { Some(n) => n, None => 0 }",
                      M{}, 42, svc::builtin_prelude());

    // --- 2b: per-module namespacing (mangling) ---
    // Two modules define a fn of the SAME name -> they coexist (no duplicate-fn error); an
    // explicit `use` selects one. (Both are loaded: `use` and `import` each pull a module in.)
    check_int_modules("mod_coexist_fn",
                      "import phys\n use geo::area\n area()",
                      M{{"geo",  "pub fn area() -> Int { 40 }"},
                        {"phys", "fn area() -> Int { 2 }"}}, 40);
    // Two modules define a struct of the SAME name -> distinct types; a fn returning one
    // resolves its OWN module's struct, and field access works cross-module via `use`.
    check_int_modules("mod_coexist_struct",
                      "import phys\n use geo::mk\n mk().v",
                      M{{"geo",  "struct P { v: Int }\n pub fn mk() -> P { P { v: 40 } }"},
                        {"phys", "struct P { v: Int }\n fn mk() -> P { P { v: 2 } }"}}, 40);
    // Two modules define an enum with the SAME variant names -> they coexist; a module's own
    // fn matches on its own variants (value + pattern both resolve to the module's variant).
    check_int_modules("mod_coexist_variant",
                      "import sig\n use pal::pick\n pick()",
                      M{{"sig", "enum C { R, G }"},
                        {"pal", "enum C { R, G }\n pub fn pick() -> Int { match R { R => 40, G => 2 } }"}}, 40);
    // A module references its OWN struct + fn unqualified (own-decl resolution).
    check_int_modules("mod_own_refs",
                      "use lib::compute\n compute()",
                      M{{"lib", "struct Box { n: Int }\n"
                                "fn wrap(n: Int) -> Box { Box { n: n } }\n"
                                "pub fn compute() -> Int { wrap(42).n }"}}, 42);

    // --- 2c: glob imports + weak-glob priority + collision errors ---
    // `use m::*` brings all of m's names in unqualified.
    check_int_modules("mod_glob_fn",
                      "use util::*\n add(40, 2)",
                      M{{"util", "pub fn add(a: Int, b: Int) -> Int { a + b }"}}, 42);
    check_int_modules("mod_glob_struct",
                      "use geo::*\n Point { x: 40, y: 2 }.x",
                      M{{"geo", "pub struct Point { x: Int, y: Int }"}}, 40);
    // Weak-glob: a LOCAL definition silently overrides a glob-imported name (no error).
    check_int_modules("mod_glob_local_wins",
                      "use util::*\n fn add(a: Int, b: Int) -> Int { a - b }\n add(44, 2)",
                      M{{"util", "pub fn add(a: Int, b: Int) -> Int { a + b }"}}, 42);
    // Weak-glob: an EXPLICIT `use` (strong) beats a glob (weak) for the same name.
    check_int_modules("mod_glob_explicit_wins",
                      "use a::*\n use b::v\n v()",
                      M{{"a", "pub fn v() -> Int { 1 }"}, {"b", "pub fn v() -> Int { 42 }"}}, 42);
    // Collision: two explicit imports of the same name -> hard error.
    check_true("mod_dup_use_error",
               cg_modules_check_fails("use a::v\n use b::v\n v()",
                   M{{"a", "pub fn v() -> Int { 1 }"}, {"b", "fn v() -> Int { 2 }"}}));
    // Collision: an explicit `use` that shadows a local definition -> hard error.
    check_true("mod_use_vs_local_error",
               cg_modules_check_fails("use util::add\n fn add(a: Int, b: Int) -> Int { a + b }\n add(1, 2)",
                   M{{"util", "pub fn add(a: Int, b: Int) -> Int { a + b }"}}));

    // --- qualified `mod::name` calls + values (case split: lident qualifier = module) ---
    // A module-qualified fn CALL.
    check_int_modules("mod_qual_fn",
                      "import util\n util::add(40, 2)",
                      M{{"util", "pub fn add(a: Int, b: Int) -> Int { a + b }"}}, 42);
    // A module-qualified variant constructor CALL, its result extracted by a `use`d fn.
    check_int_modules("mod_qual_ctor",
                      "import pal\n use pal::get\n get(pal::Wrap(42))",
                      M{{"pal", "pub enum C { Wrap(Int), Nil }\n"
                                "pub fn get(c: C) -> Int { match c { Wrap(n) => n, Nil => 0 } }"}}, 42);
    // A module-qualified nullary-constructor VALUE (`pal::Red`) passed to a fn.
    check_int_modules("mod_qual_value",
                      "import pal\n use pal::id\n id(pal::Red)",
                      M{{"pal", "pub enum C { Red, Blue }\n pub fn id(c: C) -> Int { match c { Red => 40, Blue => 2 } }"}}, 40);
    // A qualified call to a module that was never imported -> error.
    check_true("mod_qual_not_imported",
               cg_modules_check_fails("util::add(1, 2)",
                   M{{"util", "pub fn add(a: Int, b: Int) -> Int { a + b }"}}));

    // --- Slice 4: the orphan rule (impl allowed iff trait OR type is local to the module) ---
    // ALLOWED: the trait is local (impl'd for a foreign/builtin type in the trait's own module).
    check_int_modules("mod_orphan_trait_local",
                      "use gfx::run\n run()",
                      M{{"gfx", "trait Show { fn show(self) -> Int }\n"
                                "impl Show for Int { fn show(self) -> Int { 42 } }\n"
                                "pub fn run() -> Int { show(5) }"}}, 42);
    // ALLOWED: the type is local (impl a foreign trait for the module's own struct). The foreign
    // trait is brought in with `use` (trait names are module-scoped now, like structs/fns).
    check_int_modules("mod_orphan_type_local",
                      "use a::T\n struct Local { x: Int }\n"
                      "impl T for Local { fn m(self) -> Int { 42 } }\n m(Local { x: 0 })",
                      M{{"a", "pub trait T { fn m(self) -> Int }"}}, 42);
    // REJECTED: neither the trait (a's T) nor the type (b's S) is local to the entry -> orphan error.
    check_true("mod_orphan_rejected",
               cg_modules_check_fails(
                   "use a::T\n use b::S\n impl T for S { fn m(self) -> Int { 0 } }\n 1",
                   M{{"a", "pub trait T { fn m(self) -> Int }"},
                     {"b", "pub struct S { x: Int }"}}));
    // A trait impl whose target is a MODULE-LOCAL struct must dispatch correctly (the impl target
    // head has to be mangled for codegen's trait table, else the dynamic dispatch misses at runtime).
    check_int_modules("mod_impl_for_module_struct",
                      "use gfx::run\n run()",
                      M{{"gfx", "struct P { v: Int }\n"
                                "trait Show { fn show(self) -> Int }\n"
                                "impl Show for P { fn show(self) -> Int { self.v } }\n"
                                "pub fn run() -> Int { show(P { v: 42 }) }"}}, 42);
    // --- Slice 3: `std::` reaches the ambient/prelude namespace ---
    // A prelude constructor via `std::` (always available, no import).
    check_int_modules("mod_std_ctor",
                      "match std::Some(42) { Some(n) => n, None => 0 }",
                      M{}, 42, svc::builtin_prelude());
    // Diagnostic A: std:: reaching a prelude fn (root + prelude, no module).
    check_int_modules("mod_std_fn",
                      "match std::optionMap(std::Some(41), fn(x: Int) -> Int { x + 1 }) { Some(v) => v, None => 0 }",
                      M{}, 42, svc::builtin_prelude());
    // A module + the prelude together (no std::) -- the prelude keeps its own scope (a `use` in the
    // entry must not leak into the prelude's internal resolution).
    check_int_modules("mod_module_with_prelude",
                      "use m::run\n run()",
                      M{{"m", "pub fn run() -> Int { 42 }"}},
                      42, svc::builtin_prelude());
    // The real use case: a module shadows a prelude fn name with its own, and reaches the prelude
    // version through `std::`. m's local `isSome(Int)->Int` wins for a bare call; `std::isSome`
    // resolves to the prelude `isSome[T](Option[T])->Bool`.
    check_int_modules("mod_std_disambiguate",
                      "use m::run\n run()",
                      M{{"m", "fn isSome(n: Int) -> Int { n }\n"
                              "pub fn run() -> Int { let a = isSome(40)\n"
                              "                  let b = std::isSome(Some(1))\n"
                              "                  if b { a + 2 } else { a } }"}},
                      42, svc::builtin_prelude());
    // TRAIT-NAME COEXISTENCE: two modules each define a same-named trait `Show`; each qualifies
    // `Show::show` to its OWN trait (the qualifier resolves to the module's own Show). The two
    // impls (gfx::Show and aud::Show, both for Int) coexist without colliding.
    check_int_modules("mod_trait_coexist",
                      "use gfx::g\n use aud::a\n g(0) + a(0)",
                      M{{"gfx", "trait Show { fn show(self) -> Int }\n"
                                "impl Show for Int { fn show(self) -> Int { 40 } }\n"
                                "pub fn g(x: Int) -> Int { Show::show(x) }"},
                        {"aud", "trait Show { fn show(self) -> Int }\n"
                                "impl Show for Int { fn show(self) -> Int { 2 } }\n"
                                "pub fn a(x: Int) -> Int { Show::show(x) }"}}, 42);

    // --- qualified STRUCT LITERAL / PATTERNS / TYPE ANNOTATION (mod::Name in value/pattern/type) ---
    // A module-qualified struct LITERAL, constructed WITHOUT a `use` (only `import`), + field access.
    check_int_modules("mod_qual_struct_lit",
                      "import geo\n geo::Point { x: 40, y: 2 }.x",
                      M{{"geo", "pub struct Point { x: Int, y: Int }"}}, 40);
    // A qualified variant PATTERN (tuple ctor) + a qualified nullary variant pattern, matched
    // against a qualified constructor value -- none of these needs a `use`.
    check_int_modules("mod_qual_ctor_pattern",
                      "import pal\n match pal::Wrap(42) { pal::Wrap(n) => n, pal::Nil => 0 }",
                      M{{"pal", "pub enum C { Wrap(Int), Nil }"}}, 42);
    check_int_modules("mod_qual_nullary_pattern",
                      "import pal\n match pal::Nil { pal::Wrap(n) => n, pal::Nil => 99 }",
                      M{{"pal", "pub enum C { Wrap(Int), Nil }"}}, 99);
    // A qualified struct PATTERN in a destructuring `let` (irrefutable single-shape struct).
    check_int_modules("mod_qual_struct_pattern",
                      "import geo\n let geo::Point { x, y } = geo::Point { x: 40, y: 2 }\n x + y",
                      M{{"geo", "pub struct Point { x: Int, y: Int }"}}, 42);
    // A qualified TYPE ANNOTATION on a param AND a `let` -- both resolve to geo's struct.
    check_int_modules("mod_qual_type_annotation",
                      "import geo\n fn dist(p: geo::Point) -> Int { p.x + p.y }\n"
                      " let q: geo::Point = geo::Point { x: 40, y: 2 }\n dist(q)",
                      M{{"geo", "pub struct Point { x: Int, y: Int }"}}, 42);
    // std:: reaches the ambient prelude in PATTERN position too (`std::Some` / `std::None`).
    check_int_modules("mod_qual_std_pattern",
                      "match std::Some(42) { std::Some(n) => n, std::None => 0 }",
                      M{}, 42, svc::builtin_prelude());
    // NEGATIVE: a qualified struct literal whose module was never imported -> error.
    check_true("mod_qual_struct_lit_not_imported",
               cg_modules_check_fails("geo::Point { x: 1, y: 2 }.x",
                   M{{"geo", "pub struct Point { x: Int, y: Int }"}}));
    // NEGATIVE: a qualified type of an unimported module -> error.
    check_true("mod_qual_type_not_imported",
               cg_modules_check_fails("fn f(p: geo::Point) -> Int { p.x }\n 1",
                   M{{"geo", "pub struct Point { x: Int, y: Int }"}}));
    // NEGATIVE: `geo::Int` must NOT silently match the primitive `Int` (fast-path gate) -> unknown type.
    check_true("mod_qual_type_not_primitive",
               cg_modules_check_fails("import geo\n fn f(n: geo::Int) -> Int { n }\n 1",
                   M{{"geo", "fn dummy() -> Int { 0 }"}}));

    // --- S4: visibility (DEFAULT PRIVATE; `pub` exports; cross-module only) ---
    // A `pub` fn is reachable cross-module; its private sibling is not.
    check_int_modules("vis_pub_cross_ok",
                      "use lib::exposed\n exposed()",
                      M{{"lib", "fn hidden() -> Int { 1 }\n pub fn exposed() -> Int { 42 }"}}, 42);
    // A private fn is NOT reachable via an explicit `use` -> hard error ("item is private").
    check_true("vis_private_use_rejected",
               cg_modules_check_fails("use lib::hidden\n hidden()",
                   M{{"lib", "fn hidden() -> Int { 1 }"}}));
    // A private fn is NOT reachable via a qualified call -> hard error.
    check_true("vis_private_qual_rejected",
               cg_modules_check_fails("import lib\n lib::hidden()",
                   M{{"lib", "fn hidden() -> Int { 1 }"}}));
    // A private TYPE is NOT reachable via a qualified type annotation -> hard error.
    check_true("vis_private_type_rejected",
               cg_modules_check_fails("import lib\n fn f(p: lib::Secret) -> Int { p.x }\n 1",
                   M{{"lib", "struct Secret { x: Int }"}}));
    // A glob SKIPS a private name (Rust): only the `pub` one is brought in.
    check_int_modules("vis_glob_skips_private",
                      "use lib::*\n shown()",
                      M{{"lib", "fn hidden() -> Int { 1 }\n pub fn shown() -> Int { 42 }"}}, 42);
    check_true("vis_glob_private_unreachable",
               cg_modules_check_fails("use lib::*\n hidden()",
                   M{{"lib", "fn hidden() -> Int { 7 }\n pub fn shown() -> Int { 1 }"}}));
    // Same-module access to a private item is ALWAYS allowed (visibility is cross-module only).
    check_int_modules("vis_same_module_private_ok",
                      "use lib::entry\n entry()",
                      M{{"lib", "fn secret() -> Int { 42 }\n pub fn entry() -> Int { secret() }"}}, 42);
    // A `pub` enum's VARIANTS inherit its visibility (a qualified ctor works cross-module).
    check_int_modules("vis_pub_enum_variant",
                      "import pal\n use pal::unwrapc\n unwrapc(pal::Wrap(42))",
                      M{{"pal", "pub enum C { Wrap(Int), Nil }\n"
                                "pub fn unwrapc(c: C) -> Int { match c { Wrap(n) => n, Nil => 0 } }"}}, 42);
    // Parser: `pub` may only precede a fn / struct / enum / trait.
    check_true("vis_pub_on_let_parse_error",  parse_throws("pub let x = 1"));
    check_true("vis_pub_on_impl_parse_error", parse_throws("pub impl T for Int { fn m(self) -> Int { 0 } }"));
    check_true("vis_pub_on_use_parse_error",  parse_throws("pub use m::x"));
    // Parser: a `pub fn` / `pub struct` parses cleanly (no dump change -- is_pub is not rendered).
    check_str("vis_pub_fn_parses", prog_dump("pub fn f() -> Int { 1 }"), "(fn-item f () -> Int (block 1))");
}

void test_codegen_core() {
    std::cout << "[codegen: core P1]\n";

    // Int arithmetic (typed *_INT path) + precedence.
    check_int("int_add",        "1 + 2", 3);
    check_int("int_prec",       "7 - 3 * 2", 1);
    check_int("int_div_trunc",  "20 / 3", 6);
    check_int("int_mod",        "17 % 5", 2);
    check_int("unary_neg",      "-5 + 8", 3);
    check_int("let_use",        "let x = 40\n x + 2", 42);

    // Bitwise / shifts (Int-only).
    check_int("bit_and",  "6 & 3", 2);
    check_int("bit_or",   "6 | 1", 7);
    check_int("bit_xor",  "5 ^ 1", 4);
    check_int("shl",      "1 << 4", 16);
    check_int("shr_arith","-8 >> 1", -4);
    check_int("bit_not",  "~0", -1);

    // Comparisons (typed int -> Bool).
    check_bool("lt",  "1 < 2", true);
    check_bool("le",  "5 <= 5", true);
    check_bool("gt",  "3 > 10", false);
    check_bool("ge",  "7 >= 8", false);
    check_bool("eq",  "2 == 2", true);
    check_bool("ne",  "2 != 3", true);

    // Structural `==` / `!=` (the EQ_DEEP opcode): two SEPARATELY-built equal composites compare
    // EQUAL where reference-identity `==` would say false. Covers struct / nested struct / tuple /
    // enum (nullary + payload) / Vec / Array / Map (order-independent) / Bytes, plus the famous
    // `None == None` / `opt == None` cases that were silently always-false before.
    check_bool_p("eq_struct_true",   "struct P{x:Int,y:Int}\n let a=P{x:1,y:2}\n let b=P{x:1,y:2}\n a==b", true);
    check_bool_p("eq_struct_false",  "struct P{x:Int,y:Int}\n let a=P{x:1,y:2}\n let b=P{x:1,y:3}\n a==b", false);
    check_bool_p("eq_struct_ne",     "struct P{x:Int,y:Int}\n let a=P{x:1,y:2}\n let b=P{x:1,y:3}\n a!=b", true);
    check_bool_p("eq_struct_nested", "struct P{x:Int,y:Int}\n struct L{a:P,b:P}\n"
                                     " let u=L{a:P{x:1,y:2},b:P{x:3,y:4}}\n let v=L{a:P{x:1,y:2},b:P{x:3,y:4}}\n u==v", true);
    check_bool_p("eq_tuple_true",    "let a=(1,\"hi\",true)\n let b=(1,\"hi\",true)\n a==b", true);
    check_bool_p("eq_tuple_false",   "let a=(1,\"hi\",true)\n let b=(1,\"bye\",true)\n a==b", false);
    check_bool_p("eq_enum_nullary",  "enum Dir{North,East,South}\n North==North", true);
    check_bool_p("eq_enum_nullary_f","enum Dir{North,East,South}\n North==East", false);
    check_bool_p("eq_some_true",     "let a:Option[Int]=Some(1)\n let b:Option[Int]=Some(1)\n a==b", true);
    check_bool_p("eq_some_false",    "let a:Option[Int]=Some(1)\n let b:Option[Int]=Some(2)\n a==b", false);
    check_bool_p("eq_none_none",     "let n:Option[Int]=None\n n==None", true);          // was always false
    check_bool_p("eq_some_none",     "let a:Option[Int]=Some(1)\n a==None", false);       // was always false
    check_bool_p("eq_result",        "let a:Result[Int,String]=Ok(1)\n let b:Result[Int,String]=Ok(1)\n a==b", true);
    check_bool_p("eq_result_err",    "let a:Result[Int,String]=Ok(1)\n let b:Result[Int,String]=Err(\"x\")\n a==b", false);
    check_bool_p("eq_vec_true",      "toVec([1,2,3])==toVec([1,2,3])", true);
    check_bool_p("eq_vec_false",     "toVec([1,2,3])==toVec([1,2,4])", false);
    check_bool_p("eq_vec_len",       "toVec([1,2])==toVec([1,2,3])", false);
    check_bool_p("eq_array_true",    "[1,2,3]==[1,2,3]", true);
    check_bool_p("eq_bytes_true",    "toBytes(\"AB\")==toBytes(\"AB\")", true);
    check_bool_p("eq_bytes_false",   "toBytes(\"AB\")==toBytes(\"AC\")", false);
    check_bool_p("eq_map_order",     "let mut a:Map[String,Int]=#{}\n a[\"x\"]=1\n a[\"y\"]=2\n"
                                     " let mut b:Map[String,Int]=#{}\n b[\"y\"]=2\n b[\"x\"]=1\n a==b", true);
    check_bool_p("eq_map_val",       "let mut a:Map[String,Int]=#{}\n a[\"x\"]=1\n"
                                     " let mut b:Map[String,Int]=#{}\n b[\"x\"]=9\n a==b", false);
    check_bool_p("eq_vec_of_struct", "struct P{x:Int}\n toVec([P{x:1},P{x:2}])==toVec([P{x:1},P{x:2}])", true);
    check_bool_p("eq_opt_struct",    "struct P{x:Int}\n let a:Option[P]=Some(P{x:5})\n let b:Option[P]=Some(P{x:5})\n a==b", true);
    check_bool_p("eq_contains",      "struct P{x:Int}\n contains(intoIter(toVec([P{x:1},P{x:2}])), P{x:2})", true);
    // A function-carrying type has no structural equality: `==` on it is a COMPILE error.
    check_true("eq_fn_field_rejected", check_has(
        "struct Box{f:fn(Int)->Int}\n let a=Box{f:fn(x:Int)->Int{x}}\n let b=Box{f:fn(x:Int)->Int{x}}\n a==b",
        "does not support"));
    check_true("eq_fn_value_rejected", check_has(
        "let f=fn(x:Int)->Int{x}\n let g=fn(x:Int)->Int{x}\n f==g", "does not support"));
    check_true("eq_unbounded_param_rejected", check_has(
        "fn same2[T](a:T,b:T)->Bool{ a==b }\n 0", "does not support"));
    // A BOUNDED generic `T: Eq` makes `==` well-typed; the ONE erased body dispatches structurally at
    // run time (EQ_DEEP on a type-parameter operand) -- correct for a struct, an Int leaf, and a Vec.
    check_bool_p("eq_generic_struct", "struct P{x:Int}\n fn eqp[T: Eq](a:T,b:T)->Bool{a==b}\n"
                                      " eqp(P{x:1}, P{x:1})", true);
    check_bool_p("eq_generic_struct_f","struct P{x:Int}\n fn eqp[T: Eq](a:T,b:T)->Bool{a==b}\n"
                                      " eqp(P{x:1}, P{x:2})", false);
    check_bool_p("eq_generic_int",    "fn eqp[T: Eq](a:T,b:T)->Bool{a==b}\n eqp(5, 5)", true);
    check_bool_p("eq_generic_vec",    "fn eqp[T: Eq](a:T,b:T)->Bool{a==b}\n eqp(toVec([1,2]), toVec([1,2]))", true);
    // D4 (NaN): `Double` is an Eq leaf with IEEE `NaN != NaN`, so a NaN-containing composite inherits it
    // -- a struct/Vec with a NaN field is NOT equal to a structurally-equal COPY. The same-pointer fast
    // path still makes a value equal to ITSELF (aliased). Locked so nobody "fixes" it later (route A).
    check_bool_p("eq_nan_scalar", "let n = 0.0 / 0.0\n n == n", false);              // IEEE
    check_bool_p("eq_nan_self",   "struct P{x:Double,y:Double}\n let n=0.0/0.0\n let p=P{x:n,y:1.0}\n p==p", true);   // aliased -> same-object short-circuit
    check_bool_p("eq_nan_copy",   "struct P{x:Double,y:Double}\n let n=0.0/0.0\n P{x:n,y:1.0}==P{x:n,y:1.0}", false); // distinct copies -> NaN field
    check_bool_p("eq_nan_vec",    "let n=0.0/0.0\n toVec([n])==toVec([n])", false);  // distinct Vecs -> NaN element
    // The sealed built-in Eq marker + `dyn Eq` rejection need the real std::core::Eq (prelude).
    check_true("eq_user_impl_sealed", check_has_p(
        "struct P{x:Int}\n impl Eq for P {}\n 0", "cannot implement the built-in 'Eq'"));
    check_true("eq_dyn_rejected", check_has_p(
        "fn f(x: dyn Eq) -> Int { 0 }\n 0", "compile-time marker"));
    // `dyn Hashable` is likewise rejected (a methodless marker, nothing to dispatch) -- consistent with dyn Eq.
    check_true("hashable_dyn_rejected", check_has_p(
        "fn f(x: dyn Hashable) -> Int { 0 }\n 0", "compile-time marker"));
    // Hashable is SEALED: a user `impl Hashable for X` (which would defeat the map-key gate -> a
    // checker-accepts/runtime-trap hole) is rejected at compile time, like the Eq seal above.
    check_true("hashable_user_impl_sealed", check_has_p(
        "struct P{x:Int}\n impl Hashable for P {}\n 0", "cannot implement the built-in 'Hashable'"));

    // Bool logic + short-circuit.
    check_bool("and",         "true && false", false);
    check_bool("or",          "true || false", true);
    check_bool("not",         "!false", true);
    check_bool("short_and",   "let x = 5\n x > 0 && x < 10", true);
    check_bool("short_or",    "let x = 5\n x > 100 || x < 10", true);

    // if / else as an expression.
    check_int("if_then", "if 1 < 2 { 10 } else { 20 }", 10);
    check_int("if_else", "if 3 < 2 { 10 } else { 20 }", 20);

    // while + mutation.
    check_int("while_sum",
        "let mut i = 0\n let mut s = 0\n while i < 5 { s = s + i\n i = i + 1 }\n s", 10);
    check_int("mut_reassign", "let mut x = 1\n x = x + 41\n x", 42);

    // block as an expression (nested scope, reclaimed locals).
    check_int("block_value", "let y = { let a = 2\n a * 3 }\n y + 1", 7);

    // Double arithmetic (promoting path) + comparisons.
    check_dbl("dbl_add", "1.5 + 2.0", 3.5);
    check_dbl("dbl_div", "10.0 / 4.0", 2.5);
    check_bool("dbl_lt", "1.5 < 2.0", true);

    // I2D coercion at the N1 boundaries.
    check_dbl("coerce_let_annot", "let d: Double = 3\n d + 0.5", 3.5);   // runtime Int -> Double at the let
    check_dbl("coerce_mixed_op",  "let n = 3\n n + 0.5", 3.5);           // Int operand widened at `+`
    check_dbl("coerce_from_var",  "let a = 5\n let b: Double = a\n b", 5.0);

    // Type errors are rejected BEFORE codegen (CheckFailure, no bytecode).
    check_true("check_rejects_bad_add", cg_check_fails("1 + true"));

    // impl for Vec/Bytes now lowers (TID_VEC/TID_BYTES dense columns) and blanket impls now lower
    // (bound-satisfying types' columns filled) -- see test_codegen_traits (devirt_vec / blanket_*).
}

void test_codegen_functions() {
    std::cout << "[codegen: functions & TCO P2]\n";

    // Direct call.
    check_int("call_direct",
        "fn add(a: Int, b: Int) -> Int { a + b }\n add(2, 3)", 5);
    // Non-tail recursion (factorial).
    check_int("recursion",
        "fn fact(n: Int) -> Int { if n <= 1 { 1 } else { n * fact(n - 1) } }\n fact(5)", 120);
    // Self-tail recursion: 200000 deep -> only correct (and non-overflowing) if TCO'd.
    check_int("self_tco_deep",
        "fn sum(n: Int, acc: Int) -> Int { if n == 0 { acc } else { sum(n - 1, acc + n) } }\n"
        " sum(200000, 0)", 20000100000LL);
    // Direct mutual tail recursion (isEven/isOdd), 100000 deep -> TCO_CALL <label>.
    check_bool("mutual_tco_deep",
        "fn isEven(n: Int) -> Bool { if n == 0 { true } else { isOdd(n - 1) } }\n"
        "fn isOdd(n: Int) -> Bool { if n == 0 { false } else { isEven(n - 1) } }\n"
        " isEven(100000)", true);

    // Pipe (desugars lhs as arg 0).
    check_int("pipe_bare",  "fn dbl(x: Int) -> Int { x * 2 }\n 21 |> dbl", 42);
    check_int("pipe_args",  "fn ad(a: Int, b: Int) -> Int { a + b }\n 40 |> ad(2)", 42);

    // First-class function value + indirect call.
    check_int("first_class_fn",
        "fn inc(x: Int) -> Int { x + 1 }\n"
        "fn apply(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n apply(inc, 41)", 42);
    check_int("higher_order_twice",
        "fn inc(x: Int) -> Int { x + 1 }\n"
        "fn twice(f: fn(Int) -> Int, x: Int) -> Int { f(f(x)) }\n twice(inc, 40)", 42);

    // Early `return` (in a non-tail statement).
    check_int("early_return_yes",
        "fn f(x: Int) -> Int { if x > 0 { return 1 }\n 2 }\n f(5)", 1);
    check_int("early_return_no",
        "fn f(x: Int) -> Int { if x > 0 { return 1 }\n 2 }\n f(-3)", 2);

    // Generics are ERASED -> one monomorphic body, transparent to codegen.
    check_int("generic_id", "fn id[T](x: T) -> T { x }\n id(42)", 42);

    // Int->Double return-boundary coercion (fn returns Double, tail value is Int).
    check_dbl("ret_coerce", "fn half() -> Double { 3 }\n half() + 0.5", 3.5);

    // A unit-returning fn called for effect, then a value.
    check_int("unit_fn", "fn noop() -> () { }\n noop()\n 5", 5);

    // A lambda value now lowers (P6) -> it runs (see test_codegen_closures).
    check_int("lambda_basic", "let f = fn(x: Int) -> Int { x + 1 }\n f(41)", 42);
}

void test_codegen_structs() {
    std::cout << "[codegen: structs/tuples/enums P3a]\n";

    // Record struct: literal + static-slot field access.
    check_int("struct_fields",
        "struct P { x: Int, y: Int }\n let p = P { x: 3, y: 4 }\n p.x + p.y", 7);
    // Field slots resolve by NAME, not literal order.
    check_int("struct_reorder",
        "struct P { x: Int, y: Int }\n let p = P { y: 4, x: 3 }\n p.x", 3);
    // Mutable field assignment.
    check_int("struct_field_assign",
        "struct P { x: Int, y: Int }\n let mut p = P { x: 1, y: 2 }\n p.x = 40\n p.x + p.y", 42);
    // Record update: an override changes one field; the others are COPIED from the base.
    check_int("record_update_override",
        "struct P { x: Int, y: Int }\n let p = P { x: 3, y: 4 }\n let q = P { x: 10, ..p }\n q.x + q.y", 14);
    check_int("record_update_copy_all",
        "struct P { x: Int, y: Int }\n let p = P { x: 3, y: 4 }\n let q = P { ..p }\n q.x + q.y", 7);
    check_int("record_update_multi",
        "struct P { x: Int, y: Int }\n let p = P { x: 3, y: 4 }\n let q = P { y: 100, x: 5, ..p }\n q.x + q.y", 105);
    // The base is an EXPRESSION, evaluated once (a fresh P from a call).
    check_int("record_update_base_expr",
        "struct P { x: Int, y: Int }\n fn mk() -> P { P { x: 1, y: 2 } }\n let q = P { y: 9, ..mk() }\n q.x + q.y", 10);
    // An Int override into a Double field is widened (field_double / I2D path); the Int field is copied.
    check_int("record_update_double_field",
        "struct D { a: Double, b: Int }\n let d = D { a: 1.5, b: 2 }\n let e = D { a: 9, ..d }\n toInt(e.a) + e.b", 11);
    // Field access through a fn return (the static-type win: `mk(..).x` resolves the slot).
    check_int("field_through_call",
        "struct P { x: Int, y: Int }\n fn mk(a: Int) -> P { P { x: a, y: a } }\n"
        " mk(21).x + mk(21).y", 42);
    // A struct passed as a parameter, field read in the callee.
    check_int("struct_param",
        "struct P { x: Int, y: Int }\n fn sx(p: P) -> Int { p.x }\n sx(P { x: 9, y: 0 })", 9);
    // A Double field initialized with an Int -> I2D at the field boundary.
    check_dbl("struct_double_field",
        "struct V { d: Double }\n let v = V { d: 3 }\n v.d + 0.5", 3.5);
    // Generic struct -> one erased body; field access transparent.
    check_int("generic_struct",
        "struct Box[T] { value: T }\n let b = Box { value: 42 }\n b.value", 42);

    // Anonymous tuples: literal + `t.N` positional access.
    check_int("tuple_index", "let t = (10, 20, 30)\n t.0 + t.2", 40);
    check_int("tuple_through_fn",
        "fn pair(a: Int, b: Int) -> (Int, Int) { (a, b) }\n let t = pair(40, 2)\n t.0 + t.1", 42);

    // Tuple destructuring-`let` (irrefutable): reads slots into fresh locals, no discrimination.
    check_int("let_destr_tuple",   "let (a, b) = (5, 6)\n a + b", 11);
    check_int("let_destr_wild",    "let (a, _) = (7, 99)\n a", 7);
    check_int("let_destr_nested",  "let ((a, b), c) = ((1, 2), 3)\n a + b + c", 6);
    check_int("let_destr_mut",     "let mut (a, b) = (1, 2)\n a = a + 40\n a + b", 43);
    check_int("let_destr_from_fn",
        "fn pair(a: Int, b: Int) -> (Int, Int) { (a, b) }\n let (x, y) = pair(40, 2)\n x + y", 42);
    // Bare-ident init (`init.temp == 0`): the binder still needs a read temp -- the frame-sizing
    // path (`1 + test_temps`) that the emitter self-check would otherwise flag.
    check_int("let_destr_bare_init", "let t = ((4, 5), 6)\n let ((a, b), c) = t\n a + b + c", 15);

    // Record-struct + tuple-struct destructuring-`let` (irrefutable -- one shape).
    check_int("let_destr_struct",
        "struct P { x: Int, y: Int }\n let P { x, y } = P { x: 3, y: 4 }\n x + y", 7);
    check_int("let_destr_struct_rename",
        "struct P { x: Int, y: Int }\n let P { x: a, y: b } = P { x: 40, y: 2 }\n a + b", 42);
    check_int("let_destr_tuple_struct",
        "struct Pair(Int, Int)\n let Pair(a, b) = Pair(5, 6)\n a * b", 30);
    check_int("let_destr_struct_nested",
        "struct P { x: Int, y: Int }\n struct Line { a: P, b: P }\n"
        " let Line { a: P { x, y }, b } = Line { a: P { x: 1, y: 2 }, b: P { x: 3, y: 4 } }\n"
        " x + y + b.x + b.y", 10);
    check_int("let_destr_generic_struct",
        "struct Box[T] { value: T }\n let Box { value } = Box { value: 42 }\n value", 42);
    check_int("let_destr_struct_mut",
        "struct P { x: Int }\n let mut P { x } = P { x: 1 }\n x = x + 41\n x", 42);
    // Refutable patterns stay `match`-only; a type mismatch is rejected.
    check_true("let_refut_variant",
        cg_check_fails("enum Opt { Non, Som(Int) }\n let Som(x) = Som(5)\n x"));
    check_true("let_refut_list",     cg_check_fails("let [a, b] = [1, 2]\n a"));
    check_true("let_refut_map",      cg_check_fails("let #{ 1 => v } = #{ 1 => 10 }\n v"));
    check_true("let_destr_mismatch",
        cg_check_fails("struct P { x: Int }\n struct Q { y: Int }\n let Q { y } = P { x: 1 }\n y"));

    // Enum + tuple-struct CONSTRUCTION compiles and runs (payload reading needs `match` -> P4).
    check_int("enum_construct",
        "enum Opt { Non, Som(Int) }\n let a = Som(7)\n let b = Non\n 1", 1);
    check_int("tuple_struct_construct",
        "struct Pair(Int, Int)\n let p = Pair(1, 2)\n 5", 5);

    // Record-style enum variants `V { f: T }`: brace construction + match with named-field extraction.
    check_int("rvariant_field",
        "enum Shape { Dot, Circle { r: Int } }\n"
        " let s = Circle { r: 7 }\n match s { Dot => 0, Circle { r } => r }", 7);
    check_int("rvariant_two_fields",
        "enum Shape { Rect { w: Int, h: Int } }\n"
        " let s = Rect { w: 6, h: 7 }\n match s { Rect { w, h } => w * h }", 42);
    check_int("rvariant_field_reorder",           // field slots resolve by NAME, not literal order
        "enum Shape { Rect { w: Int, h: Int } }\n"
        " let s = Rect { h: 7, w: 6 }\n match s { Rect { w, h } => w - h }", -1);
    check_int("rvariant_rename_bind",
        "enum Shape { Circle { r: Int } }\n"
        " let s = Circle { r: 9 }\n match s { Circle { r: n } => n + 1 }", 10);
    check_int("rvariant_mixed",                   // nullary + record + tuple variants in one enum
        "enum Shape { Dot, Circle { r: Int }, Seg(Int, Int) }\n"
        " fn area(s: Shape) -> Int { match s { Dot => 0, Circle { r } => r * r, Seg(a, b) => a + b } }\n"
        " area(Circle { r: 5 }) + area(Seg(3, 4)) + area(Dot)", 32);   // 25 + 7 + 0
    check_int("rvariant_generic",                 // T solved from the field value
        "enum Tree[T] { Leaf { val: T }, Node(Tree[T], Tree[T]) }\n"
        " let t = Leaf { val: 42 }\n match t { Leaf { val } => val, Node(a, b) => 0 }", 42);

    // Field access through a TYPE-ERASING call. infer() stores the APPLIED Expr::ty, so the
    // result of a generic / higher-order / unwrap call carries a concrete struct type and .field
    // resolves its slot statically. These once hit "the receiver's struct type is not statically
    // known" -- they are the regression net for that (now-closed) codegen path.
    check_int("field_through_generic",   // generic id erased to one body; call result is concrete P
        "struct P { x: Int, y: Int }\n fn idf[T](v: T) -> T { v }\n"
        " idf(P { x: 30, y: 12 }).x + idf(P { x: 1, y: 99 }).y", 129);
    check_int("field_through_hof",        // return type B is OUTPUT-solved from the fn argument
        "struct P { x: Int, y: Int }\n fn ap1[A, B](f: fn(A) -> B, a: A) -> B { f(a) }\n"
        " fn mk(n: Int) -> P { P { x: n, y: 0 } }\n ap1(mk, 42).x", 42);
    check_int("field_assign_through_generic",  // the assign lvalue path resolves the slot too
        "struct P { x: Int }\n fn idf[T](v: T) -> T { v }\n"
        " let mut p = idf(P { x: 1 })\n p.x = 42\n p.x", 42);

    // Chained place-path lvalues `a[i].field = v` and friends. The base compiles as an RVALUE
    // (a heap reference), and the final SET_PROP/ARRAY_SET/MAP_SET mutates it in place -- no new
    // opcode, no chain-specific codegen. M5 requires the ROOT binding `mut` (negatives below).
    check_int("chain_vec_field",         // Vec[Point]: index-then-field
        "struct P { x: Int, y: Int }\n let mut v = vec()\n"
        " push(v, P { x: 1, y: 2 })\n push(v, P { x: 3, y: 4 })\n"
        " v[0].x = 40\n v[0].x + v[1].y", 44);
    check_int("chain_array_field",       // Array[Point]: index-then-field (distinct elements)
        "struct P { x: Int, y: Int }\n let mut a = array(2, P { x: 0, y: 0 })\n"
        " a[0] = P { x: 5, y: 6 }\n a[1] = P { x: 7, y: 8 }\n"
        " a[0].x = 50\n a[0].x + a[1].y", 58);
    check_int("chain_nested_field",      // struct-in-struct: field-then-field
        "struct P { x: Int, y: Int }\n struct N { p: P }\n"
        " let mut n = N { p: P { x: 9, y: 10 } }\n n.p.x = 60\n n.p.x + n.p.y", 70);
    check_int("chain_map_field",         // Map[Int, Point]: map-index-then-field (key pre-exists)
        "struct P { x: Int, y: Int }\n let mut m = #{ 1 => P { x: 11, y: 12 } }\n"
        " m[1].x = 70\n m[1].x + m[1].y", 82);
    check_int("chain_index_index",       // Vec[Vec[Int]]: index-then-index
        "let mut vv = vec()\n let mut r = vec()\n push(r, 100)\n push(r, 200)\n push(vv, r)\n"
        " vv[0][1] = 300\n vv[0][0] + vv[0][1]", 400);
    check_int("chain_field_index",       // struct.field[i]: field-then-index
        "struct H { xs: Vec[Int] }\n let mut v = vec()\n push(v, 7)\n push(v, 8)\n"
        " let mut h = H { xs: v }\n h.xs[0] = 500\n h.xs[0] + h.xs[1]", 508);
}

void test_codegen_containers() {
    std::cout << "[codegen: containers/for P3b]\n";

    // List literal + `for` (cons-spine walk, static List type -> no GET_KIND).
    check_int("list_for_sum",
        "let xs = [1, 2, 3, 4]\n let mut s = 0\n for x in xs { s = s + x }\n s", 10);
    // Empty list (annotated) iterates zero times.
    check_int("list_empty",
        "let xs: List[Int] = []\n let mut s = 0\n for x in xs { s = s + x }\n s", 0);
    // break / continue inside a `for`.
    check_int("for_break",
        "let xs = [1, 2, 3, 4, 5]\n let mut s = 0\n for x in xs { if x == 3 { break }\n s = s + x }\n s", 3);
    check_int("for_continue",
        "let xs = [1, 2, 3, 4]\n let mut s = 0\n for x in xs { if x == 2 { continue }\n s = s + x }\n s", 8);
    // A list of tuples, destructured in the for-pattern.
    check_int("for_tuple_pat",
        "let ps = [(1, 2), (3, 4)]\n let mut s = 0\n for (a, b) in ps { s = s + a * b }\n s", 14);
    // Nested / struct / tuple-struct for-patterns (the irrefutable binder, same as destructuring-let).
    check_int("for_nested_tuple",
        "let ps = [((1, 2), 3), ((4, 5), 6)]\n let mut s = 0\n"
        " for ((a, b), c) in ps { s = s + a + b + c }\n s", 21);
    check_int("for_struct_pat",
        "struct P { x: Int, y: Int }\n let ps = [P { x: 1, y: 2 }, P { x: 3, y: 4 }]\n let mut s = 0\n"
        " for P { x, y } in ps { s = s + x + y }\n s", 10);
    check_int("for_struct_rename_wild",
        "struct P { x: Int, y: Int }\n let ps = [P { x: 7, y: 99 }, P { x: 5, y: 88 }]\n let mut s = 0\n"
        " for P { x: a, y: _ } in ps { s = s + a }\n s", 12);
    check_int("for_tuple_struct_pat",
        "struct Pair(Int, Int)\n let ps = [Pair(1, 2), Pair(3, 4)]\n let mut s = 0\n"
        " for Pair(a, b) in ps { s = s + a * b }\n s", 14);
    check_int("for_struct_over_array",
        "struct P { x: Int, y: Int }\n let a = array(2, P { x: 3, y: 4 })\n let mut s = 0\n"
        " for P { x, y } in a { s = s + x + y }\n s", 14);
    // A struct-in-tuple for-pattern over a map's (k, v) pairs is a nested destructure.
    check_int("for_map_nested",
        "let m = #{ 1 => 10, 2 => 20 }\n let mut s = 0\n for (k, v) in m { s = s + k * v }\n s", 50);
    // Nested loops.
    check_int("nested_for",
        "let xs = [1, 2]\n let ys = [10, 20]\n let mut s = 0\n"
        " for x in xs { for y in ys { s = s + x * y } }\n s", 90);

    // Map literal + `for (k, v)`.
    check_int("map_for_kv",
        "let m = #{1 => 10, 2 => 20}\n let mut s = 0\n for (k, v) in m { s = s + k + v }\n s", 33);
    // Map `for pair` (single var bound to a (K, V) tuple).
    check_int("map_for_pair",
        "let m = #{5 => 100}\n let mut s = 0\n for p in m { s = s + p.0 + p.1 }\n s", 105);

    // `while` + `break` (this also completes while's break/continue, unreachable before P3b).
    check_int("while_break",
        "let mut i = 0\n while true { if i == 5 { break }\n i = i + 1 }\n i", 5);
    check_int("while_continue",
        "let mut i = 0\n let mut s = 0\n while i < 5 { i = i + 1\n if i == 3 { continue }\n s = s + i }\n s",
        12);   // 1+2+4+5
}

void test_codegen_match() {
    std::cout << "[codegen: match P4]\n";

    // Enum variant match + PAYLOAD extraction (the headline P4 win).
    check_int("match_enum_some",
        "enum Opt { Non, Som(Int) }\n let o = Som(7)\n match o { Som(x) => x, Non => 0 }", 7);
    check_int("match_enum_none",
        "enum Opt { Non, Som(Int) }\n let o = Non\n match o { Som(x) => x, Non => 0 }", 0);
    check_int("match_result",
        "enum R { Ok(Int), Err(Int) }\n let r = Ok(42)\n match r { Ok(v) => v, Err(e) => 0 - e }", 42);

    // Literal patterns + wildcard + guard.
    check_int("match_literal", "let n = 2\n match n { 1 => 10, 2 => 20, _ => 0 }", 20);
    check_int("match_guard",   "let n = 5\n match n { x if x > 3 => 1, _ => 0 }", 1);
    check_int("match_bool",    "let b = true\n match b { true => 1, false => 0 }", 1);

    // Tuple + struct destructuring in match.
    check_int("match_tuple",  "let t = (3, 4)\n match t { (a, b) => a * b }", 12);
    check_int("match_struct",
        "struct P { x: Int, y: Int }\n let p = P { x: 3, y: 4 }\n match p { P { x, y } => x + y }", 7);
    check_int("match_tuple_literal", "match (1, 2) { (1, b) => b, _ => 0 }", 2);

    // Nested match.
    check_int("match_nested",
        "enum Opt { Non, Som(Int) }\n let o = Som(9)\n"
        " match o { Som(x) => match x { 9 => 100, _ => 0 }, Non => -1 }", 100);

    // List patterns (`[]` / `[h, ..t]`) + recursion through match.
    check_int("match_list_sum",
        "fn sum(l: List[Int]) -> Int { match l { [] => 0, [h, ..t] => h + sum(t) } }\n sum([1, 2, 3, 4])", 10);
    check_int("match_list_len",
        "fn len(l: List[Int]) -> Int { match l { [] => 0, [_, ..t] => 1 + len(t) } }\n len([9, 8, 7, 6, 5])", 5);

    // Tail call inside a match arm (compile_tail_match + self-TCO): accumulator count.
    check_int("match_tail_tco",
        "fn count(l: List[Int], acc: Int) -> Int { match l { [] => acc, [_, ..t] => count(t, acc + 1) } }\n"
        " count([1, 2, 3, 4, 5, 6, 7], 0)", 7);
}

void test_codegen_traits() {
    std::cout << "[codegen: traits + devirtualization P5]\n";

    // Common trait+impl prefix reused across the concrete-receiver (devirtualized) cases.
    const std::string area =
        "trait Area { fn area(self) -> Int }\n"
        "struct Sq { s: Int }\n"
        "impl Area for Sq { fn area(self) -> Int { self.s * self.s } }\n";

    // Devirtualization: a concrete receiver -> a direct CALL of the impl fn (no dynamic dispatch).
    check_int("devirt_struct", area + "area(Sq { s: 5 })", 25);
    check_int("devirt_pipe",   area + "Sq { s: 6 } |> area", 36);
    check_int("devirt_qualified", area + "Area::area(Sq { s: 7 })", 49);

    // impl for a primitive type (Int) -> devirtualized on an Int receiver.
    check_int("devirt_primitive",
        "trait Twice { fn twice(self) -> Int }\n"
        "impl Twice for Int { fn twice(self) -> Int { self + self } }\n twice(21)", 42);

    // impl for an enum: the receiver's static type is the enum, the impl body matches the variant.
    check_int("devirt_enum",
        "enum Shape { Circle(Int), Rect(Int, Int) }\n"
        "trait Area { fn area(self) -> Int }\n"
        "impl Area for Shape { fn area(self) -> Int { match self { Circle(r) => r * r, Rect(w, h) => w * h } } }\n"
        "area(Rect(3, 4))", 12);
    check_int("devirt_enum_circle",
        "enum Shape { Circle(Int), Rect(Int, Int) }\n"
        "trait Area { fn area(self) -> Int }\n"
        "impl Area for Shape { fn area(self) -> Int { match self { Circle(r) => r * r, Rect(w, h) => w * h } } }\n"
        "area(Circle(5))", 25);

    // Dynamic dispatch: a generic-bounded receiver (T: Area) is erased to one body -> the fused RESOLVE_CALL.
    // Non-tail dispatch (area(x) is an operand of +).
    check_int("dynamic_nontail",
        area + "fn describe[T: Area](x: T) -> Int { area(x) + 1 }\n describe(Sq { s: 8 })", 65);
    // Tail dispatch (area(x) is the whole body) -> TCO_CALL_INDIRECT.
    check_int("dynamic_tail",
        area + "fn viaArea[T: Area](x: T) -> Int { area(x) }\n viaArea(Sq { s: 9 })", 81);

    // A default method: `greet` uses the trait default, which calls `base(self)` -- resolved
    // dynamically on Self (a Var) even though `greet` itself devirtualizes on the concrete P.
    check_int("default_method",
        "trait Greet { fn base(self) -> Int  fn greet(self) -> Int { base(self) + 100 } }\n"
        "struct P { v: Int }\n"
        "impl Greet for P { fn base(self) -> Int { self.v } }\n greet(P { v: 5 })", 105);

    // A self-recursive trait method in tail position -> devirtualized TCO_CALL (O(1) stack):
    // 200000 deep only terminates without overflow if the tail call reuses the frame.
    check_int("trait_tail_tco_deep",
        "struct Counter { id: Int }\n"
        "trait Down { fn run(self, n: Int) -> Int }\n"
        "impl Down for Counter { fn run(self, n: Int) -> Int { if n == 0 { 0 } else { run(self, n - 1) } } }\n"
        "run(Counter { id: 0 }, 200000)", 0);

    // impl for the growable containers Vec / Bytes -> a dense trait-table column (TID_VEC / TID_BYTES).
    const char* vshow = "trait Show { fn shw(self) -> Int }\n"
                        "impl Show for Vec[Int] { fn shw(self) -> Int { len(self) * 100 } }\n";
    // Concrete Vec receiver -> DEVIRTUALIZED direct CALL of the impl fn.
    check_int("devirt_vec",
        std::string(vshow) + "let mut v = vec()\n push(v, 1)\n push(v, 2)\n push(v, 3)\n shw(v)", 300);
    // Generic `T: Show` receiver bound to a Vec -> DYNAMIC dispatch, RESOLVE_CALL (TID_VEC column).
    check_int("dynamic_vec",
        std::string(vshow) + "fn d[T: Show](x: T) -> Int { shw(x) }\n"
        " let mut v = vec()\n push(v, 5)\n push(v, 6)\n d(v)", 200);
    // impl for Bytes (TID_BYTES column).
    check_int("devirt_bytes",
        "trait Len { fn lng(self) -> Int }\n"
        "impl Len for Bytes { fn lng(self) -> Int { len(self) + 1 } }\n lng(toBytes(\"ABC\"))", 4);

    // Blanket impl `impl[T: Show] Dbg for T`: every Show type gains Dbg through the shared body.
    const char* bl = "trait Show { fn shw(self) -> Int }\n trait Dbg { fn dbg(self) -> Int }\n"
                     "impl[T: Show] Dbg for T { fn dbg(self) -> Int { shw(self) + 1 } }\n"
                     "struct P { v: Int }\n impl Show for P { fn shw(self) -> Int { self.v } }\n"
                     "impl Show for Int { fn shw(self) -> Int { self * 10 } }\n";
    check_int("blanket_struct", std::string(bl) + "dbg(P { v: 41 })", 42);       // P satisfies Show
    check_int("blanket_primitive", std::string(bl) + "dbg(7)", 71);             // Int satisfies Show
    // Generic `T: Dbg` bound to P -> dynamic RESOLVE_CALL finds the blanket-filled column.
    check_int("blanket_dynamic",
        std::string(bl) + "fn via[T: Dbg](x: T) -> Int { dbg(x) }\n via(P { v: 100 })", 101);
    // A concrete `impl Dbg for P` wins over the blanket (specialization).
    check_int("blanket_concrete_wins",
        std::string(bl) + "impl Dbg for P { fn dbg(self) -> Int { 999 } }\n dbg(P { v: 41 })", 999);
    // Blanket via an enum (each variant's dense column filled).
    check_int("blanket_enum",
        "trait Show { fn shw(self) -> Int }\n trait Dbg { fn dbg(self) -> Int }\n"
        "impl[T: Show] Dbg for T { fn dbg(self) -> Int { shw(self) * 2 } }\n"
        "enum E { A(Int), B(Int) }\n impl Show for E { fn shw(self) -> Int { match self { A(n) => n, B(n) => n } } }\n"
        "dbg(A(5)) + dbg(B(6))", 22);
}

// `dyn Trait` -- trait objects (existentials). The runtime already does this: a value carries
// its own type, so a `dyn` value IS the value and dispatch is the same dynamic path (RESOLVE_CALL) an
// erased `T: Trait` takes. Everything below therefore tests the CHECKER.
void test_codegen_dyn_traits() {
    std::cout << "[codegen: dyn Trait (trait objects)]\n";

    const std::string shw =
        "trait Show { fn shw(self) -> Int }\n"
        "struct P { v: Int }\n"
        "impl Show for P { fn shw(self) -> Int { self.v } }\n"
        "impl Show for Int { fn shw(self) -> Int { self * 10 } }\n";

    // --- the coercion `Concrete <: dyn Trait`, at each check boundary ---
    check_int("dyn_let",    shw + "let x: dyn Show = P { v: 42 }\n shw(x)", 42);
    check_int("dyn_let_int", shw + "let x: dyn Show = 7\n shw(x)", 70);
    check_int("dyn_param",  shw + "fn f(x: dyn Show) -> Int { shw(x) }\n f(P { v: 5 })", 5);
    check_int("dyn_return", shw + "fn mk() -> dyn Show { P { v: 9 } }\n shw(mk())", 9);
    check_int("dyn_field",
        shw + "struct Box { it: dyn Show }\n let b = Box { it: 3 }\n shw(b.it)", 30);
    check_int("dyn_pipe",   shw + "let x: dyn Show = P { v: 8 }\n x |> shw", 8);
    check_int("dyn_qualified", shw + "let x: dyn Show = 4\n Show::shw(x)", 40);

    // --- THE headline: a HETEROGENEOUS container. Impossible with a generic `T: Show`,
    // where every element would have to be the same T.
    check_int("dyn_heterogeneous_vec",
        shw + "let mut v: Vec[dyn Show] = vec()\n push(v, P { v: 1 })\n push(v, 2)\n push(v, P { v: 3 })\n"
              "let mut sum = 0\n for x in v { sum = sum + shw(x) }\n sum", 24);   // 1 + 20 + 3
    check_int("dyn_heterogeneous_list",
        shw + "let l: List[dyn Show] = [P { v: 5 }, 6]\n"
              "let mut sum = 0\n for x in l { sum = sum + shw(x) }\n sum", 65);   // 5 + 60

    // A dyn passed on to a generic bounded fn: `dyn Show` itself satisfies `T: Show`.
    check_int("dyn_into_bounded_generic",
        shw + "fn via[T: Show](x: T) -> Int { shw(x) }\n let d: dyn Show = P { v: 11 }\n via(d)", 11);

    // --- supertraits + defaults through an object ---
    check_int("dyn_supertrait_method",
        "trait Base { fn b(self) -> Int }\n trait Ext: Base { fn e(self) -> Int }\n"
        "struct S { n: Int }\n impl Base for S { fn b(self) -> Int { self.n } }\n"
        "impl Ext for S { fn e(self) -> Int { self.n * 2 } }\n"
        "let x: dyn Ext = S { n: 21 }\n b(x) + e(x)", 63);          // supertrait method via dyn Ext
    check_int("dyn_default_method",
        "trait G { fn base(self) -> Int  fn greet(self) -> Int { base(self) + 100 } }\n"
        "struct P { v: Int }\n impl G for P { fn base(self) -> Int { self.v } }\n"
        "let x: dyn G = P { v: 5 }\n greet(x)", 105);

    // --- D9: an impl that comes from a BLANKET still dispatches through a dyn ---
    check_int("dyn_blanket",
        "trait Show { fn shw(self) -> Int }\n trait Dbg { fn dbg(self) -> Int }\n"
        "impl[T: Show] Dbg for T { fn dbg(self) -> Int { shw(self) + 1 } }\n"
        "struct P { v: Int }\n impl Show for P { fn shw(self) -> Int { self.v } }\n"
        "let x: dyn Dbg = P { v: 41 }\n dbg(x)", 42);

    // --- parametric traits (D6): the args are written out and must MATCH the impl ---
    const std::string par =
        "trait Conv[T] { fn conv(self) -> T }\n"
        "struct A { n: Int }\n impl Conv[Int] for A { fn conv(self) -> Int { self.n } }\n"
        "struct B { s: Int }\n impl Conv[String] for B { fn conv(self) -> String { \"b\" } }\n";
    check_int("dyn_parametric", par + "let x: dyn Conv[Int] = A { n: 12 }\n conv(x)", 12);
    check_true("dyn_parametric_wrong_arg",           // B implements Conv[String], not Conv[Int]
        cg_check_fails(par + "let x: dyn Conv[Int] = B { s: 1 }\n conv(x)"));
    check_true("dyn_parametric_needs_args",          // bare `dyn Conv` -- nothing could solve T
        check_has(par + "let x: dyn Conv = A { n: 1 }\n 0", "needs 1 type argument"));

    // --- negatives: the coercion is not a free-for-all ---
    check_true("dyn_no_impl",                        // Bool has no Show impl
        check_has(shw + "let x: dyn Show = true\n 0", "type mismatch"));
    check_true("dyn_unknown_trait",
        check_has("let x: dyn Nope = 1\n 0", "unknown trait"));
    check_true("dyn_not_a_trait",                    // a struct name is not a trait
        check_has("struct P { v: Int }\n let x: dyn P = P { v: 1 }\n 0", "unknown trait"));

    // CONTAINER INVARIANCE: `Vec[Int]` is NOT a `Vec[dyn Show]`. Unsound if allowed -- through
    // such an alias one could push a String into a Vec[Int]. (Element-wise coercion in a literal
    // is fine -- see dyn_heterogeneous_vec -- because it happens per element at a check boundary.)
    check_true("dyn_container_invariance",
        check_has(shw + "let v: Vec[Int] = vec()\n push(v, 1)\n let d: Vec[dyn Show] = v\n 0",
                  "expected Vec[dyn Show], found Vec[Int]"));

    // JOIN STAYS EXACT: `if c { dyn } else { Int }` does not silently produce a `dyn Show`,
    // exactly as join refuses to widen Int -> Double. An annotation (a check boundary) is the fix.
    check_true("dyn_join_is_exact",
        cg_check_fails(shw + "let d: dyn Show = P { v: 1 }\n let c = 1 < 2\n let y = if c { d } else { 2 }\n 0"));
    check_int("dyn_join_via_annotation",
        shw + "let d: dyn Show = P { v: 1 }\n let c = 1 < 2\n let y: dyn Show = if c { d } else { 2 }\n shw(y)", 1);

    // --- OBJECT SAFETY (v1 rules) ---
    // OS2 -- the SOUNDNESS rule: two `dyn Eq` may hide different types, so a `Self` parameter
    // would reach an impl body expecting the receiver's type.
    check_true("dyn_os_self_param",
        check_has("trait Eq { fn eq(self, other: Self) -> Bool }\n struct P { v: Int }\n"
                  "impl Eq for P { fn eq(self, other: P) -> Bool { true } }\n"
                  "fn f(x: dyn Eq) -> Int { 0 }\n 0",
                  "takes 'Self' as a parameter"));
    // OS1 -- nothing to dispatch on without a receiver.
    check_true("dyn_os_no_self",
        check_has("trait Mk { fn mk() -> Int }\n fn f(x: dyn Mk) -> Int { 0 }\n 0", "takes no 'self'"));
    // OS3 / OS4 -- v1 conservatism (both are sound here; see the v2 backlog).
    check_true("dyn_os_self_return",
        check_has("trait Cl { fn cl(self) -> Self }\n fn f(x: dyn Cl) -> Int { 0 }\n 0", "returns 'Self'"));
    check_true("dyn_os_generic_method",
        check_has("trait Snk { fn put[U](self, u: U) -> Int }\n fn f(x: dyn Snk) -> Int { 0 }\n 0",
                  "is generic"));
    // OS5 -- object safety propagates through supertraits.
    check_true("dyn_os_supertrait",
        check_has("trait Eq { fn eq(self, other: Self) -> Bool }\n trait Ord: Eq { fn cmp(self) -> Int }\n"
                  "fn f(x: dyn Ord) -> Int { 0 }\n 0", "supertrait"));
    // Self in a NESTED param position is caught too (the walker recurses).
    check_true("dyn_os_self_nested",
        check_has("trait Ins { fn ins(self, xs: Vec[Self]) -> Int }\n fn f(x: dyn Ins) -> Int { 0 }\n 0",
                  "takes 'Self' as a parameter"));
    // A plain, object-safe trait is accepted (the positive control for all of the above).
    check_true("dyn_os_ok", !cg_check_fails(shw + "fn f(x: dyn Show) -> Int { shw(x) }\n f(P { v: 1 })"));
}

void test_codegen_closures() {
    std::cout << "[codegen: closures P6]\n";

    // Capture-free lambda -> a Func immediate (LOAD_FN), called indirectly.
    check_int("nocap_call", "let id = fn(x: Int) -> Int { x }\n id(42)", 42);
    check_int("nocap_passed",
        "fn apply(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n"
        " apply(fn(x: Int) -> Int { x * 2 }, 21)", 42);

    // Capturing lambda -> a KIND_CLOSURE (MAKE_CLOSURE), captures read via LOAD_CAPTURE.
    check_int("capture_one", "let n = 40\n let add = fn(x: Int) -> Int { x + n }\n add(2)", 42);
    check_int("capture_two",
        "let a = 10\n let b = 30\n let f = fn(x: Int) -> Int { x + a + b }\n f(2)", 42);
    // Capture a function parameter.
    check_int("capture_param",
        "fn adder(n: Int) -> fn(Int) -> Int { fn(x: Int) -> Int { x + n } }\n adder(40)(2)", 42);

    // A capturing lambda passed to a higher-order fn.
    check_int("capture_passed",
        "fn apply(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n"
        " let k = 100\n apply(fn(x: Int) -> Int { x + k }, 5)", 105);

    // Nested lambda: the inner captures the outer's param (threaded through both closures).
    check_int("nested_capture",
        "fn make(a: Int) -> fn(Int) -> Int {\n"
        "  let g = fn(b: Int) -> fn(Int) -> Int { fn(c: Int) -> Int { a + b + c } }\n g(20)\n }\n"
        " make(10)(12)", 42);

    // Capture used in a control-flow body.
    check_int("capture_in_if",
        "let base = 100\n let clamp = fn(x: Int) -> Int { if x > base { base } else { x } }\n"
        " clamp(250) + clamp(7)", 107);

    // A lambda whose body tail-calls a captured function value (indirect TCO through the closure).
    check_int("capture_call_fnvalue",
        "fn twice(f: fn(Int) -> Int, x: Int) -> Int { f(f(x)) }\n"
        " let inc = fn(x: Int) -> Int { x + 1 }\n twice(inc, 40)", 42);

    // Int->Double coercion inside a lambda body returning Double (I2D at the RET boundary).
    check_dbl("lambda_ret_coerce",
        "let half = fn() -> Double { 3 }\n half() + 0.5", 3.5);
}

// ---- the LAMBDA WRITE BARRIER (checker) ------------------------------------
// A capture is a by-value copy of the BINDING, so assigning to a captured NAME inside a lambda has
// no code to generate -- it used to reach codegen and abort there with "assignment to undefined
// variable", a CodegenError raised from checker-accepted source, naming a binding that is in scope
// and `mut`. The rule now lives in `check_assign` (Check.cpp) behind `lambda_floor_`.
//
// The discriminator is the TARGET FORM: a captured copy of a HEAP value is a pointer, so `o.f = v`
// and `a[i] = v` through a captured name reach the one shared object and stay legal.
void test_check_lambda_capture() {
    std::cout << "[check: lambda write barrier]\n";

    const char* ASSIGN = "let mut n = 0\n let f = fn(x: Int) -> Int { n = 1  x }\n f(5)";

    // The rejection itself, at the CHECKER tier (not codegen), and exactly once.
    check_true("lamcap_assign_rejected", check_has(ASSIGN, "cannot assign to 'n' inside a lambda"));
    check_true("lamcap_assign_one_error", check_errc(ASSIGN) == 1);
    check_true("lamcap_assign_reason", check_has(ASSIGN, "captured by value"));
    // Compound assignment is the same statement kind, so it must be caught by the same guard.
    check_true("lamcap_compound_rejected",
        check_has("let mut n = 0\n let f = fn(x: Int) -> Int { n += 1  x }\n f(5)",
                  "cannot assign to 'n' inside a lambda"));

    // ORDER: a captured binding that is ALSO immutable must report the capture, not "declare it
    // 'mut'" -- that advice cannot work here, and it is the reason the guard sits above the
    // mutability test rather than below it.
    check_true("lamcap_beats_immutable",
        check_has("let n = 0\n let f = fn(x: Int) -> Int { n = 1  x }\n f(5)",
                  "cannot assign to 'n' inside a lambda"));
    check_true("lamcap_not_immutable_advice",
        !check_has("let n = 0\n let f = fn(x: Int) -> Int { n = 1  x }\n f(5)",
                   "cannot assign to immutable binding"));

    // The right-hand side is still checked: a bad target must not swallow its own value's errors.
    check_true("lamcap_rhs_still_checked",
        check_has("let mut n = 0\n let f = fn(x: Int) -> Int { n = nope  x }\n f(5)",
                  "unknown variable 'nope'"));

    // --- the negatives: each pins a distinct rule ---

    // A lambda's OWN local is not captured. Goes red if the floor is off by one.
    check_int("lamcap_own_local_ok",
        "let f = fn(x: Int) -> Int { let mut k = 0\n k = x + 1\n k }\n f(41)", 42);
    // ... and so is its own PARAMETER's shadow, and a local of a block nested inside the body.
    check_int("lamcap_nested_block_local_ok",
        "let f = fn(x: Int) -> Int { let mut k = 0\n if x > 0 { k = x }\n k }\n f(42)", 42);

    // Mutating THROUGH a captured heap value: the copy is a pointer, so the write is observable
    // outside. This is what demo/map_helpers.skn is built on and what the guide teaches.
    // (The M5 rule still applies underneath: the root binding must be `mut` to be mutated at all.
    // What matters here is that being CAPTURED adds no further restriction.)
    check_int("lamcap_field_assign_through_capture_ok",
        "struct P { v: Int }\n let mut p = P { v: 0 }\n"
        " let f = fn(x: Int) -> Int { p.v = x  p.v }\n f(42)", 42);
    check_int("lamcap_index_assign_through_capture_ok",
        "let mut m: Map[Int, Int] = #{}\n"
        " let f = fn(x: Int) -> Int { m[1] = x  m[1] }\n f(42)", 42);
    // A captured binding passed to a `mut` parameter (the require_mut_arg path) is untouched too.
    check_int("lamcap_mut_arg_through_capture_ok",
        "let mut v: Vec[Int] = vec()\n let f = fn(x: Int) -> () { push(v, x) }\n f(42)\n v[0]", 42);

    // THE LEAK. `check_lambda` delegates to `infer_lambda` and RETURNS before its own push_scope(),
    // so a floor installed at the top of `check_lambda` would never be restored -- and every plain
    // local of the ENCLOSING function would then resolve below it and be rejected as captured. The
    // delegation is taken when the expected type is not a matching `fn(...)`: here `Int`. The
    // lambda itself is a type error (that is the point); what must survive is the ORDINARY `n = 1`
    // after it. Nothing else in the suite covers this, and the failure is silent and wholesale.
    check_true("lamcap_no_floor_leak_after_delegation",
        !check_has("fn g() -> Int { let q: Int = fn(x: Int) -> Int { x }\n let mut n = 0\n n = 1\n n }",
                   "inside a lambda"));
    // The same shape with the lambda's arity mismatched -- the other way into the delegating branch.
    check_true("lamcap_no_floor_leak_arity",
        !check_has("fn g() -> Int { let q: fn(Int) -> Int = fn() -> Int { 1 }\n let mut n = 0\n n = 1\n n }",
                   "inside a lambda"));
    // And a lambda in ARGUMENT position followed by an ordinary assignment in the same function.
    check_int("lamcap_no_floor_leak_argument",
        "fn ap(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n"
        " fn g() -> Int { let mut n = ap(fn(x: Int) -> Int { x + 1 }, 10)\n n = n + 31\n n }\n g()", 42);
}

// ---- the SNAPSHOT WARNING (the outward half of the same rule) ---------------
// A capture is taken at closure-CREATION time, so REBINDING the outer name afterwards leaves the
// lambda holding the old value. That is well defined and documented (SkarnGuide.md section 10 turns
// on it), so it is advisory -- but earlier nothing was said at all, which made it the
// silent twin of the write barrier above.
//
// Armed only by a lambda bound to a `let`: such a lambda outlives its statement. One passed straight
// to a call is consumed within it, so a later rebinding cannot surprise anyone and must stay quiet.
void test_check_capture_snapshot_warning() {
    std::cout << "[check: capture snapshot warning]\n";

    // The guide's own section 10 example. `peek()` still yields 0, and now says why.
    // The tail is `peek()` itself, so the program's VALUE is the snapshot the claim is about.
    const char* SNAP = "let mut counter = 0\n let peek = fn() -> Int { counter }\n"
                       " counter = 10\n peek()";
    check_true("snapwarn_fires", check_warn_has(SNAP, "reassigned after a lambda captured it"));
    check_true("snapwarn_once",  check_warnc(SNAP) == 1);
    check_int ("snapwarn_value_unchanged", SNAP, 0);   // advisory: the program is untouched
    // Compound assignment is the same rebinding, so the guard must sit above the compound dispatch.
    check_true("snapwarn_compound",
        check_warn_has("let mut counter = 0\n let peek = fn() -> Int { counter }\n"
                       " counter += 10\n println(peek())", "reassigned after a lambda captured it"));

    // NESTED: the inner lambda is in ARGUMENT position, so it can only be armed by INHERITING the
    // enclosing let-bound lambda's state. Codegen threads an inner lambda's captures out through the
    // enclosing closure, so the snapshot outlives the statement exactly as the outer one's does --
    // this goes red if the arming assignment is `=` instead of `|=`.
    check_true("snapwarn_nested_inherits",
        check_warn_has("fn ap(f: fn() -> Int) -> Int { f() }\n let mut c = 0\n"
                       " let outer = fn() -> Int { ap(fn() -> Int { c }) }\n c = 5\n println(outer())",
                       "reassigned after a lambda captured it"));

    // --- the negatives ---

    // An ARGUMENT-position lambda is consumed within its statement: no arming, no warning.
    check_true("snapwarn_argument_lambda_quiet",
        !check_warn_has("fn ap(f: fn() -> Int) -> Int { f() }\n let mut acc = 0\n"
                        " let r = ap(fn() -> Int { acc })\n acc = 10\n println(r + acc)",
                        "reassigned after a lambda captured it"));
    // Captured but never rebound -- nothing can go stale.
    check_true("snapwarn_no_rebind_quiet",
        !check_warn_has("let mut v: Vec[Int] = vec()\n let f = fn() -> Int { len(v) }\n"
                        " push(v, 1)\n println(f())", "reassigned after a lambda captured it"));
    // An IMMUTABLE capture is never marked: it cannot be rebound in the first place.
    check_true("snapwarn_immutable_quiet",
        check_warnc("let n = 0\n let f = fn() -> Int { n }\n println(f())") == 0);
    // Mutating THROUGH the captured name reaches the one shared object, so the lambda DOES see it.
    // This is the demo/map_helpers.skn shape and the reason the discriminator is the target FORM.
    check_true("snapwarn_field_mutation_quiet",
        !check_warn_has("struct P { v: Int }\n let mut p = P { v: 0 }\n"
                        " let f = fn() -> Int { p.v }\n p.v = 5\n println(f())",
                        "reassigned after a lambda captured it"));
    check_true("snapwarn_index_mutation_quiet",
        !check_warn_has("let mut m: Map[Int, Int] = #{ 1 => 0 }\n"
                        " let f = fn() -> Int { m[1] }\n m[1] = 5\n println(f())",
                        "reassigned after a lambda captured it"));
    // Assigning a plain local that no lambda ever saw stays silent (the ordinary path).
    check_true("snapwarn_uncaptured_quiet",
        check_warnc("let mut n = 0\n n = 1\n println(n)") == 0);
    // A write INSIDE the lambda is the ERROR above, not this warning -- they must not both fire.
    check_true("snapwarn_not_on_inner_write",
        !check_warn_has("let mut n = 0\n let f = fn() -> Int { n = 1  n }\n println(f())",
                        "reassigned after a lambda captured it"));
}

// ---- typechecker: the multi-element `join` fold (`join_element`) -------------
//
// Two properties of every construct that folds N element types with `join` -- list literal, map
// literal, match arms, `break` values:
//
//   (1) SOUND: an incompatible pair must be REPORTED. `join` yields `Error` on a mismatch and
//       `Error` subsumes everything, so an unreported Error type reaching an accepted program
//       silences every later check on that value. `infer_map` folded keys AND values with a bare
//       `join` and no report at all, which made `let s: String = m[1]` compile against a
//       `#{1 => 7, 2 => 8.0}` -- an Int in a String binding on an erased runtime.
//   (2) ONE report per construct, INDEPENDENT OF ORDER. A failed join does not advance the
//       accumulator, so every later element of the offending type used to collide with it again:
//       the diagnostic COUNT depended on whether the odd element came first or last.
void test_check_join_fold() {
    std::cout << "[check: multi-element join fold]\n";

    // --- (1) soundness: the map fold reports at all ---

    // The hole itself: values disagree, and the binding downstream is a different type again.
    // Before the fix this whole program was ACCEPTED and printed an Int for `s`.
    check_true("joinfold_map_value_mismatch_rejected",
        check_has("let m = #{1 => 7, 2 => 8.0}\n let s: String = m[1]\n println(s)",
                  "map values have incompatible types"));
    // ... and the Error type no longer papers over the annotation mismatch either.
    check_true("joinfold_map_value_no_error_leak",
        check_has("let m = #{1 => 7, 2 => 8.0}\n let s: String = m[1]\n println(s)",
                  "expected String"));
    check_true("joinfold_map_key_mismatch_rejected",
        check_has("let m = #{1 => 7, 2.0 => 8}\n println(len(m))",
                  "map keys have incompatible types"));
    // A homogeneous map is untouched by all of this.
    check_true("joinfold_map_homogeneous_clean",
        check_errc("let m = #{1 => 7, 2 => 8, 3 => 9}\n println(len(m))") == 0);

    // --- (2) exactly one report, whichever end the odd element sits at ---

    check_true("joinfold_list_odd_last_one_error",
        check_errc("let a = [1, 2.0, 3]\n println(len(a))") == 1);
    check_true("joinfold_list_odd_first_one_error",
        check_errc("let a = [2.0, 1, 3]\n println(len(a))") == 1);
    // Three trailing Int arms after a Double one reported three times before the latch.
    check_true("joinfold_match_odd_first_one_error",
        check_errc("fn f(n: Int) -> Int {\n"
                   " let x = match n { 1 => 2.0, 2 => 2, 3 => 3, _ => 4 }\n 1\n}\n println(f(1))") == 1);
    check_true("joinfold_match_odd_last_one_error",
        check_errc("fn f(n: Int) -> Int {\n"
                   " let x = match n { 1 => 1, 2 => 2, 3 => 3, _ => 4.0 }\n 1\n}\n println(f(1))") == 1);
    // Keys and values are latched SEPARATELY -- a construct broken in both ways says both.
    check_true("joinfold_map_both_axes_report",
        check_errc("let m = #{1 => 7, 2.0 => 8.0}\n println(len(m))") == 2);

    // --- the messages still name the right construct, and keep their note caret ---

    check_true("joinfold_list_message",
        check_has("let a = [1, 2.0]\n println(len(a))", "list elements have incompatible types"));
    check_true("joinfold_match_message",
        check_has("fn f(n: Int) -> Int {\n let x = match n { 1 => 1, _ => 2.0 }\n 1\n}\n println(f(1))",
                  "match arms have incompatible types"));
    check_true("joinfold_break_message",
        check_has("let mut i = 0\n"
                  " let x = loop { if i > 2 { break 1 } i = i + 1 if i > 9 { break 2.0 } }\n println(1)",
                  "'break' values have incompatible types"));
    // An `if` has exactly two branches and is deliberately not routed through the helper -- it must
    // keep reporting exactly as before.
    check_true("joinfold_if_unchanged",
        check_errc("let c = true\n let x = if c { 1 } else { 2.0 }\n println(x)") == 1);
    // The check-position widening is untouched: an annotation still accepts a mixed if.
    check_true("joinfold_if_annotated_still_ok",
        check_errc("let c = true\n let x: Double = if c { 1 } else { 2.0 }\n println(x)") == 0);
}

// The `Int -> Double` COERCION, which used to be re-derived per emit site and was therefore simply
// MISSING at nine of the twenty-six sites -- `fn dv(a: Double, b: Double)` called as `dv(1, 2)`
// divided two raw Ints and returned 0, with no diagnostic and no trap. The checker records the edge
// on the node (`Expr::widen_double`) and `compile_expr` is the ONE place that honors it.
//
// Every case below asserts `isDouble()`, which is the whole point: an un-widened Int in a Double slot
// is still a perfectly fine 48-bit integer at run time, so only the TAG distinguishes right from
// wrong. Two failure directions are locked, and they need different cases:
//   (a) NO widening   -- the original hole. The value arrives as an Int and `check_dbl` fails on the
//                        tag even when the numeric value looks right.
//   (b) TWICE widened -- what a central hook costs if a site that derives the coercion ITSELF (the
//                        return boundary, `toInt`/`floor`/`toDouble`, `compile_coerced`) also routes
//                        through the honoring wrapper. `I2D` reads its source with `asSigned48()`,
//                        so a second application takes the low 48 bits of the DOUBLE and yields 0.0 --
//                        a Debug assert, and in Release a silent wrong number. Hence the `== 1.0`
//                        checks: `isDouble()` alone would pass on 0.0.
void test_codegen_widen_double() {
    std::cout << "[codegen: Int->Double coercion at every site]\n";

    // --- arguments: the shapes codegen has no callee parameter type for ---
    check_bool("widen_arg_div",      "fn dv(a: Double, b: Double) -> Double { a / b }\n dv(1, 2) == 0.5", true);
    check_dbl ("widen_arg_value",    "fn dv(a: Double, b: Double) -> Double { a / b }\n dv(1, 2)", 0.5);
    check_dbl ("widen_arg_indirect", "fn dv(a: Double, b: Double) -> Double { a / b }\n let f = dv\n f(1, 2)", 0.5);
    check_dbl ("widen_arg_lambda",   "let f = fn(a: Double) -> Double { a }\n f(1)", 1.0);

    // --- the six DELEGATING sites: the construct pushes the expected type inward, so the LEAF
    //     carries the mark and widens itself ---
    check_dbl ("widen_if_branch",    "let d: Double = if true { 1 } else { 2 }\n d", 1.0);
    check_dbl ("widen_match_arm",    "let d: Double = match 0 { 0 => 1, _ => 2 }\n d", 1.0);
    check_dbl ("widen_loop_break",   "let d: Double = loop { break 1 }\n d", 1.0);
    check_dbl ("widen_block_tail",   "let d: Double = { 1 }\n d", 1.0);
    check_dbl ("widen_map_value",    "let m: Map[String, Double] = #{\"a\" => 1}\n m[\"a\"]", 1.0);
    check_int ("widen_map_key",      "let m: Map[Double, Int] = #{1 => 7}\n m[1.0]", 7);
    // A `List` is not indexable, so its elements are read back through the container DUMP -- which
    // renders a Double with a `.0` and is therefore just as sharp a tag check as `check_dbl`.
    check_true("widen_list_elem",
        cg_run_out("let a: List[Double] = [1, 2]\n print(toString(a))") == "[1.0, 2.0]");

    check_dbl ("widen_if_binary",    "let a = 1\n let b = 2\n let d: Double = if true { a + b } else { 0 }\n d", 3.0);
    check_dbl ("widen_if_unary",     "let a = 1\n let d: Double = if true { -a } else { 0 }\n d", -1.0);
    // These two, and ONLY these two, are the falsifier for `compile_into`'s `!e.widen_double` guard --
    // verified by reverting it. The route is the INLINE EXPANSION's argument copy: it moves each
    // argument into a local with `compile_into`, whose destination-driven fast paths emit a Binary /
    // Unary producer straight into `dst` and never reach `compile_expr`. With the guard gone,
    // `id(a + b)` yields a raw `3` -- but only with inlining ON, since a real call compiles its
    // arguments through `compile_expr`. The `widen_if_*` pair above does NOT reach the fast paths (an
    // `if` branch tail is compiled as a block value), so it is no falsifier for the guard: that was
    // this change's own mistaken claim, caught by running the negative control instead of asserting it.
    check_dbl ("widen_inline_arg_binary", "fn id(x: Double) -> Double { x }\n let a = 1\n let b = 2\n id(a + b)", 3.0);
    check_dbl ("widen_inline_arg_unary",  "fn id(x: Double) -> Double { x }\n let a = 1\n id(-a)", -1.0);
    // A marked leaf that is a LOCAL sits in no temp, so the coercion must allocate one -- the case
    // the measure mirror (`measure_expr`) exists for.
    check_dbl ("widen_if_local",     "let n = 5\n let d: Double = if true { n } else { 0.0 }\n d", 5.0);
    check_true("widen_nested",
        cg_run_out("let a: List[Double] = [match 0 { 0 => 1, _ => 2 }, if false { 3 } else { 4 }]\n"
                   "print(toString(a))") == "[1.0, 4.0]");

    // --- direction (b): the sites that derive the coercion THEMSELVES must not widen twice ---
    check_dbl ("widen_ret_tail",     "fn f(x: Int) -> Double { x }\n f(1)", 1.0);
    check_dbl ("widen_ret_literal",  "fn f() -> Double { 1 }\n f()", 1.0);
    check_dbl ("widen_ret_if",       "fn f(c: Bool) -> Double { if c { 1 } else { 2 } }\n f(true)", 1.0);
    check_dbl ("widen_ret_match",    "fn f(n: Int) -> Double { match n { 0 => 1, _ => 2 } }\n f(0)", 1.0);
    check_dbl ("widen_lambda_ret",   "let f = fn(x: Int) -> Double { x }\n f(1)", 1.0);
    check_int ("widen_toint_of_int", "toInt(7)", 7);
    check_dbl ("widen_floor_of_int", "floor(5)", 5.0);
    check_dbl ("widen_todouble",     "toDouble(5)", 5.0);
    check_dbl ("widen_let_annot",    "let d: Double = 1\n d", 1.0);
    check_dbl ("widen_assign",       "let mut d: Double = 0.0\n d = 1\n d", 1.0);
}

void test_codegen_try() {
    std::cout << "[codegen: the ? operator P7]\n";

    // `?` on a Result: unwrap Ok, propagate Err. Result/Option are recognized by name, so the
    // program declares its own enum -- no static prelude needed for `?`.
    const std::string res =
        "enum Result[T, E] { Ok(T), Err(E) }\n"
        "fn parse(x: Int) -> Result[Int, Int] { if x > 0 { Ok(x) } else { Err(0 - x) } }\n"
        "fn addOne(x: Int) -> Result[Int, Int] { let v = parse(x)?\n Ok(v + 1) }\n";
    check_int("try_ok",  res + "match addOne(41) { Ok(v) => v, Err(e) => 0 - e }", 42);
    check_int("try_err", res + "match addOne(-5) { Ok(v) => v, Err(e) => e }", 5);   // Err(5) propagated

    // `?` on an Option: unwrap Some, propagate None.
    const std::string opt =
        "enum Option[T] { Some(T), None }\n"
        "fn firstPos(x: Int) -> Option[Int] { if x > 0 { Some(x) } else { None } }\n"
        "fn inc(x: Int) -> Option[Int] { let v = firstPos(x)?\n Some(v + 1) }\n";
    check_int("try_some", opt + "match inc(41) { Some(v) => v, None => -1 }", 42);
    check_int("try_none", opt + "match inc(-9) { Some(v) => v, None => -1 }", -1);   // None propagated

    // Two `?` in sequence: both must succeed to reach the Ok tail.
    check_int("try_chain",
        res + "fn add2(a: Int, b: Int) -> Result[Int, Int] { let x = parse(a)?\n let y = parse(b)?\n Ok(x + y) }\n"
              "match add2(40, 2) { Ok(v) => v, Err(e) => 0 - e }", 42);
    check_int("try_chain_short",   // the first `?` fails -> the second parse never runs
        res + "fn add2(a: Int, b: Int) -> Result[Int, Int] { let x = parse(a)?\n let y = parse(b)?\n Ok(x + y) }\n"
              "match add2(-7, 2) { Ok(v) => v, Err(e) => e }", 7);

    // `?` directly inside a constructor argument (an expression position, not a let init).
    check_int("try_in_arg",
        opt + "fn twice(x: Int) -> Option[Int] { Some(firstPos(x)? * 2) }\n"
              "match twice(21) { Some(v) => v, None => -1 }", 42);

    // A `?` in a fn that does NOT return a compatible Result/Option is a hard CHECK error.
    check_true("try_bad_return",
        cg_check_fails("enum Result[T, E] { Ok(T), Err(E) }\n"
                       "fn bad(x: Int) -> Int { let r: Result[Int, Int] = Ok(x)\n r? }"));
}

void test_codegen_arrays() {
    std::cout << "[codegen: container builtins + index P7b]\n";

    // array(n, init): always-initialized fixed array; index read.
    check_int("array_make",  "let a = array(3, 7)\n a[0] + a[1] + a[2]", 21);
    check_int("array_index", "let a = array(4, 5)\n a[2]", 5);
    check_int("array_len",   "len(array(6, 0))", 6);
    // Index write (requires a `mut` root binding).
    check_int("array_write", "let mut a = array(3, 0)\n a[1] = 9\n a[0] + a[1] + a[2]", 9);
    // `for` over an Array (index walk).
    check_int("array_for",
        "let a = array(4, 3)\n let mut s = 0\n for x in a { s = s + x }\n s", 12);
    // A Double-element array (element type = init's type; Array is invariant, so init is 0.0).
    check_dbl("array_double", "let a = array(2, 0.0)\n a[0] + 1.5", 1.5);

    // vec() + push (returns the vec, chainable) + len + index + for.
    check_int("vec_len",   "let v = push(push(vec(), 10), 20)\n len(v)", 2);
    check_int("vec_index", "let v = push(push(vec(), 5), 6)\n v[1]", 6);
    check_int("vec_for",
        "let v = push(push(push(vec(), 1), 2), 3)\n let mut s = 0\n for x in v { s = s + x }\n s", 6);
    // push chains via the pipe.
    check_int("vec_pipe", "let v = vec() |> push(7) |> push(8)\n v[0] + v[1]", 15);

    // Index write of an Int rvalue into a Double array coerces at the element boundary (I2D).
    check_dbl("array_write_double",
        "let mut a = array(2, 0.0)\n a[0] = 3\n a[0] + 0.5", 3.5);
    // push of an Int arg into a Vec[Double] coerces the arg (Vec[T] receiver fixes T=Double first).
    check_dbl("vec_push_double",
        "let mut v: Vec[Double] = vec()\n let v2 = push(v, 3)\n v2[0] + 0.5", 3.5);

    // Index assignment through a NON-mut binding is a hard CHECK error (H5).
    check_true("array_write_immut",
        cg_check_fails("let a = array(3, 0)\n a[0] = 5\n a[0]"));
    // `len` of a non-container is a hard CHECK error.
    check_true("len_bad", cg_check_fails("len(42)"));
}

void test_codegen_prelude() {
    std::cout << "[codegen: static prelude P7c]\n";

    // Option/Result come from the prelude now -- the user program does NOT declare them.
    // Construct + match a prelude Option.
    check_int_p("some_match", "match Some(41) { Some(x) => x + 1, None => 0 }", 42);
    check_int_p("none_match", "let o: Option[Int] = None\n match o { Some(x) => x, None => 99 }", 99);

    // `?` works against the prelude's Option/Result (recognized by name, now actually declared).
    check_int_p("try_prelude",
        "fn firstPos(x: Int) -> Option[Int] { if x > 0 { Some(x) } else { None } }\n"
        "fn inc(x: Int) -> Option[Int] { let v = firstPos(x)?\n Some(v + 1) }\n"
        "match inc(41) { Some(v) => v, None => -1 }", 42);

    // Option combinators.
    check_bool_p("isSome",   "isSome(Some(1))", true);
    check_bool_p("isNone",   "isNone(None)", true);
    check_int_p("unwrap_or_some", "unwrapOr(Some(7), 0)", 7);
    check_int_p("unwrap_or_none", "let o: Option[Int] = None\n unwrapOr(o, 5)", 5);
    check_int_p("optionMap",
        "match optionMap(Some(20), fn(x: Int) -> Int { x + 1 }) { Some(v) => v, None => 0 }", 21);
    check_int_p("optionAndThen",
        "match optionAndThen(Some(20), fn(x: Int) -> Option[Int] { Some(x * 2) }) { Some(v) => v, None => 0 }", 40);

    // Result combinators + the reconstruct-at-a-different-payload arms (Err(e) rebuilt as Result[U,E]).
    check_bool_p("isOk",  "isOk(Ok(1))", true);
    check_bool_p("isErr", "let r: Result[Int, Int] = Err(2)\n isErr(r)", true);
    check_int_p("result_map_ok",   // annotate so the Err type param E is pinned (Ok(20) alone leaves it free)
        "let r: Result[Int, Int] = Ok(20)\n"
        "match resultMap(r, fn(x: Int) -> Int { x + 1 }) { Ok(v) => v, Err(e) => 0 - e }", 21);
    check_int_p("result_map_err",
        "let r: Result[Int, Int] = Err(9)\n"
        "match resultMap(r, fn(x: Int) -> Int { x + 1 }) { Ok(v) => v, Err(e) => 0 - e }", -9);
    check_int_p("and_then_ok",
        "let r: Result[Int, Int] = Ok(20)\n"
        "match resultAndThen(r, fn(x: Int) -> Result[Int, Int] { Ok(x + 1) }) { Ok(v) => v, Err(e) => 0 - e }", 21);
    check_int_p("mapErr",
        "let r: Result[Int, Int] = Err(4)\n"
        "match mapErr(r, fn(e: Int) -> Int { e * 10 }) { Ok(v) => v, Err(e) => e }", 40);
    check_int_p("ok_or_some",
        "match okOr(Some(7), 1) { Ok(v) => v, Err(e) => 0 - e }", 7);
    check_int_p("ok_or_none",
        "let o: Option[Int] = None\n match okOr(o, 3) { Ok(v) => v, Err(e) => 0 - e }", -3);
    check_int_p("ok_bridge",
        "match ok(Ok(42)) { Some(v) => v, None => 0 }", 42);

    // A user program may still declare its OWN struct/enum alongside the prelude.
    check_int_p("user_alongside_prelude",
        "struct P { x: Int, y: Int }\n let p = P { x: 40, y: 2 }\n"
        "match okOr(Some(p.x + p.y), 0) { Ok(v) => v, Err(_) => -1 }", 42);

    // Tree-shaking: a program that uses no combinator drops the whole prelude (no fns emitted);
    // one that uses a combinator keeps it (transitively pulling in Option/Result as needed).
    try {
        check_true("shake_drops_all",
            svc::compile("42", svc::builtin_prelude()).function_table.empty());
        check_true("shake_keeps_used",
            svc::compile("isSome(Some(1))", svc::builtin_prelude()).function_table.size() >= 1);
    } catch (const std::exception& e) {
        ++g_fail; std::cout << "  WRONG!    shake   (threw: " << e.what() << ")\n";
    }
}

// Shadowing an AMBIENT name (builtin / native / trait method) is MODULE-SCOPED.
//
// The entry program's module prefix is "", so its declarations mangle bare -- into the same
// namespace as the builtins, natives and trait-method names, which belong to no module. A
// whole-program `fns_` lookup therefore let ONE user definition disable the ambient name
// everywhere, INCLUDING inside the sealed standard library, with no diagnostic:
// `fn toInt(x: Double) -> Int { 999 }` made `toIntChecked(3.9)` return `Ok(999)`.
//
// These run against the REAL prelude, which is what makes them multi-module (std::math /
// std::random / std::iter carry real prefixes), so they exercise the actual routing rule.
void test_ambient_shadowing() {
    std::cout << "[checker+codegen: shadowing an ambient name is module-scoped]\n";

    // A BUILTIN (toInt -> D2I). std::math's toIntChecked must keep the real conversion...
    check_int_p("shadow_builtin_std_unaffected",
        "fn toInt(x: Double) -> Int { 999 }\n"
        "use std::math::*\n"
        "match toIntChecked(3.9) { Ok(v) => v, Err(_) => -1 }", 3);
    // ...while the entry program's own call still resolves to its own fn (shadowing stays legal
    // where the fn is actually in scope -- this is a scoping fix, not a ban).
    check_int_p("shadow_builtin_entry_wins",
        "fn toInt(x: Double) -> Int { 999 }\n"
        "toInt(3.9)", 999);

    // A NATIVE (sqrt). std::random's nextGaussian is the one pure-Skarn std fn calling it: with
    // the real sqrt it yields ~0.574 for seed 42, with the faked one ~6.95 -- so a plain `< 1.0`
    // discriminates without pinning a float.
    check_bool_p("shadow_native_std_unaffected",
        "fn sqrt(x: Double) -> Double { 7.0 }\n"
        "use std::random::*\n"
        "let mut rng = Rng::fromSeed(42)\n"
        "rng.nextGaussian() < 1.0", true);
    check_bool_p("shadow_native_entry_wins",
        "fn sqrt(x: Double) -> Double { 7.0 }\n"
        "sqrt(9.0) == 7.0", true);

    // A TRAIT METHOD (next). The same hole reached trait dispatch: a bare `next` in std::iter's
    // combinator bodies resolved to the user fn, which used to bury the build in type errors
    // caretting into std/string.skn.
    check_int_p("shadow_trait_method_std_unaffected",
        "fn next(x: Int) -> Int { x + 1 }\n"
        "len([1, 2, 3] |> intoIter |> map(fn(x: Int) -> Int { x * 2 }) |> collect)", 3);
    check_int_p("shadow_trait_method_entry_wins",
        "fn next(x: Int) -> Int { x + 1 }\n"
        "next(41)", 42);

    // The other half of the same accident: the ENTRY program used to share the bare namespace, so
    // EVERY name it declared -- `pub` or not -- was reachable from any imported module, although
    // nothing can `import` the entry. It now carries an ordinary module prefix
    // (ENTRY_MODULE_PREFIX), so a module reaching for an entry name is a clean rejection.
    check_true("entry_fn_not_importable", cg_modules_check_fails(
        "import util\nuse util::callsEntry\nfn entryOnly() -> Int { 77 }\ncallsEntry()",
        { { "util", "pub fn callsEntry() -> Int { entryOnly() }" } }));
    check_true("entry_type_not_importable", cg_modules_check_fails(
        "import util\nuse util::useType\nstruct Point { x: Int }\nuseType(Point { x: 9 })",
        { { "util", "pub fn useType(p: Point) -> Int { p.x }" } }));
    // ...and the diagnostic explains WHY rather than just "unknown", which is what makes the
    // breaking change survivable for a reader who can see the name in the neighbouring file.
    check_true("entry_not_importable_says_why", [&] {
        try {
            svc::ModuleSet set = svc::load_modules(
                "import util\nuse util::callsEntry\nfn entryOnly() -> Int { 77 }\ncallsEntry()",
                mem_resolver({ { "util", "pub fn callsEntry() -> Int { entryOnly() }" } }));
            svc::compile_modules(std::move(set), nullptr);
        } catch (const svc::CheckFailure& e) {
            return std::string(e.what()).find("cannot be imported") != std::string::npos;
        } catch (...) {}
        return false;
    }());
    // The control: ordinary module-to-module resolution is untouched -- a `pub` name of a real
    // module is still reachable, which is the whole point of the entry being the ONLY exception.
    check_int_modules("module_to_module_still_works",
        "import util\nuse util::twice\ntwice(21)",
        { { "util", "pub fn twice(n: Int) -> Int { n * 2 }" } }, 42);
}

// Prelude tree-shaking now reaches Struct / Trait / Impl items (not just Fn / Enum). These tests use
// a SYNTHETIC prelude (via compile(src, prelude)) to prove shake-out precisely, inspecting the Module:
// a shaken trait contributes 0 rows (trait_method_count) and a shaken struct is absent from struct_types.
void test_tree_shake_prelude() {
    std::cout << "[codegen: prelude Trait/Impl/Struct tree-shaking]\n";

    auto has_struct = [](const svc::Module& m, const char* nm) {
        for (const auto& s : m.struct_types) if (s.name == nm) return true;
        return false;
    };
    auto try_bool = [&](const char* name, auto&& fn) {
        try { check_true(name, fn()); }
        catch (const std::exception& e) { ++g_fail; std::cout << "  WRONG!    " << name
                                                    << "   (threw: " << e.what() << ")\n"; }
    };

    // A synthetic prelude with a trait+impl+struct+fn the user program never touches.
    const char* MARKER =
        "trait Marker { fn mark(self) -> Int }\n"
        "impl Marker for Int { fn mark(self) -> Int { 42 } }\n"
        "struct Unused { x: Int }\n"
        "fn helper(n: Int) -> Int { n + 1 }\n";

    // (1) Nothing used -> trait, impl, struct AND fn all shaken.
    try_bool("shake_unused_all", [&] {
        svc::Module m = svc::compile("7", MARKER);
        return m.trait_method_count == 0 && !has_struct(m, "Unused") && m.function_table.empty();
    });

    // (1a) A `dyn Marker` ANNOTATION is the trait's only mention -- it must keep the prelude trait
    // (and its impl) alive. `dyn` names a trait exactly like a generic bound does; without the Dyn
    // case in cref_type the trait would be shaken out from under its own trait object.
    try_bool("shake_keeps_dyn_trait", [&] {
        svc::Module m = svc::compile("let x: dyn Marker = 1\n mark(x)", MARKER);
        return m.trait_method_count == 1;
    });
    // ... and a program that never says `dyn Marker` still drops it (the control).
    try_bool("shake_drops_unmentioned_trait", [&] {
        return svc::compile("7", MARKER).trait_method_count == 0;
    });

    // (1b) A USER trait with NO impl is never shaken (user items are roots), but it must still
    // contribute NO dispatch row -- its methods can never be dispatched, so they get no id.
    try_bool("dead_user_trait_no_row", [&] {
        return svc::compile("trait Dead { fn dm(self) -> Int }\n 7").trait_method_count == 0;
    });

    // (1c) A dead user trait BESIDE a live (dispatched) one: only the live method takes a row, and
    // its densely-assigned id still dispatches correctly (guards against an id/row mismatch).
    const char* MIXED =
        "trait Dead { fn dm(self) -> Int }\n"
        "trait Live { fn lm(self) -> Int }\n"
        "struct S { x: Int }\n"
        "impl Live for S { fn lm(self) -> Int { self.x } }\n"
        "fn call[T: Live](v: T) -> Int { lm(v) }\n";
    try_bool("dead_user_trait_beside_live_count", [&] {
        return svc::compile((std::string(MIXED) + "call(S { x: 5 })").c_str()).trait_method_count == 1;
    });
    try {
        Value v = cg_run((std::string(MIXED) + "call(S { x: 5 })").c_str());
        check_true("dead_user_trait_beside_live_runs", v.isInt() && v.asSigned48() == 5);
    } catch (const std::exception& e) {
        ++g_fail; std::cout << "  WRONG!    dead_user_trait_beside_live_runs   (threw: " << e.what() << ")\n";
    }

    // (1d) An EMPTY impl (all defaults) counts as an impl -> the trait stays live and dispatches.
    const char* DFLT =
        "trait Greeter { fn g(self) -> Int { 9 } }\n"
        "impl Greeter for Int {}\n"
        "fn call[T: Greeter](v: T) -> Int { g(v) }\n";
    try_bool("empty_impl_default_keeps_row", [&] {
        return svc::compile((std::string(DFLT) + "call(3)").c_str()).trait_method_count == 1;
    });
    try {
        Value v = cg_run((std::string(DFLT) + "call(3)").c_str());
        check_true("empty_impl_default_runs", v.isInt() && v.asSigned48() == 9);
    } catch (const std::exception& e) {
        ++g_fail; std::cout << "  WRONG!    empty_impl_default_runs   (threw: " << e.what() << ")\n";
    }

    // (2) A trait-method call keeps the trait + its impl (reached by method NAME); runs end-to-end.
    try {
        Value v = cg_run("mark(7)", MARKER);
        check_true("shake_method_call_runs", v.isInt() && v.asSigned48() == 42);
    } catch (const std::exception& e) {
        ++g_fail; std::cout << "  WRONG!    shake_method_call_runs   (threw: " << e.what() << ")\n";
    }
    try_bool("shake_method_call_keeps", [&] {
        return svc::compile("mark(7)", MARKER).trait_method_count >= 1;
    });
    // (2a) The same call in METHOD SYNTAX `7.mark()`: the method name lives in the Field callee, not in
    // an Ident, so the reference walk must record it -- otherwise the trait is shaken out from under a
    // program the checker already accepted, and codegen misreads `.mark` as a struct field.
    try {
        Value v = cg_run("7.mark()", MARKER);
        check_true("shake_dot_call_runs", v.isInt() && v.asSigned48() == 42);
    } catch (const std::exception& e) {
        ++g_fail; std::cout << "  WRONG!    shake_dot_call_runs   (threw: " << e.what() << ")\n";
    }
    try_bool("shake_dot_call_keeps", [&] {
        return svc::compile("7.mark()", MARKER).trait_method_count >= 1;
    });
    // ...but a plain field READ whose name happens to be a trait-method name is not a call and must not
    // keep the trait (the control against over-keeping).
    try_bool("shake_dot_field_no_keep", [&] {
        return svc::compile("struct S { mark: Int }\nlet s = S { mark: 1 }\n s.mark", MARKER).trait_method_count == 0;
    });
    // (2b) The real prelude: each program reaches its trait ONLY through method syntax.
    const std::pair<const char*, const char*> dot_only[] = {
        { "shake_dot_option_unwrapOr", "let o: Option[Int] = Some(5)\n o.unwrapOr(0)" },
        { "shake_dot_option_expect",   "let o: Option[Int] = Some(5)\n o.expect(\"none\")" },
        { "shake_dot_result_unwrap",   "let r: Result[Int, String] = Ok(5)\n r.unwrap()" },
        { "shake_dot_ord_lessThan",    "if 3.lessThan(4) { 5 } else { 0 }" },
        { "shake_dot_iterator_next",   "let mut it = range(5, 7)\n unwrapOr(it.next(), 0)" },
        { "shake_dot_tail_position",   "fn f(o: Option[Int]) -> Int { o.unwrapOr(0) }\n f(Some(5))" },
    };
    for (const auto& [name, src] : dot_only) {
        try {
            Value v = cg_run(src, svc::builtin_prelude());
            check_true(name, v.isInt() && v.asSigned48() == 5);
        } catch (const std::exception& e) {
            ++g_fail; std::cout << "  WRONG!    " << name << "   (threw: " << e.what() << ")\n";
        }
    }

    // (3) A generic BOUND `[T: Marker]` keeps the trait even with no method call in the body.
    try_bool("shake_bound_keeps", [&] {
        return svc::compile("fn f[T: Marker](x: T) -> Int { 0 }\n f(7)", MARKER).trait_method_count >= 1;
    });
    // ...and the bounded fn dispatches the method through the generic receiver, end-to-end.
    try {
        Value v = cg_run("fn f[T: Marker](x: T) -> Int { mark(x) }\n f(7)", MARKER);
        check_true("shake_bound_runs", v.isInt() && v.asSigned48() == 42);
    } catch (const std::exception& e) {
        ++g_fail; std::cout << "  WRONG!    shake_bound_runs   (threw: " << e.what() << ")\n";
    }

    // (4) The `for`-Iterable protocol. Prelude declares Iterable + a Bag impl.
    const char* ITER =
        "trait Iterable[T] { fn iter(self) -> Vec[T] }\n"
        "struct Bag { items: Vec[Int] }\n"
        "impl Iterable[Int] for Bag { fn iter(self) -> Vec[Int] { self.items } }\n";

    // (4a) `for x in bag` over a USER type that impls the RING Iterable (the iteration protocol is the
    // std::iter ring trait; a synthetic parallel trait no longer drives `for`). Uses the real prelude.
    const std::string bag_src =
        "struct Bag { items: Vec[Int] }\n"
        "impl Iterable[Int] for Bag { fn iter(self) -> Vec[Int] { self.items } }\n"
        "let mut v: Vec[Int] = vec()\n push(v, 10)\n push(v, 32)\n"
        "let b = Bag { items: v }\n let mut s = 0\n for x in b { s = s + x }\n s";
    try {
        Value r = cg_run(bag_src, svc::builtin_prelude());
        check_true("shake_for_user_runs", r.isInt() && r.asSigned48() == 42);
    } catch (const std::exception& e) {
        ++g_fail; std::cout << "  WRONG!    shake_for_user_runs   (threw: " << e.what() << ")\n";
    }
    try_bool("shake_for_user_keeps", [&] {
        svc::Module m = svc::compile(bag_src.c_str(), svc::builtin_prelude());
        return m.trait_method_count >= 1 && has_struct(m, "Bag");
    });

    // (4b) `for x in vec` over a BUILTIN container must NOT pull Iterable (the fast path) -> shaken.
    const std::string vec_src =
        "let mut v: Vec[Int] = vec()\n push(v, 1)\n push(v, 2)\n push(v, 3)\n"
        "let mut s = 0\n for x in v { s = s + x }\n s";
    try {
        Value r = cg_run(vec_src, ITER);
        check_true("shake_for_vec_runs", r.isInt() && r.asSigned48() == 6);
    } catch (const std::exception& e) {
        ++g_fail; std::cout << "  WRONG!    shake_for_vec_runs   (threw: " << e.what() << ")\n";
    }
    try_bool("shake_for_vec_drops_iterable", [&] {
        svc::Module m = svc::compile(vec_src.c_str(), ITER);
        return m.trait_method_count == 0 && !has_struct(m, "Bag");
    });

    // (5) Default-prelude regressions (builtin_prelude): the process structs now shake too.
    try_bool("shake_default_drops_process", [&] {
        svc::Module m = svc::compile("42", svc::builtin_prelude());
        return !has_struct(m, "ProcessOutput") && !has_struct(m, "ProcessText");
    });
    // `sh(...)` pulls in ProcessOutput (run's return type) but NOT ProcessText (only runText uses it).
    try_bool("shake_sh_keeps_output_only", [&] {
        svc::Module m = svc::compile("use std::process::*\nlet r = sh(\"echo\")\n 0", svc::builtin_prelude());
        return has_struct(m, "ProcessOutput") && !has_struct(m, "ProcessText");
    });
    // `runText(...)` pulls in BOTH ProcessText and ProcessOutput.
    try_bool("shake_runtext_keeps_both", [&] {
        svc::Module m = svc::compile("use std::process::*\nlet r = runText([\"echo\"])\n 0", svc::builtin_prelude());
        return has_struct(m, "ProcessText") && has_struct(m, "ProcessOutput");
    });
    // A prelude enum is still kept by a variant reference (Option via Some).
    try_bool("shake_keeps_option_enum", [&] {
        return has_struct(svc::compile("Some(1)", svc::builtin_prelude()), "Some");
    });
}

// `trait Iterable[T]` + impls for every builtin container + `toVec` are now baked into
// builtin_prelude() (tree-shaken out when unused). These run against the REAL prelude (check_int_p).
void test_codegen_iterable() {
    std::cout << "[codegen: prelude Iterable[T]]\n";

    auto has_struct = [](const svc::Module& m, const char* nm) {
        for (const auto& s : m.struct_types) if (s.name == nm) return true;
        return false;
    };
    auto try_bool = [&](const char* name, auto&& fn) {
        try { check_true(name, fn()); }
        catch (const std::exception& e) { ++g_fail; std::cout << "  WRONG!    " << name
                                                    << "   (threw: " << e.what() << ")\n"; }
    };

    // toVec over each builtin container (the impls materialize a Vec).
    check_int_p("iter_toVec_vec",   "let mut v: Vec[Int] = vec()\n push(v, 1)\n push(v, 2)\n len(toVec(v))", 2);
    check_int_p("iter_toVec_list",  "len(toVec([7, 8, 9]))", 3);
    check_int_p("iter_toVec_array", "len(toVec(array(4, 0)))", 4);
    check_int_p("iter_toVec_bytes", "len(toVec(toBytes(\"abc\")))", 3);
    check_int_p("iter_toVec_map",
        "let mut m: Map[Int, Int] = #{}\n m[1] = 10\n m[2] = 20\n len(toVec(m))", 2);
    // Vec's iter is identity -> the elements survive round-trip.
    check_int_p("iter_vec_roundtrip",
        "let mut v: Vec[Int] = vec()\n push(v, 10)\n push(v, 32)\n let w = toVec(v)\n w[0] + w[1]", 42);

    // A USER type implements the PRELUDE's Iterable (the trait comes from the prelude); `for` iterates it.
    check_int_p("iter_user_impl_for",
        "struct Bag { items: Vec[Int] }\n"
        "impl Iterable[Int] for Bag { fn iter(self) -> Vec[Int] { self.items } }\n"
        "let mut v: Vec[Int] = vec()\n push(v, 3)\n push(v, 4)\n"
        "let b = Bag { items: v }\n let mut s = 0\n for x in b { s = s + x }\n s", 7);

    // A generic `I: Iterable[T]` fn works over MULTIPLE container kinds (dynamic dispatch on the runtime type).
    check_int_p("iter_generic_over_list", "fn cnt[T, I: Iterable[T]](c: I) -> Int { len(toVec(c)) }\n cnt([1, 2, 3, 4, 5])", 5);
    check_int_p("iter_generic_over_vec",
        "fn cnt[T, I: Iterable[T]](c: I) -> Int { len(toVec(c)) }\n"
        "let mut v: Vec[Int] = vec()\n push(v, 1)\n push(v, 2)\n cnt(v)", 2);

    // Shake-out: a program that never touches Iterable drops the whole block (no trait rows, no $List).
    try_bool("iter_shaken_when_unused", [&] {
        svc::Module m = svc::compile("42", svc::builtin_prelude());
        return m.trait_method_count == 0 && !has_struct(m, "$List") && !has_struct(m, "Iterable");
    });
    // Using it keeps the trait + impls; the List impl forces `$List` (the accepted over-keep cost).
    try_bool("iter_kept_when_used", [&] {
        svc::Module m = svc::compile("len(toVec([1, 2, 3]))", svc::builtin_prelude());
        return m.trait_method_count >= 1 && has_struct(m, "$List");
    });
}

// Lazy `for` over the prelude Iterator / IntoIterator protocol (the Rust model). `for x in e` drives
// `e` itself if it is an Iterator (next() in place), else `intoIter(e)`. The built-in container fast
// paths are unchanged. See the Iterator/IntoIterator plan.
void test_codegen_iterator_for() {
    std::cout << "[codegen: lazy for over Iterator/IntoIterator]\n";

    auto try_bool = [&](const char* name, auto&& fn) {
        try { check_true(name, fn()); }
        catch (const std::exception& e) { ++g_fail; std::cout << "  WRONG!    " << name
                                                    << "   (threw: " << e.what() << ")\n"; }
    };

    const std::string COUNTER =
        "struct Counter { i: Int, n: Int }\n"
        "impl Iterator[Int] for Counter { fn next(mut self) -> Option[Int] {\n"
        "  if self.i < self.n { let x = self.i  self.i = self.i + 1  Some(x) } else { None } } }\n";

    // for over a value that IS an Iterator: drive next() in place.
    check_int_p("foriter_direct",
        COUNTER + "let c = Counter { i: 0, n: 5 }\n let mut s = 0\n for x in c { s = s + x }\n s", 10);

    // break / continue through the lazy loop.
    check_int_p("foriter_break_continue",
        COUNTER + "let c = Counter { i: 0, n: 100 }\n let mut s = 0\n"
        "for x in c { if x == 3 { continue } if x >= 6 { break } s = s + x }\n s", 12);   // 0+1+2+4+5

    // IntoIterator: a user collection produces a FRESH cursor each `for` -> iterable more than once.
    const std::string BAG =
        "struct Bag { items: Vec[Int] }\n"
        "struct BagCur { v: Vec[Int], i: Int }\n"
        "impl Iterator[Int] for BagCur { fn next(mut self) -> Option[Int] {\n"
        "  if self.i < len(self.v) { let x = self.v[self.i]  self.i = self.i + 1  Some(x) } else { None } } }\n"
        "impl IntoIterator[Int] for Bag { fn intoIter(self) -> dyn Iterator[Int] { BagCur { v: self.items, i: 0 } } }\n";
    check_int_p("foriter_intoiter_twice",
        BAG + "let mut v: Vec[Int] = vec()\n push(v, 3)\n push(v, 4)\n push(v, 5)\n"
        "let b = Bag { items: v }\n let mut s = 0\n for x in b { s = s + x }\n for x in b { s = s + x }\n s", 24);

    // A stage cursor holding its upstream behind a `dyn Iterator` field -- the dissolution keystone
    // (a mut-self `next` calling `next(self.up)` through the dyn field).
    const std::string STAGE =
        "struct Doubler { up: dyn Iterator[Int] }\n"
        "impl Iterator[Int] for Doubler { fn next(mut self) -> Option[Int] {\n"
        "  match next(self.up) { Some(x) => Some(x * 2), None => None } } }\n";
    check_int_p("foriter_dyn_stage",
        COUNTER + STAGE + "let src = Counter { i: 0, n: 4 }\n let d = Doubler { up: src }\n"
        "let mut s = 0\n for x in d { s = s + x }\n s", 12);   // (0+1+2+3)*2

    // The built-in container `for` is UNCHANGED (its fast path wins even though the traits now exist).
    check_int_p("foriter_builtin_unaffected",
        "let mut v: Vec[Int] = vec()\n push(v, 10)\n push(v, 32)\n let mut s = 0\n for x in v { s = s + x }\n s", 42);

    // for over a lazy Stream (which now impls Iterator via the bridge) -- the headline `for x in stream`.
    check_int_p("foriter_stream", "let mut s = 0\n for x in range(0, 5) { s = s + x }\n s", 10);
    // filter's element type solves from `range` directly (single param T), so no annotation is needed.
    check_int_p("foriter_stream_filter",
        "let mut s = 0\n for x in filter(range(0, 10), fn(x: Int) -> Bool { x % 2 == 0 }) { s = s + x }\n s", 20);
    // `map` introduces a fresh OUTPUT param U. Now that a lambda's annotated return is unified with
    // the expected slot, U solves from the lambda even in inference position -- `for x in map(..)`
    // type-checks WITHOUT an annotation (the closed generic-of-generic gap).
    check_int_p("foriter_stream_map",
        "let mut s = 0\n for x in map(range(0, 4), fn(x: Int) -> Int { x * 10 }) { s = s + x }\n s", 60);
    // A bare `let w = collect(map(..))` (generic-of-generic, still nested) likewise now solves.
    check_int_p("foriter_collect_map_no_annot",
        "let w = collect(map(range(0, 4), fn(x: Int) -> Int { x + 1 }))\n w[0] + w[3]", 5);   // 1 + 4
    // for over intoIter(container) explicitly (the fresh-cursor entry the container fast path bypasses).
    check_int_p("foriter_intoiter_vec",
        "let mut v: Vec[Int] = vec()\n push(v, 3)\n push(v, 4)\n let mut s = 0\n for x in intoIter(v) { s = s + x }\n s", 7);
    check_int_p("foriter_intoiter_map",
        "let mut m: Map[Int, Int] = #{}\n m[1] = 10\n m[2] = 20\n let mut s = 0\n for kv in intoIter(m) { s = s + kv.1 }\n s", 30);

    // Using an iterator keeps the trait rows; an iterator-free program drops them (tree-shaking).
    try_bool("foriter_kept_when_used", [&] {
        const std::string src =
            COUNTER + "let c = Counter { i: 0, n: 3 }\n let mut s = 0\n for x in c { s = s + x }\n s";
        svc::Module m = svc::compile(src.c_str(), svc::builtin_prelude());
        return m.trait_method_count >= 1;
    });
    try_bool("foriter_shaken_when_unused", [&] {
        svc::Module m = svc::compile("42", svc::builtin_prelude());
        return m.trait_method_count == 0;
    });
}

// Combinators round 2 (flatMap/zip/enumerate/takeWhile/dropWhile/scan/chain + the trivial terminals +
// the Ord trait). All pure prelude on the Iterator base -- no compiler/VM change. Value checks against the
// real prelude (check_int_p); the differential diff_comb_* live in the corpus below.
void test_codegen_combinators() {
    std::cout << "[codegen: iterator combinators round 2]\n";

    // -- Slice A: Tier-1 stages --
    check_int_p("comb_takeWhile", "sum(takeWhile(range(0, 10), fn(x: Int) -> Bool { x < 4 }))", 6);   // 0+1+2+3
    // takeWhile latches: a later element that would pass is NOT resumed after the first reject.
    check_int_p("comb_takeWhile_latch",
        "sum(takeWhile(range(0, 6), fn(x: Int) -> Bool { x != 3 }))", 3);   // 0+1+2, stop at 3
    check_int_p("comb_dropWhile", "sum(dropWhile(range(0, 10), fn(x: Int) -> Bool { x < 7 }))", 24);  // 7+8+9
    check_int_p("comb_dropWhile_all", "count(dropWhile(range(0, 5), fn(x: Int) -> Bool { x < 100 }))", 0);
    // enumerate -> (Int, T), consumed by tuple destructuring in a `for`.
    check_int_p("comb_enumerate",
        "let mut s = 0\n for (i, x) in enumerate(range(10, 13)) { s = s + i * 100 + x }\n s", 333); // 10+111+212
    // scan yields the running accumulator after each element: sums 1,3,6,10 over 1..4.
    check_int_p("comb_scan", "sum(scan(range(1, 5), 0, fn(a: Int, x: Int) -> Int { a + x }))", 20);

    // -- Slice B: two-upstream + nested stages --
    check_int_p("comb_zip",
        "let mut s = 0\n for (a, b) in zip(range(0, 3), range(10, 100)) { s = s + a * 100 + b }\n s", 333);
    check_int_p("comb_zip_stops_short", "count(zip(range(0, 3), range(0, 10)))", 3);
    check_int_p("comb_chain", "sum(chain(range(0, 3), range(10, 13)))", 36);   // (0+1+2)+(10+11+12)
    // flatMap keystone: x -> range(0, x); sum over 0..4 = 0 + 0 + (0+1) + (0+1+2) = 4.
    check_int_p("comb_flatMap",
        "sum(flatMap(range(0, 4), fn(x: Int) -> dyn Iterator[Int] { range(0, x) }))", 4);
    // flatMap flattening length: range(0,x) for x in 1..4 -> 1+2+3 = 6 elements.
    check_int_p("comb_flatMap_len",
        "len(collect(flatMap(range(1, 4), fn(x: Int) -> dyn Iterator[Int] { range(0, x) })))", 6);

    // -- Slice C: Ord + terminals --
    check_int_p("comb_reduce",
        "unwrapOr(reduce(range(1, 5), fn(a: Int, b: Int) -> Int { a + b }), -1)", 10);   // 1+2+3+4
    check_int_p("comb_reduce_empty",
        "unwrapOr(reduce(range(0, 0), fn(a: Int, b: Int) -> Int { a + b }), -1)", -1);
    // min/max over Ord (generic; devirtualizes on Int here). Empty -> None.
    check_int_p("comb_min", "unwrapOr(min(chain(range(5, 8), range(1, 3))), -1)", 1);
    check_int_p("comb_max", "unwrapOr(max(chain(range(1, 4), range(7, 9))), -1)", 8);
    check_int_p("comb_min_empty", "unwrapOr(min(range(0, 0)), 99)", 99);
    // minBy / maxBy comparator form: pick by |x - 5| distance would need arithmetic; use plain `<`.
    check_int_p("comb_maxBy",
        "unwrapOr(maxBy(range(1, 5), fn(a: Int, b: Int) -> Bool { a < b }), -1)", 4);
    check_int_p("comb_product", "product(range(1, 5))", 24);   // 1*2*3*4
    check_int_p("comb_position",
        "unwrapOr(position(range(10, 20), fn(x: Int) -> Bool { x == 13 }), -1)", 3);
    check_int_p("comb_position_miss",
        "unwrapOr(position(range(0, 5), fn(x: Int) -> Bool { x == 99 }), -1)", -1);
    check_int_p("comb_contains_true",
        "if contains(range(0, 10), 7) { 1 } else { 0 }", 1);
    check_int_p("comb_contains_false",
        "if contains(range(0, 10), 42) { 1 } else { 0 }", 0);
    // partition: evens vs odds of 0..6 -> (3 evens, 3 odds).
    check_int_p("comb_partition",
        "let (ev, od) = partition(range(0, 6), fn(x: Int) -> Bool { x % 2 == 0 })\n len(ev) * 10 + len(od)", 33);
    // unzip: enumerate then split -> indices vec + values vec.
    check_int_p("comb_unzip",
        "let (is, xs) = unzip(enumerate(range(100, 103)))\n is[2] * 1000 + xs[0]", 2100);   // idx 2, val 100
    // join over a String stream (map Ints to their text, join with '-').
    check_int_p("comb_join_len",
        "len(join(map(range(0, 3), fn(x: Int) -> String { toString(x) }), \"-\"))", 5);   // \"0-1-2\"
    // toMap: build i -> i*i for i in 0..4, then read one back.
    check_int_p("comb_toMap",
        "let m = toMap(map(range(0, 4), fn(x: Int) -> (Int, Int) { (x, x * x) }))\n m[3]", 9);

    // -- Slice A extra combinators: stepBy / inspect / dedup (pure lazy) --
    check_int_p("comb_stepBy_sum",   "sum(stepBy(range(0, 10), 3))", 18);         // 0+3+6+9
    check_int_p("comb_stepBy_count", "count(stepBy(range(0, 10), 2))", 5);        // 0,2,4,6,8
    check_int_p("comb_stepBy_one",   "sum(stepBy(range(0, 5), 1))", 10);          // step 1 == identity
    check_int_p("comb_inspect_pass", "sum(inspect(range(0, 5), fn(x: Int) -> () {}))", 10);   // passthrough
    check_int_p("comb_dedup_count",  "count(dedup(intoIter(toVec([1, 1, 2, 2, 2, 3, 1]))))", 4);   // 1,2,3,1
    check_int_p("comb_dedup_sum",    "sum(dedup(intoIter(toVec([1, 1, 2, 3, 3, 3]))))", 6);         // 1+2+3
    check_int_p("comb_dedup_empty",  "count(dedup(range(0, 0)))", 0);
    check_int_p("comb_dedup_single", "count(dedup(range(5, 6)))", 1);
    // inspect: the tap fires in element order while the value passes through unchanged.
    check_true("comb_inspect_out",
        cg_run_out("count(inspect(range(0, 3), fn(x: Int) -> () { print(x) }))") == "012");

    // -- Slice B combinators: chunks / windows / peekable (lazy per pull, buffering) --
    check_int_p("comb_chunks_count",  "count(chunks(range(0, 7), 3))", 3);              // [0,1,2][3,4,5][6]
    check_int_p("comb_chunks_exact",  "count(chunks(range(0, 6), 3))", 2);              // exact division
    check_int_p("comb_chunks_last",   "let v = collect(chunks(range(0, 7), 3))  len(v[2])", 1);   // short last
    check_int_p("comb_chunks_flat",   "sum(flatMap(chunks(range(0, 7), 3), fn(c: Vec[Int]) -> dyn Iterator[Int] { intoIter(c) }))", 21);
    check_int_p("comb_windows_count", "count(windows(range(0, 5), 3))", 3);             // 0-2,1-3,2-4
    check_int_p("comb_windows_short", "count(windows(range(0, 2), 3))", 0);             // source shorter than n
    check_int_p("comb_windows_slide", "let v = collect(windows(range(0, 4), 2))  len(v) * 100 + v[1][0] * 10 + v[2][1]", 313);  // [0,1][1,2][2,3]
    // peekable: peek looks ahead WITHOUT consuming (a=peek, b=next consumes it, c=peek advances).
    check_int_p("comb_peekable_peek", "let mut p = peekable(range(5, 8))  let a = unwrapOr(p.peek(), -1)  let b = unwrapOr(next(p), -1)  let c = unwrapOr(p.peek(), -1)  a * 100 + b * 10 + c", 556);
    check_int_p("comb_peekable_end",  "let mut p = peekable(range(0, 0))  unwrapOr(p.peek(), -1)", -1);   // peek at end = None
    // a peeked cursor coerces to dyn Iterator and still yields the buffered element first.
    check_int_p("comb_peekable_pipe", "let mut p = peekable(range(0, 5))  let pk = unwrapOr(p.peek(), -1)  pk * 100 + sum(p)", 10);

    // -- Slice D: String streaming (split / lines / words) --
    // split: N separators -> N+1 fields, empties kept.
    check_int_p("comb_split_count", "count(split(\"a,b,c\", \",\"))", 3);
    check_int_p("comb_split_field_lens",
        "let v = collect(split(\"hello,world\", \",\"))  len(v[0]) * 100 + len(v[1])", 505);
    // interior + trailing empties are kept: \"a,,b,\" -> [a, \"\", b, \"\"].
    check_int_p("comb_split_empties",
        "let v = collect(split(\"a,,b,\", \",\"))  len(v) * 10 + len(v[1]) + len(v[3])", 40);
    // empty input -> exactly one empty field.
    check_int_p("comb_split_empty_input", "count(split(\"\", \",\"))", 1);
    // a separator that never occurs -> the whole string as one field.
    check_int_p("comb_split_no_sep", "count(split(\"whole\", \",\"))", 1);
    // multi-byte separator; split/join round-trip: join(split(s, sep), sep) == s (len proxy).
    check_int_p("comb_split_multibyte_roundtrip", "len(join(split(\"a::b::c\", \"::\"), \"::\"))", 7);
    // lines: terminator semantics -- a trailing newline does NOT add a phantom empty line.
    check_int_p("comb_lines_count", "count(lines(\"a\\nb\\nc\"))", 3);
    check_int_p("comb_lines_trailing_nl", "count(lines(\"a\\nb\\n\"))", 2);
    check_int_p("comb_lines_no_trailing", "count(lines(\"a\\nb\"))", 2);
    check_int_p("comb_lines_empty", "count(lines(\"\"))", 0);
    // a lone newline is one empty line.
    check_int_p("comb_lines_just_nl", "let v = collect(lines(\"\\n\"))  len(v) * 10 + len(v[0])", 10);
    // CRLF: the \\r before each \\n is stripped.
    check_int_p("comb_lines_crlf",
        "let v = collect(lines(\"a\\r\\nbb\\r\\n\"))  len(v) * 10 + len(v[0]) + len(v[1])", 23);
    // driven by a `for` over the dyn Iterator directly.
    check_int_p("comb_lines_for",
        "let mut n = 0\n for ln in lines(\"a\\nbb\\nccc\") { n = n + len(ln) }\n n", 6);
    // words: separators are RUNS of ASCII whitespace and no empty field is ever produced.
    check_int_p("comb_words_count", "count(words(\"a b c\"))", 3);
    // THE distinguishing property, asserted against its neighbour in ONE expression so the two can
    // never silently converge: a doubled separator collapses for `words` (2) but not for `split` (3).
    check_int_p("comb_words_collapse_vs_split",
        "count(words(\"a  b\")) * 10 + count(split(\"a  b\", \" \"))", 23);
    // leading + trailing whitespace yield no empty field (the other half of the same rule).
    check_int_p("comb_words_trims_edges", "count(words(\"   a b   \"))", 2);
    check_int_p("comb_words_field_lens",
        "let v = collect(words(\" hello   world \"))  len(v) * 100 + len(v[0]) * 10 + len(v[1])", 255);
    // degenerate inputs: nothing to yield at all (contrast split(\"\", sep), which yields one field).
    check_int_p("comb_words_empty", "count(words(\"\"))", 0);
    check_int_p("comb_words_all_ws", "count(words(\" \\t\\n\\r \"))", 0);
    // every byte isAsciiWhitespace names separates. VT (11) and FF (12) have no string escape, so
    // this one is assembled byte-wise -- which is also the only way to reach them from source.
    check_int_p("comb_words_ws_kinds",
        "let mut b = bytes()  push(b, 97)  push(b, 9)  push(b, 98)  push(b, 11)  push(b, 99)\n"
        " push(b, 12)  push(b, 100)  push(b, 13)  push(b, 101)  count(words(fromBytes(b)))", 5);
    // BYTE-level: a byte >= 0x80 is never whitespace, so multi-byte UTF-8 stays inside one word.
    check_int_p("comb_words_high_byte_not_ws",
        "let mut b = bytes()  push(b, 97)  push(b, 0xC3)  push(b, 0xA4)  push(b, 98)\n"
        " count(words(fromBytes(b)))", 1);
    // composes with the ordinary terminals, and is driven by a `for` over the dyn Iterator directly.
    check_int_p("comb_words_join", "len(join(words(\"  a  b  \"), \"-\"))", 3);
    check_int_p("comb_words_for",
        "let mut n = 0\n for w in words(\"  a  bb ccc \") { n = n + len(w) }\n n", 6);
    // `words` is an ORDINARY prelude name, and a very common variable name -- a LOCAL must still win
    // (demo/set.skn already binds one). Locked here rather than left to the demo, because adding a
    // word this common to the shared namespace is exactly the change that could break existing code.
    check_int_p("comb_words_local_shadows",
        "let mut words: Vec[String] = vec()  push(words, \"a\")  len(words)", 1);

    // -- std::string helpers (round 1): search / trim / case / companions. BYTE-level; ASCII case.
    check_int_p("str_indexOf",            "indexOf(\"hello world\", \"o\")", 4);
    check_int_p("str_indexOf_miss",       "indexOf(\"abc\", \"z\")", -1);
    check_int_p("str_indexOf_empty",      "indexOf(\"abc\", \"\")", 0);          // "" matches at 0
    check_int_p("str_lastIndexOf",        "lastIndexOf(\"hello world\", \"o\")", 7);
    check_int_p("str_lastIndexOf_first",  "lastIndexOf(\"aXaXa\", \"a\")", 4);   // the LAST occurrence
    check_int_p("str_lastIndexOf_miss",   "lastIndexOf(\"abc\", \"z\")", -1);
    check_int_p("str_lastIndexOf_empty",  "lastIndexOf(\"abc\", \"\")", 3);      // "" matches at len(s)
    check_bool_p("str_startsWith",        "startsWith(\"hello\", \"he\")", true);
    check_bool_p("str_startsWith_no",     "startsWith(\"hello\", \"lo\")", false);
    check_bool_p("str_startsWith_empty",  "startsWith(\"hi\", \"\")", true);
    check_bool_p("str_startsWith_long",   "startsWith(\"hi\", \"hiya\")", false);
    check_bool_p("str_endsWith",          "endsWith(\"hello\", \"lo\")", true);
    check_bool_p("str_endsWith_no",       "endsWith(\"hello\", \"he\")", false);
    check_bool_p("str_endsWith_long",     "endsWith(\"hi\", \"ahoy\")", false);
    check_bool_p("str_hasSubstr",         "hasSubstr(\"hello\", \"ell\")", true);
    check_bool_p("str_hasSubstr_no",      "hasSubstr(\"hello\", \"xyz\")", false);
    check_bool_p("str_trim",              "trim(\"  \\t hi \\n \") == \"hi\"", true);
    check_bool_p("str_trim_all_ws",       "trim(\"   \") == \"\"", true);
    check_bool_p("str_trim_none",         "trim(\"hi\") == \"hi\"", true);
    check_bool_p("str_trimStart",         "trimStart(\"  hi  \") == \"hi  \"", true);
    check_bool_p("str_trimEnd",           "trimEnd(\"  hi  \") == \"  hi\"", true);
    check_bool_p("str_toUpper",           "toUpper(\"Hi, x9!\") == \"HI, X9!\"", true);
    check_bool_p("str_toLower",           "toLower(\"Hi, X9!\") == \"hi, x9!\"", true);
    check_int_p("str_charAt",             "charAt(\"ABC\", 1)", 66);
    check_bool_p("str_isEmpty",           "isEmpty(\"\")", true);
    check_bool_p("str_isEmpty_no",        "isEmpty(\"x\")", false);
    // split with an EMPTY separator now yields the whole string as one field ([s]) -- no longer a panic.
    check_int_p("str_split_empty_sep_count",    "count(split(\"abc\", \"\"))", 1);
    check_bool_p("str_split_empty_sep_field",   "collect(split(\"abc\", \"\"))[0] == \"abc\"", true);
    check_bool_p("str_split_empty_sep_rndtrip", "join(split(\"abc\", \"\"), \"\") == \"abc\"", true);
    // -- std::string round 2: replace (all occurrences; empty `from` = no-op) / pad / repeatStr.
    check_bool_p("str_replace",         "replace(\"a.b.c\", \".\", \"-\") == \"a-b-c\"", true);
    check_bool_p("str_replace_multi",   "replace(\"aXXbXXc\", \"XX\", \"_\") == \"a_b_c\"", true);
    check_bool_p("str_replace_delete",  "replace(\"hello\", \"l\", \"\") == \"heo\"", true);
    check_bool_p("str_replace_grow",    "replace(\"ab\", \"b\", \"yyy\") == \"ayyy\"", true);
    check_bool_p("str_replace_none",    "replace(\"abc\", \"z\", \"!\") == \"abc\"", true);
    check_bool_p("str_replace_empty",   "replace(\"abc\", \"\", \"X\") == \"abc\"", true);   // no-op
    check_bool_p("str_pad_start",       "padStart(\"42\", 5, ' ') == \"   42\"", true);
    check_bool_p("str_pad_start_zero",  "padStart(\"42\", 5, '0') == \"00042\"", true);
    check_bool_p("str_pad_start_nofit", "padStart(\"hello\", 3, '.') == \"hello\"", true);   // already wide
    check_bool_p("str_pad_end",         "padEnd(\"42\", 5, '.') == \"42...\"", true);
    check_int_p("str_pad_start_len",    "len(padStart(\"7\", 4, '0'))", 4);
    check_bool_p("str_repeat",          "repeatStr(\"ab\", 3) == \"ababab\"", true);
    check_bool_p("str_repeat_zero",     "repeatStr(\"x\", 0) == \"\"", true);
    check_bool_p("str_repeat_neg",      "repeatStr(\"x\", 0 - 2) == \"\"", true);            // n<=0 -> ""
    // -- ASCII classification family (Int -> Bool) + hasControl (String -> Bool). Bytes >= 128 -> false.
    check_bool_p("cls_control_lo",   "isAsciiControl(0)", true);
    check_bool_p("cls_control_hi",   "isAsciiControl(31)", true);
    check_bool_p("cls_control_del",  "isAsciiControl(127)", true);
    check_bool_p("cls_control_no",   "isAsciiControl(65) || isAsciiControl(32)", false);
    check_bool_p("cls_control_neg",  "isAsciiControl(0 - 5)", false);                        // negative -> false
    check_bool_p("cls_digit",        "isAsciiDigit('7') && !isAsciiDigit('a') && !isAsciiDigit(' ')", true);
    check_bool_p("cls_ws",           "isAsciiWhitespace(' ') && isAsciiWhitespace(9) && isAsciiWhitespace(11) && !isAsciiWhitespace('x')", true);
    check_bool_p("cls_alpha",        "isAsciiAlpha('Q') && isAsciiAlpha('z') && !isAsciiAlpha('7')", true);
    check_bool_p("cls_alnum",        "isAsciiAlphanumeric('7') && isAsciiAlphanumeric('Z') && !isAsciiAlphanumeric('_')", true);
    check_bool_p("cls_upper_lower",  "isAsciiUpper('A') && isAsciiLower('a') && !isAsciiUpper('a') && !isAsciiLower('A')", true);
    check_bool_p("cls_printable",    "isAsciiPrintable(' ') && isAsciiPrintable(126) && !isAsciiPrintable(127) && !isAsciiPrintable(9)", true);
    check_bool_p("cls_high_byte",    "!isAsciiControl(200) && !isAsciiAlpha(200) && !isAsciiPrintable(200)", true);  // >= 128 -> false
    check_bool_p("cls_hasControl",   "hasControl(\"bad\\ttab\") && !hasControl(\"clean text 123\")", true);
    check_bool_p("cls_hasControl_empty", "!hasControl(\"\")", true);

    // -- No-copy Map streaming (MAP_ITER_NEXT: for (k,v) fast path + the lazy MapCursor via intoIter).
    // All reductions are order-INDEPENDENT (sum/count), so the VM's hash order vs the oracle's
    // insertion order never shows. The for-loop is Path A (codegen), intoIter is Path B (prelude).
    check_int_p("map_for_value_sum",
        "let mut m: Map[Int, Int] = #{}\n m[1] = 10  m[2] = 20  m[3] = 30\n let mut s = 0\n for (k, v) in m { s = s + k + v }\n s", 66);
    check_int_p("map_for_whole_pair",       // `for x in m` binds the whole (K,V) tuple ($Tuple2 path)
        "let mut m: Map[Int, Int] = #{}\n m[5] = 1  m[6] = 2\n let mut s = 0\n for x in m { let (k, v) = x  s = s + k * v }\n s", 17);
    check_int_p("map_intoiter_count",       // Path B: the lazy MapCursor
        "let mut m: Map[Int, Int] = #{}\n m[1] = 1  m[2] = 2  m[3] = 3\n count(intoIter(m))", 3);
    check_int_p("map_intoiter_value_sum",   // fold the VALUES of the (K,V) pairs
        "let mut m: Map[Int, Int] = #{}\n m[1] = 10  m[2] = 20\n fold(intoIter(m), 0, fn(a: Int, kv: (Int, Int)) -> Int { let (k, v) = kv  a + v })", 30);
    check_int_p("map_stream_map_sum",       // map stage over the map cursor, then sum
        "let mut m: Map[Int, Int] = #{}\n m[1] = 100  m[2] = 200\n sum(map(intoIter(m), fn(kv: (Int, Int)) -> Int { let (k, v) = kv  v }))", 300);
    check_int_p("map_for_after_delete",     // a tombstone in the backing is skipped by the cursor
        "let mut m: Map[Int, Int] = #{}\n m[1] = 1  m[2] = 2  m[3] = 3\n delete(m, 2)\n let mut s = 0\n for (k, v) in m { s = s + v }\n s", 4);
}

// Lazy iteration (v1): the Iterator / IntoIterator cursor pipeline in the tree-shakeable prelude --
// mut-cursor sources (RangeCursor), stateless stages (MapStage, ...) and tail-recursive terminals.
// These run against the REAL prelude (check_int_p).
// (This shipped first as a closure-field `Stream[T]` struct, since dissolved into the Iterator /
// IntoIterator traits -- which is where the function name and the `stream_*` case names come from.)
void test_codegen_stream() {
    std::cout << "[codegen: lazy Iterator pipeline v1]\n";

    auto has_struct = [](const svc::Module& m, const char* nm) {
        for (const auto& s : m.struct_types) if (s.name == nm) return true;
        return false;
    };
    auto try_bool = [&](const char* name, auto&& fn) {
        try { check_true(name, fn()); }
        catch (const std::exception& e) { ++g_fail; std::cout << "  WRONG!    " << name
                                                    << "   (threw: " << e.what() << ")\n"; }
    };

    // Slice 1 -- the tracer bullet: a mut-cursor source driven from inside a closure, consumed by a
    // tail-recursive fold. `sum(range(0, 10))` = 0+1+..+9 = 45.
    check_int_p("stream_range_sum", "sum(range(0, 10))", 45);
    check_int_p("stream_fold_direct",
        "fold(range(0, 5), 100, fn(a: Int, x: Int) -> Int { a + x })", 110);
    // O(1)-stack proof: the fold TCOs, so N=1,000,000 completes without overflowing the ~16k native
    // frame limit; the closed-form sum 0..999999 = 499999500000 also proves the mut-cursor-through-
    // capture pattern codegens correctly end-to-end.
    check_int_p("stream_fold_deep", "sum(range(0, 1000000))", 499999500000LL);

    // Tree-shaking: a program that never streams drops the whole iterator block (cursors + fns).
    try_bool("stream_shaken_when_unused", [&] {
        svc::Module m = svc::compile("42", svc::builtin_prelude());
        return !has_struct(m, "RangeCursor") && !has_struct(m, "MapStage");
    });
    try_bool("stream_kept_when_used", [&] {
        svc::Module m = svc::compile("sum(range(0, 3))", svc::builtin_prelude());
        return has_struct(m, "RangeCursor");
    });

    // Slice 2 -- stateless stages + terminals + iterate.
    check_int_p("stream_map", "sum(map(range(0, 5), fn(x: Int) -> Int { x * 10 }))", 100);       // (0+1+2+3+4)*10
    check_int_p("stream_filter", "sum(filter(range(0, 10), fn(x: Int) -> Bool { x % 2 == 0 }))", 20); // 0+2+4+6+8
    check_int_p("stream_pipeline",  // filter |> map |> sum -- the probe computation via encoding C
        "sum(map(filter(range(0, 20), fn(x: Int) -> Bool { x % 2 == 0 }), fn(x: Int) -> Int { x * 3 }))", 270);
    check_int_p("stream_count", "count(range(3, 9))", 6);
    check_int_p("stream_collect_len", "len(collect(range(0, 4)))", 4);
    // `collect(map(..))` (generic-of-generic) now solves WITHOUT an annotation: the lambda's annotated
    // return drives `map`'s output param, then `collect`'s. (An annotation is still accepted.)
    check_int_p("stream_collect_elems", "let w = collect(map(range(0, 4), fn(x: Int) -> Int { x + 1 }))\n w[0] + w[3]", 5); // 1 + 4
    // iterate is infinite -> pull a few by hand (proves the generic IterCursor mut-advance): 1, 2, 4.
    check_int_p("stream_iterate_head",
        "let mut s = iterate(1, fn(x: Int) -> Int { x + x })\n"
        " match next(s) { Some(a) => match next(s) { Some(b) => match next(s) { Some(c) => a + b + c, None => 0 }, None => 0 }, None => 0 }", 7);
    // count shares fold's tail-recursive shape -> O(1) stack at N=100000.
    check_int_p("stream_count_deep", "count(range(0, 100000))", 100000);

    // Slice 3 -- zero-copy container sources (intoIter) + take + eager Iterable snapshot.
    check_int_p("stream_vec", "let mut v: Vec[Int] = vec()\n push(v, 5)\n push(v, 7)\n sum(intoIter(v))", 12);
    check_int_p("stream_array", "sum(intoIter(array(3, 4)))", 12);              // [4,4,4]
    check_int_p("stream_bytes", "sum(intoIter(toBytes(\"abc\")))", 294);        // 97+98+99
    check_int_p("stream_list", "let l: List[Int] = [10, 20, 30]\n sum(intoIter(l))", 60);   // List cursor spine walk
    check_int_p("stream_list_map", "let l: List[Int] = [1, 2, 3, 4]\n sum(map(intoIter(l), fn(x: Int) -> Int { x * x }))", 30); // 1+4+9+16
    // take short-circuits an INFINITE source -> proves laziness (1+2+4+8+16).
    check_int_p("stream_take_infinite", "sum(take(iterate(1, fn(x: Int) -> Int { x * 2 }), 5))", 31);
    // take stops early over a large array source without consuming it all.
    check_int_p("stream_take_early", "sum(take(intoIter(array(1000000, 1)), 3))", 3);
    // Streaming an eager source: a Map (live cursor via intoIter) and an Iterable-only user type
    // (intoIter over an explicit toVec snapshot -- the copy is now visible at the call site).
    check_int_p("stream_map_intoiter",
        "let mut m: Map[Int, Int] = #{}\n m[1] = 10\n m[2] = 20\n count(intoIter(m))", 2);
    check_int_p("stream_user_intoiter_tovec",
        "struct Bag { items: Vec[Int] }\n"
        "impl Iterable[Int] for Bag { fn iter(self) -> Vec[Int] { clone(self.items) } }\n"
        "let mut v: Vec[Int] = vec()\n push(v, 6)\n push(v, 8)\n"
        "let b = Bag { items: v }\n sum(intoIter(toVec(b)))", 14);

    // Gap-fillers -- more generators (repeat/empty/once), terminals (any/all/find), stage (drop).
    check_int_p("stream_repeat_take", "sum(take(repeat(5), 3))", 15);           // 5+5+5
    check_int_p("stream_empty_sum",   "sum(empty())", 0);                       // T pinned to Int by sum
    check_int_p("stream_once",        "sum(once(42))", 42);
    check_int_p("stream_once_count",  "count(once(7))", 1);
    check_bool_p("stream_any_hit",    "any(range(0, 5), fn(x: Int) -> Bool { x == 3 })", true);
    check_bool_p("stream_any_miss",   "any(range(0, 5), fn(x: Int) -> Bool { x == 9 })", false);
    check_bool_p("stream_all_true",   "all(range(0, 5), fn(x: Int) -> Bool { x < 10 })", true);
    check_bool_p("stream_all_false",  "all(range(0, 5), fn(x: Int) -> Bool { x < 3 })", false);
    check_int_p("stream_find_hit",    "unwrapOr(find(range(0, 10), fn(x: Int) -> Bool { x > 4 }), -1)", 5);
    check_int_p("stream_find_miss",   "unwrapOr(find(range(0, 3), fn(x: Int) -> Bool { x > 9 }), -1)", -1);
    check_int_p("stream_drop",        "sum(drop(range(0, 5), 2))", 9);          // 2+3+4
    check_int_p("stream_drop_all",    "sum(drop(range(0, 3), 10))", 0);         // drop more than available
    // any short-circuits on an INFINITE source (must terminate at the first hit).
    check_bool_p("stream_any_infinite", "any(iterate(0, fn(x: Int) -> Int { x + 1 }), fn(x: Int) -> Bool { x == 100 })", true);
    // O(1)-stack: any/all recurse per element -> TCO, so N=100000 completes without overflow.
    check_bool_p("stream_all_deep", "all(range(0, 100000), fn(x: Int) -> Bool { x >= 0 })", true);
}

void test_codegen_builtins() {
    std::cout << "[codegen: universal builtins P7c]\n";

    // toString(x): universal stringify -> a String (checked by content equality; the general EQ
    // opcode compares strings by content, so a freshly-built String equals a literal).
    check_bool("tostring_int",   "toString(42) == \"42\"", true);
    check_bool("tostring_bool",  "toString(true) == \"true\"", true);
    check_bool("tostring_dbl",   "toString(1.5) == \"1.5\"", true);
    // String concatenation via `+` (generic ADD, not ADD_INT).
    check_bool("str_concat",     "\"ab\" + \"cde\" == \"abcde\"", true);
    check_bool("concat_tostring","\"n=\" + toString(7) == \"n=7\"", true);

    // print / println write to the output sink.
    check_true("print_out",   cg_run_out("print(\"hi\")") == "hi");
    check_true("println_out", cg_run_out("println(\"hi\")") == "hi\n");
    check_true("println_bare", cg_run_out("println()") == "\n");
    check_true("print_int",   cg_run_out("print(42)") == "42");
    check_true("print_concat", cg_run_out("print(\"x=\" + toString(5))") == "x=5");
    // print returns Unit (Nil), so it composes as a statement before the block's value.
    check_int_p("print_then_value", "print(\"z\")\n 42", 42);
    // Multi-arg print/println: NO separator (back-to-back), newline once for println.
    check_true("print_multi_str",   cg_run_out("print(\"a\", \"b\")") == "ab");
    check_true("print_multi_mixed", cg_run_out("print(\"x=\", 5)") == "x=5");
    check_true("print_multi_three", cg_run_out("print(1, 2, 3)") == "123");
    check_true("println_multi",     cg_run_out("println(\"a\", \"b\")") == "ab\n");
    check_true("println_multi_mixed", cg_run_out("println(\"n=\", 42, \" b=\", true)") == "n=42 b=true\n");
    // print requires >= 1 arg (a type error otherwise); println(a, b, ...) now type-checks.
    check_true("print_zero_arg_rejected", cg_check_fails("print()"));
    check_true("println_multi_ok",        !cg_check_fails("println(1, 2)"));

    // panic aborts at runtime (a VM fault propagates out of execute()).
    check_true("panic_aborts", cg_faults("panic(\"boom\")"));

    // Prelude fallible extraction (built on panic).
    check_int_p("unwrap_some", "unwrap(Some(42))", 42);
    check_int_p("expect_some", "expect(Some(7), \"should be present\")", 7);
    check_int_p("unwrap",   "let r: Result[Int, Int] = Ok(42)\n unwrap(r)", 42);
    check_int_p("unwrapErr",  "let r: Result[Int, Int] = Err(9)\n unwrapErr(r)", 9);
    check_true("unwrap_none_panics", cg_faults("let o: Option[Int] = None\n unwrap(o)"));
    check_true("unwrap_ok_on_err_panics",
        cg_faults("let r: Result[Int, Int] = Err(1)\n unwrap(r)"));
    // The `Unwrap` trait folds expect/unwrapOr onto Result too (dispatched on the receiver).
    check_int_p("expect_ok",    "let r: Result[Int, Int] = Ok(7)\n expect(r, \"present\")", 7);
    check_true("expect_err_panics",
        cg_faults("let r: Result[Int, Int] = Err(1)\n expect(r, \"nope\")"));
    check_int_p("unwrapor_result_ok",  "let r: Result[Int, Int] = Ok(5)\n unwrapOr(r, 0)", 5);
    check_int_p("unwrapor_result_err", "let r: Result[Int, Int] = Err(3)\n unwrapOr(r, 9)", 9);
    // assert(cond, msg): a passing assert is a no-op; a failing one aborts.
    check_int_p("assert_pass", "assert(1 < 2, \"never\")\n 42", 42);
    check_true("assert_fail",  cg_faults("assert(1 > 2, \"nope\")"));
}

void test_codegen_maps() {
    std::cout << "[codegen: Map builtins P7d]\n";

    // has / membership (no prelude needed -- returns Bool).
    check_bool("has_present", "let m = #{1 => 10, 2 => 20}\n has(m, 1)", true);
    check_bool("has_absent",  "let m = #{1 => 10, 2 => 20}\n has(m, 3)", false);

    // keys / values snapshots (Array), indexed.
    check_int("keys_first",   "let m = #{5 => 50}\n let ks = keys(m)\n ks[0]", 5);
    check_int("values_first", "let m = #{5 => 50}\n let vs = values(m)\n vs[0]", 50);
    check_int("keys_len",     "let m = #{1 => 1, 2 => 2, 3 => 3}\n len(keys(m))", 3);

    // delete mutates in place (returns was-present Bool); len shrinks.
    check_bool("delete_present", "let m = #{1 => 10}\n delete(m, 1)", true);
    check_int("delete_shrinks",  "let m = #{1 => 10, 2 => 20}\n delete(m, 1)\n len(m)", 1);
    check_bool("delete_then_has", "let m = #{1 => 10}\n delete(m, 1)\n has(m, 1)", false);

    // get -> Option[V] (needs the prelude Option): Some on hit, None on miss.
    check_int_p("get_hit",  "let m = #{7 => 70}\n match get(m, 7) { Some(v) => v, None => -1 }", 70);
    check_int_p("get_miss", "let m = #{7 => 70}\n match get(m, 9) { Some(v) => v, None => -1 }", -1);
    // get composes with unwrapOr (prelude combinator over the Option it returns).
    check_int_p("get_unwrap_or", "let m = #{2 => 22}\n unwrapOr(get(m, 2), 0)", 22);
    // A String-keyed map (content-hashed keys).
    check_int_p("get_str_key",
        "let m = #{\"a\" => 1, \"b\" => 2}\n unwrapOr(get(m, \"b\"), -1)", 2);
    // get through the pipe.
    check_int_p("get_pipe", "let m = #{9 => 99}\n match m |> get(9) { Some(v) => v, None => 0 }", 99);

    // Type errors: a non-Map receiver, wrong key type.
    check_true("has_non_map",  cg_check_fails("has(42, 1)"));
    check_true("get_bad_key",  cg_check_fails("let m = #{1 => 10}\n get(m, \"x\")"));

    // get on SEQUENCES (Array/Vec/Bytes): 0 <= i < len ? Some(a[i]) : None -- the safe, total
    // counterpart to the trapping `a[i]`. Bounds-checked in codegen (ARRAY_GET sits inside the
    // in-bounds arm so OOB/negative never traps).
    check_int_p("get_array_hit", "let a = array(3, 7)\n unwrapOr(get(a, 1), -1)", 7);
    check_int_p("get_array_oob", "let a = array(3, 7)\n unwrapOr(get(a, 9), -1)", -1);
    check_int_p("get_array_neg", "let a = array(3, 7)\n unwrapOr(get(a, -1), -1)", -1);
    check_int_p("get_vec_hit",
        "let mut v: Vec[Int] = vec()\n push(v, 10)\n push(v, 20)\n unwrapOr(get(v, 1), -1)", 20);
    check_int_p("get_vec_oob",
        "let mut v: Vec[Int] = vec()\n push(v, 10)\n unwrapOr(get(v, 5), -1)", -1);
    check_int_p("get_bytes_hit", "unwrapOr(get(toBytes(\"abc\"), 0), -1)", 97);   // 'a'
    check_int_p("get_bytes_oob", "unwrapOr(get(toBytes(\"abc\"), 3), -1)", -1);
    check_bool_p("get_array_none", "isNone(get(array(2, 0), 5))", true);
    check_int_p("get_seq_pipe", "let a = array(2, 4)\n unwrapOr(a |> get(0), -1)", 4);
    check_true("get_seq_bad_index", cg_check_fails("get(array(2, 0), \"x\")"));    // non-Int index rejected

    // -- Map INDEX `m[k]` (MAP_GET_OR_TRAP read + MAP_SET write) -- the total, panic-on-miss path;
    //    `get(m,k) -> Option` (above) is the safe path. No prelude needed for indexing.
    check_int("mapidx_read",       "let m = #{1 => 10, 2 => 20}\n m[1] + m[2]", 30);
    check_int("mapidx_str_key",    "let m = #{\"a\" => 5, \"b\" => 6}\n m[\"a\"] + m[\"b\"]", 11);
    check_int("mapidx_write",      "let mut m = #{1 => 10}\n m[2] = 20\n m[1] + m[2]", 30);
    check_int("mapidx_overwrite",  "let mut m = #{1 => 10}\n m[1] = 99\n m[1]", 99);
    // `mut m: Map` param -- in-place insert visible to the caller (the mut-param payoff).
    check_int("mapidx_mut_param",
        "fn put(mut m: Map[Int, Int], k: Int, v: Int) -> Int { m[k] = v\n v }\n"
        " let mut m = #{1 => 10}\n let _ = put(m, 2, 20)\n m[1] + m[2]", 30);
    // A missing key TRAPS (located fault), mirroring a[i]'s OOB trap.
    check_true("mapidx_miss_traps", cg_faults("let m = #{1 => 10}\n m[9]"));
    // A write requires a `mut` root binding (H5), and a wrong key type is a type error.
    check_true("mapidx_write_needs_mut", cg_check_fails("let m = #{1 => 10}\n m[2] = 20\n m[1]"));
    check_true("mapidx_bad_key",         cg_check_fails("let m = #{1 => 10}\n m[\"x\"]"));
}

void test_codegen_vec_bytes() {
    std::cout << "[codegen: pop + Bytes builtins P7d]\n";

    // pop -> Option[T] (needs the prelude Option): Some(last) if non-empty, else None.
    check_int_p("pop_some",
        "let mut v = push(push(vec(), 1), 2)\n match pop(v) { Some(x) => x, None => -1 }", 2);
    check_int_p("pop_empty",
        "let mut v: Vec[Int] = vec()\n match pop(v) { Some(x) => x, None => -99 }", -99);
    // pop mutates in place -> len shrinks (bind to `_` to sidestep the must-use Option warning).
    check_int_p("pop_shrinks",
        "let mut v = push(push(vec(), 1), 2)\n let _ = pop(v)\n len(v)", 1);
    // pop composes with unwrapOr.
    check_int_p("pop_unwrap_or",
        "let mut v = push(vec(), 7)\n unwrapOr(pop(v), 0)", 7);
    // Regression: pop() whose ARGUMENT is a field access (a real temp), not a bare local. The Option-
    // wrapping holds THREE scratch temps (LEN/VEC_POP/zero) ABOVE the result AND the arg register; the
    // old +2 under-measured the frame by one for a non-local arg (a bare-local arg hid it). Found via
    // the language guide's generic Stack example; fixed in builtin_extra_slots(pop). Both a concrete
    // and a generic receiver -- the generic body is where the guide first hit it.
    check_int_p("pop_field_option",
        "struct Stk { xs: Vec[Int] }\n let mut s = Stk { xs: push(push(vec(), 1), 2) }\n"
        " match pop(s.xs) { Some(x) => x, None => 0 - 1 }", 2);
    check_int_p("pop_field_generic",
        "struct Stk[T] { xs: Vec[T] }\n fn sp[T](mut s: Stk[T]) -> Option[T] { pop(s.xs) }\n"
        " let mut s: Stk[Int] = Stk { xs: push(vec(), 9) }\n unwrapOr(sp(s), 0 - 1)", 9);

    // Bytes conversions (no prelude needed): encode + decode roundtrip, length, capacity.
    check_bool("bytes_roundtrip", "let b = toBytes(\"hi\")\n fromBytes(b) == \"hi\"", true);
    check_int("tobytes_len",      "len(toBytes(\"abc\"))", 3);
    check_int("bytes_empty_len",  "let b = bytes()\n len(b)", 0);
    check_int("bytes_cap_len",    "len(bytes(10))", 0);       // capacity hint: len stays 0

    // Static Bytes BUILDER (keystone): push/pop typed for Bytes (VM VEC_PUSH/VEC_POP are tri-kind).
    check_bool("bytes_build",     "fromBytes(push(push(bytes(), 104), 105)) == \"hi\"", true);
    check_int_p("bytes_pop",      "match pop(push(push(bytes(), 65), 66)) { Some(x) => x, None => -1 }", 66);
    check_int_p("bytes_pop_empty","match pop(bytes()) { Some(x) => x, None => -1 }", -1);
    check_true("bytes_push_bad",  cg_check_fails("push(bytes(), \"x\")"));   // a byte must be an Int
    // Bytes index read + assign (already supported; regression). Assign needs a `mut` root (M5).
    check_int("bytes_index_set",  "let mut b = toBytes(\"AAA\")\n b[1] = 66\n b[1]", 66);
    check_bool("bytes_index_roundtrip", "let mut b = toBytes(\"cat\")\n b[0] = 98\n fromBytes(b) == \"bat\"", true);
    check_true("bytes_index_set_immut", cg_check_fails("let b = toBytes(\"AAA\")\n b[0] = 66\n b[0]"));
    // Prelude string builders over the keystone: charStr (1-byte String), slice (byte-range substring).
    check_bool_p("charStr",       "charStr(65) == \"A\"", true);
    check_bool_p("slice",         "slice(\"hello world\", 6, 11) == \"world\"", true);
    check_bool_p("slice_clamp",   "slice(\"abc\", 1, 99) == \"bc\"", true);
    check_bool_p("slice_empty",   "slice(\"abc\", 2, 1) == \"\"", true);
    // sliceBytes: byte-range substring over an ALREADY-materialized Bytes (slice delegates to it) --
    // the O(range) primitive for a byte-scanner that holds one Bytes and slices many ranges (P2 finding).
    check_bool_p("sliceBytes",       "sliceBytes(toBytes(\"hello world\"), 6, 11) == \"world\"", true);
    check_bool_p("sliceBytes_clamp", "sliceBytes(toBytes(\"abc\"), 0 - 5, 99) == \"abc\"", true);
    check_bool_p("sliceBytes_empty", "sliceBytes(toBytes(\"abc\"), 2, 1) == \"\"", true);

    // Bulk byte-blit appendBytes (VM BYTES_APPEND): whole String src, whole Bytes src, chaining,
    // self-append, and empty. Returns the dst (chainable). BYTES_APPEND_RANGE is exercised via slice.
    check_bool("append_str",      "fromBytes(appendBytes(toBytes(\"foo\"), \"bar\")) == \"foobar\"", true);
    check_bool("append_bytes",    "fromBytes(appendBytes(toBytes(\"foo\"), toBytes(\"BAR\"))) == \"fooBAR\"", true);
    check_bool("append_chain",    "let mut b = bytes()\n appendBytes(appendBytes(b, \"a\"), \"bc\")\n fromBytes(b) == \"abc\"", true);
    check_bool("append_self",     "let mut b = toBytes(\"xy\")\n appendBytes(b, b)\n fromBytes(b) == \"xyxy\"", true);
    check_bool("append_empty",    "let mut b = toBytes(\"hi\")\n appendBytes(b, \"\")\n fromBytes(b) == \"hi\"", true);
    check_int("append_grows_len", "let mut b = bytes()\n appendBytes(b, \"abcdefghij\")\n len(b)", 10);
    check_true("append_bad_src",  cg_check_fails("appendBytes(bytes(), 5)"));      // src must be String/Bytes
    check_true("append_bad_dst",  cg_check_fails("appendBytes(\"s\", \"x\")"));     // dst must be Bytes

    // StringBuilder (pure prelude over BYTES_APPEND): method chaining, cap, single-byte, len.
    check_bool_p("sb_basic",   "stringBuilder().append(\"a\").append(\"bc\").build() == \"abc\"", true);
    check_bool_p("sb_chain",   "stringBuilder().append(\"x\").append(\"y\").build() == \"xy\"", true);
    check_bool_p("sb_cap",     "stringBuilderCap(4).append(\"data\").build() == \"data\"", true);
    check_bool_p("sb_byte",    "stringBuilder().append(\"A\").appendByte(66).build() == \"AB\"", true);
    check_int_p ("sb_len",     "let mut sb = stringBuilder()\n sb.append(\"hello\")\n sb.len()", 5);

    // Discard/void context: a statement-position if/match need not join its branches, and a `-> ()`
    // body may end in a non-unit expression -- the value is dropped (no more `put` wrapper needed).
    check_int("discard_if_hetero",     "let mut b = bytes()\n if 1 < 2 { push(b, 65) } else { push(b, 66) }\n len(b)", 1);
    check_int("discard_if_unit_mix",   "let mut b = bytes()\n let mut n = 0\n if 1 < 2 { push(b, 65) } else { n = 1 }\n len(b)", 1);
    check_int("unit_fn_nonunit_tail",  "fn f(mut b: Bytes) -> () { push(b, 65) }\n let mut b = bytes()\n f(b)\n len(b)", 1);
    check_int("discard_match_hetero",  "let mut b = bytes()\n match 1 { 1 => push(b, 65), _ => b }\n len(b)", 1);
    check_int("discard_while_body",    "let mut b = bytes()\n let mut i = 0\n while i < 3 { push(b, 65) i = i + 1 }\n len(b)", 3);
    check_int("discard_lambda_tail",   "let mut b = bytes()\n let f = fn() -> () { push(b, 65) }\n f()\n len(b)", 1);
    // Strictness preserved (NARROW): a genuine unit annotation / used unit context still errors.
    check_true("discard_let_unit_val", cg_check_fails("let x: () = 5\n 0"));
    check_true("discard_let_unit_if",  cg_check_fails("let x: () = if 1 < 2 { 1 } else { 2 }\n 0"));
    check_true("discard_nonunit_tail_checked", cg_check_fails("fn g() -> Int { push(bytes(), 1) }\n g()"));
    // A dropped MUST-USE value in a `-> ()` tail stays a HARD ERROR (owner decision); a must-use
    // value discarded as a STATEMENT stays an advisory warning (not an error).
    check_true("mustuse_tail_bytes_ok", !cg_check_fails("fn f(mut b: Bytes) -> () { push(b, 1) }\n let mut b = bytes()\n f(b)"));

    // `if let PAT = E { T } else { E }` -- refutable single-pattern binding, desugars to a 2-arm match.
    check_int_p("if_let_val",     "if let Some(x) = Some(5) { x } else { -1 }", 5);
    check_int_p("if_let_none",    "let o: Option[Int] = None\n if let Some(x) = o { x } else { -1 }", -1);
    check_int_p("if_let_stmt",    "let mut n = 0\n if let Some(x) = Some(7) { n = x }\n n", 7);       // no else
    check_int_p("if_let_stmt_miss","let mut n = 5\n let o: Option[Int] = None\n if let Some(x) = o { n = x }\n n", 5);
    check_int_p("if_let_result",  "if let Ok(v) = parseInt(\"42\") { v } else { -1 }", 42);
    check_int_p("if_let_else_if", "let o: Option[Int] = None\n if let Some(x) = o { x } else if let Ok(v) = parseInt(\"7\") { v } else { -1 }", 7);
    check_int_p("if_let_nested",  "if let Some(a) = Some(Some(9)) { if let Some(b) = a { b } else { -1 } } else { -2 }", 9);
    // Binding scope: the pattern's bindings live ONLY inside the then-branch (user enum -> no prelude).
    check_true("if_let_scope",    cg_check_fails("enum E { A(Int), B }\n if let A(x) = A(1) { x }\n x"));
    // Irrefutable pattern -> the tailored error (the `_` arm is unreachable).
    check_true("if_let_irrefutable",
        check_has("struct P { x: Int }\n let p = P { x: 1 }\n if let P { x } = p { x } else { 0 }", "irrefutable"));

    // `loop { … }` -- the condition-less loop; `break value` makes it an EXPRESSION.
    check_int("loop_break_val",   "let x = loop { break 5 }\n x", 5);
    check_int("loop_break_cond",  "let mut i = 0\n let x = loop { i = i + 1\n if i == 10 { break i } }\n x", 10);
    check_int("loop_counter_stmt","let mut n = 0\n loop { n = n + 3\n if n > 8 { break } }\n n", 9);   // no value
    check_int("loop_break_join",  "let c = 1 < 2\n let x = loop { if c { break 1 } else { break 2 } }\n x", 1);
    check_int("loop_annotated",   "let x: Int = loop { break 7 }\n x", 7);                    // check-position
    check_int("loop_in_fn_ret",   "fn f() -> Int { loop { break 7 } }\n f()", 7);
    check_int("loop_continue",    "let mut i = 0\n let mut s = 0\n let r = loop { i = i + 1\n if i > 5 { break s }\n if i == 3 { continue }\n s = s + i }\n r", 12);
    // A `break` targets the NEAREST loop: the inner one here, so the outer yields 2.
    check_int("loop_nested",      "let x = loop { let y = loop { break 1 }\n break y + 1 }\n x", 2);
    // A valueless `break` inside a nested `while` exits only the while.
    check_int("loop_while_inside","let mut i = 0\n let x = loop { while true { i = i + 1\n break }\n if i == 4 { break i } }\n x", 4);
    check_bool("loop_break_bool", "let x = loop { break 1 < 2 }\n x", true);
    // A break-less loop is `Never` -- it subsumes any expected type (an infinite loop diverges).
    check_true("loop_never_ok",   !cg_check_fails("fn f(b: Bool) -> Int { if b { 1 } else { loop { } } }"));
    // `break value` is loop-ONLY: a while/for has a fall-through exit yielding ().
    check_true("break_value_in_while", check_has("while 1 < 2 { break 5 }", "only allowed inside a 'loop'"));
    check_true("break_value_in_for",   check_has("for x in [1, 2] { break 5 }", "only allowed inside a 'loop'"));
    check_true("break_outside_loop",   check_has("break 5", "'break' outside of a loop"));
    // A `break` cannot escape a lambda into an enclosing loop (a lambda is a loop boundary).
    check_true("break_in_lambda",      check_has("loop { let f = fn() -> Int { break 1 }\n break 2 }", "'break' outside of a loop"));
    check_true("loop_type_mismatch",   cg_check_fails("let x: Bool = loop { break 5 }\n x"));
    check_true("loop_join_conflict",   check_has("let c = 1 < 2\n let x = loop { if c { break 1 } else { break true } }\n x",
                                                 "'break' values have incompatible types"));
    // A valueless break and a value break in one loop are likewise incompatible (() vs Int).
    check_true("loop_join_unit_value", cg_check_fails("let c = 1 < 2\n let x = loop { if c { break } else { break 1 } }\n x"));
    // A valueless break in a loop expected to yield a non-unit value.
    check_true("loop_bare_break_typed", cg_check_fails("let x: Int = loop { break }\n x"));
    // A `break` whose VALUE is itself a loop: inferring that value pushes a loop frame, and `infer_break`
    // held a reference into `loop_frames_` across it. When the push reallocated, the outer break's join
    // went to freed memory and the outer loop stayed `Never` -- which subsumes everything, so
    // `let y: Int = <String loop>` was accepted and printed a pointer. Whether it fired depended on the
    // vector's leftover capacity (no prelude: two levels; the std prelude: four), so nest deep enough
    // to outgrow any of it, and check both the unsound acceptance and the spurious rejection.
    {
        std::string open, close;
        for (int i = 0; i < 12; ++i) { open += "loop { break "; close += " }"; }
        check_true("loop_break_loop_value_typed",
            check_has("let x = " + open + "\"s\"" + close + "\n let y: Int = x\n y", "expected Int, found String"));
        check_true("loop_break_loop_value_typed_p",
            check_has_p("let x = " + open + "\"s\"" + close + "\n let y: Int = x\n y", "expected Int, found String"));
        check_int("loop_break_loop_value_match",
            "match (" + open + "2" + close + ") { 2 => 1, _ => 0 }", 1);
        check_int("loop_break_loop_two_levels", "match (loop { break loop { break 2 } }) { 2 => 1, _ => 0 }", 1);
    }

    // len(String) -- BYTE length (byte strings; identical to len(toBytes(s)), NOT code points).
    check_int("len_string",           "len(\"hello\")", 5);
    check_int("len_string_empty",     "len(\"\")", 0);
    check_bool("len_string_eq_bytes", "let s = \"world!\"\n len(s) == len(toBytes(s))", true);
    check_int("len_string_multibyte", "len(\"\xc3\xa9\")", 2);   // "e-acute" = 2 UTF-8 bytes -> byte count 2

    // Char literals -- pure lexer sugar for an Int (byte 0..255); the whole pipeline
    // treats them as Int, so they work in expressions, byte arithmetic, and patterns.
    check_int ("char_expr",     "'A'", 65);
    check_int ("char_arith",    "'a' - 'A'", 32);                      // byte arithmetic
    check_int ("char_in_match", "match 65 { 'A' => 1, _ => 0 }", 1);   // char literal as pattern
    check_bool("char_byte_cmp", "let s = \"{\"\n toBytes(s)[0] == '{'", true);  // JSON-scanner use case

    // toInt(x: Double) -> Int -- saturating (Java): NaN->0, +-inf/overflow->MAX/MIN_48, else trunc.
    check_int("toint_trunc_pos",  "toInt(3.9)", 3);
    check_int("toint_trunc_neg",  "toInt(-3.9)", -3);
    check_int("toint_exact",      "toInt(5.0)", 5);
    check_int("toint_of_int",     "toInt(7)", 7);                          // Int arg -> identity
    check_int("toint_nan",        "toInt(0.0 / 0.0)", 0);                  // NaN -> 0
    check_int("toint_pinf",       "toInt(1.0 / 0.0)", 140737488355327);    // +inf -> MAX_48
    check_int("toint_ninf",       "toInt(-1.0 / 0.0)", -140737488355328);  // -inf -> MIN_48
    check_int("toint_overflow",   "toInt(999999999999999.0)", 140737488355327);   // > MAX_48 -> MAX_48
    check_int("toint_underflow",  "toInt(-999999999999999.0)", -140737488355328); // < MIN_48 -> MIN_48
    // Checker: toInt wants a numeric arg; a String is a type error.
    check_true("toint_bad_arg",   cg_check_fails("toInt(\"x\")"));

    // Double rounding builtins (DROUND): Double -> Double. round = ties away; roundHalfToEven = ties even.
    check_bool("round_floor",     "floor(2.7) == 2.0", true);
    check_bool("round_floor_neg", "floor(0.0 - 2.3) == 0.0 - 3.0", true);
    check_bool("round_ceil",      "ceil(2.3) == 3.0", true);
    check_bool("round_ceil_neg",  "ceil(0.0 - 2.7) == 0.0 - 2.0", true);
    check_bool("round_trunc",     "trunc(2.7) == 2.0", true);
    check_bool("round_trunc_neg", "trunc(0.0 - 2.7) == 0.0 - 2.0", true);
    check_bool("round_round",     "round(2.5) == 3.0", true);            // ties away from zero
    check_bool("round_round_neg", "round(0.0 - 2.5) == 0.0 - 3.0", true);
    check_bool("round_round_lo",  "round(2.4) == 2.0", true);
    check_bool("round_even_25",   "roundHalfToEven(2.5) == 2.0", true);  // ties to even
    check_bool("round_even_35",   "roundHalfToEven(3.5) == 4.0", true);
    check_bool("round_even_neg",  "roundHalfToEven(0.0 - 2.5) == 0.0 - 2.0", true);
    check_bool("round_of_int",    "floor(5) == 5.0", true);              // Int arg -> widened, identity
    check_true("round_bad_arg",   cg_check_fails("floor(\"x\")"));

    // Exponent-notation Double literals flow through codegen like any Double constant.
    check_bool("exp_lit_value",   "1e3 == 1000.0", true);
    check_bool("exp_lit_neg",     "2.5e-1 == 0.25", true);
    check_int("exp_lit_toint",    "toInt(1.5e2)", 150);

    // parseInt / parseDouble -> Result[T, String] via std::from_chars (strict full-string parse).
    check_int_p("parseint_ok",       "match parseInt(\"42\") { Ok(n) => n, Err(_) => -1 }", 42);
    check_int_p("parseint_neg",      "match parseInt(\"-7\") { Ok(n) => n, Err(_) => 0 }", -7);
    check_int_p("parseint_bad",      "match parseInt(\"xy\") { Ok(n) => n, Err(_) => -1 }", -1);
    check_int_p("parseint_trailing", "match parseInt(\"12x\") { Ok(n) => n, Err(_) => -1 }", -1);
    check_int_p("parseint_empty",    "match parseInt(\"\") { Ok(n) => n, Err(_) => -1 }", -1);
    check_int_p("parseint_plus",     "match parseInt(\"+5\") { Ok(n) => n, Err(_) => -1 }", -1);  // no leading '+'
    check_int_p("parseint_range",    "match parseInt(\"999999999999999999\") { Ok(n) => n, Err(_) => -1 }", -1); // > MAX_48
    // parseDouble: fold the Double back to an Int via the just-built toInt so check_int_p can assert it.
    check_int_p("parsedouble_ok",    "match parseDouble(\"3.75\") { Ok(d) => toInt(d * 4.0), Err(_) => -1 }", 15);
    check_int_p("parsedouble_bad",   "match parseDouble(\"x\") { Ok(d) => toInt(d), Err(_) => -1 }", -1);
    // Checker: the argument must be a String.
    check_true("parseint_bad_arg",   cg_check_fails("parseInt(42)"));

    // ---- const item feature (S0): `[pub] const NAME: Type = <literal>`, compile-time inlined ----
    {
        auto no_compile = [](const std::string& s) { try { svc::compile(s.c_str()); return false; } catch (...) { return true; } };
        check_int ("const_int",       "const N: Int = 42\n N", 42);
        check_int ("const_neg",       "const N: Int = -7\n N", -7);
        check_int ("const_inlined",   "const N: Int = 10\n N * 2 + 1", 21);       // inlined at each use
        check_int ("const_double",    "const H: Double = 2.5\n toInt(H * 4.0)", 10);
        check_bool("const_bool",      "const YES: Bool = true\n YES", true);
        check_int ("const_upper",     "const HALF: Double = 3.5\n toInt(HALF * 2.0)", 7);   // uppercase name != ctor
        check_int ("const_char",      "const NL: Int = '\\n'\n NL", 10);          // char literal = Int
        check_int ("const_in_fn",     "const K: Int = 5\n fn f() -> Int { K + 1 }\n f()", 6);
        check_int ("const_shadow",    "const k: Int = 9\n fn f() -> Int { let k = 5\n k }\n f()", 5);  // local shadows
        check_true("const_type_mismatch", cg_check_fails("const X: Double = 3\n X"));   // Int lit in Double const
        check_true("const_dup",           cg_check_fails("const X: Int = 1\n const X: Int = 2\n X"));
        check_true("const_nonliteral",    no_compile("const X: Int = 1 + 2\n X"));      // parse: literal only
        check_true("const_pub_let",       no_compile("pub let X = 5\n X"));             // parse: pub only on fn/type/const
    }

    // ---- std::math library (S1 libm natives + S2 prelude helpers + PI/E consts) ----
    // Gating: a math native needs `use std::math` (else the checker rejects on the gate).
    check_true("m_gate_rejects",  cg_check_fails("sqrt(2.0)"));
    check_true("m_gate_ok",      !cg_check_fails("use std::math::*\n sqrt(2.0)"));
    // libm natives (exact-integer cases so toInt asserts them cleanly; the differential covers the rest).
    check_int_p("m_sqrt",   "use std::math::*\n toInt(sqrt(16.0))", 4);
    check_int_p("m_cbrt",   "use std::math::*\n toInt(cbrt(27.0))", 3);
    check_int_p("m_pow",    "use std::math::*\n toInt(pow(2.0, 10.0))", 1024);
    check_int_p("m_hypot",  "use std::math::*\n toInt(hypot(3.0, 4.0))", 5);
    check_int_p("m_log2",   "use std::math::*\n toInt(log2(8.0))", 3);
    check_int_p("m_abs",    "use std::math::*\n toInt(abs(-5.5) * 2.0)", 11);
    check_int_p("m_lnexp",  "use std::math::*\n toInt(exp(ln(7.0)) + 0.5)", 7);   // round-trip (+0.5 vs trunc)
    check_bool_p("m_isnan", "use std::math::*\n isNaN(sqrt(-1.0))", true);
    check_bool_p("m_isinf", "use std::math::*\n isInfinite(1.0 / 0.0)", true);
    check_bool_p("m_notnan","use std::math::*\n isNaN(sqrt(4.0))", false);
    // Constants (const feature).
    check_int_p("m_pi",     "use std::math::*\n toInt(PI * 100.0)", 314);
    check_int_p("m_e",      "use std::math::*\n toInt(E * 100.0)", 271);
    // Prelude helpers: minOf/maxOf/clamp generic over Ord (Int/Double/String), absInt/sign/gcd/lcm.
    check_int_p("m_minof",     "use std::math::*\n minOf(3, 7)", 3);
    check_int_p("m_maxof",     "use std::math::*\n maxOf(3, 7)", 7);
    check_int_p("m_minof_dbl", "use std::math::*\n toInt(minOf(2.5, 1.5) * 2.0)", 3);
    check_bool_p("m_minof_str","use std::math::*\n minOf(\"b\", \"a\") == \"a\"", true);
    check_int_p("m_clamp_hi",  "use std::math::*\n clamp(15, 0, 10)", 10);
    check_int_p("m_clamp_lo",  "use std::math::*\n clamp(-5, 0, 10)", 0);
    check_int_p("m_clamp_in",  "use std::math::*\n clamp(5, 0, 10)", 5);
    check_int_p("m_absint",    "use std::math::*\n absInt(-9)", 9);
    check_int_p("m_signint",   "use std::math::*\n signInt(-3) + signInt(0) + signInt(8)", 0);
    check_int_p("m_gcd",       "use std::math::*\n gcd(12, 18)", 6);
    check_int_p("m_lcm",       "use std::math::*\n lcm(4, 6)", 12);

    // ---- sort / sorted / sortBy (stable merge sort over Vec; std::iter ring, bare) ----
    check_int_p("sorted_int",   "let s = sorted(toVec([5, 3, 8, 1]))\n s[0]*1000 + s[1]*100 + s[2]*10 + s[3]", 1358);
    check_bool_p("sorted_str",  "let s = sorted(toVec([\"c\", \"a\", \"b\"]))\n s[0] == \"a\" && s[2] == \"c\"", true);
    check_bool_p("sorted_dbl",  "let s = sorted(toVec([2.5, 1.5, 3.5]))\n s[0] < s[1] && s[1] < s[2]", true);
    check_int_p("sortby_desc",  "let s = sortBy(toVec([1, 3, 2, 4]), fn(a: Int, b: Int) -> Bool { b < a })\n s[0]*1000 + s[1]*100 + s[2]*10 + s[3]", 4321);
    check_int_p("sort_inplace", "let mut v = toVec([3, 1, 2])\n sort(v)\n v[0]*100 + v[1]*10 + v[2]", 123);
    check_int_p("sort_dups",    "let s = sorted(toVec([3, 1, 3, 1, 2]))\n s[0]*10000 + s[1]*1000 + s[2]*100 + s[3]*10 + s[4]", 11233);
    check_int_p("sort_single",  "let s = sorted(toVec([7]))\n s[0]", 7);
    // a USER struct/enum may impl Ord and be sorted (locks the guide's §5/§19/§23 claim; erasure types cannot).
    check_int_p("sorted_user_ord",
        "struct P { k: Int }\n impl Ord for P { fn lessThan(self, o: P) -> Bool { self.k < o.k } }\n"
        " let s = sorted(toVec([P{k:3}, P{k:1}, P{k:2}]))\n s[0].k * 100 + s[1].k * 10 + s[2].k", 123);
    check_int_p("sort_empty",   "let e: Vec[Int] = vec()\n len(sorted(e))", 0);

    // std::math (OPT-IN): toIntChecked -- fallible Double->Int (Err on NaN/inf/overflow; else truncates).
    check_int_p("tointchecked_ok",   "use std::math::*\n match toIntChecked(3.9) { Ok(n) => n, Err(_) => -1 }", 3);
    check_int_p("tointchecked_neg",  "use std::math::*\n match toIntChecked(0.0 - 3.9) { Ok(n) => n, Err(_) => 0 }", -3);
    check_int_p("tointchecked_nan",  "use std::math::*\n match toIntChecked(0.0 / 0.0) { Ok(_) => 1, Err(_) => 0 }", 0);
    check_int_p("tointchecked_inf",  "use std::math::*\n match toIntChecked(1.0 / 0.0) { Ok(_) => 1, Err(_) => 0 }", 0);
    check_int_p("tointchecked_over", "use std::math::*\n match toIntChecked(999999999999999.0) { Ok(_) => 1, Err(_) => 0 }", 0);
    check_int_p("tointchecked_max",  "use std::math::*\n match toIntChecked(140737488355327.4) { Ok(n) => n - 140737488355327, Err(_) => -1 }", 0);

    // Type errors: toBytes wants a String; bytes() cap must be Int.
    check_true("tobytes_bad",  cg_check_fails("toBytes(42)"));
    check_true("bytes_bad_cap", cg_check_fails("bytes(\"x\")"));
}

void test_codegen_natives() {
    std::cout << "[codegen: native I/O]\n";
    namespace fs = std::filesystem;

    // A private scratch dir under the OS temp path. generic_string() gives forward slashes -- valid
    // in a Skarn string literal AND accepted by std::filesystem on Windows.
    std::error_code ec;
    const fs::path base = fs::temp_directory_path() / "svc_native_test";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const std::string f    = (base / "rt.txt").generic_string();
    const std::string sub  = (base / "sub").generic_string();
    const std::string miss = (base / "does_not_exist.txt").generic_string();

    // File round-trip via a Result fn + `?`: write bytes -> read back -> delete.
    {
        const std::string src =
            "fn go(p: String) -> Result[String, String] {\n"
            "  writeFile(p, toBytes(\"hello natives\"))?\n"
            "  let s = fromBytes(readFile(p)?)\n"
            "  deleteFile(p)?\n"
            "  Ok(s)\n"
            "}\n"
            "println(unwrap(go(\"" + f + "\")))\n";
        check_true("native_roundtrip", cg_run_native(src) == "hello natives\n");
    }
    // fileExists: true while present, false after delete (self-contained).
    {
        const std::string src =
            "fn setup(p: String) -> Result[Bool, String] { writeFile(p, toBytes(\"x\"))?  Ok(fileExists(p)) }\n"
            "println(toString(unwrap(setup(\"" + f + "\"))))\n"
            "let _ = deleteFile(\"" + f + "\")\n"
            "println(toString(fileExists(\"" + f + "\")))\n";
        check_true("native_file_exists", cg_run_native(src) == "true\nfalse\n");
    }
    // mkdir + listDir: exactly one file in a fresh dir -> len 1.
    {
        const std::string src =
            "fn go(d: String, f: String) -> Result[Int, String] {\n"
            "  mkdir(d)?\n"
            "  writeFile(f, toBytes(\"x\"))?\n"
            "  Ok(len(unwrap(listDir(d))))\n"
            "}\n"
            "println(toString(unwrap(go(\"" + sub + "\", \"" + sub + "/only.txt\"))))\n";
        check_true("native_mkdir_listdir", cg_run_native(src) == "1\n");
    }

    // args(): the harness supplies positional args.
    check_true("native_args_len",
        cg_run_native("println(toString(len(args())))", { "a", "b", "c" }) == "3\n");
    check_true("native_args_first",
        cg_run_native("println(args()[0])", { "alpha", "beta" }) == "alpha\n");

    // getEnv: a var we set in-process (Some), and an unlikely-unset one (None).
    _putenv_s("SVC_NAT_TEST", "marker42");
    check_true("native_getenv_some",
        cg_run_native("println(unwrapOr(getEnv(\"SVC_NAT_TEST\"), \"none\"))") == "marker42\n");
    check_true("native_getenv_none",
        cg_run_native("match getEnv(\"SVC_DEFINITELY_UNSET_ZZZ\") { Some(_) => println(\"y\"), None => println(\"n\") }")
        == "n\n");
    check_true("native_getenv_is_option",   // isSome (a prelude combinator over Option)
        cg_run_native("println(toString(isSome(getEnv(\"SVC_NAT_TEST\"))))") == "true\n");

    // stdin: readLine (first line) then readAllStdin (the whole stream).
    check_true("native_readline",
        cg_run_native("match readLine() { Some(l) => println(l), None => println(\"eof\") }",
                      {}, "first\nsecond\n") == "first\n");
    check_true("native_readall",
        cg_run_native("print(readAllStdin())", {}, "abc") == "abc");

    // time: nanoTime / millisTime are Int and non-negative.
    check_true("native_nanotime",   cg_run_native("println(toString(nanoTime() >= 0))")   == "true\n");
    check_true("native_millistime", cg_run_native("println(toString(millisTime() > 0))")  == "true\n");

    // Missing file: readFile(...)? -> Err propagates, handled by the caller.
    {
        const std::string src =
            "fn go(p: String) -> Result[String, String] { Ok(fromBytes(readFile(p)?)) }\n"
            "match go(\"" + miss + "\") { Ok(_) => println(\"ok\"), Err(_) => println(\"err\") }\n";
        check_true("native_missing_err", cg_run_native(src) == "err\n");
    }

    // Typing: arity + arg types are checked; a discarded Result native warns (must-use). The gated
    // I/O natives need `use std::io` in scope (else the check fails on the GATE, not the real defect).
    check_true("native_writefile_bad_arg", cg_check_fails("use std::io::*\nlet _ = writeFile(\"p\", \"not bytes\")"));
    check_true("native_readfile_arity",     cg_check_fails("use std::io::*\nlet _ = readFile(\"a\", \"b\")"));
    {   // a discarded Result inside a block warns (must-use tier); compile-only, no file access.
        svc::Module m = svc::compile("use std::io::*\nfn f() -> Int { readFile(\"x\")\n 0 }", svc::builtin_prelude());
        check_true("native_mustuse_warns", !m.warnings.empty());
    }

    // Native gating (S3): a gated native is REJECTED without its `use` (a CheckFailure carrying a
    // `use std::io` hint), even when everything else is fine.
    check_true("native_gate_rejects_without_use", cg_check_fails("let _ = fileExists(\"x\")"));
    check_true("native_gate_ok_with_use",         !cg_check_fails("use std::io::*\nlet _ = fileExists(\"x\")"));

    // Natives by NAME, like every other item. A native is reachable from its own module, through
    // `use M::*`, or through `use M::name` -- and through nothing else. Earlier an explicit
    // `use std::math::sqrt` was rejected as "private" (natives are not module declarations), while ANY
    // `use`/`import` of the module unlocked all of its natives (`use std::math::PI` made `cos` callable).
    // Prelude-linked: only then do std::math / std::io declare Skarn items of their own.
    check_int_p("native_use_by_name",        "use std::math::sqrt\n toInt(sqrt(9.0))", 3);
    check_int_p("native_use_by_name_braces", "use std::math::{sqrt, PI}\n toInt(sqrt(9.0) + PI)", 6);
    check_int_p("native_use_by_name_io",
        "use std::io::fileExists\n if fileExists(\"no/such/dir/at/all.xyz\") { 0 } else { 1 }", 1);
    check_int_p("native_use_by_name_hash",   "use std::hash::sha256\n len(sha256(bytes()))", 32);
    check_int_p("native_use_by_name_env",    "use std::env::nanoTime\n let _ = nanoTime()\n 1", 1);
    check_int_p("native_use_glob_still_ok",  "use std::math::*\n toInt(sqrt(9.0)) + toInt(cos(0.0))", 4);
    check_true("native_other_item_does_not_unlock",
        check_has_p("use std::math::PI\n toInt(sqrt(9.0))", "requires `use std::math::*` or `use std::math::sqrt`"));
    check_true("native_import_does_not_unlock",
        check_has_p("import std::math\n toInt(sqrt(9.0))", "requires `use std::math::*` or `use std::math::sqrt`"));
    check_true("native_named_unlocks_only_itself",
        check_has_p("use std::math::sqrt\n toInt(cos(0.0))", "requires `use std::math::*` or `use std::math::cos`"));
    // A name the module does not have says so, instead of calling it private (or saying nothing).
    check_true("use_unknown_item_std",     check_has_p("use std::math::nosuch\n 1", "module 'std::math' has no item 'nosuch'"));
    check_true("use_unknown_item_env",     check_has_p("use std::env::nosuch\n 1", "module 'std::env' has no item 'nosuch'"));
    check_true("use_unknown_module_std",   check_has_p("use std::nonexist::x\n 1", "unknown module 'std::nonexist'"));
    {
        using M = std::unordered_map<std::string, std::string>;
        auto modules_error_has = [](const std::string& entry, const M& mods, const std::string& substr) {
            try {
                svc::compile_modules(svc::load_modules(entry.c_str(), mem_resolver(mods)), nullptr);
                return false;
            } catch (const std::exception& e) { return std::string(e.what()).find(substr) != std::string::npos; }
        };
        const M LIB{{"lib", "pub fn shown() -> Int { 1 }\nfn hidden() -> Int { 2 }"}};
        check_true("use_unknown_item_module",
            modules_error_has("import lib\nuse lib::nosuch\n 1", LIB, "module 'lib' has no item 'nosuch'"));
        // Control: a real private item keeps its own diagnosis.
        check_true("use_private_item_module_message",
            modules_error_has("import lib\nuse lib::hidden\n 1", LIB, "item 'hidden' is private in module 'lib'"));
    }

    // Round-1 I/O natives: gated (need `use std::io`), and argument types are checked.
    check_true("native_appendfile_gate",  cg_check_fails("let _ = appendFile(\"p\", bytes())"));
    check_true("native_isfile_gate",      cg_check_fails("let _ = isFile(\"x\")"));
    check_true("native_filesize_gate",    cg_check_fails("let _ = fileSize(\"x\")"));
    check_true("native_appendfile_bad_arg", cg_check_fails("use std::io::*\nlet _ = appendFile(\"p\", \"not bytes\")"));
    check_true("native_isfile_ok",        !cg_check_fails("use std::io::*\nlet _ = isFile(\"x\")"));
    check_true("native_isdir_ok",         !cg_check_fails("use std::io::*\nlet _ = isDir(\"x\")"));
    {   // Pure-prelude text wrappers over the byte natives type-check (compile-only, prelude-linked).
        bool ok = true;
        try {
            svc::compile("use std::io::*\nfn f() -> Int { let _ = writeTextFile(\"p\", \"hi\")\n"
                         " let _ = readTextFile(\"p\")\n let _ = appendTextFile(\"p\", \"!\")\n 0 }",
                         svc::builtin_prelude());
        } catch (...) { ok = false; }
        check_true("native_texthelpers_compile", ok);
    }
    // Runtime: deterministic queries on a KNOWN-ABSENT path (isFile/isDir -> false, fileSize -> Err).
    check_bool_p("io_isfile_absent",  "use std::io::*\n isFile(\"svc_no_such_zzz_9x7\")", false);
    check_bool_p("io_isdir_absent",   "use std::io::*\n isDir(\"svc_no_such_zzz_9x7\")", false);
    check_bool_p("io_filesize_err",   "use std::io::*\n isErr(fileSize(\"svc_no_such_zzz_9x7\"))", true);

    // Round-2 I/O natives: rename / copyFile (two-path, Result). Gated + arity-checked.
    check_true("native_rename_gate",    cg_check_fails("let _ = rename(\"a\", \"b\")"));
    check_true("native_copyfile_gate",  cg_check_fails("let _ = copyFile(\"a\", \"b\")"));
    check_true("native_rename_arity",   cg_check_fails("use std::io::*\nlet _ = rename(\"a\")"));
    {   // rename / copyFile type-check + codegen (prelude-linked; compile-only, no file access).
        bool ok = true;
        try {
            svc::compile("use std::io::*\nfn f() -> Int { let _ = rename(\"a\", \"b\")\n"
                         " let _ = copyFile(\"c\", \"d\")\n 0 }", svc::builtin_prelude());
        } catch (...) { ok = false; }
        check_true("native_rename_copy_compile", ok);
    }
    // Runtime: rename / copyFile on a KNOWN-ABSENT source -> Err (deterministic, no side effect).
    check_bool_p("io_rename_absent_err",   "use std::io::*\n isErr(rename(\"svc_no_such_zzz_9x7\", \"svc_dst_zzz\"))", true);
    check_bool_p("io_copyfile_absent_err", "use std::io::*\n isErr(copyFile(\"svc_no_such_zzz_9x7\", \"svc_dst_zzz\"))", true);

    // Prelude gate: a Result/Option native needs the prelude to build Ok/Err / Some/None; a Plain
    // native (nanoTime) does not. (Both are std::env-gated, so `use std::env` is in scope here.)
    check_true("native_no_prelude_rejects", cg_rejects("use std::env::*\nlet _ = getEnv(\"PATH\")"));
    check_true("native_plain_no_prelude_ok",
        !cg_rejects("use std::env::*\nlet _ = nanoTime()") && !cg_check_fails("use std::env::*\nlet _ = nanoTime()"));

    fs::remove_all(base, ec);
}

// Process spawn (run / runWith / runText / sh). These launch real cmd.exe children via the rawRun
// native; the results are reshaped into the ProcessOutput / ProcessText prelude structs by codegen's
// emit_wrap_process. Assertions use deterministic exit codes + `echo` output.
void test_codegen_process() {
    std::cout << "[codegen: process spawn]\n";

    // Exit code flows through: `cmd /c exit 7` -> ProcessOutput.exitCode == 7. Also proves the
    // array->struct reshape put slot 2 (exitInt) into the exitCode field.
    check_true("process_exit_code", cg_run_native(
        "match run([\"cmd\", \"/c\", \"exit\", \"7\"]) { Ok(o) => println(toString(o.exitCode)), Err(_) => println(\"err\") }")
        == "7\n");

    // A spawn failure (program not found) is Err -- NOT a non-zero exit (Rust Command::output).
    check_true("process_spawn_err", cg_run_native(
        "match run([\"svc_definitely_not_a_real_program_zzz_qqq\"]) { Ok(_) => println(\"ran\"), Err(_) => println(\"err\") }")
        == "err\n");

    // runText decodes stdout to a String and keeps exitCode: `echo hi` -> "hi\r\n".
    check_true("process_run_text", cg_run_native(
        "match runText([\"cmd\", \"/c\", \"echo\", \"hi\"]) { Ok(t) => print(t.stdout), Err(_) => print(\"err\") }")
        == "hi\r\n");

    // sh wraps cmd.exe; the cmdline reaches the child (echo's output confirms the quoting).
    check_true("process_sh", cg_run_native(
        "match sh(\"echo ok\") { Ok(o) => print(fromBytes(o.stdout)), Err(_) => print(\"err\") }")
        == "ok\r\n");

    // runWith feeds stdin (Bytes) to the child; `sort` reads it and exits 0.
    check_true("process_run_with", cg_run_native(
        "match runWith([\"cmd\", \"/c\", \"sort\"], toBytes(\"b\\na\\n\")) { Ok(o) => println(toString(o.exitCode)), Err(_) => println(\"err\") }")
        == "0\n");

    // Typing through erasure: the named ProcessOutput/ProcessText resolve `.exitCode`/`.stdout`
    // THROUGH the generic `unwrap` (the static win the dynamic side could not offer). Compile-only
    // (running would spawn a bogus program), so we just require a clean compile.
    auto compiles = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return true; }
        catch (...) { return false; }
    };
    check_true("process_typing_ok", compiles(
        "use std::process::*\n"
        "fn a() -> Int { unwrap(run([\"x\"])).exitCode }\n"
        "fn b() -> String { unwrap(runText([\"x\"])).stdout }\n"));

    // Prelude gate: the direct rawRun native needs the prelude (Ok/Err/ProcessOutput) to reshape the
    // result -> a clean CodegenError without it (the `use` passes the S3 native gate first); and
    // `run` (a prelude fn) is undefined without the prelude regardless.
    check_true("process_rawrun_no_prelude_rejects", cg_rejects("use std::process::*\nlet _ = rawRun(vec(), bytes())"));
    check_true("process_run_needs_prelude",        cg_check_fails("let _ = run([\"x\"])"));
}

// ---- std::bytes: little-endian binary reader/writer (OPT-IN, pure prelude, no VM change) ----
void check_same(const char* name, const std::string& src, bool with_prelude);   // defined below
void test_std_bytes() {
    std::cout << "[codegen: std::bytes]\n";
    const std::string U = "use std::bytes::*\n";
    // A prelude-linked "fails to compile" check: std::bytes fns are pure-prelude opt-in items, so
    // they resolve only WITH the prelude AND a `use std::bytes` (unlike a native, gated at the seam).
    auto p_fails = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return false; }
        catch (const svc::CheckFailure&) { return true; }
        catch (...) { return false; }
    };
    // Gating: bare readU8/reader without `use std::bytes` is out of scope -> a CheckFailure.
    check_true("bytes_gate_rejects", p_fails("let mut r = ByteReader::new(bytes())\n r.readU8()"));
    check_true("bytes_gate_ok",     !p_fails(U + "let mut r = ByteReader::new(bytes())\n match r.readU8() { Ok(_) => 1, Err(_) => 0 }"));

    // Roundtrips: write then read back, asserting the exact Int (unwrap via match to dodge Option/Result plumbing).
    check_int_p("bytes_u8",
        U + "let mut b = bytes()\n writeU8(b, 200)\n let mut r = ByteReader::new(b)\n match r.readU8() { Ok(n) => n, Err(_) => -1 }", 200);
    check_int_p("bytes_u16",
        U + "let mut b = bytes()\n writeU16LE(b, 40000)\n let mut r = ByteReader::new(b)\n match r.readU16LE() { Ok(n) => n, Err(_) => -1 }", 40000);
    check_int_p("bytes_u32",
        U + "let mut b = bytes()\n writeU32LE(b, 4000000000)\n let mut r = ByteReader::new(b)\n match r.readU32LE() { Ok(n) => n, Err(_) => -1 }", 4000000000);
    check_int_p("bytes_i32_neg",
        U + "let mut b = bytes()\n writeI32LE(b, 0 - 12345)\n let mut r = ByteReader::new(b)\n match r.readI32LE() { Ok(n) => n, Err(_) => 0 }", -12345);
    check_int_p("bytes_i32_minus1",
        U + "let mut b = bytes()\n writeI32LE(b, 0 - 1)\n let mut r = ByteReader::new(b)\n match r.readI32LE() { Ok(n) => n, Err(_) => 0 }", -1);
    // Endianness: u16 300 -> [0x2C=44, 0x01=1]; index the raw buffer.
    check_int_p("bytes_le_order",
        U + "let mut b = bytes()\n writeU16LE(b, 300)\n b[0] * 1000 + b[1]", 44001);
    // varint: 0 (1 byte), 127 (1 byte boundary), 128 (2 bytes), and a large multibyte value roundtrip.
    check_int_p("bytes_varu_0",    U + "let mut b = bytes()\n writeVarU(b, 0)\n len(b)", 1);
    check_int_p("bytes_varu_128len",U + "let mut b = bytes()\n writeVarU(b, 128)\n len(b)", 2);
    check_int_p("bytes_varu_rt",
        U + "let mut b = bytes()\n writeVarU(b, 123456789)\n let mut r = ByteReader::new(b)\n match r.readVarU() { Ok(n) => n, Err(_) => -1 }", 123456789);
    check_int_p("bytes_varu_max",
        U + "let mut b = bytes()\n writeVarU(b, 140737488355327)\n let mut r = ByteReader::new(b)\n match r.readVarU() { Ok(n) => n, Err(_) => -1 }", 140737488355327);
    // writeBytes/readBytes roundtrip a chunk; readBytes(n) advances the cursor.
    check_int_p("bytes_chunk",
        U + "let mut b = bytes()\n writeBytes(b, toBytes(\"hi\"))\n let mut r = ByteReader::new(b)\n"
            "match r.readBytes(2) { Ok(c) => len(c) * 100 + c[0], Err(_) => -1 }", 304);   // len 2, 'h'=104
    // remaining/atEnd track the cursor.
    check_int_p("bytes_remaining",
        U + "let mut b = bytes()\n writeU8(b, 1)\n writeU8(b, 2)\n writeU8(b, 3)\n let mut r = ByteReader::new(b)\n"
            "match r.readU8() { Ok(_) => r.remaining(), Err(_) => -1 }", 2);
    check_bool_p("bytes_atend",
        U + "let mut b = bytes()\n writeU8(b, 9)\n let mut r = ByteReader::new(b)\n match r.readU8() { Ok(_) => r.atEnd(), Err(_) => false }", true);

    // Underrun: reading past the end is Err (total, never a trap).
    check_int_p("bytes_underrun_u8",
        U + "let mut r = ByteReader::new(bytes())\n match r.readU8() { Ok(_) => 1, Err(_) => 0 }", 0);
    check_int_p("bytes_underrun_u32",
        U + "let mut b = bytes()\n writeU8(b, 1)\n let mut r = ByteReader::new(b)\n match r.readU32LE() { Ok(_) => 1, Err(_) => 0 }", 0);
    check_int_p("bytes_readbytes_over",
        U + "let mut b = bytes()\n writeU8(b, 1)\n let mut r = ByteReader::new(b)\n match r.readBytes(5) { Ok(_) => 1, Err(_) => 0 }", 0);
    // A malformed varint (all continuation bits set) exceeds the 48-bit budget -> Err.
    check_int_p("bytes_varu_toolong",
        U + "let mut b = bytes()\n let mut i = 0\n while i < 10 { push(b, 255) i = i + 1 }\n"
            "let mut r = ByteReader::new(b)\n match r.readVarU() { Ok(_) => 1, Err(_) => 0 }", 0);

    // signed varint (zigzag): 0/-1/1 map to short codes; roundtrip across the full 48-bit signed edges.
    check_int_p("bytes_vari_0",
        U + "let mut b = bytes()\n writeVarI(b, 0)\n let mut r = ByteReader::new(b)\n match r.readVarI() { Ok(n) => n, Err(_) => 1 }", 0);
    check_int_p("bytes_vari_neg1_len", U + "let mut b = bytes()\n writeVarI(b, 0 - 1)\n len(b)", 1);   // zigzag(-1)=1 -> 1 byte
    check_int_p("bytes_vari_neg",
        U + "let mut b = bytes()\n writeVarI(b, 0 - 123456789)\n let mut r = ByteReader::new(b)\n match r.readVarI() { Ok(n) => n, Err(_) => 0 }", -123456789);
    check_int_p("bytes_vari_pos",
        U + "let mut b = bytes()\n writeVarI(b, 987654321)\n let mut r = ByteReader::new(b)\n match r.readVarI() { Ok(n) => n, Err(_) => 0 }", 987654321);
    check_int_p("bytes_vari_min48",   // -2^47 (MIN_48 via the sign-extended hex literal), the 7-byte-code edge
        U + "let mut b = bytes()\n writeVarI(b, 0x800000000000)\n let mut r = ByteReader::new(b)\n match r.readVarI() { Ok(n) => n, Err(_) => 0 }", -140737488355328LL);
    check_int_p("bytes_vari_max48",   // 2^47-1, the largest positive
        U + "let mut b = bytes()\n writeVarI(b, 140737488355327)\n let mut r = ByteReader::new(b)\n match r.readVarI() { Ok(n) => n, Err(_) => 0 }", 140737488355327LL);
    // small magnitudes stay short: zigzag(-64)=127 -> 1 byte, zigzag(64)=128 -> 2 bytes.
    check_int_p("bytes_vari_neg64_len", U + "let mut b = bytes()\n writeVarI(b, 0 - 64)\n len(b)", 1);
    check_int_p("bytes_vari_pos64_len", U + "let mut b = bytes()\n writeVarI(b, 64)\n len(b)", 2);

    // length-prefixed String: writeStr -> readStr roundtrip; empty string; the prefix advances the cursor.
    check_bool_p("bytes_str_rt",
        U + "let mut b = bytes()\n writeStr(b, \"hello\")\n let mut r = ByteReader::new(b)\n match r.readStr() { Ok(s) => s == \"hello\", Err(_) => false }", true);
    check_bool_p("bytes_str_empty",
        U + "let mut b = bytes()\n writeStr(b, \"\")\n let mut r = ByteReader::new(b)\n match r.readStr() { Ok(s) => s == \"\", Err(_) => false }", true);
    check_int_p("bytes_str_prefix_len",   // "hi" -> varU(2)=1 byte + 2 bytes = 3
        U + "let mut b = bytes()\n writeStr(b, \"hi\")\n len(b)", 3);
    check_bool_p("bytes_str_two",         // two strings back-to-back read in order
        U + "let mut b = bytes()\n writeStr(b, \"foo\")\n writeStr(b, \"bar\")\n let mut r = ByteReader::new(b)\n"
            "match r.readStr() { Ok(a) => match r.readStr() { Ok(c) => a == \"foo\" && c == \"bar\", Err(_) => false }, Err(_) => false }", true);
    check_int_p("bytes_str_underrun",     // a truncated string body -> Err
        U + "let mut b = bytes()\n writeVarU(b, 5)\n writeU8(b, 65)\n let mut r = ByteReader::new(b)\n match r.readStr() { Ok(_) => 1, Err(_) => 0 }", 0);

    // Differential (write -> read through the real VM AND the RefEval oracle): pure Skarn, so both
    // sides run the same module -- confirms the oracle executes it consistently.
    check_same("diff_bytes_u32",
        U + "let mut b = bytes()\n writeU32LE(b, 305419896)\n let mut r = ByteReader::new(b)\n unwrap(r.readU32LE())", true);
    check_same("diff_bytes_varu",
        U + "let mut b = bytes()\n writeVarU(b, 999999)\n writeVarU(b, 7)\n let mut r = ByteReader::new(b)\n"
            "let a = unwrap(r.readVarU())\n let c = unwrap(r.readVarU())\n a - c", true);
    check_same("diff_bytes_i32",
        U + "let mut b = bytes()\n writeI32LE(b, 0 - 2000000000)\n let mut r = ByteReader::new(b)\n unwrap(r.readI32LE())", true);
    check_same("diff_bytes_vari",
        U + "let mut b = bytes()\n writeVarI(b, 0 - 777777)\n writeVarI(b, 555)\n let mut r = ByteReader::new(b)\n"
            "let a = unwrap(r.readVarI())\n let c = unwrap(r.readVarI())\n a + c", true);
    check_same("diff_bytes_str",
        U + "let mut b = bytes()\n writeStr(b, \"vMachine\")\n let mut r = ByteReader::new(b)\n len(unwrap(r.readStr()))", true);
}

// End-to-end (codegen + VM) values for radix literals, incl. the negative-valued (sign-extended)
// cases -- the FIRST front-end path to drive load_const with a negative -- and a differential check.
void test_radix_literals() {
    std::cout << "[codegen: radix literals]\n";
    check_int("cg_hex",        "0xFF", 255);
    check_int("cg_hex_u32",    "0xFFFFFFFF", 4294967295LL);
    check_int("cg_hex_min48",  "0x800000000000", -140737488355328LL);   // sign-extended MIN_48 load_const
    check_int("cg_hex_neg1",   "0xFFFFFFFFFFFF", -1);
    check_int("cg_oct",        "0o7777", 4095);
    check_int("cg_bin",        "0b111111111111", 4095);
    check_int("cg_hex_or",     "0xF0 | 0x0F", 255);
    check_int("cg_hex_and",    "0xFF & 0x0F", 15);
    check_int("cg_hex_mask48", "0xFFFFFFFFFFFF & 123", 123);            // full-width mask (-1) & 123
    check_int("cg_hex_subneg", "0 - 0xFF", -255);
    check_int("cg_oct_add",    "0o17 + 1", 16);
    // VM == RefEval oracle for a negative-valued literal.
    check_same("diff_hex_neg", "0xFFFFFFFFFFFF & 123", false);

    // Digit separators are purely lexical, so the end-to-end value must be identical to the
    // un-separated spelling in every form -- including the sign-extended full-width mask.
    check_int("cg_sep_dec",   "1_000_000", 1000000);
    check_int("cg_sep_hex",   "0xFF_FF", 65535);
    check_int("cg_sep_mask",  "0xFFFF_FFFF_FFFF", -1);
    check_int("cg_sep_bin",   "0b1010_1010", 170);
    check_int("cg_sep_expr",  "1_0 * 1_0 + 0o1_0", 108);
    check_same("diff_sep",    "1_000_000 + 0xFF_FF", false);
}

// Chained place-path lvalue writes (`a[i].field = v` and friends): VM == RefEval oracle. The
// oracle handles chains for free (reference semantics: it evaluates the base via generic eval(),
// objects are shared), so no oracle change was needed.
void test_chained_lvalue_diff() {
    std::cout << "[codegen: chained lvalue differential]\n";
    check_same("diff_chain_nested_field",
        "struct P { x: Int, y: Int }\n struct N { p: P }\n"
        " let mut n = N { p: P { x: 9, y: 10 } }\n n.p.x = 60\n n.p.x + n.p.y", false);
    check_same("diff_chain_index_index",
        "let mut vv = vec()\n let mut r = vec()\n push(r, 100)\n push(r, 200)\n push(vv, r)\n"
        " vv[0][1] = 300\n vv[0][0] + vv[0][1]", false);
    check_same("diff_chain_vec_field",
        "struct P { x: Int, y: Int }\n let mut v = vec()\n"
        " push(v, P { x: 1, y: 2 })\n push(v, P { x: 3, y: 4 })\n"
        " v[0].x = 40\n v[0].x + v[1].y", false);
}

// Record-style enum variants `V { f: T }`: VM == RefEval oracle. Construction + match (RefEval's
// ctors_/StructPat already carry record-variant field names) and the `${..}` dump (Struct DumpStyle,
// mirrored by the oracle's render_dump).
void test_record_variant_diff() {
    std::cout << "[codegen: record variant differential]\n";
    check_same("diff_rvariant_match",
        "enum Shape { Dot, Circle { r: Int }, Rect { w: Int, h: Int } }\n"
        "fn area(s: Shape) -> Int { match s { Dot => 0, Circle { r } => r * r, Rect { w, h } => w * h } }\n"
        "println(area(Circle { r: 5 }))\n println(area(Rect { w: 3, h: 4 }))\n println(area(Dot))", false);
    check_same("diff_rvariant_dump",
        "enum Shape { Circle { r: Int }, Rect { w: Int, h: Int } }\n"
        "let a = Circle { r: 7 }\n let b = Rect { w: 2, h: 3 }\n"
        "println(\"${a} ${b}\")", false);
}

// Raw string literals end-to-end: a raw string is an ordinary StrLit (already oracle-covered), so the
// differentials agree for free; the value checks confirm the byte-exact, escape-free, hash-form content.
void test_raw_strings_cg() {
    std::cout << "[codegen: raw strings]\n";
    check_int ("raw_cg_no_escape", "len(r\"a\\nb\")", 4);      // a \ n b -- no escape processing
    check_int ("raw_cg_no_interp", "len(r\"${x}\")", 4);       // ${x} literal, no interpolation
    check_int ("raw_cg_empty",     "len(r\"\")", 0);
    check_int ("raw_cg_hash_quote","len(r#\"a\"b\"#)", 3);     // embedded quote via hash form
    check_bool("raw_cg_eq_plain",  "r\"x\" == \"x\"", true);   // same bytes as a plain literal
    check_bool("raw_cg_dollar",    "r\"$x\" == \"$x\"", true); // a lone `$` is literal in both
    // Differential vs the RefEval oracle (StrLit path -- agrees by construction).
    check_same("diff_raw_len",     "len(r\"a\\nb\") + len(r#\"a\"b\"#)", false);
    check_same("diff_raw_eq",      "if r\"${x}\" == \"$\" + \"{x}\" { 1 } else { 0 }", false);
}

// Round-2 batch: #1 else-match diagnostic, #2 while let, #3 range patterns, #4 `%g` formatter.
void test_hashable() {
    std::cout << "[test_hashable: map-key / set-element bound]\n";

    // ---- positives: every permitted key type constructs + runs (prelude-linked) ----
    check_int_p ("hash_map_int",    "let m: Map[Int, Int] = #{ 1 => 10, 2 => 20 }\n getOr(m, 2, 0)", 20);
    check_int_p ("hash_map_string", "let m: Map[String, Int] = #{ \"a\" => 1 }\n getOr(m, \"a\", 0)", 1);
    check_int_p ("hash_map_infer",  "let m = #{ 3 => 7 }\n getOr(m, 3, 0)", 7);           // inferred key type
    check_bool_p("hash_map_bool",   "let m: Map[Bool, Bool] = #{ true => false }\n getOr(m, true, true)", false);
    check_int_p ("hash_map_double", "let m: Map[Double, Int] = #{ 1.5 => 9 }\n getOr(m, 1.5, 0)", 9);

    // Set positives (opt-in module): Set::fromVec (arg-driven), Set::new() (return-driven), insert.
    check_int_p("hash_set_setof",
        "use std::set::*\n let mut v: Vec[Int] = vec()\n push(v, 1) push(v, 2) push(v, 2)\n Set::fromVec(v).size()", 2);
    check_int_p("hash_set_empty", "use std::set::*\n let s: Set[String] = Set::new()\n s.size()", 0);
    check_int_p("hash_set_insert",
        "use std::set::*\n let mut s: Set[Int] = Set::new()\n s.insert(5) s.insert(5) s.insert(7)\n s.size()", 2);

    // A user generic fn that CONSTRUCTS a map, correctly bounded, propagates + runs.
    check_int_p("hash_user_bound_ok",
        "fn mk[K: Hashable](k: K, v: Int) -> Map[K, Int] { let mut m: Map[K, Int] = #{}\n m[k] = v\n m }\n"
        "getOr(mk(7, 42), 7, 0)", 42);

    // ---- negatives: a non-hashable key is a COMPILE error (prelude-linked so the trait exists) ----
    // direct Map literal / annotation with a heap key type
    check_true("hash_map_struct_key",
        cg_check_fails_p("struct P { x: Int }\n let m: Map[P, Int] = #{}\n 0"));
    check_true("hash_map_struct_key_nonempty",
        cg_check_fails_p("struct P { x: Int }\n let p = P { x: 1 }\n let m = #{ p => 1 }\n 0"));
    check_true("hash_map_tuple_key",
        cg_check_fails_p("let m: Map[(Int, Int), Int] = #{}\n 0"));
    check_true("hash_map_vec_key",
        cg_check_fails_p("let m: Map[Vec[Int], Int] = #{}\n 0"));
    check_true("hash_map_enum_key",
        cg_check_fails_p("enum E { A, B }\n let m: Map[E, Int] = #{}\n 0"));

    // Set of a heap type -- via Set::fromVec (arg-pinned) AND via nullary Set::new() (return-pinned; needs the
    // general return-type-directed bound discharge, else this leaks to a runtime trap).
    check_true("hash_set_struct_setof",
        cg_check_fails_p("use std::set::*\n struct P { x: Int }\n let v: Vec[P] = vec()\n push(v, P { x: 1 })\n let s = Set::fromVec(v)\n 0"));
    check_true("hash_set_struct_nullary",
        cg_check_fails_p("use std::set::*\n struct P { x: Int }\n let s: Set[P] = Set::new()\n 0"));

    // An UNBOUNDED generic that constructs a map is rejected; adding `K: Hashable` fixes it.
    check_true("hash_unbounded_generic_rejected",
        cg_check_fails_p("fn mk[K]() -> Map[K, Int] { let m: Map[K, Int] = #{}\n m }\n 0"));
    check_true("hash_bounded_generic_ok",
        !cg_check_fails_p("fn mk[K: Hashable]() -> Map[K, Int] { let m: Map[K, Int] = #{}\n m }\n 0"));

    // A user's own `T: Hashable` bound discharges for a permitted type and rejects a heap type.
    check_true("hash_user_bound_int_ok",
        !cg_check_fails_p("fn needs[T: Hashable](x: T) -> Int { 0 }\n needs(5)"));
    check_true("hash_user_bound_struct_no",
        cg_check_fails_p("struct P { x: Int }\n fn needs[T: Hashable](x: T) -> Int { 0 }\n needs(P { x: 1 })"));
}

// Transparent (erased) newtypes: `transparent struct UserId(Int)` is a distinct type in the checker
// but IS the bare Int/Double/Bool immediate at runtime -- construction/`.0`/pattern are no-ops, no
// heap object. #0 (mechanism) + #T (RefEval + generator learn it) -- toString still lossy (prints
// the bare value); the name table is the follow-up slice #1.
void test_transparent_structs() {
    std::cout << "[test_transparent_structs: erased newtype #0]\n";

    // ---- positives: construct / project / match / bind / equality / coercion ----
    check_int_p ("ts_construct_project", "transparent struct UserId(Int)\n UserId(7).0", 7);
    check_int_p ("ts_match",             "transparent struct UserId(Int)\n match UserId(7) { UserId(n) => n }", 7);
    check_int_p ("ts_match_literal",     "transparent struct UserId(Int)\n match UserId(3) { UserId(0) => 100, UserId(n) => n }", 3);
    check_int_p ("ts_let_destructure",   "transparent struct UserId(Int)\n let UserId(n) = UserId(9)\n n", 9);
    check_bool_p("ts_equal",             "transparent struct UserId(Int)\n UserId(1) == UserId(1)", true);
    check_bool_p("ts_notequal",          "transparent struct UserId(Int)\n UserId(1) == UserId(2)", false);
    check_int_p ("ts_double_coerce",     "transparent struct Meters(Double)\n let m = Meters(5)\n toInt(m.0)", 5);
    check_int_p ("ts_bool_underlying",   "transparent struct Flag(Bool)\n match Flag(true) { Flag(b) => if b { 1 } else { 0 } }", 1);
    check_int_p ("ts_arith_on_field",    "transparent struct UserId(Int)\n let u = UserId(20)\n u.0 + u.0 + 2", 42);

    // ---- a transparent ctor NESTED in another pattern ----
    // Every nesting parent fetches the field into a TEMP and frees it right after the sub-test, so
    // binding the erased element by register ALIAS left the name pointing at a register the arm body
    // immediately reallocated -- a silent wrong VALUE (`g(3) + n` read the call result twice: 600,
    // not 307). Invisible to the emitter self-check: measure_pattern's Ctor case already reserved
    // the local, emit just never allocated it. The top-level form (`ts_match` above) and the
    // irrefutable `let` binder were always correct, which is why nothing caught it.
    // The arm body MUST allocate a temp before reading the binding, or the bug does not show.
    {
        const std::string pre = "transparent struct W(Int)\n fn g(x: Int) -> Int { x * 100 }\n";
        check_int_p("ts_nested_enum_payload", pre +
            "fn f(o: Option[W]) -> Int { match o { Some(W(n)) => g(3) + n, None => -1 } }\n f(Some(W(7)))", 307);
        check_int_p("ts_nested_record_field", pre + "struct P { a: W, b: Int }\n"
            "fn f(p: P) -> Int { match p { P { a: W(n), b: _ } => g(3) + n } }\n f(P { a: W(7), b: 0 })", 307);
        check_int_p("ts_nested_tuple_struct", pre + "struct T2(W, Int)\n"
            "fn f(t: T2) -> Int { match t { T2(W(n), _) => g(3) + n } }\n f(T2(W(7), 0))", 307);
        check_int_p("ts_nested_tuple_elem", pre +
            "fn f(t: (W, Int)) -> Int { match t { (W(n), _) => g(3) + n } }\n f((W(7), 0))", 307);
        check_int_p("ts_nested_list_elem", pre +
            "fn f(xs: List[W]) -> Int { match xs { [W(n)] => g(3) + n, _ => -1 } }\n f([W(7)])", 307);
        check_int_p("ts_nested_map_value", pre +
            "fn f(m: Map[Int, W]) -> Int { match m { #{ 1 => W(n) } => g(3) + n, _ => -1 } }\n"
            " f(#{ 1 => W(7) })", 307);
        // The two shapes that were always right -- keep them beside the six that were not.
        check_int_p("ts_nested_toplevel_ok", pre +
            "fn f(w: W) -> Int { match w { W(n) => g(3) + n } }\n f(W(7))", 307);
        check_int_p("ts_nested_let_binder_ok", pre + "struct P { a: W, b: Int }\n"
            "fn f(p: P) -> Int { let P { a: W(n), b: _ } = p\n g(3) + n }\n f(P { a: W(7), b: 0 })", 307);
        // Differential: the oracle is blind to codegen, so it is what would have caught this class.
        check_same("ts_diff_nested_payload", pre +
            "fn f(o: Option[W]) -> Int { match o { Some(W(n)) => g(3) + n, None => -1 } }\n"
            " f(Some(W(7))) + f(None)", true);
    }

    // A transparent value as a Map key and as a Set element (exercises the synthesized Hashable key
    // -- covers both Map construction gating AND the `T: Hashable` bound on Set[UserId]).
    check_int_p ("ts_map_key",
        "transparent struct UserId(Int)\n let mut m: Map[UserId, Int] = #{}\n m[UserId(3)] = 42\n getOr(m, UserId(3), 0)", 42);
    check_int_p ("ts_set_element",
        "use std::set::*\n transparent struct UserId(Int)\n let s: Set[UserId] = Set::new()\n s.size()", 0);

    // A transparent value living in a normal (heap) struct slot, and round-tripped through a fn.
    check_int_p ("ts_field_in_struct",
        "transparent struct UserId(Int)\n struct Row { id: UserId, n: Int }\n"
        "let r = Row { id: UserId(5), n: 9 }\n r.id.0 + r.n", 14);
    check_int_p ("ts_fn_roundtrip",
        "transparent struct UserId(Int)\n fn bump(u: UserId) -> UserId { UserId(u.0 + 1) }\n bump(UserId(41)).0", 42);

    // ---- differential: VM (erased Int) must match the RefEval oracle (bare RtValue) ----
    check_same("ts_diff_match",
        "transparent struct UserId(Int)\n let a = UserId(40)\n match a { UserId(n) => n + 2 }", false);
    check_same("ts_diff_field",
        "transparent struct UserId(Int)\n struct Row { id: UserId, n: Int }\n"
        "let r = Row { id: UserId(5), n: 9 }\n r.id.0 + r.n", false);
    check_same("ts_diff_double",
        "transparent struct Meters(Double)\n let m = Meters(3)\n m.0 + 0.5", false);

    // ---- #1: name-preserving toString / print / interpolation (the top-level dump) ----
    check_true("ts_str_tostring", cg_run_out("transparent struct UserId(Int)\n print(toString(UserId(42)))") == "UserId(42)");
    check_true("ts_str_print",    cg_run_out("transparent struct UserId(Int)\n print(UserId(7))") == "UserId(7)");
    check_true("ts_str_interp",   cg_run_out("transparent struct UserId(Int)\n println(\"id=${UserId(5)}\")") == "id=UserId(5)\n");
    check_true("ts_str_double",   cg_run_out("transparent struct Meters(Double)\n print(Meters(3))") == "Meters(3.0)");
    check_true("ts_str_bool",     cg_run_out("transparent struct Flag(Bool)\n print(Flag(true))") == "Flag(true)");
    check_true("ts_str_multi",    cg_run_out("transparent struct UserId(Int)\n print(UserId(1), \"/\", UserId(2))") == "UserId(1)/UserId(2)");
    // NESTED in a container stays LOSSY (the permanent partial: the VM dump has no type info) -- and both
    // the VM and the oracle agree on the bare form, so it is differentially safe.
    check_true("ts_str_nested",   cg_run_out("transparent struct UserId(Int)\n println([UserId(1), UserId(2)])") == "[1, 2]\n");
    // Differential: the printed output is compared on both sides.
    check_same("ts_diff_print",   "transparent struct UserId(Int)\n print(UserId(9))\n 0", false);
    check_same("ts_diff_interp",  "transparent struct Meters(Double)\n print(\"m=${Meters(2)}\")\n 0", false);

    // ---- negatives: shape violations + forbidden impl + nominal distinctness (prelude-linked) ----
    check_true("ts_reject_nullary",   cg_check_fails_p("transparent struct Bad\n 0"));
    check_true("ts_reject_two_field", cg_check_fails_p("transparent struct Bad(Int, Int)\n 0"));
    check_true("ts_reject_brace",     cg_check_fails_p("transparent struct Bad { x: Int }\n 0"));
    check_true("ts_reject_generic",   cg_check_fails_p("transparent struct Id[T](T)\n 0"));
    check_true("ts_reject_string",    cg_check_fails_p("transparent struct Email(String)\n 0"));
    check_true("ts_reject_nested",    cg_check_fails_p("transparent struct A(Int)\n transparent struct B(A)\n 0"));
    check_true("ts_reject_impl",
        cg_check_fails_p("transparent struct UserId(Int)\n trait Sh { fn sh(self) -> Int }\n"
                         "impl Sh for UserId { fn sh(self) -> Int { self.0 } }\n 0"));
    check_true("ts_reject_cmp_int",   cg_check_fails_p("transparent struct UserId(Int)\n if UserId(1) == 5 { 1 } else { 0 }"));

    // A valid transparent struct still COMPILES (guards the negatives above are shape-specific).
    check_true("ts_valid_compiles",   !cg_check_fails_p("transparent struct UserId(Int)\n UserId(1).0"));
    // `transparent` before a non-struct is a PARSE error (a distinct exception type).
    auto rejects_any = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return false; } catch (...) { return true; }
    };
    check_true("ts_reject_on_fn",     rejects_any("transparent fn f() -> Int { 0 }\n 0"));
}

// Int-backed enums: `enum Color : Int { Red, Green, Blue }` is a DISTINCT checker type but IS the bare
// Int discriminant at runtime -- construction/match/equality lower to Int-immediate ops, no heap object.
// #2 of the representation-erasure lever (after transparent structs #0/#1). toString prints the NAME at
// the top level (discriminant->name switch); nested-in-a-container stays lossy (the bare Int).
void test_int_enums() {
    std::cout << "[test_int_enums: erased Int-backed enum #2]\n";

    // ---- construct + exhaustive match (no wildcard) ----
    check_int_p ("ie_match",       "enum Color : Int { Red, Green, Blue }\n match Green { Red => 0, Green => 1, Blue => 2 }", 1);
    check_int_p ("ie_match_first", "enum Color : Int { Red, Green, Blue }\n match Red { Red => 10, Green => 11, Blue => 12 }", 10);
    check_int_p ("ie_let_match",   "enum Color : Int { Red, Green, Blue }\n let c: Color = Blue\n match c { Red => 0, Green => 1, Blue => 2 }", 2);

    // ---- pinned + auto-increment discriminants ----
    check_int_p ("ie_pin_b",       "enum E : Int { A = 1, B, C = 10 }\n match B { A => 100, B => 200, C => 300 }", 200);
    check_int_p ("ie_pin_c",       "enum E : Int { A = 1, B, C = 10 }\n match C { A => 100, B => 200, C => 300 }", 300);
    check_int_p ("ie_pin_neg",     "enum E : Int { A = -5, B, C }\n match B { A => 1, B => 2, C => 3 }", 2);

    // ---- equality (bare-Int, distinct per variant) ----
    check_bool_p("ie_eq",          "enum Color : Int { Red, Green, Blue }\n Red == Red", true);
    check_bool_p("ie_neq",         "enum Color : Int { Red, Green, Blue }\n Red == Green", false);
    check_bool_p("ie_pin_neq",     "enum E : Int { A = 1, B, C = 10 }\n A == B", false);
    check_bool_p("ie_pin_neq2",    "enum E : Int { A = 1, B, C = 10 }\n B == C", false);

    // ---- map key + Set element (the erased Int is Hashable) ----
    check_int_p ("ie_map_key",     "enum Color : Int { Red, Green, Blue }\n let mut m: Map[Color, Int] = #{}\n m[Green] = 42\n getOr(m, Green, 0)", 42);
    check_int_p ("ie_map_miss",    "enum Color : Int { Red, Green, Blue }\n let mut m: Map[Color, Int] = #{}\n m[Green] = 42\n getOr(m, Red, -1)", -1);
    check_int_p ("ie_set",         "use std::set::*\n enum Color : Int { Red, Green, Blue }\n let s: Set[Color] = Set::new()\n s.size()", 0);

    // ---- name-preserving toString / print / interpolation (TOP-LEVEL) ----
    check_true("ie_str_tostring",  cg_run_out("enum Color : Int { Red, Green, Blue }\n print(toString(Green))") == "Green");
    check_true("ie_str_print",     cg_run_out("enum Color : Int { Red, Green, Blue }\n print(Red)") == "Red");
    check_true("ie_str_interp",    cg_run_out("enum Color : Int { Red, Green, Blue }\n println(\"c=${Blue}\")") == "c=Blue\n");
    check_true("ie_str_pin",       cg_run_out("enum E : Int { A = 1, B, C = 10 }\n print(C)") == "C");
    check_true("ie_str_match",     cg_run_out("enum Color : Int { Red, Green, Blue }\n print(toString(match Green { Red => Blue, Green => Red, Blue => Green }))") == "Red");
    // NESTED in a container stays LOSSY (the VM dumps the bare Int discriminant); both sides agree.
    check_true("ie_str_nested",    cg_run_out("enum Color : Int { Red, Green, Blue }\n println([Red, Green, Blue])") == "[0, 1, 2]\n");

    // ---- ordinal(e): the discriminant projection (the `.0` a nullary variant cannot have) ----
    check_int_p ("ie_ordinal_first",  "enum Color : Int { Red, Green, Blue }\n ordinal(Red)", 0);
    check_int_p ("ie_ordinal_auto",   "enum Color : Int { Red, Green, Blue }\n ordinal(Blue)", 2);
    // THE decision case: with a PINNED discriminant `ordinal` yields the discriminant (200), NOT the
    // declaration position (0). Java's ordinal() means the position -- Java just has no pinning.
    check_int_p ("ie_ordinal_pinned", "enum S : Int { Ok = 200, Created, NotFound = 404 }\n ordinal(Ok)", 200);
    // and the auto-counter RESUMES from a pin, so the position and the discriminant diverge here too.
    check_int_p ("ie_ordinal_resume", "enum S : Int { Ok = 200, Created, NotFound = 404 }\n ordinal(Created)", 201);
    check_int_p ("ie_ordinal_neg",    "enum E : Int { A = -5, B, C }\n ordinal(B)", -4);
    // reached through a binding, a qualified path, a pipe and a match -- not just a bare variant name.
    check_int_p ("ie_ordinal_binding","enum Color : Int { Red, Green, Blue }\n let c: Color = Blue\n ordinal(c)", 2);
    check_int_p ("ie_ordinal_qual",   "enum Color : Int { Red, Green, Blue }\n ordinal(Color::Green)", 1);
    check_int_p ("ie_ordinal_pipe",   "enum Color : Int { Red, Green, Blue }\n Green |> ordinal", 1);
    check_int_p ("ie_ordinal_match",  "enum Color : Int { Red, Green, Blue }\n ordinal(match Red { Red => Blue, _ => Red })", 2);
    // the result is an ORDINARY Int: arithmetic, and usable where an Int is demanded.
    check_int_p ("ie_ordinal_arith",  "enum Color : Int { Red, Green, Blue }\n ordinal(Green) * 10 + ordinal(Blue)", 12);
    check_int_p ("ie_ordinal_as_key", "enum Color : Int { Red, Green, Blue }\n let mut m: Map[Int, Int] = #{}\n m[ordinal(Blue)] = 7\n getOr(m, 2, 0)", 7);
    // and it prints as the NUMBER, where the enum itself prints as the NAME -- the whole point.
    check_true("ie_ordinal_prints_int",
        cg_run_out("enum Color : Int { Red, Green, Blue }\n print(toString(ordinal(Green)))") == "1");

    // ---- differentials: the VM's erased Int must match the oracle's bare value ----
    check_same("ie_diff_match",    "enum Color : Int { Red, Green, Blue }\n let c = Green\n match c { Red => 0, Green => 5, Blue => 9 }", false);
    check_same("ie_diff_str",      "enum Color : Int { Red, Green, Blue }\n print(Blue)\n 0", false);
    check_same("ie_diff_pin",      "enum E : Int { A = 1, B, C = 10 }\n print(\"x=${C}\")\n 0", false);
    check_same("ie_diff_nested",   "enum Color : Int { Red, Green, Blue }\n println([Green, Red])\n 0", false);
    // ordinal: identity on both sides, so what this really cross-checks is that the two independent
    // discriminant ASSIGNMENTS agree -- the auto-counter, a pin, and the resume after a pin.
    check_same("ie_diff_ordinal",  "enum Color : Int { Red, Green, Blue }\n ordinal(Green) * 10 + ordinal(Blue)", false);
    check_same("ie_diff_ordinal_pin",
        "enum S : Int { Ok = 200, Created, NotFound = 404 }\n ordinal(Ok) + ordinal(Created) + ordinal(NotFound)", false);
    check_same("ie_diff_ordinal_neg", "enum E : Int { A = -5, B, C }\n let e: E = B\n ordinal(e)", false);

    // ---- negatives ----
    check_true("ie_reject_tuple",     cg_check_fails_p("enum E : Int { A, B(Int) }\n 0"));
    check_true("ie_reject_record",    cg_check_fails_p("enum E : Int { A, B { x: Int } }\n 0"));
    check_true("ie_reject_dup_disc",  cg_check_fails_p("enum E : Int { A = 1, B = 1 }\n 0"));
    check_true("ie_reject_dup_auto",  cg_check_fails_p("enum E : Int { A, B = 0 }\n 0"));   // B pins 0, collides with A's auto 0
    check_true("ie_reject_generic",   cg_check_fails_p("enum E[T] : Int { A, B }\n 0"));
    check_true("ie_reject_impl",
        cg_check_fails_p("enum Color : Int { Red, Green }\n trait Sh { fn sh(self) -> Int }\n"
                         "impl Sh for Color { fn sh(self) -> Int { 0 } }\n 0"));
    check_true("ie_reject_disc_plain", cg_check_fails_p("enum E { A = 1, B }\n 0"));   // `= 1` on a non-int-backed enum
    check_true("ie_reject_cmp_int",    cg_check_fails_p("enum Color : Int { Red, Green }\n if Red == 5 { 1 } else { 0 }"));
    check_true("ie_reject_bad_repr",   cg_check_fails_p("enum E : Bool { A, B }\n 0"));

    // ordinal is for int-backed enums ONLY -- a PLAIN enum has no discriminant to project, and the
    // other erasure types already have `.0` (Char included, since Char IS a transparent newtype).
    check_true("ie_reject_ord_plain",  cg_check_fails_p("enum Color { Red, Green }\n ordinal(Red)"));
    check_true("ie_reject_ord_int",    cg_check_fails_p("ordinal(5)"));
    check_true("ie_reject_ord_char",   cg_check_fails_p("ordinal('a')"));
    check_true("ie_reject_ord_string", cg_check_fails_p("ordinal(\"x\")"));
    check_true("ie_reject_ord_struct", cg_check_fails_p("struct P { x: Int }\n ordinal(P { x: 1 })"));
    check_true("ie_reject_ord_newtype",cg_check_fails_p("transparent struct W(Int)\n ordinal(W(3))"));
    check_true("ie_reject_ord_arity0", cg_check_fails_p("enum Color : Int { Red, Green }\n ordinal()"));
    check_true("ie_reject_ord_arity2", cg_check_fails_p("enum Color : Int { Red, Green }\n ordinal(Red, Green)"));
    // ONE error, not a cascade: check_ordinal returns ty_int() even when it rejects, so the
    // surrounding arithmetic still type-checks instead of reporting a second, derived failure.
    check_true("ie_ord_no_cascade",    check_errc("ordinal(5) + 1 + 2") == 1);
    // an ambient builtin name is shadowable by a user fn where that fn is visible (the module-scoped
    // rule); adding `ordinal` to the table must not have made it un-shadowable.
    check_int_p ("ie_ordinal_shadowed", "fn ordinal(x: Int) -> Int { x + 100 }\n ordinal(7)", 107);

    // A valid int-backed enum still COMPILES (guards the negatives are shape-specific).
    check_true("ie_valid_compiles",   !cg_check_fails_p("enum Color : Int { Red, Green, Blue }\n match Red { Red => 1, _ => 0 }"));
}

void test_enum_qualified_path() {
    std::cout << "[test_enum_qualified_path: `Enum::Variant` canonical path (S1)]\n";

    // ---- nullary variant: value + pattern, qualified and mixed with bare ----
    check_int_p ("eq_val_match",   "enum Color { Red, Green, Blue }\n match Color::Green { Color::Red => 0, Color::Green => 1, Color::Blue => 2 }", 1);
    check_int_p ("eq_mixed",       "enum Color { Red, Green, Blue }\n match Green { Color::Red => 0, Color::Green => 5, Blue => 9 }", 5);
    check_true  ("eq_nullary_str", cg_run_out("enum Color { Red, Green, Blue }\n print(toString(Color::Blue))") == "Blue");

    // ---- payload variant: qualified call at a use site + in a fn body (return position) ----
    check_int_p ("eq_payload",     "enum Box { Wrap(Int), Empty }\n match Box::Wrap(7) { Box::Wrap(x) => x, Box::Empty => 0 }", 7);
    check_int_p ("eq_ctor_ret",    "enum Box { Wrap(Int), Empty }\n fn mk(n: Int) -> Box { Box::Wrap(n) }\n match mk(5) { Box::Wrap(x) => x, Box::Empty => 0 }", 5);

    // ---- record variant: qualified construction (bound first -- struct-lit is off in a match scrutinee) + pattern ----
    check_int_p ("eq_record",      "enum Shape { Dot, Rect { w: Int, h: Int } }\n let s = Shape::Rect { w: 3, h: 4 }\n match s { Shape::Rect { w, h } => w * h, Shape::Dot => 0 }", 12);

    // ---- ring enums via their qualified name (Option/Result) ----
    check_int_p ("eq_option",      "match Option::Some(41) { Option::Some(x) => x + 1, Option::None => 0 }", 42);
    check_int_p ("eq_result",      "let r: Result[Int, Int] = Result::Ok(9)\n match r { Result::Ok(v) => v, Result::Err(e) => e }", 9);

    // ---- `Trait::method` still resolves (the `::` case-split is intact) ----
    check_int_p ("eq_trait_method","trait Sh { fn sh(self) -> Int }\n struct P { v: Int }\n"
                                   " impl Sh for P { fn sh(self) -> Int { self.v } }\n Sh::sh(P { v: 7 })", 7);

    // ---- negatives ----
    check_true  ("eq_no_variant",  cg_check_fails_p("enum Color { Red, Green }\n let x = Color::Blue\n 0"));          // no such variant
    check_true  ("eq_not_enum",    cg_check_fails_p("struct S { x: Int }\n let x = S::Foo\n 0"));                     // head is not an enum
    check_true  ("eq_wrong_owner", cg_check_fails_p("enum A { X }\n enum B { Y }\n let x = A::Y\n 0"));               // Y belongs to B
    check_true  ("eq_pat_bad",     cg_check_fails_p("enum Color { Red, Green }\n match Red { Color::Blue => 0, _ => 1 }"));
    check_true  ("eq_call_bad",    cg_check_fails_p("enum Box { Wrap(Int) }\n let x = Box::Nope(1)\n 0"));

    // ---- differentials: qualified must lower identically to bare (no RefEval change in S1) ----
    check_same("eq_diff_match",    "enum Color { Red, Green, Blue }\n match Color::Green { Color::Red => 0, Color::Green => 5, Color::Blue => 9 }", false);
    check_same("eq_diff_payload",  "enum Box { Wrap(Int), Empty }\n match Box::Wrap(3) { Box::Wrap(x) => x + 1, Box::Empty => 0 }", false);
    check_same("eq_diff_option",   "match Option::Some(7) { Option::Some(x) => x, Option::None => 0 }", true);
    check_same("eq_diff_str",      "enum Color { Red, Green, Blue }\n print(Color::Green)\n 0", false);
}

void test_use_enum_import() {
    std::cout << "[test_use_enum_import: `use mod::Enum::*` / `::{V}` (S3)]\n";
    using M = std::unordered_map<std::string, std::string>;

    // `use mod::Enum::*` brings all variants of an imported enum in bare (WEAK glob).
    check_int_modules("s3_glob",
                      "use pal::Color::*\n match Green { Red => 0, Green => 42, Blue => 1 }",
                      M{{"pal", "pub enum Color { Red, Green, Blue }"}}, 42);
    // `use mod::Enum::{X, Y}` brings selected variants bare (STRONG).
    check_int_modules("s3_explicit",
                      "use pal::Color::{Red, Green, Blue}\n match Blue { Red => 0, Green => 1, Blue => 42 }",
                      M{{"pal", "pub enum Color { Red, Green, Blue }"}}, 42);
    // Payload variants come across too (construct + match).
    check_int_modules("s3_payload",
                      "use pal::Box::*\n match Wrap(42) { Wrap(n) => n, Empty => 0 }",
                      M{{"pal", "pub enum Box { Wrap(Int), Empty }"}}, 42);

    // --- negatives ---
    check_true("s3_private",   cg_modules_check_fails("use pal::Color::*\n 0",
                      M{{"pal", "enum Color { Red }"}}));                    // enum not pub
    check_true("s3_no_variant",cg_modules_check_fails("use pal::Color::{Nope}\n 0",
                      M{{"pal", "pub enum Color { Red }"}}));               // no such variant
    check_true("s3_not_enum",  cg_modules_check_fails("use pal::Point::*\n 0",
                      M{{"pal", "pub struct Point { x: Int }"}}));          // head is not an enum
    check_true("s3_conflict",  cg_modules_check_fails("use a::E::X\n use b::F::X\n 0",
                      M{{"a", "pub enum E { X }"}, {"b", "pub enum F { X }"}}));  // two strong imports of X
}

void test_method_call_syntax() {
    std::cout << "[test_method_call_syntax: `recv.method(args)` for trait methods (S1)]\n";

    // Trait method via `.` (returns Int).
    check_int_p ("mc_basic",
        "trait Area { fn area(self) -> Int }\n struct Rect { w: Int, h: Int }\n"
        " impl Area for Rect { fn area(self) -> Int { self.w * self.h } }\n"
        " let r = Rect { w: 3, h: 4 }\n r.area()", 12);
    // `.` and free-call are interchangeable (both resolve the same trait method).
    check_int_p ("mc_free_equiv",
        "trait Area { fn area(self) -> Int }\n struct Rect { w: Int, h: Int }\n"
        " impl Area for Rect { fn area(self) -> Int { self.w * self.h } }\n"
        " let r = Rect { w: 3, h: 4 }\n if r.area() == area(r) { 1 } else { 0 }", 1);
    // Method with an extra argument.
    check_int_p ("mc_arg",
        "trait Add2 { fn plus(self, k: Int) -> Int }\n struct N { v: Int }\n"
        " impl Add2 for N { fn plus(self, k: Int) -> Int { self.v + k } }\n"
        " let n = N { v: 40 }\n n.plus(2)", 42);
    // Chaining `a.f().g()` (left-to-right).
    check_int_p ("mc_chain",
        "struct Counter { n: Int }\n trait Inc { fn inc(self) -> Counter }\n"
        " trait Get { fn get(self) -> Int }\n"
        " impl Inc for Counter { fn inc(self) -> Counter { Counter { n: self.n + 1 } } }\n"
        " impl Get for Counter { fn get(self) -> Int { self.n } }\n"
        " let c = Counter { n: 5 }\n c.inc().inc().get()", 7);
    // `mut self` via `.` -> caller-visible mutation.
    check_int_p ("mc_mut_self",
        "struct Box { n: Int }\n trait Bump { fn bump(mut self) -> () }\n"
        " impl Bump for Box { fn bump(mut self) -> () { self.n = self.n + 10 } }\n"
        " let mut b = Box { n: 1 }\n b.bump()\n b.n", 11);
    // Dynamic dispatch through `.` on a `dyn` receiver.
    check_int_p ("mc_dyn",
        "trait Sh { fn sh(self) -> Int }\n struct P { v: Int }\n"
        " impl Sh for P { fn sh(self) -> Int { self.v } }\n"
        " let d: dyn Sh = P { v: 9 }\n d.sh()", 9);
    // Regression: a fn-valued FIELD is still callable via `.f()` (field-first).
    check_int_p ("mc_field_fn",
        "struct Holder { f: fn(Int) -> Int }\n"
        " let h = Holder { f: fn(x: Int) -> Int { x + 1 } }\n h.f(41)", 42);

    // Negatives.
    check_true("mc_no_method", cg_check_fails_p("struct Q { x: Int }\n let q = Q { x: 1 }\n q.nope()"));

    // Differentials.
    check_same("mc_diff_basic",
        "trait Area { fn area(self) -> Int }\n struct Rect { w: Int, h: Int }\n"
        " impl Area for Rect { fn area(self) -> Int { self.w * self.h } }\n"
        " Rect { w: 6, h: 7 }.area()", false);
    check_same("mc_diff_chain",
        "struct Counter { n: Int }\n trait Inc { fn inc(self) -> Counter }\n"
        " trait Get { fn get(self) -> Int }\n"
        " impl Inc for Counter { fn inc(self) -> Counter { Counter { n: self.n + 1 } } }\n"
        " impl Get for Counter { fn get(self) -> Int { self.n } }\n"
        " Counter { n: 5 }.inc().get()", false);
}

void test_inherent_impl() {
    std::cout << "[test_inherent_impl: traitless `impl Type { fn m(self) }` (S2)]\n";

    check_int_p ("ii_basic",
        "struct Point { x: Int, y: Int }\n"
        " impl Point { fn sum(self) -> Int { self.x + self.y } }\n"
        " let p = Point { x: 3, y: 4 }\n p.sum()", 7);
    check_int_p ("ii_arg",
        "struct Point { x: Int, y: Int }\n"
        " impl Point { fn shift(self, d: Int) -> Int { self.x + d } }\n"
        " Point { x: 10, y: 0 }.shift(5)", 15);
    // Generic inherent impl: `impl[T] Box[T]`, return type `T` solved from the receiver.
    check_int_p ("ii_generic",
        "struct Box[T] { v: T }\n impl[T] Box[T] { fn get(self) -> T { self.v } }\n"
        " let b = Box { v: 42 }\n b.get()", 42);
    // `mut self` -> caller-visible mutation.
    check_int_p ("ii_mut_self",
        "struct Ctr { n: Int }\n impl Ctr { fn bump(mut self) -> () { self.n = self.n + 5 } }\n"
        " let mut c = Ctr { n: 1 }\n c.bump()\n c.bump()\n c.n", 11);
    // Chaining inherent methods.
    check_int_p ("ii_chain",
        "struct Ctr { n: Int }\n"
        " impl Ctr { fn inc(self) -> Ctr { Ctr { n: self.n + 1 } }  fn get(self) -> Int { self.n } }\n"
        " Ctr { n: 5 }.inc().inc().get()", 7);
    // An inherent method shadows a same-named TRAIT method for `.` (Rust rule).
    check_int_p ("ii_shadow",
        "struct S { v: Int }\n trait Tr { fn f(self) -> Int }\n"
        " impl Tr for S { fn f(self) -> Int { 0 } }\n"
        " impl S { fn f(self) -> Int { self.v } }\n S { v: 9 }.f()", 9);

    // Negatives.
    check_true("ii_orphan",   cg_check_fails_p("impl Vec[Int] { fn foo(self) -> Int { 0 } }"));            // foreign type
    // A member without `self` is no longer an error -- it is an ASSOCIATED FUNCTION, reachable only
    // as `P::ok()`. What must still hold is that it is NOT reachable through `.` (test_associated_fn
    // covers both halves); here we only pin that the declaration itself is accepted.
    check_int_p("ii_no_self_is_assoc",
        "struct P { x: Int }\n impl P { fn ok() -> Int { 3 } }\n P::ok()", 3);
    check_true("ii_dup",      cg_check_fails_p("struct P { x: Int }\n"
        " impl P { fn m(self) -> Int { 1 }  fn m(self) -> Int { 2 } }"));
    check_true("ii_dup_block",cg_check_fails_p("struct P { x: Int }\n"
        " impl P { fn a(self) -> Int { 1 } }\n impl P { fn b(self) -> Int { 2 } }"));

    // Differentials.
    check_same("ii_diff_basic",
        "struct Point { x: Int, y: Int }\n impl Point { fn sum(self) -> Int { self.x + self.y } }\n"
        " Point { x: 6, y: 7 }.sum()", false);
    check_same("ii_diff_generic",
        "struct Box[T] { v: T }\n impl[T] Box[T] { fn get(self) -> T { self.v } }\n Box { v: 5 }.get()", false);
}

void test_qualified_inherent_call() {
    std::cout << "[test_qualified_inherent_call: `Type::method(x)`, the twin of `Trait::method(x)`]\n";

    const char* PT = "struct Point { x: Int, y: Int }\n"
                     " impl Point { fn sum(self) -> Int { self.x + self.y }"
                     "              fn shift(self, d: Int) -> Int { self.x + d } }\n"
                     " let p = Point { x: 3, y: 4 }\n";

    check_int_p ("qi_call",    std::string(PT) + "Point::sum(p)", 7);
    check_int_p ("qi_args",    std::string(PT) + "Point::shift(p, 5)", 8);
    check_int_p ("qi_pipe",    std::string(PT) + "p |> Point::sum", 7);
    check_int_p ("qi_pipe_arg",std::string(PT) + "p |> Point::shift(5)", 8);
    // Same fn, both spellings -- they must lower to the same call.
    check_int_p ("qi_same_as_dot", std::string(PT) + "Point::sum(p) - p.sum()", 0);

    // THE motivating case: a FIELD with the method's name makes `.` unreachable (field-first), so
    // the qualified form is the only way to call the method at all.
    check_int_p ("qi_field_shadow",
        "struct C { n: Int, value: Int }\n impl C { fn value(self) -> Int { self.n * 2 } }\n"
        " let c = C { n: 5, value: 1 }\n C::value(c) + c.value", 11);

    // Generic head: the impl generics are solved from the receiver, exactly as for `.`.
    check_int_p ("qi_generic",
        "struct Box[T] { v: T }\n impl[T] Box[T] { fn get(self) -> T { self.v } }\n"
        " Box::get(Box { v: 42 })", 42);
    // `mut self` mutates the caller's binding through the qualified form too.
    check_int_p ("qi_mut_self",
        "struct Ctr { n: Int }\n impl Ctr { fn bump(mut self) -> () { self.n = self.n + 5 } }\n"
        " let mut c = Ctr { n: 1 }\n Ctr::bump(c)\n Ctr::bump(c)\n c.n", 11);
    // ... and the `mut`-root rule still applies to the receiver (an immutable binding is rejected).
    check_true  ("qi_mut_needs_mut", cg_check_fails_p(
        "struct Ctr { n: Int }\n impl Ctr { fn bump(mut self) -> () { self.n = self.n + 1 } }\n"
        " let c = Ctr { n: 1 }\n Ctr::bump(c)\n 0"));

    // Tail position: a direct TCO_CALL, so self-recursion through the qualified form is O(1) stack.
    check_int_p ("qi_tco",
        "struct C { n: Int }\n"
        " impl C { fn step(self, k: Int) -> Int {"
        "     if k <= 0 { self.n } else { C::step(C { n: self.n + 1 }, k - 1) } } }\n"
        " C::step(C { n: 0 }, 200000)", 200000);

    // A trait method is unaffected -- `Trait::method` still resolves to the trait even when a type
    // of the SAME name exists (backward compatible), as long as the type has no such method.
    check_int_p ("qi_trait_unchanged",
        "trait Foo { fn bar(self) -> Int }\n struct Foo { n: Int }\n"
        " impl Foo for Foo { fn bar(self) -> Int { 7 } }\n Foo::bar(Foo { n: 3 })", 7);

    // Negatives, each with its own wording.
    check_true("qi_ambiguous", check_has(
        "trait Foo { fn bar(self) -> Int }\n struct Foo { n: Int }\n"
        " impl Foo { fn bar(self) -> Int { 1 } }\n Foo::bar(Foo { n: 3 })", "is ambiguous"));
    check_true("qi_no_method", check_has(
        "struct P { n: Int }\n impl P { fn go(self) -> Int { 1 } }\n P::nope(P { n: 1 })",
        "type 'P' has no method 'nope'"));
    check_true("qi_no_impl_block", check_has(
        "struct P { n: Int }\n P::go(P { n: 1 })", "has no inherent `impl` block"));
    check_true("qi_unknown_head", check_has("Nope::go(1)", "unknown trait or type 'Nope'"));
    check_true("qi_wrong_recv", check_has(
        "struct P { n: Int }\n impl P { fn go(self) -> Int { self.n } }\n P::go(42)",
        "expects a P receiver, found Int"));
    check_true("qi_no_recv", check_has(
        "struct P { n: Int }\n impl P { fn go(self) -> Int { self.n } }\n P::go()",
        "needs a receiver argument"));
    // Still call-only, symmetric with `Trait::method` (see DEFERRED: a first-class value form).
    check_true("qi_not_a_value", check_has(
        "struct P { n: Int }\n impl P { fn go(self) -> Int { self.n } }\n let f = P::go\n 0",
        "can only be used as a call target"));

    // Differentials: only these exercise the RefEval oracle's own inherent-qualified route, so a
    // divergence between the bytecode and the oracle shows up here rather than nowhere.
    check_same("qi_diff_call",
        "struct Point { x: Int, y: Int }\n impl Point { fn sum(self) -> Int { self.x + self.y } }\n"
        " Point::sum(Point { x: 6, y: 7 })", false);
    check_same("qi_diff_pipe",
        "struct Point { x: Int, y: Int }\n impl Point { fn sum(self) -> Int { self.x + self.y } }\n"
        " Point { x: 2, y: 3 } |> Point::sum", false);
    check_same("qi_diff_generic",
        "struct Box[T] { v: T }\n impl[T] Box[T] { fn get(self) -> T { self.v } }\n"
        " Box::get(Box { v: 5 })", false);
    check_same("qi_diff_args",
        "struct P { n: Int }\n impl P { fn add(self, d: Int) -> Int { self.n + d } }\n"
        " P::add(P { n: 4 }, 6)", false);
}

// A method-syntax receiver must be inferred ONCE. Inferring `Type::fn(..)` writes the resolved head back
// (`Json` -> `std::json::Json`), and that writeback is not idempotent: a second inference read the
// lowercase mangled head as a MODULE path and reported "module 'std::json::Json' is not imported".
// The frame measure must be linear in nesting depth. It re-walked each call argument for `.local_peak`,
// again in need_call for `.temp`, and once more at an inline site, and every re-walk recursed through the
// whole subtree: n nested calls cost 2^n measure steps, 3^n with the inliner (sixteen nested `inc(..)` =
// 387 million; a 24-link method chain did not compile within five minutes). Each shape below reaches the
// measure's Call/Pipe cases by a different route; before the memo every one of them effectively hangs.
// Depth 50 stays inside the 63-register frame for all four (the measure's temp bound grows with depth).
void test_measure_linear() {
    std::cout << "[test_measure_linear: deeply nested calls compile in linear measure time]\n";
    const int depth = 50;
    std::string open, close;
    for (int i = 0; i < depth; ++i) { open += "inc("; close += ")"; }
    check_int("measure_linear_direct",
        "fn inc(x: Int) -> Int { x + 1 }\n " + open + "0" + close, depth);

    std::string chain = "trait Inc { fn inc(self) -> Int }\n impl Inc for Int { fn inc(self) -> Int { self + 1 } }\n 0";
    for (int i = 0; i < depth; ++i) chain += ".inc()";
    check_int("measure_linear_method_chain", chain, depth);

    std::string pipe = "fn inc(x: Int) -> Int { x + 1 }\n 0";
    for (int i = 0; i < depth; ++i) pipe += " |> inc";
    check_int("measure_linear_pipe", pipe, depth);

    std::string ind_open;
    for (int i = 0; i < depth; ++i) ind_open += "f(";
    check_int("measure_linear_indirect",
        "let f = fn(x: Int) -> Int { x + 1 }\n " + ind_open + "0" + close, depth);
}

void test_receiver_inferred_once() {
    std::cout << "[test_receiver_inferred_once: `Type::fn(..).method()` with a module-mangled head]\n";
    using M = std::unordered_map<std::string, std::string>;

    // A std type: trait method, then inherent, then trait again.
    check_int_p("rio_std_json",
        "use std::json::*\n Json::parse(\"7\").unwrap().asInt().unwrapOr(0)", 7);

    const char* MOD =
        "pub struct P { x: Int }\n"
        "impl P { fn new(x: Int) -> P { P { x: x } } }\n"
        "pub trait T { fn get(self) -> Int }\n"
        "impl T for P { fn get(self) -> Int { self.x } }\n"
        "pub fn mk(x: Int) -> P { P { x: x } }\n"
        "pub struct F { f: fn(Int) -> Int }\n"
        "impl F { fn new(k: Int) -> F { F { f: fn(n: Int) -> Int { n + k } } } }\n";
    // Kept apart from MOD: a failure inside the module would mask the entry-side cases above.
    const std::string MOD_RUN = std::string(MOD) + "pub fn run() -> Int { P::new(6).get() }\n";
    // An imported module's type, reached by `use`.
    check_int_modules("rio_module_type", "import m\n use m::*\n P::new(5).get()", M{{"m", MOD}}, 5);
    // A module-qualified fn call as the receiver.
    check_int_modules("rio_module_fn", "import m\n use m::T\n m::mk(5).get()", M{{"m", MOD}}, 5);
    // The field-first path: a fn-valued FIELD called through `.` on an associated-fn result.
    check_int_modules("rio_field_first", "import m\n use m::*\n F::new(3).f(2)", M{{"m", MOD}}, 5);
    // Inside the module that declares the type (its own head mangles to `m::P` too).
    check_int_modules("rio_own_module", "use m::run\n run()", M{{"m", MOD_RUN}}, 6);
    // Re-inferring each receiver doubled the checker's work per link: a 24-link chain took 10.5 s to
    // type-check. Inferred once, 40 links are instant; a regression hangs here instead of
    // passing. Checker only (check_errc) -- codegen of a deep NESTED call has its own, separate cost.
    {
        std::string chain = "trait Inc { fn inc(self) -> Int }\n impl Inc for Int { fn inc(self) -> Int { self + 1 } }\n 0";
        for (int i = 0; i < 40; ++i) chain += ".inc()";
        check_true("rio_chain_linear", check_errc(chain) == 0);
    }
    // Control: an entry-program type mangles to `$entry::P`, and a `$`-led head is never read as a module
    // path, so this never hit the bug.
    check_int_p("rio_entry_type",
        "struct P { x: Int }\n impl P { fn new(x: Int) -> P { P { x: x } } }\n"
        " trait T { fn get(self) -> Int }\n impl T for P { fn get(self) -> Int { self.x } }\n"
        " P::new(5).get()", 5);
}

void test_associated_fn() {
    std::cout << "[test_associated_fn: `Point::new(1, 2)` -- an inherent fn WITHOUT `self`]\n";

    const char* PT = "struct Point { x: Int, y: Int }\n"
                     " impl Point { fn new(x: Int, y: Int) -> Point { Point { x: x, y: y } }"
                     "              fn origin() -> Self { Point::new(0, 0) }"
                     "              fn norm(self) -> Int { self.x * self.x + self.y * self.y } }\n";

    check_int_p("af_ctor",     std::string(PT) + "Point::new(3, 4).norm()", 25);
    check_int_p("af_nullary",  std::string(PT) + "Point::origin().norm()", 0);
    // An associated fn calling another one on the same head (and `-> Self` in the signature).
    check_int_p("af_chained",  std::string(PT) + "Point::new(1, 2).x + Point::origin().y", 1);
    // `Self` in a METHOD signature too -- the same one-line env fix (it was rejected before).
    check_int_p("af_self_ty_method",
        "struct P { n: Int }\n impl P { fn dup(self) -> Self { self }  fn n2(self) -> Int { self.n } }\n"
        " P { n: 6 }.dup().n2()", 6);

    // An ENUM head works exactly like a struct head (`inherent_` is keyed by head, either kind).
    check_int_p("af_enum_head",
        "enum Col { Red, Green }\n"
        " impl Col { fn first() -> Col { Col::Red }"
        "            fn code(self) -> Int { match self { Col::Red => 1, Col::Green => 2 } } }\n"
        " Col::first().code()", 1);

    // A generic head has no receiver to solve it, so each pinning route needs its own case:
    // from an argument, from an ANNOTATION, from the enclosing fn's return type, and from a
    // rigid type param inside a generic body.
    const char* HB = "struct H[T] { v: T }\n"
                     " impl[T] H[T] { fn mk(a: T) -> H[T] { H { v: a } }"
                     "                fn get(self) -> T { self.v } }\n";
    check_int_p("af_generic_from_arg", std::string(HB) + "H::mk(7).get()", 7);
    check_int_p("af_generic_from_annot",
        std::string(HB) + "let h: H[Int] = H::mk(8)\n h.get()", 8);
    check_int_p("af_generic_from_ret",
        std::string(HB) + "fn make() -> H[Int] { H::mk(9) }\n make().get()", 9);
    check_int_p("af_generic_rigid",
        std::string(HB) + "fn wrap[U](x: U) -> H[U] { H::mk(x) }\n wrap(11).get()", 11);

    // A `mut` parameter still mutates the caller's binding through an associated fn.
    check_int_p("af_mut_param",
        "struct C { n: Int }\n impl C { fn fill(mut v: Vec[Int], k: Int) -> Int { push(v, k)  len(v) } }\n"
        " let mut v = vec()\n C::fill(v, 3)\n C::fill(v, 4)\n len(v)", 2);
    // ... and the `mut`-root rule still applies to the argument.
    check_true("af_mut_needs_mut", cg_check_fails_p(
        "struct C { n: Int }\n impl C { fn fill(mut v: Vec[Int], k: Int) -> Int { push(v, k)  len(v) } }\n"
        " let v = vec()\n C::fill(v, 3)\n 0"));

    // Tail position -> a direct TCO_CALL, so an associated fn recurses in O(1) native stack.
    check_int_p("af_tco",
        "struct C { n: Int }\n"
        " impl C { fn count(k: Int, acc: Int) -> Int {"
        "     if k <= 0 { acc } else { C::count(k - 1, acc + 1) } } }\n"
        " C::count(200000, 0)", 200000);

    // Negatives.
    // The type argument is fixed by NOTHING (T is absent from the args and from the return type),
    // so the call is rejected rather than silently leaking an unsolved var -- see
    // reject_unsolved_generics: a BOUNDED param would otherwise satisfy its own bound vacuously.
    check_true("af_cannot_infer_ret", check_has(
        "struct H[T] { v: T }\n impl[T] H[T] { fn one() -> H[Int] { H { v: 1 } } }\n H::one()\n 0",
        "cannot infer the type argument 'T' of 'H::one'"));
    // The `Set::empty()` shape, and the reason the rejection is a SOUNDNESS matter rather than
    // tidiness: with a BOUNDED head param, an unsolved var discharges against its own bound
    // vacuously (satisfies_bound), so the real type argument would never be checked.
    const char* SB = "struct S[T: Hashable] { m: Map[T, Bool] }\n"
                     " impl[T: Hashable] S[T] { fn empty() -> S[T] { S { m: #{} } }"
                     "                          fn size(self) -> Int { len(self.m) } }\n";
    check_true("af_bounded_needs_annot", check_has(
        std::string(SB) + "let s = S::empty()\n 0", "cannot infer the type argument 'T' of 'S::empty'"));
    check_int_p("af_bounded_annotated",
        std::string(SB) + "let s: S[Int] = S::empty()\n s.size()", 0);
    // The std::set migration shape: an associated fn AND a method, both inside a BOUNDED generic
    // impl, calling another associated fn on their OWN head against the impl's rigid `T`.
    // `af_generic_rigid` above covers a rigid var from an enclosing FN; this is the impl-generic
    // variant, which is what `Set::fromVec` / `union` / `intersect` / `difference` do.
    const char* BB = "struct Bag[T: Hashable] { m: Map[T, Bool] }\n"
                     " impl[T: Hashable] Bag[T] {\n"
                     "   fn empty() -> Bag[T] { Bag { m: #{} } }\n"
                     "   fn fromVec(v: Vec[T]) -> Bag[T] {"
                     "       let mut b: Bag[T] = Bag::empty()  for x in v { b.m[x] = true }  b }\n"
                     "   fn dup(self) -> Bag[T] {"
                     "       let mut r: Bag[T] = Bag::empty()  for (k, _) in self.m { r.m[k] = true }  r }\n"
                     "   fn size(self) -> Int { len(self.m) } }\n";
    check_int_p("af_impl_rigid_assoc",
        std::string(BB) + "let v = toVec([1, 2, 2, 3])\n Bag::fromVec(v).size()", 3);
    check_int_p("af_impl_rigid_method",
        std::string(BB) + "let v = toVec([4, 5])\n Bag::fromVec(v).dup().size()", 2);
    // The `.` form does not reach an associated fn -- and says so, instead of the generic
    // "has no field or method", which would claim it does not exist.
    check_true("af_not_via_dot", check_has(
        "struct P { n: Int }\n impl P { fn make(k: Int) -> P { P { n: k } } }\n"
        " let p = P { n: 1 }\n p.make(2).n",
        "is an associated function of 'P' (it takes no `self`)"));
    check_true("af_wrong_arity", check_has(
        "struct P { n: Int }\n impl P { fn make(k: Int) -> P { P { n: k } } }\n P::make(1, 2).n",
        "'P::make' expects 1 argument(s), got 2"));
    // A trait and a type sharing the name AND the member: unresolvable, because the associated fn
    // has no `.` spelling to escape to -- the message must say rename, not "use `x.bar(...)`".
    check_true("af_ambiguous", check_has(
        "trait Foo { fn bar(self) -> Int }\n struct Foo { n: Int }\n"
        " impl Foo { fn bar() -> Int { 1 } }\n Foo::bar(Foo { n: 3 })",
        "rename one of the two"));
    // Still call-only, symmetric with `Type::method` / `Trait::method`.
    check_true("af_not_a_value", check_has(
        "struct P { n: Int }\n impl P { fn make() -> P { P { n: 1 } } }\n let f = P::make\n 0",
        "can only be used as a call target"));

    // THE Slice-4 lock: a type with an associated `make()` AND a trait method `make(self)`. `.`
    // must reach the TRAIT method. Nothing else in the suite would notice if codegen's dot branch
    // picked the associated body instead -- it would just call a different function.
    check_int_p("af_no_dot_shadow",
        "trait Mk { fn make(self) -> Int }\n struct P { n: Int }\n"
        " impl Mk for P { fn make(self) -> Int { 7 } }\n"
        " impl P { fn make() -> Int { 99 } }\n P { n: 1 }.make()", 7);

    // Differentials: the generator emits no `impl` items at all, so these are the ONLY cases that
    // exercise the oracle's route for an associated fn.
    check_same("af_diff_ctor",
        "struct Point { x: Int, y: Int }\n"
        " impl Point { fn new(x: Int, y: Int) -> Point { Point { x: x, y: y } }"
        "              fn norm(self) -> Int { self.x + self.y } }\n"
        " Point::new(6, 7).norm()", false);
    check_same("af_diff_nullary",
        "struct P { n: Int }\n impl P { fn zero() -> P { P { n: 0 } }  fn n2(self) -> Int { self.n } }\n"
        " P::zero().n2()", false);
    check_same("af_diff_generic",
        "struct H[T] { v: T }\n impl[T] H[T] { fn mk(a: T) -> H[T] { H { v: a } }"
        "                                     fn get(self) -> T { self.v } }\n"
        " H::mk(5).get()", false);
    check_same("af_diff_no_dot_shadow",
        "trait Mk { fn make(self) -> Int }\n struct P { n: Int }\n"
        " impl Mk for P { fn make(self) -> Int { 7 } }\n"
        " impl P { fn make() -> Int { 99 } }\n P { n: 1 }.make()", false);
}

void test_enum_variant_coexist() {
    std::cout << "[test_enum_variant_coexist: same-module same-name variants (S2)]\n";

    // Two enums in one module SHARE a variant short name (`X`) -- previously a hard "duplicate
    // constructor" error; now they coexist and are disambiguated by `Enum::X`.
    const char* AB = "enum A { X, Y }\n enum B { X, Z }\n";
    check_int_p ("co_match_a",   std::string(AB) + "match A::X { A::X => 1, A::Y => 2 }", 1);
    check_int_p ("co_match_b",   std::string(AB) + "match B::X { B::X => 7, B::Z => 8 }", 7);

    // A NON-colliding short name in the same module still resolves bare (`Y`/`Z`); mixed with qualified.
    check_int_p ("co_bare_ok",   std::string(AB) + "match Y { Y => 5, A::X => 0 }", 5);

    // Colliding payload variants (distinct field types) coexist.
    check_int_p ("co_payload",   "enum A { Wrap(Int), N }\n enum B { Wrap(Bool), M }\n"
                                 " match A::Wrap(7) { A::Wrap(x) => x, A::N => 0 }", 7);

    // Int-backed enums may also share a variant short name (each erases to its own discriminant).
    check_int_p ("co_int",       "enum A : Int { X, Y }\n enum B : Int { X = 5, Z }\n"
                                 " match B::X { B::X => 5, B::Z => 6 }", 5);

    // toString/print renders the SHORT variant name even for a disambiguated key.
    check_true  ("co_tostring",  cg_run_out(std::string(AB) + "print(A::X)") == "X");
    check_true  ("co_int_str",   cg_run_out("enum A : Int { X }\n enum B : Int { X = 9 }\n print(B::X)") == "X");

    // A BARE reference to a colliding short is ambiguous -> a use-site error (must qualify).
    check_true  ("co_bare_ambig_val", cg_check_fails_p(std::string(AB) + "let v = X\n 0"));
    check_true  ("co_bare_ambig_pat", cg_check_fails_p(std::string(AB) + "match A::X { X => 0, _ => 1 }"));

    // A wrong-owner qualified reference is still rejected.
    check_true  ("co_wrong_owner",    cg_check_fails_p(std::string(AB) + "let v = A::Z\n 0"));

    // Differentials: the disambiguated key must lower + render identically in VM and oracle.
    check_same("co_diff_match", std::string(AB) + "match B::X { B::X => 1, B::Z => 2 }", false);
    check_same("co_diff_str",   std::string(AB) + "print(A::X)\n 0", false);
    check_same("co_diff_int",   "enum A : Int { X, Y }\n enum B : Int { X = 5, Z }\n print(B::X)\n 0", false);
}

void test_char_utf8() {
    std::cout << "[test_char_utf8: zero-cost Char + UTF-8 code points #3]\n";

    // ---- construct ('A':Char coercion / Char(cp)) + project (c.0 / codePoint) ----
    check_int_p ("cu_lit_let",     "let c: Char = 'A'\n codePoint(c)", 65);
    check_int_p ("cu_ctor_proj",   "let c = Char(0x20AC)\n c.0", 0x20AC);
    check_int_p ("cu_fn_lit_arg",  "fn f(c: Char) -> Int { codePoint(c) }\n f('Z')", 90);          // 'Z' coerces
    check_int_p ("cu_fn_roundtrip","fn f(c: Char) -> Char { c }\n codePoint(f(Char(0x20AC)))", 0x20AC);
    check_int_p ("cu_struct_field","struct Box { c: Char }\n let b = Box { c: Char(0x20AC) }\n codePoint(b.c)", 0x20AC);

    // ---- equality (Char==Char, and a byte literal coerces opposite a Char) ----
    check_bool_p("cu_eq",          "let c: Char = 'A'\n c == 'A'", true);
    check_bool_p("cu_neq",         "let c: Char = 'A'\n c == 'B'", false);
    check_bool_p("cu_eq_ctor",     "Char('x') == Char('x')", true);

    // ---- ordering (two Chars; a byte literal coerces) ----
    check_bool_p("cu_lt",          "Char('x') < Char('y')", true);
    check_bool_p("cu_range",       "let c: Char = 'm'\n 'a' <= c && c <= 'z'", true);
    check_bool_p("cu_range_no",    "let c: Char = '5'\n 'a' <= c && c <= 'z'", false);

    // ---- glyph toString / print / interpolation (TOP-LEVEL) ----
    check_true("cu_print_ascii",   cg_run_out("print(codePointToStr(65))") == "A");
    check_true("cu_print_char",    cg_run_out("let c: Char = 'A'\n print(c)") == "A");
    check_true("cu_print_2byte",   cg_run_out("print(Char(0xE9))") == "\xc3\xa9");            // e-acute
    check_true("cu_print_3byte",   cg_run_out("print(Char(0x20AC))") == "\xe2\x82\xac");      // euro
    check_true("cu_print_4byte",   cg_run_out("print(Char(0x1F600))") == "\xf0\x9f\x98\x80"); // emoji
    check_true("cu_tostring",      cg_run_out("print(toString(Char(0x20AC)))") == "\xe2\x82\xac");
    check_true("cu_interp",        cg_run_out("let c: Char = 'A'\n println(\"[${c}]\")") == "[A]\n");
    check_true("cu_bare_lit_int",  cg_run_out("print('A')") == "65");                          // byte literal stays Int
    // NESTED in a container stays LOSSY (the VM dumps the bare code-point Int); both sides agree.
    check_true("cu_nested_lossy",  cg_run_out("let a: Array[Char] = array(2, Char(65))\n println(a)") == "[65, 65]\n");

    // ---- map key + Set element (the erased code-point Int is Hashable) ----
    check_int_p ("cu_map_key",     "let mut m: Map[Char, Int] = #{}\n m[Char('A')] = 7\n getOr(m, Char('A'), 0)", 7);
    check_int_p ("cu_map_litkey",  "let mut m: Map[Char, Int] = #{}\n m['A'] = 9\n getOr(m, 'A', 0)", 9);   // literal key coerces
    check_int_p ("cu_map_miss",    "let mut m: Map[Char, Int] = #{}\n m[Char('A')] = 7\n getOr(m, Char('B'), -1)", -1);
    check_int_p ("cu_set",         "use std::set::*\n let s: Set[Char] = Set::new()\n s.size()", 0);

    // ---- pattern match on a Char (byte literal patterns; needs a wildcard -- literals never complete) ----
    check_int_p ("cu_match",       "let c: Char = 'A'\n match c { 'A' => 1, 'B' => 2, _ => 0 }", 1);
    check_int_p ("cu_match_miss",  "let c: Char = 'Z'\n match c { 'A' => 1, 'B' => 2, _ => 0 }", 0);
    check_int_p ("cu_match_some",  "match nthChar(\"A\", 0) { Some('A') => 1, Some(_) => 2, None => 3 }", 1);

    // ---- codec: codePointToStr byte widths + U+FFFD; charCount / chars / nthChar / fromChars / append ----
    check_int_p ("cu_enc_1",       "len(codePointToStr(65))", 1);
    check_int_p ("cu_enc_2",       "len(codePointToStr(0xE9))", 2);
    check_int_p ("cu_enc_3",       "len(codePointToStr(0x20AC))", 3);
    check_int_p ("cu_enc_4",       "len(codePointToStr(0x1F600))", 4);
    check_int_p ("cu_enc_neg",     "len(codePointToStr(0 - 1))", 3);        // invalid -> U+FFFD (3 bytes)
    check_int_p ("cu_enc_surro",   "len(codePointToStr(0xD800))", 3);       // surrogate -> U+FFFD
    check_int_p ("cu_charcount",   "charCount(codePointToStr(0x20AC) + \"ab\")", 3);
    check_int_p ("cu_charcount1",  "charCount(codePointToStr(0x1F600) + codePointToStr(0xE9) + \"x\")", 3);
    check_int_p ("cu_nth_cp",      "let s = codePointToStr(0x20AC) + \"Z\"\n match nthChar(s, 0) { Some(c) => codePoint(c), None => -1 }", 0x20AC);
    check_int_p ("cu_nth_ascii",   "let s = codePointToStr(0x20AC) + \"Z\"\n match nthChar(s, 1) { Some(c) => codePoint(c), None => -1 }", 90);
    check_int_p ("cu_nth_oob",     "match nthChar(\"ab\", 5) { Some(c) => codePoint(c), None => -1 }", -1);
    check_bool_p("cu_roundtrip",   "let s = codePointToStr(0x1F600) + \"x\" + codePointToStr(0xE9)\n fromChars(chars(s)) == s", true);
    check_int_p ("cu_append",      "let mut sb = stringBuilder()\n sb.appendCodePoint(0x20AC)\n sb.appendCodePoint(65)\n len(sb.build())", 4);

    // ---- differentials: VM (codePointToStr glyph / erased Int) must match the oracle (utf8_encode) ----
    check_same("cu_diff_glyph",    "print(codePointToStr(0x20AC))\n 0", true);
    check_same("cu_diff_char",     "let c = Char(0x1F600)\n print(c)\n 0", true);
    check_same("cu_diff_interp",   "let c: Char = 'Q'\n print(\"${c}!\")\n 0", true);
    check_same("cu_diff_invalid",  "print(codePointToStr(0 - 1))\n 0", true);
    check_same("cu_diff_nested",   "let a: Array[Char] = array(2, Char(65))\n println(a)\n 0", true);
    check_same("cu_diff_count",    "println(toString(charCount(codePointToStr(0x1F600) + \"abc\")))\n 0", true);
    check_same("cu_diff_round",    "let s = codePointToStr(0x20AC) + \"x\"\n println(toString(fromChars(chars(s)) == s))\n 0", true);

    // ---- negatives ----
    check_true("cu_reject_intvar",   cg_check_fails_p("let x = 5\n let c: Char = x\n 0"));         // non-literal Int -> Char
    check_true("cu_reject_over255",  cg_check_fails_p("let c: Char = 300\n 0"));                   // char literals are single-byte; use Char(300)
    check_true("cu_reject_ret_int",  cg_check_fails_p("fn f(x: Int) -> Char { x }\n 0"));          // Int expr as Char
    check_true("cu_reject_eq_int",   cg_check_fails_p("let c: Char = 'A'\n if c == 300 { 1 } else { 0 }"));  // Char == Int(>255)
    check_true("cu_reject_lt_int",   cg_check_fails_p("let c: Char = 'A'\n if c < 300 { 1 } else { 0 }"));   // Char < Int
    check_true("cu_reject_impl",     cg_check_fails_p("trait Sh { fn sh(self) -> Int }\n impl Sh for Char { fn sh(self) -> Int { 0 } }\n 0"));

    // A valid Char program still COMPILES (guards the negatives are specific).
    check_true("cu_valid_compiles",  !cg_check_fails_p("let c: Char = 'A'\n print(c)\n codePoint(c)"));
}

void test_batch_round2() {
    std::cout << "[batch: else-diag / while-let / range-pat / %g]\n";

    // ---- #1 `else match`/`else while`/... diagnostic (a PARSE error; `else if`/`else {..}` still ok) ----
    check_true("elsediag_match",  parse_throws("fn f(x: Int) -> Int { if x > 0 { 1 } else match x { _ => 2 } }"));
    check_true("elsediag_while",  parse_throws("fn f(x: Int) -> Int { if x > 0 { 1 } else while x > 0 { 2 } }"));
    check_true("elsediag_loop",   parse_throws("fn f(x: Int) -> Int { if x > 0 { 1 } else loop { 2 } }"));
    check_true("elsediag_for_ok", !parse_throws("fn f(x: Int) -> Int { if x > 0 { 1 } else { match x { _ => 2 } } }"));
    check_int ("elsediag_elseif", "fn f(x: Int) -> Int { if x > 0 { 1 } else if x < 0 { 2 } else { 3 } }\n f(0 - 5)", 2);

    // ---- #2 while let (desugar to loop + match + break) ----
    check_int ("whilelet_bool",   "let mut i = 0\n let mut s = 0\n while let true = i < 5 { s = s + i\n i = i + 1 }\n s", 10);
    check_int_p("whilelet_option",
        "let mut v: Vec[Int] = vec()\n push(v, 1) push(v, 2) push(v, 3)\n"
        " let mut t = 0\n while let Some(x) = pop(v) { t = t + x }\n t", 6);
    check_same("diff_whilelet",
        "let mut v: Vec[Int] = vec()\n push(v, 4) push(v, 5) push(v, 6)\n"
        " let mut t = 0\n while let Some(x) = pop(v) { t = t + x }\n println(toString(t))", true);

    // ---- #3 range patterns `lo..hi` (inclusive; Int/Double; char literals are Int) ----
    check_int ("range_digit",     "match '5' { '0'..'9' => 1, _ => 0 }", 1);
    check_int ("range_miss",      "match '#' { '0'..'9' => 1, _ => 0 }", 0);
    check_int ("range_bounds_lo", "match '0' { '0'..'9' => 1, _ => 0 }", 1);   // inclusive lower
    check_int ("range_bounds_hi", "match '9' { '0'..'9' => 1, _ => 0 }", 1);   // inclusive upper
    check_int ("range_neg",       "match (0 - 3) { -5..-1 => 1, _ => 0 }", 1);
    check_int ("range_double",    "match 2.5 { 1.0..3.0 => 1, _ => 0 }", 1);
    check_int ("range_or",        "match 'A' { '0'..'9' | 'a'..'z' | 'A'..'Z' => 1, _ => 0 }", 1);
    check_int ("range_after_lit", "match 5 { 0 => 100, 1..9 => 2, _ => 0 }", 2);
    // Exhaustiveness: a range NEVER completes a signature -> a match of only ranges needs `_`.
    check_true("range_nonexhaustive", cg_check_fails("match 5 { 0..9 => 1 }"));
    check_true("range_two_nonexh",    cg_check_fails("match 5 { 0..9 => 1, 10..20 => 2 }"));
    check_true("range_with_wild_ok",  !cg_check_fails("match 5 { 0..9 => 1, _ => 0 }"));
    // Non-numeric bounds are a type error.
    check_true("range_str_rejected",  cg_check_fails("match \"a\" { \"a\"..\"z\" => 1, _ => 0 }"));
    // Differential vs the oracle.
    check_same("diff_range_class",
        "fn c(x: Int) -> String { match x { '0'..'9' => \"d\", 'a'..'z' => \"l\", _ => \"o\" } }\n"
        " println(c('7')) println(c('m')) println(c('!'))", false);

    // ---- `lo..<hi` -- the EXCLUSIVE upper bound (Swift spelling; `..` stays inclusive) ----
    check_int ("rangex_lo",       "match 0 { 0..<10 => 1, _ => 0 }", 1);   // lower bound still inclusive
    check_int ("rangex_inside",   "match 9 { 0..<10 => 1, _ => 0 }", 1);
    check_int ("rangex_hi_excl",  "match 10 { 0..<10 => 1, _ => 0 }", 0);  // upper bound EXCLUDED
    check_int ("rangex_below",    "match 0 { 1..<10 => 1, _ => 0 }", 0);
    check_int ("rangex_neg",      "match (0 - 5) { -5..<-1 => 1, _ => 0 }", 1);
    check_int ("rangex_neg_hi",   "match (0 - 1) { -5..<-1 => 1, _ => 0 }", 0);
    check_int ("rangex_char",     "match '9' { '0'..<'9' => 1, _ => 0 }", 0);
    check_int ("rangex_spaced",   "match 5 { 1 ..< 9 => 1, _ => 0 }", 1);  // the token, not a digraph
    check_int ("rangex_or",       "match 'a' { '0'..<'9' | 'a'..<'z' => 1, _ => 0 }", 1);
    // On INT bounds the two spellings are interchangeable -- `0..<10` is exactly `0..9`.
    check_int ("rangex_int_same_in",  "match 9 { 0..<10 => 1, _ => 0 } + match 9 { 0..9 => 1, _ => 0 }", 2);
    check_int ("rangex_int_same_out", "match 10 { 0..<10 => 1, _ => 0 } + match 10 { 0..9 => 1, _ => 0 }", 0);
    // The one case only `..<` can express: a half-open DOUBLE range (no writable largest double < 1.0).
    check_int ("rangex_double_in",  "match 0.999 { 0.0..<1.0 => 1, _ => 0 }", 1);
    check_int ("rangex_double_out", "match 1.0 { 0.0..<1.0 => 1, _ => 0 }", 0);
    // Exhaustiveness is unchanged: a range never completes a signature.
    check_true("rangex_nonexhaustive", cg_check_fails("match 5 { 0..<9 => 1 }"));
    // The Maranget con id must distinguish the two forms. `1..9` and `1..<9` are DIFFERENT sets, so
    // the second is reachable (it is not a duplicate) -- while a real duplicate is still caught.
    check_true("rangex_distinct_ok",  !cg_check_fails("match 5 { 1..9 => 1, 1..<9 => 2, _ => 3 }"));
    check_true("rangex_dup_caught",   cg_check_fails("match 5 { 1..<9 => 1, 1..<9 => 2, _ => 3 }"));
    check_true("rangex_incl_dup_caught", cg_check_fails("match 5 { 1..9 => 1, 1..9 => 2, _ => 3 }"));
    // Differentials: the VM's BGE_NUM branch and the oracle's `<` must agree, at the bound and inside.
    check_same("diff_rangex_int",
        "fn g(n: Int) -> String { match n { 0..<60 => \"F\", 60..<70 => \"D\", _ => \"A\" } }\n"
        " println(g(59)) println(g(60)) println(g(69)) println(g(70))", false);
    check_same("diff_rangex_double",
        "fn c(x: Double) -> Int { match x { 0.0..<1.0 => 1, 1.0..2.0 => 2, _ => 3 } }\n"
        " println(c(0.0)) println(c(0.999)) println(c(1.0)) println(c(2.0)) println(c(2.5))", false);

    // ---- a pattern literal / range bound is a LEAF -- never an expression (parse_pattern_literal) ----
    // `parse_prefix` only looked restricted: its unary arm parses the operand with parse_expr(UNARY_BP)
    // and POSTFIX_BP > UNARY_BP, so a call/index/field slipped through on BOTH sides. That is a frame
    // under-measure, not just a surprise -- measure_pattern's Literal and Range cases each return a
    // hardcoded {1, 0} and never walk the bounds, so the argument temps were never budgeted.
    check_true("rangeb_expr_hi_rejected",  parse_throws("match 5 { 1..(3+4) => 1, _ => 0 }"));
    check_true("rangeb_call_hi_rejected",
        parse_throws("fn g(n: Int) -> Int { n }\n match 5 { 1..(g(3)) => 1, _ => 0 }"));
    // THE lower-bound falsifier: goes green->red the moment only the `hi` side is restricted.
    check_true("rangeb_call_lo_rejected",
        parse_throws("fn g(n: Int) -> Int { n }\n match 5 { -g(3)..0 => 1, _ => 0 }"));
    // The same door one level over: a plain LITERAL pattern could hide a call behind the sign too.
    check_true("rangeb_lit_call_rejected",
        parse_throws("fn g(n: Int) -> Int { n }\n match 5 { -g(3) => 1, _ => 0 }"));
    check_true("rangeb_index_hi_rejected",
        parse_throws("let xs = [1, 2]\n match 5 { 1..-xs[0] => 1, _ => 0 }"));
    check_true("rangeb_block_hi_rejected", parse_throws("match 5 { 1..{ 9 } => 1, _ => 0 }"));
    check_true("rangeb_interp_hi_rejected", parse_throws("let s = 1\n match 5 { 1..\"${s}\" => 1, _ => 0 }"));
    check_true("rangeb_double_minus_rejected", parse_throws("match 5 { --5..0 => 1, _ => 0 }"));
    // A bare NAME parses as a bound; what it may MEAN is the checker's call (test_range_const_bounds).
    check_true("rangeb_ident_parses",     !parse_throws("match 5 { 1..k => 1, _ => 0 }"));
    check_true("rangeb_unknown_rejected",  cg_check_fails("match 5 { 1..k => 1, _ => 0 }"));
    // ...and the sign that a sloppy restriction breaks: a signed bound must still work on both sides.
    check_int ("rangeb_neg_both",  "match (0 - 3) { -5..-1 => 1, _ => 0 }", 1);
    check_int ("rangeb_neg_excl",  "match (0 - 2) { -5..<-1 => 1, _ => 0 }", 1);
    check_int ("rangeb_neg_double","match (0.0 - 2.5) { -5.0..-1.0 => 1, _ => 0 }", 1);

    // ---- the Maranget con id for a Double must be INJECTIVE (double_con, not std::to_string) ----
    // `std::to_string` is `%f` -- six fractional digits -- so two arms differing past the 6th digit
    // shared one con and the second was falsely reported "unreachable match arm". Pure literals; this
    // was reachable long before range bounds could name anything.
    check_true("dblcon_near_lits_ok",   !cg_check_fails("match 1.0 { 1.0000001 => 1, 1.0000002 => 2, _ => 0 }"));
    check_int ("dblcon_near_lits_value", "match 1.0000002 { 1.0000001 => 1, 1.0000002 => 2, _ => 0 }", 2);
    check_true("dblcon_near_range_ok",
        !cg_check_fails("match 1.5 { 1.0000001..2.0 => 1, 1.0000002..3.0 => 2, _ => 0 }"));
    check_true("dblcon_tiny_ok",        !cg_check_fails("match 1.0 { 0.0000001 => 1, 0.0000002 => 2, _ => 0 }"));
    // ...and the guard against "fix" it by handing out a unique id per site: a REAL duplicate must
    // still be caught, for a literal and for a range alike.
    check_true("dblcon_dup_still_caught",  cg_check_fails("match 1.5 { 1.5 => 1, 1.5 => 2, _ => 0 }"));
    check_true("dblcon_dup_range_caught",  cg_check_fails("match 1.5 { 1.5..2.5 => 1, 1.5..2.5 => 2, _ => 0 }"));

    // ---- #4 `%g` float formatter (shortest round-trip; precision ignored in v1) ----
    check_true("g_basic",   cg_run_out("println(\"${3.14159:g}\")")   == "3.14159\n");
    check_true("g_width",   cg_run_out("println(\"[${3.14:>8g}]\")")  == "[    3.14]\n");
    check_true("g_big",     cg_run_out("println(\"${1000000.5:g}\")") == "1000000.5\n");
    check_true("g_prec_ignored", cg_run_out("println(\"${3.14159:.2g}\")") == "3.14159\n");
    check_true("g_int",     cg_run_out("println(\"${42:g}\")")        == "42\n");
}

// =============================================================================
// std::set -- the opt-in hash Set[T] over Map[T, Bool], pure prelude. Mirrors test_std_bytes:
// a `use std::set::*` prefix + a gate pair. Value tests are VM-only (check_int_p/check_bool_p);
// the differentials use only ORDER-INDEPENDENT results (size/membership/union-size), safe despite
// the hash-order iteration that Set inherits from Map.
// =============================================================================
// std::core Map ergonomics -- getOr / getOrElse / getOrPut / upsert. Pure prelude helpers over the `get`
// builtin + `m[k] = v`; ring names (bare, no `use`). All results here are scalars / order-independent, so
// each is also cross-checked VM == RefEval via check_same.
// emptyArray[T]() -- an empty fixed Array[T] (T inferred like vec()); a zero-count ANEW. Sound with no
// init value (0 slots, no element is ever read). The prerequisite that unblocks `impl Clone for Array`.
void test_empty_array() {
    std::cout << "[codegen: emptyArray]\n";
    check_int_p("emptyarray_len",   "let a: Array[Int] = emptyArray()\n len(a)", 0);
    check_int_p("emptyarray_double", "let a: Array[Double] = emptyArray()\n len(a)", 0);
    // Inference from a function's declared return type (no annotation at the call site).
    check_int_p("emptyarray_ret",
        "fn mk() -> Array[Int] { emptyArray() }\n len(mk())", 0);
    // Non-empty companion: array(n, init) still fills; emptyArray is the len-0 case they share.
    check_int_p("emptyarray_vs_array", "let a = array(3, 0)\n let b: Array[Int] = emptyArray()\n len(a) - len(b)", 3);
    check_same("diff_emptyarray", "let a: Array[Int] = emptyArray()\n len(a)", true);
}

// Compound assignment `a op= b` for + - * / % over Int and Double. The operator rides on AssignStmt
// (it is NOT desugared), so codegen evaluates the place exactly once -- which is what `ca_once_*`
// pins down. String is deliberately excluded, with a diagnostic that says why.
void test_compound_assign() {
    std::cout << "[codegen: compound assignment `a op= b`]\n";

    // All five operators, Int target.
    check_int_p("cpa_int_add", "let mut i = 10\n i += 5\n i", 15);
    check_int_p("cpa_int_sub", "let mut i = 10\n i -= 3\n i", 7);
    check_int_p("cpa_int_mul", "let mut i = 10\n i *= 4\n i", 40);
    check_int_p("cpa_int_div", "let mut i = 10\n i /= 3\n i", 3);      // truncates toward zero
    check_int_p("cpa_int_mod", "let mut i = 10\n i %= 3\n i", 1);
    check_int_p("cpa_chain",   "let mut i = 1\n i += 2\n i *= 5\n i -= 3\n i", 12);
    // `+= 1` / `-= 1` take the INCR/DECR shortcut; the result must be identical either way.
    check_int_p("cpa_incr",    "let mut i = 41\n i += 1\n i", 42);
    check_int_p("cpa_decr",    "let mut i = 43\n i -= 1\n i", 42);
    check_int_p("cpa_incr_loop","let mut n = 0\n let mut s = 0\n while n < 5 { s += n  n += 1 }\n s", 10);
    // Double target, and the Int->Double widening at the boundary (`d += 1` with an Int operand).
    check_true("cpa_dbl_add",  cg_run_out("let mut d = 10.0\n d += 1\n println(d)") == "11.0\n");
    check_true("cpa_dbl_mul",  cg_run_out("let mut d = 2.0\n d *= 2.5\n println(d)") == "5.0\n");
    check_true("cpa_dbl_mod",  cg_run_out("let mut d = 5.5\n d %= 4.0\n println(d)") == "1.5\n");
    // Field target -- read-modify-write through GET_PROP/SET_PROP.
    check_int_p("cpa_field",   "struct P { n: Int }\n let mut p = P { n: 5 }\n p.n += 7\n p.n", 12);
    check_true("cpa_field_dbl",
        cg_run_out("struct P { d: Double }\n let mut p = P { d: 1.5 }\n p.d *= 2\n println(p.d)") == "3.0\n");
    // Index target on a Vec, and on a Map key that EXISTS.
    check_int_p("cpa_index",   "let mut v = toVec([1, 2, 3])\n v[1] += 40\n v[1]", 42);
    check_int_p("cpa_index_mul","let mut v = toVec([1, 2, 3])\n v[2] *= 10\n v[2]", 30);
    check_int_p("cpa_map",     "let mut m = #{ \"a\" => 1 }\n m[\"a\"] += 9\n m[\"a\"]", 10);
    // Bytes is the fourth index receiver, and the only one whose WRITE masks: the element is an Int
    // 0..255, so an overflowing compound result wraps (66 + 200 = 266 -> 10). The oracle masks in the
    // same place (store_index), which is what the differential below pins.
    check_int_p("cpa_bytes",     "let mut b = toBytes(\"AB\")\n b[0] += 1\n b[0]", 66);
    check_int_p("cpa_bytes_mask","let mut b = toBytes(\"AB\")\n b[1] += 200\n b[1]", 10);

    // THE once-evaluation guarantee. A desugar to `t = t op v` would run the index expression twice;
    // `log` records one push per call, so a second evaluation would show up as an extra element.
    check_int_p("cpa_once_index",
        "fn bump(mut c: Vec[Int]) -> Int { push(c, 1)\n 0 }\n"
        " let mut log: Vec[Int] = vec()\n let mut v = toVec([0, 0])\n"
        " v[bump(log)] += 100\n len(log) * 1000 + v[0]", 1100);
    // (There is no field counterpart to ca_once_index: the M5 root rule requires a field target's
    // receiver to be a `mut` variable PATH, so it cannot contain a call, and a second evaluation
    // would be unobservable. The index expression is the only lvalue part that can have effects.)

    // Type rules: the compound form is checked as `t = t op v` against t's type.
    check_true("cpa_int_takes_double", check_has("let mut i = 1\n i += 2.5\n i", "expected Int, found Double"));
    check_true("cpa_string_rejected",  check_has("let mut s = \"a\"\n s += \"b\"\n 0", "numeric-only"));
    check_true("cpa_string_hint",      check_has("let mut s = \"a\"\n s += \"b\"\n 0", "StringBuilder"));
    check_true("cpa_bool_rejected",    check_has("let mut b = true\n b += true\n 0", "numeric-only"));
    // The `mut` rules are inherited unchanged from plain assignment.
    check_true("cpa_needs_mut",        check_has("let i = 1\n i += 2\n i", "immutable binding"));
    check_true("cpa_field_needs_mut",
        check_has("struct P { n: Int }\n let p = P { n: 1 }\n p.n += 2\n 0", "immutable binding"));
    // Traps are inherited too: integer /= 0 and %= 0 abort, as `/` and `%` do.
    check_true("cpa_div_zero", check_has_p("let mut i = 1\n i /= 0\n i", "division by zero"));
    check_true("cpa_mod_zero", check_has_p("let mut i = 1\n i %= 0\n i", "division by zero"));
    // A map key that does NOT exist traps -- `m[k] += 1` is exactly `m[k] = m[k] + 1`, and the read
    // is the total-or-trap form. Compound assignment does NOT replace getOrPut for a fresh entry.
    check_true("cpa_map_missing_traps",
        check_has_p("let mut m = #{ \"a\" => 1 }\n m[\"zz\"] += 1\n 0", "map key not found"));

    // Differentials: the oracle must model the same read-modify-write over one place evaluation.
    check_same("cpa_diff_int",
        "let mut i = 7\n i += 5\n i *= 3\n i -= 4\n i /= 2\n i %= 7\n println(i)\n 0", false);
    check_same("cpa_diff_double",
        "let mut d = 7.5\n d += 2\n d *= 1.5\n d -= 0.25\n println(d)\n 0", false);
    check_same("cpa_diff_field",
        "struct P { n: Int, d: Double }\n let mut p = P { n: 3, d: 1.5 }\n"
        " p.n *= 4\n p.d += 2\n println(p.n) println(p.d)\n 0", false);
    check_same("cpa_diff_index",
        "let mut v = array(3, 7)\n v[0] += 10\n v[1] *= 5\n v[2] -= 1\n"
        " println(v[0]) println(v[1]) println(v[2])\n 0", false);
    check_same("cpa_diff_map",
        "let mut m = #{ 1 => 10, 2 => 20 }\n m[1] += 5\n m[2] *= 2\n"
        " println(m[1]) println(m[2])\n 0", false);
    check_same("cpa_diff_bytes",
        "let mut b = toBytes(\"AB\")\n b[0] += 1\n b[1] += 200\n"
        " println(b[0]) println(b[1])\n 0", false);
    check_same("cpa_diff_loop",
        "let mut s = 0\n let mut i = 0\n while i < 6 { s += i * i  i += 1 }\n println(s)\n 0", false);

    // ---- the BITWISE / SHIFT six (`&= |= ^= <<= >>= >>>=`) --------------------------------------
    // Same mechanism as the arithmetic five (`arith_opcode` already mapped all six), so the interest
    // is entirely in the type rule (Int-only, both sides) and in the lexeme boundaries.
    check_int_p("cpa_bit_and",  "let mut i = 12\n i &= 10\n i", 8);
    check_int_p("cpa_bit_or",   "let mut i = 12\n i |= 3\n i", 15);
    check_int_p("cpa_bit_xor",  "let mut i = 12\n i ^= 10\n i", 6);
    check_int_p("cpa_bit_shl",  "let mut i = 3\n i <<= 4\n i", 48);
    check_int_p("cpa_bit_shr",  "let mut i = 48\n i >>= 4\n i", 3);
    // `>>=` is arithmetic (sign-preserving); `>>>=` is logical over the 48-bit value, so a negative
    // operand is where the two must disagree -- -16 is 0xFFFFFFFFFFF0, logically halved = 0x7FFFFFFFFFF8.
    check_int_p("cpa_bit_shr_neg",  "let mut i = -16\n i >>= 1\n i", -8);
    check_int_p("cpa_bit_ushr_neg", "let mut i = -16\n i >>>= 1\n i", 140737488355320LL);
    check_int_p("cpa_bit_chain", "let mut f = 0b1100\n f &= 0b1010\n f |= 0b0011\n f ^= 0b0001\n f <<= 3\n f", 80);
    // Field / element / map-key targets take the same read-modify-write paths as the arithmetic five.
    check_int_p("cpa_bit_field",
        "struct F { bits: Int }\n let mut f = F { bits: 1 }\n f.bits |= 4\n f.bits", 5);
    check_int_p("cpa_bit_index", "let mut v = toVec([1, 2])\n v[1] |= 6\n v[1] <<= 1\n v[1]", 12);
    check_int_p("cpa_bit_map",   "let mut m = #{ 1 => 12 }\n m[1] &= 10\n m[1]", 8);
    check_int_p("cpa_bit_bytes", "let mut b = toBytes(\"A\")\n b[0] |= 32\n b[0]", 97);   // 'A' | 0x20 = 'a'
    // ASI: a line opening with a compound operator continues the previous one, exactly as `= 5` and
    // `+= 5` do. Without `starts_continuation` this would be cut into two statements and not parse.
    check_int_p("cpa_bit_asi",   "let mut i = 7\n i\n  &= 5\n i", 5);
    // The type rule -- INTEGER-only on both sides, and the StringBuilder advice must NOT appear (it is
    // nonsense for `s |= t`, which has no string meaning at all).
    check_true("cpa_bit_double_target", check_has("let mut d = 1.0\n d &= 1\n 0", "integer-only (Int)"));
    check_true("cpa_bit_double_operand", check_has("let mut i = 1\n i |= 2.0\n 0", "needs an Int operand"));
    check_true("cpa_bit_string_target", check_has("let mut s = \"a\"\n s |= \"b\"\n 0", "integer-only (Int)"));
    check_true("cpa_bit_no_sb_hint",   !check_has("let mut s = \"a\"\n s |= \"b\"\n 0", "StringBuilder"));
    check_true("cpa_bit_bool_rejected", check_has("let mut b = true\n b ^= true\n 0", "integer-only (Int)"));
    // The arithmetic forms are unchanged by the split: Double still widens, the hint still fires.
    check_true("cpa_bit_arith_dbl_ok", check_errc("let mut d = 1.0\n d += 1\n println(d)") == 0);
    check_true("cpa_bit_arith_hint",   check_has("let mut s = \"a\"\n s += \"b\"\n 0", "StringBuilder"));
    // Differential -- the oracle routes a bitwise compound to `bitwise()`, not `arith()`. Without that
    // it would raise Unsupported, which the harness counts as a SKIP: the feature would look verified
    // while being unchecked. This case is what makes that impossible.
    check_same("cpa_diff_bitwise",
        "let mut i = 0b1011\n i &= 0b1110\n i |= 0b0100\n i ^= 0b0010\n i <<= 2\n i >>= 1\n"
        " let mut n = -32\n n >>>= 2\n n >>= 1\n"
        " struct F { b: Int }\n let mut f = F { b: 5 }\n f.b ^= 7\n"
        " let mut v = array(1, 9)\n v[0] |= 16\n"        // a builtin: this differential runs prelude-free
        " println(i) println(n) println(f.b) println(v[0])\n 0", false);
}

// Clone -- shallow independent copies of containers + user-extensible via a user `impl Clone`. Each container
// test checks BOTH element-equality AND independence (mutating the clone leaves the original untouched).
void test_clone() {
    std::cout << "[codegen: clone]\n";

    // Vec: element-equal + independent.
    check_int_p("clone_vec_eq",
        "let mut a: Vec[Int] = vec()\n push(a, 10) push(a, 20)\n let b = clone(a)\n b[0] * 100 + b[1]", 1020);
    check_int_p("clone_vec_indep",
        "let mut a: Vec[Int] = vec()\n push(a, 1) push(a, 2)\n let mut b = clone(a)\n push(b, 3)\n len(a) * 100 + len(b)", 203);

    // Map: element-equal + independent (clone must be `mut` to reassign a key).
    check_int_p("clone_map_eq",
        "let m = #{ 1 => 10, 2 => 20 }\n let c = clone(m)\n getOr(c, 1, 0) * 100 + getOr(c, 2, 0)", 1020);
    check_int_p("clone_map_indep",
        "let m = #{ 1 => 10 }\n let mut c = clone(m)\n c[1] = 99\n getOr(m, 1, 0) * 100 + getOr(c, 1, 0)", 1099);

    // Bytes: element-equal + independent.
    check_int_p("clone_bytes_eq",
        "let mut b = bytes(0)\n push(b, 65) push(b, 66)\n let c = clone(b)\n c[0] * 100 + c[1]", 6566);
    check_int_p("clone_bytes_indep",
        "let mut b = bytes(0)\n push(b, 65) push(b, 66)\n let mut c = clone(b)\n push(c, 67)\n len(b) * 100 + len(c)", 203);

    // Array: element-equal + independent + the empty-array branch (uses emptyArray()).
    check_int_p("clone_array_eq",  "let a = array(3, 7)\n let c = clone(a)\n c[0] + c[1] + c[2]", 21);
    check_int_p("clone_array_indep",
        "let a = array(2, 5)\n let mut c = clone(a)\n c[0] = 99\n a[0] * 100 + c[0]", 599);
    check_int_p("clone_array_empty", "let a: Array[Int] = emptyArray()\n let c = clone(a)\n len(c)", 0);

    // User-extensible: a user `impl Clone` dispatches like the built-in ones (the whole point).
    check_int_p("clone_user_struct",
        "struct P { x: Int, y: Int }\n"
        "impl Clone for P { fn clone(self) -> P { P { x: self.x, y: self.y } } }\n"
        "let p = P { x: 3, y: 4 }\n let q = clone(p)\n q.x * 100 + q.y", 304);

    // `dyn Clone` is rejected: `-> Self` is object-unsafe (OS3), so no trait object may form.
    check_true("clone_dyn_rejected", cg_check_fails("fn f(x: dyn Clone) -> () { }"));

    // Differentials (order-deterministic scalar results; Map returns scalars, never a hash-order dump).
    check_same("diff_clone_vec",
        "let mut a: Vec[Int] = vec()\n push(a, 3) push(a, 4)\n let mut b = clone(a)\n push(b, 5)\n len(a) * 100 + b[2]", true);
    check_same("diff_clone_array",
        "let a = array(2, 9)\n let mut c = clone(a)\n c[0] = 1\n a[0] * 10 + c[0]", true);
    check_same("diff_clone_bytes",
        "let mut b = bytes(0)\n push(b, 7) push(b, 8)\n let c = clone(b)\n c[0] * 10 + c[1]", true);
}

// Or-patterns `A | B | C => body` (v1a: top-level, no bindings). Value semantics, exhaustiveness
// through OR, redundant-alternative warnings, and the no-binding rejection.
void test_pattern_alt() {
    std::cout << "[codegen: pattern-alternation]\n";

    // Int-literal OR: any listed literal takes the arm.
    check_int_p("or_int_first",  "match 0 { 0 | 1 | 2 => 10, _ => 20 }", 10);
    check_int_p("or_int_mid",    "match 1 { 0 | 1 | 2 => 10, _ => 20 }", 10);
    check_int_p("or_int_last",   "match 2 { 0 | 1 | 2 => 10, _ => 20 }", 10);
    check_int_p("or_int_miss",   "match 9 { 0 | 1 | 2 => 10, _ => 20 }", 20);
    // Two OR arms, disjoint.
    check_int_p("or_two_arms",   "match 3 { 0 | 1 => 1, 2 | 3 => 2, _ => 9 }", 2);
    // Leading `|` (vertical formatting) is accepted.
    check_int_p("or_leading_bar","match 5 { | 4 | 5 => 7, _ => 0 }", 7);

    // Char-literal OR: the byte-classifier use case (the motivating pain point).
    check_int_p("or_char_digit",
        "fn kind(c: Int) -> Int { match c { '0' | '1' | '2' | '3' | '4' | '5' | '6' | '7' | '8' | '9' => 1, "
        "'a' | 'b' | 'c' => 2, _ => 0 } }\n kind('7') * 100 + kind('b') * 10 + kind('z')", 120);

    // Enum nullary-variant OR + exhaustive WITHOUT a wildcard (OR completes the signature).
    check_int_p("or_enum_exhaustive",
        "enum C { Red, Green, Blue }\n"
        "fn warm(x: C) -> Int { match x { Red | Green => 1, Blue => 0 } }\n"
        "warm(Red) * 100 + warm(Green) * 10 + warm(Blue)", 110);
    // A single OR arm covering every variant is exhaustive.
    check_int_p("or_enum_all",
        "enum C { Red, Green, Blue }\n match Green { Red | Green | Blue => 42 }", 42);

    // Bool OR is exhaustive (true|false completes the Bool signature).
    check_int_p("or_bool_all", "match true { true | false => 5 }", 5);

    // if-let with an OR pattern (desugars to a 2-arm match).
    check_int_p("or_if_let",   "let x = 2\n if let 1 | 2 | 3 = x { 100 } else { 200 }", 100);
    check_int_p("or_if_let_else", "let x = 8\n if let 1 | 2 | 3 = x { 100 } else { 200 }", 200);

    // --- exhaustiveness / reachability ---
    // An OR arm that omits a variant is still non-exhaustive.
    check_true("or_nonexhaustive",
        cg_check_fails("enum C { Red, Green, Blue }\n match Red { Red | Green => 1 }"));
    // A whole arm all of whose alternatives are already covered is unreachable (hard error).
    check_true("or_arm_unreachable",
        cg_check_fails("match 5 { 1 => 1, 2 => 2, 1 | 2 => 3, _ => 4 }"));

    // --- redundant single alternative: WARNING tier, not an error ---
    check_true("or_redundant_alt_warns",
        check_warn_has("match 5 { 1 | 1 | 2 => 1, _ => 2 }", "unreachable pattern alternative"));
    check_true("or_redundant_alt_count", check_warnc("match 5 { 1 | 1 | 2 => 1, _ => 2 }") == 1);
    // A clean (disjoint) OR arm warns about nothing.
    check_true("or_clean_no_warn", check_warnc("match 5 { 1 | 2 => 1, _ => 2 }") == 0);

    // --- bindings in alternatives: LEGAL, provided every alternative binds the same set ---
    //
    // Each of these was a rejection earlier. Two became legal; two stay rejections but
    // under a DIFFERENT rule, which is why every rejection below is pinned by MESSAGE as well as by
    // outcome -- asserting only "it fails" would have let them read green whether the feature works
    // or was never built.
    check_int_p("or_binding_ident_first",
        "enum E { A(Int), B(Int) }\n match A(1) { A(x) | B(x) => x }", 1);
    check_int_p("or_binding_ident_second",
        "enum E { A(Int), B(Int) }\n match B(2) { A(x) | B(x) => x }", 2);
    // Three alternatives, so the canonical register is written from a MIDDLE one too -- not just
    // from the first (which allocates first) or the last (which falls through).
    check_int_p("or_binding_three_alts",
        "enum E3 { A(Int), B(Int), C(Int) }\n"
        "fn f(e: E3) -> Int { match e { A(x) | B(x) | C(x) => x * 10 } }\n"
        " f(A(1)) + f(B(2)) + f(C(3))", 60);
    // A record-variant shorthand binds too. NOT `P { x } | P { x }` -- identical alternatives are
    // now legal AND redundant, so that would trip the unreachable-alternative WARNING instead.
    check_int_p("or_binding_struct_shorthand",
        "enum R { L { x: Int }, M { x: Int } }\n"
        "fn f(r: R) -> Int { match r { L { x } | M { x } => x } }\n"
        " f(L { x: 3 }) * 10 + f(M { x: 4 })", 34);

    // The two that stay rejections, now for "alternatives must bind the same set".
    const std::string or_bind_mut = "match 5 { mut x | 2 => x, _ => 0 }";
    check_true("or_binding_mut_rejected", cg_check_fails(or_bind_mut));
    check_true("or_binding_mut_message",  check_has(or_bind_mut, "does not bind 'x'"));

    // --- the SAME-BINDER-SET rule is fully checked, even while lowering is still refused ---
    const char* E2 = "enum E { A(Int), B(Int) }\n";

    // THE RESTORE PROBE. Each alternative is checked for real and its bindings are then unwound, so
    // the arm scope must end up with exactly ONE `x`. Two warnings would mean the unwind did not
    // happen (both alternatives' defines survive); zero would mean it unwound too much.
    check_true("or_bind_restore_one_warning",
        check_warnc(std::string(E2) + " match A(1) { A(x) | B(x) => 0 }") == 1);
    check_true("or_bind_restore_warns_unused",
        check_warn_has(std::string(E2) + " match A(1) { A(x) | B(x) => 0 }", "unused variable 'x'"));

    // Differing binder SETS -- reported from both directions, each with a secondary caret.
    check_true("or_bind_missing_name",
        check_has(std::string(E2) + " match A(1) { A(x) | B(_) => 0 }", "does not bind 'x'"));
    check_true("or_bind_extra_name",
        check_has(std::string(E2) + " match A(1) { A(_) | B(y) => 0 }",
                  "'y' is not bound by every or-pattern alternative"));

    // Differing TYPES. Exact equality: Int and Double do NOT merge, because `unify` refuses them too.
    check_true("or_bind_type_mismatch",
        check_has("enum F { A(Int), B(String) }\n match A(1) { A(x) | B(x) => 0 }", "has type"));
    check_true("or_bind_int_double_rejected",
        check_has("enum G { A(Int), B(Double) }\n match A(1) { A(x) | B(x) => 0 }", "has type"));

    // ... but two still-FREE type variables DO merge -- the body would have resolved them anyway,
    // and rejecting that would be an over-rejection with no upside. `Rgt`'s payload type is still
    // open here, so exact equality alone would fail this.
    check_true("or_bind_free_var_merges",
        !check_has("enum Two[A, B] { Lft(A), Rgt(B) }\n let r = Lft(1)\n"
                   " match r { Lft(x) | Rgt(x) => 0 }", "has type"));

    // Differing MUTABILITY.
    check_true("or_bind_mut_mismatch",
        check_has(std::string(E2) + " match A(1) { A(mut x) | B(x) => 0 }", "is immutable here but 'mut'"));

    // NO CASCADE: a disagreement installs the UNION of the names, so the arm body still resolves
    // `x` and does not pile "unknown identifier" on top of the real diagnostic.
    check_true("or_bind_no_cascade",
        !check_has(std::string(E2) + " match A(1) { A(_) | B(x) => x + 1 }", "unknown"));

    // --- interactions: nesting, `@`, guards, captures, containers ---
    // NESTED: the inner Or gets its own canonical registers, which are locals and therefore outlive
    // the temp the enclosing `Some(...)` fetched into.
    check_int_p("or_bind_nested",
        "enum E { A(Int), B(Int) }\n"
        "fn f(o: Option[E]) -> Int { match o { Some(A(x) | B(x)) => x, None => -1 } }\n"
        " f(Some(A(5))) * 100 + f(Some(B(6))) * 10 + f(None) + 10", 569);
    // An `@` OVER a binding or-pattern: `w` is defined before the Or takes its snapshot, so it is
    // not in any alternative's diff and the same-set rule correctly demands only `x`.
    check_int_p("or_bind_under_at",
        "enum E { A(Int), B(Int) }\n"
        "fn f(e: E) -> Int { match e { w @ (A(x) | B(x)) => x + (match w { A(_) => 100, B(_) => 200 }) } }\n"
        " f(A(1)) * 1000 + f(B(2))", 101202);
    // An `@` INSIDE one alternative: both bind `n` at the payload type, so this needs Result[T,T].
    check_int_p("or_bind_at_inside",
        "enum P { A(Int), B(Int) }\n"
        "fn f(p: P) -> Int { match p { A(n @ 1..5) | B(n) => n, A(n) => n * 10 } }\n"
        " f(A(3)) * 100 + f(B(4)) * 10 + f(A(9))", 430);
    // ... and the mut rule reaches through the `@`.
    check_true("or_bind_at_mut_mismatch",
        check_has("enum P { A(Int), B(Int) }\n match A(1) { A(mut n @ 1..5) | B(n) => n, A(n) => n }",
                  "in another or-pattern alternative"));

    // A GUARD reads an or-bound name -- the cheapest end-to-end proof that the canonical register is
    // what the rest of the arm sees, since the guard is emitted after the whole pattern test.
    check_int_p("or_bind_guard",
        "enum E { A(Int), B(Int) }\n"
        "fn f(e: E) -> Int { match e { A(x) | B(x) if x > 10 => x, A(x) | B(x) => 0 - x } }\n"
        " f(A(20)) * 100 + f(B(3))", 1997);

    // A LAMBDA in the arm body capturing an or-bound name. Its failure mode is a hard
    // `internal: cannot capture 'x'`, not a wrong value, so it needs its own case.
    check_int_p("or_bind_lambda_capture",
        "enum E { A(Int), B(Int) }\n"
        "fn f(e: E) -> Int { match e { A(x) | B(x) => { let g: fn(Int) -> Int = fn(k) { k + x }\n g(100) } } }\n"
        " f(A(1)) * 1000 + f(B(2))", 101102);

    // An Or as a LIST element and as a MAP value -- both reach compile_pattern_test through a temp
    // the caller frees right after, so the canonical MOVs must run inside that lifetime.
    check_int_p("or_bind_in_list",
        "enum E { A(Int), B(Int) }\n"
        "fn f(xs: List[E]) -> Int { match xs { [A(x) | B(x)] => x, _ => -1 } }\n"
        " f([A(7)]) * 10 + f([B(8)])", 78);
    check_int_p("or_bind_in_map",
        "enum E { A(Int), B(Int) }\n"
        "fn f(m: Map[Int, E]) -> Int { match m { #{ 1 => A(x) | B(x) } => x, _ => -1 } }\n"
        " f(#{ 1 => A(4) }) * 10 + f(#{ 1 => B(5) })", 45);
    // A TUPLE element, where two independent Ors each need their own canonicals.
    check_int_p("or_bind_two_ors",
        "enum E { A(Int), B(Int) }\n"
        "fn f(t: (E, E)) -> Int { match t { (A(x) | B(x), A(y) | B(y)) => x * 10 + y } }\n"
        " f((A(1), B(2)))", 12);

    // --- differentials: the oracle is blind to codegen, so these are the real lock ---
    check_same("diff_or_bind",
        "enum E { A(Int), B(Int) }\n"
        "fn f(e: E) -> Int { match e { A(x) | B(x) => x * 2 } }\n f(A(3)) + f(B(4))", false);
    check_same("diff_or_bind_three",
        "enum E3 { A(Int), B(Int), C(Int) }\n"
        "fn f(e: E3) -> Int { match e { A(x) | B(x) | C(x) => x } }\n"
        " f(A(1)) * 100 + f(B(2)) * 10 + f(C(3))", false);
    check_same("diff_or_bind_nested",
        "enum E { A(Int), B(Int) }\n"
        "fn f(o: Option[E]) -> Int { match o { Some(A(x) | B(x)) => x, None => -1 } }\n"
        " f(Some(A(5))) + f(Some(B(6))) + f(None)", true);
    check_same("diff_or_bind_guard",
        "enum E { A(Int), B(Int) }\n"
        "fn f(e: E) -> Int { match e { A(x) | B(x) if x > 10 => x, A(x) | B(x) => 0 - x } }\n"
        " f(A(20)) + f(B(3))", false);
    check_same("diff_or_bind_result",
        "fn f(r: Result[Int, Int]) -> Int { match r { Ok(v) | Err(v) => v } }\n"
        " f(Ok(7)) * 10 + f(Err(8))", true);

    // --- differentials (scalar results, deterministic) ---
    check_same("diff_or_int", "match 2 { 0 | 1 => 100, 2 | 3 => 200, _ => 300 }", false);
    check_same("diff_or_char",
        "fn k(c: Int) -> Int { match c { '0' | '1' | '2' => 1, 'x' | 'y' => 2, _ => 0 } }\n"
        "k('1') * 100 + k('y') * 10 + k('z')", false);
    check_same("diff_or_enum",
        "enum C { Red, Green, Blue }\n"
        "fn w(x: C) -> Int { match x { Red | Blue => 1, Green => 0 } }\n w(Red) * 10 + w(Green)", true);

    // ---- NESTED / parenthesized alternation (shipped with @-bindings) ----------------------
    // Alternation is no longer top-level-only. Two spellings: bare where the terminator is
    // unambiguous (a ctor / tuple / list element, a struct field, a map value), parenthesized
    // anywhere else.
    check_int_p("or_nested_ctor",  "enum C { Red, Green, Blue }\n"
        "fn f(o: Option[C]) -> Int { match o { Some(Red | Green) => 1, Some(Blue) => 0, None => -1 } }\n"
        " f(Some(Green)) * 100 + f(Some(Blue)) * 10 + f(None) + 10", 109);
    check_int_p("or_nested_tuple", "fn f(t: (Int, Int)) -> Int { match t { (1 | 2, n) => n, _ => 0 } }\n"
        " f((2, 7))", 7);
    check_int_p("or_nested_list",  "match [1, 5] { [1 | 2, n] => n, _ => 0 }", 5);
    check_int_p("or_nested_field", "struct P { x: Int, y: Int }\n"
        "fn f(p: P) -> Int { match p { P { x: 1 | 2, y: n } => n, _ => 0 } }\n f(P { x: 2, y: 9 })", 9);
    check_int_p("or_paren_group",  "match 5 { (1 | 5 | 9) => 1, _ => 0 }", 1);
    check_int_p("or_paren_lead_bar", "match 5 { ( | 1 | 5) => 1, _ => 0 }", 1);

    // The row EXPANSION, not a conservative opaque leaf: a nested alternation still COMPLETES a
    // signature, so these need no wildcard. This is the whole reason the cartesian expansion was
    // built -- the cheap treatment would reject both as non-exhaustive.
    check_int_p("or_nested_exhaustive_bool",  "fn f(t: (Bool, Int)) -> Int { match t { (true | false, n) => n } }\n f((true, 4))", 4);
    check_int_p("or_nested_exhaustive_enum",  "enum C { Red, Green, Blue }\n"
        "fn f(o: Option[C]) -> Int { match o { Some(Red | Green | Blue) => 1, None => 0 } }\n f(None)", 0);
    // ... and it must still REJECT when the expansion leaves a gap.
    check_true("or_nested_nonexhaustive", cg_check_fails_p(
        "enum C { Red, Green, Blue }\n"
        "fn f(o: Option[C]) -> Int { match o { Some(Red | Green) => 1, None => 0 } }\n f(None)"));

    // `(A|B) | C` must NOT be reported unreachable. Without the parser-side FLATTENING it is:
    // check_match_coverage peels one level of Or into rows, the inner OrPat reaches lower_pat,
    // which lowers only its first alternative -- so the row set under-approximates and a legal
    // arm is rejected. This is the regression lock for that.
    check_int_p("or_paren_then_bar", "match 3 { (1 | 2) | 3 => 1, _ => 0 }", 1);
    check_true("or_paren_then_bar_ok", !cg_check_fails_p("match 3 { (1 | 2) | 3 => 1, _ => 0 }"));
    // Each source alternative still gets its own redundancy warning after expansion.
    check_true("or_nested_redundant_warns",
        check_warn_has("match 5 { 1 | 1 | 2 => 1, _ => 2 }", "unreachable pattern alternative"));

    check_same("diff_or_nested", "enum C { Red, Green, Blue }\n"
        "fn f(o: Option[C]) -> Int { match o { Some(Red | Blue) => 1, Some(Green) => 2, None => 3 } }\n"
        " f(Some(Red)) * 100 + f(Some(Green)) * 10 + f(None)", true);
    check_same("diff_or_paren_tuple",
        "fn f(t: (Int, Int)) -> Int { match t { (1 | 2, n) => n * 10, (a, b) => a + b } }\n"
        " f((1, 5)) + f((7, 3))", false);
}

// `@`-bindings: bind the WHOLE value at a position AND require a sub-pattern to match.
// Front end only -- no VM change, no opcode, one MOV per binding.
void test_at_bindings() {
    std::cout << "[codegen: @-bindings]\n";

    // ---- the two shapes, and why only one of them is new ----
    // A RANGE sub-pattern is pure ergonomics: `k if k >= 0 && k <= 9` already worked and is
    // exhaustiveness-equivalent (a range never completes a signature either way).
    check_int_p("at_range",        "match 5 { k @ 0..9 => k * 10, _ => 0 }", 50);
    check_int_p("at_range_miss",   "match 50 { k @ 0..9 => k * 10, _ => -1 }", -1);
    check_int_p("at_range_excl",   "match 9 { k @ 0..<9 => 1, k @ 9..9 => k, _ => 0 }", 9);
    check_int_p("at_literal",      "match 7 { k @ 7 => k * 3, _ => 0 }", 21);
    // A CTOR sub-pattern is what a guard CANNOT express: bind the whole and destructure it.
    check_int_p("at_ctor",         "fn f(o: Option[Int]) -> Int { match o { w @ Some(n) => n + (match w { Some(_) => 100, None => 0 }), None => -1 } }\n f(Some(5))", 105);
    check_int_p("at_struct",       "struct P { x: Int, y: Int }\n"
        "fn f(p: P) -> Int { match p { w @ P { x: 1, y: n } => n + w.x, _ => 0 } }\n f(P { x: 1, y: 9 })", 10);

    // ---- nesting: the binding takes the type of ITS position, not of the whole scrutinee ----
    check_int_p("at_nested",       "fn f(o: Option[Int]) -> Int { match o { Some(n @ 1..9) => n * 10, Some(n) => n, None => -1 } }\n f(Some(4))", 40);
    check_int_p("at_nested_miss",  "fn f(o: Option[Int]) -> Int { match o { Some(n @ 1..9) => n * 10, Some(n) => n, None => -1 } }\n f(Some(40))", 40);
    check_int_p("at_in_tuple",     "match (3, 4) { (a @ 1..5, b) => a * 10 + b, _ => 0 }", 34);
    check_int_p("at_in_list",      "match [2, 8] { [a @ 1..5, b] => a * 10 + b, _ => 0 }", 28);
    check_int_p("at_in_map",       "let m = #{ 1 => 5 }\n match m { #{ 1 => v @ 1..9 } => v, _ => 0 }", 5);
    check_int_p("at_chained",      "fn f(o: Option[Int]) -> Int { match o { a @ b @ Some(n) => n + (match a { Some(_) => 100, None => 0 }) + (match b { Some(_) => 20, None => 0 }), None => -1 } }\n f(Some(3))", 123);
    // `n @ m` -- the sub-pattern is itself a binding. Both names see the same value; the reason
    // compile_pattern_test recurses against the fresh LOCAL rather than against `rs`.
    check_int_p("at_over_ident",   "match 6 { a @ b => a * 10 + b, }", 66);

    // ---- mut ----
    check_int_p("at_mut",          "match 3 { mut k @ 0..9 => { k = k + 100\n k }\n k => k }", 103);
    check_true("at_not_mut_rejected", cg_check_fails_p("match 3 { k @ 0..9 => { k = k + 1\n k }\n _ => 0 }"));

    // ---- @ over a PARENTHESIZED or-pattern: the case the grouping production exists for ----
    check_int_p("at_or_group",     "match 25 { k @ (1..10 | 20..30) => k, _ => -1 }", 25);
    check_int_p("at_or_group_miss","match 15 { k @ (1..10 | 20..30) => k, _ => -1 }", -1);
    // A bare `|` after `@` binds looser than the `@`, so `n @ 1 | 2` is an ARM-level or-pattern
    // whose FIRST alternative binds `n` and whose second binds nothing. Or-patterns may bind,
    // but only if EVERY alternative binds the same set -- so this stays a rejection,
    // and the message pin is what proves it is rejected for that reason rather than by accident.
    check_true("at_bare_bar_rejected", cg_check_fails_p("match 1 { n @ 1 | 2 => n, _ => 0 }"));
    check_true("at_bare_bar_message",
        check_has_p("match 1 { n @ 1 | 2 => n, _ => 0 }", "does not bind 'n'"));

    // ---- exhaustiveness: the binding is invisible ----
    // `w @ Some(n)` covers exactly what `Some(n)` covers -- no more, no less.
    check_true("at_exhaustive_ok",  !cg_check_fails_p(
        "fn f(o: Option[Int]) -> Int { match o { w @ Some(n) => n, None => 0 } }\n f(None)"));
    check_true("at_nonexhaustive",   cg_check_fails_p(
        "fn f(o: Option[Int]) -> Int { match o { w @ Some(n) => n } }\n f(None)"));
    check_true("at_range_still_needs_wild", cg_check_fails_p("match 5 { k @ 0..9 => k }"));
    // `n @ (A | B)` must get the SAME full row treatment `A | B` gets -- that is the Bind-chain
    // unwrap in check_match_coverage. Without it the arm would look like one opaque row and this
    // wildcard-free match would be rejected.
    check_int_p("at_or_group_exhaustive",
        "fn f(b: Bool) -> Int { match b { k @ (true | false) => if k { 1 } else { 0 } } }\n f(true)", 1);

    // ---- rejected in irrefutable binder positions (its own message, not the refutable one) ----
    check_true("at_in_let_rejected",  cg_check_fails_p("let n @ 5 = 5\n n"));
    check_true("at_in_let_message",   check_has("let n @ 5 = 5\n n", "'@' binding is not allowed in 'let'"));
    check_true("at_in_for_message",   check_has("for n @ 1 in [1, 2] { }\n 0", "'@' binding is not allowed in 'for'"));
    // A list `..` tail must stay a plain name or `_` -- previously only enforced by a CodegenError
    // AFTER the checker had accepted the program.
    check_true("at_in_list_rest_rejected", cg_check_fails_p("match [1, 2] { [a, ..r @ [2]] => a, _ => 0 }"));
    check_true("at_list_rest_message",     check_has("match [1, 2] { [a, ..r @ [2]] => a, _ => 0 }",
                                                    "list '..' tail must be a name or '_'"));

    // ---- ASI: `@` continues the previous line, and never ends a statement ----
    check_int_p("at_asi_newline", "match 5 { k\n @ 0..9 => k, _ => 0 }", 5);

    // ---- differentials: the oracle is blind to codegen, so these are the real lock ----
    check_same("diff_at_range",  "fn f(n: Int) -> Int { match n { k @ 0..9 => k * 10, k @ 10..99 => k, _ => -1 } }\n"
                                 " f(5) + f(42) + f(500)", false);
    check_same("diff_at_ctor",   "fn f(o: Option[Int]) -> Int { match o { w @ Some(n) => n + (match w { Some(_) => 100, None => 0 }), None => -1 } }\n"
                                 " f(Some(5)) * 10 + f(None)", true);
    check_same("diff_at_nested", "fn f(o: Option[Int]) -> Int { match o { Some(n @ 1..9) => n * 10, Some(n) => n, None => -1 } }\n"
                                 " f(Some(4)) + f(Some(40)) + f(None)", true);
    check_same("diff_at_or",     "fn f(n: Int) -> Int { match n { k @ (1..10 | 20..30) => k, _ => 0 } }\n"
                                 " f(5) + f(25) + f(15)", false);
    check_same("diff_at_mut",    "fn f(n: Int) -> Int { match n { mut k @ 0..9 => { k = k + 100\n k }\n k => k } }\n"
                                 " f(3) + f(30)", false);
    // Inside a LAMBDA body: the binding must be seen by add_pattern_names, or free_vars_expr
    // treats it as a capture and emit_capture_source throws `internal: cannot capture`.
    check_same("diff_at_in_lambda",
        "let f: fn(Int) -> Int = fn(x) { match x { k @ 0..9 => k * 2, _ => x } }\n f(4) + f(40)", false);
}

// Range-pattern bounds may NAME a scalar `const`. Front end only -- no VM change, no opcode: the
// bound stays an IdentExpr and codegen's existing const path lowers it.
void test_range_const_bounds() {
    std::cout << "[check+codegen: const range bounds]\n";

    // ---- acceptance. Every one of these RUNS, not merely type-checks --------------------------
    // Codegen and the RefEval oracle key their const tables on the MANGLED name, so a bound that
    // only type-checks would still die at "undefined variable". Measured, though, that is NOT what
    // the gate's own writeback protects: `infer` runs `infer_ident` right after, which resolves and
    // writes back on its own. What the gate's writeback protects is `bound_literal` (here and in
    // lower_pat) and, decisively, a QUALIFIED bound -- see rcb_qualified_module below.
    check_int("rcb_basic",
        "const LO: Int = 3\n const HI: Int = 7\n"
        " fn f(x: Int) -> Int { match x { LO..HI => 1, _ => 0 } }\n"
        " f(2) + f(3)*10 + f(7)*100 + f(8)*1000", 110);
    check_int("rcb_const_lo_lit_hi",
        "const LO: Int = 3\n fn f(x: Int) -> Int { match x { LO..7 => 1, _ => 0 } }\n"
        " f(2) + f(5)*10", 10);
    check_int("rcb_lit_lo_const_hi",
        "const HI: Int = 7\n fn f(x: Int) -> Int { match x { 3..HI => 1, _ => 0 } }\n"
        " f(2) + f(5)*10", 10);
    check_int("rcb_exclusive",
        "const LO: Int = 3\n const HI: Int = 7\n"
        " fn f(x: Int) -> Int { match x { LO..<HI => 1, _ => 0 } }\n"
        " f(3)*100 + f(6)*10 + f(7)", 110);
    check_int("rcb_double",
        "const A: Double = 1.0\n const B: Double = 3.0\n"
        " fn f(x: Double) -> Int { match x { A..B => 1, _ => 0 } }\n f(2.5)*10 + f(3.5)", 10);
    check_int("rcb_negative",
        "const LO: Int = -5\n const HI: Int = -1\n"
        " fn f(x: Int) -> Int { match x { LO..HI => 1, _ => 0 } }\n f(0 - 3)*10 + f(0)", 10);
    // A lowercase const name is legal, and reads as a bound only because a bound is never a binder.
    check_int("rcb_lowercase_const",
        "const lo: Int = 3\n fn f(x: Int) -> Int { match x { lo..7 => 1, _ => 0 } }\n f(5)", 1);
    // Declaration ORDER must not matter: const types are resolved in pass 1b, but `check_bodies`
    // walks items linearly, so a const declared after the fn has its BODY checked afterwards.
    check_int("rcb_forward_ref",
        "fn f(x: Int) -> Int { match x { LO..HI => 1, _ => 0 } }\n"
        " const LO: Int = 3\n const HI: Int = 7\n f(5)", 1);
    check_int("rcb_at_binding",
        "const LO: Int = 3\n const HI: Int = 7\n match 5 { n @ LO..HI => n, _ => -1 }", 5);
    check_int("rcb_in_or_pattern",
        "const LO: Int = 1\n const HI: Int = 9\n match 25 { LO..HI | 20..30 => 1, _ => 0 }", 1);
    check_int("rcb_nested_in_ctor",
        "enum E { W(Int) }\n const LO: Int = 3\n const HI: Int = 7\n"
        " fn f(e: E) -> Int { match e { W(n @ LO..HI) => n, W(_) => -1 } }\n f(W(5))", 5);
    // Cross-module: a `pub` const of an opt-in std module, reached bare through a glob `use`. This
    // is the one that exercises use_map_ resolution, visible_across, the mangled key, and prelude
    // tree-shaking through cref_pat's Range arm -- i.e. everything the writeback has to survive.
    check_int_p("rcb_std_const_bare",
        "use std::math::*\n fn f(x: Double) -> Int { match x { 0.0..PI => 1, _ => 0 } }\n"
        " f(3.0)*10 + f(4.0)", 10);

    // A QUALIFIED bound, `util::LO`. This is the case that pins the gate's writeback: the resolver
    // CLEARS the qualifier, so if the mangled name were not written back in the same breath, the
    // `infer` that follows would look up a bare `LO` and report it unknown. Checked at module level
    // (the value harnesses are single-source); the end-to-end run is a tier3 guide claim.
    check_true("rcb_qualified_module", modules_check_errs(
        "import util\n fn f(x: Int) -> Int { match x { util::LO..9 => 1, _ => 0 } }\n f(5)",
        std::unordered_map<std::string, std::string>{{"util", "pub const LO: Int = 3"}}).empty());
    // ...and a private const of another module stays private in a bound, like anywhere else.
    check_true("rcb_qualified_private_rejected", !modules_check_errs(
        "import util\n fn f(x: Int) -> Int { match x { util::LO..9 => 1, _ => 0 } }\n f(5)",
        std::unordered_map<std::string, std::string>{{"util", "const LO: Int = 3"}}).empty());

    // ---- rejections, one diagnostic each ------------------------------------------------------
    // A LOCAL wins and is then rejected. Resolving straight to the const would silently test against
    // a value the reader cannot see -- the dangerous class, a wrong result with no diagnostic.
    check_true("rcb_local_shadow_rejected", cg_check_fails(
        "const lo: Int = 3\n fn f(x: Int) -> Int { let lo = 99\n match x { lo..7 => 1, _ => 0 } }\n f(5)"));
    check_true("rcb_local_shadow_message", check_has(
        "const lo: Int = 3\n fn f(x: Int) -> Int { let lo = 99\n match x { lo..7 => 1, _ => 0 } }\n f(5)",
        "is a local binding"));
    // ...and rejecting it must not ALSO warn that the local is unused (the gate marks it used).
    check_true("rcb_local_shadow_no_unused_warn", !check_warn_has(
        "const lo: Int = 3\n fn f(x: Int) -> Int { let lo = 99\n match x { lo..7 => 1, _ => 0 } }\n f(5)",
        "unused variable 'lo'"));
    check_true("rcb_unknown_message",
        check_has("match 5 { 1..zz => 1, _ => 0 }", "unknown constant 'zz'"));
    check_true("rcb_fn_name_rejected",
        check_has("fn g(n: Int) -> Int { n }\n match 5 { 1..g => 1, _ => 0 }", "is not a constant"));
    check_true("rcb_ctor_rejected",
        check_has("enum E { A, B }\n match 5 { A..9 => 1, _ => 0 }", "is not a constant"));
    check_true("rcb_string_const_rejected",
        cg_check_fails("const S: String = \"a\"\n match 5 { 1..S => 1, _ => 0 }"));
    // An ARRAY const must give exactly ONE error. Gating AFTER infer would add the const-array
    // escape essay from const_ref_type on top -- which is why the gate runs first.
    check_true("rcb_array_const_message",
        check_has("const XS: Array[Int] = [1, 2]\n match 5 { 1..XS => 1, _ => 0 }",
                  "is not a numeric constant"));
    check_true("rcb_array_const_one_error",
        check_errc("const XS: Array[Int] = [1, 2]\n match 5 { 1..XS => 1, _ => 0 }") == 1);
    // A bound names a constant, so `mut` there is meaningless rather than misplaced.
    check_true("rcb_mut_bound_rejected", parse_throws("match 5 { mut lo..9 => 1, _ => 0 }"));
    // A qualified bound's tail must be uppercase, matching what the pattern grammar allows after
    // `mod::` -- so both bounds accept exactly the same spellings.
    check_true("rcb_qualified_lower_tail_rejected", parse_throws("match 5 { 1..m::lo => 1, _ => 0 }"));

    // ---- the Maranget con id is built from the VALUE the bound denotes -------------------------
    // `LO..HI` and `1..9` are the same SET, so they must share a con and the second must be caught
    // as a duplicate. Leave bound_literal out of lower_pat and this goes green (the ids differ).
    check_true("rcb_con_dup_vs_literal", cg_check_fails(
        "const LO: Int = 1\n const HI: Int = 9\n match 5 { LO..HI => 1, 1..9 => 2, _ => 0 }"));
    check_true("rcb_con_dup_both_named", cg_check_fails(
        "const LO: Int = 1\n const HI: Int = 9\n match 5 { LO..HI => 1, LO..HI => 2, _ => 0 }"));
    // ...and the other side: distinct values must NOT be reported as a duplicate. Wiring a constant
    // id (or leaving "lit?") makes this red, so the pair pins the behaviour from both directions.
    check_true("rcb_con_distinct_ok", !cg_check_fails(
        "const LO: Int = 1\n const HI: Int = 9\n const HI2: Int = 8\n"
        " match 5 { LO..HI => 1, LO..HI2 => 2, _ => 0 }"));
    check_true("rcb_con_exclusivity_kept", !cg_check_fails(
        "const LO: Int = 1\n const HI: Int = 9\n match 5 { LO..HI => 1, LO..<HI => 2, _ => 0 }"));
    // Two ALREADY-ERRORED ranges must contribute two errors, not three: an unresolvable bound gets a
    // site-unique con, so no bogus "unreachable match arm" stacks on top of the real diagnostics.
    check_true("rcb_two_bad_bounds_two_errors",
        check_errc("match 5 { 1..zz => 1, 1..yy => 2, _ => 0 }") == 2);

    // ---- Char ranges: `'a'..'z'` against a Char scrutinee -------------------------------------
    // The literal pattern has had the byte-int -> Char retyping since Char shipped; the RANGE did
    // not, so the single most idiomatic use of the feature was the one that did not work. These are
    // RUN checks: Char erases to the code-point Int, so the whole claim is that no VM, codegen or
    // oracle change is needed -- only the checker learning to say yes.
    check_int_p("rcbc_lower",   "fn f(c: Char) -> Int { match c { 'a'..'z' => 1, _ => 0 } }\n f('a')", 1);
    check_int_p("rcbc_middle",  "fn f(c: Char) -> Int { match c { 'a'..'z' => 1, _ => 0 } }\n f('m')", 1);
    check_int_p("rcbc_upper",   "fn f(c: Char) -> Int { match c { 'a'..'z' => 1, _ => 0 } }\n f('z')", 1);
    check_int_p("rcbc_outside", "fn f(c: Char) -> Int { match c { 'a'..'z' => 1, _ => 0 } }\n f('A')", 0);
    check_int_p("rcbc_exclusive",
        "fn f(c: Char) -> Int { match c { 'a'..<'z' => 1, _ => 0 } }\n f('z')*10 + f('y')", 1);
    // The rule keys on the VALUE, not the quotes -- the lexer erases the difference.
    check_int_p("rcbc_numeric_spelling",
        "fn f(c: Char) -> Int { match c { 97..122 => 1, _ => 0 } }\n f('m')", 1);
    check_int_p("rcbc_classify",
        "fn c(x: Char) -> Int { match x { '0'..'9' => 1, 'a'..'z' => 2, 'A'..'Z' => 3, _ => 0 } }\n"
        " c('7') + c('m')*10 + c('Q')*100 + c('!')*1000", 321);
    // Exhaustiveness is unchanged: a range never completes a signature, Char or not.
    check_true("rcbc_needs_wildcard",  cg_check_fails_p("fn f(c: Char) -> Int { match c { 'a'..'z' => 1 } }\n f('a')"));
    check_true("rcbc_with_wildcard_ok", !cg_check_fails_p("fn f(c: Char) -> Int { match c { 'a'..'z' => 1, _ => 0 } }\n f('a')"));
    // `'a'..'z'` and `97..122` are the same SET, so they must share a con and be caught as duplicates.
    check_true("rcbc_dup_caught", cg_check_fails_p(
        "fn f(c: Char) -> Int { match c { 'a'..'z' => 1, 97..122 => 2, _ => 0 } }\n f('a')"));
    // Out of byte range, and a NAMED bound, both stay type errors (v1: literal bounds only).
    check_true("rcbc_out_of_range_rejected",
        cg_check_fails_p("fn f(c: Char) -> Int { match c { 256..300 => 1, _ => 0 } }\n f('a')"));
    check_true("rcbc_const_bound_rejected", cg_check_fails_p(
        "const A: Int = 97\n fn f(c: Char) -> Int { match c { A..122 => 1, _ => 0 } }\n f('a')"));
    // One byte literal and one out-of-range bound must NOT slip through the pair check.
    check_true("rcbc_mixed_rejected",
        cg_check_fails_p("fn f(c: Char) -> Int { match c { 'a'..300 => 1, _ => 0 } }\n f('a')"));

    // ---- differentials: the oracle is blind to codegen, so these are the real lock -------------
    check_same("diff_rcbc_classify",
        "fn c(x: Char) -> String { match x { '0'..'9' => \"d\", 'a'..'z' => \"l\", _ => \"o\" } }\n"
        " println(c('7')) println(c('m')) println(c('!'))", true);
    check_same("diff_rcb_basic",
        "const LO: Int = 3\n const HI: Int = 7\n"
        " fn f(x: Int) -> Int { match x { LO..HI => 1, _ => 0 } }\n"
        " println(toString(f(2))) println(toString(f(5))) println(toString(f(9)))", false);
    check_same("diff_rcb_double_excl",
        "const A: Double = 0.0\n const B: Double = 1.0\n"
        " fn f(x: Double) -> Int { match x { A..<B => 1, _ => 0 } }\n"
        " println(toString(f(0.0))) println(toString(f(0.999))) println(toString(f(1.0)))", false);
    check_same("diff_rcb_at",
        "const LO: Int = 0\n const HI: Int = 9\n"
        " fn f(n: Int) -> Int { match n { k @ LO..HI => k * 10, _ => -1 } }\n"
        " println(toString(f(5))) println(toString(f(50)))", false);
}

void test_map_helpers() {
    std::cout << "[codegen: map helpers]\n";

    // getOr -- hit returns the value, miss returns the eager default. Non-mutating.
    check_int_p("getor_hit",  "let m = #{ 1 => 10, 2 => 20 }\n getOr(m, 2, 99)", 20);
    check_int_p("getor_miss", "let m = #{ 1 => 10 }\n getOr(m, 5, 99)", 99);
    check_int_p("getor_empty", "let m: Map[Int, Int] = #{}\n getOr(m, 1, 7)", 7);
    check_int_p("getor_string_key",
        "let m = #{ \"a\" => 1, \"b\" => 2 }\n getOr(m, \"b\", 0) * 10 + getOr(m, \"z\", 9)", 29);

    // getOrElse -- lazy default: the thunk runs only on a miss (a hit returns the stored value).
    check_int_p("getorelse_hit",  "let m = #{ 1 => 10 }\n getOrElse(m, 1, fn() { 99 })", 10);
    check_int_p("getorelse_miss", "let m: Map[Int, Int] = #{}\n getOrElse(m, 1, fn() { 42 })", 42);

    // getOrPut -- inserts dflt on a miss and returns it; on a hit returns the existing value unchanged.
    check_int_p("getorput_insert", "let mut m: Map[Int, Int] = #{}\n getOrPut(m, 1, 7)", 7);
    check_int_p("getorput_visible", "let mut m: Map[Int, Int] = #{}\n getOrPut(m, 1, 7)\n getOr(m, 1, 0)", 7);
    check_int_p("getorput_len",     "let mut m: Map[Int, Int] = #{}\n getOrPut(m, 1, 7)\n getOrPut(m, 2, 8)\n len(m)", 2);
    check_int_p("getorput_hit_keeps", "let mut m = #{ 1 => 5 }\n getOrPut(m, 1, 99)", 5);
    check_int_p("getorput_hit_no_overwrite", "let mut m = #{ 1 => 5 }\n getOrPut(m, 1, 99)\n getOr(m, 1, 0)", 5);

    // upsert -- apply f to the current value on a hit; insert dflt on a miss. Returns the stored value.
    check_int_p("upsert_hit",  "let mut m = #{ 1 => 5 }\n upsert(m, 1, 0, fn(c) { c + 10 })", 15);
    check_int_p("upsert_miss", "let mut m: Map[Int, Int] = #{}\n upsert(m, 1, 100, fn(c) { c + 10 })", 100);
    // Word-count idiom: "a" x2, "b" x1 -> a=2, b=1.
    check_int_p("upsert_wordcount",
        "let mut ws: Vec[String] = vec()\n push(ws, \"a\") push(ws, \"a\") push(ws, \"b\")\n"
        "let mut m: Map[String, Int] = #{}\n for w in ws { upsert(m, w, 1, fn(c) { c + 1 }) }\n"
        "getOr(m, \"a\", 0) * 100 + getOr(m, \"b\", 0)", 201);

    // Differential: VM == RefEval on the scalar results.
    check_same("diff_getor", "let m = #{ 1 => 10, 2 => 20 }\n getOr(m, 2, 0) * 100 + getOr(m, 9, 7)", true);
    check_same("diff_getorelse", "let m: Map[Int, Int] = #{}\n getOrElse(m, 1, fn() { 42 })", true);
    check_same("diff_getorput",
        "let mut m: Map[Int, Int] = #{}\n let a = getOrPut(m, 1, 7)\n let b = getOrPut(m, 1, 99)\n a * 100 + b", true);
    check_same("diff_upsert",
        "let mut m: Map[Int, Int] = #{}\n upsert(m, 1, 100, fn(c) { c + 1 })\n"
        "upsert(m, 1, 100, fn(c) { c + 1 })\n getOr(m, 1, 0)", true);
}

void test_std_set() {
    std::cout << "[codegen: std::set]\n";
    const std::string U = "use std::set::*\n";
    auto p_fails = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return false; }
        catch (const svc::CheckFailure&) { return true; }
        catch (...) { return false; }
    };
    // Gating: `Set` without `use std::set` is out of scope -> a CheckFailure.
    check_true("set_gate_rejects", p_fails("let s: Set[Int] = Set::new()\n s.size()"));
    check_true("set_gate_ok",     !p_fails(U + "let s: Set[Int] = Set::new()\n s.size()"));

    // insert / dedup / size / membership / remove (method syntax, S2).
    check_int_p("set_insert_dedup",
        U + "let mut s: Set[Int] = Set::new()\n s.insert(1)\n s.insert(2)\n s.insert(2)\n s.insert(3)\n s.size()", 3);
    check_bool_p("set_insert_new", U + "let mut s: Set[Int] = Set::new()\n s.insert(5)", true);
    check_bool_p("set_insert_dup", U + "let mut s: Set[Int] = Set::new()\n s.insert(5)\n s.insert(5)", false);
    check_bool_p("set_member_yes", U + "let mut s: Set[Int] = Set::new()\n s.insert(7)\n s.isMember(7)", true);
    check_bool_p("set_member_no",  U + "let mut s: Set[Int] = Set::new()\n s.insert(7)\n s.isMember(8)", false);
    check_bool_p("set_remove_present", U + "let mut s: Set[Int] = Set::new()\n s.insert(3)\n s.remove(3)", true);
    check_bool_p("set_remove_absent",  U + "let mut s: Set[Int] = Set::new()\n s.remove(3)", false);
    check_int_p("set_remove_size",
        U + "let mut s: Set[Int] = Set::new()\n s.insert(1)\n s.insert(2)\n s.remove(1)\n s.size()", 1);
    check_int_p("set_empty_size", U + "let s: Set[Int] = Set::new()\n s.size()", 0);
    check_bool_p("set_member_gone",
        U + "let mut s: Set[Int] = Set::new()\n s.insert(4)\n s.remove(4)\n s.isMember(4)", false);

    // Set::fromVec dedups a Vec.
    check_int_p("set_of_dedup",
        U + "let mut v: Vec[Int] = vec()\n push(v, 1) push(v, 2) push(v, 2) push(v, 3) push(v, 1)\n Set::fromVec(v).size()", 3);

    // Algebra over a = {1,2,3}, b = {2,3,4}.
    const std::string AB =
        U + "let mut av: Vec[Int] = vec()\n push(av, 1) push(av, 2) push(av, 3)\n let a = Set::fromVec(av)\n"
            "let mut bv: Vec[Int] = vec()\n push(bv, 2) push(bv, 3) push(bv, 4)\n let b = Set::fromVec(bv)\n";
    check_int_p("set_union_size",      AB + "a.union(b).size()", 4);
    check_int_p("set_intersect_size",  AB + "a.intersect(b).size()", 2);
    check_int_p("set_difference_size", AB + "a.difference(b).size()", 1);
    check_bool_p("set_union_member",   AB + "a.union(b).isMember(4)", true);
    check_bool_p("set_intersect_member", AB + "a.intersect(b).isMember(1)", false);
    check_bool_p("set_subset_true",    AB + "a.intersect(b).isSubset(a)", true);
    check_bool_p("set_subset_false",   AB + "a.isSubset(b)", false);
    check_bool_p("set_disjoint_true",  AB + "a.difference(b).isDisjoint(b)", true);
    check_bool_p("set_disjoint_false", AB + "a.isDisjoint(b)", false);

    // `for x in s` (IntoIterator) -- sum is order-independent. VM-only (RefEval for-over-user-struct not relied on).
    check_int_p("set_for_sum",
        U + "let mut s: Set[Int] = Set::new()\n s.insert(10) s.insert(20) s.insert(30) s.insert(20)\n"
            "let mut acc = 0\n for x in s { acc = acc + x }\n acc", 60);

    // String elements.
    check_int_p("set_string_size",
        U + "let mut s: Set[String] = Set::new()\n s.insert(\"hi\") s.insert(\"hi\") s.insert(\"yo\")\n s.size()", 2);
    check_bool_p("set_string_member",
        U + "let mut s: Set[String] = Set::new()\n s.insert(\"hi\")\n s.isMember(\"hi\")", true);

    // Differentials -- VM == RefEval on ORDER-INDEPENDENT results (size / membership / union-size).
    check_same("diff_set_size",
        U + "let mut s: Set[Int] = Set::new()\n s.insert(1)\n s.insert(2)\n s.insert(2)\n s.insert(5)\n s.remove(1)\n s.size()", true);
    check_same("diff_set_of",   U + "let mut v: Vec[Int] = vec()\n push(v, 3) push(v, 3) push(v, 7) push(v, 1)\n Set::fromVec(v).size()", true);
    check_same("diff_set_union", AB + "a.union(b).size() * 100 + a.intersect(b).size()", true);
    check_same("diff_set_member", AB + "if a.isMember(2) { if a.isMember(9) { 2 } else { 1 } } else { 0 }", true);
}

// =============================================================================
// std::time -- the opt-in date/time library, pure Skarn (Hinnant civil<->days over epoch-ms). The
// only impure surface is now()/Stopwatch (wall-clock/monotonic natives), tested VM-only. All KAT
// anchors are produced by WinPython's datetime (UTC). The pure calendar math is fully deterministic,
// so a handful of check_same differentials pin VM == RefEval.
// =============================================================================
void test_std_time() {
    std::cout << "[codegen: std::time]\n";
    const std::string U = "use std::time::*\n";
    auto p_fails = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return false; }
        catch (const svc::CheckFailure&) { return true; }
        catch (...) { return false; }
    };
    // Gating: bare instantFromMillis without `use std::time` is out of scope -> a CheckFailure.
    check_true("time_gate_rejects", p_fails("instantFromMillis(0).toEpochMillis()"));
    check_true("time_gate_ok",      !p_fails(U + "instantFromMillis(0).toEpochMillis()"));

    // Decomposition anchors (WinPython): epoch 0 = 1970-01-01; 951825600000 = 2000-02-29 12:00:00.
    check_int_p("time_epoch0_date",
        U + "let d = instantFromMillis(0).toDateTime()\n d.year * 10000 + d.month * 100 + d.day", 19700101);
    check_int_p("time_leapday_date",
        U + "let d = instantFromMillis(951825600000).toDateTime()\n d.year * 10000 + d.month * 100 + d.day", 20000229);
    check_int_p("time_leapday_tod",
        U + "let d = instantFromMillis(951825600000).toDateTime()\n d.hour * 10000 + d.minute * 100 + d.second", 120000);
    // 2026-07-21 14:30:15.123 = 1784644215123.
    check_int_p("time_frac_milli",
        U + "let d = instantFromMillis(1784644215123).toDateTime()\n d.milli", 123);
    // Negative epoch: -1 ms = 1969-12-31 23:59:59.999 (floor semantics).
    check_int_p("time_neg_date",
        U + "let d = instantFromMillis(0 - 1).toDateTime()\n d.year * 10000 + d.month * 100 + d.day", 19691231);
    check_int_p("time_neg_tod",   // ms-of-day for -1 ms = 86399999 (23:59:59.999)
        U + "let d = instantFromMillis(0 - 1).toDateTime()\n ((d.hour * 60 + d.minute) * 60 + d.second) * 1000 + d.milli", 86399999);

    // Instant round-trip is identity across a WIDE range incl. deep negatives and near the +/-2^47 edge.
    check_int_p("time_roundtrip_sweep",
        U + "let mut ms = 0 - 130000000000000\n let mut ok = 1\n"
            "while ms < 130000000000000 {\n"
            "  let dt = instantFromMillis(ms).toDateTime()\n"
            "  if fromDateTime(dt).toEpochMillis() != ms { ok = 0 }\n"
            "  ms = ms + 777000000007\n"
            "}\n ok", 1);

    // weekday (ISO Mon=1..Sun=7), WinPython anchors.
    check_int_p("time_wd_epoch", U + "instantFromMillis(0).toDateTime().weekday()", 4);             // 1970-01-01 Thu
    check_int_p("time_wd_2026",  U + "instantFromMillis(1784644215123).toDateTime().weekday()", 2); // 2026-07-21 Tue
    check_int_p("time_wd_leap",
        U + "match dateOnly(2024, 2, 29) { Some(d) => d.weekday(), None => 0 - 1 }", 4);            // Thu

    // isLeapYear / daysInMonth.
    check_bool_p("time_leap_2000", U + "isLeapYear(2000)", true);
    check_bool_p("time_leap_1900", U + "isLeapYear(1900)", false);
    check_bool_p("time_leap_2024", U + "isLeapYear(2024)", true);
    check_bool_p("time_leap_2023", U + "isLeapYear(2023)", false);
    check_bool_p("time_leap_1600", U + "isLeapYear(1600)", true);
    check_bool_p("time_leap_1700", U + "isLeapYear(1700)", false);
    check_int_p("time_dim_feb_leap",   U + "daysInMonth(2000, 2)", 29);
    check_int_p("time_dim_feb_common", U + "daysInMonth(2001, 2)", 28);
    check_int_p("time_dim_apr",        U + "daysInMonth(2026, 4)", 30);
    check_int_p("time_dim_dec",        U + "daysInMonth(2026, 12)", 31);

    // Strict validation -> None on out-of-range fields; Some on the boundary.
    check_int_p("time_valid_ok",     U + "match dateTime(2026, 7, 21, 14, 30, 15, 123) { Some(_) => 1, None => 0 }", 1);
    check_int_p("time_valid_feb29",  U + "match dateTime(2000, 2, 29, 0, 0, 0, 0) { Some(_) => 1, None => 0 }", 1);
    check_int_p("time_bad_feb29",    U + "match dateTime(2001, 2, 29, 0, 0, 0, 0) { Some(_) => 1, None => 0 }", 0);
    check_int_p("time_bad_month",    U + "match dateTime(2026, 13, 1, 0, 0, 0, 0) { Some(_) => 1, None => 0 }", 0);
    check_int_p("time_bad_day0",     U + "match dateTime(2026, 1, 0, 0, 0, 0, 0) { Some(_) => 1, None => 0 }", 0);
    check_int_p("time_bad_hour24",   U + "match dateTime(2026, 1, 1, 24, 0, 0, 0) { Some(_) => 1, None => 0 }", 0);
    check_int_p("time_bad_min60",    U + "match dateTime(2026, 1, 1, 0, 60, 0, 0) { Some(_) => 1, None => 0 }", 0);
    check_int_p("time_bad_milli1000",U + "match dateTime(2026, 1, 1, 0, 0, 0, 1000) { Some(_) => 1, None => 0 }", 0);

    // Duration constructors / accessors / algebra.
    check_int_p("time_dur_hours_min", U + "hours(2).add(minutes(30)).inMinutes()", 150);
    check_int_p("time_dur_days_ms",   U + "days(1).inMillis()", 86400000);
    check_int_p("time_dur_scale",     U + "seconds(5).scale(3).inSeconds()", 15);
    check_int_p("time_dur_negate",    U + "millis(7).negate().inMillis()", -7);
    check_int_p("time_dur_between",   U + "instantFromMillis(0).durationBetween(instantFromMillis(600000)).inMinutes()", 10);

    // Instant add/sub + ordering.
    check_int_p("time_add_dur", U + "instantFromMillis(1000).addDuration(seconds(5)).toEpochMillis()", 6000);
    check_int_p("time_sub_dur", U + "instantFromMillis(6000).subDuration(seconds(5)).toEpochMillis()", 1000);
    check_bool_p("time_before", U + "instantFromMillis(1).isBefore(instantFromMillis(2))", true);
    check_bool_p("time_after",  U + "instantFromMillis(2).isAfter(instantFromMillis(1))", true);

    // ISO formatting (VM-only string checks via the captured print sink).
    check_str("time_iso_epoch", cg_run_out(U + "println(instantFromMillis(0).toDateTime().toIso())"),
              "1970-01-01T00:00:00.000Z\n");
    check_str("time_iso_frac", cg_run_out(U + "println(instantFromMillis(1784644215123).toDateTime().toIso())"),
              "2026-07-21T14:30:15.123Z\n");
    check_str("time_iso_neg", cg_run_out(U + "println(instantFromMillis(0 - 1).toDateTime().toIso())"),
              "1969-12-31T23:59:59.999Z\n");
    check_str("time_iso_leapday", cg_run_out(U + "println(instantFromMillis(951825600000).toDateTime().toIso())"),
              "2000-02-29T12:00:00.000Z\n");

    // ISO parsing: round-trip + error cases.
    check_int_p("time_parse_roundtrip",
        U + "match parseIso(instantFromMillis(1784644215123).toDateTime().toIso()) {\n"
            "  Ok(dt) => fromDateTime(dt).toEpochMillis(), Err(_) => 0 - 1 }", 1784644215123);
    check_int_p("time_parse_no_frac",
        U + "match parseIso(\"1970-01-01T00:00:00Z\") { Ok(dt) => fromDateTime(dt).toEpochMillis(), Err(_) => 0 - 1 }", 0);
    check_int_p("time_parse_err_short",
        U + "match parseIso(\"garbage\") { Ok(_) => 1, Err(_) => 0 }", 0);
    check_int_p("time_parse_err_sep",
        U + "match parseIso(\"2026/07/21T00:00:00Z\") { Ok(_) => 1, Err(_) => 0 }", 0);
    check_int_p("time_parse_err_nan",
        U + "match parseIso(\"20x6-07-21T00:00:00Z\") { Ok(_) => 1, Err(_) => 0 }", 0);
    check_int_p("time_parse_err_range",
        U + "match parseIso(\"2026-13-01T00:00:00Z\") { Ok(_) => 1, Err(_) => 0 }", 0);

    // now / Stopwatch -- IMPURE (natives), VM-only smoke via the real native table.
    check_str("time_now_smoke",
        cg_run_native(U + "let t = now()\n if t.toEpochMillis() > 0 { println(\"ok\") } else { println(\"bad\") }"), "ok\n");
    check_str("time_stopwatch_smoke",
        cg_run_native(U + "let sw = startStopwatch()\n let e = sw.elapsedNanos()\n if e >= 0 { println(\"ok\") } else { println(\"bad\") }"), "ok\n");

    // Differentials -- VM == RefEval on the pure, deterministic calendar/duration math.
    check_same("diff_time_roundtrip", U + "fromDateTime(instantFromMillis(1784644215123).toDateTime()).toEpochMillis()", true);
    check_same("diff_time_neg_rt",    U + "fromDateTime(instantFromMillis(0 - 11670998400000).toDateTime()).toEpochMillis()", true);
    check_same("diff_time_weekday",   U + "instantFromMillis(1784644215123).toDateTime().weekday()", true);
    check_same("diff_time_leap",      U + "if isLeapYear(2000) { if isLeapYear(1900) { 2 } else { 1 } } else { 0 }", true);
    check_same("diff_time_dim",       U + "daysInMonth(2024, 2) * 100 + daysInMonth(2023, 2)", true);
    check_same("diff_time_dur",       U + "hours(2).add(minutes(30)).inMinutes()", true);
    check_same("diff_time_validate",  U + "match dateTime(2000, 2, 29, 0, 0, 0, 0) { Some(d) => d.weekday(), None => 0 - 1 }", true);
}

// =============================================================================
// std::random -- the opt-in xoshiro128** PRNG, pure prelude. Mirrors test_std_bytes:
// a `use std::random::*` prefix + a gate pair. The KAT anchors are produced by an external
// reference implementation of xoshiro128** + our SplitMix32 seeding (see the memory record);
// determinism makes the whole module fully differential-testable (VM == RefEval by construction).
// =============================================================================
void test_std_random() {
    std::cout << "[codegen: std::random]\n";
    const std::string U = "use std::random::*\n";
    auto p_fails = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return false; }
        catch (const svc::CheckFailure&) { return true; }
        catch (...) { return false; }
    };
    // Gating: `Rng` without `use std::random` is out of scope -> a CheckFailure. The constructor is
    // now an associated fn, so the gate is on the TYPE name rather than a bare fn name.
    check_true("random_gate_rejects", p_fails("let mut r = Rng::fromSeed(1)\n r.nextU32()"));
    check_true("random_gate_ok",     !p_fails(U + "let mut r = Rng::fromSeed(1)\n r.nextU32()"));

    // KAT -- xoshiro128** core, from the raw state {1,2,3,4} (external anchor; nth value = n-1 discards
    // then a returning draw). Reference: [11520, 0, 5927040, 70819200, 2031721883, ..., 3734860849].
    check_int_p("random_kat_core1", U + "let mut r = Rng::fromState(1, 2, 3, 4)\n r.nextU32()", 11520);
    check_int_p("random_kat_core2", U + "let mut r = Rng::fromState(1, 2, 3, 4)\n r.nextU32() r.nextU32()", 0);
    check_int_p("random_kat_core5",
        U + "let mut r = Rng::fromState(1, 2, 3, 4)\n r.nextU32() r.nextU32() r.nextU32() r.nextU32() r.nextU32()", 2031721883);
    check_int_p("random_kat_core8",
        U + "let mut r = Rng::fromState(1, 2, 3, 4)\n r.nextU32() r.nextU32() r.nextU32() r.nextU32() r.nextU32() r.nextU32() r.nextU32() r.nextU32()", 3734860849);
    // KAT -- SplitMix32 seeding (Rng::fromSeed(42) -> [660444221, 3652823732, 77672526, ...]).
    check_int_p("random_kat_seed1", U + "let mut r = Rng::fromSeed(42)\n r.nextU32()", 660444221);
    check_int_p("random_kat_seed3", U + "let mut r = Rng::fromSeed(42)\n r.nextU32() r.nextU32() r.nextU32()", 77672526);
    // KAT -- jump (2^64 advance) from {1,2,3,4}; first post-jump draw = 1194304935.
    check_int_p("random_kat_jump", U + "let mut r = Rng::fromState(1, 2, 3, 4)\n r.jump()\n r.nextU32()", 1194304935);

    // Determinism: same seed -> same stream.
    check_int_p("random_determinism",
        U + "let mut a = Rng::fromSeed(123)\n let mut b = Rng::fromSeed(123)\n if a.nextU32() == b.nextU32() { 1 } else { 0 }", 1);
    // splitRng yields a DIFFERENT stream than its parent (post-split).
    check_int_p("random_split_differs",
        U + "let mut a = Rng::fromSeed(9)\n let b = a.splitRng()\n let mut c = b\n if a.nextU32() == c.nextU32() { 0 } else { 1 }", 1);
    // nextInt48 (full signed range) is deterministic per seed.
    check_int_p("random_int48_det",
        U + "let mut a = Rng::fromSeed(1)\n let mut b = Rng::fromSeed(1)\n if a.nextInt48() == b.nextInt48() { 1 } else { 0 }", 1);

    // nextIntBounded stays in [0, bound) over many draws.
    check_int_p("random_bounded_range",
        U + "let mut r = Rng::fromSeed(5)\n let mut ok = 1\n let mut i = 0\n"
            "while i < 300 { let x = r.nextIntBounded(7)\n if x < 0 { ok = 0 }\n if x >= 7 { ok = 0 }\n i = i + 1 }\n ok", 1);
    // Larger-than-32-bit bound (exercises the two-draw stitch path in _drawMasked).
    check_int_p("random_bounded_big",
        U + "let mut r = Rng::fromSeed(5)\n let mut ok = 1\n let mut i = 0\n"
            "while i < 200 { let x = r.nextIntBounded(100000000000)\n if x < 0 { ok = 0 }\n if x >= 100000000000 { ok = 0 }\n i = i + 1 }\n ok", 1);
    // nextInt([lo, hi)) stays in range.
    check_int_p("random_int_range",
        U + "let mut r = Rng::fromSeed(6)\n let mut ok = 1\n let mut i = 0\n"
            "while i < 300 { let x = r.nextInt(10, 20)\n if x < 10 { ok = 0 }\n if x >= 20 { ok = 0 }\n i = i + 1 }\n ok", 1);

    // nextBool produces BOTH values over 100 draws.
    check_int_p("random_bool_mixed",
        U + "let mut r = Rng::fromSeed(3)\n let mut t = 0\n let mut i = 0\n"
            "while i < 100 { if r.nextBool() { t = t + 1 }\n i = i + 1 }\n if t > 0 { if t < 100 { 1 } else { 0 } } else { 0 }", 1);

    // nextDouble stays in [0, 1) over many draws.
    check_int_p("random_double_range",
        U + "let mut r = Rng::fromSeed(8)\n let mut ok = 1\n let mut i = 0\n"
            "while i < 300 { let d = r.nextDouble()\n if d < 0.0 { ok = 0 }\n if d >= 1.0 { ok = 0 }\n i = i + 1 }\n ok", 1);

    // shuffle is a permutation (sum + length preserved).
    check_int_p("random_shuffle_perm",
        U + "let mut r = Rng::fromSeed(2)\n let mut v: Vec[Int] = vec()\n"
            "push(v, 1) push(v, 2) push(v, 3) push(v, 4) push(v, 5)\n r.shuffle(v)\n"
            "let mut s = 0\n let mut i = 0\n while i < len(v) { s = s + v[i]\n i = i + 1 }\n s * 10 + len(v)", 155);
    // choice: a member of the vector; None on empty.
    check_int_p("random_choice_member",
        U + "let mut r = Rng::fromSeed(4)\n let mut v: Vec[Int] = vec()\n push(v, 7) push(v, 7) push(v, 7)\n"
            "match r.choice(v) { Some(x) => x, None => -1 }", 7);
    check_int_p("random_choice_empty",
        U + "let mut r = Rng::fromSeed(4)\n let e: Vec[Int] = vec()\n match r.choice(e) { Some(_) => 1, None => 0 }", 0);

    // Bulk helpers equal the per-call path on the same seed (the hoist is behavior-preserving).
    check_int_p("random_fillU32_equiv",
        U + "let mut a = Rng::fromSeed(5)\n let mut b = Rng::fromSeed(5)\n let mut out: Vec[Int] = vec()\n a.fillU32(out, 3)\n"
            "if out[0] == b.nextU32() { if out[1] == b.nextU32() { if out[2] == b.nextU32() { 1 } else { 0 } } else { 0 } } else { 0 }", 1);
    check_int_p("random_fillDoubles_equiv",
        U + "let mut a = Rng::fromSeed(5)\n let mut b = Rng::fromSeed(5)\n let mut out: Vec[Double] = vec()\n a.fillDoubles(out, 3)\n"
            "if out[0] == b.nextDouble() { if out[1] == b.nextDouble() { if out[2] == b.nextDouble() { 1 } else { 0 } } else { 0 } } else { 0 }", 1);

    // Contract violations abort at runtime (located panic).
    check_true("random_bound_zero_panics",  cg_faults(U + "let mut r = Rng::fromSeed(1)\n r.nextIntBounded(0)"));
    check_true("random_bound_neg_panics",   cg_faults(U + "let mut r = Rng::fromSeed(1)\n r.nextIntBounded(0 - 3)"));
    check_true("random_int_empty_panics",   cg_faults(U + "let mut r = Rng::fromSeed(1)\n r.nextInt(5, 5)"));
    check_true("random_int_inverted_panics",cg_faults(U + "let mut r = Rng::fromSeed(1)\n r.nextInt(5, 3)"));

    // Differential (VM vs RefEval oracle): pure Skarn, so both run the identical module.
    check_same("diff_random_seq",
        U + "let mut r = Rng::fromSeed(7)\n r.nextU32() + r.nextU32() + r.nextU32()", true);
    check_same("diff_random_bounded",
        U + "let mut r = Rng::fromSeed(11)\n let mut s = 0\n let mut i = 0\n"
            "while i < 20 { s = s + r.nextIntBounded(1000)\n i = i + 1 }\n s", true);
    check_same("diff_random_shuffle",
        U + "let mut r = Rng::fromSeed(13)\n let mut v: Vec[Int] = vec()\n"
            "push(v, 1) push(v, 2) push(v, 3) push(v, 4) push(v, 5) push(v, 6)\n r.shuffle(v)\n"
            "let mut s = 0\n let mut i = 0\n while i < len(v) { s = s + v[i] * (i + 1)\n i = i + 1 }\n s", true);

    // ---- v1.1: nextGaussian (couples std::math) / choiceWeighted / sample ----
    // nextGaussian: deterministic per seed; both signs occur; the scaled variant composes.
    check_int_p("random_gaussian_det",
        U + "let mut a = Rng::fromSeed(5)\n let mut b = Rng::fromSeed(5)\n if a.nextGaussian() == b.nextGaussian() { 1 } else { 0 }", 1);
    check_int_p("random_gaussian_signs",
        U + "let mut r = Rng::fromSeed(9)\n let mut pos = 0\n let mut neg = 0\n let mut i = 0\n"
            "while i < 100 { let g = r.nextGaussian()\n if g > 0.0 { pos = pos + 1 }\n if g < 0.0 { neg = neg + 1 }\n i = i + 1 }\n"
            "if pos > 0 { if neg > 0 { 1 } else { 0 } } else { 0 }", 1);
    check_int_p("random_gaussianMS_det",
        U + "let mut a = Rng::fromSeed(5)\n let mut b = Rng::fromSeed(5)\n"
            "if a.nextGaussianMS(10.0, 2.0) == 10.0 + 2.0 * b.nextGaussian() { 1 } else { 0 }", 1);
    check_same("diff_random_gaussian",
        U + "let mut r = Rng::fromSeed(21)\n let mut acc = 0\n let mut i = 0\n"
            "while i < 30 { acc = acc + toInt(r.nextGaussian() * 1000000.0)\n i = i + 1 }\n acc", true);

    // choiceWeighted: a single positive weight forces that element (deterministic); the edge cases -> None.
    check_int_p("random_weighted_forced",
        U + "let mut r = Rng::fromSeed(1)\n let mut items: Vec[Int] = vec()\n push(items,10) push(items,20) push(items,30)\n"
            "let mut w: Vec[Double] = vec()\n push(w,0.0) push(w,0.0) push(w,1.0)\n"
            "match r.choiceWeighted(items, w) { Some(x) => x, None => -1 }", 30);
    check_int_p("random_weighted_empty",
        U + "let mut r = Rng::fromSeed(1)\n let e: Vec[Int] = vec()\n let w: Vec[Double] = vec()\n"
            "match r.choiceWeighted(e, w) { Some(_) => 1, None => 0 }", 0);
    check_int_p("random_weighted_mismatch",
        U + "let mut r = Rng::fromSeed(1)\n let mut items: Vec[Int] = vec()\n push(items,1) push(items,2)\n"
            "let mut w: Vec[Double] = vec()\n push(w,1.0)\n"
            "match r.choiceWeighted(items, w) { Some(_) => 1, None => 0 }", 0);
    check_int_p("random_weighted_allzero",
        U + "let mut r = Rng::fromSeed(1)\n let mut items: Vec[Int] = vec()\n push(items,1) push(items,2)\n"
            "let mut w: Vec[Double] = vec()\n push(w,0.0) push(w,0.0)\n"
            "match r.choiceWeighted(items, w) { Some(_) => 1, None => 0 }", 0);
    check_int_p("random_weighted_member",   // over many draws, always a member of items
        U + "let mut r = Rng::fromSeed(2)\n let mut items: Vec[Int] = vec()\n push(items,7) push(items,8) push(items,9)\n"
            "let mut w: Vec[Double] = vec()\n push(w,1.0) push(w,2.0) push(w,3.0)\n"
            "let mut ok = 1\n let mut i = 0\n while i < 200 {\n"
            "  match r.choiceWeighted(items, w) { Some(x) => { if x < 7 { ok = 0 }\n if x > 9 { ok = 0 } }, None => { ok = 0 } }\n i = i + 1 }\n ok", 1);

    // sample: size clamps to [0, len]; distinct; leaves the source vector intact (shallow copy).
    check_int_p("random_sample_size",
        U + "let mut r = Rng::fromSeed(2)\n let mut v: Vec[Int] = vec()\n push(v,1) push(v,2) push(v,3) push(v,4) push(v,5)\n len(r.sample(v, 3))", 3);
    check_int_p("random_sample_clamp",
        U + "let mut r = Rng::fromSeed(2)\n let mut v: Vec[Int] = vec()\n push(v,1) push(v,2) push(v,3)\n len(r.sample(v, 10))", 3);
    check_int_p("random_sample_zero",
        U + "let mut r = Rng::fromSeed(2)\n let mut v: Vec[Int] = vec()\n push(v,1) push(v,2)\n len(r.sample(v, 0))", 0);
    check_int_p("random_sample_distinct",
        U + "let mut r = Rng::fromSeed(3)\n let mut v: Vec[Int] = vec()\n push(v,1) push(v,2) push(v,3) push(v,4) push(v,5)\n"
            "let s = r.sample(v, 3)\n if s[0] != s[1] { if s[0] != s[2] { if s[1] != s[2] { 1 } else { 0 } } else { 0 } } else { 0 }", 1);
    check_int_p("random_sample_source_intact",
        U + "let mut r = Rng::fromSeed(2)\n let mut v: Vec[Int] = vec()\n push(v,1) push(v,2) push(v,3) push(v,4)\n let s = r.sample(v, 2)\n"
            "let mut sum = 0\n let mut i = 0\n while i < len(v) { sum = sum + v[i]\n i = i + 1 }\n sum", 10);
    check_same("diff_random_sample",
        U + "let mut r = Rng::fromSeed(17)\n let mut v: Vec[Int] = vec()\n push(v,10) push(v,20) push(v,30) push(v,40) push(v,50)\n"
            "let s = r.sample(v, 3)\n let mut sum = 0\n let mut i = 0\n while i < len(s) { sum = sum + s[i]\n i = i + 1 }\n sum", true);
}

// =============================================================================
// std::cli -- the opt-in spec-free CLI argument parser over Vec[String]. Pure Skarn, deterministic
// (the argv vector is a parameter -- the module pulls no environment). Gate pair + grammar coverage.
// =============================================================================
void test_std_cli() {
    std::cout << "[codegen: std::cli]\n";
    const std::string U = "use std::cli::*\n";
    auto p_fails = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return false; }
        catch (const svc::CheckFailure&) { return true; }
        catch (...) { return false; }
    };
    // Gating: `CliArgs` without `use std::cli` is out of scope.
    check_true("cli_gate_rejects", p_fails("let a: Vec[String] = vec()\n let c = CliArgs::parse(a)\n 0"));
    check_true("cli_gate_ok",      !p_fails(U + "let mut a: Vec[String] = vec()\n let c = CliArgs::parse(a)\n 0"));

    // long boolean flags
    check_int_p("cli_flag_long",
        U + "let mut a: Vec[String] = vec()\n push(a, \"--verbose\")\n let c = CliArgs::parse(a)\n if c.hasFlag(\"verbose\") { 1 } else { 0 }", 1);
    check_int_p("cli_flag_absent",
        U + "let mut a: Vec[String] = vec()\n let c = CliArgs::parse(a)\n if c.hasFlag(\"verbose\") { 1 } else { 0 }", 0);
    // grouped short flags (letters only)
    check_int_p("cli_flag_short",
        U + "let mut a: Vec[String] = vec()\n push(a, \"-abc\")\n let c = CliArgs::parse(a)\n"
            "if c.hasFlag(\"a\") { if c.hasFlag(\"b\") { if c.hasFlag(\"c\") { 1 } else { 0 } } else { 0 } } else { 0 }", 1);
    // options `--name=value`
    check_str("cli_opt",
        cg_run_out(U + "let mut a: Vec[String] = vec()\n push(a, \"--out=file.txt\")\n let c = CliArgs::parse(a)\n println(c.getOptOr(\"out\", \"none\"))"), "file.txt\n");
    check_str("cli_opt_default",
        cg_run_out(U + "let mut a: Vec[String] = vec()\n let c = CliArgs::parse(a)\n println(c.getOptOr(\"out\", \"none\"))"), "none\n");
    check_int_p("cli_opt_some",
        U + "let mut a: Vec[String] = vec()\n push(a, \"--n=5\")\n let c = CliArgs::parse(a)\n match c.getOpt(\"n\") { Some(_) => 1, None => 0 }", 1);
    check_int_p("cli_opt_none",
        U + "let mut a: Vec[String] = vec()\n let c = CliArgs::parse(a)\n match c.getOpt(\"n\") { Some(_) => 1, None => 0 }", 0);
    // positionals + `--` terminator + dash-number
    check_int_p("cli_positionals",
        U + "let mut a: Vec[String] = vec()\n push(a,\"foo\") push(a,\"--v\") push(a,\"bar\")\n let c = CliArgs::parse(a)\n c.numPositionals()", 2);
    check_str("cli_positional_at",
        cg_run_out(U + "let mut a: Vec[String] = vec()\n push(a,\"foo\") push(a,\"bar\")\n let c = CliArgs::parse(a)\n"
                       " match c.positionalAt(1) { Some(s) => println(s), None => println(\"-\") }"), "bar\n");
    check_int_p("cli_positional_oob",
        U + "let mut a: Vec[String] = vec()\n push(a,\"foo\")\n let c = CliArgs::parse(a)\n match c.positionalAt(5) { Some(_) => 1, None => 0 }", 0);
    check_int_p("cli_terminator",   // after `--` everything (incl. --flag / -x) is positional
        U + "let mut a: Vec[String] = vec()\n push(a,\"--\") push(a,\"--notaflag\") push(a,\"-x\")\n let c = CliArgs::parse(a)\n c.numPositionals()", 2);
    check_int_p("cli_terminator_noflag",
        U + "let mut a: Vec[String] = vec()\n push(a,\"--\") push(a,\"--notaflag\")\n let c = CliArgs::parse(a)\n if c.hasFlag(\"notaflag\") { 1 } else { 0 }", 0);
    check_int_p("cli_dashnum",      // `-5` is a positional, not a short flag
        U + "let mut a: Vec[String] = vec()\n push(a,\"-5\")\n let c = CliArgs::parse(a)\n c.numPositionals()", 1);
    // a realistic mixed line
    check_int_p("cli_mixed",
        U + "let mut a: Vec[String] = vec()\n push(a,\"build\") push(a,\"--opt=O2\") push(a,\"-v\") push(a,\"main.skn\")\n let c = CliArgs::parse(a)\n"
            "c.numPositionals() * 100 + (if c.hasFlag(\"v\") { 10 } else { 0 }) + (if c.hasFlag(\"opt\") { 0 } else { 1 })", 211);

    // differential (pure Skarn -> VM == oracle by construction)
    check_same("diff_cli",
        U + "let mut a: Vec[String] = vec()\n push(a,\"--x=1\") push(a,\"-ab\") push(a,\"pos\") push(a,\"--\") push(a,\"--lit\")\n let c = CliArgs::parse(a)\n"
            "c.numPositionals() * 10 + (if c.hasFlag(\"a\") { 1 } else { 0 })", true);
}

// =============================================================================
// std::hash -- opt-in non-crypto hashing. v1: CRC-32 (IEEE/zlib). Pure Skarn, deterministic; KATs are
// the canonical CRC-32 check values.
// =============================================================================
void test_std_hash() {
    std::cout << "[codegen: std::hash]\n";
    const std::string U = "use std::hash::*\n";
    auto p_fails = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return false; }
        catch (const svc::CheckFailure&) { return true; }
        catch (...) { return false; }
    };
    // Gating
    check_true("hash_gate_rejects", p_fails("crc32Str(\"x\")"));
    check_true("hash_gate_ok",      !p_fails(U + "crc32Str(\"x\")"));
    // KAT: canonical CRC-32 check values
    check_int_p("hash_crc_empty", U + "crc32Str(\"\")", 0);
    check_int_p("hash_crc_check", U + "crc32Str(\"123456789\")", 3421780262);   // 0xCBF43926
    check_int_p("hash_crc_fox",   U + "crc32Str(\"The quick brown fox jumps over the lazy dog\")", 1095738169);  // 0x414FA339
    // determinism + Bytes/String equivalence + non-negativity + sensitivity
    check_int_p("hash_crc_det",    U + "if crc32Str(\"hello\") == crc32Str(\"hello\") { 1 } else { 0 }", 1);
    check_int_p("hash_crc_bytes",  U + "if crc32(toBytes(\"world\")) == crc32Str(\"world\") { 1 } else { 0 }", 1);
    check_int_p("hash_crc_nonneg", U + "if crc32Str(\"a\") >= 0 { 1 } else { 0 }", 1);
    check_int_p("hash_crc_differs",U + "if crc32Str(\"a\") == crc32Str(\"b\") { 0 } else { 1 }", 1);
    // differential
    check_same("diff_hash_crc", U + "crc32Str(\"The quick brown fox\")", true);

    // MurmurHash3 x86_32 (pure Skarn, deterministic). KATs anchored against an independent reference
    // implementation of the spec; every tail length 0..3 is exercised. murmur3(empty, 0) == 0.
    check_int_p("mmh3_empty",     U + "murmur3Str(\"\", 0)", 0);
    check_int_p("mmh3_empty_s1",  U + "murmur3Str(\"\", 1)", 1364076727);         // 0x514E28B7
    check_int_p("mmh3_tail1",     U + "murmur3Str(\"a\", 0)", 1009084850);        // len%4==1
    check_int_p("mmh3_tail2",     U + "murmur3Str(\"ab\", 0)", 2613040991);       // len%4==2
    check_int_p("mmh3_tail3",     U + "murmur3Str(\"abc\", 0)", 3017643002);      // len%4==3
    check_int_p("mmh3_block",     U + "murmur3Str(\"abcd\", 0)", 1139631978);     // len%4==0 (one full block)
    check_int_p("mmh3_hello",     U + "murmur3Str(\"Hello, world!\", 0)", 3224780355);  // 0xC0363E43
    check_int_p("mmh3_fox",       U + "murmur3Str(\"The quick brown fox jumps over the lazy dog\", 0)", 776992547);
    check_int_p("mmh3_digits",    U + "murmur3Str(\"123456789\", 0)", 3036607362);
    check_int_p("mmh3_seed",      U + "murmur3Str(\"abc\", 123)", 461137560);     // seed changes the hash
    // determinism + Bytes/String equivalence + non-negativity + seed sensitivity
    check_int_p("mmh3_bytes",     U + "if murmur3(toBytes(\"world\"), 7) == murmur3Str(\"world\", 7) { 1 } else { 0 }", 1);
    check_int_p("mmh3_nonneg",    U + "if murmur3Str(\"a\", 0) >= 0 { 1 } else { 0 }", 1);
    check_int_p("mmh3_seed_diff", U + "if murmur3Str(\"x\", 0) == murmur3Str(\"x\", 1) { 0 } else { 1 }", 1);
    // differential (VM == RefEval): a value tail-length not a multiple of 4 exercises _mul32/_rotl32/tail.
    check_same("diff_mmh3", U + "murmur3Str(\"The quick brown fox\", 0)", true);

    // SHA-256 (native). Verified by the canonical FIPS 180-4 KAT vectors (not in the differential -- a
    // native, KAT-anchored). Uses cg_run_native (the real native table) since sha256 is a C++ native.
    check_str("sha_empty", cg_run_native(U + "println(sha256HexStr(\"\"))"),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n");
    check_str("sha_abc",   cg_run_native(U + "println(sha256HexStr(\"abc\"))"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n");
    check_str("sha_fox",   cg_run_native(U + "println(sha256HexStr(\"The quick brown fox jumps over the lazy dog\"))"),
              "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592\n");
    // determinism + the raw digest is 32 bytes
    check_str("sha_len",   cg_run_native(U + "println(toString(len(sha256Str(\"x\"))))"), "32\n");
    check_true("sha_gate", p_fails("sha256(toBytes(\"x\"))"));   // native gated to std::hash
}

// =============================================================================
// std::net -- the opt-in TCP networking natives (tcpConnect/tcpSend/tcpRecv/tcpClose/tcpListen/
// tcpAccept/tcpSetTimeout; NativeRegistry ids 42-48). Non-deterministic + side-effecting, so
// EXCLUDED from the differential (like file I/O); verified instead by a single-thread LOOPBACK
// round-trip -- listen -> connect -> accept -> send -> recv -- over "127.0.0.1". A dual-stack IPv6
// listener accepts the IPv4 client via a v4-mapped address; SO_REUSEADDR sidesteps a TIME_WAIT clash.
// =============================================================================
void test_std_net() {
    std::cout << "[codegen: std::net]\n";
    auto p_fails = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return false; }
        catch (const svc::CheckFailure&) { return true; }
        catch (...) { return false; }
    };
    // Gating: a tcp* native is out of scope without `use std::net`.
    check_true("net_gate_rejects", p_fails("let r = tcpListen(0)\n0"));
    check_true("net_gate_ok",     !p_fails("use std::net::*\nlet r = tcpListen(0)\n0"));
    // Loopback round-trip. Uses `?` so every net Result is consumed cleanly.
    //
    // The port is **chosen by the OS** (`tcpListen(0)` + `tcpLocalPort`), never hardcoded. Windows
    // hands out 1024-60000 as the dynamic/ephemeral range (`netsh int ipv4 show dynamicport tcp`),
    // so ANY fixed number here can already be held by an unrelated outbound connection -- observed
    // as an intermittent `tcpListen: bind failed (WSA error 10013)` on the old port 47812,
    // with an unrelated HTTPS socket sitting in TIME_WAIT on it. That made the suite randomly red for
    // a reason having nothing to do with the code under test. Asking the OS is not a smaller race, it
    // is no race: bind succeeds, THEN we read back the port it granted.
    const std::string prog =
        "use std::net::*\n"
        "fn roundtrip() -> Result[String, String] {\n"
        "  let lfd = tcpListen(0)?\n"
        "  let port = tcpLocalPort(lfd)?\n"
        "  let cfd = tcpConnect(\"127.0.0.1\", port)?\n"
        "  let sfd = tcpAccept(lfd)?\n"
        "  tcpSend(cfd, toBytes(\"ping\"))?\n"
        "  let got = tcpRecv(sfd, 64)?\n"
        "  tcpClose(cfd)?\n"
        "  tcpClose(sfd)?\n"
        "  tcpClose(lfd)?\n"
        "  Ok(fromBytes(got))\n"
        "}\n"
        "match roundtrip() { Ok(s) => println(s), Err(e) => println(\"ERR: \" + e) }\n";
    check_str("net_loopback", cg_run_native(prog), "ping\n");

    // `tcpLocalPort` itself: after `tcpListen(0)` the OS must have granted a REAL port, so the
    // read-back is > 0 and inside the 16-bit range. Pins the property the two round-trips rely on
    // (if it silently returned 0 they would still pass -- connecting to port 0 would just fail with
    // an error the `match` prints, and neither `check_str` would notice which port was used).
    check_str("net_local_port_assigned",
        cg_run_native("use std::net::*\n"
                      "fn probe() -> Result[Bool, String] {\n"
                      "  let lfd = tcpListen(0)?\n"
                      "  let p = tcpLocalPort(lfd)?\n"
                      "  tcpClose(lfd)?\n"
                      "  Ok(p > 0 && p <= 65535)\n"
                      "}\n"
                      "match probe() { Ok(b) => println(b), Err(e) => println(\"ERR: \" + e) }\n"),
        "true\n");

    // The std::net WRAPPER (TcpConn / TcpListener + connect/accept/sendStr/recvLine). recvLine reads
    // one line and buffers the remainder; the client sends two lines, the server reads the first.
    const std::string wrap =
        "use std::net::*\n"
        "fn roundtrip2() -> Result[String, String] {\n"
        "  let lst = listen(0)?\n"
        "  let cli = connect(\"127.0.0.1\", lst.localPort()?)?\n"
        "  let mut srv = lst.accept()?\n"
        "  cli.sendStr(\"hello\\nworld\\n\")?\n"
        "  let first = srv.recvLine()?\n"
        "  let out = match first { Some(s) => s, None => \"<eof>\" }\n"
        "  srv.close()?\n"
        "  cli.close()?\n"
        "  lst.close()?\n"
        "  Ok(out)\n"
        "}\n"
        "match roundtrip2() { Ok(s) => println(s), Err(e) => println(\"ERR: \" + e) }\n";
    check_str("net_wrapper_line", cg_run_native(wrap), "hello\n");
}

// =============================================================================
// std::regex -- the opt-in byte-level Pike-VM regex engine, pure prelude (S1: compile / isMatch /
// find / findFrom). A `use std::regex::*` prefix + gate pair, then KAT triples. isMatch cases use
// check_bool_p; find cases encode start*1000+end as an Int (-1 = no match); plus check_same diffs.
// =============================================================================
void test_std_regex() {
    std::cout << "[codegen: std::regex]\n";
    const std::string U = "use std::regex::*\n";
    auto p_fails = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return false; }
        catch (const svc::CheckFailure&) { return true; }
        catch (...) { return false; }
    };
    // Gating: bare compile without `use std::regex` is out of scope -> a CheckFailure.
    check_true("rx_gate_rejects", p_fails("match Regex::compile(\"a\") { Ok(_) => 1, Err(_) => 0 }"));
    check_true("rx_gate_ok",     !p_fails(U + "match Regex::compile(\"a\") { Ok(_) => 1, Err(_) => 0 }"));

    // isMatch helper: wrap a pattern+text -> Bool (compile-error folds to false).
    auto im = [&](const char* re, const char* s) {
        return U + "match Regex::compile(\"" + re + "\") { Ok(r) => r.isMatch(\"" + s + "\"), Err(_) => false }";
    };
    check_bool_p("rx_literal_hit",  im("abc", "xxabcyy"), true);
    check_bool_p("rx_literal_miss", im("abc", "xxabxyy"), false);
    check_bool_p("rx_anchored_hit", im("^abc$", "abc"), true);
    check_bool_p("rx_anchored_end", im("^abc$", "abcd"), false);
    check_bool_p("rx_plus_hit",     im("a+b", "aaab"), true);
    check_bool_p("rx_plus_miss",    im("a+b", "b"), false);
    check_bool_p("rx_star_empty",   im("a*b", "b"), true);
    check_bool_p("rx_opt_absent",   im("colou?r", "color"), true);
    check_bool_p("rx_opt_present",  im("colou?r", "colour"), true);
    check_bool_p("rx_opt_toomany",  im("^colou?r$", "colouur"), false);
    check_bool_p("rx_alt_hit",      im("a|b|c", "zzc"), true);
    check_bool_p("rx_alt_miss",     im("^a|b|c$", "d"), false);
    check_bool_p("rx_class_digit",  im("[0-9]+", "abc123"), true);
    check_bool_p("rx_class_nodigit",im("[0-9]+", "abcxyz"), false);
    check_bool_p("rx_class_neg",    im("[^0-9]", "5a"), true);
    check_bool_p("rx_class_neg2",   im("^[^0-9]$", "5"), false);
    check_bool_p("rx_class_pick",   im("gr[ae]y", "gray"), true);
    check_bool_p("rx_dot_hit",      im("a.c", "abc"), true);
    check_bool_p("rx_dot_newline",  im("^a.c$", "a\\nc"), false);
    check_bool_p("rx_group_rep",    im("(ab)+$", "ababab"), true);
    check_bool_p("rx_group_noncap", im("(?:ab)+c", "ababc"), true);
    check_bool_p("rx_alt_prec_ok",  im("^(ab|cd)$", "cd"), true);
    check_bool_p("rx_alt_prec_no",  im("^(ab|cd)$", "abcd"), false);
    // A literal '.' via an escaped metachar. The Skarn SOURCE must read "a\\.b" (backslash, dot),
    // so the regex pattern bytes are  a \ . b  = regex \. matching a literal '.'.
    check_bool_p("rx_esc_dot_hit",  im("a\\\\.b", "a.b"), true);
    check_bool_p("rx_esc_dot_miss", im("a\\\\.b", "axb"), false);

    // search/searchFrom: encode start*1000 + end; -1 on no match.
    auto fnd = [&](const char* re, const char* s) {
        return U + "match Regex::compile(\"" + re + "\") { Ok(r) => match r.search(\"" + s +
               "\") { Some(m) => m.start * 1000 + m.end, None => 0 - 1 }, Err(_) => 0 - 2 }";
    };
    check_int_p("rx_find_span",   fnd("b+", "aabbba"), 2 * 1000 + 5);
    check_int_p("rx_find_greedy", fnd("a+", "baaab"), 1 * 1000 + 4);
    check_int_p("rx_find_empty",  fnd("a*", "baaa"), 0);            // leftmost empty at 0
    check_int_p("rx_find_class",  fnd("[0-9]+", "xx42yy99"), 2 * 1000 + 4);
    check_int_p("rx_find_none",   fnd("z", "abc"), 0 - 1);
    check_int_p("rx_findfrom",
        U + "match Regex::compile(\"[0-9]+\") { Ok(r) => match r.searchFrom(\"xx42yy99\", 4) "
            "{ Some(m) => m.start * 1000 + m.end, None => 0 - 1 }, Err(_) => 0 - 2 }", 6 * 1000 + 8);
    check_str("rx_find_text",
        cg_run_out(U + "match Regex::compile(\"[a-z]+\") { Ok(r) => match r.search(\"  hello42\") "
                       "{ Some(m) => print(m.text), None => print(\"none\") }, Err(_) => print(\"err\") }"),
        "hello");

    // Differentials (VM == RefEval) over the engine.
    check_same("rx_diff_span",
        U + "match Regex::compile(\"[a-z]+[0-9]+\") { Ok(r) => match r.search(\"  abc123!!\") "
            "{ Some(m) => m.start * 1000 + m.end, None => 0 - 1 }, Err(_) => 0 - 2 }", true);
    check_same("rx_diff_alt",
        U + "match Regex::compile(\"(cat|dog|bird)s?\") { Ok(r) => if r.isMatch(\"i have dogs\") { 1 } else { 0 }, Err(_) => 0 - 1 }", true);
    check_same("rx_diff_anchored",
        U + "match Regex::compile(\"^[A-Za-z_][A-Za-z0-9_]*$\") { Ok(r) => if r.isMatch(\"valid_ID9\") { 1 } else { 0 }, Err(_) => 0 - 1 }", true);

    // ---- S2: captures / named groups / lazy iterators / replace -------------
    // Capture spans, encoded start*1000 + end (-1 unmatched / -2 no match).
    auto capg = [&](const char* re, const char* s, int g) {
        return U + "match Regex::compile(\"" + re + "\") { Ok(r) => match r.captures(\"" + s +
               "\") { Some(c) => match c.group(" + std::to_string(g) +
               ") { Some(m) => m.start * 1000 + m.end, None => 0 - 1 }, None => 0 - 2 }, Err(_) => 0 - 3 }";
    };
    check_int_p("rx_cap_g0", capg("([0-9]+)-([0-9]+)", "xx12-345yy", 0), 2 * 1000 + 8);
    check_int_p("rx_cap_g1", capg("([0-9]+)-([0-9]+)", "xx12-345yy", 1), 2 * 1000 + 4);
    check_int_p("rx_cap_g2", capg("([0-9]+)-([0-9]+)", "xx12-345yy", 2), 5 * 1000 + 8);
    check_int_p("rx_cap_unmatched",   // (a)|(b) matching "b": group 1 is unset -> None -> -1
        U + "match Regex::compile(\"(a)|(b)\") { Ok(r) => match r.captures(\"zb\") { Some(c) => match c.group(1) "
            "{ Some(m) => m.start, None => 0 - 1 }, None => 0 - 2 }, Err(_) => 0 - 3 }", 0 - 1);
    check_str("rx_named",
        cg_run_out(U + "match Regex::compile(\"(?<num>[0-9]+)\") { Ok(r) => match r.captures(\"ab99cd\") "
                       "{ Some(c) => match c.groupNamed(\"num\") { Some(m) => print(m.text), None => print(\"?\") }, "
                       "None => print(\"nc\") }, Err(_) => print(\"e\") }"),
        "99");

    // Lazy iterators.
    check_int_p("rx_searchall_count",
        U + "match Regex::compile(\"[0-9]+\") { Ok(r) => count(r.searchAll(\"a1bb22c333\")), Err(_) => 0 - 1 }", 3);
    check_str("rx_searchall_for",
        cg_run_out(U + "match Regex::compile(\"[0-9]+\") { Ok(r) => { for m in r.searchAll(\"a1bb22c333\") { print(m.text) print(\",\") } }, Err(_) => print(\"e\") }"),
        "1,22,333,");
    check_str("rx_split",
        cg_run_out(U + "match Regex::compile(\",\") { Ok(r) => { for seg in r.splitRe(\"a,b,,c\") { print(seg) print(\"|\") } }, Err(_) => print(\"e\") }"),
        "a|b||c|");
    check_int_p("rx_split_count",
        U + "match Regex::compile(\",\") { Ok(r) => count(r.splitRe(\"a,b,,c\")), Err(_) => 0 - 1 }", 4);
    check_int_p("rx_capsall_count",
        U + "match Regex::compile(\"([a-z])([0-9])\") { Ok(r) => count(r.capturesAll(\"a1b2c3\")), Err(_) => 0 - 1 }", 3);

    // replace.
    auto rep = [&](const char* re, const char* s, const char* repl) {
        return U + "match Regex::compile(\"" + re + "\") { Ok(r) => print(r.replaceAllRe(\"" + s + "\", \"" + repl +
               "\")), Err(_) => print(\"e\") }";
    };
    check_str("rx_replace_all",    cg_run_out(rep("[0-9]", "a1b2c3", "X")), "aXbXcX");
    check_str("rx_replace_dollar0",cg_run_out(rep("[0-9]", "a1b2", "<$0>")), "a<1>b<2>");
    check_str("rx_replace_group",  cg_run_out(rep("([0-9])", "a1b2", "$1$1")), "a11b22");
    check_str("rx_replace_named",  cg_run_out(rep("(?<d>[0-9])", "a1b2", "[\\${d}]")), "a[1]b[2]");
    check_str("rx_replace_first",
        cg_run_out(U + "match Regex::compile(\"[0-9]\") { Ok(r) => print(r.replaceRe(\"a1b2\", \"X\")), Err(_) => print(\"e\") }"),
        "aXb2");
    check_str("rx_replace_dollardollar", cg_run_out(rep("x", "axb", "$$")), "a$b");

    // Differentials (VM == RefEval) over captures / iterators / replace.
    check_same("rx_diff_captures",
        U + "match Regex::compile(\"([a-z]+)=([0-9]+)\") { Ok(r) => match r.captures(\"  key=42;\") "
            "{ Some(c) => match c.group(2) { Some(m) => m.start * 1000 + m.end, None => 0 - 1 }, None => 0 - 2 }, Err(_) => 0 - 3 }", true);
    check_same("rx_diff_replaceall",
        U + "match Regex::compile(\"[aeiou]\") { Ok(r) => if r.replaceAllRe(\"regular expression\", \"_\") == \"r_g_l_r _xpr_ss__n\" { 1 } else { 0 }, Err(_) => 0 - 1 }", true);
    check_same("rx_diff_splitcount",
        U + "match Regex::compile(\"[ ]+\") { Ok(r) => count(r.splitRe(\"the  quick brown   fox\")), Err(_) => 0 - 1 }", true);

    // ---- S3: non-greedy / counted / shorthands / word boundary / flags ------
    // `im` (above) folds a pattern+text -> Bool; `imf` adds a flags string.
    auto imf = [&](const char* re, const char* fl, const char* s) {
        return U + "match Regex::compileWith(\"" + re + "\", \"" + fl + "\") { Ok(r) => r.isMatch(\"" + s + "\"), Err(_) => false }";
    };
    // Non-greedy vs greedy (search text).
    check_str("rx_nongreedy",
        cg_run_out(U + "match Regex::compile(\"a.*?b\") { Ok(r) => match r.search(\"axbxb\") { Some(m) => print(m.text), None => print(\"?\") }, Err(_) => print(\"e\") }"),
        "axb");
    check_str("rx_greedy",
        cg_run_out(U + "match Regex::compile(\"a.*b\") { Ok(r) => match r.search(\"axbxb\") { Some(m) => print(m.text), None => print(\"?\") }, Err(_) => print(\"e\") }"),
        "axbxb");
    // Counted repetition.
    check_bool_p("rx_rep_exact_ok",  im("^a{2}$", "aa"), true);
    check_bool_p("rx_rep_exact_no",  im("^a{2}$", "aaa"), false);
    check_bool_p("rx_rep_range_ok",  im("^a{2,3}$", "aaa"), true);
    check_bool_p("rx_rep_range_hi",  im("^a{2,3}$", "aaaa"), false);
    check_bool_p("rx_rep_range_lo",  im("^a{2,3}$", "a"), false);
    check_bool_p("rx_rep_open_ok",   im("^a{2,}$", "aaaaa"), true);
    check_bool_p("rx_rep_open_no",   im("^a{2,}$", "a"), false);
    check_bool_p("rx_rep_group",     im("^(ab){3}$", "ababab"), true);
    check_bool_p("rx_rep_toobig",    // count > 1000 -> compile error -> im folds to false
        im("a{2000}", "aa"), false);
    // Shorthand classes (Skarn source needs "\\d" -> C++ "\\\\d").
    check_bool_p("rx_sh_digit",   im("^\\\\d+$", "12345"), true);
    check_bool_p("rx_sh_digit_no",im("^\\\\d+$", "12a45"), false);
    check_bool_p("rx_sh_word",    im("^\\\\w+$", "ab_9"), true);
    check_bool_p("rx_sh_word_no", im("^\\\\w+$", "a b"), false);
    check_bool_p("rx_sh_nondig",  im("^\\\\D+$", "abc"), true);
    check_bool_p("rx_sh_space",   im("\\\\s", "a b"), true);
    check_bool_p("rx_sh_space_no",im("\\\\s", "ab"), false);
    check_bool_p("rx_sh_inclass", im("^[\\\\d_]+$", "4_2"), true);
    // Word boundaries.
    check_bool_p("rx_wb_hit",   im("\\\\bcat\\\\b", "a cat!"), true);
    check_bool_p("rx_wb_miss",  im("\\\\bcat\\\\b", "category"), false);
    check_bool_p("rx_wb_start", im("\\\\bcat", "cat"), true);
    check_bool_p("rx_nwb_hit",  im("\\\\Bcat", "scat"), true);
    check_bool_p("rx_nwb_miss", im("\\\\Bcat", "a cat"), false);
    // Flags.
    check_bool_p("rx_flag_i",       imf("hello", "i", "HELLO"), true);
    check_bool_p("rx_flag_i_class", imf("^[a-z]+$", "i", "ABc"), true);
    check_bool_p("rx_flag_i_off",   im("hello", "HELLO"), false);
    check_bool_p("rx_flag_s",       imf("^a.b$", "s", "a\\nb"), true);
    check_bool_p("rx_flag_s_off",   im("^a.b$", "a\\nb"), false);
    check_bool_p("rx_flag_m",       imf("^b", "m", "a\\nbc"), true);
    check_bool_p("rx_flag_m_off",   im("^b", "a\\nbc"), false);
    check_bool_p("rx_flag_inline",  im("(?i)hello", "HeLLo"), true);
    check_bool_p("rx_flag_unknown", // unknown flag -> Err -> im-style folds to false
        U + "match Regex::compileWith(\"a\", \"x\") { Ok(_) => true, Err(_) => false }", false);

    // Differentials (VM == RefEval).
    check_same("rx_diff_nongreedy",
        U + "match Regex::compile(\"<.*?>\") { Ok(r) => match r.search(\"<a><b>\") { Some(m) => m.end, None => 0 - 1 }, Err(_) => 0 - 2 }", true);
    check_same("rx_diff_counted",
        U + "match Regex::compile(\"^[0-9]{3}-[0-9]{4}$\") { Ok(r) => if r.isMatch(\"123-4567\") { 1 } else { 0 }, Err(_) => 0 - 1 }", true);
    check_same("rx_diff_shorthand",
        U + "match Regex::compile(\"\\\\w+@\\\\w+\") { Ok(r) => if r.isMatch(\"user@host\") { 1 } else { 0 }, Err(_) => 0 - 1 }", true);
    check_same("rx_diff_flags",
        U + "match Regex::compileWith(\"^[a-z]+$\", \"i\") { Ok(r) => if r.isMatch(\"MixedCase\") { 1 } else { 0 }, Err(_) => 0 - 1 }", true);
}

// =============================================================================
// std::json -- the opt-in JSON library (parse + serialize + accessors), pure prelude.
// Mirrors test_std_bytes: a `use std::json::*` prefix + a gate pair; JSON source appears
// inside C++ raw strings R"JS(...)JS" so Skarn's own \" / \\ escapes stay readable.
// =============================================================================
void test_std_json() {
    std::cout << "[codegen: std::json]\n";
    const std::string U = "use std::json::*\n";
    auto p_fails = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return false; }
        catch (const svc::CheckFailure&) { return true; }
        catch (...) { return false; }
    };
    // Gating: bare parse without `use std::json` is out of scope -> a CheckFailure.
    check_true("json_gate_rejects", p_fails(R"JS(match Json::parse("1") { Ok(_) => 1, Err(_) => 0 })JS"));
    check_true("json_gate_ok",     !p_fails(U + R"JS(match Json::parse("1") { Ok(_) => 1, Err(_) => 0 })JS"));

    // ---- parse: scalars ----
    check_int_p ("json_int",      U + R"JS(match Json::parse("42") { Ok(Integer(n)) => n, _ => -1 })JS", 42);
    check_int_p ("json_int_neg",  U + R"JS(match Json::parse("-7") { Ok(Integer(n)) => n, _ => -1 })JS", -7);
    check_int_p ("json_real",     U + R"JS(match Json::parse("3.5") { Ok(Number(d)) => toInt(d * 2.0), _ => -1 })JS", 7);
    check_int_p ("json_exp",      U + R"JS(match Json::parse("1e3") { Ok(Number(d)) => toInt(d), _ => -1 })JS", 1000);
    check_int_p ("json_overflow", U + R"JS(match Json::parse("999999999999999999") { Ok(Number(_)) => 1, Ok(Integer(_)) => 2, _ => 0 })JS", 1);
    check_bool_p("json_true",     U + R"JS(match Json::parse("true") { Ok(Boolean(b)) => b, _ => false })JS", true);
    check_int_p ("json_null",     U + R"JS(match Json::parse("null") { Ok(Null) => 1, _ => 0 })JS", 1);
    check_int_p ("json_str_len",  U + R"JS(match Json::parse("\"hi\"") { Ok(Text(s)) => len(s), _ => -1 })JS", 2);

    // ---- parse: string escapes + \uXXXX ----
    // "a\nb" -> a, newline, b => 3 bytes.
    check_int_p ("json_str_esc",  U + R"JS(match Json::parse("\"a\\nb\"") { Ok(Text(s)) => len(s), _ => -1 })JS", 3);
    // A = 'A'.
    check_bool_p("json_u_bmp",    U + R"JS(match Json::parse("\"\\u0041\"") { Ok(Text(s)) => s == "A", _ => false })JS", true);
    // Surrogate pair 😀 = U+1F600 (grinning face) => 4 UTF-8 bytes.
    check_int_p ("json_u_astral", U + R"JS(match Json::parse("\"\\uD83D\\uDE00\"") { Ok(Text(s)) => len(s), _ => -1 })JS", 4);

    // ---- parse: containers + whitespace + nesting ----
    check_int_p ("json_arr",      U + R"JS(match Json::parse("[1,2,3]") { Ok(Arr(xs)) => len(xs), _ => -1 })JS", 3);
    check_int_p ("json_arr_empty",U + R"JS(match Json::parse("[]") { Ok(Arr(xs)) => len(xs), _ => -1 })JS", 0);
    check_int_p ("json_obj",      U + R"JS(match Json::parse("{\"a\":1,\"b\":2}") { Ok(Obj(es)) => len(es), _ => -1 })JS", 2);
    check_int_p ("json_ws",       U + R"JS(match Json::parse("  [ 1 , 2 ] ") { Ok(Arr(xs)) => len(xs), _ => -1 })JS", 2);
    check_int_p ("json_nested",   U + R"JS(match Json::parse("{\"a\":[1,{\"b\":2}]}") { Ok(Obj(_)) => 1, _ => 0 })JS", 1);

    // ---- parse: errors ----
    check_int_p ("json_err_open",  U + R"JS(match Json::parse("{") { Err(_) => 1, Ok(_) => 0 })JS", 1);
    check_int_p ("json_err_trail", U + R"JS(match Json::parse("1 2") { Err(_) => 1, Ok(_) => 0 })JS", 1);
    check_int_p ("json_err_lit",   U + R"JS(match Json::parse("nul") { Err(_) => 1, Ok(_) => 0 })JS", 1);
    check_int_p ("json_err_lonesurr", U + R"JS(match Json::parse("\"\\uDE00\"") { Err(_) => 1, Ok(_) => 0 })JS", 1);

    // ---- stringify: compact ----
    check_bool_p("json_str_arr",  U + R"JS(match Json::parse("[1,2,3]") { Ok(j) => j.stringify(), _ => "?" } == "[1,2,3]")JS", true);
    check_bool_p("json_str_obj",  U + R"JS(match Json::parse("{\"a\":1,\"b\":2}") { Ok(j) => j.stringify(), _ => "?" } == "{\"a\":1,\"b\":2}")JS", true);
    check_bool_p("json_str_scalars", U + R"JS(match Json::parse("[null,true,false,-3,2.5]") { Ok(j) => j.stringify(), _ => "?" } == "[null,true,false,-3,2.5]")JS", true);
    // Re-escaping: "a\nb" -> Text(a,newline,b) -> stringify re-escapes the newline.
    check_bool_p("json_str_esc",  U + R"JS(match Json::parse("\"a\\nb\"") { Ok(j) => j.stringify(), _ => "?" } == "\"a\\nb\"")JS", true);
    check_bool_p("json_str_qbs",  U + R"JS(match Json::parse("\"x\\\"y\\\\z\"") { Ok(j) => j.stringify(), _ => "?" } == "\"x\\\"y\\\\z\"")JS", true);
    // A tab (control char) in a value re-escapes as \t.
    check_bool_p("json_str_tab",  U + R"JS(Text(charStr(9)).stringify() == "\"\\t\"")JS", true);
    // Non-ASCII UTF-8 passes through raw (2 bytes for U+00E9), not \u-escaped.
    check_int_p ("json_str_utf8", U + R"JS(match Json::parse("\"\\u00e9\"") { Ok(j) => len(j.stringify()), _ => -1 })JS", 4);

    // ---- non-finite policy ----
    check_bool_p("json_nan_null", U + R"JS(Number(0.0 / 0.0).stringify() == "null")JS", true);
    check_int_p ("json_checked_nan", U + R"JS(match Number(0.0 / 0.0).stringifyChecked() { Err(_) => 1, Ok(_) => 0 })JS", 1);
    check_int_p ("json_checked_inf", U + R"JS(match Arr(push(vec(), Number(1.0 / 0.0))).stringifyChecked() { Err(_) => 1, Ok(_) => 0 })JS", 1);
    check_int_p ("json_checked_ok",  U + R"JS(match Integer(5).stringifyChecked() { Ok(s) => len(s), Err(_) => -1 })JS", 1);

    // ---- round-trip idempotence ----
    check_bool_p("json_roundtrip", U + R"JS(let a = unwrap(Json::parse("{\"x\":[1,2,3],\"y\":true,\"z\":\"hi\"}")).stringify()
let b = unwrap(Json::parse(a)).stringify()
a == b)JS", true);

    // ---- pretty ----
    check_bool_p("json_pretty_arr", U + R"JS(unwrap(Json::parse("[1,2]")).stringifyPretty(2) == "[\n  1,\n  2\n]")JS", true);
    check_bool_p("json_pretty_obj", U + R"JS(unwrap(Json::parse("{\"a\":1}")).stringifyPretty(2) == "{\n  \"a\": 1\n}")JS", true);
    check_bool_p("json_pretty_empty", U + R"JS(unwrap(Json::parse("[]")).stringifyPretty(2) == "[]")JS", true);

    // ---- accessors ----
    check_int_p ("json_getfield",   U + R"JS(match unwrap(Json::parse("{\"a\":1,\"b\":2}")).getField("b") { Some(Integer(n)) => n, _ => -1 })JS", 2);
    check_int_p ("json_getfield_miss", U + R"JS(match unwrap(Json::parse("{\"a\":1}")).getField("z") { None => 1, _ => 0 })JS", 1);
    check_int_p ("json_getfield_nonobj", U + R"JS(match unwrap(Json::parse("[1]")).getField("a") { None => 1, _ => 0 })JS", 1);
    check_int_p ("json_at",         U + R"JS(match unwrap(Json::parse("[10,20,30]")).at(1) { Some(Integer(n)) => n, _ => -1 })JS", 20);
    check_int_p ("json_at_oob",     U + R"JS(match unwrap(Json::parse("[1]")).at(5) { None => 1, _ => 0 })JS", 1);
    check_int_p ("json_asint",      U + R"JS(match unwrap(Json::parse("42")).asInt() { Some(n) => n, None => -1 })JS", 42);
    check_int_p ("json_asint_wrong",U + R"JS(match unwrap(Json::parse("\"x\"")).asInt() { None => 1, _ => 0 })JS", 1);
    check_int_p ("json_asdbl_int",  U + R"JS(match unwrap(Json::parse("7")).asDouble() { Some(d) => toInt(d), None => -1 })JS", 7);
    check_int_p ("json_asdbl_num",  U + R"JS(match unwrap(Json::parse("2.5")).asDouble() { Some(d) => toInt(d * 2.0), None => -1 })JS", 5);
    check_int_p ("json_asstr",      U + R"JS(match unwrap(Json::parse("\"hi\"")).asStr() { Some(s) => len(s), None => -1 })JS", 2);
    check_bool_p("json_asbool",     U + R"JS(match unwrap(Json::parse("true")).asBool() { Some(b) => b, None => false })JS", true);
    check_int_p ("json_asarr",      U + R"JS(match unwrap(Json::parse("[1,2]")).asArr() { Some(xs) => len(xs), None => -1 })JS", 2);
    check_int_p ("json_asobj",      U + R"JS(match unwrap(Json::parse("{\"a\":1}")).asObj() { Some(es) => len(es), None => -1 })JS", 1);
    check_bool_p("json_isnull",     U + R"JS(unwrap(Json::parse("null")).isNull() && !unwrap(Json::parse("1")).isNull())JS", true);
    check_int_p ("json_asmap",      U + R"JS(match unwrap(Json::parse("{\"a\":1,\"b\":2}")).asMap() { Some(m) => len(m), None => -1 })JS", 2);
    check_int_p ("json_asmap_dup",  U + R"JS(match unwrap(Json::parse("{\"a\":1,\"a\":9}")).asMap() { Some(m) => match get(m, "a") { Some(Integer(n)) => n, _ => -1 }, None => -2 })JS", 9);
}

// ---- codegen: parametric traits end-to-end (dispatch on the erased element type) ----
void test_codegen_generic_traits() {
    std::cout << "[codegen: generic traits]\n";
    // User-type Iterable: a generic `toVec` dispatches `iter` DYNAMICALLY on the Bag struct
    // (RESOLVE_CALL, since `it: I` is a generic-param receiver), then returns its Vec.
    check_int("gtrait_user_toVec",
        "trait Iterable[T] { fn iter(self) -> Vec[T] }\n"
        "struct Bag { items: Vec[Int] }\n"
        "impl Iterable[Int] for Bag { fn iter(self) -> Vec[Int] { self.items } }\n"
        "fn toVec[T, I: Iterable[T]](it: I) -> Vec[T] { iter(it) }\n"
        "let mut v: Vec[Int] = vec()\n push(v, 10)\n push(v, 20)\n push(v, 12)\n"
        "let b = Bag { items: v }\n len(toVec(b))", 3);
    // A concrete-receiver call devirtualizes to a direct CALL of the impl fn.
    check_int("gtrait_user_concrete",
        "trait Iterable[T] { fn iter(self) -> Vec[T] }\n"
        "struct Bag { items: Vec[Int] }\n"
        "impl Iterable[Int] for Bag { fn iter(self) -> Vec[Int] { self.items } }\n"
        "let mut v: Vec[Int] = vec()\n push(v, 7)\n"
        "let b = Bag { items: v }\n len(iter(b))", 1);
    // `for x in <user Iterable>` -- the checker accepts it (element from the impl), codegen
    // materializes `iter(b)` to a Vec and index-walks it.
    // NOTE: `for` drives the RING iteration protocol (std::iter Iterator/IntoIterator/Iterable), so a
    // user type opts in by implementing the RING `Iterable` (Rust's model). These use the real prelude
    // (check_int_p) and impl the ring trait -- a user's own parallel `trait Iterable` no longer drives `for`.
    check_int_p("gtrait_for_user",
        "struct Bag { items: Vec[Int] }\n"
        "impl Iterable[Int] for Bag { fn iter(self) -> Vec[Int] { self.items } }\n"
        "let mut v: Vec[Int] = vec()\n push(v, 3)\n push(v, 4)\n push(v, 5)\n"
        "let b = Bag { items: v }\n let mut s = 0\n for x in b { s = s + x }\n s", 12);
    // `for x in c` over a GENERIC `I: Iterable[Int]` receiver -> dynamic RESOLVE_CALL per element.
    check_int_p("gtrait_for_generic",
        "struct Bag { items: Vec[Int] }\n"
        "impl Iterable[Int] for Bag { fn iter(self) -> Vec[Int] { self.items } }\n"
        "fn total[I: Iterable[Int]](c: I) -> Int { let mut s = 0\n for x in c { s = s + x }\n s }\n"
        "let mut v: Vec[Int] = vec()\n push(v, 10)\n push(v, 20)\n"
        "let b = Bag { items: v }\n total(b)", 30);
    // A built-in container impl: `impl[X] Iterable[X] for List[X]` lowers ($List cons + nil columns),
    // so a generic `toVec` works over a list literal (dispatch on the runtime cons cell).
    check_int("gtrait_list_toVec",
        "trait Iterable[T] { fn iter(self) -> Vec[T] }\n"
        "impl[X] Iterable[X] for List[X] { fn iter(self) -> Vec[X] { let mut v: Vec[X] = vec()\n for x in self { push(v, x) }\n v } }\n"
        "fn toVec[T, I: Iterable[T]](it: I) -> Vec[T] { iter(it) }\n"
        "len(toVec([7, 8, 9]))", 3);
}

// ---- regression: checker forward-ref to a generic type's arity --------------
// A generic type referenced by an EARLIER-declared type must see the right arity.
// register_names pre-sizes the arity in pass 1a (syntactic); before the fix a forward
// reference saw arity 0 ("type 'Stream' expects 0 type argument(s), got 1").
void test_check_forward_generic() {
    std::cout << "[check: forward-ref generic arity]\n";
    // Mutually recursive GENERIC types: Step[T] references Stream[T] declared AFTER it.
    check_true("forward_generic_ok", check_errc(
        "enum Step[T] { Yield(T, Stream[T]), Done }\n"
        "struct Stream[T] { next: fn() -> Step[T] }\n"
        "fn mk() -> Stream[Int] { Stream { next: fn() -> Step[Int] { Done } } }") == 0);
    // The arity is still ENFORCED across a forward reference (a wrong count is rejected).
    check_true("forward_generic_arity_enforced", check_has(
        "enum Bad { V(Stream[Int, Int]) }\n"
        "struct Stream[T] { x: T }", "type argument"));

    // Part B: a generic-param bound may reference a LATER sibling type param
    // (`I`'s bound names `T`, declared after) -- order-independent, previously over-rejected.
    check_true("sibling_bound_forward_ref", check_errc(
        "trait Iterable[T] { fn iter(self) -> Vec[T] }\n"
        "fn f[I: Iterable[T], T](x: I) -> Int { 0 }") == 0);
    // The already-working direction (earlier sibling) stays green.
    check_true("sibling_bound_backward_ref", check_errc(
        "trait Iterable[T] { fn iter(self) -> Vec[T] }\n"
        "fn g[T, I: Iterable[T]](x: I) -> Int { 0 }") == 0);
    // Part A: an impl referencing a LATER-declared trait resolves clean (regression lock).
    check_true("forward_trait_in_impl", check_errc(
        "impl[X] Iterable[X] for List[X] { fn iter(self) -> Vec[X] { let mut v: Vec[X] = vec()\n for x in self { push(v, x) }\n v } }\n"
        "trait Iterable[T] { fn iter(self) -> Vec[T] }") == 0);
    // Negatives still enforced: an unknown type in a bound arg, and a duplicate type param.
    check_true("bound_unknown_type", check_has(
        "fn f[T: Foo[Nope]](x: T) -> Int { 0 }", "unknown type"));
    check_true("dup_type_param", check_has(
        "fn f[T, T](x: Int) -> Int { 0 }", "duplicate type parameter"));
}

// ---- regression: 0-argument indirect call return-value frame slot -----------
// A non-tail 0-arg indirect call (e.g. a `next()` on a fn-valued field) reads its
// result at r[frame_size]; validate_frames used to widen only by nargs, so nargs==0
// left the result read one register out of bounds ("touches rN, frame_size N"). This
// blocked EVERY heap-value-through-a-loop/TCO shape (the whole lazy-stream pattern).
void test_codegen_indirect_return() {
    std::cout << "[codegen: 0-arg indirect call return slot]\n";

    // (1) a 0-arg indirect call consumed by a `let`.
    check_int("indirect0_let",
        "struct Box { f: fn() -> Int }\n"
        "fn mk() -> Box { Box { f: fn() -> Int { 42 } } }\n"
        "fn run() -> Int { let b: Box = mk()\n let x: Int = b.f()\n x }\n"
        "run()", 42);

    // (2) a `while` reassigning a heap-struct local through a 0-arg indirect call.
    check_int("indirect0_while",
        "struct Pull { more: Bool, head: Int, tail: Stream }\n"
        "struct Stream { next: fn() -> Pull }\n"
        "fn nil_s() -> Stream { Stream { next: fn() -> Pull { Pull { more: false, head: 0, tail: nil_s() } } } }\n"
        "fn range_s(i: Int, n: Int) -> Stream {\n"
        "  Stream { next: fn() -> Pull {\n"
        "    if i >= n { Pull { more: false, head: 0, tail: nil_s() } }\n"
        "    else { Pull { more: true, head: i, tail: range_s(i + 1, n) } } } } }\n"
        "fn sum_while(s: Stream) -> Int {\n"
        "  let mut acc: Int = 0\n"
        "  let mut p: Pull = s.next()\n"
        "  while p.more { acc = acc + p.head\n p = p.tail.next() }\n"
        "  acc }\n"
        "sum_while(range_s(0, 10))", 45);

    // (3) a self-recursive MATCH whose scrutinee is a 0-arg indirect call, tail-recursive
    //     so it TCOs -- run at N=100000 (>> the ~16k non-tail frame limit) to prove O(1)
    //     stack. Also exercises the checker forward-ref fix (Step[T] before Stream[T]).
    check_int("indirect0_match_tco",
        "enum Step[T] { Yield(T, Stream[T]), Done }\n"
        "struct Stream[T] { next: fn() -> Step[T] }\n"
        "fn count(i: Int, n: Int) -> Stream[Int] {\n"
        "  Stream { next: fn() -> Step[Int] {\n"
        "    if i >= n { Done } else { Yield(i, count(i + 1, n)) } } } }\n"
        "fn sum_go(s: Stream[Int], acc: Int) -> Int {\n"
        "  match s.next() { Yield(x, rest) => sum_go(rest, acc + x), Done => acc } }\n"
        "sum_go(count(0, 100000), 0)", 4999950000LL);

    // (4) an `if`-arm tail call whose argument is a 0-arg indirect call (also O(1) at N).
    check_int("indirect0_if_tco",
        "struct Pull { more: Bool, head: Int, tail: Stream }\n"
        "struct Stream { next: fn() -> Pull }\n"
        "fn nil_s() -> Stream { Stream { next: fn() -> Pull { Pull { more: false, head: 0, tail: nil_s() } } } }\n"
        "fn range_s(i: Int, n: Int) -> Stream {\n"
        "  Stream { next: fn() -> Pull {\n"
        "    if i >= n { Pull { more: false, head: 0, tail: nil_s() } }\n"
        "    else { Pull { more: true, head: i, tail: range_s(i + 1, n) } } } } }\n"
        "fn sum_if(p: Pull, acc: Int) -> Int {\n"
        "  if p.more { sum_if(p.tail.next(), acc + p.head) } else { acc } }\n"
        "sum_if(range_s(0, 100000).next(), 0)", 4999950000LL);
}

// Register-pressure "torture" cluster for the frame-sizing emitter self-check (Tier-1 item 2).
// These exercise the local-allocation paths that were previously UNGUARDED (the `for`-loop
// cursor/len/idx/(k,v) slot locals) plus heap values held across a loop and a many-binding match.
// Correct results here prove the guards/asserts do not mis-fire and the walkers match the emitter;
// a future walker under-count would trip either the alloc_local/alloc_temp guard (at the site) or
// the result comparison.
void test_codegen_frame_selfcheck() {
    std::cout << "[codegen: frame-sizing emitter self-check]\n";

    // (1) `for (k, v) in map` -- the Map for-branch allocates mapl/karr/varr/len/idx + the (k,v)
    //     slot locals (all previously unguarded). Sum is iteration-order-independent.
    check_int("selfcheck_map_kv",
        "let m = #{1 => 10, 2 => 20, 3 => 30}\n"
        "let mut s = 0\n"
        "for (k, v) in m { s = s + k + v }\n"
        "s", 66);

    // (2) nested `for` over cons-list literals -- the inner loop's cursor/len/idx locals stack on
    //     the outer's, pushing local allocation under real pressure.
    check_int("selfcheck_nested_for",
        "let mut s = 0\n"
        "for i in [1, 2, 3] { for j in [10, 20] { s = s + i * j } }\n"
        "s", 180);

    // (3) a many-binding `match` arm (5 pattern binds -> 5 arm locals).
    check_int("selfcheck_match_binds",
        "struct Rec { a: Int, b: Int, c: Int, d: Int, e: Int }\n"
        "fn sumrec(r: Rec) -> Int { match r { Rec{a, b, c, d, e} => a + b + c + d + e } }\n"
        "sumrec(Rec { a: 1, b: 2, c: 3, d: 4, e: 5 })", 15);

    // (4) a heap value (a Vec, a GC root) reassigned across a `while` loop.
    check_int("selfcheck_heap_in_while",
        "let mut v: Vec[Int] = vec()\n"
        "let mut i = 0\n"
        "while i < 5 { v = push(v, i * i)\n i = i + 1 }\n"
        "len(v)", 5);

    // (5) a heap value held/grown across a `for`-over-list loop (the list cursor local coexists
    //     with the Vec local through every iteration).
    check_int("selfcheck_heap_in_for",
        "let mut v: Vec[Int] = vec()\n"
        "for x in [1, 2, 3, 4] { v = push(v, x) }\n"
        "len(v)", 4);

    // Int->Double coercion boundaries (each widens a NON-temp Int -- a local/param -- via an extra
    // I2D temp; these previously under-sized the frame at the unvalidated top level). The results
    // prove the measure now matches the emitter at every boundary.
    check_dbl("selfcheck_coerce_mixed",  "let n = 3\n n + 0.5", 3.5);          // arithmetic operand
    check_dbl("selfcheck_coerce_let",    "let n = 3\n let d: Double = n\n d", 3.0);   // let boundary
    check_dbl("selfcheck_coerce_assign",                                        // ident-assign boundary
        "let mut d: Double = 1.0\n let n = 3\n d = n\n d", 3.0);
    check_dbl("selfcheck_coerce_field",                                         // struct field-init
        "struct P { x: Double }\n let n = 3\n let p = P { x: n }\n p.x", 3.0);
    check_dbl("selfcheck_coerce_field_assign",                                  // field-assign
        "struct P { x: Double }\n let mut p = P { x: 1.0 }\n let n = 3\n p.x = n\n p.x", 3.0);
    check_dbl("selfcheck_coerce_index",                                         // array element assign
        "let mut a: Array[Double] = array(2, 0.0)\n let n = 3\n a[0] = n\n a[0]", 3.0);
    check_dbl("selfcheck_coerce_call",                                          // call argument
        "fn f(a: Double, b: Double) -> Double { a + b }\n let n = 3\n f(n, 0.5)", 3.5);
    check_dbl("selfcheck_coerce_ret",                                           // return boundary
        "fn g(n: Int) -> Double { n }\n g(3)", 3.0);
}

// ---- differential harness: VM vs. the tree-walking reference interpreter ----
//
// Run each PURE program two ways -- the real bytecode path (svc::compile -> execute())
// and the VM-free oracle (refeval::eval_program over the checked AST) -- and compare
// canonical result value + printed output + fault-or-not. A mismatch is a codegen (or
// oracle) bug; this is the net for SILENT miscompiles the hand-written value-assert
// tests cannot catch. The oracle canonicalizes an RtValue; the VM result Value is
// converted to the SAME RtValue shape (below) and fed through the SAME canonicalizer,
// so the two backends cannot drift on formatting.

// Convert a VM result Value into a refeval::RtValue (host-side introspection; no bytecode),
// so the SAME refeval::canonicalize renders both backends and they cannot drift on formatting.
// Recurses through heap aggregates; a VM $Tuple/$List/struct/array/vec/bytes/map maps to the
// matching RtValue shape (map entries collected live, tombstones skipped -- canon sorts them).
refeval::RtValue vm_to_rt(const Value& v, const svc::Module& m) {
    using refeval::Obj; using refeval::ObjKind;
    if (v.isInt())    return static_cast<int64_t>(v.asSigned48());
    if (v.isDouble()) return v.asDouble();
    if (v.isBool())   return v.asBool();
    if (v.isNil())    return std::monostate{};
    if (v.isFunc()) return std::make_shared<refeval::Closure>();   // first-class fn value -> "<fn>"
    if (v.isPtr()) {
        const GcObject* o = GcObject::from_slots(v.asPtr());
        switch (o->kind) {
        case GcObject::KIND_STRING:
            return std::string(o->bytes(), o->string_length());
        case GcObject::KIND_ARRAY: {
            auto r = std::make_shared<Obj>(); r->kind = ObjKind::Array;
            const Value* s = o->slots(); uint32_t n = o->slot_count();
            for (uint32_t i = 0; i < n; ++i) r->items.push_back(vm_to_rt(s[i], m));
            return r;
        }
        case GcObject::KIND_VEC: {
            const Value* hs = o->slots();
            const GcObject* back = GcObject::from_slots(hs[0].asPtr());
            int64_t count = hs[1].asSigned48();
            auto r = std::make_shared<Obj>(); r->kind = ObjKind::Vec;
            const Value* bs = back->slots();
            for (int64_t i = 0; i < count; ++i) r->items.push_back(vm_to_rt(bs[i], m));
            return r;
        }
        case GcObject::KIND_BYTES: {
            const Value* hs = o->slots();
            const GcObject* back = GcObject::from_slots(hs[0].asPtr());
            int64_t count = hs[1].asSigned48();
            auto r = std::make_shared<Obj>(); r->kind = ObjKind::Bytes;
            const char* bb = back->bytes();
            for (int64_t i = 0; i < count; ++i) r->bytes.push_back(static_cast<uint8_t>(bb[i]));
            return r;
        }
        case GcObject::KIND_MAP: {
            const Value* hs = o->slots();
            const GcObject* back = GcObject::from_slots(hs[0].asPtr());
            uint32_t cap = back->slot_count();                 // 2 slots per entry (key, value)
            const Value* bs = back->slots();
            auto r = std::make_shared<Obj>(); r->kind = ObjKind::Map;
            for (uint32_t i = 0; 2 * i + 1 < cap; ++i) {
                const Value& key = bs[2 * i];
                if (key.isUndefined() || key.isTombstone()) continue;
                r->map.emplace_back(vm_to_rt(key, m), vm_to_rt(bs[2 * i + 1], m));
            }
            return r;
        }
        case GcObject::KIND_OBJECT: {
            const StructType& st = m.struct_types[o->object_type_id()];
            const Value* s = o->slots(); uint32_t n = o->slot_count();
            if (st.dump_style == DumpStyle::Tuple) {
                auto r = std::make_shared<Obj>(); r->kind = ObjKind::Tuple;
                for (uint32_t i = 0; i < n; ++i) r->items.push_back(vm_to_rt(s[i], m));
                return r;
            }
            if (st.dump_style == DumpStyle::List) {              // $List cons cell {head, tail}: walk the spine
                auto r = std::make_shared<Obj>(); r->kind = ObjKind::List;
                const GcObject* cur = o;
                while (cur && cur->kind == GcObject::KIND_OBJECT) {
                    const Value* cs = cur->slots();
                    r->items.push_back(vm_to_rt(cs[0], m));      // head
                    const Value& tail = cs[1];
                    if (!tail.isPtr()) break;                    // nil terminates
                    cur = GcObject::from_slots(tail.asPtr());
                }
                return r;
            }
            auto r = std::make_shared<Obj>(); r->kind = ObjKind::Struct; r->type_name = st.name;
            r->field_names = st.field_names;
            for (uint32_t i = 0; i < n; ++i) r->items.push_back(vm_to_rt(s[i], m));
            return r;
        }
        case GcObject::KIND_CLOSURE:
            return std::make_shared<refeval::Closure>();          // -> "<fn>"
        }
        throw std::runtime_error("vm_to_rt: unknown heap kind");
    }
    throw std::runtime_error("vm_to_rt: value kind not modelled");
}

// ---- Construct coverage (the complement of the decline signals) ---------------------------------
// The decline split (RefEval.h) answers "did the oracle turn a construct away?". It cannot answer
// "did any differential test ever REACH it?" -- a construct nothing exercises declines nothing and
// fails nothing, so the tally reads clean while an entire node kind has never been compared against
// the VM. `g_covered` is the positive record: every kind the oracle evaluated in a run that AGREED.
static refeval::Coverage g_covered;

static void merge_coverage(const refeval::Coverage& c) {
    for (size_t i = 0; i < c.expr.size(); ++i) g_covered.expr[i] |= c.expr[i];
    for (size_t i = 0; i < c.stmt.size(); ++i) g_covered.stmt[i] |= c.stmt[i];
    for (size_t i = 0; i < c.pat .size(); ++i) g_covered.pat [i] |= c.pat [i];
    for (size_t i = 0; i < c.op  .size(); ++i) g_covered.op  [i] |= c.op  [i];
    for (size_t i = 0; i < c.compound.size(); ++i) g_covered.compound[i] |= c.compound[i];
    for (size_t i = 0; i < c.builtin .size(); ++i) g_covered.builtin [i] |= c.builtin [i];
    for (size_t i = 0; i < c.native  .size(); ++i) g_covered.native  [i] |= c.native  [i];
}

// The name tables double as the REQUIRED SETS: each switch lists every enumerator and has NO
// `default:`, and C4062 ("enumerator not handled") is escalated to an ERROR for this region -- so a
// kind added to Ast.h fails the BUILD here, naming itself, instead of dropping silently out of the
// required set. That escalation is the whole rot-guard: the project is /W4 WITHOUT warnings-as-errors,
// so a bare warning would gate nothing. The required range is then [0, LAST], where the switch is
// what proves LAST is last.
#pragma warning(push)
#pragma warning(error: 4062)
static const char* kind_name(svc::ExprKind k) {
    switch (k) {
    case svc::ExprKind::IntLit:    return "IntLit";     case svc::ExprKind::DoubleLit: return "DoubleLit";
    case svc::ExprKind::StrLit:    return "StrLit";     case svc::ExprKind::BoolLit:   return "BoolLit";
    case svc::ExprKind::Ident:     return "Ident";      case svc::ExprKind::Unary:     return "Unary";
    case svc::ExprKind::Binary:    return "Binary";     case svc::ExprKind::Pipe:      return "Pipe";
    case svc::ExprKind::Call:      return "Call";       case svc::ExprKind::Field:     return "Field";
    case svc::ExprKind::Index:     return "Index";      case svc::ExprKind::Try:       return "Try";
    case svc::ExprKind::If:        return "If";         case svc::ExprKind::Match:     return "Match";
    case svc::ExprKind::While:     return "While";      case svc::ExprKind::For:       return "For";
    case svc::ExprKind::Loop:      return "Loop";       case svc::ExprKind::Break:     return "Break";
    case svc::ExprKind::Continue:  return "Continue";   case svc::ExprKind::Return:    return "Return";
    case svc::ExprKind::Block:     return "Block";      case svc::ExprKind::Lambda:    return "Lambda";
    case svc::ExprKind::StructLit: return "StructLit";  case svc::ExprKind::Tuple:     return "Tuple";
    case svc::ExprKind::ListLit:   return "ListLit";    case svc::ExprKind::MapLit:    return "MapLit";
    }
    return "<unnamed>";   // unreachable: the switch above is exhaustive, enforced by 4062-as-error
}
static const char* kind_name(svc::StmtKind k) {
    switch (k) {
    case svc::StmtKind::Let:  return "Let";
    case svc::StmtKind::Assign: return "Assign";
    case svc::StmtKind::Expr: return "Expr";
    }
    return "<unnamed>";
}
static const char* kind_name(svc::PatKind k) {
    switch (k) {
    case svc::PatKind::Wildcard: return "Wildcard";  case svc::PatKind::Literal: return "Literal";
    case svc::PatKind::Ident:    return "Ident";     case svc::PatKind::Ctor:    return "Ctor";
    case svc::PatKind::Tuple:    return "Tuple";     case svc::PatKind::List:    return "List";
    case svc::PatKind::Struct:   return "Struct";    case svc::PatKind::Map:     return "Map";
    case svc::PatKind::Or:       return "Or";        case svc::PatKind::Range:   return "Range";
    case svc::PatKind::Bind:     return "Bind";
    }
    return "<unnamed>";
}
#pragma warning(pop)

// The operator TokKinds are ONE contiguous labelled block in Token.h, so the required set is a range
// rather than a hand-kept list -- an operator added in the natural place joins it automatically. The
// two asserts pin the block's boundaries: appending an operator after `Tilde` shifts `Newline` and
// breaks the build with this message rather than silently escaping the range.
static_assert(static_cast<int>(svc::TokKind::Assign) == static_cast<int>(svc::TokKind::Question) + 1,
              "the operator block no longer starts at TokKind::Assign -- fix the coverage range");
static_assert(static_cast<int>(svc::TokKind::Newline) == static_cast<int>(svc::TokKind::Tilde) + 1,
              "an operator was added outside the Assign..Tilde block -- fix the coverage range");

// The BASE operators that have a compound `op=` form. This mirrors the parser's normalization table
// (`PlusEq -> Plus`, ... in Parser.cpp), which is the source of truth and which nothing here can read
// at run time -- so the assert below pins the compound-token block's SIZE instead: adding a twelfth
// `op=` form breaks the build and forces this list to be revisited rather than silently under-require.
static constexpr svc::TokKind COMPOUND_BASE_OPS[] = {
    svc::TokKind::Plus,   svc::TokKind::Minus,  svc::TokKind::Star,
    svc::TokKind::Slash,  svc::TokKind::Percent,
    svc::TokKind::BitAnd, svc::TokKind::BitOr,  svc::TokKind::BitXor,
    svc::TokKind::Shl,    svc::TokKind::Shr,    svc::TokKind::UShr,
};
static_assert(static_cast<int>(svc::TokKind::UShrEq) - static_cast<int>(svc::TokKind::PlusEq) + 1
                  == static_cast<int>(sizeof(COMPOUND_BASE_OPS) / sizeof(COMPOUND_BASE_OPS[0])),
              "a compound-assignment form was added or removed -- update COMPOUND_BASE_OPS");

// Pins the native registry's size. Adding a native must force an explicit decision -- is it
// DETERMINISTIC and side-effect-free, hence differentially checkable and required in
// refeval::DIFFERENTIABLE_NATIVES, or is it not, and stays an honest `Unsupported` skip? Without this
// a new native lands in neither set and is simply never cross-checked, which is the whole failure
// class these coverage assertions exist to end.
// `tcpLocalPort` (id 50) is deliberately NOT differentiable: the port it reports is chosen by the OS.
static_assert(NATIVE_COUNT == 51,
              "a native was added or removed -- decide whether it is deterministic (and so belongs in "
              "refeval::DIFFERENTIABLE_NATIVES), then update this pin");

// The non-printing differential core, shared by check_same (the hand-written corpus, which
// prints per-test) and the program generator's test_generated (which runs it silently in a
// loop and only reports disagreements). Runs `src` through BOTH the real bytecode VM and the
// reference interpreter, and classifies the outcome.
struct DiffOutcome {
    enum Kind {
        Agree,        // both compiled + ran, and value + output + fault-status match
        Disagree,     // both compiled but the results differ (a COMPILER bug: VM != oracle)
        CheckReject,  // the CHECKER rejected the program (ill-typed) -- for a generated program,
                      //   a generator bug (it must emit well-typed code by construction)
        CodegenFail,  // the checker ACCEPTED it but codegen threw (frame self-check / CodegenError)
                      //   -- a COMPILER bug: a well-typed program that lowering cannot handle
        Unsupported,  // the oracle declines BY DESIGN (map dump order / a non-differentiable native)
                      //   -> honest skip, not a pass
        OracleGap     // the oracle LACKS the construct -> a failure: the differential silently stopped
                      //   covering it, which is how the old reference-`==` bug survived (RefEval.h)
    } kind;
    std::string vm_desc;   // "<value>  out=..." or "<fault> msg"  (for Agree/Disagree)
    std::string rf_desc;
    std::string detail;    // reject / codegen-fail / unsupported message
};

// A COMPILE failure (svc::compile / parse_check throws) is kept SEPARATE from a runtime fault:
// a program that does not type-check cannot be differentially tested at run time. Scoring "both
// paths fail to compile" as agreement is a false green (it once masked trait method-dot corpus
// entries that never compiled), so a compile failure is its own outcome, never Agree.
// Fixed, deterministic fixtures for the differentiable natives (refeval::NativeEnv). The SAME values
// feed the VM (args/stdin via execute(); env via the process) and the oracle, so a native call agrees
// iff the codegen lowering is correct. The env var is set once on first use.
static const refeval::NativeEnv& diff_native_env() {
    static const refeval::NativeEnv e = [] {
        refeval::NativeEnv n;
        n.args = { "alpha", "beta" };
        n.stdin_text = "l1\nl2\n";
        n.env_name = "SVC_DIFF_ENV";
        n.env_value = "envval";
        _putenv_s(n.env_name.c_str(), n.env_value.c_str());   // one-time process env for getEnv
        return n;
    }();
    return e;
}

DiffOutcome run_diff(const std::string& src, bool with_prelude) {
    const std::vector<svc::PreludeModule>& prelude = with_prelude ? svc::builtin_prelude() : kNoPrelude;
    const refeval::NativeEnv& nenv = diff_native_env();

    // The VM path does BOTH check and codegen, so it distinguishes the two compile-failure
    // sources: a svc::CheckFailure (the checker rejected it -- ill-typed) vs. any other throw
    // from svc::compile (codegen: the frame emitter self-check / a CodegenError -- a well-typed
    // program lowering cannot handle). Only the former is a generator bug.
    std::string vm_canon, vm_out, vm_err;
    bool vm_fault = false, vm_reject = false, vm_cgfail = false;
    try {
        svc::Module m = svc::compile(src.c_str(), prelude);
        try {
            // BYTECODE-SERIALIZATION FUZZING: run the VM side through a full
            // serialize -> deserialize round-trip, so every corpus + generated program
            // exercises BytecodeIO. A serialization defect then surfaces as a VM/oracle
            // disagreement or a BytecodeError (caught below as a "runtime trap"). Flip
            // kRoundTrip to false to run the compiled Module directly (debugging).
            constexpr bool kRoundTrip = true;
            const bcio::ModuleImage img = kRoundTrip
                ? bcio::deserialize(bcio::serialize(svc::module_to_image(m)))
                : svc::module_to_image(m);
            Heap heap;
            StringInterner interner;
            std::ostringstream oss;
            // The native registry + the SAME fixed args/stdin the oracle sees, so a generated native
            // call is deterministic and cross-checkable. Harmless for programs with no native calls.
            static const std::vector<NativeFunc> natives = build_native_table();
            std::istringstream vm_in(nenv.stdin_text);
            auto res = execute(img.bytecode, &heap, nullptr, &interner, img.top_frame_size,
                               &img.constants, &img.struct_types, &img.string_literals, &kNoAtoms,
                               &img.function_table, &oss,
                               &img.trait_table, img.trait_table_width, img.trait_method_count,
                               &img.line_table, &img.function_names, &img.column_table,
                               &natives, &nenv.args, &vm_in,
                               /*function_modules=*/nullptr, &img.const_arrays);
            vm_canon = refeval::canonicalize(vm_to_rt(res.get_reg_base()[0], m));
            vm_out = oss.str();
        } catch (const std::exception& e) { vm_fault = true; vm_err = e.what(); }   // runtime trap
    } catch (const svc::CheckFailure& e) { vm_reject = true; vm_err = e.what(); }   // checker rejected
      catch (const std::exception& e)    { vm_cgfail = true; vm_err = e.what(); }   // codegen threw

    std::string rf_canon, rf_out, rf_err;
    bool rf_fault = false, rf_unsup = false, rf_gap = false, rf_reject = false;
    refeval::Coverage rf_cov;
    try {
        svc::Program prog = svc::parse_check(src.c_str(), prelude);   // check only (throws on reject)
        refeval::RunResult rr = refeval::eval_program(prog, nenv);
        rf_fault = rr.faulted; rf_unsup = rr.unsupported; rf_gap = rr.oracle_gap; rf_err = rr.fault_msg;
        rf_cov = rr.coverage;
        rf_out = rr.output;
        if (!rr.faulted) rf_canon = refeval::canonicalize(rr.value);
    } catch (const std::exception& e) { rf_reject = true; rf_err = e.what(); }

    if (vm_reject || rf_reject)   // ill-typed on either path
        return { DiffOutcome::CheckReject, {}, {}, vm_reject ? vm_err : rf_err };
    if (vm_cgfail)                // well-typed, but codegen could not lower it
        return { DiffOutcome::CodegenFail, {}, {}, vm_err };
    if (rf_gap)                   // the oracle lacks the construct => coverage silently lost
        return { DiffOutcome::OracleGap, {}, {}, rf_err };
    if (rf_unsup)                 // the oracle declines by design => an honest skip
        return { DiffOutcome::Unsupported, {}, {}, rf_err };

    const bool agree = (vm_fault == rf_fault) &&
                       (vm_fault || (vm_canon == rf_canon && vm_out == rf_out));
    // Construct coverage is folded in ONLY on agreement, and here -- the single place both the corpus
    // (check_same) and the sweep (test_generated) pass through, so neither can be forgotten. "Covered"
    // therefore means the oracle evaluated the construct AND the bytecode produced the same answer.
    if (agree) merge_coverage(rf_cov);
    DiffOutcome o;
    o.kind    = agree ? DiffOutcome::Agree : DiffOutcome::Disagree;
    o.vm_desc = vm_fault ? "<fault> " + vm_err : vm_canon + "  out=" + vm_out;
    o.rf_desc = rf_fault ? "<fault> " + rf_err : rf_canon + "  out=" + rf_out;
    return o;
}

// ---- Shrinker (Slice 2c) -----------------------------------------------------------------
// A greedy line-deletion minimizer: given a source that FAILS a predicate, repeatedly try
// deleting each single line and keep any deletion that STILL fails, to a fixpoint. It reduces
// a random generator monster to a small repro a human can paste into test_torture.
//
// TERMINATION-SAFE BY CONSTRUCTION: it only ever DELETES whole top-level lines. The generator
// emits one statement/declaration per line (a loop body stays inline on its `while`/`for` line),
// so a deletion can never alter a loop's bound or its counter increment -- it removes a whole
// loop or nothing. It deliberately does NOT rewrite/shrink integer literals (which could turn a
// `c = c + 1` counter step into `c = c + 0` and spin forever). Every candidate therefore
// terminates iff the original did -- important because the predicate RUNS each candidate.
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == '\n') { out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
std::string join_lines(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += '\n'; s += v[i]; }
    return s;
}
std::string shrink_lines(std::string src, const std::function<bool(const std::string&)>& fails) {
    for (bool changed = true; changed; ) {
        changed = false;
        std::vector<std::string> lines = split_lines(src);
        for (size_t i = 0; i < lines.size(); ++i) {
            std::vector<std::string> cand = lines;
            cand.erase(cand.begin() + i);
            if (cand.empty()) continue;
            const std::string cs = join_lines(cand);
            if (fails(cs)) { src = cs; changed = true; break; }   // keep the reduction, restart the pass
        }
    }
    return src;
}
// Minimize a generator program that reproduces a COMPILER bug, staying on the SAME failure class:
// a candidate "still fails" only if run_diff returns the same kind (CodegenFail or Disagree). A
// candidate that becomes ill-typed (CheckReject), agrees, or is unsupported is rejected -- so the
// shrinker never wanders off the bug onto a different (or spurious) failure.
std::string shrink_failure(const std::string& src, bool with_prelude, DiffOutcome::Kind want) {
    return shrink_lines(src, [&](const std::string& s) { return run_diff(s, with_prelude).kind == want; });
}

// String interpolation (`"…${e}…"`) -- a pure parser desugar to a `toString`/`+` chain. Scalar holes
// are already differentially covered (RefEval handles scalar `toString`/`+`); container-hole dumps get
// their own RefEval mirror in S2. These smoke tests lock the lexer/parser end-to-end incl. recursion.
void test_interpolation() {
    std::cout << "\n[test_interpolation]\n";
    auto throws = [](const std::string& s) {
        try { svc::compile(s.c_str(), svc::builtin_prelude()); return false; }
        catch (...) { return true; }
    };

    // scalar holes: each type stringifies exactly as toString / `+` coercion does
    check_str("interp_int",    cg_run_out("let x = 3\nprintln(\"x=${x}\")"),        "x=3\n");
    check_str("interp_dbl",    cg_run_out("println(\"d=${1.5}\")"),                 "d=1.5\n");
    check_str("interp_dbl_int",cg_run_out("println(\"d=${2.0}\")"),                 "d=2.0\n");   // Double keeps `.0`
    check_str("interp_bool",   cg_run_out("println(\"b=${true}\")"),               "b=true\n");
    check_str("interp_str",    cg_run_out("let s = \"hi\"\nprintln(\"s=${s}\")"),   "s=hi\n");    // String hole = identity

    // multiple / adjacent / leading holes + an expression inside a hole
    check_str("interp_multi",  cg_run_out("let x = 1\nlet y = 2\nprintln(\"${x}${y}!\")"), "12!\n");
    check_str("interp_expr",   cg_run_out("let x = 4\nprintln(\"${x * x}\")"),      "16\n");
    check_str("interp_lead",   cg_run_out("let n = 9\nprintln(\"${n} left\")"),     "9 left\n");

    // recursive / nested interpolation: a hole contains another interpolated string
    check_str("interp_nested", cg_run_out("let x = 5\nprintln(\"a ${ \"b${x}\" } c\")"), "a b5 c\n");

    // escapes: `\$` -> literal `$` (so a literal `${...}` is writable); a lone `$` needs no escape
    check_str("interp_esc",    cg_run_out("println(\"cost \\${x}\")"),              "cost ${x}\n");
    check_str("interp_lone$",  cg_run_out("println(\"$5 each\")"),                  "$5 each\n");

    // composes as one atomic primary: feeds a surrounding `+` and a call argument
    check_str("interp_plus",   cg_run_out("let x = 7\nprintln(\"n=\" + \"${x}\")"), "n=7\n");
    check_str("interp_arg",    cg_run_out("let x = 8\nprintln(\"${x}\")"),          "8\n");

    // more capable than `+`: a heap (struct) hole works where `"x" + struct` is a type error
    check_str("interp_struct", cg_run_out("struct P { a: Int }\nlet p = P { a: 9 }\nprintln(\"p=${p}\")"),
                               "p=P { a: 9 }\n");

    // differential (scalar + nested): VM and RefEval must agree on the desugared value
    check_same("diff_interp_int",    "let x = 42\n\"v=${x}\"", false);
    check_same("diff_interp_nested", "let x = 3\n\"a ${ \"b${x}\" } c\"", false);
    check_same("diff_interp_mix",    "let a = 1\nlet b = true\n\"a=${a} b=${b}\"", false);

    // S2 -- container-hole differentials (graded-(c) render_dump vs the VM TO_STRING dump). check_same
    // asserts the two backends AGREE, so these self-validate the ordered-dump mirror (Maps excluded).
    check_same("diff_interp_struct",   "struct P { a: Int, b: Int }\nlet p = P { a: 1, b: 2 }\n\"${p}\"", false);
    check_same("diff_interp_strfield", "struct S { name: String, n: Int }\nlet s = S { name: \"hi\", n: 4 }\n\"${s}\"", false);
    check_same("diff_interp_tuple",    "let t = (1, 2, 3)\n\"${t}\"", false);
    check_same("diff_interp_nestlist", "struct Pt { x: Int }\nlet ps = [Pt { x: 1 }, Pt { x: 2 }]\n\"${ps}\"", false);
    check_same("diff_interp_enum",     "let o = Some(3)\n\"got ${o}\"", true);
    check_same("diff_interp_none",     "let o: Option[Int] = None\n\"${o}\"", true);
    check_same("diff_interp_vec",      "let v = toVec([4, 5, 6])\n\"${v}\"", true);
    check_same("diff_interp_array",    "let a = array(2, 9)\n\"${a}\"", true);
    check_same("diff_interp_bytes",    "let mut b = bytes()\npush(b, 222)\npush(b, 173)\n\"${b}\"", true);

    // exact VM-side dump through interpolation (locks the rendering, incl. NESTED string quoting)
    check_str("interp_strfield", cg_run_out("struct S { name: String, n: Int }\nlet s = S { name: \"hi\", n: 4 }\nprintln(\"${s}\")"),
                                 "S { name: \"hi\", n: 4 }\n");
    check_str("interp_tuple",    cg_run_out("let t = (1, 2)\nprintln(\"${t}\")"),        "(1, 2)\n");
    check_str("interp_enum",     cg_run_out("let o = Some(7)\nprintln(\"${o}\")"),       "Some(7)\n");
    check_str("interp_bytes",    cg_run_out("let mut b = bytes()\npush(b, 222)\npush(b, 173)\nprintln(\"${b}\")"), "#b[DE AD]\n");

    // rejections: an empty hole is a parse error; an interpolated string is not a pattern
    check_true("interp_reject_empty",   throws("println(\"${}\")"));
    check_true("interp_reject_pattern", throws("let x = 1\nmatch x { \"${x}\" => 1, _ => 0 }"));

    // ---- format specifiers `${e:spec}` -> format(e, "spec") (the Format trait; scalars only) ----
    check_str("interp_spec_hex",  cg_run_out("let n = 255\nprintln(\"h=${n:x}\")"),        "h=ff\n");
    check_str("interp_spec_pad",  cg_run_out("println(\"[${42:>6}]\")"),                     "[    42]\n");
    check_str("interp_spec_zero", cg_run_out("println(\"[${42:06}]\")"),                     "[000042]\n");
    check_str("interp_spec_prec", cg_run_out("let p = 3.14159\nprintln(\"p=${p:.2f}\")"),    "p=3.14\n");
    check_str("interp_spec_neg",  cg_run_out("println(\"${-3.5:.1f}\")"),                     "-3.5\n");
    check_str("interp_spec_two",  cg_run_out("let n = 255\nprintln(\"${n:x}/${n:b}\")"),      "ff/11111111\n");
    check_str("interp_spec_str",  cg_run_out("println(\"[${\"hi\":*^6}]\")"),                 "[**hi**]\n");
    check_str("interp_spec_space",cg_run_out("println(\"[${42 :>5}]\")"),                     "[   42]\n");
    // a spec hole and a plain hole coexist in one string
    check_str("interp_spec_mix",  cg_run_out("let n = 10\nprintln(\"${n} = ${n:x} hex\")"),   "10 = a hex\n");
    // nested interpolation carries an inner spec
    check_str("interp_spec_nest", cg_run_out("let n = 255\nprintln(\"o ${ \"i=${n:X}\" } d\")"), "o i=FF d\n");
    // a hole ENDING IN A STRING LITERAL with an ident-start spec char (`s`/`x`/…) -- the `:s` is a
    // spec separator (a `:` at the top of a hole is always the format-spec separator)
    check_str("interp_spec_strlit", cg_run_out("println(\"${\"hi\":s}\")"),                   "hi\n");
    check_str("interp_spec_strhex", cg_run_out("println(\"${\"hi\":>5}\")"),                  "   hi\n");

    // v2 spec holes through the desugar (sign '+', '#' alt-form, scientific 'e')
    check_str("interp_spec_plus", cg_run_out("let n = 7\nprintln(\"${n:+}\")"),               "+7\n");
    check_str("interp_spec_alt",  cg_run_out("let n = 255\nprintln(\"${n:#x}\")"),            "0xff\n");
    check_str("interp_spec_sci",  cg_run_out("let d = 1234.5\nprintln(\"${d:.2e}\")"),        "1.23e3\n");

    // spec rejections (parse-time grammar validation)
    check_true("interp_spec_badtype",  throws("let x = 1\nprintln(\"${x:zq}\")"));      // unknown type char
    check_true("interp_spec_empty",    throws("let x = 1\nprintln(\"${x:}\")"));        // empty spec after ':'
    check_true("interp_spec_dyn",      throws("let x = 1\nprintln(\"${x:>{w}}\")"));    // dynamic width unsupported
    check_true("interp_spec_dotonly",  throws("let x = 1.0\nprintln(\"${x:.f}\")"));    // '.' without digits
    // a spec on a non-scalar (struct) is a Format-bound error, while a plain hole still works
    check_true("interp_spec_struct",   throws("struct P { a: Int }\nlet p = P { a: 1 }\nprintln(\"${p:>8}\")"));
    check_str ("interp_struct_plain",  cg_run_out("struct P { a: Int }\nlet p = P { a: 1 }\nprintln(\"${p}\")"), "P { a: 1 }\n");

    // differential: an interpolated spec hole must agree VM-vs-oracle (format is pure Skarn)
    check_same("diff_interp_spec_int", "let n = 4095\n\"v=${n:x} w=${n:>8}\"", true);
    check_same("diff_interp_spec_dbl", "let p = 2.71828\n\"p=${p:.3f}\"", true);
}

// ---- format specifiers: the `Format` trait behind `${e:spec}` (and the direct `format(x, spec)` call).
// v1 grammar [[fill]align]['0'][width]['.'precision][type]; scalars only; inapplicable field ignored;
// negative non-decimal bases are sign-magnitude. Pure prelude Skarn -> the oracle runs it as-is (S3). ----
void test_format() {
    std::cout << "\n[test_format]\n";
    auto fmt = [](const char* v, const char* s) {
        return cg_run_out(std::string("println(format(") + v + ", \"" + s + "\"))");
    };
    // base conversions
    check_str("fmt_hex",      fmt("255", "x"),  "ff\n");
    check_str("fmt_HEX",      fmt("255", "X"),  "FF\n");
    check_str("fmt_bin",      fmt("255", "b"),  "11111111\n");
    check_str("fmt_oct",      fmt("64",  "o"),  "100\n");
    check_str("fmt_dec",      fmt("42",  "d"),  "42\n");
    check_str("fmt_neg_hex",  fmt("-255","x"),  "-ff\n");     // sign-magnitude
    check_str("fmt_zero",     fmt("0",   "x"),  "0\n");
    // width + default align (numbers right, strings left)
    check_str("fmt_w_int",    fmt("42",  "6"),  "    42\n");
    check_str("fmt_w_left",   fmt("42",  "<6"), "42    \n");
    check_str("fmt_w_center", fmt("42",  "^6"), "  42  \n");
    check_str("fmt_w_str",    fmt("\"hi\"", "6"),  "hi    \n");
    check_str("fmt_w_strR",   fmt("\"hi\"", ">6"), "    hi\n");
    check_str("fmt_fill",     fmt("\"hi\"", "*^6"),"**hi**\n");
    // zero-pad (numbers: after the sign)
    check_str("fmt_zpad",     fmt("42",  "06"),  "000042\n");
    check_str("fmt_zpad_neg", fmt("-42", "06"),  "-00042\n");
    check_str("fmt_zpad_hex", fmt("255", "06x"), "0000ff\n");
    // precision (Double, fixed) + half-to-even
    check_str("fmt_prec2",    fmt("3.14159", ".2f"),  "3.14\n");
    check_str("fmt_prec0",    fmt("3.14159", ".0f"),  "3\n");
    check_str("fmt_prec_neg", fmt("-3.14159",".2f"),  "-3.14\n");
    check_str("fmt_even_down",fmt("2.5", ".0f"),       "2\n");
    check_str("fmt_even_up",  fmt("3.5", ".0f"),       "4\n");
    check_str("fmt_prec_pad", fmt("1.0", ".3f"),       "1.000\n");
    check_str("fmt_prec_w",   fmt("3.14159", "10.2f"), "      3.14\n");
    check_str("fmt_prec_zw",  fmt("3.14159", "010.2f"),"0000003.14\n");
    // bool, s (no-op type), default double
    check_str("fmt_bool",     fmt("true", "^7"),  " true  \n");
    check_str("fmt_s",        fmt("\"x\"", "s"),  "x\n");
    check_str("fmt_dbl_def",  fmt("2.5", ""),     "2.5\n");
    // empty spec on a scalar = identity-ish (no width) -- callable directly
    check_str("fmt_empty",    fmt("7", ""),       "7\n");

    // ---- v2: sign '+', '#' alternate form, scientific 'e'/'E' ----
    // sign flag '+' (force a leading '+' on non-negatives; '-' still wins)
    check_str("fmt_plus_pos",  fmt("42",   "+"),    "+42\n");
    check_str("fmt_plus_neg",  fmt("-42",  "+"),    "-42\n");
    check_str("fmt_plus_zpad", fmt("42",   "+06"),  "+00042\n");
    check_str("fmt_plus_dbl",  fmt("3.14", "+.1f"), "+3.1\n");
    check_str("fmt_plus_hex",  fmt("255",  "+x"),   "+ff\n");
    // '#' alternate form (base prefix)
    check_str("fmt_alt_hex",   fmt("255",  "#x"),   "0xff\n");
    check_str("fmt_alt_HEX",   fmt("255",  "#X"),   "0XFF\n");
    check_str("fmt_alt_bin",   fmt("255",  "#b"),   "0b11111111\n");
    check_str("fmt_alt_oct",   fmt("64",   "#o"),   "0o100\n");
    check_str("fmt_alt_dec",   fmt("42",   "#d"),   "42\n");           // '#' on decimal = no-op
    check_str("fmt_alt_zpad",  fmt("255",  "#08x"), "0x0000ff\n");     // zeros after the 0x prefix
    check_str("fmt_alt_neg",   fmt("-255", "#x"),   "-0xff\n");        // sign-magnitude
    check_str("fmt_alt_plus",  fmt("255",  "+#x"),  "+0xff\n");
    // scientific 'e'/'E' (default precision 6, minimal signed exponent)
    check_str("fmt_sci_def",   fmt("1234.5", "e"),   "1.234500e3\n");
    check_str("fmt_sci_up",    fmt("1234.5", "E"),   "1.234500E3\n");
    check_str("fmt_sci_p2",    fmt("1234.5", ".2e"), "1.23e3\n");
    check_str("fmt_sci_p0",    fmt("1234.5", ".0e"), "1e3\n");
    check_str("fmt_sci_zero",  fmt("0.0",    "e"),   "0.000000e0\n");
    check_str("fmt_sci_zerop0",fmt("0.0",    ".0e"), "0e0\n");
    check_str("fmt_sci_neg",   fmt("-0.05",  "e"),   "-5.000000e-2\n");
    check_str("fmt_sci_carry", fmt("9.9996", ".3e"), "1.000e1\n");     // mantissa carry bumps the exp
    check_str("fmt_sci_plus",  fmt("1234.5", "+e"),  "+1.234500e3\n");
    check_str("fmt_sci_w",     fmt("1234.5", "12.2e"),"      1.23e3\n");

    // differential: the oracle executes the pure-Skarn `format` for free (no C++ mirror)
    check_same("diff_fmt_hex",   "format(4095, \"x\")", true);
    check_same("diff_fmt_wpad",  "format(42, \"08\")",  true);
    check_same("diff_fmt_prec",  "format(3.14159, \".3f\")", true);
    check_same("diff_fmt_str",   "format(\"hi\", \"^9\")",   true);
    check_same("diff_fmt_altx",  "format(65535, \"#x\")",    true);
    check_same("diff_fmt_plus",  "format(42, \"+06\")",      true);
    check_same("diff_fmt_sci",   "format(6.022, \".3e\")",   true);
}

// ---- `for c in s`: direct String iteration = raw BYTE iteration (Int 0..255), Option B ----
// The whole compiler side is one prelude impl (`impl IntoIterator[Int] for String`); the checker/codegen
// route a String scrutinee onto the same lazy `intoIter -> next()` path as Bytes. The loop var is an Int
// byte, consistent with char literals (`'A'` is a byte). No code-point decoding (a later, encoder-gated item).
void test_string_iter() {
    std::cout << "\n[test_string_iter]\n";
    // sum of the ASCII bytes of "abc" = 97 + 98 + 99
    check_int_p("striter_sum",      "let mut s = 0\nfor c in \"abc\" { s = s + c }\ns", 294);
    // empty string -> zero iterations
    check_int_p("striter_empty",    "let mut s = 0\nfor c in \"\" { s = s + 1 }\ns", 0);
    // the byte element compares directly to a char literal (both are bytes): count 'a' in "banana"
    check_int_p("striter_char",     "let mut n = 0\nfor c in \"banana\" { if c == 'a' { n = n + 1 } }\nn", 3);
    // break: stop at 'c' in "abcd" -> summed 'a' + 'b' = 97 + 98
    check_int_p("striter_break",    "let mut s = 0\nfor c in \"abcd\" { if c == 'c' { break }\ns = s + c }\ns", 195);
    // continue: skip '.' in "a.b.c" -> 3 non-dot bytes
    check_int_p("striter_continue", "let mut n = 0\nfor c in \"a.b.c\" { if c == '.' { continue }\nn = n + 1 }\nn", 3);
    // a String is a first-class lazy source: intoIter(s) feeds the combinator surface directly
    check_int_p("striter_stream",   "sum(map(intoIter(\"abc\"), fn(b: Int) -> Int { b - 96 }))", 6);   // 1+2+3

    // exact output: iterating "AB" prints each byte's decimal value
    check_str("striter_out", cg_run_out("for c in \"AB\" { println(toString(c)) }"), "65\n66\n");

    // differential (VM: intoIter(toBytes(s)) -> BytesCursor; RefEval: the String byte branch) -- must agree
    check_same("diff_striter_ascii",  "let mut s = 0\nfor c in \"hello\" { s = s + c }\ns", true);
    check_same("diff_striter_empty",  "let mut s = 0\nfor c in \"\" { s = s + 1 }\ns", true);
    // >127 bytes: build the String via fromBytes so a 200-byte flows through (no `\x` string escape exists)
    check_same("diff_striter_hibyte", "let mut b = bytes()\npush(b, 200)\npush(b, 5)\nlet s = fromBytes(b)\nlet mut a = 0\nfor c in s { a = a + c }\na", true);
    // through a fn boundary (the String param is erased -- the for still lowers to the trait path)
    check_same("diff_striter_fn",     "fn f(s: String) -> Int { let mut n = 0\nfor c in s { n = n + c }\nn }\nf(\"xyz\")", true);
}

// ---- growable register + return stacks: deep NON-TAIL recursion ----
// Runs through execute(), which enables growth (vm.resources). `n + sumTo(n-1)` is non-tail
// (the add happens AFTER the call returns) -> no TCO -> real register + return stack growth.
// Depths far past the old fixed ~16384-frame cap must now succeed with the exact value; a
// mis-committed base or a stale root during a grow would corrupt the result or crash.
void test_deep_recursion() {
    std::cout << "\n[test_deep_recursion]\n";
    const std::string sumTo = "fn sumTo(n: Int) -> Int { if n <= 0 { 0 } else { n + sumTo(n - 1) } }\n";
    // 20000 > the old 16384 return-frame cap -> forces grow_ret (and grow_reg). n*(n+1)/2.
    check_int_p("deep_rec_20k",  sumTo + "sumTo(20000)",  200010000LL);
    // 100000 -> several doublings of BOTH stacks; the exact value proves no corruption.
    check_int_p("deep_rec_100k", sumTo + "sumTo(100000)", 5000050000LL);
    // Growth x GC roots: each frame allocates a heap array and holds it LIVE across the
    // recursive call, so a collection while deeply recursed must scan every grown frame
    // (via the return stack) correctly. sg(n) = 7*n. 20000 deep -> ~20000 simultaneously
    // live arrays across multiple collections.
    const std::string sg =
        "fn sg(n: Int) -> Int { if n <= 0 { 0 } else { let a = array(1, 7)\n let r = sg(n - 1)\n a[0] + r } }\n";
    check_int_p("deep_rec_gc", sg + "sg(20000)", 140000LL);
}

// =============================================================================
// Bytecode serialization round-trip (BytecodeIO.h). A compiled Module -> a byte
// image -> back -> execute() must reproduce the direct run exactly. Compares the
// PRINT output (heap-address-independent, unlike raw reg0 for a heap result) and,
// for pure-value programs, reg0. In-memory only (no temp files).
// =============================================================================

// Run a compiled Module directly, capturing print output; returns reg0.
static Value bc_run_module(const svc::Module& m, std::string& out) {
    Heap heap;
    StringInterner interner;
    std::ostringstream oss;
    static const std::vector<NativeFunc> natives = build_native_table();
    auto res = execute(m.bytecode, &heap, nullptr, &interner, m.top_frame_size,
                       &m.constants, &m.struct_types, &m.string_literals, &kNoAtoms,
                       &m.function_table, &oss,
                       &m.trait_table, m.trait_table_width, m.trait_method_count,
                       &m.line_table, &m.function_names, &m.column_table, &natives);
    out = oss.str();
    return res.get_reg_base()[0];
}

// Run a deserialized image, capturing print output; returns reg0.
static Value bc_run_image(const bcio::ModuleImage& m, std::string& out) {
    Heap heap;
    StringInterner interner;
    std::ostringstream oss;
    static const std::vector<NativeFunc> natives = build_native_table();
    auto res = execute(m.bytecode, &heap, nullptr, &interner, m.top_frame_size,
                       &m.constants, &m.struct_types, &m.string_literals, &kNoAtoms,
                       &m.function_table, &oss,
                       &m.trait_table, m.trait_table_width, m.trait_method_count,
                       &m.line_table, &m.function_names, &m.column_table, &natives);
    out = oss.str();
    return res.get_reg_base()[0];
}

// Compile (with prelude), run direct + round-tripped, assert the print output agrees.
static void bc_rt_out(const char* name, const std::string& src) {
    try {
        svc::Module m = svc::compile(src.c_str(), svc::builtin_prelude());
        std::string out_direct; bc_run_module(m, out_direct);
        std::vector<uint8_t> bytes = bcio::serialize(svc::module_to_image(m));
        bcio::ModuleImage img = bcio::deserialize(bytes);
        std::string out_rt; bc_run_image(img, out_rt);
        check_true(name, out_direct == out_rt);
        if (out_direct != out_rt)
            std::cout << "     direct: [" << out_direct << "]\n     rtrip : [" << out_rt << "]\n";
    } catch (const std::exception& e) {
        ++g_fail;
        std::cout << "  WRONG!    " << name << "   (threw: " << e.what() << ")\n";
    }
}

// Round-trip a pure-value program and assert reg0 == expect (Int). Also confirms the
// direct run agrees, so this pins the absolute value AND serialization fidelity.
static void bc_rt_int(const char* name, const std::string& src, int64_t expect) {
    try {
        svc::Module m = svc::compile(src.c_str(), svc::builtin_prelude());
        std::string ig;
        Value vd = bc_run_module(m, ig);
        std::vector<uint8_t> bytes = bcio::serialize(svc::module_to_image(m));
        Value vr = bc_run_image(bcio::deserialize(bytes), ig);
        const bool ok = vd.isInt() && vr.isInt() && vd.asSigned48() == expect && vr.asSigned48() == expect;
        check_true(name, ok);
        if (!ok) std::cout << "     expected Int " << expect << ", direct "
                           << (vd.isInt() ? vd.asSigned48() : -999) << ", rtrip "
                           << (vr.isInt() ? vr.asSigned48() : -999) << "\n";
    } catch (const std::exception& e) {
        ++g_fail;
        std::cout << "  WRONG!    " << name << "   (threw: " << e.what() << ")\n";
    }
}

// Expect a bcio::BytecodeError from `f` (a corrupt/incompatible image).
template <class F>
static void bc_expect_error(const char* name, F&& f) {
    bool threw = false;
    try { f(); }
    catch (const bcio::BytecodeError&) { threw = true; }
    catch (...) { threw = false; }
    check_true(name, threw);
}

void test_bytecode_roundtrip() {
    std::cout << "[bytecode: serialization round-trip]\n";

    // Pure-value programs: pin the absolute value + fidelity.
    bc_rt_int("bc_arith",  "6 * 7", 42);
    bc_rt_int("bc_let",    "let a = 10\n let b = 32\n a + b", 42);
    bc_rt_int("bc_ifexpr", "if 3 < 5 { 42 } else { 0 }", 42);

    // Output programs: exercise every serialized table (const pool / string literals /
    // struct types / function table / trait table / debug tables) via the print dump.
    bc_rt_out("bc_int",     "println(6 * 7)");
    bc_rt_out("bc_bool",    "println(3 < 5)");
    bc_rt_out("bc_str",     "println(\"hello \" + \"world\")");
    bc_rt_out("bc_interp",  "let n = 5\n println(\"n=${n} sq=${n * n}\")");
    bc_rt_out("bc_double",  "println(3.5 * 2.0)");
    bc_rt_out("bc_struct",  "struct P { x: Int, y: Int }\n println(P { x: 1, y: 2 })");
    bc_rt_out("bc_tuple",   "println((1, 2, 3))");
    bc_rt_out("bc_list",    "println([1, 2, 3])");
    bc_rt_out("bc_option",  "println(toString(Some(42)))");
    bc_rt_out("bc_vec",     "let mut v = vec()\n push(v, 1)\n push(v, 2)\n println(v)");
    bc_rt_out("bc_map",     "let mut m: Map[String, Int] = #{}\n m[\"a\"] = 1\n println(len(m))");
    bc_rt_out("bc_closure", "let add = fn(a: Int) -> fn(Int) -> Int { fn(b: Int) -> Int { a + b } }\n println(add(3)(4))");
    bc_rt_out("bc_for",     "let mut s = 0\n for i in range(0, 5) { s = s + i }\n println(s)");
    bc_rt_out("bc_native",  "println(toString(parseInt(\"123\")))");
    bc_rt_out("bc_trait",
        "trait Greet { fn greet(self) -> String }\n"
        " struct Dog {}\n"
        " impl Greet for Dog { fn greet(self) -> String { \"woof\" } }\n"
        " println(greet(Dog {}))");
    bc_rt_out("bc_strip_debug", "println(6 * 7)");   // (re-checked below without debug info)

    // Debug info is strippable: a no-DBGL image still runs to the same output.
    try {
        svc::Module m = svc::compile("println(1 + 2 + 3)", svc::builtin_prelude());
        std::string od; bc_run_module(m, od);
        std::vector<uint8_t> bytes = bcio::serialize(svc::module_to_image(m), /*include_debug=*/false);
        bcio::ModuleImage img = bcio::deserialize(bytes);
        std::string os; bc_run_image(img, os);
        check_true("bc_no_debug_runs", od == os && img.line_table.empty() && img.column_table.empty());
    } catch (const std::exception& e) {
        ++g_fail; std::cout << "  WRONG!    bc_no_debug_runs   (threw: " << e.what() << ")\n";
    }

    // Negative: a corrupt / incompatible image must throw BytecodeError, never misrun.
    svc::Module m = svc::compile("6 * 7", svc::builtin_prelude());
    const std::vector<uint8_t> good = bcio::serialize(svc::module_to_image(m));

    bc_expect_error("bc_bad_magic", [&] {
        std::vector<uint8_t> b = good; b[0] ^= 0xFF; bcio::deserialize(b);
    });
    bc_expect_error("bc_crc_flip", [&] {
        std::vector<uint8_t> b = good; b[b.size() / 2] ^= 0xFF; bcio::deserialize(b);   // breaks CRC
    });
    bc_expect_error("bc_truncated", [&] {
        std::vector<uint8_t> b(good.begin(), good.begin() + good.size() / 2); bcio::deserialize(b);
    });
    bc_expect_error("bc_too_small", [&] {
        std::vector<uint8_t> b = { 'S', 'K', 'B', 'C' }; bcio::deserialize(b);
    });
    // ISA-version gate: re-stamp the CRC so only the version check can fire.
    bc_expect_error("bc_isa_mismatch", [&] {
        std::vector<uint8_t> b = good;
        b[7] = 0xFF; b[8] = 0xFF;                              // isa_version u16 at bytes [7,8]
        uint32_t crc = bcio::crc32(b.data(), b.size() - 4);
        for (int i = 0; i < 4; ++i) b[b.size() - 4 + i] = uint8_t(crc >> (8 * i));
        bcio::deserialize(b);
    });
}

// Compare the two backends on `src` and print the per-test result. `with_prelude` links the
// static prelude (Option/Result + combinators). A thin printing wrapper over run_diff.
void check_same(const char* name, const std::string& src, bool with_prelude = false) {
    DiffOutcome o = run_diff(src, with_prelude);
    switch (o.kind) {
        case DiffOutcome::Agree:
            ++g_pass; std::cout << "  CORRECT!  " << name << "\n"; break;
        case DiffOutcome::Unsupported:   // declined BY DESIGN -- not counted, keeps corpus coverage honest
            std::cout << "  SKIP      " << name << "   (oracle: " << o.detail << ")\n"; break;
        case DiffOutcome::OracleGap:     // declined because the oracle lacks it -- this test checks NOTHING
            ++g_fail; std::cout << "  WRONG!    " << name << "  (oracle gap -- teach RefEval or justify "
                                << "it as Unsupported: " << o.detail << ")\n"; break;
        case DiffOutcome::CheckReject:
            ++g_fail; std::cout << "  WRONG!    " << name << "  (does not type-check: "
                                << o.detail << ")\n"; break;
        case DiffOutcome::CodegenFail:
            ++g_fail; std::cout << "  WRONG!    " << name << "  (codegen failed: "
                                << o.detail << ")\n"; break;
        case DiffOutcome::Disagree:
            ++g_fail;
            std::cout << "  WRONG!    " << name << "  (VM vs oracle disagree)\n";
            std::cout << "     vm    : " << o.vm_desc << "\n";
            std::cout << "     oracle: " << o.rf_desc << "\n"; break;
    }
}

// `mut self` / `mut param` -- in-place, caller-visible mutation of a heap AGGREGATE
// (structs / tuples / Array / Vec / Bytes / Map). A `mut` scalar / enum / List / fn
// param is a checker error (no mutable primitive params); `mut` is a binding-local
// qualifier, NOT part of the method signature. Codegen/VM are unchanged (a heap value
// is a pointer, so SET_PROP / ARRAY_SET already mutate in place). Value checks here; the
// differential corpus cross-checks the aliasing against the reference oracle (diff_mut_*).
void test_mut_receiver() {
    std::cout << "[codegen: mut self / mut param]\n";

    // mut self on a struct: the caller sees the field mutation (aliasing through the heap).
    check_int("mut_self_field",
        "struct Counter { n: Int }\n trait Bump { fn bump(mut self) -> Int }\n"
        " impl Bump for Counter { fn bump(mut self) -> Int { self.n = self.n + 1\n self.n } }\n"
        " let mut c = Counter { n: 0 }\n let a = bump(c)\n let b = bump(c)\n a + b + c.n", 5);

    // mut param on an Array: index-assign mutates the caller's array in place.
    check_int("mut_param_array",
        "fn setat(mut a: Array[Int], i: Int, v: Int) -> Int { a[i] = v\n a[i] }\n"
        " let mut a = array(3, 0)\n let r = setat(a, 1, 9)\n r + a[1]", 18);

    // A mutable-cursor iterator: `next(mut self)` advances `self.i` in place (the streams motive).
    check_int("mut_self_iterator",
        "struct Cur { data: Array[Int], i: Int }\n trait Nxt { fn next(mut self) -> Int }\n"
        " impl Nxt for Cur { fn next(mut self) -> Int { let v = self.data[self.i]\n self.i = self.i + 1\n v } }\n"
        " let mut a = array(3, 0)\n a[0] = 10\n a[1] = 20\n a[2] = 30\n"
        " let mut c = Cur { data: a, i: 0 }\n next(c) + next(c) + next(c)", 60);

    // mut self via a struct field that is a container (index-assign through self.field).
    check_int("mut_self_field_array",
        "struct Buf { xs: Array[Int] }\n trait Set0 { fn set0(mut self, v: Int) -> Int }\n"
        " impl Set0 for Buf { fn set0(mut self, v: Int) -> Int { self.xs[0] = v\n self.xs[0] } }\n"
        " let mut b = Buf { xs: array(2, 0) }\n let r = set0(b, 7)\n r + b.xs[0]", 14);

    // -- negatives: `mut` on a non-aggregate is a checker error (no mutable primitive params) --
    check_true("mut_reject_int_param",
        cg_check_fails("fn f(mut x: Int) -> Int { x + 1 }\n f(3)"));
    check_true("mut_reject_string_param",
        cg_check_fails("fn f(mut s: String) -> String { s }\n f(\"a\")"));
    check_true("mut_reject_self_int",
        cg_check_fails("trait D { fn d(mut self) -> Int }\n impl D for Int { fn d(mut self) -> Int { self * 2 } }\n d(21)"));
    check_true("mut_reject_enum_param",
        cg_check_fails("enum E { A, B }\n fn f(mut e: E) -> Int { 0 }\n f(A)"));
    check_true("mut_reject_list_param",
        cg_check_fails("fn f(mut l: List[Int]) -> Int { 0 }\n f([1, 2])"));

    // -- H5 baseline intact: WITHOUT `mut self`, field assignment through `self` stays rejected --
    check_true("mut_h5_self_immutable",
        cg_check_fails("struct P { x: Int }\n trait S { fn m(self) -> Int }\n"
                       " impl S for P { fn m(self) -> Int { self.x = 1\n self.x } }\n m(P { x: 0 })"));

    // -- Route B (mut-in-signature): a `mut` parameter is ENFORCED at the CALL SITE. Passing a NAMED
    //    binding to a mutating builtin/fn/`mut self` method now requires that binding be `mut` --
    //    unifying push with the `a[i] = v` rule. A temporary/rvalue argument stays exempt. --

    // A mutating builtin (`push`) demands a `mut` receiver binding; the non-`mut` form is rejected.
    check_int("mut_callsite_push_ok",   "let mut v = vec()\n push(v, 1)\n push(v, 2)\n len(v)", 2);
    check_true("mut_callsite_push_reject",
        cg_check_fails("let v = vec()\n push(v, 1)\n len(v)"));
    // A fresh temporary is trivially the caller's to mutate -> no binding, no `mut` needed.
    check_int("mut_callsite_temp_ok",   "len(push(vec(), 1))", 1);
    // A user fn that mutates a param must declare it `mut` (else the push inside is rejected)...
    check_true("mut_callsite_param_needs_mut",
        cg_check_fails("fn addTo(v: Vec[Int]) -> Int { push(v, 1)\n len(v) }\n let mut v = vec()\n addTo(v)"));
    // ...and once it does, its NAMED-binding callers must pass a `mut` binding (propagation up the chain).
    check_int("mut_callsite_param_ok",
        "fn addTo(mut v: Vec[Int]) -> Int { push(v, 1)\n len(v) }\n let mut v = vec()\n addTo(v)", 1);
    check_true("mut_callsite_caller_needs_mut",
        cg_check_fails("fn addTo(mut v: Vec[Int]) -> Int { push(v, 1)\n len(v) }\n let v = vec()\n addTo(v)"));
    // A `mut self` method on a non-`mut` receiver binding is rejected too.
    check_true("mut_callsite_self_reject",
        cg_check_fails("struct C { n: Int }\n trait B { fn bump(mut self) -> Int }\n"
                       " impl B for C { fn bump(mut self) -> Int { self.n = self.n + 1\n self.n } }\n"
                       " let c = C { n: 0 }\n bump(c)"));

    // -- Conformance: an impl may DROP a trait's `mut` but never ADD one. Callers enforce the rule
    //    above from the TRAIT signature, so an impl-only `mut` let `f(s)` mutate a non-`mut` `s`. --
    check_true("mut_impl_adds_mut_self_reject",
        check_has("struct C { n: Int }\n trait B { fn bump(self) -> Int }\n"
                  " impl B for C { fn bump(mut self) -> Int { self.n = self.n + 1\n self.n } }",
                  "takes `mut self` but trait 'B' declares `self`"));
    check_true("mut_impl_adds_mut_param_reject",
        check_has("struct S { x: Int }\n trait G { fn g(self, v: Vec[Int]) -> Int }\n"
                  " impl G for S { fn g(self, mut v: Vec[Int]) -> Int { push(v, 9)\n len(v) } }",
                  "parameter 'v' is `mut` but trait 'G' declares it without `mut`"));
    check_true("mut_impl_adds_mut_default_override_reject",
        check_has("struct C { n: Int }\n trait B { fn bump(self) -> Int { 0 } }\n"
                  " impl B for C { fn bump(mut self) -> Int { self.n = 1\n self.n } }",
                  "takes `mut self` but trait 'B' declares `self`"));
    check_true("mut_impl_adds_mut_parametric_reject",
        check_has("struct S { x: Int }\n trait Put[T] { fn put(self, x: T) -> Int }\n"
                  " impl Put[Int] for S { fn put(mut self, x: Int) -> Int { self.x = x\n x } }",
                  "takes `mut self` but trait 'Put' declares `self`"));
    check_true("mut_impl_adds_mut_blanket_reject",
        check_has("trait N { fn nm(self) -> Int }\n trait B { fn bump(self) -> Int }\n"
                  " impl[T: N] B for T { fn bump(mut self) -> Int { nm(self) } }",
                  "takes `mut self` but trait 'B' declares `self`"));
    // The original hole, end to end: this used to compile and print 2.
    check_true("mut_impl_adds_mut_hole_closed",
        cg_check_fails("struct S { x: Int }\n trait T { fn f(self) -> () }\n"
                       " impl T for S { fn f(mut self) -> () { self.x = 2 } }\n"
                       " let s = S { x: 1 }\n f(s)\n s.x"));
    // Both sides `mut` stays legal; dropping the trait's `mut` is legal but callers still need `mut`.
    check_int("mut_impl_param_both_mut_ok",
        "struct S { x: Int }\n trait G { fn g(self, mut v: Vec[Int]) -> Int }\n"
        " impl G for S { fn g(self, mut v: Vec[Int]) -> Int { push(v, 9)\n len(v) } }\n"
        " let mut v = vec()\n g(S { x: 1 }, v)", 1);
    check_int("mut_impl_drops_mut_ok",
        "struct C { n: Int }\n trait B { fn peek(mut self) -> Int }\n"
        " impl B for C { fn peek(self) -> Int { self.n } }\n let mut c = C { n: 7 }\n peek(c)", 7);
    check_true("mut_impl_drops_mut_caller_still_needs_mut",
        cg_check_fails("struct C { n: Int }\n trait B { fn peek(mut self) -> Int }\n"
                       " impl B for C { fn peek(self) -> Int { self.n } }\n let c = C { n: 7 }\n peek(c)"));

    // -- The Iterator protocol (`next(mut self)`): a terminal consuming a NAMED iterator needs `mut`
    //    (exactly Rust's `let mut it`); passing a fresh source (rvalue) is exempt. Needs the prelude. --
    check_int_p("mut_iter_terminal_ok",     "let mut it = range(0, 5)\n sum(it)", 10);
    check_int_p("mut_iter_terminal_temp_ok", "sum(range(0, 5))", 10);
    check_true("mut_iter_terminal_reject",
        cg_check_fails_p("let it = range(0, 5)\n sum(it)"));
}

// Torture corpus: adversarial COMPOSITIONS (not features in isolation). Each program stacks
// several machinery-touching features so that a frame-sizing off-by-one or a missed GC root
// yields a WRONG VALUE (not a trap) -- which only the differential oracle can catch. Curated by
// reasoning about "what composition fools the measure_* walkers / the frame_size GC-roots
// contract", biased toward the shapes the lazy iterator pipeline leans on (closures,
// `mut self` cursors, tail-recursive `match` consumers, heap values live across loop safepoints).
// No expected value is written -- correctness IS "the VM and the reference interpreter agree".
void test_torture() {
    std::cout << "\n[test_torture]\n";

    // -- frame-sizing stress: match inside for inside while inside a closure, each level binding --
    check_same("torture_nested_loops_match",
        "fn ap(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n"
        " let mut total = 0\n let mut w = 0\n"
        " while w < 3 {\n"
        "   for x in [1, 2, 3] {\n"
        "     let tag = match x { 1 => 10, 2 => 20, _ => 30 }\n"
        "     total = total + ap(fn(n: Int) -> Int { n + tag }, w)\n"
        "   }\n"
        "   w = w + 1\n"
        " }\n total");
    check_same("torture_nested_for",
        "let mut total = 0\n"
        " for i in [1, 2, 3] { for j in [10, 20] { for k in [100, 200] { total = total + i * j + k } } }\n total");
    // Regression: a closure LITERAL directly as a call argument inside a `for` body, its result
    // consumed by the enclosing expression. This once crashed the CHECKER -- a use-after-free:
    // check_expr held `expected` BY REFERENCE into the scope vector, and inferring the closure
    // define()d its param, reallocating that vector and dangling the reference (fixed by taking
    // `expected` by value in Check.cpp). Capturing + non-capturing, List + Array below; the
    // differential oracle cross-checks each value.
    check_same("torture_closure_in_for",
        "let base = 100\n fn ap(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n"
        " let mut s = 0\n for i in [1, 2, 3, 4] { s = s + ap(fn(n: Int) -> Int { n + base }, i) }\n s");
    check_same("torture_closure_arg_for_noncap",
        "fn ap(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n"
        " let mut s = 0\n for i in [1, 2, 3, 4] { s = s + ap(fn(n: Int) -> Int { n + 1 }, i) }\n s");
    check_same("torture_closure_arg_for_array",
        "fn ap(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n"
        " let a = array(4, 5)\n let mut s = 0\n for i in a { s = s + ap(fn(n: Int) -> Int { n + 1 }, i) }\n s");
    // Lazy `for` over an Iterator / IntoIterator cursor (with break/continue and a dyn-field stage),
    // cross-checked against the reference interpreter (which drives next() on the runtime head).
    check_same("diff_foriter_direct",
        "struct Counter { i: Int, n: Int }\n"
        "impl Iterator[Int] for Counter { fn next(mut self) -> Option[Int] {\n"
        "  if self.i < self.n { let x = self.i  self.i = self.i + 1  Some(x) } else { None } } }\n"
        "let c = Counter { i: 0, n: 7 }\n let mut s = 0\n"
        "for x in c { if x == 2 { continue } if x >= 5 { break } s = s + x }\n s", true);
    check_same("diff_foriter_dyn_stage",
        "struct Counter { i: Int, n: Int }\n"
        "impl Iterator[Int] for Counter { fn next(mut self) -> Option[Int] {\n"
        "  if self.i < self.n { let x = self.i  self.i = self.i + 1  Some(x) } else { None } } }\n"
        "struct Doubler { up: dyn Iterator[Int] }\n"
        "impl Iterator[Int] for Doubler { fn next(mut self) -> Option[Int] {\n"
        "  match next(self.up) { Some(x) => Some(x * 2), None => None } } }\n"
        "let d = Doubler { up: Counter { i: 0, n: 5 } }\n let mut s = 0\n for x in d { s = s + x }\n s", true);
    check_same("diff_foriter_intoiter",
        "struct Bag { items: Vec[Int] }\n"
        "struct BagCur { v: Vec[Int], i: Int }\n"
        "impl Iterator[Int] for BagCur { fn next(mut self) -> Option[Int] {\n"
        "  if self.i < len(self.v) { let x = self.v[self.i]  self.i = self.i + 1  Some(x) } else { None } } }\n"
        "impl IntoIterator[Int] for Bag { fn intoIter(self) -> dyn Iterator[Int] { BagCur { v: self.items, i: 0 } } }\n"
        "let mut v: Vec[Int] = vec()\n push(v, 3)\n push(v, 4)\n push(v, 5)\n"
        "let b = Bag { items: v }\n let mut s = 0\n for x in b { s = s + x }\n for x in b { s = s + x }\n s", true);
    // The dissolved prelude combinators (range/map/filter/sum/collect over dyn Iterator), cross-checked
    // against the reference interpreter -- which now drives the Iterator protocol on the runtime head.
    check_same("diff_stream_pipeline",
        "sum(map(filter(range(0, 20), fn(x: Int) -> Bool { x % 2 == 0 }), fn(x: Int) -> Int { x * 3 }))", true);
    check_same("diff_stream_collect",
        "let w: Vec[Int] = collect(map(range(0, 5), fn(x: Int) -> Int { x + 1 }))\n w[0] + w[4]", true);
    check_same("diff_stream_take_drop",
        "sum(take(drop(range(0, 100), 3), 4))", true);   // 3+4+5+6 = 18
    // -- combinators round 2, cross-checked against the reference interpreter --
    check_same("diff_comb_takeWhile",
        "sum(takeWhile(range(0, 20), fn(x: Int) -> Bool { x < 5 }))", true);
    check_same("diff_comb_dropWhile",
        "count(dropWhile(range(0, 20), fn(x: Int) -> Bool { x < 5 }))", true);
    check_same("diff_comb_enumerate",
        "let mut s = 0\n for (i, x) in enumerate(range(3, 8)) { s = s + i * 10 + x }\n s", true);
    check_same("diff_comb_scan",
        "sum(scan(range(1, 6), 0, fn(a: Int, x: Int) -> Int { a + x }))", true);
    check_same("diff_comb_zip",
        "let mut s = 0\n for (a, b) in zip(range(0, 4), range(100, 103)) { s = s + a + b }\n s", true);
    check_same("diff_comb_chain",
        "sum(chain(range(0, 4), range(10, 14)))", true);
    check_same("diff_comb_flatMap",
        "sum(flatMap(range(0, 5), fn(x: Int) -> dyn Iterator[Int] { range(0, x) }))", true);
    check_same("diff_comb_reduce",
        "unwrapOr(reduce(range(1, 6), fn(a: Int, b: Int) -> Int { a + b }), 0)", true);
    check_same("diff_comb_min_max",
        "unwrapOr(min(chain(range(3, 6), range(0, 2))), -1) * 100 + unwrapOr(max(range(0, 9)), -1)", true);
    check_same("diff_comb_product", "product(range(1, 6))", true);   // 120
    check_same("diff_comb_position",
        "unwrapOr(position(range(20, 40), fn(x: Int) -> Bool { x == 25 }), -1)", true);
    check_same("diff_comb_contains",
        "if contains(range(0, 20), 13) { 1 } else { 0 }", true);
    check_same("diff_comb_partition",
        "let (ev, od) = partition(range(0, 10), fn(x: Int) -> Bool { x % 2 == 0 })\n len(ev) * 100 + len(od)", true);
    check_same("diff_comb_unzip",
        "let (is, xs) = unzip(enumerate(range(50, 55)))\n is[4] * 1000 + xs[0]", true);
    check_same("diff_comb_toMap",
        "let m = toMap(map(range(0, 5), fn(x: Int) -> (Int, Int) { (x, x + 100) }))\n m[2] + m[4]", true);
    check_same("diff_comb_stepBy",
        "sum(stepBy(range(0, 20), 3)) * 100 + count(stepBy(range(0, 20), 4))", true);
    check_same("diff_comb_inspect",
        "sum(inspect(range(0, 6), fn(x: Int) -> () {}))", true);
    check_same("diff_comb_dedup",
        "sum(dedup(intoIter(toVec([1, 1, 2, 3, 3, 3, 2, 2])))) * 100 + count(dedup(intoIter(toVec([4, 4, 4, 5]))))", true);
    check_same("diff_comb_chunks",
        "sum(flatMap(chunks(range(0, 10), 3), fn(c: Vec[Int]) -> dyn Iterator[Int] { intoIter(c) })) * 100 + count(chunks(range(0, 10), 3))", true);
    check_same("diff_comb_windows",
        "let v = collect(windows(range(0, 5), 2))\n len(v) * 100 + v[0][1] * 10 + v[3][0]", true);
    check_same("diff_comb_peekable",
        "let mut p = peekable(range(3, 9))\n let a = unwrapOr(p.peek(), -1)\n a * 1000 + sum(p)", true);
    // No-copy Map streaming (MAP_ITER_NEXT). Order-INDEPENDENT reductions only (VM = hash order,
    // oracle = insertion order): a for-loop value+key sum (Path A) and a lazy MapCursor fold (Path B).
    check_same("diff_map_for_sum",
        "let mut m: Map[Int, Int] = #{}\n m[1] = 10  m[2] = 20  m[3] = 30\n let mut s = 0\n for (k, v) in m { s = s + k + v }\n s", true);
    check_same("diff_map_intoiter_sum",
        "let mut m: Map[Int, Int] = #{}\n m[7] = 1  m[8] = 2  m[9] = 3\n fold(intoIter(m), 0, fn(a: Int, kv: (Int, Int)) -> Int { let (k, v) = kv  a + k + v })", true);
    // String streaming (split / lines): pure prelude, cross-checked vs the reference interpreter.
    check_same("diff_split_empties",
        "let v = collect(split(\"a,,b,\", \",\"))  len(v) * 10 + len(v[1]) + len(v[3])", true);
    check_same("diff_split_roundtrip", "len(join(split(\"x::y::z\", \"::\"), \"::\"))", true);
    check_same("diff_lines_terminator", "count(lines(\"p\\nq\\n\"))", true);
    check_same("diff_lines_crlf",
        "let v = collect(lines(\"a\\r\\nbb\\r\\n\"))  len(v) * 10 + len(v[0]) + len(v[1])", true);
    check_same("diff_lines_for",
        "let mut n = 0\n for ln in lines(\"a\\nbb\\nccc\") { n = n + len(ln) }\n n", true);
    check_same("diff_words_collapse",
        "count(words(\"  a  bb   ccc \")) * 10 + count(split(\"a  b\", \" \"))", true);
    check_same("diff_words_for",
        "let mut n = 0\n for w in words(\" \\t a bb \\n ccc \") { n = n + len(w) }\n n", true);
    // Regression: pop() on a field argument (frame under-measure fixed in builtin_extra_slots).
    check_same("diff_pop_field",
        "struct Stk { xs: Vec[Int] }\n let mut s = Stk { xs: push(push(vec(), 3), 4) }\n"
        " match pop(s.xs) { Some(x) => x, None => 0 - 1 }", true);
    // Regression (found by the generator): a lambda body that references a BUILTIN (`len`/`array`)
    // while also capturing an outer local. The capture analysis's `is_global_name` omitted builtins,
    // so it tried to capture `len` as a local and aborted codegen. Fixed by adding builtins to the
    // globals set. The lambda here uses len+array AND captures `n`.
    check_same("torture_lambda_uses_builtin",
        "fn ap(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n"
        " let n = 3\n ap(fn(k: Int) -> Int { len(array(5, n)) + k }, 7)");
    check_same("torture_deep_if_locals",
        "let a = 1\n let b = 2\n let c = 3\n let d = 4\n let e = 5\n let f = 6\n"
        " if a < b { if c < d { if e < f { let g = a + b + c\n let h = d + e + f\n g * h } else { 0 } } else { 0 } } else { 0 }");
    check_same("torture_match_nested_arms",
        "enum T { A(Int), B(Int, Int), C }\n"
        " fn f(t: T) -> Int { match t { A(x) => match x { 0 => 100, _ => x * 2 }, B(x, y) => x + y, C => match 1 { _ => 7 } } }\n"
        " f(A(0)) + f(A(5)) + f(B(3, 4)) + f(C)");
    // Regression (found by the generator, seed 34): a `match` whose SCRUTINEE is itself a match
    // stacks scrutinee locals, but the measure_* walk measured the scrutinee at `base` instead of
    // `base+1` (the scrutinee local is allocated first) -> frame under-count -> emitter self-check
    // abort. Fixed in the Match measure. Nested 3 deep here.
    check_same("torture_match_scrutinee_nested",
        "let x: Int = (match (match (match 0 { _ => 7 }) { _ => 8 }) { _ => 9 })\n x");

    // -- GC-roots stress: a heap value kept live across an allocation safepoint INSIDE a loop --
    check_same("torture_gc_string_loop",
        "let mut s = \"\"\n let mut i = 0\n while i < 40 { s = s + \"ab\"\n i = i + 1 }\n len(toBytes(s))");
    check_same("torture_gc_vec_alloc_loop",
        "let mut v = vec()\n let mut i = 0\n"
        " while i < 30 { push(v, i)\n let _s = \"x\" + toString(i)\n i = i + 1 }\n len(v)");
    check_same("torture_gc_vec_of_structs",
        "struct P { x: Int }\n let mut v = vec()\n let mut i = 0\n"
        " while i < 20 { push(v, P { x: i })\n i = i + 1 }\n"
        " let mut s = 0\n for p in v { s = s + p.x }\n s");
    check_same("torture_gc_map_loop",
        "let mut m = #{ 0 => 0 }\n let mut i = 1\n while i < 20 { m[i] = i * i\n i = i + 1 }\n"
        " let mut s = 0\n for (k, v) in m { s = s + v }\n s");
    check_same("torture_gc_struct_array_field",
        "struct Buf { xs: Array[Int], sum: Int }\n let mut b = Buf { xs: array(10, 1), sum: 0 }\n"
        " let mut i = 0\n while i < 10 { let _s = \"v\" + toString(i)\n b.xs[i] = i\n b.sum = b.sum + b.xs[i]\n i = i + 1 }\n b.sum");
    check_same("torture_gc_return_aggregate",
        "struct P { x: Int, y: Int }\n let mut v = vec()\n let mut i = 0\n"
        " while i < 8 { push(v, P { x: i, y: i * i })\n i = i + 1 }\n v");

    // -- `mut self` cursor + closures + aliasing: the exact struct-cursor shape the Iterator stages use --
    check_same("torture_cursor_while",
        "struct Cur { data: Array[Int], i: Int }\n trait Nxt { fn next(mut self) -> Int }\n"
        " impl Nxt for Cur { fn next(mut self) -> Int { let v = self.data[self.i]\n self.i = self.i + 1\n v } }\n"
        " let mut a = array(4, 0)\n a[0] = 1\n a[1] = 2\n a[2] = 3\n a[3] = 4\n"
        " let mut c = Cur { data: a, i: 0 }\n let mut s = 0\n let mut k = 0\n while k < 4 { s = s + next(c)\n k = k + 1 }\n s");
    check_same("torture_cursor_closure",
        "struct Cur { data: Array[Int], i: Int }\n trait Nxt { fn next(mut self) -> Int }\n"
        " impl Nxt for Cur { fn next(mut self) -> Int { let v = self.data[self.i]\n self.i = self.i + 1\n v } }\n"
        " fn ap(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n"
        " let mut a = array(3, 0)\n a[0] = 5\n a[1] = 6\n a[2] = 7\n let mut c = Cur { data: a, i: 0 }\n"
        " ap(fn(n: Int) -> Int { next(c) + n }, 0) + ap(fn(n: Int) -> Int { next(c) + n }, 0) + ap(fn(n: Int) -> Int { next(c) + n }, 0)");
    check_same("torture_two_cursors",
        "struct Cur { data: Array[Int], i: Int }\n trait Nxt { fn next(mut self) -> Int }\n"
        " impl Nxt for Cur { fn next(mut self) -> Int { let v = self.data[self.i]\n self.i = self.i + 1\n v } }\n"
        " let mut a = array(3, 0)\n a[0] = 1\n a[1] = 2\n a[2] = 3\n"
        " let mut b = array(3, 0)\n b[0] = 10\n b[1] = 20\n b[2] = 30\n"
        " let mut p = Cur { data: a, i: 0 }\n let mut q = Cur { data: b, i: 0 }\n"
        " let mut s = 0\n let mut k = 0\n while k < 3 { s = s + next(p) * next(q)\n k = k + 1 }\n s");

    // -- TCO carrying HEAP values. The differential versions stay at SMALL N: the reference
    // oracle is a tree-walker that recurses in C++, so a deep `check_same` would overflow the
    // ORACLE's native stack (not the VM's -- the VM TCO-flattens it). The deep O(1)-native-stack
    // property under allocation pressure is then verified VM-only via check_int (no oracle). --
    check_same("torture_tco_struct_acc_sm",
        "struct P { x: Int }\n fn go(n: Int, acc: P) -> P { if n == 0 { acc } else { go(n - 1, P { x: acc.x + n }) } }\n"
        " go(30, P { x: 0 }).x");
    check_int("torture_tco_struct_acc_deep",
        "struct P { x: Int }\n fn go(n: Int, acc: P) -> P { if n == 0 { acc } else { go(n - 1, P { x: acc.x + n }) } }\n"
        " go(50000, P { x: 0 }).x", 1250025000);   // sum 1..50000
    check_same("torture_tco_match_tuple_sm",
        "fn go(n: Int, t: (Int, Int)) -> (Int, Int) { match t { (a, b) => if n == 0 { t } else { go(n - 1, (a + n, b + 1)) } } }\n"
        " let r = go(30, (0, 0))\n match r { (a, b) => a + b }");
    check_int("torture_tco_match_tuple_deep",
        "fn go(n: Int, t: (Int, Int)) -> (Int, Int) { match t { (a, b) => if n == 0 { t } else { go(n - 1, (a + n, b + 1)) } } }\n"
        " let r = go(100000, (0, 0))\n match r { (a, b) => a + b }", 5000150000);   // sum 1..100000 + 100000
    check_same("torture_tco_trait_method_sm",
        "trait Cnt { fn cnt(self, acc: Int) -> Int }\n struct N { v: Int }\n"
        " impl Cnt for N { fn cnt(self, acc: Int) -> Int { if self.v == 0 { acc } else { cnt(N { v: self.v - 1 }, acc + 1) } } }\n"
        " cnt(N { v: 30 }, 0)");
    check_int("torture_tco_trait_method_deep",
        "trait Cnt { fn cnt(self, acc: Int) -> Int }\n struct N { v: Int }\n"
        " impl Cnt for N { fn cnt(self, acc: Int) -> Int { if self.v == 0 { acc } else { cnt(N { v: self.v - 1 }, acc + 1) } } }\n"
        " cnt(N { v: 100000 }, 0)", 100000);

    // -- generics + devirtualization + traits + supertraits + blanket, composed --
    check_same("torture_generic_hof_twice",
        "fn twice[T](f: fn(T) -> T, x: T) -> T { f(f(x)) }\n twice(fn(n: Int) -> Int { n + 3 }, 10) + twice(fn(n: Int) -> Int { n * 2 }, 5)");
    check_same("torture_supertrait",
        "trait A { fn a(self) -> Int }\n trait B: A { fn b(self) -> Int }\n struct P { v: Int }\n"
        " impl A for P { fn a(self) -> Int { self.v } }\n impl B for P { fn b(self) -> Int { a(self) + 1 } }\n"
        " b(P { v: 41 }) + b(P { v: 7 })");
    check_same("torture_blanket_loop",
        "trait Show { fn shw(self) -> Int }\n trait Dbg { fn dbg(self) -> Int }\n"
        " impl[T: Show] Dbg for T { fn dbg(self) -> Int { shw(self) + 1 } }\n"
        " struct P { v: Int }\n impl Show for P { fn shw(self) -> Int { self.v } }\n"
        " let ps = [P { v: 1 }, P { v: 2 }, P { v: 3 }]\n let mut s = 0\n for p in ps { s = s + dbg(p) }\n s");
    check_same("torture_trait_enum_match",
        "enum Sh { Sq(Int), Ci(Int) }\n trait Area { fn area(self) -> Int }\n"
        " impl Area for Sh { fn area(self) -> Int { match self { Sq(s) => s * s, Ci(r) => r * r * 3 } } }\n"
        " let mut s = 0\n for x in [Sq(2), Ci(3), Sq(4)] { s = s + area(x) }\n s");
    check_same("torture_nested_enum_struct_field",
        "struct P { x: Int, y: Int }\n enum E { L(P), R(Int) }\n"
        " fn f(e: E) -> Int { match e { L(p) => p.x + p.y, R(n) => n } }\n"
        " f(L(P { x: 4, y: 5 })) + f(R(3)) + f(L(P { x: 1, y: 1 }))");

    // -- everything at once: a mut-self cursor feeding a trait-dispatched fold over a built vec --
    check_same("torture_cursor_feeds_trait_fold",
        "struct Cur { data: Array[Int], i: Int }\n trait Nxt { fn next(mut self) -> Int }\n"
        " impl Nxt for Cur { fn next(mut self) -> Int { let v = self.data[self.i]\n self.i = self.i + 1\n v } }\n"
        " trait Wt { fn wt(self) -> Int }\n struct Item { v: Int }\n impl Wt for Item { fn wt(self) -> Int { self.v * self.v } }\n"
        " let mut a = array(5, 0)\n a[0] = 1\n a[1] = 2\n a[2] = 3\n a[3] = 4\n a[4] = 5\n"
        " let mut c = Cur { data: a, i: 0 }\n let mut items = vec()\n let mut k = 0\n"
        " while k < 5 { push(items, Item { v: next(c) })\n k = k + 1 }\n"
        " let mut s = 0\n for it in items { s = s + wt(it) }\n s");

    // -- regression: `for` over a CONTAINER expression that itself needs a local (a match/if element).
    // compile_for allocates the loop-var slots + the destination local (cur/mapl/arr) BEFORE compiling
    // the iterable into it, so the iterable's own transient locals sit above them; measure_for used to
    // measure the iterable at plain `base`, under-counting the frame by nvars+1 (nvars+3 for Iterable).
    // Found by the generator (31/1000 seeds). Fixed in measure_expr(For)'s `pre_iter` offset. --
    check_same("torture_for_list_match_elem",   // List path (cursor)
        "struct S1 { f0: Int }\n let mut a = 0\n"
        " for x in [S1 { f0: (match 1 { 0 => 10, _ => 20 }) }, S1 { f0: 5 }] { a = a + x.f0 }\n a");
    check_same("torture_for_array_match_elem",  // Array/Vec/Bytes path (arr/len/idx)
        "let mut a = 0\n for x in array(3, (match 1 { 0 => 10, _ => 7 })) { a = a + x }\n a");
    check_same("torture_for_map_match_elem",    // Map path (mapl/karr/varr/len/idx)
        "let mut a = 0\n for (k, v) in #{ 0 => (match 1 { _ => 9 }), 1 => 4 } { a = a + v }\n a");
    check_same("torture_for_list_match_deep_base",  // the failing shape at a deeper local base
        "struct S1 { f0: Int }\n let b = 1\n let c = 2\n let d = 3\n let mut a = 0\n"
        " for x in [S1 { f0: (match b { 0 => c, _ => d }) }] { a = a + x.f0 }\n a");

    // -- Slice B (full pattern surface). The generator emitted a bare struct-literal MATCH SCRUTINEE
    // (`match S { .. } { .. }`) which collides with the arm `{` (the Rust-style restriction) -- a
    // generator syntax bug, fixed by parenthesizing the scrutinee. These pin the compiler+oracle on
    // the parenthesized-scrutinee, guarded, nested-pattern, and destructuring-let shapes the corpus
    // previously only exercised over VARIABLE scrutinees. --
    check_same("torture_match_structlit_scrut",
        "struct S1 { f0: Int, f1: Double }\n"
        " (match (S1 { f0: 7, f1: 2.5 }) { S1 { f0: a, f1: b } => a })");
    check_same("torture_match_guard_bind",
        "let n = 9\n (match n { g if g > 5 => g * 2, _ => 0 })");
    check_same("torture_match_nested_struct_in_tuple",
        "struct S1 { f0: Int, f1: Int }\n"
        " (match (S1 { f0: 3, f1: 4 }, 5) { (S1 { f0: a, f1: b }, c) => a + b + c })");
    check_same("torture_destructure_let_struct",
        "struct S1 { f0: Int, f1: Double }\n let S1 { f0: a, f1: b } = S1 { f0: 8, f1: 1.5 }\n a");
    check_same("torture_destructure_let_tuple",
        "let (a, b) = (11, 22)\n a * 10 + b");

    // -- Slice C (mut self / mut param): mutate a heap struct in place through a mut param and a
    // devirtualized mut-self method, then OBSERVE the caller-visible effect. Pins the generated shape. --
    check_same("torture_mut_param_observe",
        "struct S1 { f0: Int }\n fn setf(mut p: S1, v: Int) -> Int { p.f0 = v\n v }\n"
        " let mut s = S1 { f0: 1 }\n setf(s, 99)\n s.f0");
    check_same("torture_mut_self_bump_observe",
        "struct S1 { f0: Int }\n trait Bump { fn bump(mut self) -> Int }\n"
        " impl Bump for S1 { fn bump(mut self) -> Int { self.f0 = self.f0 + 1\n self.f0 } }\n"
        " let mut s = S1 { f0: 10 }\n let mut a = 0\n for i in [0, 1, 2] { bump(s)\n a = a + s.f0 }\n a");

    // -- Slice F (generics breadth). A TAIL call whose args are bare locals FOLLOWED by an aggregate
    // literal with a temp-needing element (`take(a, a, (S1 { f0: 5 }, 7))`). The tail path forces every
    // arg into a held temp, so the aggregate stacks above the two held local-copies; need_call used to
    // count only na>0 args as held, under-measuring the frame -> emitter self-check CodegenFail. Found
    // by the property generator (seed 48474); fixed by need_call's tail-safe `++held` bound. --
    check_same("torture_tail_call_aggregate_arg",
        "struct S1 { f0: Int }\n fn take(a: Int, b: Int, p: (S1, Int)) -> Int { a + b + (p.0).f0 + p.1 }\n"
        " fn g(a: Int) -> Int { take(a, a, (S1 { f0: 5 }, 7)) }\n g(3)");

    // -- Struct-literal SHORTHAND inside a FUNCTION body. `FieldInit::value` is null for shorthand,
    // and the inliner's body scan dereferenced it unguarded -> a hard CRASH of the compiler on any
    // fn containing `S { x }`. It never fired because both guide-claim fixtures use shorthand only
    // at TOP LEVEL, which build_inline_targets does not scan. Mixed with an explicit field and with
    // `..base`, so a future scan that walks the field list learns all three shapes at once.
    // These are value checks against the RefEval oracle,
    // so they are meaningful in BOTH modes; run the suite with `--sroa` to exercise the transform.
    // Each names the escape shape it locks, because every one of them would otherwise dissolve a
    // binding that is later read as a whole object -- a silent wrong value, not a crash. --

    // The shape that PAYS: literal init, every use a field read.
    check_same("torture_sroa_field_reads_only",
        "struct S3 { f0: Int, f1: Int, f2: Int }\n"
        " fn q() -> Int { let p = S3 { f0: 3, f1: 4, f2: 5 }\n p.f0 * p.f0 + p.f1 * p.f1 + p.f2 }\n q()");

    // ESCAPE: a whole-value call argument.
    check_same("torture_sroa_escape_call_arg",
        "struct S3 { f0: Int, f1: Int, f2: Int }\n fn sum(v: S3) -> Int { v.f0 + v.f1 + v.f2 }\n"
        " fn q() -> Int { let p = S3 { f0: 1, f1: 2, f2: 3 }\n sum(p) + p.f0 }\n q()");

    // ESCAPE: a METHOD receiver. Same AST node as a field read -- cg_field_exists is the only
    // thing separating them, so a scan that keys on the node shape alone gets this wrong.
    check_same("torture_sroa_escape_method_receiver",
        "struct S3 { f0: Int, f1: Int }\n impl S3 { fn tot(self) -> Int { self.f0 + self.f1 } }\n"
        " fn q() -> Int { let p = S3 { f0: 6, f1: 7 }\n p.tot() + p.f0 }\n q()");

    // ESCAPE: struct-literal SHORTHAND. Carries NO Ident node -- compile_struct_lit resolves the
    // local by the FIELD's name, so an Ident-based scan misses the use completely.
    check_same("torture_sroa_escape_shorthand",
        "struct S3 { f0: Int, f1: Int }\n struct H { p: S3 }\n"
        " fn q() -> Int { let p = S3 { f0: 8, f1: 9 }\n let h = H { p }\n h.p.f0 + p.f1 }\n q()");

    // ESCAPE: read inside a LAMBDA body. `p.f0` there LOOKS like a field read, but the body is
    // lifted into its own function where `p` arrives as a by-value capture -- emit_capture_source
    // MOVs the whole register. Found by the dissolved-binding guard on the probe's first run.
    check_same("torture_sroa_escape_lambda_capture",
        "struct S3 { f0: Int, f1: Int }\n"
        " fn q() -> Int { let p = S3 { f0: 5, f1: 6 }\n let g = fn() -> Int { p.f0 + p.f1 }\n g() }\n q()");

    // ESCAPE: a WRITE through the binding. `p.f0 = v` roots at `p`, so the read exemption for
    // `p.f0` must not apply to an assignment TARGET.
    check_same("torture_sroa_escape_field_write",
        "struct S3 { f0: Int, f1: Int }\n"
        " fn q() -> Int { let mut p = S3 { f0: 1, f1: 2 }\n p.f0 = 40\n p.f0 + p.f1 }\n q()");

    // ESCAPE: structural `==`. Deliberately not a special rule -- under all-or-nothing it is
    // simply a use that is not a field read, which is what keeps EQ_DEEP unable to observe SROA.
    check_same("torture_sroa_escape_deep_eq",
        "struct S3 { f0: Int, f1: Int }\n"
        " fn q() -> Int { let p = S3 { f0: 1, f1: 2 }\n let r = S3 { f0: 1, f1: 2 }\n"
        " if p == r { p.f0 } else { p.f1 } }\n q()");

    // SHADOWING: the uses after a same-named rebind belong to the INNER binding, so both may
    // dissolve independently. add_pattern_names is what makes that come out right.
    check_same("torture_sroa_shadowed_rebind",
        "struct S3 { f0: Int, f1: Int }\n"
        " fn q() -> Int { let p = S3 { f0: 1, f1: 2 }\n let a = p.f0\n"
        " let p = S3 { f0: 30, f1: 40 }\n a + p.f0 + p.f1 }\n q()");

    // A dissolved binding inside a LOOP body: the slots are reclaimed and reissued per iteration,
    // so this is where a scope_.resize that forgot the field registers would surface.
    check_same("torture_sroa_in_loop_body",
        "struct S3 { f0: Int, f1: Int }\n"
        " fn q() -> Int { let mut a = 0\n for i in [1, 2, 3, 4] { let p = S3 { f0: i, f1: i * 2 }\n"
        " a = a + p.f0 + p.f1 }\n a }\n q()");

    // Int initializers widening into DOUBLE fields must still coerce once dissolved (the per-field
    // field_double rule of compile_struct_lit, which compile_into_dissolved has to reproduce).
    check_same("torture_sroa_double_field_widen",
        "struct SD { f0: Double, f1: Double }\n"
        " fn q() -> Double { let p = SD { f0: 3, f1: 4 }\n p.f0 * p.f0 + p.f1 * p.f1 }\n q()");

    // -- SROA slices S2 (an expansion as a dissolvable PRODUCER) and S3 (a dissolved argument
    // bound to a FIELD-ONLY parameter). Both are only reachable with inlining on, which is the
    // shipping default; `--sroa` turns the transform itself on. --

    // S2: the initializer is a CALL whose callee body ENDS in a struct literal, so the expansion
    // writes the caller's field registers and the callee's result object is never built.
    check_same("torture_sroa_producer_is_expansion",
        "struct S3 { f0: Int, f1: Int, f2: Int }\n"
        " fn mk(a: Int) -> S3 { S3 { f0: a, f1: a * 2, f2: a * 3 } }\n"
        " fn q() -> Int { let p = mk(4)\n p.f0 + p.f1 + p.f2 }\n q()");

    // S2's EXPLODE fallback: the callee has an early `return`, so it has two exits and cannot feed
    // N destinations. The binding must fall back to an ordinary object, not miscompile.
    check_same("torture_sroa_producer_two_exits",
        "struct S3 { f0: Int, f1: Int }\n"
        " fn mk(a: Int) -> S3 { if a < 0 { return S3 { f0: 0, f1: 0 } }\n S3 { f0: a, f1: a + 1 } }\n"
        " fn q() -> Int { let p = mk(7)\n p.f0 + p.f1 }\n q()");

    // S3: the dissolved binding is passed to a method whose `self` is only ever field-read, so the
    // parameter is bound to the same field registers instead of an object being rebuilt.
    check_same("torture_sroa_param_field_only",
        "struct S3 { f0: Int, f1: Int, f2: Int }\n"
        " impl S3 { fn tot(self) -> Int { self.f0 + self.f1 + self.f2 } }\n"
        " fn q() -> Int { let p = S3 { f0: 1, f1: 2, f2: 3 }\n p.tot() + p.f0 }\n q()");

    // S3 in TAIL position. `p.tot()` as the body's tail takes the TCO path, so the expansion is
    // declined and the binding must be REBUILT there. This is the case the dissolved-binding guard
    // caught; without the rebuild it is a hard compile error, with a wrong rebuild a wrong value.
    check_same("torture_sroa_param_tail_position",
        "struct S3 { f0: Int, f1: Int }\n"
        " impl S3 { fn tot(self) -> Int { self.f0 + self.f1 } }\n"
        " fn q() -> Int { let p = S3 { f0: 11, f1: 12 }\n p.tot() }\n q()");

    // S2+S3 together, the measured shape: build by expansion, then consume by expansion.
    check_same("torture_sroa_producer_then_consumer",
        "struct V { f0: Int, f1: Int }\n"
        " impl V { fn add(self, b: V) -> V { V { f0: self.f0 + b.f0, f1: self.f1 + b.f1 } }\n"
        "          fn dot(self, b: V) -> Int { self.f0 * b.f0 + self.f1 * b.f1 } }\n"
        " fn q() -> Int { let a = V { f0: 1, f1: 2 }\n let b = V { f0: 3, f1: 4 }\n"
        " let c = a.add(b)\n c.dot(c) + c.f0 }\n q()");

    // A parameter that is NOT field-only (it is passed on as a whole value) must keep the binding
    // undissolved -- the pre-pass reads params_field_only, so this locks the negative direction.
    check_same("torture_sroa_param_not_field_only",
        "struct S3 { f0: Int, f1: Int }\n fn keep(v: S3) -> S3 { v }\n"
        " impl S3 { fn via(self) -> Int { (keep(self)).f0 } }\n"
        " fn q() -> Int { let p = S3 { f0: 5, f1: 6 }\n p.via() + p.f1 }\n q()");

    check_same("torture_struct_shorthand_in_fn",
        "struct S2 { f0: Int, f1: Int }\n"
        " fn mk(f0: Int, f1: Int) -> S2 { S2 { f0, f1 } }\n"
        " fn mk2(f0: Int) -> S2 { S2 { f0, f1: 9 } }\n"
        " fn upd(b: S2, f0: Int) -> S2 { S2 { f0, ..b } }\n"
        " let a = mk(1, 2)\n let b = mk2(3)\n let c = upd(a, 7)\n"
        " a.f0 + a.f1 + b.f0 + b.f1 + c.f0 + c.f1");
}

// The shrinker (Slice 2c). Its reduction ENGINE is tested against simple substring predicates
// (independent of run_diff, so it is exercised even when no live compiler bug exists), plus one
// end-to-end check that it integrates with run_diff without wandering off a passing program.
void test_shrinker() {
    std::cout << "\n[test_shrinker]\n";

    // One required line: deleting every other line must survive, leaving just the marker.
    check_true("shrink_isolates_marker",
        shrink_lines("line one\nMARKER here\nline three\nline four",
                     [](const std::string& s) { return s.find("MARKER") != std::string::npos; })
            == "MARKER here");

    // Two required lines: both must survive (removing either loses the failure).
    check_true("shrink_keeps_both_needles",
        shrink_lines("a\nb\nNEEDLE1\nc\nNEEDLE2\nd",
                     [](const std::string& s) {
                         return s.find("NEEDLE1") != std::string::npos &&
                                s.find("NEEDLE2") != std::string::npos;
                     })
            == "NEEDLE1\nNEEDLE2");

    // A predicate false on the input => nothing "fails" => the engine returns it unchanged.
    check_true("shrink_noop_when_never_fails",
        shrink_lines("x\ny\nz", [](const std::string&) { return false; }) == "x\ny\nz");

    // End-to-end: shrink_failure over an AGREEING program is a no-op (run_diff never returns the
    // wanted failure kind, so no line is removed) -- proves the run_diff wiring doesn't wander.
    const std::string ok = "let a = 1\n let b = 2\n a + b";
    check_true("shrink_failure_noop_on_pass",
        shrink_failure(ok, false, DiffOutcome::Disagree) == ok);
}

// Property testing: sweep a range of seeds, generate a well-typed program from each, and run
// it through the same differential core the corpus uses. Two distinct failure signals:
//   - CompileFail  = a GENERATOR bug (it emitted something the checker rejects -- it is meant to
//                    be well-typed by construction), reported with its seed + program.
//   - Disagree     = a real COMPILER/oracle bug (VM value/output/fault != reference interpreter).
// Agree/Unsupported are fine. A clean sweep counts as ONE suite pass; each bug is a failure.
void test_generated(uint32_t lo, uint32_t hi, bool with_prelude = false) {
    std::cout << "\n[test_generated" << (with_prelude ? " +prelude" : "") << "] seeds "
              << lo << ".." << (hi - 1) << "\n";
    int agree = 0, skip = 0, gen_bug = 0, compiler_bug = 0, oracle_gap = 0;
    // A skip is the oracle declining to model a program -- honest, but INVISIBLE as a bare count:
    // "98 skipped" reads identically whether those are the Map hash-order dump (deliberately outside
    // the oracle's remit) or a whole operator family the oracle silently stopped checking. The reason
    // already rides along in `o.detail` and was simply dropped here; tallying it turns the silence
    // into a named line, so a NEW reason appearing is something a reader can actually notice.
    std::unordered_map<std::string, int> skip_reasons;
    for (uint32_t seed = lo; seed < hi; ++seed) {
        const std::string src = gen::generate(seed, with_prelude);
        const DiffOutcome o = run_diff(src, with_prelude);
        switch (o.kind) {
            case DiffOutcome::Agree:       ++agree; break;
            case DiffOutcome::Unsupported: ++skip; ++skip_reasons[o.detail]; break;
            case DiffOutcome::OracleGap: {   // the oracle lacks it => the differential covers nothing here
                ++oracle_gap;
                const std::string mini = shrink_failure(src, with_prelude, DiffOutcome::OracleGap);
                std::cout << "  ORACLE-GAP  seed=" << seed << ": " << o.detail << "\n"
                          << "    --- minimized (" << split_lines(mini).size() << " lines) ---\n"
                          << mini << "\n    ---------------\n";
                break;
            }
            case DiffOutcome::CheckReject:   // ill-typed => the generator emitted bad code
                ++gen_bug;
                std::cout << "  GEN-BUG    seed=" << seed << " (does not type-check: " << o.detail << ")\n"
                          << "    --- program ---\n" << src << "    ---------------\n";
                break;
            case DiffOutcome::CodegenFail: {  // well-typed but lowering failed => a COMPILER bug
                ++compiler_bug;
                const std::string mini = shrink_failure(src, with_prelude, DiffOutcome::CodegenFail);
                std::cout << "  COMPILER-BUG (codegen)  seed=" << seed << ": " << o.detail << "\n"
                          << "    --- minimized (" << split_lines(mini).size() << " lines) ---\n"
                          << mini << "\n    ---------------\n";
                break;
            }
            case DiffOutcome::Disagree: {     // VM value/output != oracle => a COMPILER bug
                ++compiler_bug;
                const std::string mini = shrink_failure(src, with_prelude, DiffOutcome::Disagree);
                std::cout << "  COMPILER-BUG (diff)  seed=" << seed << " (VM vs oracle disagree)\n"
                          << "     vm    : " << o.vm_desc << "\n"
                          << "     oracle: " << o.rf_desc << "\n"
                          << "    --- minimized (" << split_lines(mini).size() << " lines) ---\n"
                          << mini << "\n    ---------------\n";
                break;
            }
        }
    }
    std::cout << "  generated: " << (hi - lo) << " seeds  ->  " << agree << " agree, "
              << skip << " skipped, " << gen_bug << " gen-bugs, "
              << compiler_bug << " compiler-bugs, " << oracle_gap << " oracle-gaps\n";
    if (!skip_reasons.empty()) {
        std::vector<std::pair<std::string, int>> rs(skip_reasons.begin(), skip_reasons.end());
        // Count-descending, ties by reason: the map is unordered, so without the tiebreak the
        // listing would shuffle between runs and could not be diffed against a previous sweep.
        std::sort(rs.begin(), rs.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        std::cout << "  skip reasons:\n";
        for (const auto& r : rs) std::cout << "    " << r.second << " x " << r.first << "\n";
    }
    if (gen_bug + compiler_bug + oracle_gap == 0) ++g_pass;
    else                                          g_fail += gen_bug + compiler_bug + oracle_gap;
}

// Const arrays (`const NAME: Array[T] = [...]`, Route 2): typing + read/index/for/len, the
// non-escaping read-only guard, the pub ban, and the element/shape constraints. The positive cases
// are differential (VM == RefEval), so the pooled KIND_ARRAY + LOAD_CONST_ARRAY is cross-checked
// against the oracle's materialized array (incl. the Int<:Double element coercion).
void test_const_array_feature() {
    std::cout << "\n[const arrays]\n";

    // Positive: typing + read/index/for/len (differential VM == RefEval, no prelude needed).
    check_same("ca_index",     "const K: Array[Int] = [10, 20, 30]\n K[1] + K[2]", false);           // 50
    check_same("ca_len",       "const K: Array[Int] = [5, 6, 7, 8]\n len(K)", false);                 // 4
    check_same("ca_for_sum",   "const K: Array[Int] = [1, 2, 3, 4, 5]\n"
                               "let mut s = 0\n for x in K { s = s + x }\n s", false);                // 15
    check_same("ca_double",    "const R: Array[Double] = [0.5, 1.5, 2.0]\n R[0] + R[2]", false);      // 2.5
    check_same("ca_double_ilit","const R: Array[Double] = [1, 2, 3]\n R[1]", false);                  // 2.0 (Int coerced)
    check_same("ca_bool",      "const F: Array[Bool] = [true, false]\n if F[1] { 1 } else { 0 }", false); // 0
    check_same("ca_neg",       "const K: Array[Int] = [-5, -10, 15]\n K[0] + K[1] + K[2]", false);    // 0
    check_same("ca_in_fn",     "const T: Array[Int] = [100, 200, 300]\n"
                               "fn at(i: Int) -> Int { T[i] }\n at(2) - at(0)", false);               // 200
    check_same("ca_hex",       "const M: Array[Int] = [0xFF, 0x100]\n M[0] + M[1]", false);           // 511
    check_int ("ca_exact",     "const K: Array[Int] = [7, 8, 9]\n K[0] * 100 + K[2]", 709);

    // Negative: the non-escaping read-only guard (a const array may only be read directly).
    check_true("ca_reject_bind",    cg_check_fails("const K: Array[Int] = [1,2,3]\n let x = K\n x[0]"));
    check_true("ca_reject_return",  cg_check_fails("const K: Array[Int] = [1,2,3]\n"
                                                   "fn g() -> Array[Int] { K }\n g()[0]"));
    check_true("ca_reject_arg",     cg_check_fails("const K: Array[Int] = [1,2,3]\n"
                                                   "fn f(a: Array[Int]) -> Int { a[0] }\n f(K)"));
    check_true("ca_reject_store",   cg_check_fails("const K: Array[Int] = [1,2,3]\n let v = [K]\n 0"));
    check_true("ca_reject_assign",  cg_check_fails("const K: Array[Int] = [1,2,3]\n K[0] = 9\n K[0]"));

    // Negative: pub ban + element/shape constraints.
    check_true("ca_reject_pub",     cg_check_fails("pub const K: Array[Int] = [1,2,3]\n K[0]"));
    check_true("ca_reject_string",  cg_check_fails("const K: Array[String] = [\"a\", \"b\"]\n len(K)"));
    check_true("ca_reject_empty",   cg_check_fails("const K: Array[Int] = []\n len(K)"));
    check_true("ca_reject_nonlit",  cg_check_fails("const N: Int = 5\n const K: Array[Int] = [N]\n K[0]"));
}

// Every language construct must be EXERCISED by the differential, not merely modelled by the oracle.
// The decline split makes a construct the oracle LACKS fail loudly; this makes a construct no test
// ever REACHES fail loudly -- the other half, and the one nothing else can see, because an unexercised
// kind declines nothing, disagrees with nothing, and leaves the tally green.
//
// Reported per category, naming the missing kinds. Fed from `run_diff` on agreement only, so it counts
// the whole differential population at once: the hand-written corpus AND the in-suite seed sweep. It
// is therefore called only from the full no-arg run (deterministic: fixed corpus + fixed seeds
// 0..999), never from `--range`, whose coverage is legitimately partial.
//
// Closing a gap belongs in the CORPUS, not the generator: a `check_same` entry is a named, stable
// lock, while sweep coverage is incidental and shifts whenever the generator changes.
void test_construct_coverage() {
    std::cout << "\n[construct coverage]\n";

    std::string miss;
    auto join = [&miss](const char* n) { if (!miss.empty()) miss += ", "; miss += n; };
    auto verdict = [&miss](const char* name, const char* what) {
        if (!miss.empty()) std::cout << "    never evaluated (" << what << "): " << miss << "\n";
        check_true(name, miss.empty());
        miss.clear();
    };

    for (int i = 0; i <= static_cast<int>(svc::ExprKind::MapLit); ++i)
        if (!g_covered.expr[i]) join(kind_name(static_cast<svc::ExprKind>(i)));
    verdict("every_exprkind_differentially_exercised", "ExprKind");

    for (int i = 0; i <= static_cast<int>(svc::StmtKind::Expr); ++i)
        if (!g_covered.stmt[i]) join(kind_name(static_cast<svc::StmtKind>(i)));
    verdict("every_stmtkind_differentially_exercised", "StmtKind");

    // The `kind_name` switches above prove each SET is complete, but the loop bounds are a second,
    // manual copy of "who is numerically last" -- and an enumerator APPENDED past the bound drops
    // out of the required set silently, which is the exact failure this whole check exists to
    // prevent. Pin the ordinals so that adding one breaks the BUILD here, next to the three lines
    // that then have to move (this bound, the sibling bound, and Coverage::pat in RefEval.h).
    static_assert(static_cast<int>(svc::ExprKind::MapLit) == 25, "ExprKind gained an enumerator -- move the loop bound above");
    static_assert(static_cast<int>(svc::StmtKind::Expr)   ==  2, "StmtKind gained an enumerator -- move the loop bound above");
    static_assert(static_cast<int>(svc::PatKind::Bind)    == 10, "PatKind gained an enumerator -- move the loop bound below AND Coverage::pat's static_assert in RefEval.h");

    for (int i = 0; i <= static_cast<int>(svc::PatKind::Bind); ++i)
        if (!g_covered.pat[i]) join(kind_name(static_cast<svc::PatKind>(i)));
    verdict("every_patkind_differentially_exercised", "PatKind");

    // TWO justified exclusions from the operator range, both because the token never reaches a
    // dispatch point -- printed so they stay visible rather than living only in a comment:
    //   * `|>` lexes to TokKind::Pipe but parses to its own ExprKind::Pipe, never a BinaryExpr
    //     operator. Covered as an ExprKind above.
    //   * `+=` .. `>>>=` are normalized by the parser to their BASE op in AssignStmt::op, so these
    //     eleven token kinds do not exist in the AST. Their coverage is the `compound` check below,
    //     which is the question actually worth asking ("is `>>>=` differentially exercised?").
    std::cout << "    excluded (justified): TokKind::Pipe -- `|>` is its own ExprKind, covered there\n"
                 "    excluded (justified): += .. >>>= -- parser-normalized to the base op; see the"
                 " compound check\n";
    for (int i = static_cast<int>(svc::TokKind::Assign); i <= static_cast<int>(svc::TokKind::Tilde); ++i) {
        const auto k = static_cast<svc::TokKind>(i);
        if (k == svc::TokKind::Pipe) continue;
        if (i >= static_cast<int>(svc::TokKind::PlusEq) && i <= static_cast<int>(svc::TokKind::UShrEq)) continue;
        if (!g_covered.op[i]) join(svc::to_string(k));
    }
    verdict("every_operator_differentially_exercised", "TokKind operators");

    // The compound forms, by base op: `x += 1` must be exercised as such, not merely as `x = x + 1`.
    for (svc::TokKind base : COMPOUND_BASE_OPS)
        if (!g_covered.compound[static_cast<size_t>(base)]) join(svc::to_string(base));
    verdict("every_compound_assign_differentially_exercised", "compound assign (base op)");

    // The two NAME populations. Builtins have NO exclusions: every one is PURE and deterministic, so
    // every one can be differentially tested and an uncovered one is always a finding. (Purity is the
    // load-bearing half. Most are also opcode-backed, but `ordinal` is not -- it lowers to an identity,
    // because an int-backed enum erases to the very Int it projects -- and it is no less testable for
    // it. Do not weaken this to "opcode-backed" and let a non-opcode builtin slip out of the net.)
    for (size_t i = 0; i < std::size(svc::BUILTIN_FN_NAMES); ++i)
        if (!g_covered.builtin[i]) join(svc::BUILTIN_FN_NAMES[i].data());
    verdict("every_builtin_differentially_exercised", "builtins");

    // Natives: ONLY the differentiable subset is required, and can ever be -- a non-deterministic
    // native (nanoTime / writeFile / rawRun) cannot agree with a second implementation at all. The
    // list is a judgement about the fixture, so it is guarded from both sides instead of derived:
    // every entry must name a real native, and NATIVE_COUNT is pinned so ADDING one forces the
    // "is it deterministic?" decision rather than letting it default into neither set.
    for (const std::string_view n : refeval::DIFFERENTIABLE_NATIVES)
        if (native_id_of(std::string(n)) < 0) join(n.data());
    verdict("differentiable_natives_all_exist", "unknown native names");

    for (size_t i = 0; i < std::size(refeval::DIFFERENTIABLE_NATIVES); ++i)
        if (!g_covered.native[i]) join(refeval::DIFFERENTIABLE_NATIVES[i].data());
    verdict("every_differentiable_native_exercised", "differentiable natives");
}

// The oracle's TWO ways of declining a program must stay apart -- that split is what stops the
// differential from failing by silence (see the RefEval.h header comment). These lock the FAMILY-A
// set: the only declines allowed to stay silent. A genuine gap needs no fixture of its own, because
// it now turns the whole suite red by construction -- which is precisely the property being bought.
void test_oracle_decline_categories() {
    std::cout << "\n[oracle decline categories]\n";
    auto kind = [](const std::string& src, bool pre) { return run_diff(src, pre).kind; };

    // BY DESIGN: a non-differentiable native. `nanoTime` is non-deterministic, so the two backends
    // could never agree on a value -- there is nothing to model. Chosen over writeFile/deleteFile
    // deliberately: it is read-only, so asserting a classification does not touch the filesystem.
    check_true("decline_native_is_by_design",
        kind("use std::env::*\nlet t = nanoTime()\n1", true) == DiffOutcome::Unsupported);

    // BY DESIGN: the Map dump. The VM renders hash/backing order; this walker keeps insertion order
    // and cannot reproduce it, so a mismatch here would be spurious rather than a finding.
    check_true("decline_map_dump_is_by_design",
        kind("let m = #{1 => 2}\ntoString(m)", false) == DiffOutcome::Unsupported);

    // The complement: an ordinary modelled program must not be declined at all. Without this, a
    // reclassification that made a LIVE path throw would show up only as a quieter skip count.
    check_true("modelled_program_is_not_declined",
        kind("let a = 6\nlet b = 7\na * b", false) == DiffOutcome::Agree);
}

void test_differential() {
    std::cout << "\n[test_differential]\n";
    // Slice 1 -- core: literals, arithmetic + Int->Double coercion, control flow, let/mut/
    // assign, fns + recursion (small N). Each program returns a scalar the VM and the oracle
    // must agree on.
    check_same("diff_int_arith",       "1 + 2 * 3 - 4");
    check_same("diff_int_wrap_div",    "17 / 5 * 5 + 17 % 5");
    check_same("diff_bitwise",         "(6 & 3) | (1 << 4) ^ 2");
    check_same("diff_ushr",            "let x = 0 - 8\n x >>> 2");
    check_same("diff_double",          "3.0 * 2.0 + 1.5");
    check_same("diff_neg_zero",        "let x = 5.0\n (-(x - x))");   // VM normalizes -0.0 -> 0.0; oracle must too
    check_same("diff_mixed_promote",   "let n = 3\n n + 0.5");
    check_same("diff_let_double_annot","let x: Double = 3\n x + 1.0");
    check_same("diff_neg_tilde",       "let a = 5\n (0 - a) + (~a)");
    check_same("diff_bool_logic",      "let a = true\n let b = false\n (a && !b) || (b && a)");
    check_same("diff_cmp_chain",       "let a = 3\n let b = 7\n a < b && b >= 7 && a != b");
    check_same("diff_str_concat",      "\"foo\" + \"bar\" + \"!\"");
    check_same("diff_str_num_concat",  "\"n=\" + toString(42) + \" x=\" + toString(3.5)");
    check_same("diff_str_compare",     "\"abc\" < \"abd\"");
    // std::string round-1 helpers vs the RefEval oracle (prelude-linked). Exercises the first-class-fn
    // path in toUpper/toLower (_mapBytes) and the empty-separator split ([s]).
    check_same("diff_str_trim_case",   "toUpper(trim(\"  hi there  \")) == \"HI THERE\"", true);
    check_same("diff_str_split_empty", "join(split(\"a-b-c\", \"\"), \"|\") == \"a-b-c\"", true);
    check_same("diff_str_predicates",
        "let s = \"README.md\"\n"
        "if startsWith(s, \"READ\") { if endsWith(s, \".md\") { indexOf(s, \".\") } else { 0 - 1 } } else { 0 - 2 }",
        true);
    check_same("diff_str_replace", "replace(\"a-b-c-d\", \"-\", \"::\") == \"a::b::c::d\"", true);
    check_same("diff_str_pad_repeat", "padStart(repeatStr(\"ab\", 3), 8, '*') == \"**ababab\"", true);
    // Java-style implicit `+` coercion (scalars): VM must match the oracle (both via format_number).
    check_same("diff_concat_int",      "\"n=\" + 42");
    check_same("diff_concat_lhs_num",  "42 + \"!\"");
    check_same("diff_concat_dbl",      "\"x=\" + 3.5");
    check_same("diff_concat_dbl_int",  "\"x=\" + 3.0");            // integral double -> trailing ".0"
    check_same("diff_concat_bool",     "\"b=\" + true");
    check_same("diff_concat_chain",    "\"n=\" + 42 + \" b=\" + true");   // left-assoc + Bool
    check_same("diff_concat_expr",     "let n = 3\n \"sum=\" + (n + 4)");  // non-literal numeric operand
    check_same("diff_if_else",         "let n = 10\n if n > 5 { n * 2 } else { n - 1 }");
    check_same("diff_while_sum",       "let mut i = 0\n let mut s = 0\n while i < 10 { s = s + i\n i = i + 1 }\n s");
    check_same("diff_fn_direct",       "fn add(a: Int, b: Int) -> Int { a + b }\n add(20, 22)");
    check_same("diff_fn_recursion",    "fn fac(n: Int) -> Int { if n <= 1 { 1 } else { n * fac(n - 1) } }\n fac(6)");
    check_same("diff_fn_tco_smallN",   "fn go(n: Int, acc: Int) -> Int { if n == 0 { acc } else { go(n - 1, acc + n) } }\n go(25, 0)");
    check_same("diff_ret_double",      "fn g(n: Int) -> Double { n }\n g(3) + 0.25");
    check_same("diff_block_value",     "let r = { let a = 4\n let b = 5\n a * b }\n r + 1");

    // Slice 2 -- data, patterns, containers, for.
    check_same("diff_struct_field",    "struct P { x: Int, y: Int }\n let p = P { x: 7, y: 9 }\n p.x + p.y");
    check_same("diff_struct_result",   "struct P { x: Int, y: Int }\n P { x: 1, y: 2 }");
    check_same("diff_struct_double",   "struct P { x: Double }\n let n = 3\n P { x: n }");
    check_same("diff_struct_mut",      "struct P { x: Int }\n let mut p = P { x: 1 }\n p.x = 42\n p.x");
    check_same("diff_tuple",           "let t = (1, 2, 3)\n t.0 + t.1 * t.2");
    check_same("diff_tuple_result",    "(1, \"a\", true)");
    check_same("diff_enum_ctor",       "enum Color { Red, Green, Blue }\n Green");
    check_same("diff_enum_payload",    "enum Box { Full(Int), Empty }\n Full(99)");
    check_same("diff_list_result",     "[1, 2, 3, 4]");
    check_same("diff_list_empty",      "let xs: List[Int] = []\n xs");
    check_same("diff_array_index",     "let a = array(3, 7)\n a[0] + a[2]");
    check_same("diff_array_result",    "array(3, 5)");
    // get on sequences (total Some/None) -- prelude-on, cross-checked vs the oracle (in-bounds / OOB / negative).
    check_same("diff_get_array",     "let a = array(4, 3)\n unwrapOr(get(a, 2), -1)", true);
    check_same("diff_get_array_oob", "let a = array(4, 3)\n unwrapOr(get(a, 10), -9)", true);
    check_same("diff_get_vec_neg",   "let mut v: Vec[Int] = vec()\n push(v, 1)\n unwrapOr(get(v, -1), -7)", true);
    check_same("diff_get_bytes",     "unwrapOr(get(toBytes(\"XY\"), 1), -1)", true);
    check_same("diff_vec_push",        "let mut v = vec()\n push(v, 1)\n push(v, 2)\n push(v, 3)\n v");
    check_same("diff_vec_len",         "let mut v = vec()\n push(v, 10)\n push(v, 20)\n len(v)");
    check_same("diff_bytes",           "let b = toBytes(\"AB\")\n b[0] + b[1]");
    check_same("diff_len_string",      "len(\"abcde\") + len(\"\")");
    check_same("diff_char",            "'z' - 'a'");
    check_same("diff_toint",           "toInt(3.9) + toInt(-3.9)");
    check_same("diff_toint_nan",       "toInt(0.0 / 0.0)");
    check_same("diff_toint_sat",       "toInt(999999999999999.0)");
    check_same("diff_round_mix",       "floor(2.7) + ceil(2.3) + trunc(0.0 - 2.7)");   // 2 + 3 - 2
    check_same("diff_round_ties",      "round(2.5) - roundHalfToEven(2.5)");            // 3.0 - 2.0
    check_same("diff_round_neg",       "floor(0.0 - 2.3) + round(0.0 - 2.5)");
    check_same("diff_exp_literal",     "1.5e2 + 2e-1 + 3E1");   // 150.0 + 0.2 + 30.0
    check_same("diff_parseint",        "match parseInt(\"123\") { Ok(n) => n, Err(_) => -1 }", true);
    check_same("diff_parseint_bad",    "match parseInt(\"9x\") { Ok(n) => n, Err(_) => -1 }", true);
    check_same("diff_parsedouble",     "match parseDouble(\"2.5\") { Ok(d) => d, Err(_) => 0.0 }", true);
    check_same("diff_parsedouble_bad", "match parseDouble(\"z\") { Ok(d) => d, Err(_) => 0.0 }", true);
    check_same("diff_bytes_build",     "fromBytes(push(push(bytes(), 120), 121))");
    check_same("diff_bytes_set",       "let mut b = toBytes(\"xy\")\n b[0] = 122\n fromBytes(b)");
    // Bulk byte-blit (BYTES_APPEND) + a String-slice that drives BYTES_APPEND_RANGE through the prelude.
    check_same("diff_append_str",      "let mut b = toBytes(\"go\")\n appendBytes(b, \"od\")\n fromBytes(b)");
    check_same("diff_append_bytes",    "let mut b = bytes()\n appendBytes(b, toBytes(\"hi\"))\n appendBytes(b, toBytes(\"!\"))\n len(b)");
    check_same("diff_append_self",     "let mut b = toBytes(\"ab\")\n appendBytes(b, b)\n fromBytes(b)");
    check_same("diff_append_slice",    "slice(\"hello world\", 6, 11)", true);
    check_same("diff_slice_bytes",     "let b = toBytes(\"hello world\")\n sliceBytes(b, 0, 5) + \"|\" + sliceBytes(b, 6, 11)", true);
    check_same("diff_stringbuilder",   "let mut sb = stringBuilder()\n sb.append(\"go\")\n sb.append(\"od\")\n sb.build()", true);
    check_same("diff_discard_if",      "let mut b = bytes()\n if 1 < 2 { push(b, 120) } else { push(b, 121) }\n len(b)");
    check_same("diff_if_let",          "if let Ok(v) = parseInt(\"12\") { v } else { -1 }", true);
    // `dyn Trait`: both the VM and the oracle dispatch on the RUNTIME type (neither reads a
    // static type), so these confirm the erasure end-to-end rather than exercising new code.
    check_same("diff_dyn_hetero",
        "trait Show { fn shw(self) -> Int }\n struct P { v: Int }\n"
        "impl Show for P { fn shw(self) -> Int { self.v } }\n"
        "impl Show for Int { fn shw(self) -> Int { self * 10 } }\n"
        "let mut v: Vec[dyn Show] = vec()\n push(v, P { v: 1 })\n push(v, 2)\n push(v, P { v: 3 })\n"
        "let mut sum = 0\n for x in v { sum = sum + shw(x) }\n sum");
    check_same("diff_dyn_blanket",
        "trait Show { fn shw(self) -> Int }\n trait Dbg { fn dbg(self) -> Int }\n"
        "impl[T: Show] Dbg for T { fn dbg(self) -> Int { shw(self) + 1 } }\n"
        "struct P { v: Int }\n impl Show for P { fn shw(self) -> Int { self.v } }\n"
        "let x: dyn Dbg = P { v: 41 }\n dbg(x)");
    check_same("diff_loop_sum",        "let mut i = 0\n let mut s = 0\n let r = loop { if i >= 5 { break s }\n s = s + i\n i = i + 1 }\n r");
    check_same("diff_loop_break_val",  "let x = loop { break 6 * 7 }\n x");
    check_same("diff_loop_nested",     "let mut i = 0\n let x = loop { let y = loop { i = i + 1\n if i > 2 { break i } }\n break y * 10 }\n x");
    check_same("diff_loop_continue",   "let mut i = 0\n let mut s = 0\n let r = loop { i = i + 1\n if i > 6 { break s }\n if i == 3 { continue }\n s = s + i }\n r");
    check_same("diff_loop_stmt",       "let mut b = bytes()\n let mut i = 0\n loop { push(b, 65 + i)\n i = i + 1\n if i == 3 { break } }\n fromBytes(b)");
    check_same("diff_map_result",      "#{ 1 => 10, 2 => 20, 3 => 30 }");
    check_same("diff_map_has_get",     "let m = #{ 1 => 10, 2 => 20 }\n has(m, 2)");
    check_same("diff_mapidx_read",     "let m = #{ 1 => 10, 2 => 20 }\n m[1] + m[2]");
    check_same("diff_mapidx_write",    "let mut m = #{ 1 => 10 }\n m[2] = 20\n m[1] + m[2]");
    check_same("diff_mapidx_str_key",  "let m = #{ \"a\" => 5, \"b\" => 6 }\n m[\"a\"] * 10 + m[\"b\"]");
    check_same("diff_mapidx_mut_param",
        "fn put(mut m: Map[Int, Int], k: Int, v: Int) -> Int { m[k] = v\n v }\n"
        " let mut m = #{ 1 => 10 }\n let _ = put(m, 2, 20)\n m[1] + m[2]");
    check_same("diff_match_enum",      "enum Box { Full(Int), Empty }\n let b = Full(5)\n match b { Full(x) => x * 2, Empty => 0 }");
    check_same("diff_match_struct",    "struct P { x: Int, y: Int }\n let p = P { x: 3, y: 4 }\n match p { P { x, y } => x + y }");
    check_same("diff_match_tuple",     "let t = (1, 2)\n match t { (a, b) => a * 10 + b }");
    check_same("diff_match_list",      "let xs = [1, 2, 3]\n match xs { [h, ..t] => h, [] => 0 }");
    check_same("diff_match_guard",     "let n = 7\n match n { x if x > 5 => 100, _ => 0 }");
    check_same("diff_match_literal",   "let n = 3\n match n { 1 => 10, 3 => 30, _ => 0 }");
    check_same("diff_match_nested",    "let t = ((1, 2), 3)\n match t { ((a, b), c) => a + b + c }");
    // A MAP pattern -- a partial match, so it needs the wildcard arm. Added because the construct-
    // coverage assertion found PatKind::Map to be the one pattern shape no differential test reached:
    // it was modelled by the oracle and lowered by codegen, and neither had ever been compared.
    check_same("diff_match_map",       "let m = #{ 1 => 10, 2 => 20 }\n"
                                       " match m { #{ 1 => a, 2 => b } => a + b, _ => 0 }");
    check_same("diff_match_map_absent", "let m = #{ 1 => 10 }\n match m { #{ 9 => a } => a, _ => -1 }");
    check_same("diff_for_list_sum",    "let mut s = 0\n for x in [10, 20, 30] { s = s + x }\n s");
    check_same("diff_for_array_sum",   "let a = array(4, 5)\n let mut s = 0\n for x in a { s = s + x }\n s");
    check_same("diff_for_map_kv",      "let m = #{ 1 => 100, 2 => 200 }\n let mut s = 0\n for (k, v) in m { s = s + k + v }\n s");
    check_same("diff_for_nested_tuple", "let ps = [((1, 2), 3), ((4, 5), 6)]\n let mut s = 0\n for ((a, b), c) in ps { s = s + a + b + c }\n s");
    check_same("diff_for_struct_pat",
        "struct P { x: Int, y: Int }\n let ps = [P { x: 1, y: 2 }, P { x: 3, y: 4 }]\n let mut s = 0\n for P { x, y } in ps { s = s + x + y }\n s");
    check_same("diff_fn_struct_arg",   "struct P { x: Int, y: Int }\n fn sum(p: P) -> Int { p.x + p.y }\n sum(P { x: 8, y: 9 })");
    // `mut self` / `mut param` -- in-place heap mutation is caller-visible (aliasing); the
    // oracle's shared_ptr<Obj> agrees with the VM's heap-pointer semantics.
    check_same("diff_mut_self",
        "struct Counter { n: Int }\n trait Bump { fn bump(mut self) -> Int }\n"
        " impl Bump for Counter { fn bump(mut self) -> Int { self.n = self.n + 1\n self.n } }\n"
        " let mut c = Counter { n: 0 }\n let a = bump(c)\n let b = bump(c)\n a + b + c.n");
    check_same("diff_mut_param_array",
        "fn setat(mut a: Array[Int], i: Int, v: Int) -> Int { a[i] = v\n a[i] }\n"
        " let mut a = array(3, 0)\n let r = setat(a, 1, 9)\n r + a[1]");
    check_same("diff_mut_iterator",
        "struct Cur { data: Array[Int], i: Int }\n trait Nxt { fn next(mut self) -> Int }\n"
        " impl Nxt for Cur { fn next(mut self) -> Int { let v = self.data[self.i]\n self.i = self.i + 1\n v } }\n"
        " let mut a = array(3, 0)\n a[0] = 10\n a[1] = 20\n a[2] = 30\n"
        " let mut c = Cur { data: a, i: 0 }\n next(c) + next(c) + next(c)");
    // Tuple destructuring-`let` (irrefutable) -- VM binder cross-checked against the oracle.
    check_same("diff_let_tuple",        "let (a, b) = (5, 6)\n a * 10 + b");
    check_same("diff_let_tuple_nested", "let ((a, b), c) = ((1, 2), 3)\n a + b + c");
    check_same("diff_let_tuple_fn",     "fn swap(a: Int, b: Int) -> (Int, Int) { (b, a) }\n let (x, y) = swap(3, 7)\n x * 10 + y");
    check_same("diff_let_struct",        "struct P { x: Int, y: Int }\n let P { x, y } = P { x: 3, y: 4 }\n x * 10 + y");
    check_same("diff_let_tuple_struct",  "struct Pair(Int, Int)\n let Pair(a, b) = Pair(5, 6)\n a * 10 + b");
    check_same("diff_let_struct_nested",
        "struct P { x: Int, y: Int }\n struct Line { a: P, b: P }\n"
        " let Line { a: P { x, y }, b } = Line { a: P { x: 1, y: 2 }, b: P { x: 3, y: 4 } }\n x + y + b.x + b.y");
    // Field access through a TYPE-ERASING call result -- the applied Expr::ty carries the concrete
    // struct type, so `.field` resolves its slot statically (VM) and matches the oracle.
    check_same("diff_field_generic",   "struct P { x: Int, y: Int }\n fn idf[T](v: T) -> T { v }\n idf(P { x: 3, y: 4 }).x");
    check_same("diff_field_hof",
        "struct P { x: Int, y: Int }\n fn ap1[A, B](f: fn(A) -> B, a: A) -> B { f(a) }\n"
        " fn mk(n: Int) -> P { P { x: n, y: n } }\n ap1(mk, 7).y");
    check_same("diff_field_nested",
        "struct P { x: Int, y: Int }\n struct Pair[A, B] { fst: A, snd: B }\n"
        " let p = Pair { fst: 5, snd: P { x: 9, y: 0 } }\n p.snd.x + p.fst");
    check_same("diff_field_enum_unwrap",
        "struct P { x: Int, y: Int }\n enum Box[T] { Wrap(T) }\n"
        " fn unwrapb[T](b: Box[T]) -> T { match b { Wrap(v) => v } }\n unwrapb(Wrap(P { x: 4, y: 5 })).y");
    check_same("diff_field_prelude_unwrap",
        "struct P { x: Int, y: Int }\n unwrap(Some(P { x: 5, y: 6 })).x", true);
    check_same("diff_panic_div0",      "let a = 10\n let b = 0\n a / b");

    // Slice 3 -- closures, pipe, traits, `?`, prelude combinators, print.
    check_same("diff_lambda_nocap",    "fn ap(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n ap(fn(n: Int) -> Int { n * n }, 7)");
    check_same("diff_lambda_capture",  "let k = 10\n fn ap(f: fn(Int) -> Int, x: Int) -> Int { f(x) }\n ap(fn(n: Int) -> Int { n + k }, 5)");
    check_same("diff_lambda_ret_dbl",  "fn ap(f: fn(Int) -> Double, x: Int) -> Double { f(x) }\n ap(fn(n: Int) -> Double { n }, 4)");
    check_same("diff_pipe",            "fn inc(x: Int) -> Int { x + 1 }\n fn dbl(x: Int) -> Int { x * 2 }\n 5 |> inc |> dbl");
    check_same("diff_pipe_args",       "fn add(a: Int, b: Int) -> Int { a + b }\n 10 |> add(5)");
    check_same("diff_trait_struct",
        "trait Sz { fn sz(self) -> Int }\n struct P { x: Int, y: Int }\n"
        " impl Sz for P { fn sz(self) -> Int { self.x + self.y } }\n sz(P { x: 3, y: 4 })");
    check_same("diff_trait_qualified",
        "trait Sz { fn sz(self) -> Int }\n struct P { x: Int }\n impl Sz for P { fn sz(self) -> Int { self.x } }\n"
        " Sz::sz(P { x: 42 })");
    check_same("diff_trait_primitive",
        "trait Dbl { fn dbl(self) -> Int }\n impl Dbl for Int { fn dbl(self) -> Int { self * 2 } }\n dbl(21)");
    check_same("diff_trait_vec",
        "trait Sz { fn sz(self) -> Int }\n impl Sz for Vec[Int] { fn sz(self) -> Int { len(self) }\n }\n"
        " let mut v = vec()\n push(v, 1)\n push(v, 2)\n push(v, 3)\n sz(v)");
    check_same("diff_trait_blanket",
        "trait Show { fn shw(self) -> Int }\n trait Dbg { fn dbg(self) -> Int }\n"
        " impl[T: Show] Dbg for T { fn dbg(self) -> Int { shw(self) + 1 } }\n"
        " struct P { v: Int }\n impl Show for P { fn shw(self) -> Int { self.v } }\n"
        " dbg(P { v: 41 }) + dbg(P { v: 8 })");
    check_same("diff_trait_enum",
        "enum Sh { Sq(Int), Ci(Int) }\n trait Area { fn area(self) -> Int }\n"
        " impl Area for Sh { fn area(self) -> Int { match self { Sq(s) => s * s, Ci(r) => r * r * 3 } } }\n"
        " area(Sq(5)) + area(Ci(2))");
    check_same("diff_trait_default",
        "trait Greet { fn name(self) -> Int\n fn greet(self) -> Int { name(self) + 1 } }\n"
        " struct P { n: Int }\n impl Greet for P { fn name(self) -> Int { self.n } }\n greet(P { n: 41 })");
    check_same("diff_trait_recursion",
        "trait Cnt { fn cnt(self, acc: Int) -> Int }\n struct N { v: Int }\n"
        " impl Cnt for N { fn cnt(self, acc: Int) -> Int { if self.v == 0 { acc } else { cnt(N { v: self.v - 1 }, acc + 1) } } }\n"
        " cnt(N { v: 20 }, 0)");
    // `?` + Option/Result (prelude).
    check_same("diff_try_ok",
        "fn f() -> Result[Int, Int] { let x = Ok(10)?\n Ok(x + 5) }\n match f() { Ok(v) => v, Err(e) => e }", true);
    check_same("diff_try_err",
        "fn f() -> Result[Int, Int] { let x: Int = Err(3)?\n Ok(x + 5) }\n match f() { Ok(v) => v, Err(e) => e }", true);
    check_same("diff_try_some",
        "fn f() -> Option[Int] { let x = Some(7)?\n Some(x * 2) }\n match f() { Some(v) => v, None => 0 }", true);
    check_same("diff_prelude_unwrap_or", "unwrapOr(Some(9), 0) + unwrapOr(None, 100)", true);
    check_same("diff_prelude_is_some",   "if isSome(Some(1)) && isNone(None) { 1 } else { 0 }", true);
    check_same("diff_prelude_map",       "match optionMap(Some(5), fn(x: Int) -> Int { x * 3 }) { Some(v) => v, None => 0 }", true);
    check_same("diff_prelude_and_then",
        "fn step(x: Int) -> Result[Int, Int] { Ok(x + 1) }\n match resultAndThen(Ok(10), step) { Ok(v) => v, Err(e) => e }", true);
    check_same("diff_prelude_unwrap",    "unwrap(Some(77))", true);
    // Unwrap trait dispatched on a Result receiver -- the oracle must resolve the Result impl too.
    check_same("diff_unwrap_result",     "let r: Result[Int, Int] = Ok(88)\n unwrap(r)", true);
    check_same("diff_unwrapor_result",   "let a: Result[Int, Int] = Ok(4)\n let b: Result[Int, Int] = Err(1)\n unwrapOr(a, 0) + unwrapOr(b, 100)", true);
    // Lazy Stream[T] (v1) -- the pull pipeline must agree with the tree-walking oracle (closures +
    // mut-cursor source + tail-recursive fold). The oracle drives the SAME prelude source.
    check_same("diff_stream_range_sum", "sum(range(0, 10))", true);
    check_same("diff_stream_fold",
        "fold(range(2, 8), 0, fn(a: Int, x: Int) -> Int { a + x })", true);
    check_same("diff_stream_pipeline",  // filter |> map |> sum
        "sum(map(filter(range(0, 20), fn(x: Int) -> Bool { x % 2 == 0 }), fn(x: Int) -> Int { x * 3 }))", true);
    check_same("diff_stream_collect",
        "let w: Vec[Int] = collect(map(range(0, 6), fn(x: Int) -> Int { x + 1 }))\n w[0] + w[5]", true);
    check_same("diff_stream_foreach",  // side-effect output must match the oracle too
        "forEach(range(0, 4), fn(x: Int) -> () { print(x) })", true);
    check_same("diff_stream_array", "sum(intoIter(array(5, 3)))", true);
    check_same("diff_stream_bytes", "sum(intoIter(toBytes(\"xyz\")))", true);
    check_same("diff_stream_list",  // the risky one: refutable list pattern + List-field mut in the advance
        "let l: List[Int] = [2, 4, 6, 8]\n sum(map(intoIter(l), fn(x: Int) -> Int { x + 1 }))", true);
    check_same("diff_stream_take",  // take over an infinite iterate stream (lazy -- must terminate)
        "sum(take(iterate(3, fn(x: Int) -> Int { x + 3 }), 4))", true);          // 3+6+9+12
    check_same("diff_stream_map_intoiter",
        "let mut m: Map[Int, Int] = #{}\n m[7] = 1\n m[8] = 2\n count(intoIter(m))", true);
    check_same("diff_stream_drop",  "sum(drop(range(0, 8), 3))", true);         // 3+4+5+6+7
    check_same("diff_stream_find",  "unwrapOr(find(range(0, 20), fn(x: Int) -> Bool { x * x > 30 }), -1)", true);
    check_same("diff_stream_any",   "any(range(0, 6), fn(x: Int) -> Bool { x == 4 })", true);
    check_same("diff_stream_once",  "sum(once(9))", true);
    // Differentiable natives (G1): the fixed NativeEnv (args {alpha,beta}, stdin "l1\nl2\n",
    // SVC_DIFF_ENV=envval) feeds BOTH the VM and the oracle, so each native's lowering is cross-checked.
    check_same("diff_native_args_len",    "use std::env::*\nlen(args())", true);                                  // Plain Array build
    check_same("diff_native_getenv_some", "use std::env::*\nunwrapOr(getEnv(\"SVC_DIFF_ENV\"), \"none\")", true); // Option wrap, Some
    check_same("diff_native_getenv_none", "use std::env::*\nisSome(getEnv(\"SVC_DIFF_UNSET_ZZZ\"))", true);        // Option wrap, None
    // Three builtins the coverage assertion found never differentially exercised.
    //   * `panic` -- both backends must FAULT (check_same compares fault-or-not, not the message).
    //   * `values` -- a SINGLE-entry map on purpose: the VM returns backing/hash order and the oracle
    //     insertion order, so a multi-entry case would disagree for a reason that is not a defect.
    //   * `toDouble` -- the Int->Double widening (I2D); the second line locks that `Int / Int` stays
    //     integer division, which is the trap the explicit widening exists to escape.
    check_same("diff_builtin_panic",  "fn boom(n: Int) -> Int { if n > 0 { panic(\"boom\") } else { 1 } }\n boom(1)", false);
    check_same("diff_builtin_values", "let m = #{ \"k\" => 7 }\n values(m)[0] + len(values(m))", false);
    check_same("diff_builtin_todouble", "let n = 7\n let d = toDouble(n) / 2.0\n"
                                        " let i = n / 2\n toString(d) + \"|\" + toString(i)", false);
    check_same("diff_const_int", "const N: Int = 42\n N + 8", false);        // const inlines identically in VM + oracle
    check_same("diff_const_dbl", "const H: Double = 1.5\n H * 2.0", false);
    // std::math natives: bit-exact vs the RefEval libm mirror (same std:: call both sides).
    check_same("diff_m_sqrt",  "use std::math::*\n sqrt(2.0)", true);
    check_same("diff_m_sin",   "use std::math::*\n sin(1.0)", true);
    check_same("diff_m_pow",   "use std::math::*\n pow(2.0, 0.5)", true);
    check_same("diff_m_atan2", "use std::math::*\n atan2(1.0, 2.0)", true);
    check_same("diff_m_ln",    "use std::math::*\n ln(10.0)", true);
    check_same("diff_m_isnan", "use std::math::*\n isNaN(sqrt(-1.0))", true);
    check_same("diff_m_minof", "use std::math::*\n minOf(5, 2)", true);
    check_same("diff_m_pi",    "use std::math::*\n toInt(PI * 1000.0)", true);
    // The rest of the libm surface the oracle mirrors. Added because the native-coverage assertion
    // found nine of them modelled on both sides yet never once compared.
    check_same("diff_m_cbrt",  "use std::math::*\n cbrt(27.0)", true);
    check_same("diff_m_exp",   "use std::math::*\n exp(1.0)", true);
    check_same("diff_m_hypot", "use std::math::*\n hypot(3.0, 4.0)", true);
    check_same("diff_m_abs",   "use std::math::*\n abs(-2.5) + abs(2.5)", true);
    check_same("diff_m_tan",   "use std::math::*\n tan(0.5)", true);
    check_same("diff_m_asin",  "use std::math::*\n asin(0.5)", true);
    check_same("diff_m_acos",  "use std::math::*\n acos(0.5)", true);
    check_same("diff_m_atan",  "use std::math::*\n atan(0.5)", true);
    check_same("diff_m_isinf", "use std::math::*\n isInfinite(1.0 / 0.0)", true);
    // sort/sorted/sortBy: the prelude merge sort runs in BOTH the VM and the RefEval oracle (no C++ mirror).
    check_same("diff_sorted",  "let s = sorted(toVec([5, 3, 8, 1, 9, 2]))\n s[0]*100000 + s[1]*10000 + s[2]*1000 + s[3]*100 + s[4]*10 + s[5]", true);
    check_same("diff_sortby",  "let s = sortBy(toVec([4, 2, 6, 1]), fn(a: Int, b: Int) -> Bool { b < a })\n s[0]*1000 + s[1]*100 + s[2]*10 + s[3]", true);
    check_same("diff_sort_mut","let mut v = toVec([9, 4, 7, 1])\n sort(v)\n v[0]*1000 + v[1]*100 + v[2]*10 + v[3]", true);
    check_same("diff_native_fileexists",  "use std::io::*\nfileExists(\"svc_diff_no_such_file_zzz_9x7\")", true);  // Plain Bool
    check_same("diff_native_isfile",      "use std::io::*\nisFile(\"svc_diff_no_such_file_zzz_9x7\")", true);      // Plain Bool
    check_same("diff_native_isdir",       "use std::io::*\nisDir(\"svc_diff_no_such_file_zzz_9x7\")", true);       // Plain Bool
    check_same("diff_native_filesize_err","use std::io::*\nisErr(fileSize(\"svc_diff_no_such_file_zzz_9x7\"))", true); // Result wrap (discriminant)
    check_same("diff_native_readfile_err","use std::io::*\nisErr(readFile(\"svc_diff_no_such_file_zzz_9x7\"))", true); // Result wrap (discriminant)
    check_same("diff_native_readall",     "use std::io::*\nreadAllStdin()", true);                                // Plain String
    check_same("diff_native_readline",    "use std::io::*\nmatch readLine() { Some(s) => s, None => \"eof\" }", true); // Option wrap
    check_same("diff_native_in_lambda",   // a lambda body referencing a NATIVE -- capture analysis must
        "use std::io::*\n"
        "fn ap(f: fn(Int) -> Bool, x: Int) -> Bool { f(x) }\n"                    // treat it as global, not
        " ap(fn(n: Int) -> Bool { fileExists(\"svc_diff_no_such_file_zzz_9x7\") }, 0)", true);  // capture it
    // print / println output comparison.
    check_same("diff_print",   "print(1)\n print(2)\n print(3)", true);
    check_same("diff_println", "println(42)\n println()\n println(\"hi\")", true);
    check_same("diff_print_loop", "for i in [1, 2, 3] { print(i) }\n println()", true);
    // multi-arg (no separator, newline once).
    check_same("diff_print_multi",   "print(\"a\", 1, \"b\")", true);
    check_same("diff_println_multi", "println(1, 2, 3)", true);
}

} // namespace

// =============================================================================
// Phase-0 generational-GC benchmark (measurement only; `static_compiler_tests --bench [N]`).
//
// Runs a set of allocation-churn workloads on a fresh Heap and reports the GC
// profile from Heap::GcStats: allocation volume, #collections, GC time / max
// pause, and -- the decisive generational metric -- the AVERAGE SURVIVOR RATIO
// per collection (low => most garbage dies young => a nursery pays off). It
// changes NO collector behaviour; it only reads the counters. Run in RELEASE
// (Debug adds verify_heap() to every collect(), inflating GC time). This is the
// "measure-before-decide" gate for the write-barrier fork -- see the plan.
// =============================================================================
void bench_gc_one(const char* name, const std::string& src, uint32_t n) {
    (void)n; // reserved for a future per-workload N column
    svc::Module m = svc::compile(src.c_str(), svc::builtin_prelude());
    static const std::vector<NativeFunc> natives = build_native_table();
    Heap heap;
    StringInterner interner;
    std::ostringstream sink; // swallow the workload's own println output

    const auto t0 = std::chrono::steady_clock::now();
    execute(m.bytecode, &heap, nullptr, &interner, m.top_frame_size,
            &m.constants, &m.struct_types, &m.string_literals, &kNoAtoms,
            &m.function_table, &sink,
            &m.trait_table, m.trait_table_width, m.trait_method_count,
            &m.line_table, &m.function_names, &m.column_table, &natives);
    const auto t1 = std::chrono::steady_clock::now();

    const Heap::GcStats& s = heap.stats();
    const double wall_ms  = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
    const double alloc_mb = s.bytes_alloced / (1024.0 * 1024.0);
    const double gc_ms    = s.gc_ns_total / 1.0e6;
    const double pause_ms = s.gc_ns_max   / 1.0e6;
    const double surv_pct = s.from_used_sum ? (100.0 * (double)s.survivors_sum / (double)s.from_used_sum) : 0.0;
    const double live_mb  = heap.used() / (1024.0 * 1024.0);
    const double churn    = live_mb > 0.0 ? alloc_mb / live_mb : 0.0;
    const double gc_frac  = wall_ms > 0.0 ? 100.0 * gc_ms / wall_ms : 0.0;

    std::cout << std::format(
        "  {:<22} alloc={:>8.1f}MB objs={:>10}  GC[n={:>4} t={:>7.2f}ms/{:>4.1f}% maxPause={:>6.3f}ms]"
        "  surv/coll={:>5.1f}%  live={:>6.1f}MB  churn={:>5.1f}x  grow={:>2}  wall={:>8.2f}ms\n",
        name, alloc_mb, s.objects_alloced, s.collections, gc_ms, gc_frac, pause_ms,
        surv_pct, live_mb, churn, s.grow_events, wall_ms);
}

void run_gc_bench(uint32_t n) {
    const std::string N = std::to_string(n);
    std::cout << "=== Phase-0 generational-GC benchmark (N=" << n << "; run in RELEASE) ===\n";

    // W1: 3-stage lazy pipeline, terminal sum -> pure young churn (a Some() per next());
    //     almost NOTHING survives. The headline generational case.
    bench_gc_one("stream-filter-map-sum",
        "let pipe: dyn Iterator[Int] = map(filter(range(0, " + N + "), "
        "fn(x: Int) -> Bool { x % 2 == 0 }), fn(x: Int) -> Int { x + 1 })\n"
        "let s = sum(pipe)\n"
        "println(toString(s))\n", n);

    // W2: lazy map -> collect into a Vec -> young cursor churn + one large SURVIVING Vec.
    bench_gc_one("stream-collect-vec",
        "let it: dyn Iterator[Int] = map(range(0, " + N + "), fn(x: Int) -> Int { x * 2 })\n"
        "let v: Vec[Int] = collect(it)\n"
        "println(toString(len(v)))\n", n);

    // W3: struct allocation in a tight loop, each dead immediately -> pure young garbage, no streams.
    bench_gc_one("struct-loop-dead",
        "struct P { x: Int, y: Int }\n"
        "let mut acc = 0\n"
        "let mut i = 0\n"
        "while i < " + N + " { let p = P { x: i, y: i + 1 }\n acc = acc + p.x + p.y\n i = i + 1 }\n"
        "println(toString(acc))\n", n);

    // W4: a long-lived (soon-old) Vec that keeps gaining young elements -> the old<-young store
    //     pattern a write barrier must track; HIGH retention (all survive).
    bench_gc_one("mutation-vec-grow",
        "struct P { x: Int, y: Int }\n"
        "let mut v: Vec[P] = vec()\n"
        "let mut i = 0\n"
        "while i < " + N + " { push(v, P { x: i, y: i })\n i = i + 1 }\n"
        "println(toString(len(v)))\n", n);

    std::cout << "\nReading: low 'surv/coll' on W1/W3 (young dies young) is the generational signal that a\n"
                 "nursery pays off; W2 mixes dead cursors with a surviving Vec; W4 (high retention + old<-young\n"
                 "stores) is the write-barrier-pressure workload. See the plan's Phase 0 (barrier fork).\n";
}

int main(int argc, char** argv) {
    // The suite runs with the SHIPPING codegen configuration, so inlining is ON unless
    // `--no-inline` is appended. Both spellings are scanned for rather than parsed positionally,
    // and every positional reader below compares against a fixed token, so a trailing flag is
    // inert to all of them.
    //
    // A mis-inline is a VALUE
    // disagreement against the RefEval oracle, and the oracle walks the typed AST, so it is blind
    // to the transform by construction -- 100 000 generated programs decide it, not inspection.
    // The banner records the mode so a stored sweep log cannot be compared against a run of the
    // other mode by accident.
    bool inline_mode = true;
    // SROA is the same shape of knob but the opposite default: OFF is the shipping configuration
    // while its slices land, so `--sroa` is what runs the load-bearing sweep for it. Scanned, not
    // parsed positionally, for the same reason as the inline flags.
    bool sroa_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--inline")    inline_mode = true;
        if (a == "--no-inline") inline_mode = false;
        if (a == "--sroa")      sroa_mode = true;
        if (a == "--no-sroa")   sroa_mode = false;
    }
    svc::set_inline_calls(inline_mode);
    svc::set_sroa(sroa_mode);
    std::cout << "[codegen inlining: " << (inline_mode ? "ON (shipping default)" : "off")
              << " | sroa: " << (sroa_mode ? "ON" : "off (shipping default)") << "]\n";

    // (The Phase-0 throwaway `--bench-barrier` per-store cost probe was removed after P4 --
    // it had served its purpose: the precise remembered-set barrier it modelled is now the
    // shipping generational barrier, validated by the GC-active differential sweep + vm_tests.)
    // Phase-0 GC benchmark entry: `static_compiler_tests --bench [N]` (measurement only, no
    // collector change; run in Release). Separate from the gated test suite -- returns 0.
    if (argc > 1 && std::string(argv[1]) == "--bench") {
        uint32_t n = 1000000;
        if (argc > 2) { uint32_t v = (uint32_t)std::strtoul(argv[2], nullptr, 10); if (v) n = v; }
        run_gc_bench(n);
        return 0;
    }
    // Debug entry: `static_compiler_tests --repro <seed> [p]` regenerates one seed (p = prelude mode),
    // runs the differential, and prints the auto-minimized failing program (or the source if it agrees).
    // `static_compiler_tests --gen <seed> [p]` prints the generated program WITHOUT running it -- the only
    // way to read a seed whose run does not terminate (`--repro` runs it first).
    if (argc > 2 && std::string(argv[1]) == "--gen") {
        const bool pre = (argc > 3 && std::string(argv[3]) == "p");
        std::cout << gen::generate((uint32_t)std::strtoul(argv[2], nullptr, 10), pre);
        return 0;
    }
    if (argc > 2 && std::string(argv[1]) == "--repro") {
        const bool pre = (argc > 3 && std::string(argv[3]) == "p");
        const uint32_t seed = (uint32_t)std::strtoul(argv[2], nullptr, 10);
        const std::string src = gen::generate(seed, pre);
        const DiffOutcome o = run_diff(src, pre);
        const char* kn = o.kind == DiffOutcome::Agree ? "Agree" :
                         o.kind == DiffOutcome::Disagree ? "Disagree" :
                         o.kind == DiffOutcome::CodegenFail ? "CodegenFail" :
                         o.kind == DiffOutcome::CheckReject ? "CheckReject" :
                         o.kind == DiffOutcome::OracleGap ? "OracleGap" : "Unsupported";
        std::cout << "seed=" << seed << (pre ? " +prelude" : "") << " kind=" << kn << "\n";
        if (o.kind == DiffOutcome::Disagree)   std::cout << "vm=" << o.vm_desc << "\noracle=" << o.rf_desc << "\n";
        // Unsupported prints its reason too: without it a skipped seed reports only `kind=Unsupported`,
        // which is the same silence the sweep histogram removes -- and the reason is what tells you
        // whether the oracle declined by design or simply does not model the construct.
        if (o.kind == DiffOutcome::CodegenFail || o.kind == DiffOutcome::Unsupported ||
            o.kind == DiffOutcome::OracleGap)
            std::cout << "detail=" << o.detail << "\n";
        if (o.kind == DiffOutcome::Disagree || o.kind == DiffOutcome::CodegenFail ||
            o.kind == DiffOutcome::OracleGap)
            std::cout << "--- minimized ---\n" << shrink_failure(src, pre, o.kind) << "\n---\n";
        else
            std::cout << "--- source ---\n" << src << "---\n";
        return 0;
    }
    // Sweep-only entry: `static_compiler_tests --range <lo> <hi> [np]` runs ONLY the differential
    // sweep over the half-open seed range [lo, hi) -- no unit suite -- so it can be launched as N
    // parallel PROCESSES over DISJOINT ranges (e.g. --range 0 50000, --range 50000 100000, ...), each
    // covering NEW seeds. Without `np` it runs both the no-prelude and the (heavier) prelude sweep over
    // the range. The unit suite + the from-0 sweep still run via the no-arg / `<count>` forms. Never run
    // the SAME range twice -- the generator is deterministic per seed, so a repeat adds zero coverage.
    if (argc > 2 && std::string(argv[1]) == "--range") {
        const uint32_t lo = (uint32_t)std::strtoul(argv[2], nullptr, 10);
        const uint32_t hi = (argc > 3) ? (uint32_t)std::strtoul(argv[3], nullptr, 10) : 0;
        if (hi <= lo) {
            std::cout << "usage: static_compiler_tests --range <lo> <hi> [np]   (hi must be > lo)\n";
            return 2;
        }
        const bool run_prelude = !(argc > 4 && std::string(argv[4]) == "np");
        test_generated(lo, hi, /*with_prelude=*/false);
        if (run_prelude) test_generated(lo, hi, /*with_prelude=*/true);
        std::cout << "\n==== " << g_pass << " passed, " << g_fail << " failed ====\n";
        return g_fail == 0 ? 0 : 1;
    }
    test_case_classification();
    test_keywords();
    test_numbers();
    test_radix_literals();
    test_chained_lvalue_diff();
    test_record_variant_diff();
    test_raw_strings_cg();
    test_batch_round2();
    test_compound_assign();
    test_hashable();
    test_transparent_structs();
    test_int_enums();
    test_enum_qualified_path();
    test_enum_variant_coexist();
    test_use_enum_import();
    test_method_call_syntax();
    test_inherent_impl();
    test_qualified_inherent_call();
    test_receiver_inferred_once();
    test_measure_linear();
    test_associated_fn();
    test_char_utf8();
    test_char_literals();
    test_asi();
    test_colons();
    test_operators();
    test_strings();
    test_raw_strings();
    test_errors();
    test_typed_ast();
    test_parser_expr();
    test_parser_items();
    test_parser_patterns();
    test_parser_errors();
    test_loader();
    test_modules();
    test_import_matrix();
    test_newcomer_diagnostics();
    test_check_solver();
    test_check_signatures();
    test_check_bodies();
    test_check_match();
    test_check_generics();
    test_check_traits_use();
    test_check_corpus();
    test_structured_diagnostics();
    test_check_warnings();
    test_check_duplicate_map_key();
    test_check_blanket();
    test_check_bounded_fn_values();
    test_check_generic_traits();
    test_check_forward_generic();
    test_codegen_core();
    test_codegen_functions();
    test_codegen_structs();
    test_codegen_containers();
    test_codegen_match();
    test_codegen_traits();
    test_codegen_dyn_traits();
    test_codegen_closures();
    test_check_lambda_capture();
    test_check_capture_snapshot_warning();
    test_check_join_fold();
    test_codegen_widen_double();
    test_codegen_try();
    test_codegen_arrays();
    test_codegen_prelude();
    test_ambient_shadowing();
    test_tree_shake_prelude();
    test_codegen_iterable();
    test_codegen_iterator_for();
    test_codegen_combinators();
    test_codegen_stream();
    test_codegen_builtins();
    test_codegen_maps();
    test_codegen_vec_bytes();
    test_codegen_natives();
    test_codegen_process();
    test_std_bytes();
    test_std_json();
    test_std_regex();
    test_std_random();
    test_empty_array();
    test_const_array_feature();
    test_clone();
    test_pattern_alt();
    test_at_bindings();
    test_range_const_bounds();
    test_map_helpers();
    test_std_set();
    test_std_time();
    test_std_cli();
    test_std_hash();
    test_std_net();
    test_interpolation();
    test_format();
    test_string_iter();
    test_deep_recursion();
    test_codegen_generic_traits();
    test_codegen_indirect_return();
    test_codegen_frame_selfcheck();
    test_mut_receiver();
    test_bytecode_roundtrip();
    test_torture();
    test_shrinker();
    test_oracle_decline_categories();
    test_differential();
    // Sweep size defaults to 1000 (the committed gate); an optional argv[1] drives a heavier
    // reproducible run (e.g. `static_compiler_tests 20000`) without a rebuild. This form covers seeds
    // 0..N in BOTH modes -- there is no need to invoke the exe twice (a repeat re-tests identical
    // deterministic seeds). For MORE coverage or PARALLELISM, launch several `--range` processes over
    // disjoint ranges instead (see the --range entry above).
    uint32_t sweep = 1000;
    if (argc > 1) { uint32_t n = (uint32_t)std::strtoul(argv[1], nullptr, 10); if (n) sweep = n; }
    // argv[2] == "np" skips the (much heavier, prelude-per-program) prelude sweep -- for a cheap
    // deep no-prelude run (e.g. `static_compiler_tests 200000 np`).
    const bool run_prelude_sweep = !(argc > 2 && std::string(argv[2]) == "np");
    test_generated(0, sweep, /*with_prelude=*/false);
    if (run_prelude_sweep)
        test_generated(0, sweep, /*with_prelude=*/true);   // Option/Result + `?` + combinators sweep

    // LAST: it judges the coverage accumulated by everything above (corpus + both sweeps), so it must
    // run after them. Only on this path -- `--range` runs no corpus, so its coverage is partial by
    // construction and asserting on it would be a false alarm.
    test_construct_coverage();

    std::cout << "\n==== " << g_pass << " passed, " << g_fail << " failed ====\n";
    return g_fail == 0 ? 0 : 1;
}
