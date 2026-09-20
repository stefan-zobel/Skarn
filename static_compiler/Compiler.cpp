// =============================================================================
// Compiler.cpp -- the Skarn compile() driver: source -> Module.
//
// lex -> parse -> CHECK -> lower. The pipeline REFUSES to lower a program the
// typechecker rejected (types are erased at runtime, so only a checked program is
// safe to run): a check error throws CheckFailure and no bytecode is produced.
// =============================================================================

#include "Compiler.h"
#include "Codegen.h"

#include "Lexer.h"
#include "Parser.h"
#include "Check.h"
#include "Ast.h"
#include "NativeRegistry.h"   // native_return_of -- a Result/Option native keeps its prelude enum

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace svc {

namespace {

// ----- prelude tree-shaking -------------------------------------------------
//
// After typechecking the WHOLE program (prelude + user), drop the prelude items the
// program does not transitively reference, so a program only carries the combinators it
// uses. Conservative by construction: an EXHAUSTIVE reference walk over every AST node
// (a missed node kind would UNDER-keep -> break a real program), collecting every
// referenced name; a prelude fn/enum/struct/trait whose name is reached is kept, to a
// fixpoint. `?` discriminates Option/Result by name, so a TryExpr keeps both (if the prelude
// declares them). EVERY prelude item kind except a top-level Stmt shakes: a Fn/Enum/Struct by
// its name, a Trait by its name / a method-name call (free `m(x)` or method syntax `x.m()`) / a
// generic bound / a supertrait / the `for`-Iterable protocol, and an Impl transitively when its trait
// is kept (an impl is reached by type+trait dispatch, not by name, so keeping a trait keeps ALL its
// impls -- a sound over-approximation that never under-keeps a needed impl).

// `names` is every referenced name, matched against all prelude item kinds. `methods` holds the names
// seen ONLY as a method-syntax callee `x.m(...)`: such a name can reach a trait method (or an inherent
// one, kept through its type) but never a free fn / struct / const, so it keeps traits only -- matching
// it against everything would drag in a same-named prelude fn for every `.size()`-style call.
struct RefSet { std::unordered_set<std::string>* names; std::unordered_set<std::string>* methods; bool* uses_try; };

void cref_expr(const Expr& e, RefSet r);
void cref_stmt(const Stmt& s, RefSet r);

void cref_type(const Type& t, RefSet r) {
    switch (t.kind) {
    case TypeKind::Named: {
        const auto& nt = static_cast<const NamedType&>(t);
        r.names->insert(nt.name);
        for (const auto& a : nt.args) cref_type(*a, r);
        break;
    }
    case TypeKind::Fn: {
        const auto& ft = static_cast<const FnType&>(t);
        for (const auto& p : ft.params) cref_type(*p, r);
        if (ft.ret) cref_type(*ft.ret, r);
        break;
    }
    case TypeKind::Tuple: {
        const auto& tt = static_cast<const TupleType&>(t);
        for (const auto& e : tt.elems) cref_type(*e, r);
        break;
    }
    case TypeKind::Dyn: {
        // `dyn Show` NAMES a trait, so it is a reference that must keep a shakeable prelude
        // trait alive (and with it, its impls) -- exactly like a generic bound `T: Show` in
        // cref_generics below. Without this, a program whose only mention of Show is a
        // `dyn Show` annotation would have the trait shaken out from under it.
        const auto& dt = static_cast<const DynType&>(t);
        r.names->insert(dt.trait);
        for (const auto& a : dt.args) cref_type(*a, r);
        break;
    }
    }
}

// A generic-parameter list (`[T: Show, I: Iterable[T]]`): each bound names a trait (a reference
// that must keep a shakeable prelude trait) and may carry type args. Walking bounds is required so
// a kept bounded fn/impl (`fn map[I: Iterable[T]]`) keeps its bound traits.
void cref_generics(const std::vector<GenericParam>& gps, RefSet r) {
    for (const GenericParam& gp : gps)
        for (const BoundRef& b : gp.bounds) {
            r.names->insert(b.trait);
            for (const auto& a : b.args) cref_type(*a, r);
        }
}

void cref_pat(const Pattern& p, RefSet r) {
    switch (p.kind) {
    case PatKind::Wildcard: case PatKind::Ident: break;
    // `n @ sub` -- the NAME references nothing, but `sub` may name a ctor the tree-shaker must keep.
    case PatKind::Bind: { const auto& bp = static_cast<const BindPat&>(p); if (bp.sub) cref_pat(*bp.sub, r); break; }
    case PatKind::Literal: { const auto& lp = static_cast<const LiteralPat&>(p); if (lp.lit) cref_expr(*lp.lit, r); break; }
    case PatKind::Range: { const auto& rp = static_cast<const RangePat&>(p); if (rp.lo) cref_expr(*rp.lo, r); if (rp.hi) cref_expr(*rp.hi, r); break; }
    case PatKind::Ctor: {
        const auto& cp = static_cast<const CtorPat&>(p);
        r.names->insert(cp.name);
        for (const auto& e : cp.elems) cref_pat(*e, r);
        break;
    }
    case PatKind::Tuple: { const auto& tp = static_cast<const TuplePat&>(p); for (const auto& e : tp.elems) cref_pat(*e, r); break; }
    case PatKind::List: {
        const auto& lp = static_cast<const ListPat&>(p);
        for (const auto& e : lp.elems) cref_pat(*e, r);
        if (lp.rest) cref_pat(*lp.rest, r);
        break;
    }
    case PatKind::Struct: {
        const auto& sp = static_cast<const StructPat&>(p);
        r.names->insert(sp.name);
        for (const auto& f : sp.fields) if (f.pat) cref_pat(*f.pat, r);
        break;
    }
    case PatKind::Map: {
        const auto& mp = static_cast<const MapPat&>(p);
        for (const auto& kv : mp.entries) { if (kv.first) cref_expr(*kv.first, r); if (kv.second) cref_pat(*kv.second, r); }
        break;
    }
    case PatKind::Or: {   // `A | B | C` -- each alternative may name a ctor/struct that must be kept
        const auto& op = static_cast<const OrPat&>(p);
        for (const auto& a : op.alts) cref_pat(*a, r);
        break;
    }
    }
}

void cref_expr(const Expr& e, RefSet r) {
    switch (e.kind) {
    case ExprKind::IntLit: case ExprKind::DoubleLit: case ExprKind::StrLit:
    case ExprKind::BoolLit:
    case ExprKind::Continue:
        break;
    case ExprKind::Break:  { const auto& br = static_cast<const BreakExpr&>(e); if (br.value) cref_expr(*br.value, r); break; }
    case ExprKind::Ident: {
        const auto& id = static_cast<const IdentExpr&>(e);
        r.names->insert(id.name);
        if (!id.qualifier.empty()) r.names->insert(id.qualifier);
        break;
    }
    case ExprKind::Unary:  { const auto& u = static_cast<const UnaryExpr&>(e);  if (u.operand) cref_expr(*u.operand, r); break; }
    case ExprKind::Binary: { const auto& b = static_cast<const BinaryExpr&>(e); if (b.lhs) cref_expr(*b.lhs, r); if (b.rhs) cref_expr(*b.rhs, r); break; }
    case ExprKind::Pipe:   { const auto& p = static_cast<const PipeExpr&>(e);   if (p.lhs) cref_expr(*p.lhs, r); if (p.rhs) cref_expr(*p.rhs, r); break; }
    case ExprKind::Call:   {
        const auto& c = static_cast<const CallExpr&>(e);
        if (c.callee) cref_expr(*c.callee, r);
        for (const auto& a : c.args) cref_expr(*a, r);
        // Method syntax `recv.m(args)`: the method name is the Field callee's name, which the Field case
        // (a plain field read) deliberately does not record. Without it, a prelude trait reached only as
        // `o.unwrapOr(0)` is shaken out after the checker accepted the program, and codegen then misreads
        // `.unwrapOr` as a struct field.
        if (c.callee && c.callee->kind == ExprKind::Field) {
            const auto& fld = static_cast<const FieldExpr&>(*c.callee);
            if (!fld.tuple_index) r.methods->insert(fld.name);
        }
        // A `toString`/`print`/`println` (or `${}` desugars to toString) of a Char-typed argument is
        // lowered by codegen to a SYNTHESIZED `codePointToStr(cp)` call (the glyph), so the fn name never
        // appears literally -- keep the shakeable prelude encoder here. (Conservative: the check is by the
        // "Char" short name, a superset of codegen's transparent-struct test -- never under-keeps.)
        if (c.callee && c.callee->kind == ExprKind::Ident) {
            const auto& id = static_cast<const IdentExpr&>(*c.callee);
            if (id.qualifier.empty() && (id.name == "toString" || id.name == "print" || id.name == "println")) {
                for (const auto& a : c.args)
                    if (a->ty && a->ty->kind == TyKind::Named && short_name(a->ty->name) == "Char") {
                        r.names->insert(std_codePointToStr());
                        break;
                    }
            }
        }
        break;
    }
    case ExprKind::Field:  { const auto& f = static_cast<const FieldExpr&>(e);  if (f.obj) cref_expr(*f.obj, r); break; }
    case ExprKind::Index:  { const auto& ix = static_cast<const IndexExpr&>(e); if (ix.obj) cref_expr(*ix.obj, r); if (ix.index) cref_expr(*ix.index, r); break; }
    case ExprKind::Try:    { const auto& t = static_cast<const TryExpr&>(e); *r.uses_try = true; if (t.operand) cref_expr(*t.operand, r); break; }
    case ExprKind::If:     { const auto& i = static_cast<const IfExpr&>(e); if (i.cond) cref_expr(*i.cond, r); if (i.then_blk) cref_expr(*i.then_blk, r); if (i.else_blk) cref_expr(*i.else_blk, r); break; }
    case ExprKind::Match:  {
        const auto& m = static_cast<const MatchExpr&>(e);
        if (m.scrut) cref_expr(*m.scrut, r);
        for (const auto& arm : m.arms) { if (arm.pat) cref_pat(*arm.pat, r); if (arm.guard) cref_expr(*arm.guard, r); if (arm.body) cref_expr(*arm.body, r); }
        break;
    }
    case ExprKind::While:  { const auto& w = static_cast<const WhileExpr&>(e); if (w.cond) cref_expr(*w.cond, r); if (w.body) cref_expr(*w.body, r); break; }
    case ExprKind::Loop:   { const auto& l = static_cast<const LoopExpr&>(e); if (l.body) cref_expr(*l.body, r); break; }
    case ExprKind::For:    {
        const auto& f = static_cast<const ForExpr&>(e);
        if (f.pat) cref_pat(*f.pat, r);
        if (f.iter) {
            cref_expr(*f.iter, r);
            // A `for` over a NON-builtin-container iterable drives one of the (hard-coded-named) iteration
            // protocols -- the lazy `Iterator`/`IntoIterator` (a cursor, a stream, or a user collection)
            // or the eager legacy `Iterable` (mirrors Check.cpp for_elem_type). Codegen SYNTHESIZES the
            // next()/intoIter()/iter() call, so the trait name never appears literally -- keep the shakeable
            // prelude traits here. Conservative: a null/unknown element type also keeps them.
            const TyPtr& t = f.iter->ty;
            bool builtin_iter = false;
            if (t && t->kind == TyKind::Named)
                builtin_iter = t->name == "List" || t->name == "Array" || t->name == "Vec" ||
                               t->name == "Bytes" || t->name == "Map" || t->name == "String";
            if (!builtin_iter) {
                r.names->insert(std_Iterator());
                r.names->insert(std_IntoIterator());
                r.names->insert(std_Iterable());
            }
        }
        if (f.body) cref_expr(*f.body, r);
        break;
    }
    case ExprKind::Return: { const auto& rt = static_cast<const ReturnExpr&>(e); if (rt.value) cref_expr(*rt.value, r); break; }
    case ExprKind::Block:  { const auto& b = static_cast<const BlockExpr&>(e); for (const auto& s : b.stmts) cref_stmt(*s, r); break; }
    case ExprKind::Lambda: {
        const auto& lam = static_cast<const LambdaExpr&>(e);
        for (const auto& p : lam.params) if (p.type) cref_type(*p.type, r);
        if (lam.ret) cref_type(*lam.ret, r);
        if (lam.body) cref_expr(*lam.body, r);
        break;
    }
    case ExprKind::StructLit: {
        const auto& sl = static_cast<const StructLit&>(e);
        r.names->insert(sl.name);
        for (const auto& fi : sl.fields) if (fi.value) cref_expr(*fi.value, r);
        if (sl.base) cref_expr(*sl.base, r);   // record update: the base's refs are used too
        break;
    }
    case ExprKind::Tuple:   { const auto& t = static_cast<const TupleExpr&>(e); for (const auto& x : t.elems) cref_expr(*x, r); break; }
    case ExprKind::ListLit: { const auto& ll = static_cast<const ListLit&>(e); for (const auto& x : ll.elems) cref_expr(*x, r); break; }
    case ExprKind::MapLit:  { const auto& ml = static_cast<const MapLit&>(e); for (const auto& kv : ml.entries) { if (kv.first) cref_expr(*kv.first, r); if (kv.second) cref_expr(*kv.second, r); } break; }
    }
}

void cref_stmt(const Stmt& s, RefSet r) {
    switch (s.kind) {
    case StmtKind::Let:    { const auto& l = static_cast<const LetStmt&>(s); if (l.pat) cref_pat(*l.pat, r); if (l.type) cref_type(*l.type, r); if (l.init) cref_expr(*l.init, r); break; }
    case StmtKind::Assign: { const auto& a = static_cast<const AssignStmt&>(s); if (a.target) cref_expr(*a.target, r); if (a.value) cref_expr(*a.value, r); break; }
    case StmtKind::Expr:   { const auto& es = static_cast<const ExprStmt&>(s); if (es.expr) cref_expr(*es.expr, r); break; }
    }
}

void cref_item(const Item& it, RefSet r) {
    switch (it.kind) {
    case ItemKind::Fn: {
        const auto& f = static_cast<const FnItem&>(it);
        cref_generics(f.generics, r);
        for (const auto& p : f.params) if (p.type) cref_type(*p.type, r);
        if (f.ret) cref_type(*f.ret, r);
        if (f.body) cref_expr(*f.body, r);
        break;
    }
    case ItemKind::Stmt:   { const auto& si = static_cast<const StmtItem&>(it); if (si.stmt) cref_stmt(*si.stmt, r); break; }
    case ItemKind::Const:  { const auto& c = static_cast<const ConstItem&>(it); if (c.type) cref_type(*c.type, r); if (c.value) cref_expr(*c.value, r); break; }
    case ItemKind::Enum:   { const auto& e = static_cast<const EnumItem&>(it); cref_generics(e.generics, r); for (const auto& v : e.variants) for (const auto& fld : v.fields) if (fld.type) cref_type(*fld.type, r); break; }
    case ItemKind::Struct: { const auto& s = static_cast<const StructItem&>(it); cref_generics(s.generics, r); for (const auto& fld : s.fields) if (fld.type) cref_type(*fld.type, r); break; }
    case ItemKind::Trait:  {
        const auto& t = static_cast<const TraitDecl&>(it);
        cref_generics(t.generics, r);
        for (const auto& su : t.supertraits) r.names->insert(su);
        for (const auto& m : t.methods) { cref_generics(m.generics, r); for (const auto& p : m.params) if (p.type) cref_type(*p.type, r); if (m.ret) cref_type(*m.ret, r); if (m.body) cref_expr(*m.body, r); }
        break;
    }
    case ItemKind::Impl:   {
        const auto& im = static_cast<const ImplDecl&>(it);
        cref_generics(im.generics, r);
        r.names->insert(im.trait_name);
        for (const auto& a : im.trait_args) cref_type(*a, r);
        if (im.target) cref_type(*im.target, r);
        for (const auto& m : im.methods) { cref_generics(m.generics, r); for (const auto& p : m.params) if (p.type) cref_type(*p.type, r); if (m.ret) cref_type(*m.ret, r); if (m.body) cref_expr(*m.body, r); }
        break;
    }
    case ItemKind::Import:
    case ItemKind::Use:
        break;   // module items contribute no reachability edges yet (Slice 2 handles cross-module)
    }
}

void tree_shake_prelude(Program& prog) {
    const size_t npre = prog.prelude_item_count;
    if (npre == 0) return;
    const size_t n = prog.items.size();

    // Every prelude item EXCEPT a top-level statement is shakeable (the prelude declares only
    // items). Index the shakeable prelude items by the names that reach them.
    auto is_shakeable = [&](size_t i) { return i < npre && prog.items[i]->kind != ItemKind::Stmt; };
    std::unordered_map<std::string, size_t> fn_idx, enum_idx, variant_to_enum, struct_idx, trait_idx, const_idx;
    std::unordered_map<std::string, std::vector<size_t>> method_to_trait, impls_by_trait, struct_by_bare;
    std::unordered_map<std::string, std::vector<size_t>> inherent_by_head;   // S2: inherent impls per target head
    // The BARE name = the segment after the last "::" (identity when unmangled). A struct referenced
    // only through a TYPE ANNOTATION (not constructed) is seen BARE by `cref`, while struct_idx is keyed
    // MANGLED -- so index structs by their bare name too and consult it below (the general fix for the
    // rawRun/ProcessOutput class of drop; sound: at worst it over-keeps a same-bare-named struct).
    auto bare_of = [](const std::string& s) {
        const size_t p = s.rfind("::");
        return p == std::string::npos ? s : s.substr(p + 2);
    };
    for (size_t i = 0; i < npre; ++i) {
        if (!is_shakeable(i)) continue;
        const Item& it = *prog.items[i];
        if (it.kind == ItemKind::Fn)
            fn_idx[static_cast<const FnItem&>(it).name] = i;
        else if (it.kind == ItemKind::Enum) {
            const auto& e = static_cast<const EnumItem&>(it);
            enum_idx[e.name] = i;
            for (const auto& v : e.variants) variant_to_enum[v.name] = i;
        }
        else if (it.kind == ItemKind::Struct) {
            const std::string& sn = static_cast<const StructItem&>(it).name;
            struct_idx[sn] = i;
            struct_by_bare[bare_of(sn)].push_back(i);   // catch a bare-name TYPE reference too
        }
        else if (it.kind == ItemKind::Const)
            const_idx[static_cast<const ConstItem&>(it).name] = i;
        else if (it.kind == ItemKind::Trait) {
            const auto& t = static_cast<const TraitDecl&>(it);
            trait_idx[t.name] = i;
            for (const Method& m : t.methods) method_to_trait[m.name].push_back(i);
        }
        else if (it.kind == ItemKind::Impl) {
            const auto& im = static_cast<const ImplDecl&>(it);
            if (im.is_inherent) {   // S2: keyed by target head -> kept when the target type is kept
                if (im.target && im.target->kind == TypeKind::Named)
                    inherent_by_head[static_cast<const NamedType&>(*im.target).name].push_back(i);
            } else {
                impls_by_trait[im.trait_name].push_back(i);
            }
        }
    }

    std::vector<char> kept(n, 0);
    std::vector<size_t> work;
    auto keep = [&](size_t idx) { if (!kept[idx]) { kept[idx] = 1; work.push_back(idx); } };
    // Seed: every USER item, and every non-shakeable prelude item (top-level stmts only), is a root.
    for (size_t i = 0; i < n; ++i)
        if (!is_shakeable(i)) keep(i);

    while (!work.empty()) {
        const size_t i = work.back(); work.pop_back();
        // Keeping a trait keeps ALL its impls (sound: an impl is reachable by type+trait dispatch,
        // not by name, so over-keep rather than risk under-keeping a needed impl).
        if (prog.items[i]->kind == ItemKind::Trait)
            if (auto it = impls_by_trait.find(static_cast<const TraitDecl&>(*prog.items[i]).name);
                it != impls_by_trait.end())
                for (size_t im : it->second) keep(im);
        // S2: keeping a TYPE keeps its inherent impl(s) -- an inherent method is reached by `.name()`
        // on the type, not by a bare name, so over-keep (like the trait rule) rather than under-keep.
        if (prog.items[i]->kind == ItemKind::Struct || prog.items[i]->kind == ItemKind::Enum) {
            const std::string& tn = prog.items[i]->kind == ItemKind::Struct
                ? static_cast<const StructItem&>(*prog.items[i]).name
                : static_cast<const EnumItem&>(*prog.items[i]).name;
            if (auto it = inherent_by_head.find(tn); it != inherent_by_head.end())
                for (size_t im : it->second) keep(im);
        }
        std::unordered_set<std::string> names, methods; bool uses_try = false;
        cref_item(*prog.items[i], RefSet{ &names, &methods, &uses_try });
        for (const auto& nm : methods)   // method syntax reaches trait methods only (see RefSet)
            if (auto it = method_to_trait.find(nm); it != method_to_trait.end()) for (size_t ti : it->second) keep(ti);
        for (const auto& nm : names) {
            if (auto it = fn_idx.find(nm);          it != fn_idx.end())          keep(it->second);
            if (auto it = enum_idx.find(nm);        it != enum_idx.end())        keep(it->second);
            if (auto it = variant_to_enum.find(nm); it != variant_to_enum.end()) keep(it->second);
            if (auto it = struct_idx.find(nm);      it != struct_idx.end())      keep(it->second);
            // A struct named only in a type annotation reaches cref BARE -> match by bare name too.
            if (auto it = struct_by_bare.find(bare_of(nm)); it != struct_by_bare.end())
                for (size_t si : it->second) keep(si);
            if (auto it = const_idx.find(nm);       it != const_idx.end())       keep(it->second);
            if (auto it = trait_idx.find(nm);       it != trait_idx.end())       keep(it->second);
            if (auto it = method_to_trait.find(nm); it != method_to_trait.end()) for (size_t ti : it->second) keep(ti);
        }
        if (uses_try) {   // `?` keys Option/Result by name -> keep whichever the prelude declares
            if (auto it = enum_idx.find(std_Option()); it != enum_idx.end()) keep(it->second);
            if (auto it = enum_idx.find(std_Result()); it != enum_idx.end()) keep(it->second);
        }
        // The Option-wrapping builtins (`get`, `pop`) build Some/None in codegen, so their Option
        // enum must survive even if the program never names Some/None itself.
        if ((names.count("get") || names.count("pop")))
            if (auto it = enum_idx.find(std_Option()); it != enum_idx.end()) keep(it->second);
        // A native I/O call whose result is wrapped (readFile -> Result, getEnv -> Option) builds
        // Ok/Err / Some/None in codegen, so keep the enum even if the program never names them.
        for (const auto& nm : names) {
            const int nid = native_id_of(nm);
            if (nid < 0) continue;
            const std::string want = native_return_of(nid) == NRET_RESULT ? std_Result()
                                   : native_return_of(nid) == NRET_OPTION ? std_Option() : std::string();
            if (!want.empty())
                if (auto it = enum_idx.find(want); it != enum_idx.end()) keep(it->second);
        }
        // rawRun's codegen (emit_wrap_process) BUILDS a ProcessOutput struct from the raw native
        // array. The program reaches ProcessOutput only through run's return TYPE annotation, which
        // cref sees BARE while struct_idx is keyed MANGLED -- so keep it explicitly when rawRun is
        // referenced (the struct analogue of the get/pop -> Some/None seeding above).
        if (names.count("rawRun"))
            if (auto it = struct_idx.find(std_ProcessOutput()); it != struct_idx.end()) keep(it->second);
    }

    // Rebuild items in order, dropping the unreached prelude items; recompute the boundary.
    std::vector<ItemPtr> out;
    out.reserve(n);
    size_t kept_pre = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!kept[i]) continue;
        if (i < npre) ++kept_pre;
        out.push_back(std::move(prog.items[i]));
    }
    prog.items = std::move(out);
    prog.prelude_item_count = kept_pre;
}

std::string first_error_summary(const std::vector<TypeError>& errs) {
    if (errs.empty()) return "type check failed";
    const TypeError& e = errs.front();
    std::string msg = "type error: " + e.message;
    if (e.line) msg += " at line " + std::to_string(e.line) + ":" + std::to_string(e.col);
    if (errs.size() > 1) msg += " (+" + std::to_string(errs.size() - 1) + " more)";
    return msg;
}
} // namespace

CheckFailure::CheckFailure(std::vector<TypeError> errs)
    : std::runtime_error(first_error_summary(errs)), errors_(std::move(errs)) {}

namespace {
Program parse_unit(const std::string& src) {
    Lexer lexer(src);
    Parser parser(lexer.tokenize());
    return parser.parse_program();
}

// Parse each prelude module (a { prefix, source } unit), stamp its items with the module's
// prefix, and append them to `out`. Returns the total prelude item count (the tree-shaker
// boundary). A prelude-origin parse error is re-tagged "prelude: ..." so it is never blamed
// on user source. Pre-split the list is one { "$prelude", ... } entry (mangles to bare).
size_t append_prelude_items(const std::vector<PreludeModule>* prelude, std::vector<ItemPtr>& out) {
    size_t npre = 0;
    if (!prelude) return 0;
    for (const auto& pm : *prelude) {
        if (pm.source.empty()) continue;
        Program pre;
        try {
            pre = parse_unit(pm.source);
        } catch (const ParseError& e) {
            throw ParseError(std::string("prelude: ") + e.what(), e.line(), e.col());
        }
        // A std module HONORS explicit `pub` exactly like user code: `pub` exports, unmarked = module-
        // private (same-module access always allowed). We keep whatever `is_pub` the parser set from the
        // `pub` keyword -- only the module prefix is stamped here. (A monolithic `$prelude` unit is one
        // module, so all its refs are same-module and need no `pub`.)
        for (auto& it : pre.items) { it->module_prefix = pm.prefix; out.push_back(std::move(it)); }
        npre += pre.items.size();
    }
    return npre;
}
} // namespace

Program parse_check(const char* source, const std::vector<PreludeModule>* prelude,
                    std::vector<TypeError>* out_warnings) {
    const std::string src = source ? source : "";
    Program prog = parse_unit(src);

    // Stamp the entry program's module prefix. It is an ordinary prefix (see ENTRY_MODULE_PREFIX):
    // without it these items would carry the empty prefix and mangle BARE, sharing the namespace of
    // the ambient builtins / natives / trait-method names.
    for (auto& it : prog.items) it->module_prefix = ENTRY_MODULE_PREFIX;

    // Prepend the prelude modules' items (each tagged with its module prefix) and record the
    // boundary. The user's top-level statements stay the "main" region (the prelude declares
    // only items, no stmts). npre == 0 (no prelude) leaves the program byte-identical.
    std::vector<ItemPtr> combined;
    const size_t npre = append_prelude_items(prelude, combined);
    combined.reserve(combined.size() + prog.items.size());
    for (auto& it : prog.items) combined.push_back(std::move(it));
    prog.items = std::move(combined);
    prog.prelude_item_count = npre;

    // Typecheck the WHOLE combined program (the prelude is our only guard under erasure, so
    // it is checked too). Refuse to hand back a rejected (hence potentially unsound) tree.
    CheckResult cr = check(prog);
    if (!cr.ok())
        throw CheckFailure(std::move(cr.errors));
    if (out_warnings) *out_warnings = std::move(cr.warnings);
    return prog;
}

namespace {
// The combined Program behind parse_check_modules and check_modules_for_tools.
Program assemble_modules(ModuleSet& modules, const std::vector<PreludeModule>* prelude) {
    // Assemble ONE program: the prelude modules' items first (each tagged with its own module
    // prefix), then every user module's items in the loader's topological order (dependencies
    // before dependents; the entry/root module last). The prelude/user boundary is recorded for
    // tree-shaking exactly as parse_check.
    Program combined;
    const size_t npre = append_prelude_items(prelude, combined.items);
    for (auto& mod : modules.modules)
        for (auto& it : mod.program.items) {
            // The loader keys the entry module as "" (that key carries its cycle detection and its
            // "imported by" messages, so it stays); the CHECKER sees it as an ordinary module under
            // ENTRY_MODULE_PREFIX. "util" / "net::http" pass through unchanged.
            it->module_prefix = mod.name.empty() ? ENTRY_MODULE_PREFIX : mod.name;
            combined.items.push_back(std::move(it));
        }
    combined.prelude_item_count = npre;
    return combined;
}
} // namespace

Program parse_check_modules(ModuleSet modules, const std::vector<PreludeModule>* prelude,
                            std::vector<TypeError>* out_warnings) {
    Program combined = assemble_modules(modules, prelude);
    CheckResult cr = check(combined);
    if (!cr.ok())
        throw CheckFailure(std::move(cr.errors));
    if (out_warnings) *out_warnings = std::move(cr.warnings);
    return combined;
}

ToolCheck check_modules_for_tools(ModuleSet modules, const std::vector<PreludeModule>* prelude) {
    ToolCheck out;
    out.program = assemble_modules(modules, prelude);
    CheckOptions options;
    options.resolve_types = true;
    options.list_ambient  = true;
    CheckResult cr = check(out.program, options);
    out.errors   = std::move(cr.errors);
    out.warnings = std::move(cr.warnings);
    out.ambient  = std::move(cr.ambient);
    return out;
}

// The inlining knob (declared in Compiler.h). File-scope, read only when a Codegen is built.
static bool g_inline_calls  = true;    // ON by default since the S5 measurement (see Compiler.h)
static bool g_inline_report = false;
void set_inline_calls(bool on)  { g_inline_calls  = on; }
bool inline_calls()             { return g_inline_calls; }
void set_inline_report(bool on) { g_inline_report = on; }
bool inline_report()            { return g_inline_report; }
static int g_inline_max_nodes = 20;
static int g_inline_max_depth = 2;
void set_inline_max_nodes(int n) { g_inline_max_nodes = n; }
int  inline_max_nodes()          { return g_inline_max_nodes; }
void set_inline_max_depth(int d) { g_inline_max_depth = d; }
int  inline_max_depth()          { return g_inline_max_depth; }

// The SROA knob (declared in Compiler.h). Same file-scope shape as inlining above.
static bool g_sroa            = false;   // OFF while the slices land (see Compiler.h)
static bool g_sroa_report     = false;
static int  g_sroa_max_fields = 4;
void set_sroa(bool on)           { g_sroa = on; }
bool sroa()                      { return g_sroa; }
void set_sroa_report(bool on)    { g_sroa_report = on; }
bool sroa_report()               { return g_sroa_report; }
void set_sroa_max_fields(int n)  { g_sroa_max_fields = n; }
int  sroa_max_fields()           { return g_sroa_max_fields; }

// ONE place where the process-wide codegen knobs reach a Codegen. It used to be two identical
// blocks, one per compile entry point, and that duplication has already cost a bug: after the
// inlining default was flipped on, a driver overriding only one path kept it off and the flip
// looked like it had no effect. A second optimisation doubles the ways to get that wrong, so the
// blocks are merged instead -- a new knob is now added in exactly one function.
static void configure_codegen(Codegen& cg) {
    cg.set_inlining(inline_calls());
    cg.set_inline_report(inline_report());
    cg.set_inline_max_nodes(inline_max_nodes());
    cg.set_inline_max_depth(inline_max_depth());
    cg.set_sroa(sroa());
    cg.set_sroa_report(sroa_report());
    cg.set_sroa_max_fields(sroa_max_fields());
}

Module compile_modules(ModuleSet modules, const std::vector<PreludeModule>* prelude) {
    std::vector<TypeError> warnings;
    Program prog = parse_check_modules(std::move(modules), prelude, &warnings);
    tree_shake_prelude(prog);
    Codegen cg;
    configure_codegen(cg);
    Module m = cg.generate(prog);
    m.warnings = std::move(warnings);
    return m;
}

Module compile(const char* source, const std::vector<PreludeModule>* prelude) {
    std::vector<TypeError> warnings;
    Program prog = parse_check(source, prelude, &warnings);

    // Drop the prelude items the (checked) program does not transitively use, so a program
    // carries only the combinators it references. Runs AFTER check (the whole prelude is
    // type-checked) and only affects which items codegen emits.
    tree_shake_prelude(prog);

    Codegen cg;
    configure_codegen(cg);
    Module m = cg.generate(prog);
    m.warnings = std::move(warnings);
    return m;
}

// The static prelude (P7c). Only what the first-order, statically-typed language can express
// cleanly: the Option/Result sum types + their fully-typed combinators. NOTE: the dynamic
// prelude's universally-polymorphic iteration combinators (map(coll,f) over ANY iterable) are
// deliberately absent -- this language has no Iterable abstraction (first-order generics; `for`
// ranges only over the built-in containers), so a combinator must fix one container type. The
// container ops that ARE polymorphic over kinds (len/pop/keys/...) and the universal ones
// (print/toString/panic) are codegen builtins, not prelude functions.
const std::vector<PreludeModule>& builtin_prelude() {
    // The prelude modules are generated from the std/*.skn sources (the SINGLE source of
    // truth) by the static_compiler pre-build. Edit the .skn files, not this. The .inc is a
    // sequence of `{ "<prefix>", R"(...)" ... },` initializer entries (one per prelude module).
    static const std::vector<PreludeModule> mods = {
#include "prelude_embed.inc"
    };
    return mods;
}

bcio::ModuleImage module_to_image(const Module& m) {
    bcio::ModuleImage img;
    img.bytecode           = m.bytecode;
    img.constants          = m.constants;
    img.const_arrays       = m.const_arrays;
    img.struct_types       = m.struct_types;
    img.string_literals    = m.string_literals;
    img.function_table     = m.function_table;
    img.trait_table        = m.trait_table;
    img.trait_table_width  = m.trait_table_width;
    img.trait_method_count = m.trait_method_count;
    img.top_frame_size     = m.top_frame_size;
    img.line_table         = m.line_table;
    img.column_table       = m.column_table;
    img.function_names     = m.function_names;
    img.function_modules   = m.function_modules;
    return img;
}

} // namespace svc
