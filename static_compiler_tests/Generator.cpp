#include "Generator.h"

#include <random>
#include <string>
#include <utility>
#include <vector>

namespace gen {
namespace {

// The generator's type universe. Scalars (Int/Double/Bool/String) plus three composites:
// a named record STRUCT (referenced by its index into `structs`), an anonymous TUPLE
// (its element types listed inline), and a named ENUM (referenced by its index into
// `enums`). Struct FIELDS / enum-variant PAYLOADS are always scalar and tuple ELEMENTS
// are scalar-or-struct (never a nested tuple), so every GType is constructible from
// literals in a bounded number of steps -- `leaf` can always synthesize a value.
struct GType {
    // Vec/Map are FIRST-CLASS: a Vec's element type is elems[0]; a Map's key/value are elems[0]/elems[1].
    // Elements/keys/values are always SCALAR (never nested containers / composites), so every GType is
    // still constructible from literals in a bounded number of steps.
    enum K { Int, Double, Bool, String, Struct, Tuple, Enum, Vec, Map, Newtype } kind = Int;
    int sid = -1;              // structs[] index when kind == Struct
    int eid = -1;              // enums[] index when kind == Enum
    int nid = -1;              // newtypes[] index when kind == Newtype (a transparent struct over a scalar)
    std::vector<GType> elems;  // Tuple elements / Vec elem (elems[0]) / Map key,value (elems[0..1])
};

bool is_scalar(const GType& t) {
    return t.kind == GType::Int || t.kind == GType::Double ||
           t.kind == GType::Bool || t.kind == GType::String;
}
bool is_container(const GType& t) { return t.kind == GType::Vec || t.kind == GType::Map; }
bool same(const GType& a, const GType& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case GType::Struct:  return a.sid == b.sid;
        case GType::Newtype: return a.nid == b.nid;
        case GType::Enum:   return a.eid == b.eid;
        case GType::Vec:    return same(a.elems[0], b.elems[0]);
        case GType::Map:    return same(a.elems[0], b.elems[0]) && same(a.elems[1], b.elems[1]);
        case GType::Tuple:
            if (a.elems.size() != b.elems.size()) return false;
            for (size_t i = 0; i < a.elems.size(); ++i)
                if (!same(a.elems[i], b.elems[i])) return false;
            return true;
        default: return true;  // scalar kinds carry no extra identity
    }
}

GType scalar(GType::K k) { GType t; t.kind = k; return t; }
GType struct_ty(int sid) { GType t; t.kind = GType::Struct; t.sid = sid; return t; }
GType newtype_ty(int nid){ GType t; t.kind = GType::Newtype; t.nid = nid; return t; }
GType enum_ty(int eid)   { GType t; t.kind = GType::Enum;   t.eid = eid; return t; }
GType vec_ty(GType e)    { GType t; t.kind = GType::Vec; t.elems = { std::move(e) }; return t; }
GType map_ty(GType k, GType v) { GType t; t.kind = GType::Map; t.elems = { std::move(k), std::move(v) }; return t; }

struct StructDef { std::string name; std::vector<std::pair<std::string, GType>> fields; };
// A transparent (erased) newtype `transparent struct Name(under)`, under a scalar Int/Double/Bool.
struct NewtypeDef { std::string name; GType under; };
struct EnumDef {
    std::string name;
    // fields empty => nullary; is_record => named-field brace form `V { f0: T0, .. }` (names are
    // synthesized `f0`, `f1`, .. deterministically), else positional tuple form `V(T0, ..)`.
    struct Variant { std::string name; std::vector<GType> fields; bool is_record = false; };
    std::vector<Variant> variants;
};
struct Binding   { std::string name; GType ty; bool is_mut = false; };
struct FnSig     { std::string name; std::vector<GType> params; GType ret; bool first_mut = false; };
struct HofSig    { std::string name; GType a; GType b; };          // fn hN(f: fn(a)->b, x: a) -> b { f(x) }
struct TraitDef  { std::string name, method, wrapper; GType ret; int sid; };  // one method, one impl struct
// A CONDITIONAL impl over a generic wrapper (gen_conditional_impls): `struct CwK[T] { inner: T, k: Int }`,
// `impl CtK for S`, `impl[T: CtK] CtK for CwK[T]`, a bounded wrapper fn `cfK[X: CtK]`, and a bounded GENERIC
// trait method on Int (`trait GbK { fn gbK[X: CtK](self, x: X) -> Int }`) whose impl repeats the bound.
struct CondImplDef  { std::string wrap, trait, method, fn, gtrait, gmethod; int sid; };
struct MutatorDef   { std::string name; int sid; std::string field; };   // fn mN(mut p0: S, p1: Int) -> Int
struct MutMethodDef { std::string method; int sid; std::string field; std::string trait; };  // trait+impl: fn bN(mut self) -> Int
// A traitless `impl S { .. }` block (gen_inherent). Every member is optional (an empty name = absent):
//   fn new(p0: F0, ..) -> S          an ASSOCIATED fn (no self), reachable only as `S::new(..)`
//   fn igK(self) -> get_ret          a plain method
//   fn iaK(self, d: arg_ty) -> arg_ret   a method with one argument
//   fn ibK(mut self) -> ()           an in-place mutator of the Int field `bump_field`
struct InherentDef {
    int sid = -1;
    bool has_new = false;
    std::string get_m;  GType get_ret;
    std::string arg_m;  GType arg_ty, arg_ret;
    std::string bump_m, bump_field;
};

struct Gen {
    std::mt19937 rng;
    std::vector<StructDef> structs;
    std::vector<NewtypeDef> newtypes;   // transparent (erased) newtypes over a scalar
    std::vector<EnumDef>   enums;
    std::vector<Binding>   env;
    std::vector<FnSig>     funcs;
    std::vector<HofSig>    hofs;
    std::vector<TraitDef>  traits;
    std::vector<CondImplDef>  conds;
    std::vector<MutatorDef>   mutators;
    std::vector<MutMethodDef> mut_methods;
    std::vector<InherentDef>  inherents;
    bool gid_declared = false, gtw_declared = false;   // generic id / twice helpers available
    bool prelude_mode = false;                          // emit Option/Result + `?` + combinators
    int name_counter = 0;

    explicit Gen(uint32_t seed) : rng(seed) {}

    uint32_t pick(uint32_t n) { return n ? (rng() % n) : 0; }
    bool chance(uint32_t pct) { return pick(100) < pct; }
    std::string fresh_name() { return "v" + std::to_string(name_counter++); }

    std::string ty_name(const GType& t) {
        switch (t.kind) {
            case GType::Int:    return "Int";
            case GType::Double: return "Double";
            case GType::Bool:   return "Bool";
            case GType::String: return "String";
            case GType::Struct: return structs[t.sid].name;
            case GType::Newtype: return newtypes[t.nid].name;
            case GType::Enum:   return enums[t.eid].name;
            case GType::Vec:    return "Vec[" + ty_name(t.elems[0]) + "]";
            case GType::Map:    return "Map[" + ty_name(t.elems[0]) + ", " + ty_name(t.elems[1]) + "]";
            default: {  // Tuple
                std::string s = "(";
                for (size_t i = 0; i < t.elems.size(); ++i) { if (i) s += ", "; s += ty_name(t.elems[i]); }
                return s + ")";
            }
        }
    }

    GType random_scalar() {
        switch (pick(4)) {
            case 0:  return scalar(GType::Int);
            case 1:  return scalar(GType::Double);
            case 2:  return scalar(GType::Bool);
            default: return scalar(GType::String);
        }
    }
    // A tuple type whose elements are scalar-or-struct (never a nested tuple -> bounded).
    GType tuple_type() {
        GType t; t.kind = GType::Tuple;
        const int n = 2 + static_cast<int>(pick(2));   // 2..3 elements
        for (int i = 0; i < n; ++i)
            t.elems.push_back((!structs.empty() && chance(25)) ? struct_ty(pick((uint32_t)structs.size()))
                                                               : random_scalar());
        return t;
    }
    // A Map key type: Int or String only -- both hash consistently (Int by bits, String by content) so
    // the VM and the oracle agree on key identity. (Double/Bool keys are avoided: NaN/+-0 edge cases.)
    GType random_map_key() { return chance(50) ? scalar(GType::Int) : scalar(GType::String); }
    // A first-class container type: Vec[scalar] or Map[Int|String, scalar].
    GType random_container() {
        return chance(55) ? vec_ty(random_scalar()) : map_ty(random_map_key(), random_scalar());
    }
    // A type demanded somewhere (param / return / let). Mostly scalar, sometimes composite/container.
    GType random_ty(bool allow_composite = true) {
        if (allow_composite && !newtypes.empty() && chance(15)) return newtype_ty(pick((uint32_t)newtypes.size()));
        if (allow_composite && !structs.empty() && chance(20)) return struct_ty(pick((uint32_t)structs.size()));
        if (allow_composite && !enums.empty()   && chance(18)) return enum_ty(pick((uint32_t)enums.size()));
        if (allow_composite && chance(15)) return tuple_type();
        if (allow_composite && chance(12)) return random_container();
        return random_scalar();
    }
    // A type for a `${...}` interpolation hole: any DIFFERENTIALLY-SAFE type -- scalars + Struct / Tuple
    // / Enum / Vec (their TO_STRING dump is deterministic and the oracle's render_dump mirrors it). A
    // **Map** is EXCLUDED: the VM dumps a map in hash/backing order, which the oracle cannot reproduce
    // byte-for-byte, so a map hole would be a spurious "compiler-bug". (Vec elems / struct fields / enum
    // payloads / tuple elems are never Maps, so a non-Map top type is fully safe to render.)
    GType random_interp_hole_type() {
        for (int tries = 0; tries < 8; ++tries) {
            GType t = random_ty(true);
            if (t.kind != GType::Map) return t;
        }
        return random_scalar();
    }

    // ---- in-scope binding lookups ----
    int var_of(const GType& t) {
        std::vector<int> idx;
        for (int i = 0; i < (int)env.size(); ++i) if (same(env[i].ty, t)) idx.push_back(i);
        return idx.empty() ? -1 : idx[pick((uint32_t)idx.size())];
    }
    std::string mut_var_of(const GType& t) {
        std::vector<int> idx;
        for (int i = 0; i < (int)env.size(); ++i) if (env[i].is_mut && same(env[i].ty, t)) idx.push_back(i);
        return idx.empty() ? std::string() : env[idx[pick((uint32_t)idx.size())]].name;
    }

    // ---- literals (all within the checker's accepted ranges) ----
    std::string int_lit() { return std::to_string((int64_t)pick(20001) - 10000); }
    std::string double_lit() {
        return std::to_string((int64_t)pick(201) - 100) + "." + std::to_string(pick(10));
    }
    std::string bool_lit() { return chance(50) ? "true" : "false"; }
    std::string string_lit() {
        std::string s = "\"";
        const int len = (int)pick(6);   // 0..5 lowercase letters (no escaping needed)
        for (int i = 0; i < len; ++i) s += (char)('a' + pick(26));
        return s + "\"";
    }
    std::string scalar_lit(const GType& t) {
        switch (t.kind) {
            case GType::Int:    return int_lit();
            case GType::Double: return double_lit();
            case GType::Bool:   return bool_lit();
            default:            return string_lit();
        }
    }
    // A raw (unquoted) text chunk for an interpolated string: 0..3 lowercase letters -- no `$`, `"`, or
    // `\`, so it never opens a `${` hole, closes the string, or forms an escape (keeps the literal inert
    // except for the deliberate holes).
    std::string interp_text_chunk() {
        std::string s;
        const int len = (int)pick(4);   // 0..3 lowercase letters
        for (int i = 0; i < len; ++i) s += (char)('a' + pick(26));
        return s;
    }
    // A valid `${e:spec}` format specifier for a SCALAR hole (base/precision/width/align matched to the
    // type). Pure-Skarn `format` runs in the oracle too, so any valid spec stays differentially safe.
    std::string random_spec(const GType& t) {
        switch (t.kind) {
            case GType::Int: {
                static const char* v[] = {"x","X","b","o","d","5",">6","<6","^6","06","08x","*^7",
                                          "+","+06","#x","#X","#b","#o","+#x","#010x"};   // v2: sign / alt-form
                return v[pick(20)];
            }
            case GType::Double: {
                static const char* v[] = {".2f",".0f",".1f","8.2f","010.3f",">10",".4f",
                                          "+.2f","e","E",".3e","+e","12.2e",".0e"};        // v2: sign / scientific
                return v[pick(14)];
            }
            case GType::String: {
                static const char* v[] = {"<8",">8","^8","*^7","s","10"};
                return v[pick(6)];
            }
            case GType::Bool: {
                static const char* v[] = {"^7","<6",">6","7"};
                return v[pick(4)];
            }
            default: return "";
        }
    }
    // An interpolated string literal `"t0 ${e0} t1 ... ${eN} tN"` with 1..3 differentially-safe holes.
    // The parser desugars it to a `toString`/`+` chain (or `format(e, "spec")` for a spec hole) and the
    // oracle mirrors it, so this drives the lexer interpolation mode-stack + the desugar + render_dump/
    // format end-to-end in the sweep. A String hole may itself be another interpolation (nested).
    std::string interp_string(int depth) {
        std::string s = "\"" + interp_text_chunk();
        const int holes = 1 + (int)pick(3);   // 1..3 holes
        for (int i = 0; i < holes; ++i) {
            s += "${";
            GType ht = random_interp_hole_type();
            s += gen_expr(ht, depth - 1);
            // A `${e:spec}` spec desugars to `format(e, ...)` -- a PRELUDE fn (unlike the base hole's
            // `toString`, a builtin), so only emit specs in prelude mode or the no-prelude sweep rejects it.
            if (prelude_mode && is_scalar(ht) && chance(45)) {
                std::string spec = random_spec(ht);
                if (!spec.empty()) { s += ":"; s += spec; }
            }
            s += "}";
            s += interp_text_chunk();
        }
        return s + "\"";
    }

    std::string gen_expr(const GType& t, int depth);

    // A struct / tuple literal, sub-parts generated at their own types (shares the depth budget).
    std::string struct_lit(int sid, int depth) {
        const StructDef& sd = structs[sid];
        // ~25% of the time use RECORD-UPDATE form `Name { f: e, ..base }`: override a random subset of fields,
        // copy the rest from a nested literal of the SAME type (always well-typed; exercises the copy path).
        if (depth > 0 && !sd.fields.empty() && pick(4) == 0) {
            std::string s = sd.name + " { ";
            for (size_t i = 0; i < sd.fields.size(); ++i)
                if (pick(2) == 0)                            // ~half overridden; the rest come from base
                    s += sd.fields[i].first + ": " + gen_expr(sd.fields[i].second, depth - 1) + ", ";
            s += ".." + struct_lit(sid, depth - 1);          // base last (a nested same-type literal)
            return s + " }";
        }
        // ~1/6: FIELD SHORTHAND `Name { f0, f1: e }`. Shorthand resolves a LOCAL named exactly like
        // the field (compile_struct_lit's local_reg(fi.name) path), so it needs a block wrapper that
        // binds one. Safe against capture: every generator binding is named v<N>/a<N> and only a
        // FIELD is ever f<N>, so these locals cannot shadow anything gen_expr might reference.
        // Worth generating because a shorthand FieldInit carries a NULL value node -- the shape that
        // crashed the inliner's body scan, and the one an escape analysis over `IdentExpr` misses.
        if (depth > 0 && !sd.fields.empty() && pick(6) == 0) {
            std::vector<char> shand(sd.fields.size(), 0);
            for (size_t i = 0; i < sd.fields.size(); ++i) shand[i] = pick(2) == 0;
            shand[pick(static_cast<uint32_t>(sd.fields.size()))] = 1;   // at least one, else it is not the shape
            std::string lets, lit = sd.name + " { ";
            for (size_t i = 0; i < sd.fields.size(); ++i) {
                if (i) lit += ", ";
                if (shand[i]) {
                    lets += "let " + sd.fields[i].first + " = " +
                            gen_expr(sd.fields[i].second, depth - 1) + "; ";
                    lit  += sd.fields[i].first;
                } else {
                    lit += sd.fields[i].first + ": " + gen_expr(sd.fields[i].second, depth - 1);
                }
            }
            return "({ " + lets + lit + " } })";
        }
        std::string s = sd.name + " { ";
        for (size_t i = 0; i < sd.fields.size(); ++i) {
            if (i) s += ", ";
            s += sd.fields[i].first + ": " + gen_expr(sd.fields[i].second, depth - 1);
        }
        return s + " }";
    }
    std::string tuple_lit(const GType& t, int depth) {
        std::string s = "(";
        for (size_t i = 0; i < t.elems.size(); ++i) {
            if (i) s += ", ";
            s += gen_expr(t.elems[i], depth - 1);
        }
        return s + ")";
    }
    // A variant value of enum `eid`. At depth<=0 a nullary variant is preferred (bottoms out),
    // else a random variant; payload sub-parts are generated at their own (scalar) types.
    std::string enum_lit(int eid, int depth) {
        const EnumDef& ed = enums[eid];
        std::vector<int> nullary, all;
        for (int i = 0; i < (int)ed.variants.size(); ++i) {
            all.push_back(i);
            if (ed.variants[i].fields.empty()) nullary.push_back(i);
        }
        const int vi = (depth <= 0 && !nullary.empty()) ? nullary[pick((uint32_t)nullary.size())]
                                                        : all[pick((uint32_t)all.size())];
        const EnumDef::Variant& v = ed.variants[vi];
        if (v.fields.empty()) return v.name;
        if (v.is_record) {   // brace construction `V { f0: v0, f1: v1 }`
            std::string s = v.name + " { ";
            for (size_t i = 0; i < v.fields.size(); ++i) {
                if (i) s += ", ";
                s += "f" + std::to_string(i) + ": " + gen_expr(v.fields[i], depth - 1);
            }
            return s + " }";
        }
        std::string s = v.name + "(";
        for (size_t i = 0; i < v.fields.size(); ++i) { if (i) s += ", "; s += gen_expr(v.fields[i], depth - 1); }
        return s + ")";
    }
    // A container value. Vec: a push-chain `push(push(vec(), e), e)` (>=1 element so the element type
    // infers without an annotation). Map: `#{ k => v, ... }` (>=1 entry) with LITERAL keys (a literal
    // avoids a duplicate-key-order question and keeps keys well-typed; values are full sub-expressions).
    std::string container_lit(const GType& t, int depth) {
        if (t.kind == GType::Vec) {
            const int n = 1 + (int)pick(3);
            std::string s = "vec()";
            for (int i = 0; i < n; ++i) s = "push(" + s + ", " + gen_expr(t.elems[0], depth - 1) + ")";
            return s;
        }
        const int n = 1 + (int)pick(3);
        std::string s = "#{ ";
        for (int i = 0; i < n; ++i) {
            if (i) s += ", ";
            s += scalar_lit(t.elems[0]) + " => " + gen_expr(t.elems[1], depth - 1);
        }
        return s + " }";
    }
    std::string ctor(const GType& t, int depth) {
        if (t.kind == GType::Newtype) return newtype_lit(t.nid, depth);
        if (t.kind == GType::Struct) return struct_lit(t.sid, depth);
        if (t.kind == GType::Enum)   return enum_lit(t.eid, depth);
        if (is_container(t))         return container_lit(t, depth);
        return tuple_lit(t, depth);
    }

    // A leaf value of type t: an in-scope var (preferred) or a minimal construction.
    std::string leaf(const GType& t) {
        const int v = var_of(t);
        if (v >= 0 && chance(55)) return env[v].name;
        if (is_scalar(t)) return scalar_lit(t);
        return ctor(t, 0);   // struct/tuple built from leaf sub-parts
    }

    // ---- declared-callable helpers ----

    // ONE CALL ARGUMENT. Normally an expression of the parameter's EXACT type -- but for a `Double`
    // parameter it is sometimes an `Int` expression instead. That is well-typed (`Int <: Double`, the
    // first of the language's two subtyping edges), and it is a shape this generator could never
    // produce before: every argument was built at the parameter's exact type, so an Int never reached
    // a Double parameter.
    //
    // That blind spot is why two 50 000-seed sweeps reported `0 compiler-bugs` while the compiler
    // silently computed wrong numbers: codegen inserted NO argument coercion, whereas the RefEval
    // oracle DOES coerce at argument binding (RefEval.cpp:1173 / :1212 / :1255). The two genuinely
    // disagreed -- the sweep simply never built a program that could show it. This production found
    // it in 2 of 2000 seeds, and the hole is now closed (`Expr::widen_double`).
    //
    // KEEP THIS PRODUCTION. It is the standing falsifier for that coercion, and the reason it exists
    // is the durable lesson: a green sweep measures the GENERATOR'S REACH, not the compiler's
    // correctness. Anything that narrows the shapes reachable here narrows the guard, silently.
    std::string arg_of(const GType& pt, int depth) {
        if (pt.kind == GType::Double && chance(35)) return gen_expr(scalar(GType::Int), depth);
        return gen_expr(pt, depth);
    }

    bool has_fn(const GType& t) { for (const auto& f : funcs) if (same(f.ret, t)) return true; return false; }
    std::string call_of(const GType& t, int depth) {
        std::vector<int> idx;
        for (int i = 0; i < (int)funcs.size(); ++i) if (same(funcs[i].ret, t)) idx.push_back(i);
        const FnSig& f = funcs[idx[pick((uint32_t)idx.size())]];
        std::string s = f.name + "(";
        for (size_t i = 0; i < f.params.size(); ++i) {
            if (i) s += ", ";
            // A `mut` first param requires a mut binding OR a temporary; a fresh rvalue ctor is always
            // exempt from the mut-at-call-site rule, so use one rather than a possibly-immutable binding.
            if (i == 0 && f.first_mut) s += ctor(f.params[i], depth - 1);
            else                       s += arg_of(f.params[i], depth - 1);
        }
        return s + ")";
    }
    bool has_hof(const GType& t) { for (const auto& h : hofs) if (same(h.b, t)) return true; return false; }
    // Apply a HOF returning t with an inline lambda + argument. The lambda body is generated with
    // the enclosing env in scope, so it may CAPTURE outer bindings -> MAKE_CLOSURE (the exact
    // "closure literal as a call argument" shape streams lean on).
    std::string call_hof(const GType& t, int depth) {
        std::vector<int> idx;
        for (int i = 0; i < (int)hofs.size(); ++i) if (same(hofs[i].b, t)) idx.push_back(i);
        const HofSig& h = hofs[idx[pick((uint32_t)idx.size())]];
        const std::string ap = "a" + std::to_string(name_counter++);
        env.push_back({ ap, h.a, false });
        // Sometimes an early `return` inside the LAMBDA body -- it leaves the lambda, not the caller.
        const std::string body = chance(15) ? early_return_body(h.b, depth - 1) : gen_expr(h.b, depth - 1);
        env.pop_back();
        const std::string arg = gen_expr(h.a, depth - 1);
        return h.name + "(fn(" + ap + ": " + ty_name(h.a) + ") -> " + ty_name(h.b) +
               " { " + body + " }, " + arg + ")";
    }

    // ---- scalar projections out of composites (only when t is scalar) ----
    // A struct field of type t: build a struct that HAS such a field, then read it.
    bool struct_has_field(const GType& t) {
        for (const auto& sd : structs) for (const auto& f : sd.fields) if (same(f.second, t)) return true;
        return false;
    }
    std::string struct_project(const GType& t, int depth) {
        std::vector<std::pair<int, std::string>> hits;
        for (int s = 0; s < (int)structs.size(); ++s)
            for (const auto& f : structs[s].fields) if (same(f.second, t)) hits.push_back({ s, f.first });
        const auto& h = hits[pick((uint32_t)hits.size())];
        return "(" + gen_expr(struct_ty(h.first), depth - 1) + ")." + h.second;
    }

    // ---- transparent newtypes (erased single-field tuple structs over a scalar) ----
    std::string newtype_lit(int nid, int depth) {   // `Name(<under-expr>)`
        const NewtypeDef& nd = newtypes[nid];
        return nd.name + "(" + gen_expr(nd.under, depth > 0 ? depth - 1 : 0) + ")";
    }
    bool has_newtype_under(const GType& t) {
        for (const auto& nd : newtypes) if (same(nd.under, t)) return true;
        return false;
    }
    // A scalar t projected out of a transparent newtype: build one carrying t, then read `.0`.
    std::string newtype_project(const GType& t, int depth) {
        std::vector<int> hits;
        for (int i = 0; i < (int)newtypes.size(); ++i) if (same(newtypes[i].under, t)) hits.push_back(i);
        const int nid = hits[pick((uint32_t)hits.size())];
        return "(" + gen_expr(newtype_ty(nid), depth - 1) + ").0";
    }
    // A tuple element of type t: synthesize a tuple carrying t, then read that slot.
    std::string tuple_project(const GType& t, int depth) {
        GType tt; tt.kind = GType::Tuple;
        const int pos = (int)pick(2);          // t lands at index 0 or 1
        for (int i = 0; i < 2; ++i) tt.elems.push_back(i == pos ? t : random_scalar());
        return "(" + gen_expr(tt, depth - 1) + ")." + std::to_string(pos);
    }
    // A tuple DESTRUCTURING match yielding t: `(match (a, b) { (n0, n1) => <expr(t) with n0,n1> })`.
    std::string tuple_match_project(const GType& t, int depth) {
        const GType e0 = random_scalar(), e1 = random_scalar();
        GType tt; tt.kind = GType::Tuple; tt.elems = { e0, e1 };
        const std::string scrut = gen_expr(tt, depth - 1);
        const std::string n0 = "a" + std::to_string(name_counter++);
        const std::string n1 = "a" + std::to_string(name_counter++);
        env.push_back({ n0, e0, false });
        env.push_back({ n1, e1, false });
        const std::string arm = gen_expr(t, depth - 1);
        env.pop_back(); env.pop_back();
        return "(match " + scrut + " { (" + n0 + ", " + n1 + ") => " + arm + " })";
    }
    // A trait-method call returning t, on a concrete receiver of the impl's struct type.
    // Randomly dispatches CONCRETELY (devirtualized direct call) or via the generic wrapper
    // `wN[X: TrN](x: X)` (erased param -> dynamic dispatch via the fused RESOLVE_CALL).
    bool trait_returns(const GType& t) { for (const auto& tr : traits) if (same(tr.ret, t)) return true; return false; }
    std::string trait_call(const GType& t, int depth) {
        std::vector<int> idx;
        for (int i = 0; i < (int)traits.size(); ++i) if (same(traits[i].ret, t)) idx.push_back(i);
        const TraitDef& tr = traits[idx[pick((uint32_t)idx.size())]];
        const std::string recv = gen_expr(struct_ty(tr.sid), depth - 1);
        // The generic wrapper `wN` only exists for the plain traits; supertrait / blanket entries
        // carry an empty wrapper -> always call the method directly there.
        const std::string callee = (!tr.wrapper.empty() && chance(50)) ? tr.wrapper : tr.method;
        // The METHOD itself (not the generic wrapper, which is a free fn -- `x.wN()` would be UFCS, a
        // non-goal) is also spelled the two ways a user writes it: method syntax `recv.m()` and the
        // trait-qualified `Tr::m(recv)`. For years the sweep produced neither, so every method-syntax
        // path (the receiver inference, the prelude-trait keep rule, the head writeback) was covered
        // only by hand tests -- and `o.unwrapOr(0)` / `Json::parse(s).unwrap()` broke in exactly
        // those paths. An associated-fn receiver `S::new(..).m()` is that second bug's shape.
        if (callee == tr.method) {
            switch (pick(10)) {
                case 0: case 1: case 2:
                    return "((" + recv + ")." + callee + "())";
                case 3: {
                    const std::string assoc = assoc_new_call(tr.sid, depth - 1);
                    if (!assoc.empty()) return "(" + assoc + "." + callee + "())";
                    return "((" + recv + ")." + callee + "())";
                }
                case 4:
                    return tr.name + "::" + callee + "(" + recv + ")";
                default: break;
            }
        }
        // Randomly use pipe form `(recv) |> callee` (parenthesize recv so a bare struct-literal LHS
        // can't be misparsed) -- exercises the tail-position trait-call pipe path.
        if (chance(40)) return "((" + recv + ") |> " + callee + ")";
        return callee + "(" + recv + ")";
    }

    // ---- inherent impls (gen_inherent) ----
    const InherentDef* inherent_of(int sid) const {
        for (const auto& ih : inherents) if (ih.sid == sid) return &ih;
        return nullptr;
    }
    // `S::new(a0, ..)` for struct `sid`, or "" when S has no associated `new`.
    std::string assoc_new_call(int sid, int depth) {
        const InherentDef* ih = inherent_of(sid);
        if (!ih || !ih->has_new) return std::string();
        const StructDef& sd = structs[sid];
        std::string s = sd.name + "::new(";
        for (size_t i = 0; i < sd.fields.size(); ++i) {
            if (i) s += ", ";
            s += arg_of(sd.fields[i].second, depth);
        }
        return s + ")";
    }
    bool inherent_returns(const GType& t) {
        for (const auto& ih : inherents)
            if ((!ih.get_m.empty() && same(ih.get_ret, t)) || (!ih.arg_m.empty() && same(ih.arg_ret, t))) return true;
        return false;
    }
    // An inherent-method call returning t, in one of the three spellings: `recv.m(..)`, `S::m(recv, ..)`,
    // `(recv) |> S::m(..)`. The receiver is sometimes the associated-fn call itself, unparenthesized --
    // `S::new(..).m()`, a chain of an associated fn and a method.
    std::string inherent_call(const GType& t, int depth) {
        struct Hit { const InherentDef* ih; bool with_arg; };
        std::vector<Hit> hits;
        for (const auto& ih : inherents) {
            if (!ih.get_m.empty() && same(ih.get_ret, t)) hits.push_back({ &ih, false });
            if (!ih.arg_m.empty() && same(ih.arg_ret, t)) hits.push_back({ &ih, true });
        }
        const Hit h = hits[pick((uint32_t)hits.size())];
        const int sid = h.ih->sid;
        const std::string sn = structs[sid].name;
        const std::string m  = h.with_arg ? h.ih->arg_m : h.ih->get_m;
        const std::string arg = h.with_arg ? arg_of(h.ih->arg_ty, depth - 1) : std::string();
        std::string assoc = chance(30) ? assoc_new_call(sid, depth - 1) : std::string();
        switch (pick(3)) {
            case 0: {
                const std::string recv = assoc.empty() ? "(" + gen_expr(struct_ty(sid), depth - 1) + ")" : assoc;
                return "(" + recv + "." + m + "(" + arg + "))";
            }
            case 1: {
                const std::string recv = assoc.empty() ? gen_expr(struct_ty(sid), depth - 1) : assoc;
                return sn + "::" + m + "(" + recv + (h.with_arg ? ", " + arg : std::string()) + ")";
            }
            default: {
                const std::string recv = assoc.empty() ? gen_expr(struct_ty(sid), depth - 1) : assoc;
                return "((" + recv + ") |> " + sn + "::" + m + (h.with_arg ? "(" + arg + ")" : std::string()) + ")";
            }
        }
    }
    // A pipe into a user function whose return type is t: `(<arg0>) |> f` (arity 1) or
    // `(<arg0>) |> f(<arg1..>)` (arity >1) -- semantically f(arg0, ...), routed through `|>`.
    bool has_pipeable(const GType& t) {
        for (const auto& f : funcs) if (same(f.ret, t) && f.params.size() >= 1) return true;
        return false;
    }
    std::string gen_pipe(const GType& t, int depth) {
        std::vector<int> idx;
        for (int i = 0; i < (int)funcs.size(); ++i)
            if (same(funcs[i].ret, t) && funcs[i].params.size() >= 1) idx.push_back(i);
        const FnSig& f = funcs[idx[pick((uint32_t)idx.size())]];
        // A `mut` first param (the piped LHS) needs a mut binding OR a temporary; a fresh rvalue ctor
        // is always exempt from the mut-at-call-site rule (mirrors call_of).
        const std::string arg0 = f.first_mut ? ctor(f.params[0], depth - 1) : gen_expr(f.params[0], depth - 1);
        std::string s = "((" + arg0 + ") |> " + f.name;
        if (f.params.size() > 1) {
            s += "(";
            for (size_t i = 1; i < f.params.size(); ++i) { if (i > 1) s += ", "; s += arg_of(f.params[i], depth - 1); }
            s += ")";
        }
        return s + ")";
    }

    // An EXHAUSTIVE match on an enum value yielding scalar t: one arm per variant, each binding
    // its payload fields (a Ctor pattern `Vk(a0, a1)` / bare `Vk`) and producing t in that scope.
    // No wildcard arm -- full variant coverage keeps the checker's usefulness/exhaustiveness happy.
    bool has_enum() { return !enums.empty(); }
    std::string gen_enum_match(const GType& t, int depth) {
        const int eid = (int)pick((uint32_t)enums.size());
        const EnumDef& ed = enums[eid];
        // Parenthesize the scrutinee: a bare struct-literal / record scrutinee would collide with
        // the match-arm `{` (the Rust-style no-struct-literal-in-scrutinee restriction).
        std::string s = "(match (" + gen_expr(enum_ty(eid), depth - 1) + ") { ";
        for (size_t vi = 0; vi < ed.variants.size(); ++vi) {
            const EnumDef::Variant& v = ed.variants[vi];
            if (vi) s += ", ";
            std::vector<std::string> binds;
            if (v.fields.empty()) s += v.name;
            else if (v.is_record) {   // record pattern `V { f0: a0, f1: a1 }`
                s += v.name + " { ";
                for (size_t i = 0; i < v.fields.size(); ++i) {
                    if (i) s += ", ";
                    const std::string nm = "a" + std::to_string(name_counter++);
                    binds.push_back(nm);
                    s += "f" + std::to_string(i) + ": " + nm;
                }
                s += " }";
            }
            else {
                s += v.name + "(";
                for (size_t i = 0; i < v.fields.size(); ++i) {
                    if (i) s += ", ";
                    const std::string nm = "a" + std::to_string(name_counter++);
                    binds.push_back(nm);
                    s += nm;
                }
                s += ")";
            }
            for (size_t i = 0; i < v.fields.size(); ++i) env.push_back({ binds[i], v.fields[i], false });
            s += " => " + gen_expr(t, depth - 1);
            for (size_t i = 0; i < v.fields.size(); ++i) env.pop_back();
        }
        return s + " })";
    }

    // A single-arm irrefutable match on a struct value yielding scalar t: `match s { S { f0: a0,
    // f1: a1 } => <t with a0,a1> }`. One constructor => exhaustive; the `f: name` sub-pattern form
    // exercises the field-pattern binder (distinct from the shorthand path).
    bool has_struct() { return !structs.empty(); }
    std::string gen_struct_match(const GType& t, int depth) {
        const int sid = (int)pick((uint32_t)structs.size());
        const StructDef& sd = structs[sid];
        std::string s = "(match (" + gen_expr(struct_ty(sid), depth - 1) + ") { " + sd.name + " { ";
        std::vector<std::pair<std::string, GType>> binds;
        for (size_t i = 0; i < sd.fields.size(); ++i) {
            if (i) s += ", ";
            const std::string nm = "a" + std::to_string(name_counter++);
            s += sd.fields[i].first + ": " + nm;
            binds.push_back({ nm, sd.fields[i].second });
        }
        s += " } => ";
        for (const auto& b : binds) env.push_back({ b.first, b.second, false });
        s += gen_expr(t, depth - 1);
        for (size_t i = 0; i < binds.size(); ++i) env.pop_back();
        return s + " })";
    }
    // A GUARDED match yielding scalar t: `match <int> { g if <bool(g)> => <t>, _ => <t> }`. The
    // guarded ident arm is non-exhaustive, so the trailing `_` is mandatory (Maranget).
    std::string gen_guard_match(const GType& t, int depth) {
        const std::string g = "a" + std::to_string(name_counter++);
        const std::string scrut = gen_expr(scalar(GType::Int), depth - 1);
        env.push_back({ g, scalar(GType::Int), false });
        const std::string guard = gen_expr(scalar(GType::Bool), depth - 1);   // may read g
        const std::string arm1  = gen_expr(t, depth - 1);
        env.pop_back();
        const std::string arm2  = gen_expr(t, depth - 1);
        return "(match " + scrut + " { " + g + " if " + guard + " => " + arm1 + ", _ => " + arm2 + " })";
    }
    // A RANGE-pattern match yielding scalar t: `match <int> { <lo>..<hi> => <t>, _ => <t> }`. A range
    // never completes a signature (Int is infinite), so the trailing `_` is mandatory -- like the guard
    // match. lo <= hi, both possibly negative, exercising BLT_NUM/BGT_NUM + the oracle's range path.
    // The fixed const table the range productions draw NAMED bounds from. Value and spelling move in
    // lockstep -- `RCI2..RCI5` and `-1..7` denote exactly the same set -- which is precisely what the
    // sweep must be unable to tell apart: a bound resolved to the wrong value, or a con id that
    // collapses two distinct ranges, changes WHICH ARM runs and therefore the compared value. A
    // scalar const is inlined at its use site and there is no unused-const warning, so emitting the
    // whole table into every program costs nothing.
    //
    // MEASURED LIMIT, and it is structural: the sweep can NOT catch a bound that resolves to the
    // WRONG constant. Deliberately resolving `RCI3` to `RCI4` left 2000 seeds in both arms fully
    // green, because the checker writes its resolution back into the AST and codegen AND the oracle
    // both read that same node -- so the two agree on the wrong value. What this arm does buy is
    // SHAPE breadth (named bounds, `..<` and Double bounds, none of which the generator produced
    // before) inside arbitrary programs: it would catch a crash, a frame under-measure, or a valid
    // program the checker rejects. The VALUE of a named bound is locked by the corpus instead --
    // the `rcb_*` tests in main.cpp compare results, and the same deliberate mis-resolution turns
    // nine of them red. Do not read the sweep's green as covering that class.
    static constexpr long        RC_INT[]     = { -50, -20, -1, 0, 1, 7, 20, 50 };
    static constexpr const char* RC_DBL_TXT[] = { "-20.0", "-1.5", "0.0", "1.0", "7.25", "20.0" };
    static constexpr uint32_t    RC_INT_N = 8, RC_DBL_N = 6;

    std::string gen_range_consts() {
        std::string s;
        for (uint32_t i = 0; i < RC_INT_N; ++i)
            s += "const RCI" + std::to_string(i) + ": Int = " + std::to_string(RC_INT[i]) + "\n";
        for (uint32_t i = 0; i < RC_DBL_N; ++i)
            s += "const RCD" + std::to_string(i) + ": Double = " + RC_DBL_TXT[i] + "\n";
        return s;
    }
    // Spell an Int bound: a plain literal, or -- half the time -- a const NAME, whose value REPLACES
    // `v` so the caller's ordering still describes the range it built. A named `hi` may land below
    // `lo`; an empty range is legal and worth exercising (the `_` arm then always wins).
    std::string int_bound(long& v) {
        if (!chance(50)) return std::to_string(v);
        const uint32_t i = pick(RC_INT_N);
        v = RC_INT[i];
        return "RCI" + std::to_string(i);
    }
    std::string dbl_bound() {
        const uint32_t i = pick(RC_DBL_N);
        return chance(50) ? ("RCD" + std::to_string(i)) : std::string(RC_DBL_TXT[i]);
    }

    std::string gen_range_match(const GType& t, int depth) {
        const bool dbl  = chance(30);                     // Double bounds: never generated before
        const std::string sep = chance(35) ? "..<" : "..";   // nor was the exclusive spelling
        std::string scrut, lo_s, hi_s;
        if (dbl) {
            scrut = gen_expr(scalar(GType::Double), depth - 1);
            lo_s = dbl_bound(); hi_s = dbl_bound();
        } else {
            scrut = gen_expr(scalar(GType::Int), depth - 1);
            long lo = (long)pick(101) - 50;               // [-50, 50]
            lo_s = int_bound(lo);
            long hi = lo + (long)pick(21);                // [lo, lo+20]
            hi_s = int_bound(hi);
        }
        const std::string arm1 = gen_expr(t, depth - 1);
        const std::string arm2 = gen_expr(t, depth - 1);
        return "(match " + scrut + " { " + lo_s + sep + hi_s + " => " + arm1 + ", _ => " + arm2 + " })";
    }
    // An `@`-BINDING over a range: `match <int> { a0 @ <lo>..<hi> => <t(a0)>, _ => <t> }`. Like the
    // range match, the trailing `_` is mandatory -- the binding is invisible to exhaustiveness. The
    // binder joins the `a<N>` namespace (match-arm binders), so it cannot shadow a generated fn.
    // The arm body is generated with `a0` in scope AT THE SCRUTINEE'S TYPE, which is the property
    // that makes an `@` well-typed by construction: the name takes the type of its POSITION.
    std::string gen_at_match(const GType& t, int depth) {
        const std::string scrut = gen_expr(scalar(GType::Int), depth - 1);
        long lo = (long)pick(101) - 50;
        const std::string lo_s = int_bound(lo);           // a named bound behind an `@` too
        long hi = lo + (long)pick(21);
        const std::string hi_s = int_bound(hi);
        const std::string sep = chance(35) ? "..<" : "..";
        const std::string n = "a" + std::to_string(name_counter++);
        env.push_back({ n, scalar(GType::Int), false });
        const std::string arm1 = gen_expr(t, depth - 1);
        env.pop_back();
        const std::string arm2 = gen_expr(t, depth - 1);
        return "(match " + scrut + " { " + n + " @ " + lo_s + sep + hi_s +
               " => " + arm1 + ", _ => " + arm2 + " })";
    }
    // A NESTED pattern: a struct pattern inside a tuple pattern -- `match (s, x) { (S { f0: a0 }, b)
    // => <t> }`. Single irrefutable arm; exercises compile_field_pattern's recursive (non-ident) path.
    std::string gen_nested_match(const GType& t, int depth) {
        const int sid = (int)pick((uint32_t)structs.size());
        const StructDef& sd = structs[sid];
        const GType st = random_scalar();
        GType tt; tt.kind = GType::Tuple; tt.elems = { struct_ty(sid), st };
        std::string s = "(match " + gen_expr(tt, depth - 1) + " { (" + sd.name + " { ";
        std::vector<std::pair<std::string, GType>> binds;
        for (size_t i = 0; i < sd.fields.size(); ++i) {
            if (i) s += ", ";
            const std::string nm = "a" + std::to_string(name_counter++);
            s += sd.fields[i].first + ": " + nm;
            binds.push_back({ nm, sd.fields[i].second });
        }
        const std::string bn = "a" + std::to_string(name_counter++);
        s += " }, " + bn + ") => ";
        for (const auto& b : binds) env.push_back({ b.first, b.second, false });
        env.push_back({ bn, st, false });
        s += gen_expr(t, depth - 1);
        env.pop_back();
        for (size_t i = 0; i < binds.size(); ++i) env.pop_back();
        return s + " })";
    }
    // A BINDING or-pattern: `match ptry_ok(<int>) { Ok(a0) | Err(a0) => a0 + (<int>) }`.
    //
    // `Result[Int, Int]` gives both alternatives the same payload type by construction, which is
    // what the same-binder-set rule demands -- generated enums draw each field type independently,
    // so a matching pair there is only luck. Exhaustive without a wildcard (Ok|Err covers Result).
    //
    // Two things are deliberate, and BOTH were established by re-breaking the lowering and watching
    // the sweep stay silent:
    //   * It is its OWN production with its own `chance`, not a case inside gen_prelude_int. That
    //     function is reached roughly once per 130 programs, so a case inside it is drawn far too
    //     rarely to lock anything -- 2000 seeds found nothing against a deliberately broken MOV block.
    //   * The arm body READS the binding unconditionally (`a0 + (...)`). Merely pushing it into
    //     `env` is not enough: gen_expr references an in-scope name only by chance, and a run that
    //     never reads it agrees no matter which register the name points at. This is the same trap
    //     the gen_newtype_match comment warns about -- written there, then not enforced here.
    // `ptry_ok` matches the FIRST alternative and `ptry_err` the last, so both the jump-to-`matched`
    // path and the fall-through path are exercised.
    std::string gen_or_bind_match(int depth) {
        const std::string n   = "a" + std::to_string(name_counter++);
        const std::string src = chance(50) ? "ptry_ok(" : "ptry_err(";
        const std::string scrut = src + gen_expr(scalar(GType::Int), depth - 1) + ")";
        env.push_back({ n, scalar(GType::Int), false });
        const std::string body = gen_expr(scalar(GType::Int), depth - 1);
        env.pop_back();
        return "(match " + scrut + " { Ok(" + n + ") | Err(" + n + ") => " + n + " + (" + body + ") })";
    }
    // A transparent-newtype PATTERN nested in a tuple pattern: `match (N(e), x) { (N(a0), b) => <t> }`.
    //
    // The nesting is the entire point. compile_field_pattern fetches the tuple element into a TEMP and
    // frees it the instant the sub-test returns, while the erased newtype forwards that same register to
    // its element -- so binding the element by ALIAS reads a register the arm body immediately reclaims.
    // That was a live silent-wrong-value bug which 100 000 generated
    // programs never reached, because newtypes were generated only as VALUES (`N(e)`, `(N(e)).0`) and
    // there was no newtype-PATTERN production at all. Proof of the blindness: the 50k dual sweep returned
    // character-identical tallies across the fix.
    //
    // The arm body is generated with the binding IN SCOPE -- a production that binds a name and never
    // reads it would reproduce exactly the blindness it is here to remove.
    std::string gen_newtype_match(const GType& t, int depth) {
        const int nid = (int)pick((uint32_t)newtypes.size());
        const NewtypeDef& nd = newtypes[nid];
        const GType st = random_scalar();
        GType tt; tt.kind = GType::Tuple; tt.elems = { newtype_ty(nid), st };
        const std::string scrut = gen_expr(tt, depth - 1);
        const std::string n0 = "a" + std::to_string(name_counter++);   // the erased element
        const std::string n1 = "a" + std::to_string(name_counter++);   // the plain tuple element
        env.push_back({ n0, nd.under, false });
        env.push_back({ n1, st, false });
        const std::string arm = gen_expr(t, depth - 1);
        env.pop_back(); env.pop_back();
        return "(match " + scrut + " { (" + nd.name + "(" + n0 + "), " + n1 + ") => " + arm + " })";
    }
    // An irrefutable destructuring-`let` STATEMENT: `let (a, b) = <tuple2>` or `let S { f0: a } =
    // <struct>`. The bound scalars persist in scope (like the other program-level lets).
    std::string gen_destructure_let() {
        if (chance(50) && has_struct()) {
            const int sid = (int)pick((uint32_t)structs.size());
            const StructDef& sd = structs[sid];
            std::string s = "let " + sd.name + " { ";
            std::vector<std::pair<std::string, GType>> binds;
            for (size_t i = 0; i < sd.fields.size(); ++i) {
                if (i) s += ", ";
                const std::string nm = fresh_name();
                s += sd.fields[i].first + ": " + nm;
                binds.push_back({ nm, sd.fields[i].second });
            }
            s += " } = " + gen_expr(struct_ty(sid), 3) + "\n";
            for (const auto& b : binds) env.push_back({ b.first, b.second, false });
            return s;
        }
        const GType e0 = random_scalar(), e1 = random_scalar();
        GType tt; tt.kind = GType::Tuple; tt.elems = { e0, e1 };
        const std::string n0 = fresh_name(), n1 = fresh_name();
        std::string s = "let (" + n0 + ", " + n1 + ") = " + gen_expr(tt, 3) + "\n";
        env.push_back({ n0, e0, false });
        env.push_back({ n1, e1, false });
        return s;
    }

    // A boolean comparison. Usually two same-typed SCALAR operands (Int/Double/String), but ~1/3 of the
    // time -- if two DISTINCT in-scope bindings share a COMPOSITE type -- a structural `==`/`!=` between
    // them, exercising the EQ_DEEP opcode against the oracle over structs / tuples / enums / Vec / Map.
    // Every generator GType is Eq (there is no function type), so any same-typed pair is comparable.
    std::string gen_compare(int depth) {
        static const char* rel[] = { "==", "!=", "<", "<=", ">", ">=" };
        if (pick(3) == 0) {
            std::vector<std::pair<std::string, std::string>> pairs;
            for (size_t i = 0; i < env.size(); ++i)
                for (size_t j = i + 1; j < env.size(); ++j)
                    if (!is_scalar(env[i].ty) && same(env[i].ty, env[j].ty))
                        pairs.push_back({ env[i].name, env[j].name });
            if (!pairs.empty()) {
                const auto& p = pairs[pick(static_cast<uint32_t>(pairs.size()))];
                const char* eop = pick(2) == 0 ? "==" : "!=";
                return "(" + p.first + " " + eop + " " + p.second + ")";
            }
        }
        const char* op = rel[pick(6)];
        GType ct = pick(3) == 0 ? scalar(GType::Int) : (pick(2) == 0 ? scalar(GType::Double) : scalar(GType::String));
        return "(" + gen_expr(ct, depth - 1) + " " + op + " " + gen_expr(ct, depth - 1) + ")";
    }
    std::string gen_if(const GType& t, int depth) {
        return "(if " + gen_expr(scalar(GType::Bool), depth - 1) +
               " { " + gen_expr(t, depth - 1) + " } else { " + gen_expr(t, depth - 1) + " })";
    }
    // ---- control-flow sugar and early exits (Slice 4) ----

    // The pattern for enum variant `v`, binding each payload field to a fresh `a<N>` appended to `binds`.
    std::string variant_pattern(const EnumDef::Variant& v, std::vector<std::string>& binds) {
        if (v.fields.empty()) return v.name;
        std::string s = v.name + (v.is_record ? " { " : "(");
        for (size_t i = 0; i < v.fields.size(); ++i) {
            if (i) s += ", ";
            const std::string nm = "a" + std::to_string(name_counter++);
            binds.push_back(nm);
            s += v.is_record ? "f" + std::to_string(i) + ": " + nm : nm;
        }
        return s + (v.is_record ? " }" : ")");
    }
    // An `Option[Int]`-typed expression (prelude mode): literal Some, a total `get`, a `pop` of a temp,
    // or a call of a generated `?`-using fn. Several of them are None for some inputs.
    std::string gen_opt_int(int depth) {
        auto g = [&]() { return gen_expr(scalar(GType::Int), depth - 1); };
        std::vector<int> qo;
        for (int i = 0; i < (int)try_fns.size(); ++i) if (!try_fns[i].is_result) qo.push_back(i);
        switch (pick(qo.empty() ? 4 : 5)) {
            case 0:  return "Some(" + g() + ")";
            case 1:  return "get(" + gen_int_vec(depth) + ", " + g() + ")";
            case 2:  return "pop(" + gen_int_vec(depth) + ")";
            // A first-class Vec[Int] expression (a var, a field projection, ..). NOT `get(vec(), i)`: with
            // no consumer pinning the element (`if let Some(a) = ..`, `let a = ..?`) its `T` stays unsolved.
            case 3:  return "get(" + gen_expr(vec_ty(scalar(GType::Int)), depth - 1) + ", " + g() + ")";
            default: return try_fn_call(qo[pick((uint32_t)qo.size())], depth);
        }
    }
    // A `Result[Int, Int]`-typed expression (prelude mode).
    std::string gen_res_int(int depth) {
        auto g = [&]() { return gen_expr(scalar(GType::Int), depth - 1); };
        std::vector<int> qr;
        for (int i = 0; i < (int)try_fns.size(); ++i) if (try_fns[i].is_result) qr.push_back(i);
        switch (pick(qr.empty() ? 3 : 4)) {
            case 0:  return "pstep(" + g() + ")";
            case 1:  return "ptry_err(" + g() + ")";
            case 2:  return "okOr(get(" + gen_int_vec(depth) + ", " + g() + "), " + g() + ")";
            default: return try_fn_call(qr[pick((uint32_t)qr.size())], depth);
        }
    }
    // `if let PAT = (e) { t } else { t }`, over an enum variant (any mode) or Some/Ok/Err (prelude mode).
    // The else branch is sometimes an `else if let` chain. The parser desugars both to a match, so this
    // reaches the same lowering as a match -- but by the parser route a user actually types.
    std::string gen_if_let(const GType& t, int depth) {
        std::string head;
        std::vector<std::pair<std::string, GType>> binds;
        const bool opt = prelude_mode && (enums.empty() || chance(50));
        if (opt) {
            const std::string a = "a" + std::to_string(name_counter++);
            switch (pick(3)) {
                case 0:  head = "Some(" + a + ") = (" + gen_opt_int(depth) + ")"; break;
                case 1:  head = "Ok(" + a + ") = (" + gen_res_int(depth) + ")"; break;
                default: head = "Err(" + a + ") = (" + gen_res_int(depth) + ")"; break;
            }
            binds.push_back({ a, scalar(GType::Int) });
        } else {
            if (enums.empty()) return gen_if(t, depth);
            const int eid = (int)pick((uint32_t)enums.size());
            const EnumDef::Variant& v = enums[eid].variants[pick((uint32_t)enums[eid].variants.size())];
            std::vector<std::string> names;
            head = variant_pattern(v, names) + " = (" + gen_expr(enum_ty(eid), depth - 1) + ")";
            for (size_t i = 0; i < names.size(); ++i) binds.push_back({ names[i], v.fields[i] });
        }
        for (const auto& b : binds) env.push_back({ b.first, b.second, false });
        const std::string then_e = gen_expr(t, depth - 1);
        for (size_t i = 0; i < binds.size(); ++i) env.pop_back();
        const std::string else_e = chance(25) ? gen_if_let(t, depth - 1) : "{ " + gen_expr(t, depth - 1) + " }";
        // `else if let` must not be wrapped in braces: strip the production's outer parens instead.
        const bool chain = else_e.rfind("(if ", 0) == 0;
        return "(if let " + head + " { " + then_e + " } else " +
               (chain ? else_e.substr(1, else_e.size() - 2) : else_e) + ")";
    }
    // `({ let mut cN: Int = 0; loop { cN = cN + 1; [if (cN < K && b) { continue };] if (cN >= K || b) { break <t> } } })`
    // -- `loop` as an EXPRESSION whose value comes from `break <value>`. It leaves by iteration K at the
    // latest: the counter is advanced first AND the `continue` guard is bounded by the counter too -- an
    // unbounded `if (true) { continue }` skips the break check forever (the first version of this
    // production did exactly that and hung the sweep).
    std::string gen_loop_break(const GType& t, int depth) {
        const std::string c = fresh_name();
        const int k = 1 + (int)pick(5);
        env.push_back({ c, scalar(GType::Int), false });   // readable in the guards / break value, never assigned by them
        std::string body = c + " = " + c + " + 1; ";
        if (chance(30)) body += "if (" + c + " < " + std::to_string(k) + " && " +
                                gen_expr(scalar(GType::Bool), depth - 1) + ") { continue }; ";
        const std::string guard = chance(50) ? " || " + gen_expr(scalar(GType::Bool), depth - 1) : std::string();
        body += "if (" + c + " >= " + std::to_string(k) + guard + ") { break " + gen_expr(t, depth - 1) + " }";
        env.pop_back();
        return "({ let mut " + c + ": Int = 0; loop { " + body + " } })";
    }
    // A fn/lambda body with an EARLY `return`: `if (b) { return <ret> }; <ret>`, or a counting `loop` that
    // only leaves through `return` (its type is Never, so the body's value is the return). `sep` is the
    // statement separator the context needs.
    std::string early_return_body(const GType& ret, int depth) {
        if (chance(35)) {
            const std::string c = fresh_name();
            const int k = 1 + (int)pick(4);
            env.push_back({ c, scalar(GType::Int), false });
            const std::string v = gen_expr(ret, depth - 1);
            env.pop_back();
            return "let mut " + c + ": Int = 0; loop { " + c + " = " + c + " + 1; if (" + c + " >= " +
                   std::to_string(k) + ") { return " + v + " } }";
        }
        return "if (" + gen_expr(scalar(GType::Bool), depth - 1) + ") { return " + gen_expr(ret, depth - 1) +
               " }; " + gen_expr(ret, depth - 1);
    }
    // `while let Some(x) = pop(v) { .. }` draining a fresh mut Vec[Int] into the accumulator (prelude).
    std::string gen_while_let() {
        const std::string acc = mut_var_of(scalar(GType::Int));
        const std::string v = fresh_name(), x = fresh_name();
        std::string s = "let mut " + v + " = " + gen_int_vec(2) + "\n";
        env.push_back({ x, scalar(GType::Int), false });
        const std::string pre = break_continue_prefix(2);   // pop advances in the condition -> continue-safe
        const std::string body = acc.empty() ? gen_expr(scalar(GType::Int), 2)
                                             : acc + " = " + acc + " + " + gen_expr(scalar(GType::Int), 2);
        env.pop_back();
        return s + "while let Some(" + x + ") = pop(" + v + ") { " + pre + body + " }\n";
    }

    // ---- `?` inside generated functions (prelude mode) ----
    // `fn qoN(p0: Int, ..) -> Option[Int]` / `fn qrN(..) -> Result[Int, Int]`: one or two `let a = e?`
    // steps (each may short-circuit), an optional early `return None` / `return Err(..)`, and sometimes
    // a `?` INSIDE an expression (`Some(e? + a)`). Each may call the EARLIER ones.
    struct TryFn { std::string name; int arity; bool is_result; };
    std::vector<TryFn> try_fns;
    std::string try_fn_call(int i, int depth) {
        const TryFn& f = try_fns[i];
        std::string s = f.name + "(";
        for (int p = 0; p < f.arity; ++p) { if (p) s += ", "; s += gen_expr(scalar(GType::Int), depth - 1); }
        return s + ")";
    }
    std::string gen_try_fns() {
        if (!prelude_mode) return std::string();
        std::string src;
        const int n = (int)pick(4);   // 0..3
        for (int i = 0; i < n; ++i) {
            const bool res = chance(50);
            TryFn f{ (res ? "qr" : "qo") + std::to_string(i), (int)pick(3), res };
            std::vector<Binding> saved = env;
            env.clear();
            std::string sig;
            for (int p = 0; p < f.arity; ++p) {
                if (p) sig += ", ";
                sig += "p" + std::to_string(p) + ": Int";
                env.push_back({ "p" + std::to_string(p), scalar(GType::Int), false });
            }
            auto src_e = [&]() { return res ? gen_res_int(2) : gen_opt_int(2); };
            std::string body;
            if (chance(30)) body += "if (" + gen_expr(scalar(GType::Bool), 2) + ") { return " +
                                    (res ? "Err(" + gen_expr(scalar(GType::Int), 2) + ")" : std::string("None")) + " }\n";
            const int steps = 1 + (int)pick(2);
            for (int k = 0; k < steps; ++k) {
                const std::string a = "a" + std::to_string(name_counter++);
                body += "let " + a + " = " + src_e() + "?\n";
                env.push_back({ a, scalar(GType::Int), false });
            }
            std::string val = gen_expr(scalar(GType::Int), 2);
            if (chance(35)) val = "(" + src_e() + ")? + " + val;
            body += std::string(res ? "Ok(" : "Some(") + val + ")";
            env = saved;
            src += "fn " + f.name + "(" + sig + ") -> " + (res ? "Result[Int, Int]" : "Option[Int]") +
                   " {\n" + body + "\n}\n";
            try_fns.push_back(f);
        }
        return src;
    }

    // ---- lazy iterator pipelines (prelude mode, Slice 5) ----
    // Every pipeline is FINITE and SHORT by construction: a source yields at most ~9 elements (a small
    // `range`, a Vec expression, or an unbounded `iterate` that is always cut by `take`), because the
    // oracle runs each stage's prelude cursor code tree-walking.

    // An `a<N>: Int` lambda `fn(aN: Int) -> R { body }`.
    std::string int_lambda(const GType& ret, int depth) {
        const std::string x = "a" + std::to_string(name_counter++);
        env.push_back({ x, scalar(GType::Int), false });
        const std::string body = gen_expr(ret, depth);
        env.pop_back();
        return "fn(" + x + ": Int) -> " + ty_name(ret) + " { " + body + " }";
    }
    std::string small_int() { return std::to_string((int64_t)pick(7) - 3); }
    // A `dyn Iterator[Int]` source.
    std::string pipe_source(int depth) {
        switch (pick(4)) {
            case 0: { const int lo = (int)pick(7) - 3;
                      return "range(" + std::to_string(lo) + ", " + std::to_string(lo + (int)pick(9)) + ")"; }
            case 1:  return "intoIter(" + gen_expr(vec_ty(scalar(GType::Int)), depth - 1) + ")";
            case 2:  return "(" + gen_int_vec(depth) + " |> intoIter)";
            default: return "take(iterate(" + small_int() + ", " + int_lambda(scalar(GType::Int), depth - 1) +
                            "), " + std::to_string(pick(6)) + ")";
        }
    }
    // One `dyn Iterator[Int] -> dyn Iterator[Int]` stage, as (verb, extra-args) so it can be spelled
    // prefix `verb(src, args)` or piped `src |> verb(args)`.
    std::pair<std::string, std::string> pipe_stage(int depth) {
        switch (pick(9)) {
            case 0: case 1: return { "map",       int_lambda(scalar(GType::Int), depth - 1) };
            case 2: case 3: return { "filter",    int_lambda(scalar(GType::Bool), depth - 1) };
            case 4:         return { "take",      std::to_string(pick(6)) };
            case 5:         return { "drop",      std::to_string(pick(4)) };
            case 6:         return { "stepBy",    std::to_string(1 + pick(3)) };
            case 7:         return { chance(50) ? "takeWhile" : "dropWhile", int_lambda(scalar(GType::Bool), depth - 1) };
            default: {
                const std::string acc = "a" + std::to_string(name_counter++), x = "a" + std::to_string(name_counter++);
                env.push_back({ acc, scalar(GType::Int), false });
                env.push_back({ x, scalar(GType::Int), false });
                const std::string body = gen_expr(scalar(GType::Int), depth - 1);
                env.pop_back(); env.pop_back();
                return { "scan", small_int() + ", fn(" + acc + ": Int, " + x + ": Int) -> Int { " + body + " }" };
            }
        }
    }
    // src + 0..3 stages (0..`max_stages`), spelled all-pipe, all-prefix, or mixed per stage.
    std::string pipe_stream(int depth, int max_stages = 3) {
        std::string s = pipe_source(depth);
        const int n = (int)pick((uint32_t)max_stages + 1);
        for (int i = 0; i < n; ++i) {
            const auto st = pipe_stage(depth);
            s = chance(55) ? "(" + s + " |> " + st.first + "(" + st.second + "))"
                           : st.first + "(" + s + ", " + st.second + ")";
        }
        return s;
    }
    // An Int produced by a terminal over a pipeline.
    std::string gen_pipeline_int(int depth) {
        const std::string s = pipe_stream(depth);
        auto g = [&]() { return gen_expr(scalar(GType::Int), depth - 1); };
        switch (pick(8)) {
            case 0:  return "(" + s + " |> sum)";
            case 1:  return "count(" + s + ")";
            case 2: {
                const std::string acc = "a" + std::to_string(name_counter++), x = "a" + std::to_string(name_counter++);
                env.push_back({ acc, scalar(GType::Int), false });
                env.push_back({ x, scalar(GType::Int), false });
                const std::string body = gen_expr(scalar(GType::Int), depth - 1);
                env.pop_back(); env.pop_back();
                return "fold(" + s + ", " + g() + ", fn(" + acc + ": Int, " + x + ": Int) -> Int { " + body + " })";
            }
            case 3:  return "len(" + s + " |> collect)";
            case 4:  return "(" + s + " |> max).unwrapOr(" + g() + ")";
            case 5:  return "unwrapOr(find(" + s + ", " + int_lambda(scalar(GType::Bool), depth - 1) + "), " + g() + ")";
            case 6:  return "(" + s + " |> enumerate |> map(fn(p: (Int, Int)) -> Int { p.0 * 3 + p.1 }) |> sum)";
            default: return "unwrapOr(reduce(" + s + ", fn(l: Int, r: Int) -> Int { l - r }), " + g() + ")";
        }
    }
    std::string gen_pipeline_bool(int depth) {
        const std::string s = pipe_stream(depth);
        switch (pick(3)) {
            case 0:  return "any(" + s + ", " + int_lambda(scalar(GType::Bool), depth - 1) + ")";
            case 1:  return "(" + s + " |> all(" + int_lambda(scalar(GType::Bool), depth - 1) + "))";
            default: return "contains(" + s + ", " + small_int() + ")";
        }
    }
    // `for x in <pipeline> { acc = acc + <expr(x)> }` -- the lazy `for` over a `dyn Iterator`.
    std::string gen_for_pipeline() {
        const std::string acc = mut_var_of(scalar(GType::Int));
        const std::string src = pipe_stream(2);
        const std::string x = fresh_name();
        env.push_back({ x, scalar(GType::Int), false });
        const std::string pre = break_continue_prefix(2);   // an ordered stream -> break/continue safe
        const std::string body = acc.empty() ? gen_expr(scalar(GType::Int), 2)
                                             : acc + " = " + acc + " + " + gen_expr(scalar(GType::Int), 2);
        env.pop_back();
        return "for " + x + " in " + src + " { " + pre + body + " }\n";
    }

    // ---- deep nesting (Slice 5) ----
    // A 10..20-link chain over one Int, in one of four shapes: nested direct calls, a pipe chain, a
    // method chain on a trait implemented for Int, and a pipeline of `map` stages (prelude only). The
    // frame measure used to be exponential in exactly this nesting (2^n, 3^n with the inliner), so a
    // regression would turn the sweep into a hang instead of passing. Kept well below the ~60-link
    // point where the 63-register frame limit is reached.
    std::string gen_deep_chain(int depth) {
        const int n = 10 + (int)pick(11);
        const std::string seed = gen_expr(scalar(GType::Int), depth > 1 ? 1 : 0);
        std::string s;
        switch (pick(prelude_mode ? 4 : 3)) {
            case 0:
                for (int i = 0; i < n; ++i) s += "ginc(";
                s += seed;
                for (int i = 0; i < n; ++i) s += ")";
                return s;
            case 1:
                s = "(" + seed;
                for (int i = 0; i < n; ++i) s += " |> ginc";
                return s + ")";
            case 2:
                s = "(" + seed + ")";
                for (int i = 0; i < n; ++i) s += ".gstep()";
                return "(" + s + ")";
            default:
                s = "range(0, 3)";
                for (int i = 0; i < n; ++i) s += " |> map(fn(x: Int) -> Int { x + 1 })";
                return "(" + s + " |> sum)";
        }
    }

    std::string gen_match(const GType& t, int depth) {
        std::string s = "(match " + gen_expr(scalar(GType::Int), depth - 1) + " { ";
        const int narms = (int)pick(3) + 1;
        for (int k = 0; k < narms; ++k) s += std::to_string(k) + " => " + gen_expr(t, depth - 1) + ", ";
        return s + "_ => " + gen_expr(t, depth - 1) + " })";
    }

    // An OR-PATTERN match yielding scalar t: `match <int> { 0 | 1 => e, 2 | 3 => e, _ => e }`. Each arm
    // groups two DISTINCT int literals with `|` (no overlap within or across arms => no unreachable-
    // alternative warnings), trailing `_` for exhaustiveness. Cross-checks the or-pattern codegen vs.
    // the oracle (first matching alternative wins). v1a: no bindings in alternatives.
    std::string gen_or_match(const GType& t, int depth) {
        std::string s = "(match " + gen_expr(scalar(GType::Int), depth - 1) + " { ";
        const int narms = (int)pick(3) + 1;
        for (int k = 0; k < narms; ++k)
            s += std::to_string(2 * k) + " | " + std::to_string(2 * k + 1) +
                 " => " + gen_expr(t, depth - 1) + ", ";
        return s + "_ => " + gen_expr(t, depth - 1) + " })";
    }

    // =====================================================================================
    // Top-level declaration generation.  Order: structs -> traits(+impl+wrapper) -> funcs
    // -> hofs.  Each stage may reference only EARLIER stages, so the whole decl graph is
    // acyclic and every generated call chain terminates (no runtime recursion to overflow
    // the tree-walking oracle).
    // =====================================================================================
    std::string gen_structs() {
        std::string src;
        // The prelude `Char` (a transparent newtype over the code-point Int) is always available in
        // prelude mode: seed it as a usable newtype WITHOUT a definition (it lives in std::string), so it
        // flows into toString/interpolation holes and stresses the GLYPH render (codePointToStr) across
        // arbitrary code points. Differentially safe for ANY under-value -- the VM's codePointToStr and
        // the oracle's utf8_encode agree byte-for-byte (incl. invalid/surrogate -> U+FFFD).
        if (prelude_mode) { NewtypeDef c; c.name = "Char"; c.under = scalar(GType::Int); newtypes.push_back(std::move(c)); }
        const int n = (int)pick(4);   // 0..3 record structs
        for (int k = 0; k < n; ++k) {
            StructDef sd; sd.name = "S" + std::to_string(k);
            const int nf = 1 + (int)pick(3);   // 1..3 fields (mostly scalar, sometimes a container)
            src += "struct " + sd.name + " { ";
            for (int f = 0; f < nf; ++f) {
                const GType ft = chance(28) ? random_container() : random_scalar();
                const std::string fn = "f" + std::to_string(f);
                sd.fields.push_back({ fn, ft });
                if (f) src += ", ";
                src += fn + ": " + ty_name(ft);
            }
            src += " }\n";
            structs.push_back(std::move(sd));
        }
        // 0..2 transparent (erased) newtypes over a scalar Int/Double/Bool -- stress the erasure paths
        // (construction / `.0` projection all lower to no-ops; the value IS the underlying immediate).
        const int nt = (int)pick(3);
        for (int k = 0; k < nt; ++k) {
            const int u = (int)pick(3);
            const GType under = scalar(u == 0 ? GType::Int : (u == 1 ? GType::Double : GType::Bool));
            NewtypeDef nd; nd.name = "N" + std::to_string(k); nd.under = under;
            src += "transparent struct " + nd.name + "(" + ty_name(under) + ")\n";
            newtypes.push_back(std::move(nd));
        }
        return src;
    }
    // 0..2 enums, each 2..3 variants, each variant 0..2 SCALAR payload fields. Variant names are
    // globally unique (`E{k}V{v}`) as the language requires. Emitted after structs, before traits.
    std::string gen_enums() {
        std::string src;
        const int n = (int)pick(3);   // 0..2 enums
        for (int k = 0; k < n; ++k) {
            EnumDef ed; ed.name = "E" + std::to_string(k);
            const int nv = 2 + (int)pick(2);   // 2..3 variants
            // ~40% of enums are INT-BACKED (`: Int`): every variant nullary, the value erases to a bare
            // Int discriminant. ~half of those PIN a strictly-increasing (distinct) discriminant sequence
            // to stress `= v` parsing/checking; the rest use declaration-order auto-increment. Their
            // values flow into interpolation holes (random_ty->Enum), stressing the toString name switch.
            const bool int_backed = chance(40);
            const bool pin = int_backed && chance(50);
            int64_t disc = (int64_t)pick(7) - 3;   // pinned sequence start in [-3, 3]
            src += "enum " + ed.name + (int_backed ? " : Int { " : " { ");
            for (int v = 0; v < nv; ++v) {
                EnumDef::Variant var; var.name = ed.name + "V" + std::to_string(v);
                const int nf = int_backed ? 0 : (int)pick(3);   // int-backed: nullary only
                if (v) src += ", ";
                src += var.name;
                if (pin) { src += " = " + std::to_string(disc); disc += 1 + (int64_t)pick(3); }  // strictly increasing
                if (nf) {
                    var.is_record = chance(40);   // ~40% of payload variants use the record (brace) form
                    src += var.is_record ? " { " : "(";
                    for (int f = 0; f < nf; ++f) {
                        const GType ft = random_scalar();
                        var.fields.push_back(ft);
                        if (f) src += ", ";
                        if (var.is_record) src += "f" + std::to_string(f) + ": ";
                        src += ty_name(ft);
                    }
                    src += var.is_record ? " }" : ")";
                }
                ed.variants.push_back(std::move(var));
            }
            src += " }\n";
            enums.push_back(std::move(ed));
        }
        return src;
    }
    // Traitless `impl S { .. }` blocks (see InherentDef), for ~half the structs. Bodies see only EARLIER
    // declarations: a struct's own members are recorded after all of its bodies are generated, so no
    // member can call itself or a sibling (no runtime recursion for the tree-walking oracle).
    std::string gen_inherent() {
        std::string src;
        for (int s = 0; s < (int)structs.size(); ++s) {
            if (!chance(50)) continue;
            const StructDef& sd = structs[s];
            InherentDef ih; ih.sid = s;
            const std::string k = std::to_string(s);
            std::string body = "impl " + sd.name + " {\n";
            if (chance(70)) {   // fn new(p0: F0, ..) -> S { S { f0: p0, .. } }   (`-> Self` sometimes)
                ih.has_new = true;
                std::string sig, lit;
                for (size_t i = 0; i < sd.fields.size(); ++i) {
                    if (i) { sig += ", "; lit += ", "; }
                    sig += "p" + std::to_string(i) + ": " + ty_name(sd.fields[i].second);
                    lit += sd.fields[i].first + ": p" + std::to_string(i);
                }
                body += "  fn new(" + sig + ") -> " + (chance(30) ? std::string("Self") : sd.name) +
                        " { " + sd.name + " { " + lit + " } }\n";
            }
            if (chance(80)) {   // fn igK(self) -> R
                ih.get_m = "ig" + k; ih.get_ret = random_scalar();
                env.push_back({ "self", struct_ty(s), false });
                const std::string b = gen_expr(ih.get_ret, 2);
                env.pop_back();
                body += "  fn " + ih.get_m + "(self) -> " + ty_name(ih.get_ret) + " { " + b + " }\n";
            }
            if (chance(50)) {   // fn iaK(self, d: A) -> R
                ih.arg_m = "ia" + k; ih.arg_ty = random_scalar(); ih.arg_ret = random_scalar();
                env.push_back({ "self", struct_ty(s), false });
                env.push_back({ "d", ih.arg_ty, false });
                const std::string b = gen_expr(ih.arg_ret, 2);
                env.pop_back(); env.pop_back();
                body += "  fn " + ih.arg_m + "(self, d: " + ty_name(ih.arg_ty) + ") -> " +
                        ty_name(ih.arg_ret) + " { " + b + " }\n";
            }
            const std::string fld = int_field_of(s);
            if (!fld.empty() && chance(60)) {   // fn ibK(mut self) -> () { self.f = self.f + 1 }
                ih.bump_m = "ib" + k; ih.bump_field = fld;
                body += "  fn " + ih.bump_m + "(mut self) -> () { self." + fld + " = self." + fld + " + 1 }\n";
            }
            if (!ih.has_new && ih.get_m.empty() && ih.arg_m.empty() && ih.bump_m.empty()) continue;
            src += body + "}\n";
            inherents.push_back(std::move(ih));
        }
        return src;
    }
    std::string gen_traits() {
        std::string src;
        for (int s = 0; s < (int)structs.size(); ++s) {
            if (!chance(45)) continue;
            const int k = (int)traits.size();
            const std::string tn = "Tr" + std::to_string(k);
            const std::string mn = "m" + std::to_string(k);
            const std::string wn = "w" + std::to_string(k);
            const GType ret = random_scalar();
            const std::string rn = ty_name(ret);
            src += "trait " + tn + " { fn " + mn + "(self) -> " + rn + " }\n";
            env.push_back({ "self", struct_ty(s), false });   // `self` in scope for the method body
            const std::string body = gen_expr(ret, 2);
            env.pop_back();
            src += "impl " + tn + " for " + structs[s].name + " { fn " + mn + "(self) -> " + rn +
                   " { " + body + " } }\n";
            // A generic wrapper whose erased param exercises the DYNAMIC dispatch path.
            src += "fn " + wn + "[X: " + tn + "](x: X) -> " + rn + " { " + mn + "(x) }\n";
            traits.push_back({ tn, mn, wn, ret, s });
            // Sometimes a SUBTRAIT `Sub : Tr` whose method calls the supertrait method (only when the
            // supertrait method returns Int, so `mN(self) + 1` type-checks). Impls both on the SAME
            // struct (coherence: a subtrait impl requires the supertrait impl, emitted just above).
            if (ret.kind == GType::Int && chance(40)) {
                const std::string subn = "Sub" + std::to_string(k);
                const std::string subm = "sm" + std::to_string(k);
                src += "trait " + subn + " : " + tn + " { fn " + subm + "(self) -> Int }\n";
                src += "impl " + subn + " for " + structs[s].name + " { fn " + subm +
                       "(self) -> Int { " + mn + "(self) + 1 } }\n";
                traits.push_back({ subn, subm, "", ret, s });   // no wrapper
            }
        }
        return src;
    }
    // A wide-frame function: 25..45 simultaneously-live Int locals, all read in one final sum, so the
    // frame allocator is pushed toward the 64-register r6 ceiling (small-frame generation never
    // exercises register pressure). Stays safely < 64 (locals + a couple sum temps). Recorded so the
    // call productions can also invoke it (call + wide frame). Declared in a fraction of programs.
    std::string gen_wide_fns() {
        if (!chance(60)) return std::string();
        const int k = 25 + (int)pick(21);   // 25..45 locals
        std::string src = "fn wide0() -> Int {\n";
        for (int i = 0; i < k; ++i) src += "let a" + std::to_string(i) + ": Int = " + int_lit() + "\n";
        src += "(";
        for (int i = 0; i < k; ++i) { if (i) src += " + "; src += "a" + std::to_string(i); }
        src += ")\n}\n";
        funcs.push_back({ "wide0", {}, scalar(GType::Int) });
        return src;
    }
    // Generic helper functions (declared once): `gid` (identity at any type) and `gtw` (apply twice).
    // Always emitted; if never instantiated, codegen emits no body (monomorphize-on-use).
    std::string gen_generic_fns() {
        gid_declared = gtw_declared = true;
        return "fn gid[T](x: T) -> T { x }\n"
               "fn gtw[T](f: fn(T) -> T, x: T) -> T { f(f(x)) }\n"
               // The links of gen_deep_chain: a direct fn and a trait method on Int.
               "fn ginc(x: Int) -> Int { x + 1 }\n"
               "trait GStep { fn gstep(self) -> Int }\n"
               "impl GStep for Int { fn gstep(self) -> Int { self + 1 } }\n";
    }
    // A blanket impl `impl[T: Base] Dbg for T { fn dbg(self) -> Int { base(self) + 1 } }` plus a
    // concrete `impl Base for S`, so `dbg(S{..})` routes through the blanket. Records both methods.
    std::string gen_blankets() {
        std::string src;
        for (int s = 0; s < (int)structs.size(); ++s) {
            if (!chance(30)) continue;
            const std::string fld = int_field_of(s);
            const int k = (int)traits.size();
            const std::string basen = "Bshow" + std::to_string(k), basem = "bshw" + std::to_string(k);
            const std::string dbgn  = "Bdbg" + std::to_string(k),  dbgm  = "bdbg" + std::to_string(k);
            const std::string body  = fld.empty() ? std::string("0") : "self." + fld;
            src += "trait " + basen + " { fn " + basem + "(self) -> Int }\n";
            src += "trait " + dbgn + " { fn " + dbgm + "(self) -> Int }\n";
            src += "impl[T: " + basen + "] " + dbgn + " for T { fn " + dbgm +
                   "(self) -> Int { " + basem + "(self) + 1 } }\n";
            src += "impl " + basen + " for " + structs[s].name + " { fn " + basem +
                   "(self) -> Int { " + body + " } }\n";
            traits.push_back({ dbgn, dbgm, "", scalar(GType::Int), s });   // via blanket
            traits.push_back({ basen, basem, "", scalar(GType::Int), s }); // direct
        }
        return src;
    }
    // Conditional impls: the wrapper satisfies the trait only because its type argument does, so every call
    // below makes the checker prove the impl's OWN bound (`T: CtK`) through 1..3 wrapper layers. Only
    // satisfying instantiations are generated, so a spurious rejection surfaces as a GEN-BUG.
    std::string gen_conditional_impls() {
        std::string src;
        for (int s = 0; s < (int)structs.size(); ++s) {
            if (!chance(25)) continue;
            const std::string k = std::to_string(conds.size());
            const std::string wrap = "Cw" + k, tn = "Ct" + k, mn = "cm" + k, fnw = "cf" + k;
            const std::string fld = int_field_of(s);
            src += "struct " + wrap + "[T] { inner: T, k: Int }\n";
            src += "trait " + tn + " { fn " + mn + "(self) -> Int }\n";
            src += "impl " + tn + " for " + structs[s].name + " { fn " + mn + "(self) -> Int { " +
                   (fld.empty() ? std::string("0") : "self." + fld) + " } }\n";
            src += "impl[T: " + tn + "] " + tn + " for " + wrap + "[T] { fn " + mn +
                   "(self) -> Int { self.inner." + mn + "() + self.k } }\n";
            src += "fn " + fnw + "[X: " + tn + "](x: X) -> Int { x." + mn + "() }\n";
            const std::string gtn = "Gb" + k, gmn = "gb" + k;
            src += "trait " + gtn + " { fn " + gmn + "[X: " + tn + "](self, x: X) -> Int }\n";
            src += "impl " + gtn + " for Int { fn " + gmn + "[X: " + tn + "](self, x: X) -> Int { self + x." + mn + "() } }\n";
            conds.push_back({ wrap, tn, mn, fnw, gtn, gmn, s });
        }
        return src;
    }
    std::string cond_call(int depth) {
        const CondImplDef& c = conds[pick((uint32_t)conds.size())];
        std::string v = gen_expr(struct_ty(c.sid), depth - 1);
        const int layers = 1 + (int)pick(3);
        for (int i = 0; i < layers; ++i) v = c.wrap + " { inner: " + v + ", k: " + int_lit() + " }";
        switch (pick(6)) {
            case 5:  return "((" + int_lit() + ")." + c.gmethod + "(" + v + "))";
            case 0:  return "((" + v + ")." + c.method + "())";
            case 1:  return c.trait + "::" + c.method + "(" + v + ")";
            case 2:  return c.fn + "(" + v + ")";
            case 3:  return "((" + v + ") |> " + c.fn + ")";
            default: return c.method + "(" + v + ")";
        }
    }
    // `gtw(fn(a: T) -> T { body }, arg)` -- apply a lambda twice through the generic helper (scalar T).
    std::string gen_gtw(const GType& t, int depth) {
        const std::string ap = "a" + std::to_string(name_counter++);
        env.push_back({ ap, t, false });
        const std::string body = gen_expr(t, depth - 1);
        env.pop_back();
        const std::string arg = gen_expr(t, depth - 1);
        return "gtw(fn(" + ap + ": " + ty_name(t) + ") -> " + ty_name(t) + " { " + body + " }, " + arg + ")";
    }
    std::string gen_functions() {
        std::string src;
        const int nfns = (int)pick(4);   // 0..3
        for (int i = 0; i < nfns; ++i) {
            // `uf<N>`, NOT `f<N>`: a struct FIELD is `f<N>` too, and the field-shorthand production
            // binds a block local named exactly like the field. A generated fn named `f1` was then
            // shadowed by that local, so a later `f1()` in the same block called a Double -- the
            // generator emitting ill-typed source, i.e. a violation of its own by-construction
            // contract (7 gen-bugs per 1000 seeds). Shadowing a top-level fn is a real language
            // feature and worth generating one day, but deliberately, by a production that also
            // teaches gen_expr the name is hidden -- not by accident.
            const std::string name = "uf" + std::to_string(i);
            const int arity = (int)pick(4);   // 0..3
            std::vector<GType> params;
            std::string sig;
            for (int p = 0; p < arity; ++p) {
                const GType pt = random_ty();
                params.push_back(pt);
                if (p) sig += ", ";
                sig += "p" + std::to_string(p) + ": " + ty_name(pt);
            }
            const GType ret = random_ty();
            std::vector<Binding> saved = env;
            env.clear();
            for (int p = 0; p < arity; ++p) env.push_back({ "p" + std::to_string(p), params[p], false });
            const std::string body = chance(30) ? early_return_body(ret, 3) : gen_expr(ret, 3);
            env = saved;
            src += "fn " + name + "(" + sig + ") -> " + ty_name(ret) + " { " + body + " }\n";
            funcs.push_back({ name, params, ret });
        }
        const int nhofs = (int)pick(3);   // 0..2
        for (int i = 0; i < nhofs; ++i) {
            const std::string name = "h" + std::to_string(i);
            const GType a = random_scalar(), b = random_scalar();
            src += "fn " + name + "(f: fn(" + ty_name(a) + ") -> " + ty_name(b) +
                   ", x: " + ty_name(a) + ") -> " + ty_name(b) + " { f(x) }\n";
            hofs.push_back({ name, a, b });
        }
        return src;
    }

    // First Int field name of struct `sid`, or "" if it has none.
    std::string int_field_of(int sid) {
        for (const auto& f : structs[sid].fields) if (f.second.kind == GType::Int) return f.first;
        return std::string();
    }
    // `mut`-mutation declarations (emitted after functions): for each struct with an Int field, maybe
    // a mut-PARAM mutator fn (`fn mN(mut p0: S, p1: Int) -> Int { p0.f = p1 ... }`) and/or a mut-SELF
    // trait method (`impl MTr for S { fn bN(mut self) -> Int { self.f = self.f + 1 ... } }`). Both
    // mutate a heap struct field in place; gen_mut_stmt exercises the caller-visible effect.
    std::string gen_mutation_decls() {
        std::string src;
        for (int s = 0; s < (int)structs.size(); ++s) {
            const std::string fld = int_field_of(s);
            if (fld.empty()) continue;
            const std::string sn = structs[s].name;
            if (chance(45)) {
                const std::string nm = "mmut" + std::to_string(mutators.size());
                src += "fn " + nm + "(mut p0: " + sn + ", p1: Int) -> Int { p0." + fld + " = p1\n p1 }\n";
                mutators.push_back({ nm, s, fld });
            }
            if (chance(45)) {
                const int k = (int)mut_methods.size();
                const std::string tn = "MTr" + std::to_string(k);
                const std::string mn = "bump" + std::to_string(k);
                src += "trait " + tn + " { fn " + mn + "(mut self) -> Int }\n";
                src += "impl " + tn + " for " + sn + " { fn " + mn + "(mut self) -> Int { self." + fld +
                       " = self." + fld + " + 1\n self." + fld + " } }\n";
                mut_methods.push_back({ mn, s, fld, tn });
            }
        }
        return src;
    }
    // A statement that mutates a fresh heap struct in place through a mut param / mut self, then
    // OBSERVES the caller-visible effect by folding the mutated field into the Int accumulator.
    bool has_inherent_bump() {
        for (const auto& ih : inherents) if (!ih.bump_m.empty()) return true;
        return false;
    }
    std::string gen_mut_stmt() {
        const bool useInherent = has_inherent_bump() && ((mutators.empty() && mut_methods.empty()) || chance(50));
        const bool useMethod = !useInherent && !mut_methods.empty() && (mutators.empty() || chance(50));
        int sid; std::string call, fld;
        const std::string sv = fresh_name();
        if (useInherent) {
            // An inherent `mut self` method: `sv.ibK()` or `S::ibK(sv)` (both mutate the caller's binding).
            std::vector<const InherentDef*> bs;
            for (const auto& ih : inherents) if (!ih.bump_m.empty()) bs.push_back(&ih);
            const InherentDef& ih = *bs[pick((uint32_t)bs.size())];
            sid = ih.sid; fld = ih.bump_field;
            call = chance(60) ? sv + "." + ih.bump_m + "()\n"
                              : structs[sid].name + "::" + ih.bump_m + "(" + sv + ")\n";
        } else if (useMethod) {
            const MutMethodDef& m = mut_methods[pick((uint32_t)mut_methods.size())];
            sid = m.sid; fld = m.field;
            // Prefix, method syntax on the mut binding, or trait-qualified.
            switch (pick(3)) {
                case 0:  call = m.method + "(" + sv + ")\n"; break;
                case 1:  call = sv + "." + m.method + "()\n"; break;
                default: call = m.trait + "::" + m.method + "(" + sv + ")\n"; break;
            }
        } else {
            const MutatorDef& m = mutators[pick((uint32_t)mutators.size())];
            sid = m.sid; fld = m.field;
            call = m.name + "(" + sv + ", " + gen_expr(scalar(GType::Int), 2) + ")\n";
        }
        std::string s = "let mut " + sv + ": " + structs[sid].name + " = " + struct_lit(sid, 2) + "\n";
        env.push_back({ sv, struct_ty(sid), true });
        s += call;
        const std::string acc = mut_var_of(scalar(GType::Int));
        if (!acc.empty()) s += acc + " = " + acc + " + (" + sv + ")." + fld + "\n";
        return s;
    }

    // ---- statements ----
    std::string int_container() {
        if (chance(50)) {
            const int n = (int)pick(4) + 1;
            std::string s = "[";
            for (int i = 0; i < n; ++i) { if (i) s += ", "; s += int_lit(); }
            return s + "]";
        }
        return "array(" + std::to_string(pick(4) + 1) + ", " + int_lit() + ")";
    }
    // A compound-assignment operator for a NUMERIC target, or "" for the plain `=`. Compound
    // assignment is numeric-only, and its whole point is that the place is read-modify-written over a
    // single evaluation -- so the generator must produce it, or the differential sweep would cover
    // none of that path. `/=` and `%=` may divide by zero; that is fine, both sides trap identically
    // and check_same compares faults.
    std::string compound_op(const GType& t) {
        if (t.kind != GType::Int && t.kind != GType::Double) return "";
        if (!chance(45)) return "";
        // Int also takes the bitwise and shift forms (`&=` .. `>>>=`); Double has only the arithmetic ones.
        static const char* OPS[] = { "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", ">>>" };
        return OPS[pick(t.kind == GType::Int ? 11 : 5)];
    }
    std::string gen_assign() {
        // Pick from the types that actually HAVE a mut binding, rather than drawing a random type and
        // usually finding none. MEASURED: the old order (random_ty, then mut_var_of) produced an
        // assignment in a small minority of draws, so this production -- and with it every compound
        // form -- was nearly absent from the sweep (1 statement across 60 seeds).
        std::vector<int> idx;
        for (int i = 0; i < (int)env.size(); ++i) if (env[i].is_mut) idx.push_back(i);
        if (idx.empty()) return "";
        const int k = idx[pick((uint32_t)idx.size())];
        // COPY the name and type out of `env` before calling gen_expr: gen_expr pushes bindings of its
        // own (a lambda parameter, a match-arm binding), which reallocates the vector and would leave a
        // reference into it dangling -- the emitted statement then lost its target name entirely.
        const std::string name = env[k].name;
        const GType       ty   = env[k].ty;
        return name + " " + compound_op(ty) + "= " + gen_expr(ty, 3) + "\n";
    }
    // `s.field = <expr>` on a mutable in-scope STRUCT binding (exercises SET_PROP through mut, and
    // with a compound operator the GET_PROP/op/SET_PROP round trip over one receiver evaluation).
    std::string gen_field_assign() {
        std::vector<std::pair<std::string, std::pair<std::string, GType>>> hits;  // (var, (field, fieldTy))
        for (const auto& b : env)
            if (b.is_mut && b.ty.kind == GType::Struct)
                for (const auto& f : structs[b.ty.sid].fields) hits.push_back({ b.name, f });
        if (hits.empty()) return "";
        const auto& h = hits[pick((uint32_t)hits.size())];
        return h.first + "." + h.second.first + " " + compound_op(h.second.second) + "= " +
               gen_expr(h.second.second, 3) + "\n";
    }
    // Optional guarded `break` / `continue` prefix for a loop body. Both guards are pure value
    // predicates, so the set of iterations they affect is identical in the VM and the oracle. Only
    // safe where iteration order matches (arrays/lists/vecs/bytes/while) -- NOT map iteration, whose
    // order differs, so a positional `break` would diverge.
    std::string break_continue_prefix(int depth) {
        std::string b;
        if (chance(22)) b += "if (" + gen_expr(scalar(GType::Bool), depth) + ") { continue }\n";
        if (chance(18)) b += "if (" + gen_expr(scalar(GType::Bool), depth) + ") { break }\n";
        return b;
    }
    // Bounded counting `while`. The counter is advanced FIRST (so a `continue` can't skip the
    // increment and loop forever), then an optional guarded break/continue, then the accumulation.
    std::string gen_while() {
        const int k = (int)pick(6) + 2;                 // 2..7 iterations
        const std::string acc = mut_var_of(scalar(GType::Int));
        const std::string c = fresh_name();
        env.push_back({ c, scalar(GType::Int), true });
        std::string body = c + " = " + c + " + 1\n";    // increment FIRST -> continue-safe
        body += break_continue_prefix(2);
        if (!acc.empty()) body += acc + " = " + acc + " + " + gen_expr(scalar(GType::Int), 2) + "\n";
        return "let mut " + c + ": Int = 0\n" +
               "while (" + c + " < " + std::to_string(k) + ") { " + body + " }\n";
    }
    // `for x in <container> { acc = acc + <expr(x)> }` over an Int array or (when a struct with an
    // Int field exists) a small vec of structs whose field is summed -- the vec-of-structs + GC +
    // field-access-in-loop shape.
    std::string gen_for() {
        const std::string acc = mut_var_of(scalar(GType::Int));
        // Sometimes iterate an inline MAP (two-column `(k, v)`) or BYTE buffer instead of an array.
        if (!acc.empty() && chance(30)) {
            if (chance(50)) {   // for (k, v) in #{ ... }
                const int n = 2 + (int)pick(2);
                std::string lit = "#{ ";
                for (int i = 0; i < n; ++i) { if (i) lit += ", "; lit += int_lit() + " => " + int_lit(); }
                lit += " }";
                const std::string k = fresh_name(), v = fresh_name();
                return "for (" + k + ", " + v + ") in " + lit + " { " + acc + " = " + acc + " + " + v + " }\n";
            }
            const std::string x = fresh_name();
            const std::string lit = nonempty_string_lit();
            // Iterate either the String DIRECTLY (`for c in s` -- Option B byte iteration) or its Bytes
            // snapshot; both yield the same Int bytes 0..255. The direct form needs the prelude's
            // `impl IntoIterator[Int] for String`, so it is prelude-mode-only; Bytes iteration is a
            // prelude-free codegen fast path.
            const std::string src = (prelude_mode && chance(50)) ? lit : ("toBytes(" + lit + ")");
            return "for " + x + " in " + src + " { " + acc + " = " + acc + " + " + x + " }\n";
        }
        // Try a struct container: pick a struct with an Int field.
        std::vector<std::pair<int, std::string>> sf;
        for (int s = 0; s < (int)structs.size(); ++s)
            for (const auto& f : structs[s].fields) if (f.second.kind == GType::Int) sf.push_back({ s, f.first });
        if (!sf.empty() && chance(45)) {
            const auto& h = sf[pick((uint32_t)sf.size())];
            const std::string x = fresh_name();
            const int n = (int)pick(3) + 1;   // 1..3 elements
            std::string lit = "[";
            for (int i = 0; i < n; ++i) { if (i) lit += ", "; lit += struct_lit(h.first, 2); }
            lit += "]";
            env.push_back({ x, struct_ty(h.first), false });
            const std::string pre = break_continue_prefix(2);   // ordered iteration -> break/continue safe
            const std::string body = pre + (acc.empty() ? gen_expr(scalar(GType::Int), 2)
                                                        : acc + " = " + acc + " + (" + x + ")." + h.second);
            env.pop_back();
            return "for " + x + " in " + lit + " { " + body + " }\n";
        }
        const std::string x = fresh_name();
        const std::string container = int_container();
        env.push_back({ x, scalar(GType::Int), false });
        const std::string pre = break_continue_prefix(2);       // ordered iteration -> break/continue safe
        const std::string body = pre + (acc.empty() ? gen_expr(scalar(GType::Int), 2)
                                                    : acc + " = " + acc + " + " + gen_expr(scalar(GType::Int), 2));
        env.pop_back();
        return "for " + x + " in " + container + " { " + body + " }\n";
    }
    // Prelude helper fns (declared only in prelude mode) exercising `?` on Result/Option and a
    // named-fn `resultAndThen` step. Deterministic: ptry_ok(n)=n+5, ptry_err(n)=n, ptry_some(n)=n*2.
    std::string gen_prelude_helpers() {
        return "fn pstep(x: Int) -> Result[Int, Int] { Ok(x + 1) }\n"
               "fn ptry_ok(n: Int) -> Result[Int, Int] { let x = Ok(n)?\n Ok(x + 5) }\n"
               "fn ptry_err(n: Int) -> Result[Int, Int] { let x: Int = Err(n)?\n Ok(x + 5) }\n"
               "fn ptry_some(n: Int) -> Option[Int] { let x = Some(n)?\n Some(x * 2) }\n";
    }
    // A Vec[Int] built inline as a push-chain, so pop/get receive a TEMP-materializing argument (a call
    // result). The hand tests only ever pop/get a BARE LOCAL, which hides an Option-wrapping frame
    // under-measure (a local is not a temp, so the scratch fits in the slack); a call/field argument
    // stacks the builtin's scratch on top of a real temp. 2..3 elements.
    std::string gen_int_vec(int depth) {
        auto g = [&]() { return gen_expr(scalar(GType::Int), depth - 1); };
        const int n = 2 + static_cast<int>(pick(2));
        std::string s = "vec()";
        for (int i = 0; i < n; ++i) s = "push(" + s + ", " + g() + ")";
        return s;
    }
    // An Int-typed expression built from Option/Result + the prelude combinators (prelude mode only).
    std::string gen_prelude_int(int depth) {
        auto g = [&]() { return gen_expr(scalar(GType::Int), depth - 1); };
        // The bound must stay ONE ABOVE the highest explicit case, or `default:` becomes dead and
        // silently stops being generated.
        switch (pick(23)) {
            case 21: return "(" + gen_opt_int(depth) + ").unwrapOr(" + g() + ")";
            case 22: return "(match " + gen_res_int(depth) + " { Ok(v) => v, Err(e) => e })";
            // Method syntax on the prelude's Unwrap / Ord traits -- the form a user reaches for first, and
            // the one whose tree-shaker keep rule was missing (`o.unwrapOr(0)` crashed codegen).
            case 15: return "Some(" + g() + ").unwrapOr(" + g() + ")";
            case 16: return "pstep(" + g() + ").unwrap()";
            case 17: return "Some(" + g() + ").expect(\"gen\")";
            case 18: return "pop(" + gen_int_vec(depth) + ").unwrapOr(" + g() + ")";
            case 19: return "(if (" + g() + ").lessThan(" + g() + ") { " + g() + " } else { " + g() + " })";
            case 20: return chance(50) ? "ptry_err(" + g() + ").unwrapOr(" + g() + ")"
                                       : "get(" + gen_int_vec(depth) + ", " + g() + ").unwrapOr(" + g() + ")";
            case 0:  return "unwrapOr(Some(" + g() + "), " + g() + ")";
            case 1:  return "unwrapOr(None, " + g() + ")";
            case 2:  return "unwrap(Some(" + g() + "))";
            case 3:  return "(match Some(" + g() + ") { Some(v) => v, None => 0 })";
            case 4:  { const std::string x = "a" + std::to_string(name_counter++);
                       env.push_back({ x, scalar(GType::Int), false });
                       const std::string body = g();
                       env.pop_back();
                       return "(match optionMap(Some(" + g() + "), fn(" + x + ": Int) -> Int { " + body +
                              " }) { Some(v) => v, None => 0 })"; }
            case 5:  return "(match resultAndThen(Ok(" + g() + "), pstep) { Ok(v) => v, Err(e) => e })";
            case 6:  return "(if isSome(Some(" + g() + ")) { " + g() + " } else { " + g() + " })";
            case 7:  return "(if isNone(None) { " + g() + " } else { " + g() + " })";
            case 8:  return "(match ptry_ok(" + g() + ") { Ok(v) => v, Err(e) => e })";
            case 9:  return "(match ptry_err(" + g() + ") { Ok(v) => v, Err(e) => e })";
            // A BINDING or-pattern. `Result[Int, Int]` gives both alternatives the same payload type
            // by construction, which is what the same-binder-set rule requires -- generated enums
            // draw each field type independently, so a matching pair is only luck there.
            //
            // This is the only thing that puts the canonical-register MOV block under arbitrary
            // surrounding register pressure, which is where a measure/emit disagreement shows up.
            // `gen_or_match` cannot be extended in place: bindings would collide with its
            // disjoint-literal invariant (the thing that keeps it warning-free). The binder joins
            // the shared `a<N>` namespace and is pushed into `env` at Int for the arm body.
            case 10: return "(match ptry_some(" + g() + ") { Some(v) => v, None => 0 })";
            // pop/get over a container -> Option, unwrapped to Int. These exercise the Option-wrapping
            // builtins' frame sizing -- the class the hand tests missed (they only ever pop/get a bare
            // LOCAL). Two argument shapes: a nullary container call `vec()`/`bytes()` (measured temp
            // EXACTLY 1 -- the shape whose scratch stacks on a real temp and can trip an under-measured
            // frame), and a push-chain build (temp >= 2). get is total (None on an out-of-range index),
            // so any g() index cross-checks against the oracle.
            case 11: return "unwrapOr(pop(vec()), " + g() + ")";     // temp-1 arg (empty -> None)
            case 12: return "unwrapOr(pop(" + gen_int_vec(depth) + "), " + g() + ")";
            case 13: return "(match pop(" + gen_int_vec(depth) + ") { Some(v) => v, None => " + g() + " })";
            default: return chance(50)
                ? "unwrapOr(get(vec(), " + g() + "), " + g() + ")"   // temp-1 arg (OOB -> None)
                : "unwrapOr(get(" + gen_int_vec(depth) + ", " + g() + "), " + g() + ")";
        }
    }
    // pop/get over a first-class Vec[Int] or Map[K,Int] -> Option[Int], unwrapped to Int. The container
    // expression routes through gen_expr, so it may be a struct-field PROJECTION (`(s).f`) -- a
    // temp-materializing argument, the frame-sizing shape. get is total, so any index/key agrees.
    std::string gen_container_int(int depth) {
        auto g = [&]() { return gen_expr(scalar(GType::Int), depth - 1); };
        switch (pick(3)) {
            // pop MUTATES its receiver, so it must get a TEMPORARY (an rvalue push-chain is exempt from
            // the mut-at-call-site rule); a named non-mut Vec binding from gen_expr would be rejected.
            case 0:  return "unwrapOr(pop(" + gen_int_vec(depth) + "), " + g() + ")";
            case 1:  { const std::string get = "get(" + gen_expr(vec_ty(scalar(GType::Int)), depth - 1) + ", " + g() + ")";
                       return chance(35) ? get + ".unwrapOr(" + g() + ")" : "unwrapOr(" + get + ", " + g() + ")"; }
            default: { const GType k = random_map_key();
                       const std::string get = "get(" + gen_expr(map_ty(k, scalar(GType::Int)), depth - 1) +
                                               ", " + scalar_lit(k) + ")";
                       return chance(35) ? get + ".unwrapOr(" + g() + ")" : "unwrapOr(" + get + ", " + g() + ")"; }
        }
    }
    // Thin container accessors: for each struct with a Vec[Int]/Map[K,Int] field, a RAW Option-returning
    // accessor whose body is the bare pop/get on the field. Its body is a TIGHT frame (one struct param,
    // a temp-materializing GET_PROP argument), so its COMPILATION exercises the Option-wrapping builtins'
    // frame sizing -- the exact shape the hand regression tests cover, now generated. Plus an Int wrapper
    // recorded as a callable fn (runtime differential of pop/get-through-a-field). Prelude-only.
    std::string gen_container_accessors() {
        if (!prelude_mode) return std::string();
        std::string src;
        for (int s = 0; s < (int)structs.size(); ++s) {
            for (const auto& f : structs[s].fields) {
                const GType& ft = f.second;
                const bool vecInt = ft.kind == GType::Vec && ft.elems[0].kind == GType::Int;
                const bool mapInt = ft.kind == GType::Map && ft.elems[1].kind == GType::Int;
                if (!(vecInt || mapInt) || !chance(70)) continue;
                const std::string sn  = structs[s].name;
                const std::string raw = "cacc" + std::to_string(name_counter++);
                const std::string use = "cget" + std::to_string(name_counter++);
                // pop MUTATES the Vec field, so its receiver must be `mut` (Route-B); get is read-only.
                const bool mut_recv = vecInt;
                const std::string mp = mut_recv ? "mut " : "";
                const std::string body = vecInt
                    ? "pop(p." + f.first + ")"
                    : "get(p." + f.first + ", " + scalar_lit(ft.elems[0]) + ")";
                src += "fn " + raw + "(" + mp + "p: " + sn + ") -> Option[Int] { " + body + " }\n";
                src += "fn " + use + "(" + mp + "p: " + sn + ") -> Int { unwrapOr(" + raw + "(p), 0) }\n";
                funcs.push_back({ use, { struct_ty(s) }, scalar(GType::Int), mut_recv });
            }
        }
        return src;
    }
    // Differentiable native calls (prelude mode only). Deterministic, side-effect-free and
    // message-independent, so they cross-check against the RefEval oracle: the constants MUST match
    // run_diff's NativeEnv (env var SVC_DIFF_ENV=envval; a known-absent path / unset var). Only the
    // STATELESS natives are generated (args/getEnv/fileExists/readFile) -- the stdin natives carry a
    // consumed cursor and stay in the hand-written differential tests.
    std::string gen_native_bool() {
        switch (pick(6)) {
            case 0:  return "fileExists(\"svc_diff_no_such_file_zzz_9x7\")";        // false
            case 1:  return "isErr(readFile(\"svc_diff_no_such_file_zzz_9x7\"))";  // true
            case 2:  return "isOk(readFile(\"svc_diff_no_such_file_zzz_9x7\"))";   // false
            case 3:  return "isSome(getEnv(\"SVC_DIFF_ENV\"))";                    // true
            case 4:  return "isNone(getEnv(\"SVC_DIFF_UNSET_ZZZ\"))";              // true
            default: return "isSome(getEnv(\"SVC_DIFF_UNSET_ZZZ\"))";              // false
        }
    }
    std::string gen_native_string() {
        return chance(50) ? "unwrapOr(getEnv(\"SVC_DIFF_ENV\"), \"def\")"          // "envval"
                          : "unwrapOr(getEnv(\"SVC_DIFF_UNSET_ZZZ\"), \"def\")";   // "def"
    }
    // A non-empty (1..4 lowercase letters) string literal -- so a byte index [0] is always in range.
    std::string nonempty_string_lit() {
        std::string s = "\"";
        const int len = 1 + (int)pick(4);
        for (int i = 0; i < len; ++i) s += (char)('a' + pick(26));
        return s + "\"";
    }
    // A growable VECTOR: `let mut v = vec()` + 2..4 `push`es, then fold its length / an element / a
    // for-iteration into the Int accumulator (exercises vec()/push/len/index/for-over-vec).
    std::string gen_vec_stmt() {
        const std::string v = fresh_name();
        std::string s = "let mut " + v + " = vec()\n";
        const int n = 2 + (int)pick(3);
        for (int i = 0; i < n; ++i) s += "push(" + v + ", " + gen_expr(scalar(GType::Int), 2) + ")\n";
        const std::string acc = mut_var_of(scalar(GType::Int));
        if (acc.empty()) return s;
        switch (pick(3)) {
            case 0: s += acc + " = " + acc + " + len(" + v + ")\n"; break;
            case 1: s += acc + " = " + acc + " + (" + v + ")[0]\n"; break;
            default: { const std::string x = fresh_name();
                       s += "for " + x + " in " + v + " { " + acc + " = " + acc + " + " + x + " }\n"; break; }
        }
        return s;
    }
    // A dynamic MAP `#{ k => v, ... }` (Int keys/values), then fold via len / for-(k,v) / has+index /
    // insert (exercises the map literal, len, for-over-map, has, total m[k] read, and m[k]=v write).
    std::string gen_map_stmt() {
        const std::string m = fresh_name();
        const int n = 2 + (int)pick(2);
        std::vector<std::string> keys;
        std::string lit = "#{ ";
        for (int i = 0; i < n; ++i) {
            if (i) lit += ", ";
            const std::string k = int_lit();
            keys.push_back(k);
            lit += k + " => " + gen_expr(scalar(GType::Int), 2);
        }
        lit += " }";
        std::string s = "let mut " + m + " = " + lit + "\n";
        const std::string acc = mut_var_of(scalar(GType::Int));
        if (acc.empty()) return s;
        switch (pick(4)) {
            case 0: s += acc + " = " + acc + " + len(" + m + ")\n"; break;
            case 1: { const std::string k = fresh_name(), val = fresh_name();
                      s += "for (" + k + ", " + val + ") in " + m + " { " + acc + " = " + acc + " + " + val + " }\n"; break; }
            case 2: s += "if has(" + m + ", " + keys[0] + ") { " + acc + " = " + acc + " + " + m + "[" + keys[0] + "] }\n"; break;
            default: s += m + "[" + int_lit() + "] = 7\n" + acc + " = " + acc + " + len(" + m + ")\n"; break;
        }
        return s;
    }
    // A BYTE buffer via `toBytes(<nonempty>)`, then fold length / byte[0] / for-over-bytes.
    std::string gen_bytes_stmt() {
        const std::string b = fresh_name();
        // Optionally grow b via the bulk appendBytes (String or Bytes source) -> BYTES_APPEND. When we
        // do, `b` is mutated (its destination), so it must be a `mut` binding (Route-B mut-at-call-site).
        const bool grow = (pick(2) == 0);
        std::string s = "let " + std::string(grow ? "mut " : "") + b + " = toBytes(" + nonempty_string_lit() + ")\n";
        if (grow) {
            if (pick(2) == 0) s += "appendBytes(" + b + ", " + nonempty_string_lit() + ")\n";
            else              s += "appendBytes(" + b + ", toBytes(" + nonempty_string_lit() + "))\n";
        }
        const std::string acc = mut_var_of(scalar(GType::Int));
        if (acc.empty()) return s;
        switch (pick(3)) {
            case 0: s += acc + " = " + acc + " + len(" + b + ")\n"; break;
            case 1: s += acc + " = " + acc + " + " + b + "[0]\n"; break;
            default: { const std::string x = fresh_name();
                       s += "for " + x + " in " + b + " { " + acc + " = " + acc + " + " + x + " }\n"; break; }
        }
        return s;
    }
    std::string gen_stmt() {
        const bool can_mut = !mutators.empty() || !mut_methods.empty() || has_inherent_bump();
        switch (pick(12)) {
            case 0: return gen_while();
            case 1: return gen_for();
            case 2: return gen_field_assign();
            case 3: return gen_destructure_let();
            case 4: if (can_mut) return gen_mut_stmt(); return gen_assign();
            case 5: return gen_vec_stmt();
            case 6: return gen_map_stmt();
            case 7: return gen_bytes_stmt();
            case 8: if (can_mut) return gen_mut_stmt(); return gen_assign();   // twice: the method-syntax mutators are rare otherwise
            case 9: if (prelude_mode) return gen_while_let(); return gen_assign();
            case 10: if (prelude_mode) return gen_for_pipeline(); return gen_for();
            default: return gen_assign();
        }
    }

    // Generate a whole program. The compared tail is always a SCALAR binding (the guaranteed
    // `mut` Int accumulator is always available), so the differential comparison is over a
    // well-defined scalar value regardless of how composite the intermediate flow got.
    std::string program() {
        std::string src;
        // Prelude mode emits gated I/O natives (readFile/fileExists -> std::io; getEnv/args ->
        // std::env; see gen_native_bool/gen_native_string/`len(args())`). After the stdlib split
        // those require an explicit `use`; inject it so the sweep stays a pure identity net.
        if (prelude_mode) src += "use std::io::*\nuse std::env::*\n";
        src += gen_range_consts();          // the fixed const table the range bounds may name
        src += gen_structs();
        src += gen_enums();
        src += gen_inherent();
        src += gen_traits();
        src += gen_blankets();
        src += gen_conditional_impls();
        src += gen_generic_fns();
        if (prelude_mode) src += gen_prelude_helpers();
        src += gen_functions();
        src += gen_try_fns();               // `?` inside generated Option/Result fns (prelude only)
        src += gen_wide_fns();
        src += gen_mutation_decls();
        src += gen_container_accessors();   // thin pop/get-through-field accessors (frame-sizing shape)

        const std::string acc = fresh_name();
        src += "let mut " + acc + ": Int = " + gen_expr(scalar(GType::Int), 3) + "\n";
        env.push_back({ acc, scalar(GType::Int), true });
        const int nbind = (int)pick(4) + 1;   // 1..4 more bindings (some mut, some composite)
        for (int i = 0; i < nbind; ++i) {
            const GType t = random_ty();
            const bool mut = chance(40);
            const std::string name = fresh_name();
            src += "let " + std::string(mut ? "mut " : "") + name + ": " + ty_name(t) +
                   " = " + gen_expr(t, 3) + "\n";
            env.push_back({ name, t, mut });
        }

        const int nstmts = (int)pick(5);   // 0..4 statements
        for (int i = 0; i < nstmts; ++i) src += gen_stmt();

        // Phase 3 -- the program VALUE: a bare-identifier read of an in-scope SCALAR binding.
        std::vector<int> scal;
        for (int i = 0; i < (int)env.size(); ++i) if (is_scalar(env[i].ty)) scal.push_back(i);
        return src + env[scal[pick((uint32_t)scal.size())]].name + "\n";
    }
};

std::string Gen::gen_expr(const GType& t, int depth) {
    if (depth <= 0) return leaf(t);

    // Productions available at any type: calls, pipes, generic id, if, match.
    if (has_fn(t) && chance(22)) return call_of(t, depth);
    if (has_pipeable(t) && chance(15)) return gen_pipe(t, depth);
    if (gid_declared && chance(10)) return "gid(" + gen_expr(t, depth - 1) + ")";   // generic identity, any T
    if (has_hof(t) && chance(18)) return call_hof(t, depth);
    if (chance(10)) return gen_match(t, depth);
    if (chance(9))  return gen_or_match(t, depth);
    if (chance(10)) return gen_if(t, depth);
    if (chance(6))  return gen_if_let(t, depth);       // `if let PAT = e { } else [if let ..] { }`
    if (chance(5))  return gen_loop_break(t, depth);   // `loop { .. break <value> }` as an expression

    // Composite types: build them (var / literal), or route through if/match/call above. A container
    // type may also be projected out of a struct field -- `(s).f` -- which is a temp-materializing
    // GET_PROP argument; feeding it to pop/get is the shape that exercises the Option-wrapping builtins'
    // frame sizing (a bare-local container argument does not).
    if (!is_scalar(t)) {
        const int v = var_of(t);
        if (v >= 0 && chance(45)) return env[v].name;
        if (is_container(t) && struct_has_field(t) && chance(40)) return struct_project(t, depth);
        if (t.kind == GType::Struct && chance(25)) {   // `S::new(..)` -- an associated-fn construction
            const std::string assoc = assoc_new_call(t.sid, depth - 1);
            if (!assoc.empty()) return assoc;
        }
        return ctor(t, depth);
    }

    // Container consumers yielding a scalar (order-INDEPENDENT so the VM and the oracle agree regardless
    // of the map's hash order): len (any container -> Int), pop/get (prelude -> Option -> Int), has (map
    // membership -> Bool). pop/get take a container EXPRESSION that may be a struct-field projection.
    if (t.kind == GType::Int && chance(12)) return "len(" + gen_expr(random_container(), depth - 1) + ")";
    if (t.kind == GType::Int && prelude_mode && chance(14)) return gen_container_int(depth);
    if (t.kind == GType::Bool && chance(9)) {
        const GType k = random_map_key();
        return "has(" + gen_expr(map_ty(k, random_scalar()), depth - 1) + ", " + scalar_lit(k) + ")";
    }

    // Scalar-only projections out of composites.
    if (has_newtype_under(t) && chance(15)) return newtype_project(t, depth);   // `(N(e)).0` -- erased read
    if (struct_has_field(t) && chance(20)) return struct_project(t, depth);
    if (chance(12)) return tuple_project(t, depth);
    if (trait_returns(t) && chance(20)) return trait_call(t, depth);
    if (t.kind == GType::Int && !conds.empty() && chance(20)) return cond_call(depth);
    if (inherent_returns(t) && chance(20)) return inherent_call(t, depth);
    if (has_enum() && chance(18)) return gen_enum_match(t, depth);
    if (has_struct() && chance(12)) return gen_struct_match(t, depth);
    if (has_struct() && chance(9)) return gen_nested_match(t, depth);
    if (chance(9)) return gen_guard_match(t, depth);
    if (chance(9)) return gen_range_match(t, depth);           // `match <int> { lo..hi => t, _ => t }`
    if (chance(8)) return gen_at_match(t, depth);              // `match <int> { a @ lo..hi => t, _ => t }`
    if (!newtypes.empty() && chance(9)) return gen_newtype_match(t, depth);   // `(N(a), b)` -- NESTED erasure
    if (t.kind == GType::Int && prelude_mode && chance(9))
        return gen_or_bind_match(depth);                       // `Ok(a) | Err(a) => a + ...`
    if (gtw_declared && chance(8)) return gen_gtw(t, depth);   // gtw(lambda, arg): apply twice (scalar T)
    if (t.kind == GType::Int && prelude_mode && chance(20)) return gen_prelude_int(depth);
    if (t.kind == GType::Int && prelude_mode && chance(6)) return "len(args())";   // differentiable native
    if (t.kind == GType::Int && prelude_mode && chance(12)) return gen_pipeline_int(depth);   // src |> stages |> terminal
    if (t.kind == GType::Bool && prelude_mode && chance(8)) return gen_pipeline_bool(depth);
    if (t.kind == GType::Int && chance(3)) return gen_deep_chain(depth);                     // 10..20 links
    if (chance(8)) return tuple_match_project(t, depth);

    switch (t.kind) {
        case GType::Int:
            switch (pick(7)) {
                case 0: case 1: return leaf(t);
                case 2: {   // arithmetic / bitwise / shift -- all total (no int / % to avoid div-by-0)
                    static const char* ops[] = { "+", "-", "*", "&", "|", "^", "<<", ">>", ">>>" };
                    const char* op = ops[pick(9)];
                    return "(" + gen_expr(t, depth - 1) + " " + op + " " + gen_expr(t, depth - 1) + ")";
                }
                case 3: return "(-" + gen_expr(t, depth - 1) + ")";
                case 4: return "(~" + gen_expr(t, depth - 1) + ")";
                case 5:  return "len(array(" + std::to_string(pick(5) + 1) + ", " + gen_expr(t, depth - 1) + "))";
                default: return gen_if(t, depth);
            }
        case GType::Double:
            switch (pick(6)) {
                case 0: case 1: return leaf(t);
                case 2: {   // + - * /  -- double / 0.0 is IEEE inf/NaN, no trap
                    static const char* ops[] = { "+", "-", "*", "/" };
                    const char* op = ops[pick(4)];
                    return "(" + gen_expr(t, depth - 1) + " " + op + " " + gen_expr(t, depth - 1) + ")";
                }
                case 3: return "(-" + gen_expr(t, depth - 1) + ")";
                default: return gen_if(t, depth);
            }
        case GType::Bool:
            if (prelude_mode && chance(12)) return gen_native_bool();   // differentiable native
            switch (pick(7)) {
                case 0: return leaf(t);
                case 1: return "(!" + gen_expr(t, depth - 1) + ")";
                case 2: return "(" + gen_expr(t, depth - 1) + " && " + gen_expr(t, depth - 1) + ")";
                case 3: return "(" + gen_expr(t, depth - 1) + " || " + gen_expr(t, depth - 1) + ")";
                case 4: case 5: return gen_compare(depth);
                default: return gen_if(t, depth);
            }
        default:  // String
            if (prelude_mode && chance(12)) return gen_native_string();   // differentiable native
            if (depth > 0 && chance(22)) return interp_string(depth);      // `"…${e}…"` interpolation
            switch (pick(6)) {
                case 0: case 1: return leaf(t);
                case 2: case 3: return "(" + gen_expr(t, depth - 1) + " + " + gen_expr(t, depth - 1) + ")";
                case 4: {   // Java-style mixed concat: String + a scalar (Int/Double/Bool), either order
                    const GType s = random_scalar();
                    return chance(50) ? "(" + gen_expr(t, depth - 1) + " + " + gen_expr(s, depth - 1) + ")"
                                      : "(" + gen_expr(s, depth - 1) + " + " + gen_expr(t, depth - 1) + ")";
                }
                default: return gen_if(t, depth);
            }
    }
}

}  // namespace

std::string generate(uint32_t seed, bool with_prelude) {
    Gen g(seed);
    g.prelude_mode = with_prelude;
    return g.program();
}

}  // namespace gen
