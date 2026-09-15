#pragma once

// =============================================================================
// Compiler.h -- the public entry of the Skarn compiler: source -> Module.
//
// svc::compile drives lex -> parse -> CHECK -> lower and returns a Module (the
// bundle vmcore's execute() consumes). The pipeline REFUSES to lower a program
// the typechecker rejected: types are erased at runtime (the Gleam model), so only
// a checked program is safe to run -- a check error throws CheckFailure and no
// bytecode is produced.
//
// Module's shape is inherited from the removed dynamic front end's Module, which it
// deliberately mirrored rather than shared while both existed. Only this front end
// remains, so the duplication is gone; the shape stayed because it is exactly what
// execute() consumes. Codegen never touches the opcode handlers -- only the
// header-only Assembler seam.
//
// Namespace `svc`.
// =============================================================================

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "Value.h"
#include "StructType.h"
#include "FunctionTable.h"
#include "BytecodeIO.h" // bcio::ModuleImage -- the serializable, execute()-ready image
#include "Check.h"    // svc::TypeError (carried by CheckFailure)
#include "Loader.h"   // svc::ModuleSet -- the multi-module compile entry consumes it

namespace svc {

// One embedded/loaded prelude module: a source unit + the module prefix its items are
// tagged with (Ast.h mangle_name). The prelude is a LIST of these (the stdlib split turns
// the one $prelude blob into std::core / std::iter / std::string / ... ) -- in the pre-split
// state the list has a single { "$prelude", <full source> } entry, which mangles to bare, so
// behavior is identical to the old single-const-char* prelude. builtin_prelude() returns the
// embedded set; a driver may instead load them from disk.
struct PreludeModule {
    std::string prefix;   // module_prefix stamped on this unit's items (e.g. "$prelude", "std::core")
    std::string source;   // the .skn source text of this prelude module
};

// A compiled module: everything execute() needs to run the program. Field-for-field
// the same shape as the former dynamic front end's Module (minus the advisory `diagnostics`, which the
// static checker owns via CheckResult). After a successful run the top-level program
// value is left in register 0.
struct Module {
    std::vector<uint32_t>    bytecode;
    std::vector<Value>       constants;
    // Const-array literals (LOAD_CONST_ARRAY): one inner vector per `const NAME: Array[T] = [...]`,
    // holding its scalar element immediates. execute() builds each into a GC-rooted KIND_ARRAY.
    std::vector<std::vector<Value>> const_arrays;
    std::vector<StructType>  struct_types;
    std::vector<std::string> string_literals;
    std::vector<FnInfo>      function_table;
    std::vector<uint16_t>    trait_table;
    uint32_t                 trait_table_width  = 0;
    uint32_t                 trait_method_count = 0;
    uint8_t                  top_frame_size = 0;
    // Per-instruction source position (index == instruction index) + per-function-id
    // names, for VM fault reporting. Cold path only; never GC roots.
    std::vector<uint32_t>    line_table;
    std::vector<uint32_t>    column_table;
    std::vector<std::string> function_names;
    std::vector<std::string> function_modules;   // parallel to function_names (owning module prefix; fault caret)
    // The checker's advisory warnings (must-use / unused bindings), carried through so a
    // driver can print them. Never affects the emitted bytecode.
    std::vector<TypeError>   warnings;
};

// Thrown by compile() when the typechecker rejected the program: carries every
// TypeError the checker reported. `what()` summarizes the first error with its
// position; the full list is in errors().
class CheckFailure : public std::runtime_error {
public:
    explicit CheckFailure(std::vector<TypeError> errs);
    const std::vector<TypeError>& errors() const noexcept { return errors_; }
private:
    std::vector<TypeError> errors_;
};

// Inlining of small non-recursive direct calls -- a process-wide codegen
// knob, read by compile() / compile_modules() when they construct the Codegen. A knob rather than
// a parameter on all four compile entry points, deliberately: its whole purpose is that ONE binary
// can compile the same program both ways, which is what makes the A/B free of a build confound.
// Single-threaded compiler, set once at startup.
//
// ON by default since the measurement: +8.7 % (full render) / +10.3 % (preview) on demo/raytracer
// against a null control that read 0.00 %, and 0 % on demo/aes256, which gets exactly one
// expansion -- the payoff is workload-shaped, never negative. `static_vmrun --no-inline` turns it
// off. The accepted cost is +27.5 % bytecode and a flatter fault trace: a trap inside an expanded
// callee reports the CALL SITE, and the callee's frames are absent (decision D4).
void set_inline_calls(bool on);
bool inline_calls();
// Print the inline drift report (candidate sites seen by the measure vs expansions taken by
// emit) to stderr after codegen. Development instrumentation; independent of set_inline_calls
// only in that it does nothing while inlining is off.
void set_inline_report(bool on);
bool inline_report();
// Inliner tuning, for the S4 threshold/depth sweep: max callee body size in AST nodes, and how
// deep an expansion may nest (1 = a call inside an expanded body stays a call). Settable so one
// binary can produce every data point -- a rebuild per point would vary code layout between them.
void set_inline_max_nodes(int n);
int  inline_max_nodes();
void set_inline_max_depth(int d);
int  inline_max_depth();

// Scalar Replacement of Aggregates -- the same shape of process-wide codegen
// knob as inlining above, and for the same reason: ONE binary must be able to compile a program
// both ways, or a rebuild sits between the two measured points.
//
// OFF by default while the slices land. Predicted worth ~4.7 % on demo/raytracer, of which roughly
// 70 % is deleted GET_PROPs inside inlined callee bodies and only 30 % the deleted allocation --
// so it is a field-read optimisation more than an allocation one, and it PRESUPPOSES inlining.
void set_sroa(bool on);
bool sroa();
// Print the SROA drift report (bindings the pre-pass permitted vs dissolutions emit took) to
// stderr after codegen. Development instrumentation; does nothing while SROA is off.
void set_sroa_report(bool on);
bool sroa_report();
// Dissolve only a struct with at most N fields -- the register-budget knob, since a dissolved
// binding costs one register per field. **0 dissolves nothing**, which is the A/B's null control:
// SROA on, bytecode byte-identical, true effect exactly zero.
void set_sroa_max_fields(int n);
int  sroa_max_fields();

// Compile source text into a Module. Throws LexError / ParseError (malformed input),
// CheckFailure (type errors), or CodegenError (a construct not yet lowered).
//
// `prelude_source` is parsed as a SEPARATE unit and its items are prepended to the user
// program (P.items ++ U.items), with Program::prelude_item_count marking the boundary --
// the same shape as the former dynamic front end. The whole combined program is typechecked (the
// prelude is our guard, so it is checked too, not trusted). nullptr / empty => no prelude
// (the path all existing core-language tests take). The static prelude (Option/Result +
// typed combinators) is returned by builtin_prelude() below; a driver / test harness passes
// it here to make Option/Result + `?` available without the user declaring them.
Module compile(const char* source, const std::vector<PreludeModule>* prelude = nullptr);
// Convenience overloads: pass the embedded set by reference (builtin_prelude()), or a single
// monolithic "$prelude" source string (a synthetic test prelude). Both forward to the primary.
inline Module compile(const char* source, const std::vector<PreludeModule>& prelude) {
    return compile(source, &prelude);
}
inline Module compile(const char* source, const char* prelude_source) {
    std::vector<PreludeModule> mods;
    if (prelude_source && *prelude_source) mods.push_back({ "$prelude", prelude_source });
    return compile(source, &mods);
}

// Compile an already-loaded MODULE SET (from svc::load_modules) into one Module -- the
// multi-module sibling of compile(). The prelude items are prepended, then every module's
// items in the loader's TOPOLOGICAL order (dependencies before dependents; the entry module
// last); the combined program is checked + tree-shaken + lowered exactly as compile() does.
// A single root module with no imports produces byte-identical output to compile(). Same
// throw contract (CheckFailure / CodegenError). Namespace/mangling semantics land in Slice 2b;
// Slice 2a is a flat namespace (import/use items are inert).
Module compile_modules(ModuleSet modules, const std::vector<PreludeModule>* prelude = nullptr);
inline Module compile_modules(ModuleSet modules, const std::vector<PreludeModule>& prelude) {
    return compile_modules(std::move(modules), &prelude);
}

// Parse (source and, if given, prelude -- prelude items first) and TYPECHECK, returning the
// checked Program with every Expr::ty filled. Throws LexError / ParseError / CheckFailure exactly
// as compile() does (it IS compile()'s front half). Exposed so a consumer that needs the checked
// typed AST directly -- e.g. a tree-walking reference interpreter used as a differential test
// oracle -- gets byte-for-byte the same combined+checked program the bytecode path sees, without
// re-implementing the parse-two-concatenate-then-check logic. If `out_warnings` is non-null it
// receives the checker's advisory warnings. The returned program is NOT tree-shaken (compile()
// shakes its own copy before codegen).
Program parse_check(const char* source, const std::vector<PreludeModule>* prelude = nullptr,
                    std::vector<TypeError>* out_warnings = nullptr);
inline Program parse_check(const char* source, const std::vector<PreludeModule>& prelude,
                           std::vector<TypeError>* out_warnings = nullptr) {
    return parse_check(source, &prelude, out_warnings);
}
inline Program parse_check(const char* source, const char* prelude_source,
                           std::vector<TypeError>* out_warnings = nullptr) {
    std::vector<PreludeModule> mods;
    if (prelude_source && *prelude_source) mods.push_back({ "$prelude", prelude_source });
    return parse_check(source, &mods, out_warnings);
}

// The multi-module sibling of parse_check: assemble one Program from the prelude items +
// the module set's items in topological order (entry last), then TYPECHECK it. Same throw
// contract. Exposed so a differential oracle sees the exact combined+checked program the
// bytecode path (compile_modules) does. Not tree-shaken (compile_modules shakes its own copy).
Program parse_check_modules(ModuleSet modules, const std::vector<PreludeModule>* prelude = nullptr,
                            std::vector<TypeError>* out_warnings = nullptr);
inline Program parse_check_modules(ModuleSet modules, const std::vector<PreludeModule>& prelude,
                                   std::vector<TypeError>* out_warnings = nullptr) {
    return parse_check_modules(std::move(modules), &prelude, out_warnings);
}

// The built-in static prelude, as an ordered list of prelude modules (embedded from the
// std/*.skn sources by the pre-build). Pre-split this is a single { "$prelude", <source> }
// entry (mangles to bare = the old monolithic prelude); the stdlib split turns it into
// std::core / std::iter / std::string / ... . static_vmrun may instead load these from disk;
// the test harness passes this embedded set to compile().
const std::vector<PreludeModule>& builtin_prelude();

// Copy the execution-relevant fields of a compiled Module into a vmcore bcio::ModuleImage
// (the serializable, execute()-ready subset -- everything except the compile-time-only
// `warnings`). This is the bridge to bytecode serialization (vmcore/BytecodeIO.h): the
// compiler owns the copy because vmcore must not depend on svc::Module.
bcio::ModuleImage module_to_image(const Module& m);

} // namespace svc
