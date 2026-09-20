// =============================================================================
// RefEval.cpp -- the tree-walking reference interpreter (oracle). See RefEval.h.
//
// Covers the pure, deterministic language surface: literals + operators (48-bit
// wrapping Int, Int->Double promotion, IEEE compares), control flow, let/mut/assign,
// top-level fns + recursion; structs / enums / tuples / lists / arrays / vecs / bytes /
// maps, field + index access & assign, `match` over the full pattern set, `for`;
// lambdas + by-value capture, first-class fn values, `|>`, dynamic trait dispatch
// (+ defaults, unqualified & `Trait::m`), the `?` operator, and the opcode-backed
// builtins (array/vec/push/pop/len/get/has/delete/keys/values/toBytes/fromBytes/
// bytes/toString/print/println/panic). A program the oracle declines raises one of the
// two signals below -- `Unsupported` (by design, an honest skip) or `NotModelled` (a gap,
// a hard failure). See the RefEval.h header comment for why the two are kept apart.
// =============================================================================

#include "RefEval.h"
#include "NativeRegistry.h"   // native_id_of -- tells a deliberately excluded native from a forgotten
                              // builtin at the one call site where both land (see call_named). The
                              // header is name-table only; it pulls in no VM-runtime dependency.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>

using namespace svc;

namespace refeval {
namespace {

// ----- control-flow signals (thrown, not returned) --------------------------
struct ReturnSignal { RtValue value; };
// A `break` carries a value only inside a `loop` (the checker rejects it elsewhere), so the
// while/for handlers simply ignore it.
struct BreakSignal { RtValue value; bool has_value = false; };
struct ContinueSignal {};
struct PanicSignal   { std::string msg; };
// The two ways of declining a program. See the RefEval.h header comment for WHY they are distinct:
// one silent category made the oracle fail by silence. `Unsupported` must be justified at its throw
// site (there are only three such sites); every other decline is a gap and must be loud.
struct Unsupported   { std::string what; };   // outside the oracle's remit BY DESIGN -> an honest SKIP
struct NotModelled   { std::string what; };   // the oracle LACKS this -> a FAILURE, like a disagreement

const char SEP = '\x1f';   // key separator for the trait tables

// ----- numeric core (mirrors the committed semantics) -----------------------
int64_t wrap48(int64_t x) {
    uint64_t u = static_cast<uint64_t>(x) & 0xFFFFFFFFFFFFull;
    if (u & 0x800000000000ull) u |= 0xFFFF000000000000ull;   // sign-extend bit 47
    return static_cast<int64_t>(u);
}
// The Int-only operator family. `eval_binary` spells it as a case list; the compound-assignment
// path needs it as a predicate, so it lives here once (mirrors the checker's `is_bitwise_op`).
bool is_bitwise(TokKind op) {
    switch (op) {
        case TokKind::BitAnd: case TokKind::BitOr: case TokKind::BitXor:
        case TokKind::Shl:    case TokKind::Shr:   case TokKind::UShr:
            return true;
        default:
            return false;
    }
}
bool   is_int(const RtValue& v)    { return std::holds_alternative<int64_t>(v); }
bool   is_double(const RtValue& v) { return std::holds_alternative<double>(v); }
bool   is_num(const RtValue& v)    { return is_int(v) || is_double(v); }
double as_double(const RtValue& v) { return is_int(v) ? static_cast<double>(std::get<int64_t>(v))
                                                      : std::get<double>(v); }
bool is_obj(const RtValue& v) { return std::holds_alternative<std::shared_ptr<Obj>>(v); }
const std::shared_ptr<Obj>& as_obj(const RtValue& v) { return std::get<std::shared_ptr<Obj>>(v); }

// ----- type-directed Int->Double coercion at N1 boundaries ------------------
bool syn_is_double(const Type* t) {
    return t && t->kind == TypeKind::Named && static_cast<const NamedType*>(t)->name == "Double";
}
bool ty_is_double(const TyPtr& t) { return t && t->kind == TyKind::Double; }

RtValue coerce_dbl(RtValue v, bool want_double) {
    if (want_double && is_int(v)) return static_cast<double>(std::get<int64_t>(v));
    return v;
}

} // namespace

// ===========================================================================
// canonicalization (shared with the VM-side converter in main.cpp)
// ===========================================================================

std::string canon_double(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    // The VM normalizes -0.0 -> 0.0 at every Value construction (Value::fromDouble, a committed
    // invariant), so it never holds a negative zero. The oracle keeps raw C++ doubles, where
    // -(x - x) yields -0.0; canonicalize both zeros to "0.0" to mirror the VM.
    if (d == 0.0) return "0.0";
    char buf[40];
    auto r = std::to_chars(buf, buf + sizeof(buf), d);
    std::string s(buf, r.ptr);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos)
        s += ".0";
    return s;
}

namespace {
// The last "::"-segment of a (possibly module-mangled) name -- e.g. std::core::Some -> "Some".
// RefEval compares/canonicalizes value type/enum names by their SHORT form so they match the VM's
// display name (Assembler::define_struct strips the prefix identically). Bare names are unchanged.
std::string short_name(const std::string& n) {
    auto pos = n.rfind("::");
    return pos == std::string::npos ? n : n.substr(pos + 2);
}
std::string quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
    o += '"';
    return o;
}

std::string canon_obj(const Obj& o) {
    std::string s;
    auto join_items = [&](const char* open, const char* close) {
        s += open;
        for (size_t i = 0; i < o.items.size(); ++i) { if (i) s += ","; s += canonicalize(o.items[i]); }
        s += close;
    };
    switch (o.kind) {
    case ObjKind::Struct:
        if (o.items.empty()) return o.type_name;
        s = o.type_name; join_items("(", ")"); return s;
    case ObjKind::Tuple:  join_items("(", ")"); return s;
    case ObjKind::List:
    case ObjKind::Array:
    case ObjKind::Vec:    join_items("[", "]"); return s;
    case ObjKind::Bytes: {
        s = "b[";
        for (size_t i = 0; i < o.bytes.size(); ++i) { if (i) s += ","; s += std::to_string(o.bytes[i]); }
        s += "]"; return s;
    }
    case ObjKind::Map: {
        std::vector<std::string> entries;
        entries.reserve(o.map.size());
        for (const auto& kv : o.map) entries.push_back(canonicalize(kv.first) + "=>" + canonicalize(kv.second));
        std::sort(entries.begin(), entries.end());   // order-insensitive
        s = "#{";
        for (size_t i = 0; i < entries.size(); ++i) { if (i) s += ","; s += entries[i]; }
        s += "}"; return s;
    }
    }
    return "?";
}

// Byte-for-byte mirror of the VM's TO_STRING dump (vmcore/opcodes/op_string.h) -- the graded-(c)
// oracle for string interpolation (`"${x}"`) and toString/print of a container. `field` = false is
// the TOP-LEVEL rendering (a String is IDENTITY, unquoted -- to_string_value); `field` = true is a
// NESTED element/field (a String is RAW-QUOTED, no escaping -- append_field_value); containers always
// recurse with field=true. `depth` mirrors TO_STRING_MAX_DEPTH=4 elision. A **Map** cannot be mirrored
// (the VM dumps it in hash/backing order, unreproducible here) -> Unsupported, so the harness SKIPS a
// map-in-a-hole rather than reporting a spurious mismatch (the generator must not emit one).
std::string render_dump(const RtValue& v, bool field, int depth) {
    switch (v.index()) {
    case 0: return "nil";                                                  // monostate = nil / empty-list
    case 1: return std::to_string(std::get<int64_t>(v));                   // Int    (format_number)
    case 2: return canon_double(std::get<double>(v));                      // Double (format_number)
    case 3: return std::get<bool>(v) ? "true" : "false";
    case 4: { const std::string& s = std::get<std::string>(v);            // String
              return field ? "\"" + s + "\"" : s; }                        // nested: raw-quoted (VM does NOT escape)
    case 5: return ":" + std::get<Atom>(v).name;                           // Atom -> :name
    case 7: return "<fn>";                                                 // closure placeholder
    default: break;                                                        // case 6: heap object
    }
    const Obj& o = *std::get<std::shared_ptr<Obj>>(v);
    const size_t n = o.items.size();
    auto join = [&](const char* open, const char* close) {
        std::string s = open;
        for (size_t i = 0; i < n; ++i) { if (i) s += ", "; s += render_dump(o.items[i], true, depth + 1); }
        s += close; return s;
    };
    switch (o.kind) {
    case ObjKind::Map:
        // FAMILY A (by design): the VM dumps a map in hash/backing order, which this walker's
        // insertion-ordered entries cannot reproduce -- so it is not a gap to close, and stays a skip.
        throw Unsupported{"toString of a Map (hash-order dump not mirrored)"};
    case ObjKind::Bytes: {                                                 // `#b[DE AD]`, uppercase hex; no depth guard
        static const char* HEX = "0123456789ABCDEF";
        std::string s = "#b[";
        for (size_t i = 0; i < o.bytes.size(); ++i) {
            if (i) s += ' ';
            const unsigned b = o.bytes[i];
            s += HEX[b >> 4]; s += HEX[b & 0xF];
        }
        return s + "]";
    }
    case ObjKind::Array:
    case ObjKind::Vec:
        if (n == 0)      return "[]";
        if (depth >= 4)  return "[ ... ]";
        return join("[", "]");
    case ObjKind::List:                                                    // non-empty (empty list = monostate -> "nil")
        if (depth >= 4)  return "[ ... ]";                                 // append_list_dump checks depth first
        return join("[", "]");
    case ObjKind::Tuple: {
        if (n == 0)      return "()";
        if (depth >= 4)  return "(...)";
        std::string s = "(";
        for (size_t i = 0; i < n; ++i) { if (i) s += ", "; s += render_dump(o.items[i], true, depth + 1); }
        if (n == 1) s += ",";                                              // (e0,)
        return s + ")";
    }
    case ObjKind::Struct:
        // register_struct_shape: is_tuple ? NamedTuple `Name(e)` : Struct `Name { f: v }` (enum-agnostic).
        if (o.is_tuple) {
            if (n == 0)     return o.type_name;                            // nullary -> just the name (e.g. None)
            if (depth >= 4) return o.type_name + "(...)";
            return o.type_name + join("(", ")");
        }
        if (n == 0)     return o.type_name;                                // zero-field struct -> just the name
        if (depth >= 4) return o.type_name + " { ... }";
        {
            std::string s = o.type_name + " { ";
            for (size_t i = 0; i < n; ++i) {
                if (i) s += ", ";
                s += (i < o.field_names.size() ? o.field_names[i] : std::string("?"));
                s += ": ";
                s += render_dump(o.items[i], true, depth + 1);
            }
            return s + " }";
        }
    }
    throw NotModelled{"toString of an unmodelled value"};
}
} // namespace

std::string canonicalize(const RtValue& v) {
    switch (v.index()) {
    case 0: return "unit";
    case 1: return std::to_string(std::get<int64_t>(v));
    case 2: return canon_double(std::get<double>(v));
    case 3: return std::get<bool>(v) ? "true" : "false";
    case 4: return quote(std::get<std::string>(v));
    case 5: return ":" + std::get<Atom>(v).name;
    case 6: return canon_obj(*std::get<std::shared_ptr<Obj>>(v));
    case 7: return "<fn>";
    }
    return "?";
}

// ===========================================================================
// the interpreter
// ===========================================================================
namespace {

struct CtorInfo {
    std::string type_name;
    std::string enum_name;                     // "" for a struct
    std::vector<std::string> field_names;      // "_0".. for tuple/positional
    std::vector<const Type*> field_types;
    bool is_variant = false;
    bool is_tuple   = false;                    // positional fields -> `Name(e)` dump vs `Name { f: v }`
    bool is_transparent = false;                // erased newtype: the value IS its single field's immediate
    bool is_int_enum = false;                   // int-backed enum variant: the value IS the Int discriminant
    int64_t disc = 0;                           // its runtime discriminant (valid iff is_int_enum)
};

class Interp {
public:
    Interp(const Program& prog, const NativeEnv& nenv) : nenv_(nenv) { collect(prog); }

    RunResult run() {
        RunResult res;
        try {
            Env top;
            RtValue last;
            for (const Stmt* s : top_stmts_) last = exec_stmt(*s, top);
            res.value = last;
        } catch (const PanicSignal& p)   { res.faulted = true; res.fault_msg = p.msg; }
          catch (const Unsupported& u)   { res.faulted = true; res.unsupported = true; res.fault_msg = u.what; }
          catch (const NotModelled& u)   { res.faulted = true; res.oracle_gap  = true; res.fault_msg = u.what; }
          catch (const ReturnSignal&)    { res.faulted = true; res.fault_msg = "return at top level"; }
          catch (const BreakSignal&)     { res.faulted = true; res.fault_msg = "break at top level"; }
          catch (const ContinueSignal&)  { res.faulted = true; res.fault_msg = "continue at top level"; }
          catch (const std::exception& e){ res.faulted = true; res.fault_msg = e.what(); }
        res.output = std::move(out_);
        res.coverage = cov_;   // what this run evaluated; the caller merges it only if the run AGREED
        return res;
    }

private:
    Coverage cov_;   // node kinds evaluated by this run (see RefEval.h)
    std::unordered_map<std::string, const FnItem*> fn_table_;
    std::unordered_map<std::string, const Expr*>   const_defs_;   // module const -> literal (S0; inlined)
    std::unordered_map<std::string, CtorInfo>      ctors_;
    // Int-backed enum SHORT type name -> (disc, short variant name), for the static-type-directed
    // toString name recovery (mirrors Codegen::int_enum_variants_).
    std::unordered_map<std::string, std::vector<std::pair<int64_t, std::string>>> int_enum_names_;
    std::unordered_map<std::string, std::string>   method_owner_;   // method -> trait, "" if ambiguous
    // The only trait declaring `method`, or "" (none, or several). The fallback for a trait-method call
    // the checker recorded no `resolved_trait` for.
    std::string owner_of(const std::string& method) const {
        const auto mo = method_owner_.find(method);
        return mo != method_owner_.end() ? mo->second : std::string();
    }
    std::unordered_map<std::string, std::unordered_map<std::string, const Method*>> inherent_;  // head -> method -> fn (S2)
    std::unordered_map<std::string, std::unordered_map<std::string, const Method*>> impls_;    // trait\x1fhead -> {method -> Method*}
    std::unordered_map<std::string, std::unordered_map<std::string, const Method*>> blanket_;  // trait -> {method -> Method*}
    std::unordered_map<std::string, const Method*> defaults_;        // trait\x1fmethod -> default Method*
    std::vector<const Stmt*> top_stmts_;
    std::string out_;
    NativeEnv    nenv_;             // fixed fixtures for the differentiable natives
    std::size_t  stdin_pos_ = 0;    // shared cursor into nenv_.stdin_text (readLine / readAllStdin)

    // ----- collect top-level declarations -----
    void collect(const Program& prog) {
        for (const auto& it : prog.items) {
            switch (it->kind) {
            case ItemKind::Fn:
                fn_table_[static_cast<const FnItem&>(*it).name] = static_cast<const FnItem*>(it.get());
                break;
            case ItemKind::Const:
                const_defs_[static_cast<const ConstItem&>(*it).name] = static_cast<const ConstItem&>(*it).value.get();
                break;
            case ItemKind::Struct: {
                const auto& si = static_cast<const StructItem&>(*it);
                CtorInfo c; c.type_name = short_name(si.name); c.is_tuple = si.is_tuple;   // SHORT name (VM display)
                c.is_transparent = si.is_transparent;
                for (const auto& f : si.fields) { c.field_names.push_back(f.name); c.field_types.push_back(f.type.get()); }
                ctors_[si.name] = std::move(c);                   // keyed by the full mangled name (lookup)
                break;
            }
            case ItemKind::Enum: {
                const auto& ei = static_cast<const EnumItem&>(*it);
                int64_t next_disc = 0;   // int-backed discriminants: same rule as the checker / codegen
                for (const auto& v : ei.variants) {
                    CtorInfo c; c.type_name = short_name(v.name); c.enum_name = short_name(ei.name);
                    c.is_variant = true; c.is_tuple = v.is_tuple;
                    for (const auto& f : v.fields) { c.field_names.push_back(f.name); c.field_types.push_back(f.type.get()); }
                    if (ei.is_int_backed) {
                        const int64_t d = v.has_disc ? v.disc : next_disc;
                        next_disc = d + 1;
                        c.is_int_enum = true; c.disc = d;
                        int_enum_names_[short_name(ei.name)].emplace_back(d, short_name(v.name));
                    }
                    ctors_[v.name] = std::move(c);   // keyed by the full mangled variant name
                }
                break;
            }
            case ItemKind::Trait: {
                const auto& td = static_cast<const TraitDecl&>(*it);
                const std::string tn = short_name(td.name);   // trait tables keyed by SHORT trait name
                for (const auto& mth : td.methods) {
                    if (mth.body) defaults_[tn + SEP + mth.name] = &mth;
                    auto ins = method_owner_.emplace(mth.name, tn);
                    if (!ins.second && ins.first->second != tn) ins.first->second.clear();   // ambiguous
                }
                break;
            }
            case ItemKind::Impl: {
                const auto& im = static_cast<const ImplDecl&>(*it);
                const std::string head = short_name(target_head(im));   // short trait + short head keys
                if (im.is_inherent) {                                    // S2: inherent methods keyed by head
                    for (const auto& mth : im.methods) inherent_[head][mth.name] = &mth;
                    break;
                }
                const std::string tn   = short_name(im.trait_name);
                for (const auto& mth : im.methods) {
                    if (head.empty()) blanket_[tn][mth.name] = &mth;
                    else              impls_[tn + SEP + head][mth.name] = &mth;
                }
                break;
            }
            case ItemKind::Stmt:
                top_stmts_.push_back(static_cast<const StmtItem*>(it.get())->stmt.get());
                break;
            }
        }
    }

    // Head-type name of an impl's target ("" if it is a blanket impl over a generic param).
    static std::string target_head(const ImplDecl& im) {
        if (!im.target || im.target->kind != TypeKind::Named) return "";
        const std::string& n = static_cast<const NamedType&>(*im.target).name;
        for (const auto& g : im.generics) if (g.name == n) return "";   // `impl[T] Tr for T`
        return n;
    }

    // ----- env lookup -----
    static RtValue* find(Env& env, const std::string& name) {
        for (Env* e = &env; e; e = e->parent) {
            auto it = e->vars.find(name);
            if (it != e->vars.end()) return &it->second;
        }
        return nullptr;
    }

    // ----- statements -----
    RtValue exec_stmt(const Stmt& s, Env& env) {
        cov_.mark_stmt(s.kind);
        switch (s.kind) {
        case StmtKind::Let: {
            const auto& l = static_cast<const LetStmt&>(s);
            RtValue v = l.init ? coerce_dbl(eval(*l.init, env), syn_is_double(l.type.get())) : RtValue{};
            bind_pattern(*l.pat, std::move(v), env);
            return RtValue{};
        }
        case StmtKind::Assign: {
            const auto& a = static_cast<const AssignStmt&>(s);
            // `a.op` is the BASE operator (the parser maps `+=` to `Plus`), so the two dimensions are
            // needed to tell `x = x + 1` from `x += 1`: `op` records the operator, `compound` records
            // that it arrived in compound form.
            cov_.mark_op(a.op);
            if (a.op != TokKind::Assign) { cov_.mark_compound(a.op); compound_assign(a, env); return RtValue{}; }
            RtValue v = coerce_dbl(eval(*a.value, env), a.target ? ty_is_double(a.target->ty) : false);
            assign(*a.target, std::move(v), env);
            return RtValue{};
        }
        case StmtKind::Expr:
            return eval(*static_cast<const ExprStmt&>(s).expr, env);
        }
        return RtValue{};
    }

    // `target op= value`. The place is evaluated exactly ONCE, mirroring codegen's read-modify-write
    // -- the receiver / container / index sub-expressions must not run twice, or the oracle would
    // model a different program than the bytecode whenever they have side effects. The map read uses
    // the same total-or-trap semantics as `m[k]`, so a compound assign to an ABSENT key aborts.
    void compound_assign(const AssignStmt& a, Env& env) {
        const Expr& target = *a.target;
        const bool dbl = ty_is_double(target.ty);
        // The bitwise/shift forms (`&=` ... `>>>=`) must route to `bitwise`, not `arith`: `arith` has
        // no cases for them and would fall through to its operand guard, so the oracle would stop
        // checking every program that uses them (the failure mode recorded for the old reference-`==`
        // bug below -- and the case that motivated splitting the decline signal in two: that guard is
        // now `NotModelled`, so this mistake fails the sweep instead of quietly lowering coverage).
        // A bitwise target is always Int, so `dbl` is false.
        auto combine = [&](const RtValue& old) {
            RtValue rhs = eval(*a.value, env);
            return is_bitwise(a.op) ? bitwise(a.op, old, rhs)
                                    : coerce_dbl(arith(a.op, old, rhs), dbl);
        };
        if (target.kind == ExprKind::Ident) {
            RtValue* slot = find(env, static_cast<const IdentExpr&>(target).name);
            if (!slot) throw NotModelled{"assign to unknown binding"};
            *slot = combine(*slot);
            return;
        }
        if (target.kind == ExprKind::Field) {
            const auto& fe = static_cast<const FieldExpr&>(target);
            RtValue o = eval(*fe.obj, env);                       // once
            if (!is_obj(o)) throw NotModelled{"field assign on a non-object"};
            const size_t idx = field_index(*as_obj(o), fe);
            as_obj(o)->items[idx] = combine(as_obj(o)->items[idx]);
            return;
        }
        if (target.kind == ExprKind::Index) {
            const auto& ix = static_cast<const IndexExpr&>(target);
            RtValue o = eval(*ix.obj, env);                       // once
            RtValue key = eval(*ix.index, env);                   // once
            if (is_obj(o) && as_obj(o)->kind == ObjKind::Map) {
                for (auto& e : as_obj(o)->map)
                    if (eq(e.first, key)) { e.second = combine(e.second); return; }
                throw PanicSignal{"map key not found"};           // total-or-trap, as `m[k]`
            }
            const int64_t i = as_int(key);
            if (is_obj(o)) {
                Obj& ob = *as_obj(o);
                if (ob.kind == ObjKind::Bytes) {
                    if (i < 0 || i >= (int64_t)ob.bytes.size()) throw PanicSignal{"index out of bounds"};
                    store_index(o, i, combine((int64_t)ob.bytes[(size_t)i]));
                    return;
                }
                if (i < 0 || i >= (int64_t)ob.items.size()) throw PanicSignal{"index out of bounds"};
                store_index(o, i, combine(ob.items[(size_t)i]));
                return;
            }
            throw PanicSignal{"index out of bounds"};
        }
        throw NotModelled{"compound assign to this lvalue"};
    }

    void assign(const Expr& target, RtValue v, Env& env) {
        if (target.kind == ExprKind::Ident) {
            RtValue* slot = find(env, static_cast<const IdentExpr&>(target).name);
            if (!slot) throw NotModelled{"assign to unknown binding"};
            *slot = std::move(v); return;
        }
        if (target.kind == ExprKind::Field) {
            const auto& fe = static_cast<const FieldExpr&>(target);
            RtValue o = eval(*fe.obj, env);
            if (!is_obj(o)) throw NotModelled{"field assign on a non-object"};
            as_obj(o)->items[field_index(*as_obj(o), fe)] = std::move(v); return;
        }
        if (target.kind == ExprKind::Index) {
            const auto& ix = static_cast<const IndexExpr&>(target);
            RtValue o = eval(*ix.obj, env);
            if (is_obj(o) && as_obj(o)->kind == ObjKind::Map) {   // m[k] = v -- insert/update
                map_set(*as_obj(o), eval(*ix.index, env), std::move(v)); return;
            }
            int64_t i = as_int(eval(*ix.index, env));
            store_index(o, i, std::move(v)); return;
        }
        throw NotModelled{"assign to this lvalue"};
    }

    // ----- expressions -----
    RtValue eval(const Expr& e, Env& env) {
        // The five `cov_` probes sit at the oracle's DISPATCH points (here, exec_stmt, match_pattern,
        // eval_unary, eval_binary) -- one per node-kind category, so nothing can be evaluated without
        // being recorded. Marking on ENTRY, before the construct can throw, is harmless: a run that
        // throws never reaches Agree, and only an agreeing run's record is merged (see RefEval.h).
        cov_.mark_expr(e.kind);
        switch (e.kind) {
        case ExprKind::IntLit:    return static_cast<const IntLit&>(e).value;
        case ExprKind::DoubleLit: return static_cast<const DoubleLit&>(e).value;
        case ExprKind::BoolLit:   return static_cast<const BoolLit&>(e).value;
        case ExprKind::StrLit:    return static_cast<const StrLit&>(e).value;
        case ExprKind::Ident:     return eval_ident(static_cast<const IdentExpr&>(e), env);
        case ExprKind::Unary:     return eval_unary(static_cast<const UnaryExpr&>(e), env);
        case ExprKind::Binary:    return eval_binary(static_cast<const BinaryExpr&>(e), env);
        case ExprKind::If: {
            const auto& f = static_cast<const IfExpr&>(e);
            if (as_bool(eval(*f.cond, env))) return eval(*f.then_blk, env);
            if (f.else_blk) return eval(*f.else_blk, env);
            return RtValue{};
        }
        case ExprKind::While: {
            const auto& w = static_cast<const WhileExpr&>(e);
            while (as_bool(eval(*w.cond, env))) {
                try { eval(*w.body, env); }
                catch (const BreakSignal&) { break; }
                catch (const ContinueSignal&) { continue; }
            }
            return RtValue{};
        }
        case ExprKind::For:       return eval_for(static_cast<const ForExpr&>(e), env);
        case ExprKind::Loop: {
            // No fall-through exit: the only way out is a `break` (or a return/panic unwinding
            // past it), so the loop's value is exactly the break's value (unit if valueless).
            const auto& l = static_cast<const LoopExpr&>(e);
            for (;;) {
                try { if (l.body) eval(*l.body, env); }
                catch (const BreakSignal& b) { return b.has_value ? b.value : RtValue{}; }
                catch (const ContinueSignal&) { continue; }
            }
        }
        case ExprKind::Block: {
            const auto& b = static_cast<const BlockExpr&>(e);
            Env child; child.parent = &env;
            RtValue last;
            for (const auto& s : b.stmts) last = exec_stmt(*s, child);
            return last;
        }
        case ExprKind::Call:      return eval_call(static_cast<const CallExpr&>(e), env);
        case ExprKind::Pipe:      return eval_pipe(static_cast<const PipeExpr&>(e), env);
        case ExprKind::Field:     return eval_field(static_cast<const FieldExpr&>(e), env);
        case ExprKind::Index:     return eval_index(static_cast<const IndexExpr&>(e), env);
        case ExprKind::StructLit: return eval_struct_lit(static_cast<const StructLit&>(e), env);
        case ExprKind::Tuple:     return eval_tuple(static_cast<const TupleExpr&>(e), env);
        case ExprKind::ListLit:   return eval_list(static_cast<const ListLit&>(e), env);
        case ExprKind::MapLit:    return eval_map(static_cast<const MapLit&>(e), env);
        case ExprKind::Match:     return eval_match(static_cast<const MatchExpr&>(e), env);
        case ExprKind::Lambda:    return eval_lambda(static_cast<const LambdaExpr&>(e), env);
        case ExprKind::Try:       return eval_try(static_cast<const TryExpr&>(e), env);
        case ExprKind::Return:    throw ReturnSignal{ static_cast<const ReturnExpr&>(e).value
                                                        ? eval(*static_cast<const ReturnExpr&>(e).value, env)
                                                        : RtValue{} };
        case ExprKind::Break: {
            const auto& br = static_cast<const BreakExpr&>(e);
            if (br.value) throw BreakSignal{ eval(*br.value, env), true };
            throw BreakSignal{};
        }
        case ExprKind::Continue:  throw ContinueSignal{};
        default:                  throw NotModelled{"expression kind not modelled"};
        }
    }

    RtValue eval_ident(const IdentExpr& id, Env& env) {
        if (!id.qualifier.empty()) throw NotModelled{"qualified trait method as a value"};
        if (RtValue* slot = find(env, id.name)) return *slot;
        if (auto kit = const_defs_.find(id.name); kit != const_defs_.end()) return eval(*kit->second, env);
        auto fit = fn_table_.find(id.name);
        if (fit != fn_table_.end()) { auto c = std::make_shared<Closure>(); c->fn = fit->second; return c; }
        auto cit = ctors_.find(id.name);
        if (cit != ctors_.end() && cit->second.field_names.empty()) return construct(cit->second, {});
        throw NotModelled{"unresolved identifier '" + id.name + "'"};
    }

    static bool as_bool(const RtValue& v) {
        if (std::holds_alternative<bool>(v)) return std::get<bool>(v);
        throw NotModelled{"non-Bool condition"};
    }
    static int64_t as_int(const RtValue& v) {
        if (is_int(v)) return std::get<int64_t>(v);
        throw NotModelled{"expected an Int"};
    }

    RtValue eval_unary(const UnaryExpr& u, Env& env) {
        cov_.mark_op(u.op);
        RtValue x = eval(*u.operand, env);
        switch (u.op) {
        case TokKind::Minus:
            if (is_int(x)) return wrap48(-std::get<int64_t>(x));
            if (is_double(x)) return -std::get<double>(x);
            break;
        case TokKind::Not:
            if (std::holds_alternative<bool>(x)) return !std::get<bool>(x);
            break;
        case TokKind::Tilde:
            if (is_int(x)) return wrap48(std::get<int64_t>(x) ^ -1);
            break;
        default: break;
        }
        throw NotModelled{"unary operator on this operand"};
    }

    RtValue eval_binary(const BinaryExpr& b, Env& env) {
        cov_.mark_op(b.op);
        if (b.op == TokKind::AndAnd) return as_bool(eval(*b.lhs, env)) ? eval(*b.rhs, env) : RtValue{ false };
        if (b.op == TokKind::OrOr)   return as_bool(eval(*b.lhs, env)) ? RtValue{ true } : eval(*b.rhs, env);
        RtValue a = eval(*b.lhs, env), c = eval(*b.rhs, env);
        switch (b.op) {
        case TokKind::Plus:
            if (std::holds_alternative<std::string>(a) || std::holds_alternative<std::string>(c)) return concat(a, c);
            return arith(b.op, a, c);
        case TokKind::Minus: case TokKind::Star: case TokKind::Slash: case TokKind::Percent:
            return arith(b.op, a, c);
        case TokKind::BitAnd: case TokKind::BitOr: case TokKind::BitXor:
        case TokKind::Shl: case TokKind::Shr: case TokKind::UShr:
            return bitwise(b.op, a, c);
        case TokKind::EqEq:  return equal(a, c);
        case TokKind::NotEq: return !std::get<bool>(equal(a, c));
        case TokKind::Lt: case TokKind::Le: case TokKind::Gt: case TokKind::Ge:
            return compare(b.op, a, c);
        default: throw NotModelled{"binary operator not modelled"};
        }
    }

    RtValue arith(TokKind op, const RtValue& a, const RtValue& c) {
        if (is_double(a) || is_double(c)) {
            double x = as_double(a), y = as_double(c);
            switch (op) {
            case TokKind::Plus:  return x + y;   case TokKind::Minus: return x - y;
            case TokKind::Star:  return x * y;   case TokKind::Slash: return x / y;
            case TokKind::Percent: return std::fmod(x, y);
            default: break;
            }
        } else if (is_int(a) && is_int(c)) {
            int64_t x = std::get<int64_t>(a), y = std::get<int64_t>(c);
            switch (op) {
            case TokKind::Plus:  return wrap48(x + y);   case TokKind::Minus: return wrap48(x - y);
            case TokKind::Star:  return wrap48(x * y);
            case TokKind::Slash: if (y == 0) throw PanicSignal{"Division by zero"}; return wrap48(x / y);
            case TokKind::Percent: if (y == 0) throw PanicSignal{"Division by zero"}; return wrap48(x % y);
            default: break;
            }
        }
        throw NotModelled{"arithmetic on non-numeric operands"};
    }

    RtValue bitwise(TokKind op, const RtValue& a, const RtValue& c) {
        int64_t x = as_int(a), y = as_int(c);
        switch (op) {
        case TokKind::BitAnd: return wrap48(x & y);   case TokKind::BitOr:  return wrap48(x | y);
        case TokKind::BitXor: return wrap48(x ^ y);   case TokKind::Shl:    return wrap48(x << (y & 63));
        case TokKind::Shr:    return wrap48(x >> (y & 63));
        case TokKind::UShr: { uint64_t u = static_cast<uint64_t>(x) & 0xFFFFFFFFFFFFull;
                              return wrap48(static_cast<int64_t>(u >> (y & 63))); }
        default: break;
        }
        throw NotModelled{"bitwise operator"};
    }

    // STRUCTURAL equality -- mirrors the VM's EQ_DEEP (value_deep_eq): immediates + String by value/
    // content; a heap object by SHAPE (struct/enum-variant/tuple by type_name + fields, array/vec by
    // element, Bytes by byte, Map ORDER-INDEPENDENTLY). This is the oracle that would have caught the
    // old reference-`==` bug -- before, heap `==` was Unsupported (skipped), so the oracle silently
    // agreed with the buggy VM. A function value has no structural equality (the checker forbids `==`
    // on it), so it stays Unsupported.
    RtValue equal(const RtValue& a, const RtValue& c) {
        if (is_num(a) && is_num(c)) return as_double(a) == as_double(c);
        if (a.index() != c.index()) return false;
        switch (a.index()) {
        case 0: return true;
        case 3: return std::get<bool>(a) == std::get<bool>(c);
        case 4: return std::get<std::string>(a) == std::get<std::string>(c);
        case 5: return std::get<Atom>(a).name == std::get<Atom>(c).name;
        case 6: return obj_deep_eq(std::get<std::shared_ptr<Obj>>(a), std::get<std::shared_ptr<Obj>>(c));
        // FAMILY A (by design): a function value has no structural equality to mirror, and the
        // checker rejects `==` on one -- so there is nothing to model, and this stays a skip.
        default: throw Unsupported{"== on function values"};   // case 7 (closure) -- checker forbids
        }
    }
    bool eq(const RtValue& a, const RtValue& c) { return std::get<bool>(equal(a, c)); }

    bool obj_deep_eq(const std::shared_ptr<Obj>& a, const std::shared_ptr<Obj>& c) {
        if (a == c) return true;                              // same object
        if (!a || !c || a->kind != c->kind) return false;
        switch (a->kind) {
        case ObjKind::Bytes:
            return a->bytes == c->bytes;
        case ObjKind::Map: {                                  // order-INDEPENDENT
            if (a->map.size() != c->map.size()) return false;
            for (const auto& ka : a->map) {
                bool found = false;
                for (const auto& kc : c->map)
                    if (eq(ka.first, kc.first)) {
                        if (!eq(ka.second, kc.second)) return false;
                        found = true; break;
                    }
                if (!found) return false;
            }
            return true;
        }
        default:                                             // Struct / Tuple / List / Array / Vec
            // type_name distinguishes enum variants (None vs Some) and struct/tuple names; empty for
            // anonymous tuples and containers (so element-wise alone decides those).
            if (a->type_name != c->type_name) return false;
            if (a->items.size() != c->items.size()) return false;
            for (size_t i = 0; i < a->items.size(); ++i)
                if (!eq(a->items[i], c->items[i])) return false;
            return true;
        }
    }

    RtValue compare(TokKind op, const RtValue& a, const RtValue& c) {
        int rel;
        if (is_num(a) && is_num(c)) {
            double x = as_double(a), y = as_double(c);
            if (x < y) rel = -1; else if (x > y) rel = 1; else if (x == y) rel = 0; else return false;
        } else if (std::holds_alternative<std::string>(a) && std::holds_alternative<std::string>(c)) {
            const auto& x = std::get<std::string>(a); const auto& y = std::get<std::string>(c);
            rel = (x < y) ? -1 : (x > y) ? 1 : 0;
        } else { throw NotModelled{"ordering compare on these operands"}; }
        switch (op) {
        case TokKind::Lt: return rel < 0;  case TokKind::Le: return rel <= 0;
        case TokKind::Gt: return rel > 0;  case TokKind::Ge: return rel >= 0;
        default: break;
        }
        throw NotModelled{"compare operator"};
    }

    RtValue concat(const RtValue& a, const RtValue& c) { return to_concat_str(a) + to_concat_str(c); }
    std::string to_concat_str(const RtValue& v) {
        if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
        if (is_int(v))    return std::to_string(std::get<int64_t>(v));
        if (is_double(v)) return canon_double(std::get<double>(v));
        if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";  // Java `+` Bool concat
        throw NotModelled{"`+` with a non-string/non-scalar operand"};  // heap: checker rejects, so unreachable
    }

    // ----- data construction -----
    RtValue construct(const CtorInfo& c, std::vector<RtValue> fields) {
        if (c.is_int_enum) {
            // Erased int-backed enum variant: the runtime value IS the bare Int discriminant (no Obj),
            // canonicalizing identically to the VM's bare Int.
            return RtValue{ c.disc };
        }
        if (c.is_transparent && fields.size() == 1) {
            // Erased newtype: the runtime value IS the single field's immediate (no Obj wrapper),
            // so it canonicalizes identically to the VM's bare Int/Double/Bool.
            return coerce_dbl(std::move(fields[0]),
                              syn_is_double(!c.field_types.empty() ? c.field_types[0] : nullptr));
        }
        auto o = std::make_shared<Obj>();
        o->kind = ObjKind::Struct; o->type_name = c.type_name; o->enum_name = c.enum_name;
        o->is_tuple = c.is_tuple;
        o->field_names = c.field_names;
        for (size_t i = 0; i < fields.size(); ++i)
            o->items.push_back(coerce_dbl(std::move(fields[i]),
                                          syn_is_double(i < c.field_types.size() ? c.field_types[i] : nullptr)));
        return o;
    }

    RtValue eval_struct_lit(const StructLit& sl, Env& env) {
        auto cit = ctors_.find(sl.name);
        if (cit == ctors_.end()) throw NotModelled{"struct literal of unknown type"};
        const CtorInfo& c = cit->second;
        std::vector<RtValue> fields(c.field_names.size());
        if (sl.base) {                                     // record update: seed with a copy of the base
            RtValue b = eval(*sl.base, env);               // evaluated once
            if (is_obj(b)) {
                const Obj& bo = *as_obj(b);
                for (size_t i = 0; i < fields.size() && i < bo.items.size(); ++i) fields[i] = bo.items[i];
            }
        }
        for (const auto& fi : sl.fields) {                 // explicit fields override the base copy
            size_t idx = 0; bool found = false;
            for (; idx < c.field_names.size(); ++idx) if (c.field_names[idx] == fi.name) { found = true; break; }
            if (!found) throw NotModelled{"struct literal field not found"};
            fields[idx] = fi.value ? eval(*fi.value, env)
                                   : (find(env, fi.name) ? *find(env, fi.name) : RtValue{});
        }
        return construct(c, std::move(fields));
    }

    RtValue eval_tuple(const TupleExpr& t, Env& env) {
        auto o = std::make_shared<Obj>(); o->kind = ObjKind::Tuple;
        for (const auto& x : t.elems) o->items.push_back(eval(*x, env));
        return o;
    }
    RtValue eval_list(const ListLit& ll, Env& env) {
        if (ll.elems.empty()) return RtValue{};
        // A const array (`const K: Array[T] = [...]`) is typed Array[T] by the checker; a plain
        // sequence literal is List[T]. Both index/iterate/len identically here, but set the kind so
        // canonicalization/typeof agree, and coerce Int elements to Double when T = Double (the
        // Int<:Double element coercion the VM applies when it builds the pooled const array).
        const bool elem_double = ll.ty && ll.ty->kind == TyKind::Named && ll.ty->args.size() == 1 &&
                                 ty_is_double(ll.ty->args[0]);
        auto o = std::make_shared<Obj>();
        o->kind = (ll.ty && ll.ty->kind == TyKind::Named && ll.ty->name == "Array")
                      ? ObjKind::Array : ObjKind::List;
        for (const auto& x : ll.elems) {
            RtValue v = eval(*x, env);
            if (elem_double && is_int(v)) v = as_double(v);
            o->items.push_back(std::move(v));
        }
        return o;
    }
    RtValue eval_map(const MapLit& ml, Env& env) {
        auto o = std::make_shared<Obj>(); o->kind = ObjKind::Map;
        for (const auto& kv : ml.entries) map_set(*o, eval(*kv.first, env), eval(*kv.second, env));
        return o;
    }
    void map_set(Obj& m, RtValue k, RtValue v) {
        for (auto& e : m.map) if (eq(e.first, k)) { e.second = std::move(v); return; }
        m.map.emplace_back(std::move(k), std::move(v));
    }

    // ----- field / index access -----
    size_t field_index(const Obj& o, const FieldExpr& fe) {
        if (fe.tuple_index) return fe.index;
        for (size_t i = 0; i < o.field_names.size(); ++i) if (o.field_names[i] == fe.name) return i;
        throw NotModelled{"field '" + fe.name + "' not found"};
    }
    RtValue eval_field(const FieldExpr& fe, Env& env) {
        RtValue o = eval(*fe.obj, env);
        if (!is_obj(o)) {
            // A transparent struct erased to its underlying immediate; `.0` is the value itself.
            if (fe.tuple_index && fe.index == 0) return o;
            throw NotModelled{"field access on a non-object"};
        }
        return as_obj(o)->items[field_index(*as_obj(o), fe)];
    }
    RtValue eval_index(const IndexExpr& ix, Env& env) {
        RtValue o = eval(*ix.obj, env);
        if (is_obj(o) && as_obj(o)->kind == ObjKind::Map) {   // m[k] -- lookup by key, TRAP on a miss
            RtValue key = eval(*ix.index, env);
            for (const auto& e : as_obj(o)->map) if (eq(e.first, key)) return e.second;
            throw PanicSignal{"map key not found"};
        }
        int64_t i = as_int(eval(*ix.index, env));
        if (is_obj(o)) {
            const Obj& ob = *as_obj(o);
            if (ob.kind == ObjKind::Bytes) {
                if (i < 0 || i >= (int64_t)ob.bytes.size()) throw PanicSignal{"index out of bounds"};
                return (int64_t)ob.bytes[(size_t)i];
            }
            if (i < 0 || i >= (int64_t)ob.items.size()) throw PanicSignal{"index out of bounds"};
            return ob.items[(size_t)i];
        }
        throw PanicSignal{"index out of bounds"};
    }
    void store_index(RtValue& container, int64_t i, RtValue v) {
        if (!is_obj(container)) throw PanicSignal{"index out of bounds"};
        Obj& ob = *as_obj(container);
        if (ob.kind == ObjKind::Bytes) {
            if (i < 0 || i >= (int64_t)ob.bytes.size()) throw PanicSignal{"index out of bounds"};
            ob.bytes[(size_t)i] = (uint8_t)(as_int(v) & 0xFF); return;
        }
        if (i < 0 || i >= (int64_t)ob.items.size()) throw PanicSignal{"index out of bounds"};
        ob.items[(size_t)i] = std::move(v);
    }

    // ----- match + patterns -----
    RtValue eval_match(const MatchExpr& m, Env& env) {
        RtValue scrut = eval(*m.scrut, env);
        for (const auto& arm : m.arms) {
            Env armEnv; armEnv.parent = &env;
            if (match_pattern(*arm.pat, scrut, armEnv)) {
                if (arm.guard && !as_bool(eval(*arm.guard, armEnv))) continue;
                return arm.body ? eval(*arm.body, armEnv) : RtValue{};
            }
        }
        throw PanicSignal{"no match arm"};
    }
    void bind_pattern(const Pattern& pat, RtValue v, Env& env) {
        if (!match_pattern(pat, std::move(v), env)) throw NotModelled{"irrefutable-let pattern did not match"};
    }
    static bool is_list_like(const RtValue& v, const Obj** out) {
        if (std::holds_alternative<std::monostate>(v)) { *out = nullptr; return true; }
        if (is_obj(v)) { const Obj& o = *as_obj(v);
            if (o.kind == ObjKind::List || o.kind == ObjKind::Array || o.kind == ObjKind::Vec) { *out = &o; return true; } }
        return false;
    }
    bool match_pattern(const Pattern& pat, RtValue v, Env& env) {
        cov_.mark_pat(pat.kind);
        switch (pat.kind) {
        case PatKind::Wildcard: return true;
        case PatKind::Ident:    env.vars[static_cast<const IdentPat&>(pat).name] = std::move(v); return true;
        case PatKind::Bind: {   // `n @ sub` -- bind the whole value, then require `sub` to match
            const auto& bp = static_cast<const BindPat&>(pat);
            env.vars[bp.name] = v;                     // copy: `v` is still needed for the sub-match
            return bp.sub ? match_pattern(*bp.sub, std::move(v), env) : true;
        }
        case PatKind::Literal:  return eq(v, eval(*static_cast<const LiteralPat&>(pat).lit, env));
        case PatKind::Range: {   // `lo..hi` inclusive / `lo..<hi` exclusive upper bound
            const auto& rp = static_cast<const RangePat&>(pat);
            RtValue lo = eval(*rp.lo, env), hi = eval(*rp.hi, env);
            if (!is_num(v) || !is_num(lo) || !is_num(hi)) return false;
            const double x = as_double(v);
            if (x < as_double(lo)) return false;
            return rp.exclusive ? x < as_double(hi) : x <= as_double(hi);
        }
        case PatKind::Ctor: {
            const auto& cp = static_cast<const CtorPat&>(pat);
            if (auto cit = ctors_.find(cp.name); cit != ctors_.end() && cit->second.is_transparent)
                // Erased newtype: v is the bare underlying immediate; match the inner pattern on it.
                return !cp.elems.empty() && match_pattern(*cp.elems[0], std::move(v), env);
            if (auto cit = ctors_.find(cp.name); cit != ctors_.end() && cit->second.is_int_enum)
                // Erased int-backed enum: v is the bare Int discriminant; match on equality (nullary).
                return is_int(v) && as_int(v) == cit->second.disc;
            if (!is_obj(v) || as_obj(v)->kind != ObjKind::Struct || as_obj(v)->type_name != short_name(cp.name)) return false;
            const Obj& o = *as_obj(v);
            for (size_t i = 0; i < cp.elems.size(); ++i)
                if (i >= o.items.size() || !match_pattern(*cp.elems[i], o.items[i], env)) return false;
            return true;
        }
        case PatKind::Struct: {
            const auto& sp = static_cast<const StructPat&>(pat);
            if (!is_obj(v) || as_obj(v)->kind != ObjKind::Struct || as_obj(v)->type_name != short_name(sp.name)) return false;
            const Obj& o = *as_obj(v);
            for (const auto& fp : sp.fields) {
                size_t idx = 0; bool found = false;
                for (; idx < o.field_names.size(); ++idx) if (o.field_names[idx] == fp.name) { found = true; break; }
                if (!found) return false;
                if (fp.pat) { if (!match_pattern(*fp.pat, o.items[idx], env)) return false; }
                else        env.vars[fp.name] = o.items[idx];
            }
            return true;
        }
        case PatKind::Tuple: {
            const auto& tp = static_cast<const TuplePat&>(pat);
            if (!is_obj(v) || as_obj(v)->kind != ObjKind::Tuple) return false;
            const Obj& o = *as_obj(v);
            if (o.items.size() != tp.elems.size()) return false;
            for (size_t i = 0; i < tp.elems.size(); ++i) if (!match_pattern(*tp.elems[i], o.items[i], env)) return false;
            return true;
        }
        case PatKind::List: {
            const auto& lp = static_cast<const ListPat&>(pat);
            const Obj* o = nullptr;
            if (!is_list_like(v, &o)) return false;
            size_t n = o ? o->items.size() : 0;
            if (!lp.rest) { if (n != lp.elems.size()) return false; }
            else          { if (n < lp.elems.size()) return false; }
            for (size_t i = 0; i < lp.elems.size(); ++i) if (!match_pattern(*lp.elems[i], o->items[i], env)) return false;
            if (lp.rest) {
                RtValue tail;
                if (n > lp.elems.size()) {
                    auto t = std::make_shared<Obj>(); t->kind = ObjKind::List;
                    for (size_t i = lp.elems.size(); i < n; ++i) t->items.push_back(o->items[i]);
                    tail = t;
                }
                if (!match_pattern(*lp.rest, std::move(tail), env)) return false;
            }
            return true;
        }
        case PatKind::Map: {
            const auto& mp = static_cast<const MapPat&>(pat);
            if (!is_obj(v) || as_obj(v)->kind != ObjKind::Map) return false;
            const Obj& o = *as_obj(v);
            for (const auto& kv : mp.entries) {
                RtValue key = eval(*kv.first, env);
                const RtValue* found = nullptr;
                for (const auto& e : o.map) if (eq(e.first, key)) { found = &e.second; break; }
                if (!found || !match_pattern(*kv.second, *found, env)) return false;
            }
            return true;
        }
        case PatKind::Or: {   // `A | B | C` -- v1: no bindings; first matching alternative wins
            const auto& op = static_cast<const OrPat&>(pat);
            for (const auto& a : op.alts) {
                // Restore on failure: a partially-matched alternative must not leave its bindings
                // behind for the next one. A no-op under v1a (alternatives cannot bind); here so
                // binding or-patterns do not have to rediscover it in the oracle.
                auto saved = env.vars;
                if (match_pattern(*a, v, env)) return true;
                env.vars = std::move(saved);
            }
            return false;
        }
        }
        return false;
    }

    // ----- for -----
    RtValue eval_for(const ForExpr& f, Env& env) {
        RtValue it = eval(*f.iter, env);
        const bool kv_pat = f.pat->kind == PatKind::Tuple &&
                            static_cast<const TuplePat&>(*f.pat).elems.size() == 2;
        // Built-in container: snapshot the elements (mirrors the compiled fast paths).
        std::vector<RtValue> elems;
        bool builtin = false;
        if (std::holds_alternative<std::monostate>(it)) {
            builtin = true;   // empty list -> no iterations
        } else if (std::holds_alternative<std::string>(it)) {
            // String iterates its raw BYTES (Int 0..255) -- Option B; mirrors the VM path
            // `intoIter(toBytes(s))` -> BytesCursor and the Bytes case below.
            for (unsigned char b : std::get<std::string>(it)) elems.push_back((int64_t)b);
            builtin = true;
        } else if (is_obj(it)) {
            const Obj& o = *as_obj(it);
            switch (o.kind) {
            case ObjKind::List: case ObjKind::Array: case ObjKind::Vec: elems = o.items; builtin = true; break;
            case ObjKind::Bytes: for (uint8_t b : o.bytes) elems.push_back((int64_t)b); builtin = true; break;
            case ObjKind::Map:
                for (const auto& e : o.map) {
                    if (kv_pat) { auto t = std::make_shared<Obj>(); t->kind = ObjKind::Tuple;
                                  t->items = { e.first, e.second }; elems.push_back(t); }
                    else elems.push_back(e.first);
                }
                builtin = true;
                break;
            default: break;   // a user Struct -> try the lazy iterator protocol below
            }
        }
        if (builtin) {
            for (RtValue& el : elems) {
                Env loopEnv; loopEnv.parent = &env;
                bind_pattern(*f.pat, std::move(el), loopEnv);
                try { eval(*f.body, loopEnv); }
                catch (const BreakSignal&) { break; }
                catch (const ContinueSignal&) { continue; }
            }
            return RtValue{};
        }
        // Lazy protocol: `it` is itself an Iterator (drive next() in place), or an IntoIterator (make a
        // fresh cursor via intoIter first). Dispatch keys on the runtime head, mirroring the VM.
        if (is_obj(it)) {
            const std::string head = head_type(it);
            RtValue cursor;
            bool ok = false;
            if (impls_.count("Iterator" + std::string(1, SEP) + head)) { cursor = it; ok = true; }
            else if (impls_.count("IntoIterator" + std::string(1, SEP) + head)) {
                cursor = dispatch_trait("intoIter", "IntoIterator", { it }, env); ok = true;
            }
            if (ok) {
                while (true) {
                    RtValue opt = dispatch_trait("next", "Iterator", { cursor }, env);
                    if (!is_obj(opt) || as_obj(opt)->type_name == "None") break;   // None -> stop
                    RtValue el = as_obj(opt)->items.empty() ? RtValue{} : as_obj(opt)->items[0];
                    Env loopEnv; loopEnv.parent = &env;
                    bind_pattern(*f.pat, std::move(el), loopEnv);
                    try { eval(*f.body, loopEnv); }
                    catch (const BreakSignal&) { break; }
                    catch (const ContinueSignal&) { continue; }
                }
                return RtValue{};
            }
        }
        throw NotModelled{"for over a non-iterable"};
    }

    // ----- calls & dispatch -----
    RtValue eval_call(const CallExpr& c, Env& env) {
        if (!c.callee) throw NotModelled{"call without callee"};
        std::vector<const Expr*> argx; argx.reserve(c.args.size());
        for (const auto& a : c.args) argx.push_back(a.get());
        if (c.callee->kind == ExprKind::Ident) return call_named(static_cast<const IdentExpr&>(*c.callee), argx, env);
        // Method-call syntax `recv.method(args)` (S1): field-first, else a trait-method dispatch
        // with the receiver as arg 0 (mirrors the checker / codegen).
        if (c.callee->kind == ExprKind::Field) {
            const auto& fld = static_cast<const FieldExpr&>(*c.callee);
            if (!fld.tuple_index) {
                RtValue recv = eval(*fld.obj, env);
                if (is_obj(recv)) {
                    const Obj& o = *as_obj(recv);
                    for (size_t i = 0; i < o.field_names.size(); ++i)
                        if (o.field_names[i] == fld.name) return apply_value(o.items[i], argx, env);
                }
                // S2: an inherent method on the receiver's head wins over a trait method -- but only
                // a method WITH `self`. An associated function (no `self`) is `Head::name(...)`-only,
                // so it must not capture `.` here; mirrors the checker's filter and codegen's
                // `inherent_methods_` filter. Unreachable from well-typed source, kept because an
                // oracle that models a different call than the bytecode is the failure mode this
                // whole harness exists to catch.
                if (auto ih = inherent_.find(head_type(recv)); ih != inherent_.end()) {
                    auto im = ih->second.find(fld.name);
                    if (im != ih->second.end() && im->second->has_self) {
                        std::vector<RtValue> margs; margs.push_back(recv);
                        for (const Expr* a : argx) margs.push_back(eval(*a, env));
                        return call_method(*im->second, std::move(margs));
                    }
                }
                // The trait the checker dispatched this call to (several may declare the name); the
                // by-name owner only for a call it recorded none for.
                const std::string trait = !fld.resolved_trait.empty() ? fld.resolved_trait : owner_of(fld.name);
                if (!trait.empty()) {
                    std::vector<RtValue> margs; margs.push_back(recv);
                    for (const Expr* a : argx) margs.push_back(eval(*a, env));
                    return dispatch_trait(fld.name, trait, std::move(margs), env);
                }
                throw NotModelled{"no field or method '" + fld.name + "'"};
            }
        }
        return apply_value(eval(*c.callee, env), argx, env);
    }

    RtValue eval_pipe(const PipeExpr& p, Env& env) {
        std::vector<const Expr*> argx; argx.push_back(p.lhs.get());
        const IdentExpr* id = nullptr;
        if (p.rhs->kind == ExprKind::Ident) id = &static_cast<const IdentExpr&>(*p.rhs);
        else if (p.rhs->kind == ExprKind::Call) {
            const auto& rc = static_cast<const CallExpr&>(*p.rhs);
            if (rc.callee && rc.callee->kind == ExprKind::Ident) {
                id = &static_cast<const IdentExpr&>(*rc.callee);
                for (const auto& a : rc.args) argx.push_back(a.get());
            }
        }
        if (!id) { RtValue callee = eval(*p.rhs, env); std::vector<const Expr*> one{ p.lhs.get() };
                   return apply_value(callee, one, env); }
        return call_named(*id, argx, env);
    }

    // Route a named call `id(argx...)` -- shared by eval_call and eval_pipe.
    RtValue call_named(const IdentExpr& id, const std::vector<const Expr*>& argx, Env& env) {
        if (id.qualifier.empty()) {
            if (RtValue* slot = find(env, id.name)) return apply_value(*slot, argx, env);
            // `id.ambient` is the checker's routing decision: a bare name it resolved PAST a
            // shadowing entry-module fn to the ambient builtin / native / trait method. The oracle
            // must follow it, or it would model a call the bytecode does not make.
            auto fit = id.ambient ? fn_table_.end() : fn_table_.find(id.name);
            if (fit != fn_table_.end()) return call_fn(*fit->second, eval_args(fit->second, argx, env));
            auto cit = ctors_.find(id.name);
            if (cit != ctors_.end()) {
                std::vector<RtValue> a; a.reserve(argx.size());
                for (const Expr* e : argx) a.push_back(eval(*e, env));
                return construct(cit->second, std::move(a));
            }
            // The sixth cov_ probe, and the only one keyed by NAME rather than node kind. On the
            // HANDLED path only: an unhandled name falls through to trait dispatch or a decline.
            if (RtValue r; try_builtin(id.name, argx, env, r)) { record_builtin_call(cov_, id.name); return r; }
            // As for `recv.m(..)`: the checker's pick first (IdentExpr::resolved_trait).
            if (const std::string trait = !id.resolved_trait.empty() ? id.resolved_trait : owner_of(id.name);
                !trait.empty())
                return dispatch_trait(id.name, trait, eval_all(argx, env), env);
            // The one site where the two decline categories MEET, which is why the split has to be
            // made here and could never be made from the message: a non-differentiable native
            // (write/delete/mkdir/listDir/time/process -- side-effecting or non-deterministic) falls
            // through to exactly this point on purpose, and so does a builtin the oracle simply
            // forgot. `native_id_of` tells them apart (the DIFFERENTIABLE natives already returned
            // above, so a native reaching here is one of the deliberately excluded ones).
            if (native_id_of(id.name) >= 0)
                throw Unsupported{"non-differentiable native '" + id.name + "'"};   // FAMILY A
            throw NotModelled{"call target not modelled: " + id.name};
        }
        // `id.inherent` is the checker's routing decision for an uppercase qualifier: the head is a
        // TYPE with an inherent method, not a trait (both may carry the same mangled name), so this
        // is a STATIC call of that body -- no trait dispatch. Following the flag rather than
        // re-deciding here is what keeps the oracle modelling the call the bytecode actually makes.
        if (id.inherent) {
            // `inherent_` is keyed by the SHORT head (like every other impl table here, and like
            // `head_type` of a runtime value), while the checker writes back the MANGLED head.
            auto ih = inherent_.find(short_name(id.qualifier));
            if (ih != inherent_.end()) {
                auto im = ih->second.find(id.name);
                if (im != ih->second.end()) return call_method(*im->second, eval_all(argx, env));
            }
            throw NotModelled{"inherent method not modelled: " + id.qualifier + "::" + id.name};
        }
        return dispatch_trait(id.name, id.qualifier, eval_all(argx, env), env);
    }

    std::vector<RtValue> eval_all(const std::vector<const Expr*>& argx, Env& env) {
        std::vector<RtValue> a; a.reserve(argx.size());
        for (const Expr* e : argx) a.push_back(eval(*e, env));
        return a;
    }
    std::vector<RtValue> eval_args(const FnItem* fn, const std::vector<const Expr*>& argx, Env& env) {
        std::vector<RtValue> out; out.reserve(argx.size());
        for (size_t i = 0; i < argx.size(); ++i) {
            const Type* pt = (fn && i < fn->params.size()) ? fn->params[i].type.get() : nullptr;
            out.push_back(coerce_dbl(eval(*argx[i], env), syn_is_double(pt)));
        }
        return out;
    }

    RtValue apply_value(const RtValue& callee, const std::vector<const Expr*>& argx, Env& env) {
        if (!std::holds_alternative<std::shared_ptr<Closure>>(callee))
            throw NotModelled{"calling a non-function value"};
        const Closure& cl = *std::get<std::shared_ptr<Closure>>(callee);
        if (cl.fn)  return call_fn(*cl.fn, eval_args(cl.fn, argx, env));
        if (cl.lam) return call_lambda(cl, argx, env);
        throw NotModelled{"empty closure"};
    }

    RtValue call_fn(const FnItem& fn, std::vector<RtValue> args) {
        Env fenv;
        for (size_t i = 0; i < fn.params.size(); ++i)
            fenv.vars[fn.params[i].name] = (i < args.size()) ? args[i] : RtValue{};
        RtValue r;
        try { r = eval(*fn.body, fenv); } catch (ReturnSignal& rs) { r = std::move(rs.value); }
        return coerce_dbl(std::move(r), syn_is_double(fn.ret.get()));
    }

    // ----- lambdas (by-value capture) -----
    RtValue eval_lambda(const LambdaExpr& lam, Env& env) {
        auto c = std::make_shared<Closure>(); c->lam = &lam;
        for (Env* e = &env; e; e = e->parent)                    // snapshot all visible bindings (by value)
            for (auto& kv : e->vars) c->captured.emplace(kv.first, kv.second);
        return c;
    }
    RtValue call_lambda(const Closure& cl, const std::vector<const Expr*>& argx, Env& callerEnv) {
        const LambdaExpr& lam = *cl.lam;
        Env lenv;
        for (const auto& kv : cl.captured) lenv.vars[kv.first] = kv.second;
        const bool fn_ty = lam.ty && lam.ty->kind == TyKind::Fn;
        for (size_t i = 0; i < lam.params.size(); ++i) {
            bool wantD = lam.params[i].type ? syn_is_double(lam.params[i].type.get())
                       : (fn_ty && i < lam.ty->args.size() ? ty_is_double(lam.ty->args[i]) : false);
            RtValue av = i < argx.size() ? eval(*argx[i], callerEnv) : RtValue{};
            lenv.vars[lam.params[i].name] = coerce_dbl(std::move(av), wantD);
        }
        RtValue r;
        try { r = eval(*lam.body, lenv); } catch (ReturnSignal& rs) { r = std::move(rs.value); }
        return coerce_dbl(std::move(r), fn_ty ? ty_is_double(lam.ty->ret) : false);
    }

    // ----- trait dispatch (dynamic on the receiver's runtime type) -----
    std::string head_type(const RtValue& v) {
        switch (v.index()) {
        case 1: return "Int";  case 2: return "Double"; case 3: return "Bool";
        case 4: return "String"; case 5: return "Atom"; case 0: return "Nil"; case 7: return "Func";
        case 6: {
            const Obj& o = *as_obj(v);
            if (!o.enum_name.empty()) return o.enum_name;
            switch (o.kind) {
            case ObjKind::Struct: return o.type_name;
            case ObjKind::List:  return "List";  case ObjKind::Array: return "Array";
            case ObjKind::Vec:   return "Vec";   case ObjKind::Bytes: return "Bytes";
            case ObjKind::Map:   return "Map";   case ObjKind::Tuple: return "Tuple";
            }
        }
        }
        return "?";
    }
    RtValue dispatch_trait(const std::string& method, const std::string& trait_in, std::vector<RtValue> args, Env&) {
        if (args.empty()) throw NotModelled{"trait method with no receiver"};
        const std::string trait = short_name(trait_in);   // trait tables are keyed by SHORT trait name
        const std::string head = head_type(args[0]);
        const Method* mth = nullptr;
        auto ik = impls_.find(trait + SEP + head);
        if (ik != impls_.end()) { auto mm = ik->second.find(method); if (mm != ik->second.end()) mth = mm->second; }
        if (!mth) { auto bk = blanket_.find(trait); if (bk != blanket_.end()) { auto mm = bk->second.find(method); if (mm != bk->second.end()) mth = mm->second; } }
        if (!mth) { auto dk = defaults_.find(trait + SEP + method); if (dk != defaults_.end()) mth = dk->second; }
        if (!mth) throw PanicSignal{"No trait implementation for type"};
        return call_method(*mth, std::move(args));
    }
    RtValue call_method(const Method& m, std::vector<RtValue> args) {
        Env env;
        size_t base = 0;
        if (m.has_self) { env.vars["self"] = args.empty() ? RtValue{} : args[0]; base = 1; }
        for (size_t i = 0; i < m.params.size(); ++i) {
            size_t ai = i + base;
            env.vars[m.params[i].name] = coerce_dbl(ai < args.size() ? args[ai] : RtValue{},
                                                    syn_is_double(m.params[i].type.get()));
        }
        RtValue r;
        try { r = eval(*m.body, env); } catch (ReturnSignal& rs) { r = std::move(rs.value); }
        return coerce_dbl(std::move(r), syn_is_double(m.ret.get()));
    }

    // ----- the `?` operator -----
    RtValue eval_try(const TryExpr& t, Env& env) {
        RtValue v = eval(*t.operand, env);
        if (is_obj(v) && as_obj(v)->kind == ObjKind::Struct) {
            const std::string& n = as_obj(v)->type_name;
            if (n == "Ok" || n == "Some") return as_obj(v)->items.empty() ? RtValue{} : as_obj(v)->items[0];
            if (n == "Err" || n == "None") throw ReturnSignal{ v };
        }
        throw NotModelled{"`?` on a non-Result/Option value"};
    }

    // ----- opcode-backed builtins -----
    bool try_builtin(const std::string& name, const std::vector<const Expr*>& argx, Env& env, RtValue& out) {
        auto ev = [&](size_t i) { return eval(*argx[i], env); };
        auto some = [&](RtValue x) { CtorInfo c; c.type_name = "Some"; c.enum_name = "Option"; c.is_variant = true;
                                     c.is_tuple = true; c.field_names = {"_0"}; std::vector<RtValue> f; f.push_back(std::move(x));
                                     return construct(c, std::move(f)); };
        auto none = [&]() { CtorInfo c; c.type_name = "None"; c.enum_name = "Option"; c.is_variant = true;
                            return construct(c, {}); };
        auto ok  = [&](RtValue x) { CtorInfo c; c.type_name = "Ok"; c.enum_name = "Result"; c.is_variant = true;
                                    c.is_tuple = true; c.field_names = {"_0"}; std::vector<RtValue> f; f.push_back(std::move(x));
                                    return construct(c, std::move(f)); };
        auto err = [&](RtValue x) { CtorInfo c; c.type_name = "Err"; c.enum_name = "Result"; c.is_variant = true;
                                    c.is_tuple = true; c.field_names = {"_0"}; std::vector<RtValue> f; f.push_back(std::move(x));
                                    return construct(c, std::move(f)); };
        if (name == "array") {
            int64_t n = as_int(ev(0)); RtValue init = ev(1);
            auto o = std::make_shared<Obj>(); o->kind = ObjKind::Array;
            for (int64_t i = 0; i < n; ++i) o->items.push_back(init);
            out = o; return true;
        }
        if (name == "vec")   { auto o = std::make_shared<Obj>(); o->kind = ObjKind::Vec;   out = o; return true; }
        if (name == "emptyArray") { auto o = std::make_shared<Obj>(); o->kind = ObjKind::Array; out = o; return true; }
        if (name == "bytes") { auto o = std::make_shared<Obj>(); o->kind = ObjKind::Bytes; out = o; return true; }
        if (name == "push") {
            RtValue vv = ev(0); if (!is_obj(vv)) throw NotModelled{"push on non-buffer"};
            Obj& o = *as_obj(vv); RtValue x = ev(1);
            if (o.kind == ObjKind::Bytes) o.bytes.push_back((uint8_t)(as_int(x) & 0xFF));
            else                          o.items.push_back(std::move(x));
            out = vv; return true;
        }
        if (name == "pop") {
            RtValue vv = ev(0); if (!is_obj(vv)) throw NotModelled{"pop on non-buffer"};
            Obj& o = *as_obj(vv);
            if (o.kind == ObjKind::Bytes) { if (o.bytes.empty()) { out = none(); return true; }
                int64_t b = o.bytes.back(); o.bytes.pop_back(); out = some((int64_t)b); return true; }
            if (o.items.empty()) { out = none(); return true; }
            RtValue x = o.items.back(); o.items.pop_back(); out = some(std::move(x)); return true;
        }
        if (name == "appendBytes") {                     // appendBytes(dst, String|Bytes) -> dst (whole)
            RtValue dv = ev(0); if (!is_obj(dv)) throw NotModelled{"appendBytes into non-buffer"};
            Obj& d = *as_obj(dv);
            RtValue sv = ev(1);
            if (std::holds_alternative<std::string>(sv)) {
                const std::string& s = std::get<std::string>(sv);
                for (unsigned char c : s) d.bytes.push_back(c);
            } else if (is_obj(sv)) {
                const Obj& s = *as_obj(sv);
                const size_t n = s.bytes.size();          // snapshot n (src may alias dst)
                for (size_t i = 0; i < n; ++i) d.bytes.push_back(s.bytes[i]);
            } else throw NotModelled{"appendBytes source not string/bytes"};
            out = dv; return true;
        }
        if (name == "_appendBytesRange") {               // _appendBytesRange(dst, src, lo, hi) -> dst
            RtValue dv = ev(0); if (!is_obj(dv)) throw NotModelled{"appendBytesRange into non-buffer"};
            Obj& d = *as_obj(dv);
            RtValue sv = ev(1); if (!is_obj(sv)) throw NotModelled{"appendBytesRange source non-bytes"};
            const Obj& s = *as_obj(sv);
            const int64_t lo = as_int(ev(2)), hi = as_int(ev(3));
            if (lo < 0 || hi < lo || hi > (int64_t)s.bytes.size())
                throw NotModelled{"appendBytesRange out of bounds"};
            for (int64_t i = lo; i < hi; ++i) d.bytes.push_back(s.bytes[(size_t)i]);  // index (may alias)
            out = dv; return true;
        }
        if (name == "len") {
            RtValue vv = ev(0);
            if (std::holds_alternative<std::monostate>(vv)) { out = (int64_t)0; return true; }
            if (std::holds_alternative<std::string>(vv)) {   // String: BYTE length (byte strings)
                out = (int64_t)std::get<std::string>(vv).size(); return true; }
            if (is_obj(vv)) { const Obj& o = *as_obj(vv);
                out = (int64_t)(o.kind == ObjKind::Bytes ? o.bytes.size() : o.kind == ObjKind::Map ? o.map.size() : o.items.size());
                return true; }
            throw NotModelled{"len on this value"};
        }
        if (name == "has") { RtValue m = ev(0), k = ev(1); if (!is_obj(m)) throw NotModelled{"has on non-map"};
            for (const auto& e : as_obj(m)->map) if (eq(e.first, k)) { out = true; return true; } out = false; return true; }
        if (name == "get") { RtValue m = ev(0), k = ev(1); if (!is_obj(m)) throw NotModelled{"get on non-collection"};
            const Obj& ob = *as_obj(m);
            if (ob.kind == ObjKind::Map) {                       // Map: Some(value) if the key is present
                for (const auto& e : ob.map) if (eq(e.first, k)) { out = some(e.second); return true; } out = none(); return true; }
            int64_t i = as_int(k);                               // sequence: 0 <= i < len ? Some(a[i]) : None
            if (ob.kind == ObjKind::Bytes) {
                out = (i >= 0 && i < (int64_t)ob.bytes.size()) ? some((int64_t)ob.bytes[(size_t)i]) : none(); return true; }
            out = (i >= 0 && i < (int64_t)ob.items.size()) ? some(ob.items[(size_t)i]) : none(); return true; }
        if (name == "delete") { RtValue m = ev(0), k = ev(1); if (!is_obj(m)) throw NotModelled{"delete on non-map"};
            auto& mm = as_obj(m)->map;
            for (size_t i = 0; i < mm.size(); ++i) if (eq(mm[i].first, k)) { mm.erase(mm.begin() + i); out = true; return true; }
            out = false; return true; }
        if (name == "keys" || name == "values") {
            RtValue m = ev(0); if (!is_obj(m)) throw NotModelled{"keys/values on non-map"};
            auto o = std::make_shared<Obj>(); o->kind = ObjKind::Array;
            for (const auto& e : as_obj(m)->map) o->items.push_back(name == "keys" ? e.first : e.second);
            out = o; return true;
        }
        // Internal live-map cursor primitives (behind the prelude MapCursor). The oracle's map is a
        // DENSE insertion-ordered vector (no tombstones), so the cursor index is a plain dense index:
        // mapIterNext(m, i) = i < size ? i : -1; mapKeyAt/mapValAt(m, i) = entry i's key/value. (The VM
        // walks hash order -- the multiset of pairs matches, only ORDER differs, which stays unobserved.)
        if (name == "mapIterNext" || name == "mapKeyAt" || name == "mapValAt") {
            RtValue m = ev(0); if (!is_obj(m) || as_obj(m)->kind != ObjKind::Map) throw NotModelled{"map cursor on non-map"};
            const auto& mm = as_obj(m)->map;
            int64_t i = as_int(ev(1));
            if (name == "mapIterNext") { out = (i >= 0 && (size_t)i < mm.size()) ? i : (int64_t)-1; return true; }
            if (i < 0 || (size_t)i >= mm.size()) throw NotModelled{"map cursor index out of range"};
            out = (name == "mapKeyAt") ? mm[(size_t)i].first : mm[(size_t)i].second;
            return true;
        }
        if (name == "toInt") {   // saturating Double->Int (Java): NaN->0, +-inf/overflow->MAX/MIN_48, else trunc
            double d = as_double(ev(0));
            constexpr int64_t MAX_48 =  140737488355327LL, MIN_48 = -140737488355328LL;
            int64_t r;
            if (d != d)                                r = 0;
            else if (d > static_cast<double>(MAX_48))  r = MAX_48;
            else if (d < static_cast<double>(MIN_48))  r = MIN_48;
            else                                       r = static_cast<int64_t>(d);
            out = r; return true;
        }
        // toDouble(x: Int) -> Double, the ambient mirror of toInt, lowered by codegen to I2D. It was
        // MISSING here until the builtin-coverage assertion asked for it -- a shipped builtin used by
        // demo/raytracer/main.skn and locked by two guide claims, yet never once compared VM-vs-oracle.
        if (name == "toDouble") { out = as_double(ev(0)); return true; }
        // ordinal(e: <int-backed enum>) -> Int. Identity on BOTH sides: `construct` already returns
        // the bare discriminant for an int-backed variant (there is no Obj to unwrap), exactly as the
        // VM's erasure does, so there is nothing here for the two implementations to disagree about
        // except the discriminant assignment itself -- which is the half worth cross-checking.
        if (name == "ordinal") { out = as_int(ev(0)); return true; }
        if (name == "floor" || name == "ceil" || name == "trunc" ||   // Double -> Double (DROUND mirror)
            name == "round" || name == "roundHalfToEven") {
            double d = as_double(ev(0));
            out = name == "floor" ? std::floor(d) : name == "ceil"  ? std::ceil(d)  :
                  name == "trunc" ? std::trunc(d) : name == "round" ? std::round(d) :
                                    std::nearbyint(d);   // roundHalfToEven (ties to even)
            return true;
        }
        // std::math natives -- mirror the identical libm call (bit-exact vs the VM native).
        if (name == "sqrt")  { out = std::sqrt (as_double(ev(0))); return true; }
        if (name == "cbrt")  { out = std::cbrt (as_double(ev(0))); return true; }
        if (name == "exp")   { out = std::exp  (as_double(ev(0))); return true; }
        if (name == "ln")    { out = std::log  (as_double(ev(0))); return true; }
        if (name == "log2")  { out = std::log2 (as_double(ev(0))); return true; }
        if (name == "log10") { out = std::log10(as_double(ev(0))); return true; }
        if (name == "sin")   { out = std::sin  (as_double(ev(0))); return true; }
        if (name == "cos")   { out = std::cos  (as_double(ev(0))); return true; }
        if (name == "tan")   { out = std::tan  (as_double(ev(0))); return true; }
        if (name == "asin")  { out = std::asin (as_double(ev(0))); return true; }
        if (name == "acos")  { out = std::acos (as_double(ev(0))); return true; }
        if (name == "atan")  { out = std::atan (as_double(ev(0))); return true; }
        if (name == "abs")   { out = std::fabs (as_double(ev(0))); return true; }
        if (name == "pow")   { out = std::pow  (as_double(ev(0)), as_double(ev(1))); return true; }
        if (name == "hypot") { out = std::hypot(as_double(ev(0)), as_double(ev(1))); return true; }
        if (name == "atan2") { out = std::atan2(as_double(ev(0)), as_double(ev(1))); return true; }
        if (name == "isNaN")      { out = static_cast<bool>(std::isnan(as_double(ev(0)))); return true; }
        if (name == "isInfinite") { out = static_cast<bool>(std::isinf(as_double(ev(0)))); return true; }
        if (name == "toBytes") { RtValue s = ev(0); if (!std::holds_alternative<std::string>(s)) throw NotModelled{"toBytes on non-string"};
            auto o = std::make_shared<Obj>(); o->kind = ObjKind::Bytes;
            for (unsigned char ch : std::get<std::string>(s)) o->bytes.push_back(ch);
            out = o; return true; }
        if (name == "fromBytes") { RtValue b = ev(0); if (!is_obj(b)) throw NotModelled{"fromBytes on non-bytes"};
            out = std::string(as_obj(b)->bytes.begin(), as_obj(b)->bytes.end()); return true; }
        if (name == "toString") { out = stringify_arg(argx[0], ev(0)); return true; }
        if (name == "print" || name == "println") {       // variadic, NO separator; println adds one newline
            for (size_t i = 0; i < argx.size(); ++i) out_ += stringify_arg(argx[i], ev(i));
            if (name == "println") out_ += "\n";
            out = RtValue{}; return true;
        }
        // `panic` DIVERGES, so it is the one builtin that never reaches the handled `return true` the
        // coverage probe sits on -- without this it could never be recorded as covered, no matter how
        // many tests exercise it. Marked here instead, where the divergence starts.
        if (name == "panic")    { RtValue m = ev(0);
                                  record_builtin_call(cov_, name);
                                  throw PanicSignal{ std::holds_alternative<std::string>(m) ? std::get<std::string>(m) : "panic" }; }
        // ---- differentiable natives (see NativeEnv): deterministic, side-effect-free, message-
        // independent. run_diff feeds the VM the SAME fixtures, so the two sides agree. Every OTHER
        // native (write/delete/mkdir/listDir/time/process) is left to fall through to `call_named`,
        // which recognizes it via `native_id_of` and skips it as Unsupported rather than a gap.
        if (name == "args") {                             // args() -> Array[String] (the fixed list)
            auto o = std::make_shared<Obj>(); o->kind = ObjKind::Array;
            for (const auto& a : nenv_.args) o->items.push_back(a);
            out = o; return true;
        }
        if (name == "getEnv") {                           // getEnv(env_name) -> Some(env_value); else None
            RtValue nm = ev(0);
            if (std::holds_alternative<std::string>(nm) && std::get<std::string>(nm) == nenv_.env_name)
                out = some(std::string(nenv_.env_value));
            else out = none();
            return true;
        }
        if (name == "parseInt") {                         // pure: same std::from_chars logic as the VM native
            std::string s = std::get<std::string>(ev(0));
            const char* first = s.data(); const char* last = first + s.size();
            int64_t v = 0; auto r = std::from_chars(first, last, v);
            constexpr int64_t MAX_48 = 140737488355327LL, MIN_48 = -140737488355328LL;
            if (r.ec != std::errc{} || r.ptr != last || v < MIN_48 || v > MAX_48)
                out = err(std::string("parseInt: invalid integer"));
            else out = ok((int64_t)v);
            return true;
        }
        if (name == "parseDouble") {
            std::string s = std::get<std::string>(ev(0));
            const char* first = s.data(); const char* last = first + s.size();
            double v = 0.0; auto r = std::from_chars(first, last, v);
            if (r.ec != std::errc{} || r.ptr != last)
                out = err(std::string("parseDouble: invalid number"));
            else out = ok(v);
            return true;
        }
        if (name == "fileExists") { out = false; return true; }   // generator only queries KNOWN-ABSENT paths
        if (name == "isFile") { out = false; return true; }       // absent path -> not a regular file
        if (name == "isDir")  { out = false; return true; }       // absent path -> not a directory
        if (name == "fileSize") {                         // absent path -> Err (discriminant only; the
            CtorInfo c; c.type_name = "Err"; c.enum_name = "Result"; c.is_variant = true;  // ec.message() is
            c.field_names = { "_0" };                     // OS-specific, so tests read only isOk/isErr)
            std::vector<RtValue> f; f.push_back(std::string("could not stat file"));
            out = construct(c, std::move(f)); return true;
        }
        if (name == "readFile") {                         // absent path -> Err (discriminant only; the VM's
            CtorInfo c; c.type_name = "Err"; c.enum_name = "Result"; c.is_variant = true;  // ec.message() is
            c.field_names = { "_0" };                     // OS-specific, so tests read only isOk/isErr)
            std::vector<RtValue> f; f.push_back(std::string("no such file"));
            out = construct(c, std::move(f)); return true;
        }
        if (name == "rawOsId") {                          // the running platform, same mapping as the
#ifdef _WIN32                                             // native -- a disagreement here means the two
            out = static_cast<int64_t>(0);                // sides were built for different platforms,
#elif defined(__APPLE__)                                  // which is exactly what the differential
            out = static_cast<int64_t>(1);                // should call a failure
#else
            out = static_cast<int64_t>(2);
#endif
            return true;
        }
        if (name == "readAllStdin") {                     // the rest of the shared stdin cursor
            out = nenv_.stdin_text.substr(std::min(stdin_pos_, nenv_.stdin_text.size()));
            stdin_pos_ = nenv_.stdin_text.size(); return true;
        }
        if (name == "readLine") {                         // next line (trailing \r stripped); None at EOF
            if (stdin_pos_ >= nenv_.stdin_text.size()) { out = none(); return true; }
            const std::size_t nl = nenv_.stdin_text.find('\n', stdin_pos_);
            std::string line;
            if (nl == std::string::npos) { line = nenv_.stdin_text.substr(stdin_pos_); stdin_pos_ = nenv_.stdin_text.size(); }
            else { line = nenv_.stdin_text.substr(stdin_pos_, nl - stdin_pos_); stdin_pos_ = nl + 1; }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out = some(std::move(line)); return true;
        }
        return false;
    }

    // toString(x) / print(x) / println(x): the TOP-LEVEL VM TO_STRING coercion. Scalars stringify as
    // before; a String is identity (unquoted); a container renders its ordered dump (graded-(c) mirror,
    // strings quoted inside) -- a Map still raises Unsupported (skipped). See render_dump.
    std::string scalar_to_string(const RtValue& v) {
        return render_dump(v, /*field=*/false, /*depth=*/0);
    }
    // If the STATIC type `ty` is a transparent (erased) struct, its short display name; else "". Mirrors
    // the codegen `compile_stringify` name recovery so `toString`/`print`/`${}` of a transparent value
    // renders `Name(inner)` (the runtime value is a bare immediate with no type id). TOP-LEVEL only --
    // a NESTED transparent value dumps as the bare immediate on both sides (the permanent partial).
    std::string transparent_name(const TyPtr& ty) const {
        if (ty && ty->kind == TyKind::Named) {
            auto it = ctors_.find(ty->name);
            if (it != ctors_.end() && it->second.is_transparent) return it->second.type_name;
        }
        return "";
    }
    // If the STATIC type `ty` is an int-backed enum and `v` is the matching bare Int discriminant,
    // its short variant NAME; else "". Mirrors codegen `compile_stringify`'s discriminant->name switch.
    std::string int_enum_name(const TyPtr& ty, const RtValue& v) const {
        if (ty && ty->kind == TyKind::Named && is_int(v)) {
            auto it = int_enum_names_.find(short_name(ty->name));
            if (it != int_enum_names_.end())
                for (const auto& dv : it->second)
                    if (dv.first == std::get<int64_t>(v)) return dv.second;
        }
        return "";
    }
    // UTF-8 encode a code point -> its glyph bytes. Replicates the prelude `codePointToStr`/`_encodeCp`
    // BYTE-FOR-BYTE (incl. invalid/surrogate -> U+FFFD) so a Char glyph is differentially identical to the
    // VM's codePointToStr output.
    static std::string utf8_encode(int64_t cp) {
        std::string s;
        if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            s.push_back('\xEF'); s.push_back('\xBF'); s.push_back('\xBD');   // U+FFFD
        } else if (cp <= 0x7F) {
            s.push_back((char)cp);
        } else if (cp <= 0x7FF) {
            s.push_back((char)(0xC0 | (cp >> 6)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            s.push_back((char)(0xE0 | (cp >> 12)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            s.push_back((char)(0xF0 | (cp >> 18)));
            s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        }
        return s;
    }
    // Stringify an argument at a toString/print site, honoring the transparent-newtype + int-enum naming
    // (TOP-LEVEL only; a NESTED erased value dumps as the bare immediate on both sides).
    std::string stringify_arg(const Expr* argExpr, const RtValue& v) {
        const TyPtr aty = argExpr ? argExpr->ty : nullptr;
        // A Char (transparent newtype over the code-point Int, std::string) renders as its GLYPH -- the
        // UTF-8 encoding of the code point -- matching codegen's synthesized codePointToStr call. Checked
        // BEFORE the generic transparent `Name(inner)` form (Char IS a transparent struct).
        if (is_int(v) && aty && aty->kind == TyKind::Named && short_name(aty->name) == "Char" &&
            !transparent_name(aty).empty())
            return utf8_encode(std::get<int64_t>(v));
        const std::string en = int_enum_name(aty, v);
        if (!en.empty()) return en;
        const std::string tn = transparent_name(aty);
        return tn.empty() ? scalar_to_string(v) : (tn + "(" + scalar_to_string(v) + ")");
    }
};

} // namespace

RunResult eval_program(const Program& prog, const NativeEnv& nenv) {
    Interp interp(prog, nenv);
    return interp.run();
}

} // namespace refeval
