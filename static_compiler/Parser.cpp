// =============================================================================
// Parser.cpp -- implementation of the Skarn recursive-descent + Pratt parser.
//
// Adapted from the former dynamic parser. The expression
// layer is a Pratt parser: parse_prefix() produces leaves/prefix forms, the
// binding-power loop in parse_expr() folds infix operators, and the postfix loop
// picks up call/index/field/try. Items/statements/patterns/types are ordinary
// recursive descent. See Parser.h + docs/skarn_grammar.ebnf.
// =============================================================================

#include "Parser.h"

#include <utility>

namespace svc {

// ---- ParseError -------------------------------------------------------------

ParseError::ParseError(const std::string& msg, uint32_t line, uint32_t col)
    : std::runtime_error(msg + " at line " + std::to_string(line) + ":" + std::to_string(col)),
      line_(line), col_(col) {}

// ---- cursor -----------------------------------------------------------------

Parser::Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {
    // The lexer always terminates the stream with Eof, so toks_ is never empty.
}

const Token& Parser::peek(size_t off) const noexcept {
    size_t i = pos_ + off;
    return i < toks_.size() ? toks_[i] : toks_.back();   // back() is Eof
}

bool Parser::at_end() const noexcept { return cur().kind == TokKind::Eof; }

const Token& Parser::advance() noexcept {
    const Token& t = toks_[pos_];
    if (pos_ + 1 < toks_.size()) ++pos_;
    return t;
}

bool Parser::accept(TokKind k) noexcept {
    if (check(k)) { advance(); return true; }
    return false;
}

const Token& Parser::expect(TokKind k, const char* what) {
    if (check(k)) return advance();
    error(std::string("expected ") + what);
}

void Parser::error(const std::string& msg) const {
    throw ParseError(msg, cur().line, cur().col);
}

void Parser::skip_terminators() {
    while (check(TokKind::StmtEnd)) advance();
}

// ---- error recovery (editor tooling) ----------------------------------------

namespace {
constexpr size_t MAX_ERRORS = 100;
}

bool Parser::at_item_start() const noexcept {
    switch (cur().kind) {
    case TokKind::KwStruct: case TokKind::KwEnum:   case TokKind::KwTrait: case TokKind::KwImpl:
    case TokKind::KwImport: case TokKind::KwUse:    case TokKind::KwPub:   case TokKind::KwConst:
    case TokKind::KwTransparent:
        return true;
    case TokKind::KwFn:
        return peek(1).kind == TokKind::LIdent;   // `fn (` is a lambda
    default:
        return false;
    }
}

void Parser::note(const ParseError& e) {
    ++syntax_events_;
    if (errors_->size() >= MAX_ERRORS) return;
    for (const ParseError& x : *errors_)
        if (x.line() == e.line() && x.col() == e.col()) return;
    errors_->push_back(e);
}

uint32_t Parser::line_indent(const Token& t) const {
    size_t i = static_cast<size_t>(&t - toks_.data());   // back to the first token of its line
    while (i > 0 && toks_[i - 1].line == t.line && toks_[i - 1].kind != TokKind::StmtEnd) --i;
    return toks_[i].col;
}

void Parser::recover(const ParseError& e, Sync where, size_t start, const Token* open) {
    note(e);
    // An error at the first token of a line, in a construct that began on an earlier line, is
    // usually an unfinished line above (`let x = ` then the next statement): resume right there.
    if (pos_ > start && cur().line > toks_[start].line) {
        size_t p = pos_;
        while (p > start && toks_[p - 1].kind == TokKind::StmtEnd) --p;
        if (p > start && toks_[p - 1].line < cur().line && !check(TokKind::RBrace)) return;
    }
    int depth = 0;
    while (!at_end()) {
        const TokKind k = cur().kind;
        // An item keyword closes everything open. At item level only at depth 0 or in column 1, so a
        // broken impl header does not re-enter its own body at the first `fn`.
        if (at_item_start() && (where != Sync::Item || depth == 0 || cur().col == 1)) break;
        if (k == TokKind::LBrace || k == TokKind::LParen || k == TokKind::LBracket) {
            ++depth; advance(); continue;
        }
        if (k == TokKind::RParen || k == TokKind::RBracket) {
            if (depth > 0) --depth;
            advance(); continue;
        }
        if (k == TokKind::RBrace) {
            if (depth > 0) { --depth; advance(); continue; }
            // A construct that lost its `{` (`Leaf => },`, `fn m(self) -> Int  x }`) leaves its `}`
            // behind, on a later line and indented past the body's own closer: skip it with the
            // construct instead of ending the enclosing body there.
            if (open && cur().line != open->line && cur().col > line_indent(*open)) { advance(); break; }
            if (where != Sync::Item) break;        // ends the enclosing body; left for its loop
            advance(); continue;                   // a stray `}` at item level
        }
        if (depth == 0) {
            if (k == TokKind::StmtEnd && where != Sync::Member) { advance(); break; }
            if (k == TokKind::Comma && (where == Sync::Arm || where == Sync::Field)) { advance(); break; }
            if (k == TokKind::KwFn && where == Sync::Member) break;
        }
        advance();
    }
    const bool loop_exits = where != Sync::Item && (check(TokKind::RBrace) || at_item_start());
    if (pos_ == start && !at_end() && !loop_exits) advance();
}

bool Parser::body_unclosed(const Token& open, uint32_t owner_col, bool members_are_fns) {
    if (!recover_) return false;
    if (!at_end()) {
        if (!at_item_start()) return false;
        if (members_are_fns && cur().col > owner_col) return false;   // an indented member
    }
    if (unclosed_at_ != pos_) {                    // only the innermost body reports
        const Token& at = first_mismatch_ ? *first_mismatch_ : open;
        note(ParseError("this '{' is never closed", at.line, at.col));
        unclosed_at_ = pos_;
        first_mismatch_ = nullptr;
    }
    return true;
}

void Parser::close_body(const Token& open) {
    if (recover_ && !first_mismatch_ && check(TokKind::RBrace) && cur().line != open.line &&
        cur().col != line_indent(open))
        first_mismatch_ = &open;
    expect(TokKind::RBrace, "'}'");
}

Program Parser::parse_program_tolerant(std::vector<ParseError>& errors, const std::vector<LexError>& lex_errors) {
    recover_ = true;
    errors_ = &errors;
    auto before = [](uint32_t l1, uint32_t c1, uint32_t l2, uint32_t c2) {
        return l1 < l2 || (l1 == l2 && c1 < c2);
    };
    Program prog;
    skip_terminators();
    while (!at_end()) {
        const size_t start = pos_;
        const uint32_t sl = cur().line, sc = cur().col;
        const size_t events = syntax_events_;
        first_mismatch_ = nullptr;
        ItemPtr it;
        try {
            it = parse_item();
        } catch (const ParseError& e) {
            no_struct_lit_ = false;
            recover(e, Sync::Item, start);
        }
        skip_terminators();
        if (!it) continue;
        bool broken = syntax_events_ != events;
        for (const LexError& le : lex_errors)
            if (!before(le.line(), le.col(), sl, sc) &&
                (at_end() || before(le.line(), le.col(), cur().line, cur().col)))
                broken = true;
        it->has_syntax_error = broken;
        prog.items.push_back(std::move(it));
    }
    recover_ = false;
    errors_ = nullptr;
    return prog;
}

// ---- program / items --------------------------------------------------------

Program Parser::parse_program() {
    Program prog;
    skip_terminators();
    while (!at_end()) {
        layout_start();
        prog.items.push_back(parse_item());
        skip_terminators();
    }
    return prog;
}

ItemPtr Parser::parse_item() {
    // Optional `pub` visibility prefix (S4). Only a fn / struct / enum / trait can be exported;
    // `pub impl` / `pub use` / `pub let` are meaningless (an impl is reached by dispatch; a `use`
    // is not an export in v1 -- no `pub use`; a top-level stmt is module-private state).
    item_col_ = cur().col;
    bool is_pub = false;
    if (check(TokKind::KwPub)) { advance(); is_pub = true; }

    // Optional `transparent` modifier -- only before `struct`. Marks a single-field tuple struct
    // that erases to its underlying immediate at runtime (a zero-cost newtype). Order: `pub
    // transparent struct N(T)`. Any other following item is rejected below.
    bool is_transparent = false;
    if (check(TokKind::KwTransparent)) { advance(); is_transparent = true; }

    ItemPtr it;
    // `fn NAME` is a function item; `fn (` is a lambda expression-statement.
    if (check(TokKind::KwFn) && peek(1).kind == TokKind::LIdent) it = parse_fn_item();
    else if (check(TokKind::KwStruct)) it = parse_struct_item();
    else if (check(TokKind::KwEnum))   it = parse_enum_item();
    else if (check(TokKind::KwTrait))  it = parse_trait_item();
    else if (check(TokKind::KwConst))  it = parse_const_item();
    else {
        if (is_pub)
            error("'pub' may only precede a fn, struct, enum, trait, or const");
        if (is_transparent)
            error("'transparent' may only precede a struct");
        if (check(TokKind::KwImpl))   return parse_impl_item();
        if (check(TokKind::KwImport)) return parse_import_item();
        if (check(TokKind::KwUse))    return parse_use_item();

        auto si = std::make_unique<StmtItem>();
        StmtPtr s = parse_stmt();
        si->line = s->line; si->col = s->col;
        si->stmt = std::move(s);
        return si;
    }
    it->is_pub = is_pub;
    if (is_transparent) {
        if (it->kind == ItemKind::Struct)
            static_cast<StructItem&>(*it).is_transparent = true;
        else
            error("'transparent' may only precede a struct");
    }
    return it;
}

ItemPtr Parser::parse_fn_item() {
    const Token& kw = expect(TokKind::KwFn, "'fn'");
    auto fn = std::make_unique<FnItem>();
    fn->line = kw.line; fn->col = kw.col;
    const Token& nm = expect(TokKind::LIdent, "function name");
    fn->name = nm.text; fn->name_line = nm.line; fn->name_col = nm.col;
    fn->generics = parse_generic_params();
    fn->params = parse_param_list();
    expect(TokKind::Arrow, "'->' (return type is mandatory)");
    fn->ret = parse_type();
    fn->body = parse_block();
    return fn;
}

// `[pub] const NAME: Type = <literal>` -- a compile-time module constant. The initializer is
// restricted to a single literal (Int/Double/Bool/String/char), optionally a negated number
// (folded here into a signed literal); const-EXPRESSION folding (`2.0 * PI`) is deferred.
ItemPtr Parser::parse_const_item() {
    const Token& kw = expect(TokKind::KwConst, "'const'");
    auto c = std::make_unique<ConstItem>();
    c->line = kw.line; c->col = kw.col;
    if (check(TokKind::LIdent) || check(TokKind::UIdent)) {
        c->name = cur().text; c->name_line = cur().line; c->name_col = cur().col; advance();
    }
    else error("constant name");
    expect(TokKind::Colon, "':' (a const requires a type annotation)");
    c->type = parse_type();
    expect(TokKind::Assign, "'='");
    ExprPtr v = parse_expr(0);
    // Fold a leading unary minus on a numeric literal into the literal itself.
    if (v->kind == ExprKind::Unary) {
        auto& u = static_cast<UnaryExpr&>(*v);
        if (u.op == TokKind::Minus && u.operand) {
            if (u.operand->kind == ExprKind::IntLit) {
                static_cast<IntLit&>(*u.operand).value = -static_cast<IntLit&>(*u.operand).value;
                v = std::move(u.operand);
            } else if (u.operand->kind == ExprKind::DoubleLit) {
                static_cast<DoubleLit&>(*u.operand).value = -static_cast<DoubleLit&>(*u.operand).value;
                v = std::move(u.operand);
            }
        }
    }
    switch (v->kind) {
        case ExprKind::IntLit: case ExprKind::DoubleLit:
        case ExprKind::BoolLit: case ExprKind::StrLit: break;
        // An array literal `[lit, ...]` -> a `const NAME: Array[T] = [...]` table (the checker
        // validates it's an Array[T] const with literal scalar elements; element minus-folding
        // and pub-ban live there, since the element/type context is known only after checking).
        case ExprKind::ListLit: break;
        default: error("a const initializer must be a literal (Int, Double, Bool, String) or an array literal [ ... ]");
    }
    c->value = std::move(v);
    return c;
}

ItemPtr Parser::parse_struct_item() {
    const Token& kw = expect(TokKind::KwStruct, "'struct'");
    auto s = std::make_unique<StructItem>();
    s->line = kw.line; s->col = kw.col;
    const Token& nm = expect(TokKind::UIdent, "struct name");
    s->name = nm.text; s->name_line = nm.line; s->name_col = nm.col;
    s->generics = parse_generic_params();

    if (check(TokKind::LBrace)) {                  // record form: { f: T, ... }
        layout_brace(ParseLayout::Brace::Fields);
        const Token& open = advance();
        s->is_tuple = false;
        skip_terminators();
        while (!check(TokKind::RBrace)) {
            if (body_unclosed(open)) return s;
            if (at_end()) error("unterminated struct body");
            const size_t start = pos_;
            try {
                Field f;
                f.name = expect(TokKind::LIdent, "field name").text;
                expect(TokKind::Colon, "':'");
                f.type = parse_type();
                s->fields.push_back(std::move(f));
            } catch (const ParseError& e) {
                if (!recover_) throw;
                recover(e, Sync::Field, start, &open);
            }
            accept(TokKind::Comma);
            skip_terminators();
        }
        close_body(open);
    } else if (accept(TokKind::LParen)) {          // positional tuple form: ( T0, T1 )
        s->is_tuple = true;
        s->fields = parse_positional_fields();
    } else {                                       // nullary: struct Name
        s->is_tuple = true;
    }
    return s;
}

ItemPtr Parser::parse_enum_item() {
    const Token& kw = expect(TokKind::KwEnum, "'enum'");
    auto en = std::make_unique<EnumItem>();
    en->line = kw.line; en->col = kw.col;
    const Token& nm = expect(TokKind::UIdent, "enum name");
    en->name = nm.text; en->name_line = nm.line; en->name_col = nm.col;
    en->generics = parse_generic_params();
    if (accept(TokKind::Colon)) {              // `enum Name : Int` -- int-backed repr annotation (opt-in)
        en->is_int_backed = true;
        en->repr = expect(TokKind::UIdent, "an enum repr type (Int)").text;
    }
    layout_brace(ParseLayout::Brace::Fields);
    const Token& open = expect(TokKind::LBrace, "'{'");
    skip_terminators();
    while (!check(TokKind::RBrace)) {
        if (body_unclosed(open)) return en;
        if (at_end()) error("unterminated enum body");
        const size_t start = pos_;
        try {
            EnumVariant v;
            const Token& vt = expect(TokKind::UIdent, "variant name");
            v.name = vt.text; v.line = vt.line; v.col = vt.col;
            if (accept(TokKind::LParen)) {             // V(T0, T1) -- positional
                v.is_tuple = true;
                v.fields = parse_positional_fields();
            } else if (accept(TokKind::LBrace)) {      // V { f: T } -- record
                v.is_tuple = false;
                skip_terminators();
                while (!check(TokKind::RBrace)) {
                    if (at_end()) error("unterminated variant body");
                    Field f;
                    f.name = expect(TokKind::LIdent, "field name").text;
                    expect(TokKind::Colon, "':'");
                    f.type = parse_type();
                    v.fields.push_back(std::move(f));
                    accept(TokKind::Comma);
                    skip_terminators();
                }
                expect(TokKind::RBrace, "'}'");
            } else {
                v.is_tuple = true;                     // V -- nullary
                if (accept(TokKind::Assign)) {         // `V = <intlit>` -- explicit discriminant (int-backed enums)
                    const bool neg = accept(TokKind::Minus);
                    const Token& d = expect(TokKind::Int, "an integer discriminant");
                    v.has_disc = true;
                    v.disc = neg ? -d.int_val : d.int_val;
                }
            }
            en->variants.push_back(std::move(v));
        } catch (const ParseError& e) {
            if (!recover_) throw;
            recover(e, Sync::Field, start, &open);
        }
        accept(TokKind::Comma);
        skip_terminators();
    }
    close_body(open);
    return en;
}

ItemPtr Parser::parse_trait_item() {
    const Token& kw = expect(TokKind::KwTrait, "'trait'");
    auto tr = std::make_unique<TraitDecl>();
    tr->line = kw.line; tr->col = kw.col;
    const Token& nm = expect(TokKind::UIdent, "trait name");
    tr->name = nm.text; tr->name_line = nm.line; tr->name_col = nm.col;
    tr->generics = parse_generic_params();         // trait Iterable[T] ...
    if (accept(TokKind::Colon)) {                  // supertraits: `: A, B`
        auto add_super = [&] {
            const Token& st = expect(TokKind::UIdent, "supertrait name");
            tr->supertraits.push_back(st.text);
            tr->supertrait_pos.emplace_back(st.line, st.col);
        };
        add_super();
        while (accept(TokKind::Comma)) {
            if (check(TokKind::LBrace)) break;
            add_super();
        }
    }
    layout_brace(ParseLayout::Brace::Members);
    const Token& open = expect(TokKind::LBrace, "'{'");
    skip_terminators();
    while (!check(TokKind::RBrace)) {
        if (body_unclosed(open, item_col_, /*members_are_fns=*/true)) return tr;
        if (at_end()) error("unterminated trait body");
        const size_t start = pos_;
        layout_start();
        try {
            tr->methods.push_back(parse_method(/*require_body=*/false));
        } catch (const ParseError& e) {
            if (!recover_) throw;
            no_struct_lit_ = false;
            recover(e, Sync::Member, start, &open);
        }
        skip_terminators();
    }
    close_body(open);
    return tr;
}

ItemPtr Parser::parse_impl_item() {
    const Token& kw = expect(TokKind::KwImpl, "'impl'");
    auto im = std::make_unique<ImplDecl>();
    im->line = kw.line; im->col = kw.col;
    im->generics = parse_generic_params();         // impl[T] ...
    // Parse the head UIdent + optional `[...]`; it is either the trait (`Trait[args] for Target`)
    // or -- for a traitless inherent impl -- the target type itself (`Target[args] { … }`). The
    // following `for` vs `{` disambiguates.
    const Token& headTok = expect(TokKind::UIdent, "trait or type name");
    std::string head = headTok.text;
    uint32_t hl = headTok.line, hc = headTok.col;
    std::vector<TypePtr> head_args;
    if (accept(TokKind::LBracket)) {
        if (!check(TokKind::RBracket)) {
            head_args.push_back(parse_type());
            while (accept(TokKind::Comma)) {
                if (check(TokKind::RBracket)) break;
                head_args.push_back(parse_type());
            }
        }
        expect(TokKind::RBracket, "']'");
    }
    if (accept(TokKind::KwFor)) {                   // `impl[G] Trait[args] for Target { … }`
        im->trait_name = std::move(head);
        im->trait_line = hl; im->trait_col = hc;
        im->trait_args = std::move(head_args);
        im->target = parse_type();                 // e.g. List[T]
    } else {                                        // `impl[G] Target[args] { … }` -- inherent (traitless)
        im->is_inherent = true;                     // trait_name stays empty
        auto nt = std::make_unique<NamedType>();
        nt->line = hl; nt->col = hc; nt->name_line = hl; nt->name_col = hc;
        nt->name = std::move(head); nt->args = std::move(head_args);
        im->target = std::move(nt);
    }
    layout_brace(ParseLayout::Brace::Members);
    const Token& open = expect(TokKind::LBrace, "'{'");
    skip_terminators();
    while (!check(TokKind::RBrace)) {
        if (body_unclosed(open, item_col_, /*members_are_fns=*/true)) return im;
        if (at_end()) error("unterminated impl body");
        const size_t start = pos_;
        layout_start();
        try {
            im->methods.push_back(parse_method(/*require_body=*/true));
        } catch (const ParseError& e) {
            if (!recover_) throw;
            no_struct_lit_ = false;
            recover(e, Sync::Member, start, &open);
        }
        skip_terminators();
    }
    close_body(open);
    return im;
}

// `import a::b::c` -- module path only (all segments lowercase); no name tail.
ItemPtr Parser::parse_import_item() {
    const Token& kw = expect(TokKind::KwImport, "'import'");
    auto im = std::make_unique<ImportItem>();
    im->line = kw.line; im->col = kw.col;
    im->path.push_back(expect(TokKind::LIdent, "module name").text);
    while (accept(TokKind::ColonColon))
        im->path.push_back(expect(TokKind::LIdent, "module path segment").text);
    return im;
}

// `use a::b::{x, Y}` (list) / `use a::b::*` (glob) / `use a::b::name` (single). The
// module path is `lident (:: lident)*`; a `::` may instead open the `{ }` / `*` tail.
// For the single-name form the LAST `::`-segment is the imported name. Case-validation
// of the segments (module lowercase, name either) is deferred to the resolver (Slice 2).
ItemPtr Parser::parse_use_item() {
    const Token& kw = expect(TokKind::KwUse, "'use'");
    auto u = std::make_unique<UseItem>();
    u->line = kw.line; u->col = kw.col;
    std::vector<std::string> segs;
    uint32_t last_line = 0, last_col = 0;   // the last segment: the imported name of `use a::name`
    segs.push_back(expect(TokKind::LIdent, "module name").text);
    bool tail_done = false;
    while (accept(TokKind::ColonColon)) {
        if (check(TokKind::LBrace)) {                       // `::{ x, Y }`
            advance();
            if (!check(TokKind::RBrace)) {
                for (;;) {
                    if (check(TokKind::LIdent) || check(TokKind::UIdent)) {
                        u->name_pos.emplace_back(cur().line, cur().col);
                        u->names.push_back(advance().text);
                    }
                    else error("expected an imported name inside '{ }'");
                    if (!accept(TokKind::Comma)) break;
                    if (check(TokKind::RBrace)) break;       // trailing comma
                }
            }
            expect(TokKind::RBrace, "'}'");
            u->path = std::move(segs);
            tail_done = true;
            break;
        }
        if (check(TokKind::Star)) {                          // `::*` glob
            advance();
            u->glob = true;
            u->path = std::move(segs);
            tail_done = true;
            break;
        }
        if (check(TokKind::LIdent) || check(TokKind::UIdent)) { // an ordinary path segment / final name
            last_line = cur().line; last_col = cur().col;
            segs.push_back(advance().text);
        }
        else
            error("expected a module path segment or name after '::'");
    }
    if (!tail_done) {                                        // `use a::b` single-name form
        if (segs.size() < 2)
            error("'use' needs a module path and a name (e.g. `use util::helper`)");
        u->names.push_back(segs.back());
        u->name_pos.emplace_back(last_line, last_col);
        segs.pop_back();
        u->path = std::move(segs);
    }
    return u;
}

Method Parser::parse_method(bool require_body) {
    Method m;
    // `pub fn` is the Rust habit for an exported method. Members carry no visibility of their own, and the
    // bare "expected 'fn'" this used to report pointed at `pub` without saying why it is wrong there.
    if (check(TokKind::KwPub))
        error("'pub' is not allowed on a method -- a method is visible wherever its type or trait is; "
              "mark the type or trait `pub` instead");
    expect(TokKind::KwFn, "'fn'");
    const Token& nm = expect(TokKind::LIdent, "method name");
    m.name = nm.text; m.line = nm.line; m.col = nm.col;
    m.generics = parse_generic_params();
    expect(TokKind::LParen, "'('");
    // method_params = [ "mut" ] "self" { "," param } | param_list
    // ( `mut param` starts with KwMut too, disambiguated by the token after `mut`. )
    if (check(TokKind::KwMut) && peek(1).kind == TokKind::KwSelf) {
        m.self_mut = true; m.self_line = cur().line; m.self_col = cur().col;
        advance();  // 'mut'
        advance();  // 'self'
        m.has_self = true;
        while (accept(TokKind::Comma)) {
            if (check(TokKind::RParen)) break;
            m.params.push_back(parse_param());
        }
    } else if (check(TokKind::KwSelf)) {
        m.has_self = true;
        m.self_line = cur().line; m.self_col = cur().col;
        advance();
        while (accept(TokKind::Comma)) {
            if (check(TokKind::RParen)) break;
            m.params.push_back(parse_param());
        }
    } else if (!check(TokKind::RParen)) {
        m.params.push_back(parse_param());
        while (accept(TokKind::Comma)) {
            if (check(TokKind::RParen)) break;
            m.params.push_back(parse_param());
        }
    }
    expect(TokKind::RParen, "')'");
    expect(TokKind::Arrow, "'->' (return type is mandatory)");
    m.ret = parse_type();
    if (check(TokKind::LBrace) || (recover_ && require_body)) m.body = parse_block();
    else if (require_body)      error("impl method requires a body");
    return m;
}

// ---- item helpers -----------------------------------------------------------

Param Parser::parse_param() {
    Param p;
    if (check(TokKind::KwMut)) { p.is_mut = true; p.line = cur().line; p.col = cur().col; advance(); }
    const Token& nm = expect(TokKind::LIdent, "parameter name");
    p.name = nm.text; p.name_line = nm.line; p.name_col = nm.col;
    expect(TokKind::Colon, "':' (parameter type is mandatory)");
    p.type = parse_type();
    return p;
}

std::vector<Param> Parser::parse_param_list() {
    expect(TokKind::LParen, "'('");
    std::vector<Param> params;
    if (!check(TokKind::RParen)) {
        params.push_back(parse_param());
        while (accept(TokKind::Comma)) {
            if (check(TokKind::RParen)) break;
            params.push_back(parse_param());
        }
    }
    expect(TokKind::RParen, "')'");
    return params;
}

std::vector<GenericParam> Parser::parse_generic_params() {
    std::vector<GenericParam> gs;
    if (!accept(TokKind::LBracket)) return gs;      // absent -> empty
    // A single bound reference: `Display` or a parametric `Iterable[T]` (args reuse
    // the ordinary type grammar).
    auto parse_bound_ref = [&]() -> BoundRef {
        BoundRef br;
        const Token& bt = expect(TokKind::UIdent, "trait bound");
        br.trait = bt.text; br.line = bt.line; br.col = bt.col;
        if (accept(TokKind::LBracket)) {           // parametric bound: `Iterable[T]`
            if (!check(TokKind::RBracket)) {
                br.args.push_back(parse_type());
                while (accept(TokKind::Comma)) {
                    if (check(TokKind::RBracket)) break;
                    br.args.push_back(parse_type());
                }
            }
            expect(TokKind::RBracket, "']'");
        }
        return br;
    };
    while (!check(TokKind::RBracket)) {
        if (at_end()) error("unterminated generic parameter list");
        GenericParam gp;
        gp.name = expect(TokKind::UIdent, "type parameter name").text;
        if (accept(TokKind::Colon)) {              // bound(s): `: A + B` or `: Iterable[T]`
            gp.bounds.push_back(parse_bound_ref());
            while (accept(TokKind::Plus))
                gp.bounds.push_back(parse_bound_ref());
        }
        gs.push_back(std::move(gp));
        if (!accept(TokKind::Comma)) break;
    }
    expect(TokKind::RBracket, "']'");
    return gs;
}

// `( T0, T1 )` -- assumes the `(` is already consumed; consumes the `)`. Fields
// get synthesized positional names `_0`, `_1`, ... (tuple struct / tuple variant).
std::vector<Field> Parser::parse_positional_fields() {
    std::vector<Field> fields;
    size_t i = 0;
    while (!check(TokKind::RParen)) {
        if (at_end()) error("unterminated field list");
        Field f;
        f.name = "_" + std::to_string(i++);
        f.type = parse_type();
        fields.push_back(std::move(f));
        if (!accept(TokKind::Comma)) break;
    }
    expect(TokKind::RParen, "')'");
    return fields;
}

// ---- statements -------------------------------------------------------------

StmtPtr Parser::parse_stmt() {
    if (check(TokKind::KwLet)) return parse_let();

    ExprPtr e = parse_expr(0);
    // `target = value`, or a compound `target op= value`. The operator is recorded on the node (see
    // AssignStmt) rather than desugared, so the place is evaluated exactly once.
    if (check(TokKind::Assign)     || check(TokKind::PlusEq)   || check(TokKind::MinusEq)  ||
        check(TokKind::StarEq)     || check(TokKind::SlashEq)  || check(TokKind::PercentEq) ||
        check(TokKind::BitAndEq)   || check(TokKind::BitOrEq)  || check(TokKind::BitXorEq) ||
        check(TokKind::ShlEq)      || check(TokKind::ShrEq)    || check(TokKind::UShrEq)) {
        auto a = std::make_unique<AssignStmt>();
        a->line = cur().line; a->col = cur().col;
        // `op=` records the BASE operator, so everything downstream (the checker's type rule, the
        // codegen's `arith_opcode`) sees the same op the plain binary form would produce.
        switch (cur().kind) {
            case TokKind::PlusEq:    a->op = TokKind::Plus;    break;
            case TokKind::MinusEq:   a->op = TokKind::Minus;   break;
            case TokKind::StarEq:    a->op = TokKind::Star;    break;
            case TokKind::SlashEq:   a->op = TokKind::Slash;   break;
            case TokKind::PercentEq: a->op = TokKind::Percent; break;
            case TokKind::BitAndEq:  a->op = TokKind::BitAnd;  break;
            case TokKind::BitOrEq:   a->op = TokKind::BitOr;   break;
            case TokKind::BitXorEq:  a->op = TokKind::BitXor;  break;
            case TokKind::ShlEq:     a->op = TokKind::Shl;     break;
            case TokKind::ShrEq:     a->op = TokKind::Shr;     break;
            case TokKind::UShrEq:    a->op = TokKind::UShr;    break;
            default:                 a->op = TokKind::Assign;  break;   // the plain `=`
        }
        advance();                                 // '=' / '+=' / ...
        a->target = std::move(e);
        a->value = parse_expr(0);
        return a;
    }
    auto es = std::make_unique<ExprStmt>();
    if (e) { es->line = e->line; es->col = e->col; }
    es->expr = std::move(e);
    return es;
}

StmtPtr Parser::parse_let() {
    const Token& kw = expect(TokKind::KwLet, "'let'");
    auto l = std::make_unique<LetStmt>();
    l->line = kw.line; l->col = kw.col;
    if (accept(TokKind::KwMut)) l->is_mut = true;
    l->pat = parse_pattern();
    if (accept(TokKind::Colon)) l->type = parse_type();
    expect(TokKind::Assign, "'='");
    l->init = parse_expr(0);
    return l;
}

// ---- expressions (Pratt) ----------------------------------------------------

namespace {

constexpr int UNARY_BP   = 11;   // prefix `- ! ~` -- tighter than any binary op
constexpr int POSTFIX_BP = 100;  // call / index / field / `?` -- tightest

int infix_bp(TokKind k) {
    switch (k) {
    case TokKind::Pipe:                                                        return 1;
    case TokKind::OrOr:                                                        return 2;
    case TokKind::AndAnd:                                                      return 3;
    case TokKind::EqEq: case TokKind::NotEq:
    case TokKind::Lt:   case TokKind::Le: case TokKind::Gt: case TokKind::Ge:  return 4;
    case TokKind::BitOr:                                                       return 5;
    case TokKind::BitXor:                                                      return 6;
    case TokKind::BitAnd:                                                      return 7;
    case TokKind::Shl: case TokKind::Shr: case TokKind::UShr:                  return 8;
    case TokKind::Plus: case TokKind::Minus:                                   return 9;
    case TokKind::Star: case TokKind::Slash: case TokKind::Percent:            return 10;
    default:                                                                   return 0;
    }
}

} // namespace

ExprPtr Parser::parse_expr(int min_bp) {
    ExprPtr lhs = parse_prefix();

    for (;;) {
        TokKind k = cur().kind;

        // ---- postfix (bind tightest) ----
        if (k == TokKind::LParen) {
            if (POSTFIX_BP < min_bp) break;
            auto call = std::make_unique<CallExpr>();
            call->line = cur().line; call->col = cur().col;
            call->callee = std::move(lhs);
            call->args = parse_arg_list();
            lhs = std::move(call);
            continue;
        }
        if (k == TokKind::LBracket) {
            if (POSTFIX_BP < min_bp) break;
            auto ix = std::make_unique<IndexExpr>();
            ix->line = cur().line; ix->col = cur().col;
            advance();                             // '['
            ix->obj = std::move(lhs);
            ix->index = parse_expr(0);
            expect(TokKind::RBracket, "']'");
            lhs = std::move(ix);
            continue;
        }
        if (k == TokKind::Dot) {
            if (POSTFIX_BP < min_bp) break;
            const uint32_t dl = cur().line, dc = cur().col;
            advance();                             // '.'
            if (check(TokKind::Int)) {             // `.N` -- tuple index (`t.0`)
                const Token& it = cur();
                if (it.int_val < 0 || it.int_val > 4095)
                    throw ParseError("tuple index out of range", it.line, it.col);
                auto fld = std::make_unique<FieldExpr>();
                fld->line = dl; fld->col = dc;
                fld->obj = std::move(lhs);
                fld->tuple_index = true;
                fld->index = static_cast<uint32_t>(it.int_val);
                fld->name_line = it.line; fld->name_col = it.col;
                fld->name  = "_" + std::to_string(it.int_val);
                advance();
                lhs = std::move(fld);
                continue;
            }
            auto fld = std::make_unique<FieldExpr>();   // `.name` -- field access
            fld->line = dl; fld->col = dc;
            fld->obj = std::move(lhs);
            const Token& nm = expect(TokKind::LIdent, "field name");
            fld->name = nm.text; fld->name_line = nm.line; fld->name_col = nm.col;
            lhs = std::move(fld);
            continue;
        }
        if (k == TokKind::Question) {              // `e?` -- first-class try
            if (POSTFIX_BP < min_bp) break;
            auto t = std::make_unique<TryExpr>();
            t->line = cur().line; t->col = cur().col;
            advance();                             // '?'
            t->operand = std::move(lhs);
            lhs = std::move(t);
            continue;
        }

        // ---- infix ----
        int bp = infix_bp(k);
        if (bp == 0 || bp < min_bp) break;
        const Token& op = cur();
        const uint32_t ol = op.line, oc = op.col;
        advance();
        ExprPtr rhs = parse_expr(bp + 1);          // left-associative
        if (k == TokKind::Pipe) {
            auto p = std::make_unique<PipeExpr>();
            p->line = ol; p->col = oc;
            p->lhs = std::move(lhs); p->rhs = std::move(rhs);
            lhs = std::move(p);
        } else {
            auto b = std::make_unique<BinaryExpr>();
            b->line = ol; b->col = oc; b->op = k;
            b->lhs = std::move(lhs); b->rhs = std::move(rhs);
            lhs = std::move(b);
        }
    }
    return lhs;
}

ExprPtr Parser::parse_prefix() {
    const Token& t = cur();
    switch (t.kind) {
    case TokKind::Int:    { auto n = std::make_unique<IntLit>();    n->line=t.line; n->col=t.col; n->value=t.int_val;    advance(); return n; }
    case TokKind::Double: { auto n = std::make_unique<DoubleLit>(); n->line=t.line; n->col=t.col; n->value=t.double_val; advance(); return n; }
    case TokKind::Str:    { auto n = std::make_unique<StrLit>();    n->line=t.line; n->col=t.col; n->value=t.text;       advance(); return n; }
    case TokKind::InterpStrBegin: return parse_interp_string();
    case TokKind::KwTrue: { auto n = std::make_unique<BoolLit>();   n->line=t.line; n->col=t.col; n->value=true;         advance(); return n; }
    case TokKind::KwFalse:{ auto n = std::make_unique<BoolLit>();   n->line=t.line; n->col=t.col; n->value=false;        advance(); return n; }
    case TokKind::KwSelf: { auto n = std::make_unique<IdentExpr>(); n->line=t.line; n->col=t.col; n->name="self";
                            n->name_line=t.line; n->name_col=t.col; advance(); return n; }

    case TokKind::LIdent: {
        auto id = std::make_unique<IdentExpr>();
        id->line = t.line; id->col = t.col; id->name = t.text; id->upper = false;
        id->name_line = t.line; id->name_col = t.col;
        advance();
        if (accept(TokKind::ColonColon)) {             // module-qualified path `mod::name`
            id->qualifier = std::move(id->name);        // the module (lowercase head)
            const Token& tail = cur();
            id->name_line = tail.line; id->name_col = tail.col;
            if (tail.kind == TokKind::LIdent)      { id->name = tail.text; id->upper = false; advance(); }
            else if (tail.kind == TokKind::UIdent) {
                id->name = tail.text; id->upper = true; advance();
                if (!no_struct_lit_ && check(TokKind::LBrace))   // `mod::Name { ... }` struct literal
                    return parse_struct_lit(std::move(id->qualifier), std::move(id->name), id->line, id->col,
                                            id->name_line, id->name_col);
            }
            else error("expected a function or constructor name after 'module::'");
        }
        return id;   // bare lident, or a module-qualified `mod::name` (upper set by the tail's case)
    }
    case TokKind::UIdent: {
        auto id = std::make_unique<IdentExpr>();
        id->line = t.line; id->col = t.col; id->name = t.text; id->upper = true;
        id->name_line = t.line; id->name_col = t.col;
        advance();
        if (accept(TokKind::ColonColon)) {             // qualified path: `Trait::method` or `Enum::Variant`
            id->qualifier = std::move(id->name);
            id->name_line = cur().line; id->name_col = cur().col;
            if (cur().kind == TokKind::UIdent) {       // `Enum::Variant` (S1) -- an enum-variant path
                id->name = cur().text; id->upper = true; advance();
                if (!no_struct_lit_ && check(TokKind::LBrace))   // `Enum::Variant { ... }` record variant
                    return parse_struct_lit(std::move(id->qualifier), std::move(id->name), id->line, id->col,
                                            id->name_line, id->name_col);
                return id;
            }
            id->name = expect(TokKind::LIdent, "method or variant name after '::'").text;
            id->upper = false;                         // `Trait::method` (lowercase tail)
            return id;
        }
        if (!no_struct_lit_ && check(TokKind::LBrace))  // struct literal Name { ... }
            return parse_struct_lit("", std::move(id->name), id->line, id->col, id->line, id->col);
        return id;                                     // bare constructor / type value (None)
    }

    case TokKind::Minus:
    case TokKind::Not:
    case TokKind::Tilde: {
        auto u = std::make_unique<UnaryExpr>();
        u->line = t.line; u->col = t.col; u->op = t.kind;
        advance();
        u->operand = parse_expr(UNARY_BP);
        return u;
    }

    case TokKind::LParen:   return parse_paren();
    case TokKind::LBracket: return parse_list();
    case TokKind::Hash:     return parse_map_lit();
    case TokKind::LBrace:   return parse_block();
    case TokKind::KwIf:     return parse_if();
    case TokKind::KwMatch:  return parse_match();
    case TokKind::KwWhile:  return parse_while();
    case TokKind::KwFor:    return parse_for();
    case TokKind::KwLoop:   return parse_loop();
    case TokKind::KwFn:     return parse_lambda();

    case TokKind::KwBreak: {
        auto n = std::make_unique<BreakExpr>();
        n->line = t.line; n->col = t.col;
        advance();
        // Optional value, on the SAME line (ASI inserts StmtEnd after a bare `break`) -- the
        // terminator set mirrors `return` below. A value is `loop`-only; the checker enforces that.
        const bool stop = check(TokKind::StmtEnd) || check(TokKind::RBrace) ||
                          check(TokKind::RParen)  || check(TokKind::RBracket) ||
                          check(TokKind::Comma)   || at_end();
        if (!stop) n->value = parse_expr(0);
        return n;
    }
    case TokKind::KwContinue: { auto n = std::make_unique<ContinueExpr>(); n->line=t.line; n->col=t.col; advance(); return n; }
    case TokKind::KwReturn: {
        auto n = std::make_unique<ReturnExpr>();
        n->line = t.line; n->col = t.col;
        advance();
        // Optional value, on the SAME line (ASI inserts StmtEnd after a bare `return`).
        const bool stop = check(TokKind::StmtEnd) || check(TokKind::RBrace) ||
                          check(TokKind::RParen)  || check(TokKind::RBracket) ||
                          check(TokKind::Comma)   || at_end();
        if (!stop) n->value = parse_expr(0);
        return n;
    }
    default:
        error("expected an expression");
    }
}

std::vector<ExprPtr> Parser::parse_arg_list() {
    expect(TokKind::LParen, "'('");
    std::vector<ExprPtr> args;
    const bool save = no_struct_lit_; no_struct_lit_ = false;
    if (!check(TokKind::RParen)) {
        args.push_back(parse_expr(0));
        while (accept(TokKind::Comma)) {
            if (check(TokKind::RParen)) break;
            args.push_back(parse_expr(0));
        }
    }
    no_struct_lit_ = save;
    expect(TokKind::RParen, "')'");
    return args;
}

ExprPtr Parser::parse_paren() {
    const Token& kw = expect(TokKind::LParen, "'('");
    const bool save = no_struct_lit_; no_struct_lit_ = false;
    if (check(TokKind::RParen)) {                   // `()` -- unit (empty tuple)
        advance();
        no_struct_lit_ = save;
        auto u = std::make_unique<TupleExpr>();
        u->line = kw.line; u->col = kw.col;
        return u;
    }
    ExprPtr first = parse_expr(0);
    if (check(TokKind::Comma)) {                    // tuple
        auto tup = std::make_unique<TupleExpr>();
        tup->line = kw.line; tup->col = kw.col;
        tup->elems.push_back(std::move(first));
        while (accept(TokKind::Comma)) {
            if (check(TokKind::RParen)) break;
            tup->elems.push_back(parse_expr(0));
        }
        no_struct_lit_ = save;
        expect(TokKind::RParen, "')'");
        return tup;
    }
    no_struct_lit_ = save;
    expect(TokKind::RParen, "')'");
    return first;                                   // grouping
}

ExprPtr Parser::parse_list() {
    const Token& kw = expect(TokKind::LBracket, "'['");
    auto l = std::make_unique<ListLit>();
    l->line = kw.line; l->col = kw.col;
    const bool save = no_struct_lit_; no_struct_lit_ = false;
    if (!check(TokKind::RBracket)) {
        l->elems.push_back(parse_expr(0));
        while (accept(TokKind::Comma)) {
            if (check(TokKind::RBracket)) break;
            l->elems.push_back(parse_expr(0));
        }
    }
    no_struct_lit_ = save;
    expect(TokKind::RBracket, "']'");
    return l;
}

ExprPtr Parser::parse_map_lit() {
    const Token& kw = expect(TokKind::Hash, "'#'");
    expect(TokKind::LBrace, "'{' (map literal)");
    auto m = std::make_unique<MapLit>();
    m->line = kw.line; m->col = kw.col;
    const bool save = no_struct_lit_; no_struct_lit_ = false;
    skip_terminators();
    while (!check(TokKind::RBrace)) {
        if (at_end()) error("unterminated map literal");
        ExprPtr key = parse_expr(0);
        expect(TokKind::FatArrow, "'=>'");
        ExprPtr val = parse_expr(0);
        m->entries.emplace_back(std::move(key), std::move(val));
        accept(TokKind::Comma);
        skip_terminators();
    }
    no_struct_lit_ = save;
    expect(TokKind::RBrace, "'}'");
    return m;
}

ExprPtr Parser::parse_struct_lit(std::string qualifier, std::string name, uint32_t line, uint32_t col,
                                 uint32_t name_line, uint32_t name_col) {
    expect(TokKind::LBrace, "'{'");
    auto s = std::make_unique<StructLit>();
    s->line = line; s->col = col; s->name = std::move(name); s->qualifier = std::move(qualifier);
    s->name_line = name_line; s->name_col = name_col;
    const bool save = no_struct_lit_; no_struct_lit_ = false;
    skip_terminators();
    while (!check(TokKind::RBrace)) {
        if (at_end()) error("unterminated struct literal");
        if (accept(TokKind::DotDot)) {                          // record update `..base` -- must be last
            s->base = parse_expr(0);
            skip_terminators();
            break;                                              // nothing may follow the base
        }
        FieldInit fi;
        fi.name = expect(TokKind::LIdent, "field name").text;
        if (accept(TokKind::Colon)) fi.value = parse_expr(0);   // else shorthand `{ x }`
        s->fields.push_back(std::move(fi));
        accept(TokKind::Comma);
        skip_terminators();
    }
    no_struct_lit_ = save;
    expect(TokKind::RBrace, "'}'");                             // a field/comma after `..base` fails here
    return s;
}

ExprPtr Parser::parse_if() {
    const Token& kw = expect(TokKind::KwIf, "'if'");
    if (check(TokKind::KwLet)) return parse_if_let(kw);   // `if let PAT = E {…}` sugar
    auto e = std::make_unique<IfExpr>();
    e->line = kw.line; e->col = kw.col;
    const bool save = no_struct_lit_; no_struct_lit_ = true;
    e->cond = parse_expr(0);
    no_struct_lit_ = save;
    e->then_blk = parse_block();
    if (accept(TokKind::KwElse)) e->else_blk = parse_else_branch();
    return e;
}

// An `else` tail: an `else if …` / `else if let …` chain, or an `else { … }` block. `else` (like Rust)
// requires a block or a chained `if`; a braceless `else match`/`else while`/`else for`/`else loop` is a
// common mistake, so give a tailored "wrap it in braces" diagnostic instead of the bare "expected '{'".
ExprPtr Parser::parse_else_branch() {
    if (check(TokKind::KwIf)) return parse_if();          // `else if` / `else if let`
    if (!check(TokKind::LBrace)) {
        const Token& t = cur();
        if (t.kind == TokKind::KwMatch || t.kind == TokKind::KwWhile ||
            t.kind == TokKind::KwFor   || t.kind == TokKind::KwLoop) {
            const char* w = to_string(t.kind);
            error(std::string("`else` must be followed by a block or `if`; wrap the `") + w +
                  "` in braces: `else { " + w + " … }`");
        }
    }
    return parse_block();                                 // requires `{` (emits its own error otherwise)
}

// `if let PAT = EXPR { THEN } [else { ELSE }]` -- refutable single-pattern conditional binding.
// A pure DESUGAR to a 2-arm `match EXPR { PAT => THEN, _ => (ELSE | ()) }`: the `_` arm makes it
// exhaustive, PAT's bindings scope to THEN, and it composes with the discard/void context (statement
// position drops each arm; value position joins them). `from_if_let` lets the coverage checker give a
// tailored message when PAT is irrefutable (the `_` arm is then unreachable).
ExprPtr Parser::parse_if_let(const Token& kw) {
    expect(TokKind::KwLet, "'let'");
    PatPtr pat = parse_arm_pattern();
    expect(TokKind::Assign, "'='");
    const bool save = no_struct_lit_; no_struct_lit_ = true;   // scrutinee before `{` -- as if/while/match
    ExprPtr scrut = parse_expr(0);
    no_struct_lit_ = save;
    ExprPtr then_blk = parse_block();
    ExprPtr else_blk;
    if (accept(TokKind::KwElse)) else_blk = parse_else_branch();   // `else if`/`else if let` chains or a block
    auto m = std::make_unique<MatchExpr>();
    m->line = kw.line; m->col = kw.col; m->from_if_let = true;
    m->scrut = std::move(scrut);
    MatchArm hit; hit.pat = std::move(pat); hit.body = std::move(then_blk);
    m->arms.push_back(std::move(hit));
    auto wild = std::make_unique<WildcardPat>(); wild->line = kw.line; wild->col = kw.col;
    MatchArm miss; miss.pat = std::move(wild);
    if (else_blk) miss.body = std::move(else_blk);
    else {
        auto unit = std::make_unique<TupleExpr>(); unit->line = kw.line; unit->col = kw.col;
        miss.body = std::move(unit);
    }
    m->arms.push_back(std::move(miss));
    return m;
}

// `"t0 ${e0} t1 ${e1} t2"` -- string interpolation. A pure DESUGAR to a left-associative `+`-chain of
// literal text pieces and `toString(e_i)` calls (empty text chunks dropped): `"t0" + toString(e0) +
// "t1" + toString(e1) + "t2"`. Reuses StrLit / BinaryExpr(+) / CallExpr(toString) -- no new AST node,
// and no checker/codegen/VM change: `toString` lowers to the total TO_STRING (renders ANY type incl.
// container dumps -- so `"${struct}"` works where `"x" + struct` does not), and `String + String`
// lowers to ADD (concat). The lexer delivers InterpStrBegin(t0) <e0> ( InterpStrMid(t_i) <e_i> )*
// InterpStrEnd(t_n); a hole expression is parsed by the ordinary expression layer, so a NESTED
// interpolated string inside a hole recurses here for free. `cur()` is the InterpStrBegin.
ExprPtr Parser::parse_interp_string() {
    const Token begin = cur();                     // carries the leading text chunk
    const uint32_t line = begin.line, col = begin.col;
    advance();

    ExprPtr acc;                                   // running `+`-chain (null until the first piece)
    auto add_piece = [&](ExprPtr piece) {
        if (!acc) { acc = std::move(piece); return; }
        auto b = std::make_unique<BinaryExpr>();
        b->line = line; b->col = col; b->op = TokKind::Plus;
        b->lhs = std::move(acc); b->rhs = std::move(piece);
        acc = std::move(b);
    };
    auto add_text = [&](const std::string& s, uint32_t l, uint32_t c) {
        if (s.empty()) return;                     // drop empty text chunks (adjacent/leading/trailing holes)
        auto lit = std::make_unique<StrLit>();
        lit->line = l; lit->col = c; lit->value = s;
        add_piece(std::move(lit));
    };
    // A plain hole `${e}` -> `toString(e)`; a spec hole `${e:spec}` -> `format(e, "spec")` (the `Format`
    // trait, resolved on e's runtime type -- scalars only). `fn_name` picks between them.
    auto add_call1 = [&](const char* fn_name, ExprPtr e, uint32_t l, uint32_t c) {
        auto call = std::make_unique<CallExpr>();
        call->line = l; call->col = c;
        auto callee = std::make_unique<IdentExpr>();
        callee->line = l; callee->col = c; callee->name = fn_name; callee->upper = false;
        callee->name_line = l; callee->name_col = c;
        call->callee = std::move(callee);
        call->args.push_back(std::move(e));
        return call;
    };

    add_text(begin.text, line, col);
    for (;;) {
        if (check(TokKind::InterpStrMid) || check(TokKind::InterpStrEnd))
            error("empty interpolation `${}` -- expected an expression");
        const uint32_t hl = cur().line, hc = cur().col;
        ExprPtr he = parse_expr(0);                // the hole expression (stops at Mid/End)
        const Token& closer = cur();               // the Mid/End that stopped it carries this hole's spec
        if (closer.has_spec) {
            if (closer.spec.empty())
                error("empty format specifier after ':' -- write `${e}` for default formatting");
            validate_format_spec(closer.spec);
            auto call = add_call1("format", std::move(he), hl, hc);   // format(e, "spec")
            auto lit = std::make_unique<StrLit>();
            lit->line = hl; lit->col = hc; lit->value = closer.spec;
            call->args.push_back(std::move(lit));
            add_piece(std::move(call));
        } else {
            add_piece(add_call1("toString", std::move(he), hl, hc));  // toString(e)
        }
        if (check(TokKind::InterpStrMid)) {
            const Token mid = cur(); advance();
            add_text(mid.text, mid.line, mid.col);
            continue;
        }
        const Token end = expect(TokKind::InterpStrEnd, "end of interpolated string");
        add_text(end.text, end.line, end.col);
        break;
    }
    // Every interpolation has >= 1 hole (Begin is only lexed when a `${` was seen), so `acc` is set;
    // guard defensively so the result is always a well-formed String expression.
    if (!acc) { auto lit = std::make_unique<StrLit>(); lit->line = line; lit->col = col; acc = std::move(lit); }
    return acc;
}

// Validate a `${e:spec}` format specifier at parse time (nice compile error on malformed). Grammar
// (v2, a subset of Rust's):
//   [[fill]align] ['+'] ['#'] ['0'] [width] ['.' precision] [type]
//     align  '<' | '>' | '^'   (a char before it is the fill)
//     '+'    sign flag  -- force a leading '+' on a non-negative number
//     '#'    alternate form -- add a base prefix (0x/0X/0b/0o) for the x/X/b/o types (Int)
//     '0'    zero-pad flag;  width = digits;  '.'precision = digits (Double)
//     type   x X b o d f g e E s
// Any leftover / unknown char is rejected. Applicability (e.g. a base on a Double, or '#' on a decimal)
// is intentionally NOT checked here -- it is ignored at runtime (Fork A). Caps guard the runtime
// (precision overflow / a pathological width allocation). Runs while cur() is the closing Interp token,
// so error() points there.
void Parser::validate_format_spec(const std::string& spec) const {
    auto is_align = [](char c) { return c == '<' || c == '>' || c == '^'; };
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    const std::string& s = spec;
    size_t i = 0, n = s.size();
    if (n >= 2 && is_align(s[1]))      i = 2;   // [fill]align
    else if (n >= 1 && is_align(s[0])) i = 1;   // align only
    if (i < n && s[i] == '+') i++;              // sign flag  (before '#'/'0', Rust order)
    if (i < n && s[i] == '#') i++;              // alternate-form flag (base prefix)
    if (i < n && s[i] == '0') i++;              // zero-pad flag
    long width = 0;                             // width digits
    while (i < n && is_digit(s[i])) { width = width * 10 + (s[i] - '0'); if (width > 100000) error("format width too large"); i++; }
    if (i < n && s[i] == '.') {                 // '.' precision
        i++;
        size_t pstart = i;
        long prec = 0;
        while (i < n && is_digit(s[i])) { prec = prec * 10 + (s[i] - '0'); i++; }
        if (i == pstart) error("format specifier: expected digits after '.'");
        if (prec > 10)   error("format precision too large (max 10)");
    }
    if (i < n) {                                // type char (must be last)
        char t = s[i];
        if (t == 'x' || t == 'X' || t == 'b' || t == 'o' || t == 'd' || t == 'f' || t == 'g' ||
            t == 'e' || t == 'E' || t == 's') i++;
        else error(std::string("invalid format type '") + t + "' in `${e:" + spec + "}` (use x/X/b/o/d/f/g/e/E/s)");
    }
    if (i != n) error("invalid format specifier `" + spec + "`");
}

ExprPtr Parser::parse_while() {
    const Token& kw = expect(TokKind::KwWhile, "'while'");
    if (check(TokKind::KwLet)) return parse_while_let(kw);   // `while let PAT = E {…}` sugar
    auto e = std::make_unique<WhileExpr>();
    e->line = kw.line; e->col = kw.col;
    const bool save = no_struct_lit_; no_struct_lit_ = true;
    e->cond = parse_expr(0);
    no_struct_lit_ = save;
    e->body = parse_block();
    return e;
}

// `while let PAT = EXPR { BODY }` -- loop while EXPR matches PAT, binding PAT into BODY. A pure DESUGAR
// to `loop { match EXPR { PAT => BODY, _ => break } }`: the `_ => break` (valueless) exits when the
// pattern stops matching and gives the loop the unit type (like `while`/`for`); PAT's bindings scope to
// BODY. Reuses loop/match/break -- no new AST node, no checker/codegen/VM change. Mirrors `if let`.
ExprPtr Parser::parse_while_let(const Token& kw) {
    expect(TokKind::KwLet, "'let'");
    PatPtr pat = parse_arm_pattern();
    expect(TokKind::Assign, "'='");
    const bool save = no_struct_lit_; no_struct_lit_ = true;   // scrutinee before `{` -- as if/while/match
    ExprPtr scrut = parse_expr(0);
    no_struct_lit_ = save;
    ExprPtr body = parse_block();

    auto m = std::make_unique<MatchExpr>();
    m->line = kw.line; m->col = kw.col;
    m->scrut = std::move(scrut);
    MatchArm hit; hit.pat = std::move(pat); hit.body = std::move(body);
    m->arms.push_back(std::move(hit));
    auto wild = std::make_unique<WildcardPat>(); wild->line = kw.line; wild->col = kw.col;
    MatchArm miss; miss.pat = std::move(wild);
    auto brk = std::make_unique<BreakExpr>(); brk->line = kw.line; brk->col = kw.col;  // valueless -> () loop
    miss.body = std::move(brk);
    m->arms.push_back(std::move(miss));

    auto blk = std::make_unique<BlockExpr>(); blk->line = kw.line; blk->col = kw.col;
    auto st = std::make_unique<ExprStmt>(); st->line = kw.line; st->col = kw.col; st->expr = std::move(m);
    blk->stmts.push_back(std::move(st));

    auto lp = std::make_unique<LoopExpr>();
    lp->line = kw.line; lp->col = kw.col; lp->body = std::move(blk);
    return lp;
}

ExprPtr Parser::parse_for() {
    const Token& kw = expect(TokKind::KwFor, "'for'");
    auto e = std::make_unique<ForExpr>();
    e->line = kw.line; e->col = kw.col;
    e->pat = parse_pattern();
    expect(TokKind::KwIn, "'in'");
    const bool save = no_struct_lit_; no_struct_lit_ = true;
    e->iter = parse_expr(0);
    no_struct_lit_ = save;
    e->body = parse_block();
    return e;
}

// `loop { … }` -- no header expression, so no `no_struct_lit_` dance (unlike while/for/match).
ExprPtr Parser::parse_loop() {
    const Token& kw = expect(TokKind::KwLoop, "'loop'");
    auto e = std::make_unique<LoopExpr>();
    e->line = kw.line; e->col = kw.col;
    e->body = parse_block();
    return e;
}

ExprPtr Parser::parse_match() {
    const Token& kw = expect(TokKind::KwMatch, "'match'");
    auto m = std::make_unique<MatchExpr>();
    m->line = kw.line; m->col = kw.col;
    const bool save = no_struct_lit_; no_struct_lit_ = true;
    m->scrut = parse_expr(0);
    no_struct_lit_ = save;
    layout_brace(ParseLayout::Brace::Match);
    const Token& open = expect(TokKind::LBrace, "'{'");
    skip_terminators();
    while (!check(TokKind::RBrace)) {
        if (body_unclosed(open)) return m;
        if (at_end()) error("unterminated match");
        const size_t start = pos_;
        layout_start();
        const bool nsl = no_struct_lit_;
        try {
            MatchArm arm;
            arm.pat = parse_arm_pattern();
            if (accept(TokKind::KwIf)) arm.guard = parse_expr(0);
            expect(TokKind::FatArrow, "'=>'");
            arm.body = parse_expr(0);
            m->arms.push_back(std::move(arm));
        } catch (const ParseError& e) {
            if (!recover_) throw;
            no_struct_lit_ = nsl;
            recover(e, Sync::Arm, start, &open);
        }
        accept(TokKind::Comma);
        skip_terminators();
    }
    close_body(open);
    return m;
}

ExprPtr Parser::parse_lambda() {
    const Token& kw = expect(TokKind::KwFn, "'fn'");
    auto l = std::make_unique<LambdaExpr>();
    l->line = kw.line; l->col = kw.col;
    expect(TokKind::LParen, "'('");
    // lambda params: `lident [: type]`, types OPTIONAL (supplied by the checker).
    if (!check(TokKind::RParen)) {
        for (;;) {
            if (check(TokKind::RParen)) break;
            Param p;
            const Token& nm = expect(TokKind::LIdent, "parameter name");
            p.name = nm.text; p.name_line = nm.line; p.name_col = nm.col;
            if (accept(TokKind::Colon)) p.type = parse_type();
            l->params.push_back(std::move(p));
            if (!accept(TokKind::Comma)) break;
        }
    }
    expect(TokKind::RParen, "')'");
    if (accept(TokKind::Arrow)) l->ret = parse_type();
    l->body = parse_block();
    return l;
}

// Recover mode, a block is required here and its `{` is missing: the first `}` ahead (at depth 0,
// before any item start) closes it if it is on this line, or on the indentation of the line the
// block belongs to (`if c` / `fn m(..) -> T` then the statements, then that `}`). The tokens up to
// it are read as the block's statements. Otherwise nothing is consumed and null is returned.
ExprPtr Parser::parse_braceless_block() {
    if (pos_ == 0) return nullptr;
    const uint32_t owner_indent = line_indent(toks_[pos_ - 1]);
    size_t close = SIZE_MAX;
    int depth = 0;
    for (size_t i = pos_; i < toks_.size(); ++i) {
        const TokKind k = toks_[i].kind;
        if (k == TokKind::Eof) break;
        if (depth == 0 && i > pos_) {
            const size_t save = pos_;
            pos_ = i;
            const bool item = at_item_start();
            pos_ = save;
            if (item) break;
        }
        if (k == TokKind::LBrace || k == TokKind::LParen || k == TokKind::LBracket) ++depth;
        else if (k == TokKind::RParen || k == TokKind::RBracket) { if (depth > 0) --depth; }
        else if (k == TokKind::RBrace) {
            if (depth > 0) { --depth; continue; }
            if (toks_[i].line == cur().line || toks_[i].col == owner_indent) close = i;
            break;
        }
    }
    if (close == SIZE_MAX) return nullptr;
    note(ParseError("expected '{'", cur().line, cur().col));
    auto blk = std::make_unique<BlockExpr>();
    blk->line = cur().line; blk->col = cur().col;
    const bool save = no_struct_lit_; no_struct_lit_ = false;
    skip_terminators();
    while (pos_ < close && !at_end()) {
        const size_t start = pos_;
        try {
            blk->stmts.push_back(parse_stmt());
        } catch (const ParseError& e) {
            no_struct_lit_ = false;
            recover(e, Sync::Stmt, start);
        }
        skip_terminators();
    }
    no_struct_lit_ = save;
    if (pos_ == close) advance();                   // the `}`
    return blk;
}

ExprPtr Parser::parse_block() {
    if (recover_ && !check(TokKind::LBrace))
        if (ExprPtr b = parse_braceless_block()) return b;
    layout_brace(ParseLayout::Brace::Block);
    const Token& kw = expect(TokKind::LBrace, "'{'");
    auto blk = std::make_unique<BlockExpr>();
    blk->line = kw.line; blk->col = kw.col;
    const bool save = no_struct_lit_; no_struct_lit_ = false;
    skip_terminators();
    while (!check(TokKind::RBrace)) {
        if (body_unclosed(kw)) { no_struct_lit_ = save; return blk; }
        // `if c { a else { b } }`: a statement cannot start with `else`, so the `}` of this block is
        // missing right before it. Close here and leave `else` to the `if` that owns it.
        if (recover_ && check(TokKind::KwElse)) {
            note(ParseError("expected '}' before 'else'", cur().line, cur().col));
            no_struct_lit_ = save;
            return blk;
        }
        if (at_end()) error("unterminated block");
        const size_t start = pos_;
        layout_start();
        try {
            blk->stmts.push_back(parse_stmt());
        } catch (const ParseError& e) {
            if (!recover_) throw;
            no_struct_lit_ = false;
            recover(e, Sync::Stmt, start, &kw);
        }
        skip_terminators();
    }
    no_struct_lit_ = save;
    close_body(kw);
    return blk;
}

// ---- patterns ---------------------------------------------------------------

// `first | a | b | ...` -> an OrPat; `first` unchanged when no `|` follows (the common case).
//
// Nested OrPats are SPLICED, not nested. The parenthesized form returns its inner pattern verbatim
// (see the LParen branch of parse_pattern), so `(A|B) | C` and `A|B|C` are indistinguishable to
// everything downstream and MUST be the same tree. Keeping them nested would be a live defect, not
// a cosmetic one: check_match_coverage peels exactly one level of Or into Maranget rows, so the
// inner OrPat would reach lower_pat, which lowers only its first alternative -- the row set becomes
// a strict under-approximation and a legal arm gets rejected as "unreachable match arm".
PatPtr Parser::parse_or_tail(PatPtr first, uint32_t line, uint32_t col) {
    if (!check(TokKind::BitOr)) return first;
    auto o = std::make_unique<OrPat>();
    o->line = line; o->col = col;
    auto push = [&o](PatPtr p) {
        if (p->kind == PatKind::Or)
            for (auto& a : static_cast<OrPat&>(*p).alts) o->alts.push_back(std::move(a));
        else
            o->alts.push_back(std::move(p));
    };
    push(std::move(first));
    while (accept(TokKind::BitOr)) push(parse_pattern());
    return o;
}

PatPtr Parser::parse_arm_pattern() {
    accept(TokKind::BitOr);                             // optional leading `|` (vertical formatting)
    const uint32_t line = cur().line, col = cur().col;
    return parse_or_tail(parse_pattern(), line, col);
}

// A pattern in a NESTED position, where `|` needs no parentheses because the terminator is
// unambiguous: a ctor/tuple/list element (`,` or the closer), a struct field value, a map value.
// So `Some(Red | Green)` and `[1 | 2, x]` read the way anyone would write them, and the explicit
// `( ... )` form is needed only where a bare `|` really would be ambiguous -- `n @ (1..5 | 9)`,
// whose sub-pattern is not delimited by anything.
//
// NOT used for the `..rest` tail of a list (that must stay a plain name or `_`) and NOT for the
// `let` / `for` heads, which take parse_pattern directly so a bare `A | B` there stays a parse
// error rather than becoming a checker rejection about refutability.
PatPtr Parser::parse_nested_pattern() {
    PatPtr p = parse_pattern();
    const uint32_t l = p->line, c = p->col;
    return parse_or_tail(std::move(p), l, c);
}

// A binding name has just been consumed. `name @ sub` makes it a BindPat, a bare name an IdentPat.
// Shared by the `lident` and `mut lident` branches so the two spellings cannot drift -- `mut` builds
// its own IdentPat and does NOT fall through to the lident case, which is how `mut n @ p` would
// otherwise have been quietly left out.
//
// This is reached from EVERY pattern position, including the irrefutable `let` / `for` ones, and
// deliberately so: gating here would need a parser mode flag (the `no_struct_lit_` shape) threaded
// through every nested call, while the checker already owns well-worded rejections and has the one
// place -- bind_pattern -- that both binder positions funnel through.
PatPtr Parser::finish_binding_pattern(std::string name, bool is_mut, uint32_t line, uint32_t col) {
    if (accept(TokKind::At)) {
        auto b = std::make_unique<BindPat>();
        b->line = line; b->col = col; b->is_mut = is_mut; b->name = std::move(name);
        b->sub = parse_pattern();
        return b;
    }
    auto p = std::make_unique<IdentPat>();
    p->line = line; p->col = col; p->is_mut = is_mut; p->name = std::move(name);
    return p;
}

// `42` / `-42` / `1.5` / `-1.5` / `'a'` / `"s"` / `true` -- and nothing else. See the header comment
// for why this exists instead of `parse_prefix()`; the short version is that `parse_prefix`'s unary
// arm reaches the Pratt loop, so `-f(3)` parsed as a pattern literal and under-measured the frame.
ExprPtr Parser::parse_pattern_literal() {
    const Token& t = cur();
    if (t.kind == TokKind::Minus) {                    // a SIGN, applied to one number literal
        const uint32_t l = t.line, c = t.col;
        advance();
        if (!check(TokKind::Int) && !check(TokKind::Double))
            error("'-' in a pattern must be followed by a number literal");
        auto u = std::make_unique<UnaryExpr>();
        u->line = l; u->col = c; u->op = TokKind::Minus;
        u->operand = parse_pattern_literal();          // guarded above: exactly one Int/Double leaf
        return u;
    }
    switch (t.kind) {
    // These five `parse_prefix` arms are pure leaves -- one token, no recursion, no Pratt loop --
    // so delegating keeps the literal-node construction in one place instead of drifting copies.
    case TokKind::Int:  case TokKind::Double: case TokKind::Str:
    case TokKind::KwTrue: case TokKind::KwFalse:
        return parse_prefix();
    case TokKind::InterpStrBegin:
        error("an interpolated string cannot be used as a pattern (a pattern literal must be constant)");
    default:
        error("a pattern literal must be a number, character, string, or boolean literal");
    }
}

// A range bound: a literal leaf, or `NAME` / `mod::NAME` / `Enum::NAME` for the checker to resolve
// to a scalar `const`. The IdentExpr is built HERE rather than by `parse_prefix` so that neither a
// struct literal (`Name { ... }`) nor any postfix (`f()`, `a[i]`, `p.x`) can slip in behind it.
ExprPtr Parser::parse_range_bound() {
    const Token& t = cur();
    if (t.kind != TokKind::LIdent && t.kind != TokKind::UIdent) return parse_pattern_literal();
    auto id = std::make_unique<IdentExpr>();
    id->line = t.line; id->col = t.col; id->name = t.text; id->upper = (t.kind == TokKind::UIdent);
    id->name_line = t.line; id->name_col = t.col;
    advance();
    if (accept(TokKind::ColonColon)) {                 // `mod::LO` / `Enum::LO`
        id->qualifier = std::move(id->name);
        const Token& tail = expect(TokKind::UIdent, "constant name after '::'");
        id->name = tail.text; id->upper = true;
        id->name_line = tail.line; id->name_col = tail.col;
    }
    return id;
}

PatPtr Parser::finish_range_pattern(ExprPtr lo, uint32_t line, uint32_t col) {
    auto rp = std::make_unique<RangePat>();
    rp->line = line; rp->col = col;
    rp->exclusive = check(TokKind::DotDotLess);         // `..` inclusive vs `..<` exclusive upper
    advance();
    rp->lo = std::move(lo);
    rp->hi = parse_range_bound();
    return rp;
}

PatPtr Parser::parse_pattern() {
    const Token& t = cur();
    switch (t.kind) {
    case TokKind::Underscore: {
        auto p = std::make_unique<WildcardPat>();
        p->line = t.line; p->col = t.col; advance();
        return p;
    }
    case TokKind::KwMut: {
        const uint32_t l = t.line, c = t.col;
        advance();
        std::string name = expect(TokKind::LIdent, "binding name").text;
        // `mut lo..hi` would otherwise die at the arm's `expect('=>')` pointing at `..`. A bound is
        // a constant, never a binding, so `mut` there is meaningless rather than merely misplaced.
        if (check(TokKind::DotDot) || check(TokKind::DotDotLess))
            error("a range-pattern bound cannot be 'mut' -- a bound names a constant, it binds nothing");
        return finish_binding_pattern(std::move(name), /*is_mut=*/true, l, c);
    }
    case TokKind::LIdent: {                         // a fresh binding, or a `mod::Ctor` qualified pattern
        std::string first = t.text; uint32_t line = t.line, col = t.col;
        advance();
        if (accept(TokKind::ColonColon)) {          // module-qualified constructor / struct pattern
            const Token& tail = expect(TokKind::UIdent, "constructor name after 'module::'");
            return parse_ctor_or_struct_pattern(tail.text, std::move(first), tail.line, tail.col, line, col);
        }
        // A lowercase name followed by `..` is a range LOWER bound, not a binding -- the one place
        // in a pattern where an identifier is unambiguous, since a bound is never a binder.
        if (check(TokKind::DotDot) || check(TokKind::DotDotLess)) {
            auto id = std::make_unique<IdentExpr>();
            id->line = line; id->col = col; id->name = std::move(first); id->upper = false;
            id->name_line = line; id->name_col = col;
            return finish_range_pattern(std::move(id), line, col);
        }
        return finish_binding_pattern(std::move(first), /*is_mut=*/false, line, col);
    }
    case TokKind::UIdent: {                         // a constructor / struct pattern, or `Enum::Variant`
        std::string name = t.text; uint32_t line = t.line, col = t.col;
        advance();
        if (accept(TokKind::ColonColon)) {          // `Enum::Variant` qualified pattern (S1)
            const Token& tail = expect(TokKind::UIdent, "variant name after 'Enum::'");
            return parse_ctor_or_struct_pattern(tail.text, std::move(name), tail.line, tail.col, line, col);
        }
        return parse_ctor_or_struct_pattern(std::move(name), "", line, col, 0, 0);
    }
    case TokKind::LParen: {                         // tuple / grouping / parenthesized or-pattern
        const uint32_t l = t.line, c = t.col;
        advance();
        auto tp = std::make_unique<TuplePat>();
        tp->line = l; tp->col = c;
        if (!check(TokKind::RParen)) {
            accept(TokKind::BitOr);                 // optional leading `|`, as in a match arm
            PatPtr first = parse_pattern();
            // `A | B` HERE, before the comma decision: that ordering is what makes `(A | B, C)` a
            // 2-tuple whose first element is an or-pattern, rather than an or-pattern of `A` and
            // the tuple `(B, C)`. `(A | B)` then falls into the grouping arm below and is returned
            // verbatim, which is how a nested or-pattern becomes expressible at all.
            const uint32_t fl = first->line, fc = first->col;
            first = parse_or_tail(std::move(first), fl, fc);
            if (check(TokKind::Comma)) {
                tp->elems.push_back(std::move(first));
                while (accept(TokKind::Comma)) {
                    if (check(TokKind::RParen)) break;
                    tp->elems.push_back(parse_nested_pattern());
                }
            } else {
                expect(TokKind::RParen, "')'");
                return first;                      // grouping
            }
        }
        expect(TokKind::RParen, "')'");
        return tp;
    }
    case TokKind::LBracket: {                       // list pattern
        const uint32_t l = t.line, c = t.col;
        advance();
        auto lp = std::make_unique<ListPat>();
        lp->line = l; lp->col = c;
        while (!check(TokKind::RBracket)) {
            if (at_end()) error("unterminated list pattern");
            if (accept(TokKind::DotDot)) {         // `..` or `..name` tail (must be last)
                if (check(TokKind::LIdent) || check(TokKind::Underscore))
                    lp->rest = parse_pattern();
                else {
                    auto w = std::make_unique<WildcardPat>();
                    w->line = cur().line; w->col = cur().col; lp->rest = std::move(w);
                }
                break;
            }
            lp->elems.push_back(parse_nested_pattern());
            if (!accept(TokKind::Comma)) break;
        }
        expect(TokKind::RBracket, "']'");
        return lp;
    }
    case TokKind::Hash: {                           // `#{ key => pat, ... }` map pattern
        const uint32_t l = t.line, c = t.col;
        advance();
        expect(TokKind::LBrace, "'{' (map pattern)");
        auto mp = std::make_unique<MapPat>();
        mp->line = l; mp->col = c;
        skip_terminators();
        while (!check(TokKind::RBrace)) {
            if (at_end()) error("unterminated map pattern");
            ExprPtr key = parse_expr(0);           // literal-ness enforced by the checker
            expect(TokKind::FatArrow, "'=>'");
            PatPtr val = parse_nested_pattern();
            mp->entries.emplace_back(std::move(key), std::move(val));
            accept(TokKind::Comma);
            skip_terminators();
        }
        expect(TokKind::RBrace, "'}'");
        return mp;
    }
    case TokKind::Int:  case TokKind::Double: case TokKind::Str:
    case TokKind::KwTrue: case TokKind::KwFalse: case TokKind::Minus: {
        const uint32_t l = t.line, c = t.col;
        ExprPtr lit = parse_pattern_literal();     // a literal leaf (`-42`, `'0'`) -- never an expression
        // `lo..hi` (inclusive) / `lo..<hi` (exclusive upper bound) -- a range pattern.
        if (check(TokKind::DotDot) || check(TokKind::DotDotLess))
            return finish_range_pattern(std::move(lit), l, c);
        auto lp = std::make_unique<LiteralPat>();
        lp->line = l; lp->col = c;
        lp->lit = std::move(lit);
        return lp;
    }
    case TokKind::InterpStrBegin:
        error("an interpolated string cannot be used as a pattern (a pattern literal must be constant)");
    default:
        error("expected a pattern");
    }
}

// Ctor / struct pattern body past the head name: `Name { f: p, ... }` (StructPat),
// `Name(p0, ...)` (CtorPat, has_parens), or a bare nullary `Name` (CtorPat). `qualifier`
// is the module path (empty for a bare `Name`), carried onto the node for the checker.
PatPtr Parser::parse_ctor_or_struct_pattern(std::string name, std::string qualifier,
                                            uint32_t line, uint32_t col, uint32_t qual_line, uint32_t qual_col) {
    if (check(TokKind::LBrace)) {                  // Name { f: p, ... }
        auto sp = std::make_unique<StructPat>();
        sp->line = line; sp->col = col; sp->name = std::move(name); sp->qualifier = std::move(qualifier);
        sp->qual_line = qual_line; sp->qual_col = qual_col;
        expect(TokKind::LBrace, "'{'");
        skip_terminators();
        while (!check(TokKind::RBrace)) {
            if (at_end()) error("unterminated struct pattern");
            FieldPat fp;
            fp.name = expect(TokKind::LIdent, "field name").text;
            if (accept(TokKind::Colon)) fp.pat = parse_nested_pattern();
            sp->fields.push_back(std::move(fp));
            accept(TokKind::Comma);
            skip_terminators();
        }
        expect(TokKind::RBrace, "'}'");
        return sp;
    }
    if (accept(TokKind::LParen)) {                 // Name(p0, ...)
        auto cp = std::make_unique<CtorPat>();
        cp->line = line; cp->col = col; cp->name = std::move(name); cp->qualifier = std::move(qualifier); cp->has_parens = true;
        cp->qual_line = qual_line; cp->qual_col = qual_col;
        if (!check(TokKind::RParen)) {
            cp->elems.push_back(parse_nested_pattern());
            while (accept(TokKind::Comma)) {
                if (check(TokKind::RParen)) break;
                cp->elems.push_back(parse_nested_pattern());
            }
        }
        expect(TokKind::RParen, "')'");
        return cp;
    }
    // An uppercase head followed by `..` is a range LOWER bound, not a nullary constructor. Diverting
    // HERE rather than at the three call sites covers `LO`, `mod::LO` and `Enum::LO` in one place --
    // and it is the only point at which the head is known to carry no `{...}` and no `(...)`.
    if (check(TokKind::DotDot) || check(TokKind::DotDotLess)) {
        auto id = std::make_unique<IdentExpr>();
        id->line = line; id->col = col; id->name = std::move(name);
        id->qualifier = std::move(qualifier); id->upper = true;
        id->name_line = line; id->name_col = col;
        return finish_range_pattern(std::move(id), line, col);
    }
    auto cp = std::make_unique<CtorPat>();         // bare Name (nullary, e.g. None)
    cp->line = line; cp->col = col; cp->name = std::move(name); cp->qualifier = std::move(qualifier); cp->has_parens = false;
    cp->qual_line = qual_line; cp->qual_col = qual_col;
    return cp;
}

// ---- types ------------------------------------------------------------------

TypePtr Parser::parse_type() {
    const Token& t = cur();
    switch (t.kind) {
    case TokKind::KwFn: {                           // fn(T, U) -> R
        const uint32_t l = t.line, c = t.col;
        advance();
        auto ft = std::make_unique<FnType>();
        ft->line = l; ft->col = c;
        expect(TokKind::LParen, "'('");
        if (!check(TokKind::RParen)) {
            ft->params.push_back(parse_type());
            while (accept(TokKind::Comma)) {
                if (check(TokKind::RParen)) break;
                ft->params.push_back(parse_type());
            }
        }
        expect(TokKind::RParen, "')'");
        expect(TokKind::Arrow, "'->'");
        ft->ret = parse_type();
        return ft;
    }
    case TokKind::LParen: {                          // (T, U) tuple, (T) grouping, () unit
        const uint32_t l = t.line, c = t.col;
        advance();
        auto tt = std::make_unique<TupleType>();
        tt->line = l; tt->col = c;
        if (!check(TokKind::RParen)) {
            TypePtr first = parse_type();
            if (check(TokKind::Comma)) {
                tt->elems.push_back(std::move(first));
                while (accept(TokKind::Comma)) {
                    if (check(TokKind::RParen)) break;
                    tt->elems.push_back(parse_type());
                }
            } else {
                expect(TokKind::RParen, "')'");
                return first;                      // grouping
            }
        }
        expect(TokKind::RParen, "')'");
        return tt;                                 // empty () = unit, or (a, b, ...)
    }
    case TokKind::UIdent: {                          // Name or Name[T, U]
        const uint32_t l = t.line, c = t.col;
        std::string name = t.text;
        advance();
        auto nt = std::make_unique<NamedType>();
        nt->line = l; nt->col = c; nt->name = std::move(name);
        nt->name_line = l; nt->name_col = c;
        parse_type_args(nt->args);
        return nt;
    }
    case TokKind::KwDyn:                             // `dyn Trait[...]` -- a trait object
        return parse_dyn_type();
    case TokKind::LIdent: {                          // module-qualified type `mod::Name[...]`
        const uint32_t l = t.line, c = t.col;
        std::string qual = t.text;
        advance();
        expect(TokKind::ColonColon, "'::' (a lowercase name is only valid as a module qualifier here)");
        const Token& nm = expect(TokKind::UIdent, "type name after 'module::'");
        auto nt = std::make_unique<NamedType>();
        nt->line = l; nt->col = c; nt->qualifier = std::move(qual); nt->name = nm.text;
        nt->name_line = nm.line; nt->name_col = nm.col;
        parse_type_args(nt->args);
        return nt;
    }
    default:
        error("expected a type");
    }
}

// The optional `[T, U]` type-argument tail of a named or `dyn` type (no-op if absent).
void Parser::parse_type_args(std::vector<TypePtr>& out) {
    if (accept(TokKind::LBracket)) {
        if (!check(TokKind::RBracket)) {
            out.push_back(parse_type());
            while (accept(TokKind::Comma)) {
                if (check(TokKind::RBracket)) break;
                out.push_back(parse_type());
            }
        }
        expect(TokKind::RBracket, "']'");
    }
}

// `dyn Trait` / `dyn Trait[A, ...]` / `dyn mod::Trait[...]` -- a trait object. Only the trait
// NAME may follow `dyn` (a lowercase head is a module qualifier, as everywhere else); whether
// the trait is object-safe is the checker's call, not the parser's.
TypePtr Parser::parse_dyn_type() {
    const Token& kw = expect(TokKind::KwDyn, "'dyn'");
    auto dt = std::make_unique<DynType>();
    dt->line = kw.line; dt->col = kw.col;
    if (check(TokKind::LIdent)) {                 // `dyn mod::Trait`
        dt->qualifier = cur().text;
        advance();
        expect(TokKind::ColonColon, "'::' (a lowercase name is only valid as a module qualifier here)");
    }
    const Token& nm = expect(TokKind::UIdent, "a trait name after 'dyn'");
    dt->trait = nm.text;
    dt->name_line = nm.line; dt->name_col = nm.col;
    parse_type_args(dt->args);
    return dt;
}

// ---- test entry point -------------------------------------------------------

ExprPtr Parser::parse_expression() {
    ExprPtr e = parse_expr(0);
    skip_terminators();
    expect(TokKind::Eof, "end of input");
    return e;
}

} // namespace svc
