// =============================================================================
// Ast.cpp -- S-expression rendering of the typed AST (dump()).
//
// The rendering is a stable, parenthesised prefix form the tests assert against
// (mirroring how the lexer tests assert on a token-kind string). It is purely for
// tests/diagnostics; nothing downstream depends on it. Operators print via
// to_string(TokKind) so `+`/`|>`/`==` show as their source symbols.
//
// Unlike the former dynamic front end's dump (which omitted types), this renders the
// RETAINED type annotations -- param `name:Type`, fn `-> ret`, generics `[T: B]`,
// impl targets -- because in the static language types are first-class and
// mandatory at the boundaries; showing them lets the tests confirm they attach.
// The resolved semantic `ty` slot on Expr is NOT rendered (it is null until the
// checker runs; structural dumps must stay stable once it starts filling in).
// =============================================================================

#include "Ast.h"

#include <cstdio>

namespace svc {

namespace {

// A double rendered with %g so 3.5 -> "3.5" and 3.0 -> "3"; good enough for the
// tests, which use small exact literals.
std::string fmt_double(double d) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", d);
    return buf;
}

// When true, render_expr appends `:<describe(e.ty)>` after each expression node (the P4 typed dump).
// Single-threaded test-oracle state; set only for the duration of dump_typed().
bool g_dump_typed = false;

void render_expr(const Expr& e, std::string& out);
void render_stmt(const Stmt& s, std::string& out);
void render_pat(const Pattern& p, std::string& out);
void render_type(const Type& t, std::string& out);
void render_item(const Item& i, std::string& out);

// A null child renders as `_` so an absent optional (else branch, guard, rest
// pattern, type annotation, required-method body) is visible and unambiguous.
void render_expr_opt(const Expr* e, std::string& out) {
    if (e) render_expr(*e, out); else out += '_';
}
void render_type_opt(const Type* t, std::string& out) {
    if (t) render_type(*t, out); else out += '_';
}

// `[T, U: Display + Clone]` -- appends nothing when there are no generics.
void render_generics(const std::vector<GenericParam>& gs, std::string& out) {
    if (gs.empty()) return;
    out += '[';
    for (size_t i = 0; i < gs.size(); ++i) {
        if (i) out += ',';
        out += gs[i].name;
        for (size_t b = 0; b < gs[i].bounds.size(); ++b) {
            out += (b == 0) ? ':' : '+';
            out += gs[i].bounds[b].trait;
            const auto& ba = gs[i].bounds[b].args;
            if (!ba.empty()) {
                out += '[';
                for (size_t k = 0; k < ba.size(); ++k) { if (k) out += ','; render_type(*ba[k], out); }
                out += ']';
            }
        }
    }
    out += ']';
}

// A parameter: `name` / `name:Type`, with a leading `mut ` when the binding is mutable.
void render_param(const Param& p, std::string& out) {
    if (p.is_mut) out += "mut ";
    out += p.name;
    if (p.type) { out += ':'; render_type(*p.type, out); }
}

// `(self x:T y:Int)` -- a param list, with a leading `[mut ]self` when the method has one.
void render_param_list(const std::vector<Param>& params, bool has_self, std::string& out,
                       bool self_mut = false) {
    out += '(';
    bool first = true;
    if (has_self) { if (self_mut) out += "mut "; out += "self"; first = false; }
    for (auto& p : params) { if (!first) out += ' '; first = false; render_param(p, out); }
    out += ')';
}

void render_method(const Method& m, std::string& out) {
    out += "(method "; out += m.name;
    render_generics(m.generics, out);
    out += ' '; render_param_list(m.params, m.has_self, out, m.self_mut);
    out += " -> "; render_type_opt(m.ret.get(), out);
    out += ' '; render_expr_opt(m.body.get(), out); out += ')';
}

void render_expr(const Expr& e, std::string& out) {
    switch (e.kind) {
    case ExprKind::IntLit:    out += std::to_string(static_cast<const IntLit&>(e).value); break;
    case ExprKind::DoubleLit: out += fmt_double(static_cast<const DoubleLit&>(e).value); break;
    case ExprKind::StrLit:    out += "(str "; out += static_cast<const StrLit&>(e).value; out += ')'; break;
    case ExprKind::BoolLit:   out += static_cast<const BoolLit&>(e).value ? "true" : "false"; break;
    case ExprKind::Ident: {
        const auto& id = static_cast<const IdentExpr&>(e);
        if (!id.qualifier.empty()) { out += id.qualifier; out += "::"; }
        out += id.name;
        break;
    }
    case ExprKind::Unary: {
        auto& u = static_cast<const UnaryExpr&>(e);
        out += '('; out += to_string(u.op); out += ' '; render_expr_opt(u.operand.get(), out); out += ')';
        break;
    }
    case ExprKind::Binary: {
        auto& b = static_cast<const BinaryExpr&>(e);
        out += '('; out += to_string(b.op); out += ' ';
        render_expr_opt(b.lhs.get(), out); out += ' '; render_expr_opt(b.rhs.get(), out); out += ')';
        break;
    }
    case ExprKind::Pipe: {
        auto& p = static_cast<const PipeExpr&>(e);
        out += "(|> "; render_expr_opt(p.lhs.get(), out); out += ' '; render_expr_opt(p.rhs.get(), out); out += ')';
        break;
    }
    case ExprKind::Call: {
        auto& c = static_cast<const CallExpr&>(e);
        out += "(call ";
        // Do NOT annotate a callee whose type is the Error POISON: infer_call stamps ty_error()
        // on a direct call's callee ident as a "this is a call target, not a value" marker
        // (Check.cpp), so annotating it would print `<error>` next to every call in a perfectly
        // well-typed program -- alarming and false. A callee that carries a REAL type (a
        // fn-valued binding, a computed callee) still shows it, and the call node's own type (the
        // return type) is unaffected. Only reachable via dump_typed; plain dump() renders neither.
        {
            const Expr* ce = c.callee.get();
            const bool  poisoned = g_dump_typed && ce && ce->ty && ce->ty->kind == TyKind::Error;
            const bool  saved    = g_dump_typed;
            if (poisoned) g_dump_typed = false;
            render_expr_opt(ce, out);
            g_dump_typed = saved;
        }
        for (auto& a : c.args) { out += ' '; render_expr(*a, out); }
        out += ')';
        break;
    }
    case ExprKind::Field: {
        auto& f = static_cast<const FieldExpr&>(e);
        out += "(. "; render_expr_opt(f.obj.get(), out); out += ' '; out += f.name; out += ')';
        break;
    }
    case ExprKind::Index: {
        auto& ix = static_cast<const IndexExpr&>(e);
        out += "(index "; render_expr_opt(ix.obj.get(), out); out += ' '; render_expr_opt(ix.index.get(), out); out += ')';
        break;
    }
    case ExprKind::Try: {
        auto& t = static_cast<const TryExpr&>(e);
        out += "(try "; render_expr_opt(t.operand.get(), out); out += ')';
        break;
    }
    case ExprKind::If: {
        auto& f = static_cast<const IfExpr&>(e);
        out += "(if "; render_expr_opt(f.cond.get(), out); out += ' ';
        render_expr_opt(f.then_blk.get(), out); out += ' '; render_expr_opt(f.else_blk.get(), out); out += ')';
        break;
    }
    case ExprKind::Match: {
        auto& m = static_cast<const MatchExpr&>(e);
        out += "(match "; render_expr_opt(m.scrut.get(), out);
        for (auto& arm : m.arms) {
            out += " (arm "; render_pat(*arm.pat, out);
            if (arm.guard) { out += " (guard "; render_expr(*arm.guard, out); out += ')'; }
            out += ' '; render_expr_opt(arm.body.get(), out); out += ')';
        }
        out += ')';
        break;
    }
    case ExprKind::While: {
        auto& w = static_cast<const WhileExpr&>(e);
        out += "(while "; render_expr_opt(w.cond.get(), out); out += ' '; render_expr_opt(w.body.get(), out); out += ')';
        break;
    }
    case ExprKind::For: {
        auto& f = static_cast<const ForExpr&>(e);
        out += "(for "; render_pat(*f.pat, out); out += ' ';
        render_expr_opt(f.iter.get(), out); out += ' '; render_expr_opt(f.body.get(), out); out += ')';
        break;
    }
    case ExprKind::Loop: {
        auto& l = static_cast<const LoopExpr&>(e);
        out += "(loop "; render_expr_opt(l.body.get(), out); out += ')';
        break;
    }
    case ExprKind::Break: {
        auto& b = static_cast<const BreakExpr&>(e);
        out += "(break"; if (b.value) { out += ' '; render_expr(*b.value, out); } out += ')';
        break;
    }
    case ExprKind::Continue: out += "(continue)"; break;
    case ExprKind::Return: {
        auto& r = static_cast<const ReturnExpr&>(e);
        out += "(return"; if (r.value) { out += ' '; render_expr(*r.value, out); } out += ')';
        break;
    }
    case ExprKind::Block: {
        auto& b = static_cast<const BlockExpr&>(e);
        out += "(block";
        for (auto& s : b.stmts) { out += ' '; render_stmt(*s, out); }
        out += ')';
        break;
    }
    case ExprKind::Lambda: {
        auto& l = static_cast<const LambdaExpr&>(e);
        out += "(fn "; render_param_list(l.params, /*has_self=*/false, out);
        if (l.ret) { out += " -> "; render_type(*l.ret, out); }
        out += ' '; render_expr_opt(l.body.get(), out); out += ')';
        break;
    }
    case ExprKind::StructLit: {
        auto& s = static_cast<const StructLit&>(e);
        out += "(struct-lit ";
        if (!s.qualifier.empty()) { out += s.qualifier; out += "::"; }
        out += s.name;
        for (auto& f : s.fields) {
            out += " ("; out += f.name;
            if (f.value) { out += ' '; render_expr(*f.value, out); }   // shorthand `{x}` = value null
            out += ')';
        }
        if (s.base) { out += " (.. "; render_expr(*s.base, out); out += ')'; }   // record update `..base`
        out += ')';
        break;
    }
    case ExprKind::Tuple: {
        auto& t = static_cast<const TupleExpr&>(e);
        out += "(tuple";
        for (auto& x : t.elems) { out += ' '; render_expr(*x, out); }
        out += ')';
        break;
    }
    case ExprKind::ListLit: {
        auto& l = static_cast<const ListLit&>(e);
        out += "(list";
        for (auto& x : l.elems) { out += ' '; render_expr(*x, out); }
        out += ')';
        break;
    }
    case ExprKind::MapLit: {
        auto& m = static_cast<const MapLit&>(e);
        out += "(map";
        for (auto& kv : m.entries) {
            out += " ("; render_expr(*kv.first, out); out += ' '; render_expr(*kv.second, out); out += ')';
        }
        out += ')';
        break;
    }
    }
    if (g_dump_typed) { out += ':'; out += describe(e.ty); }   // P4 typed dump: annotate each Expr's type
}

void render_stmt(const Stmt& s, std::string& out) {
    switch (s.kind) {
    case StmtKind::Let: {
        auto& l = static_cast<const LetStmt&>(s);
        out += l.is_mut ? "(let-mut " : "(let ";
        render_pat(*l.pat, out);
        if (l.type) { out += " (: "; render_type(*l.type, out); out += ')'; }
        out += ' '; render_expr_opt(l.init.get(), out); out += ')';
        break;
    }
    case StmtKind::Assign: {
        auto& a = static_cast<const AssignStmt&>(s);
        out += '(';
        if (a.op != TokKind::Assign) out += to_string(a.op);   // `(+= x 1)` for a compound form
        out += "= "; render_expr_opt(a.target.get(), out); out += ' ';
        render_expr_opt(a.value.get(), out); out += ')';
        break;
    }
    case StmtKind::Expr:
        render_expr_opt(static_cast<const ExprStmt&>(s).expr.get(), out);
        break;
    }
}

void render_pat(const Pattern& p, std::string& out) {
    switch (p.kind) {
    case PatKind::Wildcard: out += '_'; break;
    case PatKind::Literal:  render_expr_opt(static_cast<const LiteralPat&>(p).lit.get(), out); break;
    case PatKind::Range: {
        auto& rp = static_cast<const RangePat&>(p);
        out += "(range "; render_expr_opt(rp.lo.get(), out);
        out += rp.exclusive ? " ..< " : " .. ";
        render_expr_opt(rp.hi.get(), out); out += ')';
        break;
    }
    case PatKind::Ident: {
        auto& i = static_cast<const IdentPat&>(p);
        if (i.is_mut) { out += "(mut "; out += i.name; out += ')'; } else out += i.name;
        break;
    }
    case PatKind::Bind: {
        auto& b = static_cast<const BindPat&>(p);
        out += "(@ ";
        if (b.is_mut) { out += "(mut "; out += b.name; out += ')'; } else out += b.name;
        out += ' ';
        if (b.sub) render_pat(*b.sub, out); else out += "?";
        out += ')';
        break;
    }
    case PatKind::Ctor: {
        auto& c = static_cast<const CtorPat&>(p);
        out += "(ctor ";
        if (!c.qualifier.empty()) { out += c.qualifier; out += "::"; }
        out += c.name;
        for (auto& e : c.elems) { out += ' '; render_pat(*e, out); }
        out += ')';
        break;
    }
    case PatKind::Tuple: {
        auto& t = static_cast<const TuplePat&>(p);
        out += "(tuple-pat";
        for (auto& e : t.elems) { out += ' '; render_pat(*e, out); }
        out += ')';
        break;
    }
    case PatKind::List: {
        auto& l = static_cast<const ListPat&>(p);
        out += "(list-pat";
        for (auto& e : l.elems) { out += ' '; render_pat(*e, out); }
        if (l.rest) { out += " (.. "; render_pat(*l.rest, out); out += ')'; }
        out += ')';
        break;
    }
    case PatKind::Struct: {
        auto& s = static_cast<const StructPat&>(p);
        out += "(struct-pat ";
        if (!s.qualifier.empty()) { out += s.qualifier; out += "::"; }
        out += s.name;
        for (auto& f : s.fields) {
            out += " (" ; out += f.name;
            if (f.pat) { out += ' '; render_pat(*f.pat, out); }
            out += ')';
        }
        out += ')';
        break;
    }
    case PatKind::Map: {
        auto& m = static_cast<const MapPat&>(p);
        out += "(map-pat";
        for (auto& kv : m.entries) {
            out += " (";
            render_expr_opt(kv.first.get(), out);
            out += " => ";
            render_pat(*kv.second, out);
            out += ')';
        }
        out += ')';
        break;
    }
    case PatKind::Or: {
        auto& o = static_cast<const OrPat&>(p);
        out += "(or-pat";
        for (auto& a : o.alts) { out += ' '; render_pat(*a, out); }
        out += ')';
        break;
    }
    }
}

void render_type(const Type& t, std::string& out) {
    switch (t.kind) {
    case TypeKind::Named: {
        auto& n = static_cast<const NamedType&>(t);
        if (n.args.empty()) { out += n.name; break; }
        out += '('; out += n.name;
        for (auto& a : n.args) { out += ' '; render_type(*a, out); }
        out += ')';
        break;
    }
    case TypeKind::Fn: {
        auto& f = static_cast<const FnType&>(t);
        out += "(fn-type (";
        for (size_t k = 0; k < f.params.size(); ++k) { if (k) out += ' '; render_type(*f.params[k], out); }
        out += ") "; render_type_opt(f.ret.get(), out); out += ')';
        break;
    }
    case TypeKind::Tuple: {
        auto& tt = static_cast<const TupleType&>(t);
        out += "(tuple-type";
        for (auto& e : tt.elems) { out += ' '; render_type(*e, out); }
        out += ')';
        break;
    }
    case TypeKind::Dyn: {
        auto& d = static_cast<const DynType&>(t);
        out += "(dyn ";
        if (!d.qualifier.empty()) { out += d.qualifier; out += "::"; }
        out += d.trait;
        for (auto& a : d.args) { out += ' '; render_type(*a, out); }
        out += ')';
        break;
    }
    }
}

void render_item(const Item& i, std::string& out) {
    switch (i.kind) {
    case ItemKind::Fn: {
        auto& f = static_cast<const FnItem&>(i);
        out += "(fn-item "; out += f.name;
        render_generics(f.generics, out);
        out += ' '; render_param_list(f.params, /*has_self=*/false, out);
        out += " -> "; render_type_opt(f.ret.get(), out);
        out += ' '; render_expr_opt(f.body.get(), out); out += ')';
        break;
    }
    case ItemKind::Struct: {
        auto& s = static_cast<const StructItem&>(i);
        out += "(struct-item "; out += s.name;
        render_generics(s.generics, out);
        if (s.is_tuple) out += " tuple";
        for (auto& fld : s.fields) {
            out += " ("; out += fld.name; out += ':'; render_type_opt(fld.type.get(), out); out += ')';
        }
        out += ')';
        break;
    }
    case ItemKind::Stmt:
        render_stmt(*static_cast<const StmtItem&>(i).stmt, out);
        break;
    case ItemKind::Trait: {
        auto& t = static_cast<const TraitDecl&>(i);
        out += "(trait "; out += t.name;
        render_generics(t.generics, out);
        if (!t.supertraits.empty()) {
            out += " (:";
            for (auto& s : t.supertraits) { out += ' '; out += s; }
            out += ')';
        }
        for (auto& m : t.methods) { out += ' '; render_method(m, out); }
        out += ')';
        break;
    }
    case ItemKind::Impl: {
        auto& im = static_cast<const ImplDecl&>(i);
        out += "(impl "; out += im.trait_name;
        if (!im.trait_args.empty()) {
            out += '[';
            for (size_t k = 0; k < im.trait_args.size(); ++k) { if (k) out += ','; render_type(*im.trait_args[k], out); }
            out += ']';
        }
        render_generics(im.generics, out);
        out += ' '; render_type_opt(im.target.get(), out);
        for (auto& m : im.methods) { out += ' '; render_method(m, out); }
        out += ')';
        break;
    }
    case ItemKind::Enum: {
        auto& en = static_cast<const EnumItem&>(i);
        out += "(enum "; out += en.name;
        render_generics(en.generics, out);
        for (auto& v : en.variants) {
            out += " ("; out += v.name;
            for (auto& fld : v.fields) { out += ' '; out += fld.name; out += ':'; render_type_opt(fld.type.get(), out); }
            out += ')';
        }
        out += ')';
        break;
    }
    case ItemKind::Import: {
        auto& im = static_cast<const ImportItem&>(i);
        out += "(import ";
        for (size_t k = 0; k < im.path.size(); ++k) { if (k) out += "::"; out += im.path[k]; }
        out += ')';
        break;
    }
    case ItemKind::Use: {
        auto& u = static_cast<const UseItem&>(i);
        out += "(use ";
        for (size_t k = 0; k < u.path.size(); ++k) { if (k) out += "::"; out += u.path[k]; }
        if (u.glob) {
            out += " *";
        } else {
            out += " {";
            for (size_t k = 0; k < u.names.size(); ++k) { if (k) out += ' '; out += u.names[k]; }
            out += '}';
        }
        out += ')';
        break;
    }
    case ItemKind::Const: {
        auto& c = static_cast<const ConstItem&>(i);
        out += "(const " + c.name + " ";
        if (c.type) render_type(*c.type, out);
        out += " = ";
        render_expr(*c.value, out);
        out += ')';
        break;
    }
    }
}

} // namespace

std::string dump(const Program& p) {
    std::string out;
    for (size_t k = 0; k < p.items.size(); ++k) {
        if (k) out += ' ';
        render_item(*p.items[k], out);
    }
    return out;
}

std::string dump_typed(const Program& p, bool include_prelude) {
    // Clear the flag even if a render throws (bad_alloc on a large program): a leaked `true`
    // would silently turn every later plain dump() into a typed one.
    struct FlagGuard { ~FlagGuard() { g_dump_typed = false; } } guard;
    g_dump_typed = true;
    std::string   out;
    const size_t  first = include_prelude ? 0 : p.prelude_item_count;
    for (size_t k = first; k < p.items.size(); ++k) {
        if (k != first) out += ' ';
        render_item(*p.items[k], out);
    }
    return out;
}

std::string dump_expr(const Expr& e) { std::string s; render_expr(e, s); return s; }
std::string dump_stmt(const Stmt& s) { std::string o; render_stmt(s, o); return o; }
std::string dump_pat(const Pattern& p) { std::string s; render_pat(p, s); return s; }
std::string dump_type(const Type& t) { std::string s; render_type(t, s); return s; }
std::string dump_item(const Item& i) { std::string s; render_item(i, s); return s; }

} // namespace svc
