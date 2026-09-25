#pragma once

// =============================================================================
// RefEval.h -- a tree-walking REFERENCE INTERPRETER over the Skarn typed AST.
//
// This is the ORACLE half of a differential test harness (see main.cpp's
// test_differential): a second, VM-free implementation of the language semantics.
// It evaluates a checked `svc::Program` (every Expr::ty filled by svc::check)
// DIRECTLY -- no bytecode, no frames, no registers, no register windows, no GC.
// Recursion rides the native C++ stack; values are ordinary C++ objects.
//
// The point is independence: the production path (svc::compile -> bytecode ->
// execute()) and this oracle share only the front end (lex/parse/check). A codegen
// bug yields a wrong value on the VM path; the oracle -- which has none of the
// frame/register machinery where those bugs live -- yields the right value; the
// harness compares canonical forms and flags the mismatch, catching SILENT
// miscompiles that the hand-written value-assertion tests cannot.
//
// Scope: PURE, DETERMINISTIC programs. Deep-recursion / N=1M torture programs stay VM-only
// (this walker uses the native stack).
//
// TWO WAYS TO DECLINE A PROGRAM, and the distinction is load-bearing:
//   * `Unsupported` -- deliberately outside the oracle's remit (the Map hash-order dump, `==` on
//     function values, a non-deterministic / side-effecting native). Reported as an honest SKIP.
//   * `NotModelled` -- the oracle simply LACKS the construct. Reported as a FAILURE, exactly like
//     a VM/oracle disagreement.
// Both used to be one silent skip, which made the oracle fail BY SILENCE: adding a language
// construct without teaching the oracle merely lowered the skip count, and a bare "98 skipped"
// reads identically either way. RefEval.cpp:equal() records that shape for the old reference-`==`
// bug (heap `==` was skipped, so the oracle silently agreed with the buggy VM), and the bitwise
// compound-assignment slice reproduced it. The rule that follows: `Unsupported` must be JUSTIFIED
// at its throw site; anything else is loud.
// =============================================================================

#include "Ast.h"
#include "Types.h"

#include <array>      // Coverage -- fixed, allocation-free per-run node-kind record
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace refeval {

struct Obj;
struct Closure;

// An atom value (`:name`), kept distinct from a plain String.
struct Atom { std::string name; };

// The dynamic runtime value. Types are erased, so the oracle is a plain dynamic
// evaluator (the checker already proved type-safety -> the oracle never hits a type
// error). `std::monostate` is Unit / nil (also the empty list, mirroring the VM's
// nil-terminated cons representation).
using RtValue = std::variant<
    std::monostate,               // 0: Unit / nil / empty-list
    int64_t,                      // 1: Int (48-bit wrapping arithmetic)
    double,                       // 2: Double
    bool,                         // 3: Bool
    std::string,                  // 4: String
    Atom,                         // 5: Atom
    std::shared_ptr<Obj>,         // 6: heap object (struct/enum/tuple/list/array/vec/bytes/map)
    std::shared_ptr<Closure>>;    // 7: first-class function / lambda

enum class ObjKind { Struct, Tuple, List, Array, Vec, Bytes, Map };

// A heap object. `type_name` names a struct / enum-variant / tuple-struct constructor
// (empty for an anonymous tuple / container). `enum_name` is the owning enum for an
// enum-variant value (empty otherwise) -- used for trait dispatch (an impl targets the
// ENUM, `impl Trait for Option`, but the value is a variant `Some`/`None`). Canon never
// needs `enum_name` (Struct and enum-variant both render `Name(...)`).
struct Obj {
    ObjKind kind = ObjKind::Struct;
    std::string type_name;
    std::string enum_name;
    bool is_tuple = false;                                   // Struct: positional fields -> `Name(e)` dump vs `Name { f: v }`
    std::vector<std::string> field_names;                   // Struct: ordered field names (for `.f` access)
    std::vector<RtValue> items;                              // fields / elements
    std::vector<uint8_t> bytes;                              // Bytes payload
    std::vector<std::pair<RtValue, RtValue>> map;            // Map entries (insertion order)
};

// A binding environment: a scope chain (name -> value + parent). Mirrors lexical scope.
struct Env {
    std::unordered_map<std::string, RtValue> vars;
    Env* parent = nullptr;
};

// A first-class function value: either a top-level `fn` or a lambda, plus (for a lambda)
// the by-value captured environment (mirroring codegen's analyze_captures).
struct Closure {
    const svc::FnItem*     fn  = nullptr;   // set for a top-level fn value
    const svc::LambdaExpr* lam = nullptr;   // set for a lambda
    std::unordered_map<std::string, RtValue> captured;   // lambda captures (by value)
};

// Which AST node kinds the oracle ACTUALLY EVALUATED during one run -- the coverage half of the
// differential, and the complement of the decline signals above. Those answer "did the oracle turn a
// construct away?"; this answers "did any differential test ever reach it at all?" -- a construct no
// test exercises declines nothing and fails nothing, so it is invisible without a positive record.
//
// DYNAMIC, deliberately: recorded where the oracle DISPATCHES, not where the parser produced a node.
// A construct sitting in an uncalled function or an untaken branch would satisfy a syntactic walk
// while never having been compared against the VM -- exactly the false comfort this removes.
// `main.cpp` merges a run's record into its global tally ONLY when the run AGREED with the bytecode,
// so "covered" means *the oracle ran it and the VM matched*, not merely *the oracle reached it*.
//
// Indexed by the raw enum value; sized generously and locked by the asserts below, so a kind added to
// Ast.h cannot silently land out of range. Fixed arrays => no allocation on the oracle's hot path.
struct Coverage {
    std::array<uint8_t, 32>  expr{};   // svc::ExprKind
    std::array<uint8_t, 8>   stmt{};   // svc::StmtKind
    std::array<uint8_t, 16>  pat{};    // svc::PatKind
    std::array<uint8_t, 128> op{};     // svc::TokKind (the operator block only; see main.cpp)
    // Compound assignment needs its OWN dimension, indexed by the BASE op. The parser normalizes
    // `a += b` to an AssignStmt carrying `TokKind::Plus` (Parser.cpp), so the eleven `*Eq` token kinds
    // never reach the oracle at all and `op` above cannot distinguish `a += b` from a plain `a + b`.
    // Without this, "all six bitwise compound forms are exercised" would be unaskable.
    std::array<uint8_t, 128> compound{};
    // The two NAME populations: opcode-backed builtins (svc::BUILTIN_FN_NAMES, Ast.h) and the
    // differentiable natives (DIFFERENTIABLE_NATIVES below). Indexed by position in those tables --
    // a string set would allocate on the oracle's call path for no gain.
    std::array<uint8_t, 64>  builtin{};
    std::array<uint8_t, 64>  native{};

    void mark_expr(svc::ExprKind k) { expr[static_cast<size_t>(k)] = 1; }
    void mark_stmt(svc::StmtKind k) { stmt[static_cast<size_t>(k)] = 1; }
    void mark_pat (svc::PatKind  k) { pat [static_cast<size_t>(k)] = 1; }
    void mark_op  (svc::TokKind  k) { op  [static_cast<size_t>(k)] = 1; }
    void mark_compound(svc::TokKind base) { compound[static_cast<size_t>(base)] = 1; }
    // By table position -- see record_builtin_call below, which owns the name lookup (it needs both
    // name tables, and one of them is declared further down beside the fixture it describes).
    void mark_builtin_at(size_t i) { builtin[i] = 1; }
    void mark_native_at (size_t i) { native [i] = 1; }
};
static_assert(static_cast<size_t>(svc::ExprKind::MapLit) < 32,  "Coverage::expr too small");
static_assert(static_cast<size_t>(svc::StmtKind::Expr)   < 8,   "Coverage::stmt too small");
static_assert(static_cast<size_t>(svc::PatKind::Bind)    < 16,  "Coverage::pat too small");
static_assert(static_cast<size_t>(svc::TokKind::Eof)     < 128, "Coverage::op too small");

// The outcome of evaluating a program via the oracle. Never throws to the caller: a
// panic / declined construct / runtime error is reported via `faulted`.
struct RunResult {
    RtValue     value;             // the program's top-level value
    std::string output;            // accumulated print/println text
    std::string err_output;        // accumulated eprint/eprintln text
    bool        faulted = false;   // a panic / declined program / runtime error occurred
    bool        unsupported = false;   // declined BY DESIGN (see the header comment) -> an honest skip
    bool        oracle_gap  = false;   // declined because the oracle lacks the construct -> a FAILURE
    std::string fault_msg;
    Coverage    coverage;          // the node kinds this run evaluated (see above)
};

// The DIFFERENTIABLE natives -- the ONLY ones the oracle may cross-check, and the required set of the
// native half of the coverage assertion. A native qualifies iff it is side-effect-free, deterministic
// and message-independent, so that two independent implementations can agree at all; every other
// native (write/delete/mkdir/listDir/time/process) is `Unsupported` BY DESIGN and must stay that way.
//
// This is a JUDGEMENT ABOUT THE TEST FIXTURE, not a property of the VM, which is why it lives here and
// is NOT derived from NativeRegistry.h: nothing about the registry can say whether `nanoTime` could be
// compared, and the coverage metric must never create pressure to pretend it could. The filesystem
// entries qualify only because the fixture pins their answers (`readFile` always Err, and the probe
// paths never exist).
//
// Two rot-guards, since nothing can derive this list: main.cpp asserts every entry is a real native
// (`native_id_of >= 0`), and NATIVE_COUNT is pinned there too, so ADDING a native breaks the build and
// forces the "is it deterministic?" decision instead of letting it default into neither set.
inline constexpr std::string_view DIFFERENTIABLE_NATIVES[] = {
    // Process-level inputs, fixed by NativeEnv below
    "args", "getEnv", "readAllStdin", "readLine",
    // Filesystem probes -- deterministic against a path that is guaranteed absent
    "fileExists", "isFile", "isDir", "fileSize", "readFile",
    // Pure parsing
    "parseInt", "parseDouble",
    // The running platform -- one answer per machine, mirrored below with the same #ifdef
    "rawOsId",
    // std::math -- pure libm, bit-identical on both sides
    "sqrt", "cbrt", "exp", "ln", "pow", "hypot", "abs",
    "sin", "cos", "tan", "asin", "acos", "atan",
    "isNaN", "isInfinite",
};

// Deterministic fixtures for that subset. run_diff builds ONE and passes it to BOTH the VM (args/stdin
// via execute(); env via the process) and this oracle -- a single source of truth, so the two sides
// cannot drift.
struct NativeEnv {
    std::vector<std::string> args;             // args()
    std::string              stdin_text;       // readLine / readAllStdin (a shared, consumed cursor)
    std::string              env_name;         // getEnv(env_name) -> Some(env_value)
    std::string              env_value;        // getEnv(any other name) -> None
};

static_assert(std::size(svc::BUILTIN_FN_NAMES)  <= 64, "Coverage::builtin too small");
static_assert(std::size(DIFFERENTIABLE_NATIVES) <= 64, "Coverage::native too small");

// Record that a named builtin / native call was actually EVALUATED. Called from the single place the
// oracle dispatches one (`try_builtin`'s handled path), so nothing can be run without being recorded.
// A name in neither table is silently ignored: the oracle also models internal prelude helpers that
// belong to no coverage population, and a native the oracle handles but that is NOT differentiable
// would be a contradiction the tables themselves have to resolve, not this lookup.
inline void record_builtin_call(Coverage& c, const std::string& name) {
    for (size_t i = 0; i < std::size(svc::BUILTIN_FN_NAMES); ++i)
        if (svc::BUILTIN_FN_NAMES[i] == name) { c.mark_builtin_at(i); return; }
    for (size_t i = 0; i < std::size(DIFFERENTIABLE_NATIVES); ++i)
        if (DIFFERENTIABLE_NATIVES[i] == name) { c.mark_native_at(i); return; }
}

// Evaluate a checked Program (from svc::parse_check / svc::check). Pure + deterministic. `nenv`
// supplies the fixed values the differentiable natives return (default: empty args / stdin / env).
RunResult eval_program(const svc::Program& prog, const NativeEnv& nenv = {});

// Canonical, comparison-ready textual form of a value (maps sorted by canonical key,
// doubles with a trailing `.0` when integral). The SAME function canonicalizes the VM
// result (main.cpp converts a NaN-boxed Value into an RtValue, then calls this), so the
// two backends cannot drift on formatting.
std::string canonicalize(const RtValue& v);

// Shared scalar formatter for doubles (shortest round-trip + `.0` when integral),
// exposed so the VM-side converter matches byte-for-byte.
std::string canon_double(double d);

} // namespace refeval
