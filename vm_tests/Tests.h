#pragma once

#include <iostream>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <format>
#include <cmath>
#include <filesystem>   // temp-dir round-trip for test_native_dirops (listDir / mkdir)
#include <fstream>      // std::ofstream -- seed the temp dir in test_native_dirops
#include <thread>       // the concurrency probes: several execute() instances at once
#include <atomic>       // ditto -- the ThreadSlotTable unit test's cross-thread flags
#include <chrono>       // test_rooted_pool_release times one pool release
#include <optional>     // ... and holds the pool in an optional so reset() is the destructor
#include "Assembler.h"
#include "Disassembler.h"
#include "VM_Resources.h"
#include "Heap.h"
#include "VM.h"
#include "GlobalEnv.h"
#include "Opcodes.h"
#include "HashTable.h"
#include "StringInterner.h"
#include "Execute.h"
#include "BytecodeIO.h"    // SKBC CARR round-trip in test_const_array
#include "Debug.h"
#include "TypeUniverse.h"   // BuiltinTid / BUILTIN_COUNT / TRAIT_METHOD_NONE (trait dispatch)
#include "NativeRegistry.h" // NativeId ids (nanoTime / millisTime -> test_native_time)
#include "Natives.h"        // build_native_table() -- the real native registry
#include "ThreadSlotTable.h"// the per-thread fault rendezvous -> test_thread_slot_table
#include "ValueCodec.h"     // value graph <-> byte buffer -> test_value_codec_*

// =============================================================================
// Minimal test harness -- lets the suite FAIL THE BUILD (non-zero exit code).
//
// The suite is still a set of `inline void test_*()` that print a human-readable
// trace; this only adds a pass/fail tally so CI can gate on it. Every assertion
// goes through check(); every unexpected exception is recorded as a failure
// (record_fail() from a test's own catch, or the run() backstop in main() for
// tests without one). main() prints the tally and returns non-zero on any
// failure. See "Tests" in docs/VirtualMachine.md.
// =============================================================================
struct TestStats {
    int passed       = 0;  // assertions that passed  (check() == true)
    int failed       = 0;  // assertions/throws that failed (check()==false / record_fail)
    int tests_passed = 0;  // whole tests with no failed assertion and no throw (via run())
    int tests_failed = 0;  // whole tests with >=1 failed assertion or an escaped throw
};
inline TestStats g_test_stats;

// Print + record one assertion. Returns `ok` for convenience.
inline bool check(bool ok) {
    if (ok) ++g_test_stats.passed; else ++g_test_stats.failed;
    std::cout << (ok ? "CORRECT!\n" : "WRONG!\n");
    return ok;
}

// Record an unexpected exception as a failure (used from a test's catch block).
inline void record_fail(const char* why) {
    ++g_test_stats.failed;
    std::cerr << "Test failed: " << why << "\n";
}

// =============================================================================
// Native functions -- callable from VM bytecode via CALL_NATIVE
// =============================================================================

// Adds all integer arguments and returns the sum
static Value native_add(Value* args, uint8_t nargs, Context*) {
    int64_t sum = 0;
    for (uint8_t i = 0; i < nargs; ++i)
        sum += args[i].asSigned48();
    return Value::fromSigned48(sum);
}

// Prints all arguments to stdout, returns nil
static Value native_print(Value* args, uint8_t nargs, Context*) {
    for (uint8_t i = 0; i < nargs; ++i) {
        if (i > 0) std::cout << ", ";
        if (args[i].isInt())
            std::cout << args[i].asSigned48();
        else if (args[i].isBool())
            std::cout << (args[i].asBool() ? "true" : "false");
        else if (args[i].isNil())
            std::cout << "nil";
        else
            std::cout << "?";
    }
    std::cout << "\n";
    return Value::fromNil();
}

// Runs a full GC cycle and returns the number of bytes reclaimed as an Int.
// Used by test_frame_gc_roots to trigger a collection at a known program point
// and observe exactly what was freed.
static Value native_collect(Value*, uint8_t, Context* ctx) {
    const size_t freed = ctx->vm->heap->collect(ctx);
    return Value::fromSigned48(static_cast<int64_t>(freed));
}

// The GC-helper tests drive native_collect through the registry CALL_NATIVE path -- the
// only mode since the baked-pointer path (load_native_ptr / Assembler::call_native) was
// retired earlier. This 1-entry table maps id 0 -> native_collect; execute_gc() wires
// it into execute() so the GC tests keep short call sites instead of spelling out the long
// positional path to execute()'s native_table parameter.
static const std::vector<NativeFunc> GC_NTAB = { native_collect };

inline VM_Resources execute_gc(const std::vector<uint32_t>& bc, Heap* heap, GlobalEnv* globals,
                               uint8_t top, const std::vector<StructType>* structs = nullptr) {
    return execute(bc, heap, globals, /*interner*/nullptr, top, /*const_pool*/nullptr, structs,
                   /*string_literals*/nullptr, /*atom_names*/nullptr, /*fn_table*/nullptr,
                   /*out*/nullptr, /*trait_table*/nullptr, 0, 0,
                   /*line_table*/nullptr, /*function_names*/nullptr, /*column_table*/nullptr,
                   &GC_NTAB);
}

// =============================================================================
// test_factorial, test_sum_tco, test_load_const_wide,
// test_set_and_branch, test_cmov  (unchanged -- omitted for brevity)
// =============================================================================
// A direct CALL whose target is >32767 instructions away overflows the 16-bit `call` offset. The Assembler's
// relax_far_calls() must route it through a trampoline island (at the function boundary nearest the midpoint)
// so it still assembles and runs. `mid` is an (uncalled) function providing a central boundary, as real dense
// programs always have.
inline void test_far_call_relaxation() {
    std::cout << "=== far_call_relaxation ===\n";
    constexpr uint8_t FRAME = 1;
    Assembler as;
    as.func("f", FRAME);
    as.func("mid", 1);
    as.label("main");
    as.CALL("f");                       // f is far below; return value lands in r[FRAME]
    as.J(OpCode::HALT);
    for (int k = 0; k < 16000; ++k) as.J(OpCode::NOP);
    as.label("mid");                    // a central function boundary for the island
    as.J(OpCode::RET);
    for (int k = 0; k < 16000; ++k) as.J(OpCode::NOP);
    as.label("f");
    as.C2(OpCode::LOAD_CONST, 0, 42);
    as.J(OpCode::RET);
    bool assembled = false;
    int64_t result = -1;
    try {
        const auto bytecode = as.assemble();          // must NOT throw -- relaxation kicks in
        assembled = true;
        auto res = execute(bytecode, nullptr, nullptr, nullptr, FRAME);
        result = res.get_reg_base()[FRAME].asSigned48();
    } catch (const std::exception& e) { record_fail(e.what()); }
    std::cout << "  far CALL (>32767 away) assembled + ran: result=" << result << "\n";
    check(assembled && result == 42);
}

inline void test_factorial() {
    constexpr uint8_t FACTORIAL_FRAME = 2;
    Assembler as;
    as.func("factorial", FACTORIAL_FRAME);
    as.label("main");
    as.C2  (OpCode::LOAD_CONST, FACTORIAL_FRAME, 5);
    as.CALL("factorial");
    as.J   (OpCode::HALT);
    as.label("factorial");
    as.C2(OpCode::LOAD_CONST, 1, 1);
    as.B (OpCode::BGE_INT, 0, 1, "fact_recursive");
    as.C2(OpCode::LOAD_CONST, 0, 1);
    as.J (OpCode::RET);
    as.label("fact_recursive");
    as.R6(OpCode::SUB_INT, FACTORIAL_FRAME, 0, 1);
    as.CALL("factorial");
    as.R6(OpCode::MUL_INT, 0, 0, FACTORIAL_FRAME);
    as.J (OpCode::RET);
    const auto bytecode = as.assemble();
    std::cout << "=== factorial(5) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main"); dis.add_label(3, "factorial"); dis.add_label(7, "fact_recursive");
    dis.print();
    std::cout << "Running...\n";
    try {
        // Top-level places the argument at r[FACTORIAL_FRAME] and reads the result
        // there; with the decoupled contract the slide is the CALLER's frame size,
        // so the top-level frame size must equal that boundary (FACTORIAL_FRAME).
        auto res = execute(bytecode, nullptr, nullptr, nullptr, FACTORIAL_FRAME);
        const int64_t result = res.get_reg_base()[FACTORIAL_FRAME].asSigned48();
        std::cout << std::format("factorial(5) = {}\n", result);
        check(result == 120);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

inline void test_sum_tco() {
    constexpr uint8_t SUM_FRAME = 3;
    Assembler as;
    as.func("sum", SUM_FRAME);
    as.label("main");
    as.C2(OpCode::LOAD_CONST, SUM_FRAME + 0, 100);
    as.C2(OpCode::LOAD_CONST, SUM_FRAME + 1,   0);
    as.CALL("sum");
    as.J(OpCode::HALT);
    as.label("sum");
    as.C2(OpCode::LOAD_CONST, 2, 0);
    as.B (OpCode::BEQ_INT, 0, 2, "sum_base");
    as.R6(OpCode::ADD_INT, 2, 1, 0);
    as.C2(OpCode::DECR, 0);
    as.R6(OpCode::MOV, 1, 2, 0);
    as.TCO_CALL("sum");
    as.label("sum_base");
    as.R6(OpCode::MOV, 0, 1, 0);
    as.J(OpCode::RET);
    const auto bytecode = as.assemble();
    std::cout << "=== sum_tco(100) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main"); dis.add_label(4, "sum");
    dis.add_label(8, "sum_recursive"); dis.add_label(12, "sum_base");
    dis.print();
    std::cout << "Running...\n";
    try {
        // Top-level places the two arguments at r[SUM_FRAME], r[SUM_FRAME+1] and
        // reads the result at r[SUM_FRAME]; the top-level frame size is that
        // boundary (SUM_FRAME) since the CALL now slides by the caller frame size.
        auto res = execute(bytecode, nullptr, nullptr, nullptr, SUM_FRAME);
        const int64_t result = res.get_reg_base()[SUM_FRAME].asSigned48();
        std::cout << std::format("sum(100) = {}\n", result);
        check(result == 5050);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

inline void test_load_const_wide() {
    constexpr int64_t VALUE = 100'000;
    Assembler as;
    as.label("main");
    as.load_const(0, VALUE);
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== load_const_wide (value = 100'000) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res = execute(bytecode);
        const int64_t result = res.get_reg_base()[0].asSigned48();
        std::cout << std::format("r0 = {}\n", result);
        check(result == VALUE);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

inline void test_set_and_branch() {
    Assembler as;
    as.label("main");
    as.C2(OpCode::LOAD_CONST, 0, 6);
    as.C2(OpCode::LOAD_CONST, 1, 6);
    as.C2(OpCode::LOAD_CONST, 2, 7);
    as.R6(OpCode::SET_EQ, 3, 0, 1);
    as.B1(OpCode::BF, 3, "was_false_1");
    as.C2(OpCode::LOAD_CONST, 4, 1);
    as.J (OpCode::J, "next");
    as.label("was_false_1");
    as.C2(OpCode::LOAD_CONST, 4, 0);
    as.label("next");
    as.R6(OpCode::SET_EQ, 3, 0, 2);
    as.B1(OpCode::BF, 3, "was_false_2");
    as.C2(OpCode::LOAD_CONST, 5, 0);
    as.J (OpCode::J, "done");
    as.label("was_false_2");
    as.C2(OpCode::LOAD_CONST, 5, 1);
    as.label("done");
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== set_eq + bf ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode);
        auto* regs = res.get_reg_base();
        const int64_t r4 = regs[4].asSigned48();
        const int64_t r5 = regs[5].asSigned48();
        std::cout << std::format("6==6 branch skipped: {} (expect 1)\n", r4);
        std::cout << std::format("6==7 branch taken:   {} (expect 1)\n", r5);
        check(r4 == 1 && r5 == 1);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

inline void test_cmov() {
    Assembler as;
    as.label("main");
    as.C2(OpCode::LOAD_CONST, 0, 3); as.C2(OpCode::LOAD_CONST, 1, 7);
    as.R6(OpCode::SET_LT, 2, 0, 1); as.Q4(OpCode::CMOV, 4, 1, 0, 2);
    as.C2(OpCode::LOAD_CONST, 0, 9); as.C2(OpCode::LOAD_CONST, 1, 2);
    as.R6(OpCode::SET_LT, 2, 0, 1); as.Q4(OpCode::CMOV, 5, 1, 0, 2);
    as.C2(OpCode::LOAD_CONST, 0, 5); as.C2(OpCode::LOAD_CONST, 1, 5);
    as.R6(OpCode::SET_LT, 2, 0, 1); as.Q4(OpCode::CMOV, 6, 1, 0, 2);
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== cmov / branchless max ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode);
        auto* regs = res.get_reg_base();
        const int64_t r4 = regs[4].asSigned48();
        const int64_t r5 = regs[5].asSigned48();
        const int64_t r6 = regs[6].asSigned48();
        std::cout << std::format("max(3,7)={} max(9,2)={} max(5,5)={}\n", r4, r5, r6);
        check(r4 == 7 && r5 == 9 && r6 == 5);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// (test_call_native was removed earlier together with the legacy baked-pointer
//  CALL_NATIVE path it exercised. Its native_add / native_print coverage lives on in
//  test_native_registry below, driven through the id-based native_table registry.)
// =============================================================================

// =============================================================================
// test_native_registry -- the id-based native-call path (the compiler path).
//
// Instead of baking a function ADDRESS into the bytecode (load_native_ptr /
// call_native, the legacy in-process form), the caller passes a native_table to
// execute() and the bytecode carries only a stable NativeId index. CALL_NATIVE
// then resolves ctx->vm->native_table[id]. This is what makes the compiler's
// readFile / writeFile serializable + address-free. See NativeRegistry.h.
//
//   Test 1: native at id 0 (native_add) over (5, 7)  -> r1 = 12
//   Test 2: native at id 1 (native_print) over (42, 1) -> r6 = nil (proves a
//           non-zero id indexes correctly)
// =============================================================================
inline void test_native_registry() {
    Assembler as;
    as.label("main");

    // id 0: native_add(&r2, 2)
    as.C2(OpCode::LOAD_CONST, 2, 5);                  // r2 = 5
    as.C2(OpCode::LOAD_CONST, 3, 7);                  // r3 = 7
    as.call_native_id(1, 0, 2, 2, 0);                 // r1 = table[0](&r2, 2)

    // id 1: native_print(&r7, 2)
    as.C2(OpCode::LOAD_CONST, 7, 42);                 // r7 = 42
    as.C2(OpCode::LOAD_CONST, 8,  1);                 // r8 = 1
    as.call_native_id(6, 5, 7, 2, 1);                 // r6 = table[1](&r7, 2)

    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();

    std::cout << "=== native_registry (id-based CALL_NATIVE) ===\n";
    try {
        // The registry: id 0 -> native_add, id 1 -> native_print.
        std::vector<NativeFunc> ntab = { native_add, native_print };
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0,
                             nullptr, nullptr, nullptr, &ntab);
        auto* regs = res.get_reg_base();
        const int64_t add_result = regs[1].asSigned48();
        std::cout << std::format("table[0] native_add(5, 7) = {} (expect 12)\n", add_result);
        std::cout << std::format("table[1] native_print returned nil: {}\n",
            regs[6].isNil() ? "yes" : "no");
        check(add_result == 12 && regs[6].isNil());
    }
    catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_call_native_high_registers -- CALL_NATIVE must reach the WHOLE 64-register
// file, not just the low 32.
//
// Regression for the q4 -> r6 re-encode. CALL_NATIVE used to ride the
// q4 layout (four 5-bit fields), so any operand register >= 32 silently truncated
// in Release: with the encoding below, rd=r40 became r8 and the id register r33
// became r1 -- i.e. a DIFFERENT native was resolved and its result written to a
// DIFFERENT register, with no diagnostic. An ordinary Skarn program with ~34 live
// locals at a native call site reproduced it. Debug caught it only as an assert
// inside the Q4 builder.
//
// Registers here are deliberately all >= 32 (the old 5-bit ceiling):
//   r33 = the NativeId temp, r34/r35 = the arguments, r40 = the result.
// r1 and r8 hold sentinels: they are exactly where the truncated fields pointed
// (33 & 31 == 1, 40 & 31 == 8), so a regression overwrites r8 and reads its native
// id out of r1 instead of r33.
// =============================================================================
inline void test_call_native_high_registers() {
    Assembler as;
    as.label("main");
    as.C2(OpCode::LOAD_CONST,  1, 111);               // r1  = sentinel (old truncated id register)
    as.C2(OpCode::LOAD_CONST,  8, 222);               // r8  = sentinel (old truncated result register)
    as.C2(OpCode::LOAD_CONST, 34, 5);                 // r34 = 5
    as.C2(OpCode::LOAD_CONST, 35, 7);                 // r35 = 7
    as.call_native_id(40, 33, 34, 2, 0);              // r40 = table[0](&r34, 2)
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();

    std::cout << "=== call_native_high_registers (r6 fields, registers >= 32) ===\n";
    try {
        std::vector<NativeFunc> ntab = { native_add, native_print };
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, 42, nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0,
                             nullptr, nullptr, nullptr, &ntab);
        auto* regs = res.get_reg_base();
        const int64_t result = regs[40].asSigned48();
        std::cout << std::format("r40 = native_add(5, 7) = {} (expect 12)\n", result);
        std::cout << std::format("sentinels intact: r1 = {} (expect 111), r8 = {} (expect 222)\n",
            regs[1].asSigned48(), regs[8].asSigned48());
        check(result == 12 && regs[1].asSigned48() == 111 && regs[8].asSigned48() == 222);
    }
    catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_native_time -- the first Plain-return natives (nanoTime / millisTime),
// exercised through the REAL registry (build_native_table). Both are zero-arg
// and return a bare Int (no Ok/Err wrap): the "unfehlbar" native pattern.
//
//   r0 = millisTime()          -> wall-clock ms since epoch (> 2020)
//   r2 = nanoTime(); r3 = nanoTime()  -> monotonic, r3 >= r2
// The zero-arg CALL_NATIVE passes a dummy base register (never dereferenced).
// =============================================================================
inline void test_native_time() {
    Assembler as;
    as.label("main");
    as.call_native_id(0, 1, 0, 0, NATIVE_MILLIS_TIME);   // r0 = millisTime()
    as.call_native_id(2, 4, 0, 0, NATIVE_NANO_TIME);     // r2 = nanoTime()
    as.call_native_id(3, 4, 0, 0, NATIVE_NANO_TIME);     // r3 = nanoTime()
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();

    std::cout << "=== native_time (nanoTime / millisTime, Plain Int) ===\n";
    try {
        std::vector<NativeFunc> ntab = build_native_table();
        auto res  = execute(bytecode, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0,
                            nullptr, nullptr, nullptr, &ntab);
        auto* regs = res.get_reg_base();
        const int64_t ms    = regs[0].asSigned48();
        const int64_t nano1 = regs[2].asSigned48();
        const int64_t nano2 = regs[3].asSigned48();
        std::cout << std::format("millisTime() = {} (Int: {}, > 1.6e12: {})\n",
            ms, regs[0].isInt() ? "yes" : "no", ms > 1600000000000LL ? "yes" : "no");
        std::cout << std::format("nanoTime() twice = {}, {} (Int: {}/{}, monotonic: {})\n",
            nano1, nano2, regs[2].isInt() ? "yes" : "no", regs[3].isInt() ? "yes" : "no",
            nano2 >= nano1 ? "yes" : "no");
        check(regs[0].isInt() && ms > 1600000000000LL &&
              regs[2].isInt() && regs[3].isInt() && nano2 >= nano1);
    }
    catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_native_procctx -- the args() native (Plain -> Array[String]), the first
// native that BUILDS a multi-object heap structure (an array of freshly allocated
// strings, rooted while filled). Driven through the real registry with a
// script_args vector wired into execute()'s new trailing parameter.
//
//   r0 = args()  -> ["alpha", "beta"]
//   r2 = len(r0) -> 2 ;  r4 = r0[0] -> "alpha"
// =============================================================================
inline void test_native_procctx() {
    Assembler as;
    as.label("main");
    as.call_native_id(0, 1, 0, 0, NATIVE_ARGS);   // r0 = args()
    as.R6(OpCode::LEN, 2, 0, 0);                   // r2 = len(r0)
    as.C2(OpCode::LOAD_CONST, 3, 0);               // r3 = 0 (index)
    as.R6(OpCode::ARRAY_GET, 4, 0, 3);             // r4 = r0[0]
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();

    std::cout << "=== native_procctx (args() -> Array[String]) ===\n";
    try {
        Heap heap;
        StringInterner interner;
        std::vector<std::string> sargs = { "alpha", "beta" };
        std::vector<NativeFunc> ntab = build_native_table();
        // top_frame_size = 8 covers r0..r4 as GC roots (r0 holds the heap array).
        auto res = execute(bytecode, &heap, nullptr, &interner, 8, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0,
                           nullptr, nullptr, nullptr, &ntab, &sargs);
        auto* regs = res.get_reg_base();
        const int64_t n = regs[2].asSigned48();
        bool first_ok = false;
        if (regs[4].isPtr()) {
            GcObject* s = GcObject::from_slots(regs[4].asPtr());
            if (s->kind == GcObject::KIND_STRING)
                first_ok = std::string(s->bytes(), s->string_length()) == "alpha";
        }
        std::cout << std::format("len(args()) = {} (expect 2); args()[0] == \"alpha\": {}\n",
            n, first_ok ? "yes" : "no");
        check(n == 2 && first_ok);
    }
    catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_native_dirops -- listDir() (Result-model native returning a raw KIND_ARRAY
// of leaf names) and mkdir() (create_directories -p, returning raw nil), driven
// through the real registry over a fresh temp directory. At the id-path level the
// native returns its BARE result (KIND_ARRAY / nil); the Ok/Err wrap is the
// compiler's job (exercised in static_compiler_tests), so here we inspect the raw array.
//
//   r0 = listDir(dirWithOneFile) -> ["one.txt"] ; r2 = len -> 1 ; r4 = r0[0]
//   r5 = mkdir("<root>/made/deep")  -> nil, and the dir tree now exists (mkdir -p)
//   r6 = mkdir(dirWithOneFile)      -> nil (idempotent: an existing dir is Ok)
// =============================================================================
inline void test_native_dirops() {
    namespace fs = std::filesystem;
    std::cout << "=== native_dirops (listDir / mkdir) ===\n";
    try {
        std::error_code ec;
        const fs::path root   = fs::temp_directory_path() / "vmtest_dirops";
        fs::remove_all(root, ec);                       // clean slate
        const fs::path listme = root / "listme";
        fs::create_directories(listme, ec);
        { std::ofstream(listme / "one.txt") << "x"; }   // one known entry, host-side
        const fs::path newdir = root / "made" / "deep"; // parent "made" does NOT exist yet

        const std::string s_listme = listme.string();
        const std::string s_newdir = newdir.string();

        Assembler as;
        as.label("main");
        as.load_str(10, s_listme);                      // r10 = path arg for listDir/mkdir
        as.call_native_id(0, 9, 10, 1, NATIVE_LIST_DIR);// r0 = listDir(r10) (raw array)
        as.R6(OpCode::LEN, 2, 0, 0);                    // r2 = len(r0)
        as.C2(OpCode::LOAD_CONST, 3, 0);                // r3 = 0
        as.R6(OpCode::ARRAY_GET, 4, 0, 3);              // r4 = r0[0]
        as.load_str(11, s_newdir);                      // r11 = path arg for mkdir -p
        as.call_native_id(5, 9, 11, 1, NATIVE_MAKE_DIR);// r5 = mkdir(r11) (raw nil)
        as.call_native_id(6, 9, 10, 1, NATIVE_MAKE_DIR);// r6 = mkdir(r10) again -> nil (idempotent)
        as.J(OpCode::HALT);
        const auto bytecode = as.assemble();

        Heap heap;
        StringInterner interner;
        const auto slits = as.string_literals();
        std::vector<NativeFunc> ntab = build_native_table();
        // top_frame_size = 16 covers r0..r11 as GC roots (r0 array, r4/r10/r11 strings).
        auto res = execute(bytecode, &heap, nullptr, &interner, 16, nullptr, nullptr,
                           &slits, nullptr, nullptr, nullptr, nullptr, 0, 0,
                           nullptr, nullptr, nullptr, &ntab);
        auto* regs = res.get_reg_base();
        const int64_t n = regs[2].asSigned48();
        bool name_ok = false;
        if (regs[4].isPtr()) {
            GcObject* s = GcObject::from_slots(regs[4].asPtr());
            if (s->kind == GcObject::KIND_STRING)
                name_ok = std::string(s->bytes(), s->string_length()) == "one.txt";
        }
        const bool made_ok = regs[5].isNil() && fs::exists(newdir, ec);   // -p created parents
        const bool idem_ok = regs[6].isNil();                             // existing dir -> nil
        std::cout << std::format("len(listDir) = {} (expect 1); [0] == \"one.txt\": {}\n",
            n, name_ok ? "yes" : "no");
        std::cout << std::format("mkdir -p created tree: {}; mkdir(existing) -> nil: {}\n",
            made_ok ? "yes" : "no", idem_ok ? "yes" : "no");
        check(n == 1 && name_ok && made_ok && idem_ok);
        fs::remove_all(root, ec);                       // cleanup
    }
    catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_native_stdin -- readLine() (Option-model: a line, or nil at EOF) and
// readAllStdin() (Plain: the whole remaining stream to EOF). Both read the VM's
// input sink (ctx->vm->in), which execute()'s new trailing `in` parameter lets us
// point at an istringstream -- so the test never blocks on real stdin. At the
// id-path level readLine returns a BARE String / nil (the Some/None wrap is the
// compiler's job); we inspect the raw result.
//
//   in = "first\nsecond\nthird\n"
//   r0 = readLine()      -> "first"
//   r1 = readAllStdin()  -> "second\nthird\n"  (the remainder)
//   r3 = readLine()      -> nil                (stream now at EOF)
// =============================================================================
inline void test_native_stdin() {
    std::cout << "=== native_stdin (readLine / readAllStdin) ===\n";
    try {
        Assembler as;
        as.label("main");
        as.call_native_id(0, 8, 0, 0, NATIVE_READ_LINE);       // r0 = readLine()
        as.call_native_id(1, 8, 0, 0, NATIVE_READ_ALL_STDIN);  // r1 = readAllStdin()
        as.call_native_id(3, 8, 0, 0, NATIVE_READ_LINE);       // r3 = readLine() at EOF -> nil
        as.J(OpCode::HALT);
        const auto bytecode = as.assemble();

        Heap heap;
        StringInterner interner;
        std::istringstream input("first\nsecond\nthird\n");
        std::vector<NativeFunc> ntab = build_native_table();
        // top_frame_size = 8 covers r0..r3 as GC roots (r0/r1 hold heap strings).
        auto res = execute(bytecode, &heap, nullptr, &interner, 8, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0,
                           nullptr, nullptr, nullptr, &ntab, nullptr, &input);
        auto* regs = res.get_reg_base();
        auto as_str = [](const Value& v) -> std::string {
            if (!v.isPtr()) return std::string();
            GcObject* s = GcObject::from_slots(v.asPtr());
            return s->kind == GcObject::KIND_STRING
                 ? std::string(s->bytes(), s->string_length()) : std::string();
        };
        const std::string line0 = as_str(regs[0]);
        const std::string rest  = as_str(regs[1]);
        const bool eof_none     = regs[3].isNil();
        std::cout << std::format("readLine() = \"{}\" (expect \"first\")\n", line0);
        std::cout << std::format("readAllStdin() = {} (expect \"second\\nthird\\n\")\n",
            rest == "second\nthird\n" ? "match" : "MISMATCH");
        std::cout << std::format("readLine() at EOF -> nil: {}\n", eof_none ? "yes" : "no");
        check(line0 == "first" && rest == "second\nthird\n" && eof_none);
    }
    catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_native_process -- rawRun(argv, input) at the id/registry path (the prelude's
// run/runWith reshape its result into a ProcessOutput struct, tested at the compiler
// level). rawRun returns a bare Array[3] {stdoutBytes, stderrBytes, exitInt} on a
// successful spawn, or an error String on a spawn failure (Rust Command::output
// semantics: a non-zero exit is a SUCCESS whose exitCode is data; only "could not
// start" is the error String). The one program per case: build the argv KIND_ARRAY,
// call rawRun, GET_KIND-branch the result, and on the array path decode stdout via
// BYTES_TO_STR + read the exit slot. The program spawned per case is the one each platform is
// guaranteed to have -- cmd.exe on Windows, the POSIX shell elsewhere -- for determinism.
// =============================================================================
inline void test_native_process() {
    std::cout << "=== native_process (rawRun) ===\n";
    try {
        // Result of one rawRun: either an error String (is_err), or a success carrying
        // the decoded stdout + the exit code.
        struct RawResult { bool is_err; std::string out; long long exit; };
        auto run_raw = [](const std::vector<std::string>& argv,
                          const std::string* stdin_data) -> RawResult {
            Assembler as;
            as.label("main");
            // Build the argv KIND_ARRAY at r0 (scratch r2 = element string, r3 = index).
            as.C2(OpCode::ALLOC, 0, static_cast<int16_t>(argv.size()));
            for (size_t i = 0; i < argv.size(); ++i) {
                as.load_str(2, argv[i]);
                as.C2(OpCode::LOAD_CONST, 3, static_cast<int16_t>(i));
                as.R6(OpCode::ARRAY_SET, 2, 0, 3);          // argv[i] = r2
            }
            // input at r1: a KIND_BYTES (stdin feed) or a non-ptr Int(0) (native reads
            // that as "no stdin", since it is not a KIND_BYTES pointer).
            if (stdin_data) { as.load_str(4, *stdin_data); as.R6(OpCode::BYTES_FROM_STR, 1, 4, 0); }
            else            { as.C2(OpCode::LOAD_CONST, 1, 0); }
            as.call_native_id(10, 9, 0, 2, NATIVE_RUN_PROCESS);   // r10 = rawRun(&window[0], 2)
            // Branch on the result kind: KIND_STRING (1) => spawn error, else the Array[3].
            as.R6(OpCode::GET_KIND, 5, 10, 0);
            as.C2(OpCode::LOAD_CONST, 6, 1);
            as.B(OpCode::BEQ_INT, 5, 6, "is_err");
            as.C2(OpCode::LOAD_CONST, 7, 0); as.R6(OpCode::ARRAY_GET, 11, 10, 7);  // r11 = stdout bytes
            as.R6(OpCode::BYTES_TO_STR, 12, 11, 0);                                // r12 = stdout string
            as.C2(OpCode::LOAD_CONST, 8, 2); as.R6(OpCode::ARRAY_GET, 13, 10, 8);  // r13 = exit int
            as.J(OpCode::HALT);
            as.label("is_err");
            as.J(OpCode::HALT);
            const auto bytecode = as.assemble();

            Heap heap;
            StringInterner interner;
            const auto slits = as.string_literals();
            std::vector<NativeFunc> ntab = build_native_table();
            auto res = execute(bytecode, &heap, nullptr, &interner, 16, nullptr, nullptr,
                               &slits, nullptr, nullptr, nullptr, nullptr, 0, 0,
                               nullptr, nullptr, nullptr, &ntab);
            auto* regs = res.get_reg_base();
            auto as_str = [](const Value& v) -> std::string {
                if (!v.isPtr()) return std::string();
                GcObject* s = GcObject::from_slots(v.asPtr());
                return s->kind == GcObject::KIND_STRING
                     ? std::string(s->bytes(), s->string_length()) : std::string();
            };
            const bool err = regs[10].isPtr() &&
                GcObject::from_slots(regs[10].asPtr())->kind == GcObject::KIND_STRING;
            if (err) return { true, as_str(regs[10]), 0 };
            return { false, as_str(regs[12]), regs[13].asSigned48() };
        };

        // 1) echo -> stdout carries "hello", exit 0 (a plain successful spawn).
#ifdef _WIN32
        RawResult echo = run_raw({ "cmd", "/c", "echo", "hello" }, nullptr);
#else
        RawResult echo = run_raw({ "echo", "hello" }, nullptr);
#endif
        const bool echo_ok = !echo.is_err && echo.exit == 0 &&
                             echo.out.rfind("hello", 0) == 0;
        std::cout << std::format("echo -> exit {}, stdout starts \"hello\": {}\n",
            echo.exit, echo.out.rfind("hello", 0) == 0 ? "yes" : "no");

        // 2) exit 3 -> a NON-zero exit is still a success (Ok), exitCode == 3.
#ifdef _WIN32
        RawResult ex = run_raw({ "cmd", "/c", "exit", "3" }, nullptr);
#else
        RawResult ex = run_raw({ "sh", "-c", "exit 3" }, nullptr);
#endif
        const bool exit_ok = !ex.is_err && ex.exit == 3;
        std::cout << std::format("exit 3 -> is_err {}, exitCode {} (expect 3)\n",
            ex.is_err ? "yes" : "no", ex.exit);

        // 3) grep/findstr with fed stdin -> the child sees the input (exit 0 == "needle" found)
        //    and echoes the matching line back, proving the pipe + concurrent drain
        //    path delivers stdin AND captures stdout together.
        const std::string feed = "has needle here\n";
#ifdef _WIN32
        RawResult fs = run_raw({ "cmd", "/c", "findstr", "needle" }, &feed);
#else
        RawResult fs = run_raw({ "grep", "needle" }, &feed);
#endif
        const bool sort_ok = !fs.is_err && fs.exit == 0 &&
                             fs.out.find("needle") != std::string::npos;
        std::cout << std::format("grep/findstr <stdin -> exit {} (expect 0), stdout has \"needle\": {}\n",
            fs.exit, fs.out.find("needle") != std::string::npos ? "yes" : "no");

        // 4) a non-existent program -> a spawn error (the error String, not an Array).
        RawResult bad = run_raw({ "definitely_not_a_real_program_zzz_qeg" }, nullptr);
        const bool bad_ok = bad.is_err;
        std::cout << std::format("bad program -> is_err (spawn failure): {}\n",
            bad.is_err ? "yes" : "no");

        // 5) rawOsId -- the zero-arg sibling that tells a program which platform it is on (the
        // prelude's currentOs wraps it, and sh() branches on the answer to pick the shell above).
        // Plain Int return: no Ok/Err wrap, no allocation, so the register holds the id directly.
        Assembler os_as;
        os_as.label("main");
        os_as.call_native_id(1, 0, 0, 0, NATIVE_OS_ID);   // r1 = rawOsId() -- 0 args, dummy window base
        os_as.J(OpCode::HALT);
        const auto os_code = os_as.assemble();
        Heap os_heap;
        StringInterner os_interner;
        std::vector<NativeFunc> os_ntab = build_native_table();
        auto os_res = execute(os_code, &os_heap, nullptr, &os_interner, 8, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0,
                              nullptr, nullptr, nullptr, &os_ntab);
        const long long os_id = os_res.get_reg_base()[1].asSigned48();
#ifdef _WIN32
        const long long os_want = 0;
#elif defined(__APPLE__)
        const long long os_want = 1;
#else
        const long long os_want = 2;
#endif
        const bool os_ok = os_id == os_want;
        std::cout << std::format("rawOsId -> {} (expect {} for this build)\n", os_id, os_want);

        check(echo_ok && exit_ok && sort_ok && bad_ok && os_ok);
    }
    catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_gc -- verifies tri-color mark & sweep without the dispatch loop.
//
// Scenario A: single live object
//   - alloc object A (3 slots), store pointer in a fake register window
//   - run collect() -> A must survive (still reachable from root)
//
// Scenario B: dead object collected
//   - alloc object B, do NOT store pointer anywhere
//   - alloc object C, store pointer in register window
//   - run collect() -> B must be gone, C must survive
//
// Scenario C: object graph (A -> B)
//   - alloc B (no root pointer), alloc A (root pointer)
//   - store pointer to B in A.slot[0]
//   - run collect() -> both A and B must survive (B reachable via A)
//
// Scenario D: cycle (A -> B -> A)
//   - alloc A and B, cross-link them, root points to A only
//   - run collect() -> both must survive, no infinite loop
// =============================================================================
inline void test_gc() {
    std::cout << "=== test_gc ===\n";

    VM_Resources res;
    Value* win = res.get_reg_base();

    // Shared minimal Context
    Context ctx{};
    ctx.window_ptr         = win;
    ctx.ret_stack_base     = res.get_ret_base();
    ctx.ret_stack_ptr      = res.get_ret_base();
    ctx.frame_size_ptr     = res.get_frame_size_base();
    ctx.current_frame_size = 4; // r0..r3 are live roots

    bool all_ok = true;

    // ------------------------------------------------------------------
    // Scenario A: single live object survives
    // ------------------------------------------------------------------
    {
        for (size_t i = 0; i < 4; ++i) win[i] = Value{};
        Heap heap(64 * 1024);

        GcObject* a = heap.alloc(GcObject::KIND_ARRAY, 2);
        assert(a && "Scenario A: alloc failed");
        a->slots()[0] = Value::fromSigned48(111);
        a->slots()[1] = Value::fromSigned48(222);
        win[0] = Value::fromPtr(a->slots());

        const size_t used_before = heap.used();
        heap.collect(&ctx);
        const size_t used_after = heap.used();

        GcObject* a2   = GcObject::from_slots(win[0].asPtr());
        const bool ok  = win[0].isPtr()
                      && a2->slots()[0].asSigned48() == 111
                      && a2->slots()[1].asSigned48() == 222
                      && used_after == used_before;

        std::cout << std::format("  Scenario A (live object survives):      {}\n",
            ok ? "PASS" : "FAIL");
        all_ok &= ok;
    }

    // ------------------------------------------------------------------
    // Scenario B: unreachable object is collected, live object survives
    // ------------------------------------------------------------------
    {
        for (size_t i = 0; i < 4; ++i) win[i] = Value{};
        Heap heap(64 * 1024);

        GcObject* b = heap.alloc(GcObject::KIND_ARRAY, 1); // no root -> dead
        assert(b && "Scenario B: alloc B failed");
        b->slots()[0] = Value::fromSigned48(999);

        GcObject* c = heap.alloc(GcObject::KIND_ARRAY, 1);
        assert(c && "Scenario B: alloc C failed");
        c->slots()[0] = Value::fromSigned48(777);
        win[1] = Value::fromPtr(c->slots());

        const size_t freed = heap.collect(&ctx);

        GcObject* c2   = GcObject::from_slots(win[1].asPtr());
        const bool ok  = freed > 0
                      && win[1].isPtr()
                      && c2->slots()[0].asSigned48() == 777;

        std::cout << std::format("  Scenario B (dead object collected):     {}\n",
            ok ? "PASS" : "FAIL");
        all_ok &= ok;
    }

    // ------------------------------------------------------------------
    // Scenario C: object graph A -> B, only A is a root
    // ------------------------------------------------------------------
    {
        for (size_t i = 0; i < 4; ++i) win[i] = Value{};
        Heap heap(64 * 1024);

        GcObject* b = heap.alloc(GcObject::KIND_ARRAY, 1);
        assert(b && "Scenario C: alloc B failed");
        b->slots()[0] = Value::fromSigned48(42);

        GcObject* a = heap.alloc(GcObject::KIND_ARRAY, 1);
        assert(a && "Scenario C: alloc A failed");
        a->slots()[0] = Value::fromPtr(b->slots()); // A -> B

        win[2] = Value::fromPtr(a->slots()); // only A is a root

        const size_t freed = heap.collect(&ctx);

        GcObject* a2   = GcObject::from_slots(win[2].asPtr());
        GcObject* b2   = GcObject::from_slots(a2->slots()[0].asPtr());
        const bool ok  = freed == 0
                      && b2->slots()[0].asSigned48() == 42;

        std::cout << std::format("  Scenario C (object graph A->B):         {}\n",
            ok ? "PASS" : "FAIL");
        all_ok &= ok;
    }

    // ------------------------------------------------------------------
    // Scenario D: cycle A -> B -> A, root holds A only
    // ------------------------------------------------------------------
    {
        for (size_t i = 0; i < 4; ++i) win[i] = Value{};
        Heap heap(64 * 1024);

        GcObject* a = heap.alloc(GcObject::KIND_ARRAY, 1);
        assert(a && "Scenario D: alloc A failed");
        GcObject* b = heap.alloc(GcObject::KIND_ARRAY, 1);
        assert(b && "Scenario D: alloc B failed");

        a->slots()[0] = Value::fromPtr(b->slots()); // A -> B
        b->slots()[0] = Value::fromPtr(a->slots()); // B -> A

        win[3] = Value::fromPtr(a->slots()); // only A is a root

        const size_t freed = heap.collect(&ctx);

        GcObject* a2   = GcObject::from_slots(win[3].asPtr());
        GcObject* b2   = GcObject::from_slots(a2->slots()[0].asPtr());
        GcObject* a3   = GcObject::from_slots(b2->slots()[0].asPtr());
        const bool ok  = freed == 0
                      && a3 == a2; // cycle pointer is consistent

        std::cout << std::format("  Scenario D (cycle A->B->A):             {}\n",
            ok ? "PASS" : "FAIL");
        all_ok &= ok;
    }

    check(all_ok);
}

// =============================================================================
// test_globals -- NEW: verify GlobalEnv behavior
//
// Scenario A: simple STORE_GLOBAL / LOAD_GLOBAL via bytecode
// Scenario B: GlobalEnv slots are GC roots (survive GC)
// =============================================================================
inline void test_globals() {
    std::cout << "=== globals ===\n";

    Heap heap(64 * 1024);
    GlobalEnv globals(heap);

    const uint8_t g0 = globals.define();
    const uint8_t g1 = globals.define();

    bool all_ok = true;

    // Scenario A ...
    {
        Assembler as;
        as.label("main");
        as.C2(OpCode::LOAD_CONST, 0, 42);
        as.STORE_GLOBAL(g0, 0);
        as.LOAD_GLOBAL(1, g0);

        as.C2(OpCode::LOAD_CONST, 2, 7);
        as.STORE_GLOBAL(g1, 2);
        as.LOAD_GLOBAL(3, g1);

        as.J(OpCode::HALT);

        const auto bytecode = as.assemble();

        Disassembler dis(bytecode.data(), bytecode.size());
        dis.add_label(0, "main");
        dis.print();

        std::cout << "Running...\n";
        try {
            auto res   = execute(bytecode, &heap, &globals);
            auto* regs = res.get_reg_base();

            const int64_t r1  = regs[1].asSigned48();
            const int64_t r3  = regs[3].asSigned48();
            const int64_t gv0 = globals.get(g0).asSigned48();
            const int64_t gv1 = globals.get(g1).asSigned48();

            const bool ok = (r1 == 42) && (r3 == 7) && (gv0 == 42) && (gv1 == 7);

            std::cout << std::format("globals[g0]={} r1={}\n", gv0, r1);
            std::cout << std::format("globals[g1]={} r3={}\n", gv1, r3);
            std::cout << std::format("  Scenario A (bytecode globals):         {}\n",
                ok ? "PASS" : "FAIL");
            all_ok &= ok;
        }
        catch (const std::exception& e) {
            // Diagnostic only -- the failure is accounted for by the
            // check(all_ok) at the end of this test (all_ok is set false here).
            std::cerr << "Test failed: " << e.what() << "\n";
            all_ok = false;
        }
    }

    // Scenario B ...
    {
        VM_Resources res;
        Value* win = res.get_reg_base();
        for (size_t i = 0; i < 4; ++i) win[i] = Value{};

        VM vm{};
        vm.heap = &heap;

        Context ctx{};
        ctx.window_ptr         = win;
        ctx.vm                 = &vm;
        ctx.ret_stack_base     = res.get_ret_base();
        ctx.ret_stack_ptr      = res.get_ret_base();
        ctx.ret_stack_limit    = res.get_ret_base() + VM_Resources::RET_FRAME_COUNT;
        ctx.frame_size_ptr     = res.get_frame_size_base();
        ctx.current_frame_size = 0;

        GcObject* obj = heap.alloc(GcObject::KIND_ARRAY, 2);
        assert(obj && "Scenario B: alloc failed");
        obj->slots()[0] = Value::fromSigned48(123);
        obj->slots()[1] = Value::fromSigned48(456);

        globals.set(g0, Value::fromPtr(obj->slots()));

        const size_t used_before = heap.used();
        const size_t freed       = heap.collect(&ctx);
        const size_t used_after  = heap.used();

        GcObject* obj2 = GcObject::from_slots(globals.get(g0).asPtr());
        const bool ok =
            globals.get(g0).isPtr() &&
            obj2->slots()[0].asSigned48() == 123 &&
            obj2->slots()[1].asSigned48() == 456 &&
            freed == 0 &&
            used_after == used_before;

        std::cout << std::format("  Scenario B (globals are GC roots):      {}\n",
            ok ? "PASS" : "FAIL");
        all_ok &= ok;

        globals.set(g0, Value{});
    }

    check(all_ok);
}

// =============================================================================
// test_alloc -- verify ALLOC opcode and heap integration.
//
// Scenario A:
//   - bytecode allocates an array object with 3 slots into r0
//   - verify r0 is a heap pointer
//   - verify all slots are initialized to Undefined
// =============================================================================
inline void test_alloc() {
    std::cout << "=== alloc ===\n";

    Heap heap(64 * 1024);

    Assembler as;
    as.label("main");
    as.ALLOC(0, 3);      // r0 = new array[3]
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main");
    dis.print();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr);
        auto* regs = res.get_reg_base();

        const bool is_ptr = regs[0].isPtr();
        GcObject* obj = is_ptr ? GcObject::from_slots(regs[0].asPtr()) : nullptr;

        const bool ok =
            is_ptr &&
            obj &&
            obj->slot_count() == 3 &&
            obj->kind == GcObject::KIND_ARRAY &&
            obj->slots()[0].isUndefined() &&
            obj->slots()[1].isUndefined() &&
            obj->slots()[2].isUndefined();

        std::cout << std::format("r0 is ptr: {}\n", is_ptr ? "yes" : "no");
        if (obj) {
            std::cout << std::format("slot_count={} kind={}\n", obj->slot_count(), obj->kind);
        }
        std::cout << std::format("  Scenario A (ALLOC basic):               {}\n",
            ok ? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_alloc_gc_retry -- force ALLOC to fail once, trigger GC, then retry.
//
// Setup:
//   - tiny heap: 64 bytes
//   - allocate one live object ("survivor") and store it in a global root
//   - allocate one dead tail object to fill the heap
//
// Then execute bytecode:
//   ALLOC r0, 2
//   HALT
//
// Expected:
//   - first ALLOC attempt fails (heap full)
//   - GC runs
//   - survivor remains alive via GlobalEnv root
//   - dead tail object is reclaimed
//   - ALLOC retry succeeds and writes a fresh pointer to r0
// =============================================================================
inline void test_alloc_gc_retry() {
    std::cout << "=== alloc_gc_retry ===\n";

    // 64 bytes total:
    //   survivor: 8 + 2*8 = 24
    //   dead tail: 8 + 2*8 = 24
    //   used before bytecode = 48
    //   ALLOC(2) needs 24 -> first attempt fails (48 + 24 > 64)
    Heap heap(64);
    GlobalEnv globals(heap);
    const uint8_t g0 = globals.define();

    // Live object, kept alive through GlobalEnv
    GcObject* survivor = heap.alloc(GcObject::KIND_ARRAY, 2);
    assert(survivor && "test_alloc_gc_retry: survivor alloc failed");
    survivor->slots()[0] = Value::fromSigned48(111);
    survivor->slots()[1] = Value::fromSigned48(222);
    globals.set(g0, Value::fromPtr(survivor->slots()));

    // Dead tail object, not rooted
    GcObject* dead_tail = heap.alloc(GcObject::KIND_ARRAY, 2);
    assert(dead_tail && "test_alloc_gc_retry: dead_tail alloc failed");
    dead_tail->slots()[0] = Value::fromSigned48(333);
    dead_tail->slots()[1] = Value::fromSigned48(444);

    Assembler as;
    as.label("main");
    as.ALLOC(0, 2); // must fail once, trigger GC, then succeed
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main");
    dis.print();

    std::cout << "Running...\n";
    try {
        const size_t used_before = heap.used();

        auto res   = execute(bytecode, &heap, &globals);
        auto* regs = res.get_reg_base();

        const size_t used_after = heap.used();

        const bool alloc_ok = regs[0].isPtr();
        GcObject*   new_obj  = alloc_ok ? GcObject::from_slots(regs[0].asPtr()) : nullptr;

        const Value survivor_value = globals.get(g0);
        const bool  survivor_ok    = survivor_value.isPtr();
        GcObject*   survivor_after = survivor_ok ? GcObject::from_slots(survivor_value.asPtr()) : nullptr;

        const bool ok =
            alloc_ok &&
            new_obj &&
            new_obj->slot_count() == 2 &&
            new_obj->kind == GcObject::KIND_ARRAY &&
            new_obj->slots()[0].isUndefined() &&
            new_obj->slots()[1].isUndefined() &&
            survivor_ok &&
            survivor_after &&
            survivor_after->slots()[0].asSigned48() == 111 &&
            survivor_after->slots()[1].asSigned48() == 222 &&
            used_before == 48 &&
            used_after == 48; // dead tail freed, new object allocated

        std::cout << std::format("used_before={} used_after={}\n", used_before, used_after);
        std::cout << std::format("survivor slots = {}, {}\n",
            survivor_after ? survivor_after->slots()[0].asSigned48() : -1,
            survivor_after ? survivor_after->slots()[1].asSigned48() : -1);
        std::cout << std::format("  Scenario A (ALLOC gc-retry path):       {}\n",
            ok ? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_alloc_global_root -- ALLOC object, store it in a global, then force GC.
//
// Scenario A:
//   - bytecode allocates an array object with 2 slots into r0
//   - bytecode stores r0 into globals[g0]
//   - after execute(), C++ writes payload values into the object
//   - a manual GC run must keep the object alive via the global root
// =============================================================================
inline void test_alloc_global_root() {
    std::cout << "=== alloc_global_root ===\n";

    Heap heap(64 * 1024);
    GlobalEnv globals(heap);
    const uint8_t g0 = globals.define();

    Assembler as;
    as.label("main");
    as.ALLOC(0, 2);          // r0 = new array[2]
    as.STORE_GLOBAL(g0, 0);  // globals[g0] = r0
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main");
    dis.print();

    std::cout << "Running...\n";
    try {
        auto res = execute(bytecode, &heap, &globals);
        (void)res;

        const bool global_is_ptr = globals.get(g0).isPtr();
        GcObject* obj = global_is_ptr ? GcObject::from_slots(globals.get(g0).asPtr()) : nullptr;

        // Write recognizable payload after bytecode execution so we can verify
        // survival across a later GC cycle.
        if (obj) {
            obj->slots()[0] = Value::fromSigned48(123);
            obj->slots()[1] = Value::fromSigned48(456);
        }

        VM_Resources gc_res;
        Value* win = gc_res.get_reg_base();
        for (size_t i = 0; i < 4; ++i) win[i] = Value{};

        VM vm{};
        vm.heap = &heap;

        Context ctx{};
        ctx.window_ptr         = win;
        ctx.vm                 = &vm;
        ctx.ret_stack_base     = gc_res.get_ret_base();
        ctx.ret_stack_ptr      = gc_res.get_ret_base();
        ctx.ret_stack_limit    = gc_res.get_ret_base() + VM_Resources::RET_FRAME_COUNT;
        ctx.frame_size_ptr     = gc_res.get_frame_size_base();
        ctx.current_frame_size = 0;

        const size_t used_before = heap.used();
        const size_t freed       = heap.collect(&ctx);
        const size_t used_after  = heap.used();

        const bool survived = globals.get(g0).isPtr();
        GcObject* obj_after = survived ? GcObject::from_slots(globals.get(g0).asPtr()) : nullptr;

        // Copying GC: the object is evacuated, so the pre-collection handle
        // `obj` is now a stale from-space address. Everything is inspected via
        // `obj_after`, re-fetched from the (forwarded) global root.
        const bool ok =
            global_is_ptr &&
            obj &&                 // allocation succeeded before the collection
            survived &&
            obj_after &&
            obj_after->slot_count() == 2 &&
            obj_after->kind == GcObject::KIND_ARRAY &&
            obj_after->slots()[0].asSigned48() == 123 &&
            obj_after->slots()[1].asSigned48() == 456 &&
            freed == 0 &&
            used_after == used_before;

        std::cout << std::format("used_before={} used_after={} freed={}\n",
            used_before, used_after, freed);
        if (obj_after) {
            std::cout << std::format("survivor slots = {}, {}\n",
                obj_after->slots()[0].asSigned48(),
                obj_after->slots()[1].asSigned48());
        }
        std::cout << std::format("  Scenario A (ALLOC survives via global): {}\n",
            ok ? "PASS" : "FAIL");
        check(ok);

        globals.set(g0, Value{});
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_string_gc -- verify KIND_STRING allocation and GC behavior//
// Scenario A:
//   - allocate one live string and keep it alive through a global root
//   - allocate one dead tail string with no root
//   - run GC
//   - verify the live string survives with intact bytes
//   - verify the dead tail string is reclaimed
// =============================================================================
inline void test_string_gc() {
    std::cout << "=== string_gc ===\n";

    Heap heap(64 * 1024);
    GlobalEnv globals(heap);
    const uint8_t g0 = globals.define();

    GcObject* live = heap.alloc_string("hello");
    assert(live && "test_string_gc: live string alloc failed");
    globals.set(g0, Value::fromPtr(live->bytes()));

    GcObject* dead = heap.alloc_string("dead");
    assert(dead && "test_string_gc: dead string alloc failed");
    const size_t dead_total = dead->padded_total_bytes();

    VM_Resources res;
    Value* win = res.get_reg_base();
    for (size_t i = 0; i < 4; ++i) win[i] = Value{};

    VM vm{};
    vm.heap = &heap;

    Context ctx{};
    ctx.window_ptr         = win;
    ctx.vm                 = &vm;
    ctx.ret_stack_base     = res.get_ret_base();
    ctx.ret_stack_ptr      = res.get_ret_base();
    ctx.ret_stack_limit    = res.get_ret_base() + VM_Resources::RET_FRAME_COUNT;
    ctx.frame_size_ptr     = res.get_frame_size_base();
    ctx.current_frame_size = 0;

    const size_t used_before = heap.used();
    const size_t freed       = heap.collect(&ctx);
    const size_t used_after  = heap.used();

    const bool survived = globals.get(g0).isPtr();
    GcObject* live_after = survived ? GcObject::from_slots(globals.get(g0).asPtr()) : nullptr;

    const bool ok =
        survived &&
        live_after &&
        live_after->kind == GcObject::KIND_STRING &&
        live_after->string_length() == 5 &&
        std::string_view(live_after->bytes(), live_after->string_length()) == "hello" &&
        freed == dead_total &&
        used_after + freed == used_before;

    std::cout << std::format("used_before={} used_after={} freed={}\n",
        used_before, used_after, freed);
    if (live_after) {
        std::cout << std::format("string_len={} bytes=\"{}\"\n",
            live_after->string_length(),
            std::string_view(live_after->bytes(), live_after->string_length()));
    }
    std::cout << std::format("  Scenario A (rooted string survives):    {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);

    globals.set(g0, Value{});
}

// =============================================================================
// test_string_pool_policy -- verify content-based hashing/equality for strings.
//
// Scenario A:
//   - two different string objects with the same bytes compare equal
//   - equal strings produce the same hash
//   - different strings compare unequal
// =============================================================================
inline void test_string_pool_policy() {
    std::cout << "=== string_pool_policy ===\n";

    Heap heap(64 * 1024);

    GcObject* hello_a = heap.alloc_string("hello");
    GcObject* hello_b = heap.alloc_string("hello");
    GcObject* world   = heap.alloc_string("world");

    assert(hello_a && "hello_a alloc failed");
    assert(hello_b && "hello_b alloc failed");
    assert(world   && "world alloc failed");

    Value v_hello_a = Value::fromPtr(hello_a->bytes());
    Value v_hello_b = Value::fromPtr(hello_b->bytes());
    Value v_world   = Value::fromPtr(world->bytes());

    const uint32_t h_a = StringPoolPolicy::hash(v_hello_a);
    const uint32_t h_b = StringPoolPolicy::hash(v_hello_b);
    const uint32_t h_w = StringPoolPolicy::hash(v_world);

    const bool ok =
        StringPoolPolicy::isEqual(v_hello_a, v_hello_b) &&
        !StringPoolPolicy::isEqual(v_hello_a, v_world) &&
        h_a == h_b;

    std::cout << std::format("hash(hello_a)={} hash(hello_b)={} hash(world)={}\n", h_a, h_b, h_w);
    std::cout << std::format("  Scenario A (content hash/equality):     {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_string_hash_table -- verify HashTable<StringPoolPolicy>.
//
// Scenario A:
//   - insert with one string object
//   - lookup with a different string object of same content succeeds
//   - lookup with different content fails
//   - remove via same-content different object succeeds
// =============================================================================
inline void test_string_hash_table() {
    std::cout << "=== string_hash_table ===\n";

    Heap heap(64 * 1024);

    GcObject* hello_a = heap.alloc_string("hello");
    GcObject* hello_b = heap.alloc_string("hello");
    GcObject* world   = heap.alloc_string("world");

    assert(hello_a && "hello_a alloc failed");
    assert(hello_b && "hello_b alloc failed");
    assert(world   && "world alloc failed");

    Value k_hello_a = Value::fromPtr(hello_a->bytes());
    Value k_hello_b = Value::fromPtr(hello_b->bytes());
    Value k_world   = Value::fromPtr(world->bytes());

    HashTable<StringPoolPolicy> table;

    table.set(k_hello_a, Value::fromSigned48(123));

    const Value found_same_content = table.getOrUndefined(k_hello_b);
    const Value found_other        = table.getOrUndefined(k_world);

    const bool lookup_ok =
        found_same_content.isInt() &&
        found_same_content.asSigned48() == 123 &&
        found_other.isUndefined();

    const bool remove_ok = table.remove(k_hello_b);
    const Value after_remove = table.getOrUndefined(k_hello_a);

    const bool ok =
        lookup_ok &&
        remove_ok &&
        after_remove.isUndefined();

    std::cout << std::format("lookup(hello_b) = {}\n",
        found_same_content.isInt() ? found_same_content.asSigned48() : -1);
    std::cout << std::format("remove(hello_b) = {}\n", remove_ok ? "true" : "false");
    std::cout << std::format("  Scenario A (string hash table):         {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_string_interner -- verify content-based string interning.
//
// Scenario A:
//   - intern "hello" twice
//   - verify both returned Values are identical
//   - verify heap usage does not grow on the second intern
//   - intern "world" and verify it is different
//   - verify lookup works for both present and absent strings
// =============================================================================
inline void test_string_interner() {
    std::cout << "=== string_interner ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    const size_t used0 = heap.used();

    Value hello_a = interner.intern("hello", heap);
    const size_t used1 = heap.used();

    Value hello_b = interner.intern("hello", heap);
    const size_t used2 = heap.used();

    Value world = interner.intern("world", heap);
    const size_t used3 = heap.used();

    Value found_hello = interner.find("hello");
    Value found_world = interner.find("world");
    Value found_absent = interner.find("absent");

    const bool ok =
        hello_a.isPtr() &&
        hello_b.isPtr() &&
        world.isPtr() &&
        hello_a == hello_b &&
        hello_a != world &&
        used1 > used0 &&
        used2 == used1 &&
        used3 > used2 &&
        found_hello == hello_a &&
        found_world == world &&
        found_absent.isUndefined();

    std::cout << std::format("used0={} used1={} used2={} used3={}\n",
        used0, used1, used2, used3);
    std::cout << std::format("hello_a == hello_b: {}\n", (hello_a == hello_b) ? "yes" : "no");
    std::cout << std::format("hello_a == world:   {}\n", (hello_a == world) ? "yes" : "no");
    std::cout << std::format("  Scenario A (string interning):          {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_string_interner_gc_root -- interned strings act as strong GC roots.
//
// Scenario A:
//   - intern "hello"
//   - keep it only in the StringInterner
//   - run GC with no register roots and no global roots
//   - verify the string survives
// =============================================================================
inline void test_string_interner_gc_root() {
    std::cout << "=== string_interner_gc_root ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Value hello = interner.intern("hello", heap);
    const size_t used_before = heap.used();

    VM_Resources res;
    Value* win = res.get_reg_base();
    for (size_t i = 0; i < 4; ++i) {
        win[i] = Value{};
    }

    VM vm{};
    vm.heap     = &heap;
    vm.interner = &interner;   // interner is a strong root: collect() reaches it via ctx->vm

    Context ctx{};
    ctx.window_ptr         = win;
    ctx.vm                 = &vm;
    ctx.ret_stack_base     = res.get_ret_base();
    ctx.ret_stack_ptr      = res.get_ret_base();
    ctx.ret_stack_limit    = res.get_ret_base() + VM_Resources::RET_FRAME_COUNT;
    ctx.frame_size_ptr     = res.get_frame_size_base();
    ctx.current_frame_size = 0;

    const size_t freed = heap.collect(&ctx);
    const size_t used_after = heap.used();

    Value found = interner.find("hello");
    const bool survived = found.isPtr();
    GcObject* obj = survived ? GcObject::from_slots(found.asPtr()) : nullptr;

    // Copying GC: the interned string is evacuated and the interner table is
    // forwarded, so the post-collection handle `found` differs in bits from the
    // stale pre-collection `hello` (identity is preserved only among rooted
    // references, which the local `hello` is not). Survival is proven by content
    // via the re-fetched `found`.
    const bool ok =
        hello.isPtr() &&
        survived &&
        obj &&
        obj->kind == GcObject::KIND_STRING &&
        obj->string_length() == 5 &&
        std::string_view(obj->bytes(), obj->string_length()) == "hello" &&
        freed == 0 &&
        used_after == used_before;

    std::cout << std::format("used_before={} used_after={} freed={}\n",
        used_before, used_after, freed);
    if (obj) {
        std::cout << std::format("string_len={} bytes=\"{}\"\n",
            obj->string_length(),
            std::string_view(obj->bytes(), obj->string_length()));
    }
    std::cout << std::format("  Scenario A (interner is strong root):   {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_string_interner_gc_retry -- force intern() to fail once, trigger GC,
// then retry successfully.
//
// Setup:
//   - tiny heap: 32 bytes
//   - intern "hello" first (16 bytes total, padded up from 14)
//   - allocate one dead tail string "dead" (16 bytes total, padded up from 13)
//   - used before retry = 32
//   - interning "world" needs 16 bytes -> first attempt fails
//
// Expected:
//   - GC runs
//   - "hello" survives because the interner is a strong root source
//   - dead tail string is reclaimed
//   - "world" allocation succeeds on retry
// =============================================================================
inline void test_string_interner_gc_retry() {
    std::cout << "=== string_interner_gc_retry ===\n";

    Heap heap(32);
    StringInterner interner;

    // Interned live string
    VM_Resources res;
    Value* win = res.get_reg_base();
    for (size_t i = 0; i < 4; ++i) {
        win[i] = Value{};
    }

    VM vm{};
    vm.heap     = &heap;
    vm.interner = &interner;   // interner is a strong root: collect() reaches it via ctx->vm

    Context ctx{};
    ctx.window_ptr         = win;
    ctx.vm                 = &vm;
    ctx.ret_stack_base     = res.get_ret_base();
    ctx.ret_stack_ptr      = res.get_ret_base();
    ctx.ret_stack_limit    = res.get_ret_base() + VM_Resources::RET_FRAME_COUNT;
    ctx.frame_size_ptr     = res.get_frame_size_base();
    ctx.current_frame_size = 0;

    Value hello = interner.intern("hello", heap, &ctx);

    // Dead tail string, not interned and not rooted
    GcObject* dead_tail = heap.alloc_string("dead");
    assert(dead_tail && "test_string_interner_gc_retry: dead_tail alloc failed");
    const size_t dead_total = dead_tail->padded_total_bytes();

    const size_t used_before = heap.used();

    Value world = interner.intern("world", heap, &ctx);
    const size_t used_after = heap.used();

    Value hello_after = interner.find("hello");
    Value world_after = interner.find("world");

    const bool hello_ok = hello_after.isPtr();
    const bool world_ok = world_after.isPtr();

    GcObject* hello_obj = hello_ok ? GcObject::from_slots(hello_after.asPtr()) : nullptr;
    GcObject* world_obj = world_ok ? GcObject::from_slots(world_after.asPtr()) : nullptr;

    // Copying GC: the GC that fires inside intern("world") evacuates "hello",
    // so the pre-collection handle `hello` is stale by design; `hello_after`
    // (re-fetched post-collection) is the live, forwarded pointer. `world` is
    // returned by the retry allocation after that GC, so it is already current
    // and `world_after == world` still holds. Identity between the two live
    // interned strings is checked via `hello_after != world_after`.
    const bool ok =
        hello.isPtr() &&
        world.isPtr() &&
        world_after == world &&
        hello_after != world_after &&
        hello_obj &&
        world_obj &&
        hello_obj->kind == GcObject::KIND_STRING &&
        world_obj->kind == GcObject::KIND_STRING &&
        std::string_view(hello_obj->bytes(), hello_obj->string_length()) == "hello" &&
        std::string_view(world_obj->bytes(), world_obj->string_length()) == "world" &&
        used_before == 32 &&
        used_after == (16 + 16); // "hello" survived, "dead" freed, "world" added (padded)

    std::cout << std::format("used_before={} used_after={} dead_total={}\n",
        used_before, used_after, dead_total);
    std::cout << std::format("hello survives: {} world interned: {}\n",
        hello_ok ? "yes" : "no", world_ok ? "yes" : "no");
    std::cout << std::format("  Scenario A (interner gc-retry path):    {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// Cheney copying-collector regression tests.
//
// These pin down the three properties that distinguish the copying collector
// from the previous suffix-only bump allocator:
//   1. test_gc_moves            -- survivors are physically relocated, with
//                                  their contents intact and roots rewritten.
//   2. test_gc_interleaved_reclaim -- an interior dead object between two live
//                                  ones is reclaimed (the old allocator could
//                                  only reclaim a trailing suffix, so this test
//                                  fails under it and passes under Cheney).
//   3. test_gc_root_convergence -- one object referenced from two roots (a
//                                  register and the interner) forwards to a
//                                  single address; identity is preserved among
//                                  live references.
// (Cycle-through-a-move and graph-through-a-move are already covered by
// test_gc scenarios C and D, which assert pointer consistency after the copy.)
// =============================================================================

// Small helper: fresh VM_Resources plus a factory for a bare Context with
// `frame_size` live register roots. The caller writes roots into
// win[0..frame_size). Context is built on demand (not stored as a member) so
// its alignas(64) does not pad this helper (MSVC C4324).
struct GcTestCtx {
    VM_Resources res;
    VM           vm;           // heap wired in the ctor; tests may set .interner etc.
    Heap*        heap;
    uint8_t      frame_size;
    Value*       win;

    explicit GcTestCtx(Heap* h, uint8_t fs) : heap(h), frame_size(fs), win(res.get_reg_base()) {
        vm.heap = h;
        for (size_t i = 0; i < 64; ++i) win[i] = Value{};
    }

    [[nodiscard]] Context context() {
        Context ctx{};
        ctx.window_ptr         = win;
        ctx.vm                 = &vm;
        ctx.ret_stack_base     = res.get_ret_base();
        ctx.ret_stack_ptr      = res.get_ret_base();
        ctx.ret_stack_limit    = res.get_ret_base() + VM_Resources::RET_FRAME_COUNT;
        ctx.frame_size_ptr     = res.get_frame_size_base();
        ctx.current_frame_size = frame_size;
        return ctx;
    }
};

// =============================================================================
// test_string_alignment -- regression guard for the 8-byte footprint padding.
//
// Odd-length KIND_STRING payloads (e.g. "a" -> 10 bytes total, "hello" -> 14)
// are not multiples of 8. Without footprint padding the *next* object's header
// would start misaligned, violating static_assert(alignof(GcObject) == 8).
// This test allocates an interleaved string/array mix and asserts:
//   - every object header is 8-aligned (bump stride padded), both the object
//     directly following an odd-length string and the bump cursor itself;
//   - the gap between an odd string and the next header is the padded footprint
//     (16, not the unpadded 10) -- the direct guard the fix exists for;
//   - the same holds AFTER a collect(), i.e. the copy/scan stride is padded too,
//     with string contents preserved through the move.
// =============================================================================
inline void test_string_alignment() {
    std::cout << "=== string_alignment ===\n";

    auto aligned8 = [](const void* p) noexcept {
        return (reinterpret_cast<uintptr_t>(p) % 8) == 0;
    };

    Heap heap(64 * 1024);
    GcTestCtx t(&heap, 5); // all five objects are rooted -> all survive

    // Interleave odd-length strings with arrays so a misaligned header would
    // surface immediately on the object that follows each string.
    GcObject* s0 = heap.alloc_string("a");            // payload 2  -> padded 16
    GcObject* a0 = heap.alloc(GcObject::KIND_ARRAY, 1); // 8 + 8   =  16
    GcObject* s1 = heap.alloc_string("hello");        // payload 6  -> padded 16
    GcObject* a1 = heap.alloc(GcObject::KIND_ARRAY, 2); // 8 + 16  =  24
    GcObject* s2 = heap.alloc_string("xyz");          // payload 4  -> padded 16
    assert(s0 && a0 && s1 && a1 && s2 && "string_alignment: alloc failed");

    const bool pre_aligned =
        aligned8(s0) && aligned8(a0) && aligned8(s1) && aligned8(a1) && aligned8(s2) &&
        (heap.used() % 8 == 0);

    // The object right after odd-length "a" must sit at s0 + padded footprint 16,
    // not the unpadded 10.
    const size_t gap_after_odd_string =
        reinterpret_cast<uintptr_t>(a0) - reinterpret_cast<uintptr_t>(s0);
    const bool gap_padded = (gap_after_odd_string == 16);

    // Root all five survivors, then relocate them through a collection.
    t.win[0] = Value::fromPtr(s0->payload());
    t.win[1] = Value::fromPtr(a0->payload());
    t.win[2] = Value::fromPtr(s1->payload());
    t.win[3] = Value::fromPtr(a1->payload());
    t.win[4] = Value::fromPtr(s2->payload());

    Context ctx = t.context();
    heap.collect(&ctx);

    GcObject* s0b = GcObject::from_slots(t.win[0].asPtr());
    GcObject* a0b = GcObject::from_slots(t.win[1].asPtr());
    GcObject* s1b = GcObject::from_slots(t.win[2].asPtr());
    GcObject* a1b = GcObject::from_slots(t.win[3].asPtr());
    GcObject* s2b = GcObject::from_slots(t.win[4].asPtr());

    const bool post_aligned =
        aligned8(s0b) && aligned8(a0b) && aligned8(s1b) && aligned8(a1b) && aligned8(s2b) &&
        (heap.used() % 8 == 0);

    const bool contents_ok =
        s1b->kind == GcObject::KIND_STRING &&
        s1b->string_length() == 5 &&
        std::string_view(s1b->bytes(), s1b->string_length()) == "hello" &&
        std::string_view(s2b->bytes(), s2b->string_length()) == "xyz";

    const bool ok = pre_aligned && gap_padded && post_aligned && contents_ok;

    std::cout << std::format("gap_after_odd_string={} (expect 16) used={}\n",
        gap_after_odd_string, heap.used());
    std::cout << std::format("  Scenario A (headers 8-aligned pre+post GC): {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

inline void test_gc_moves() {
    std::cout << "=== gc_moves ===\n";

    Heap heap(64 * 1024);
    GcTestCtx t(&heap, 1); // r0 is the only live root

    GcObject* a = heap.alloc(GcObject::KIND_ARRAY, 2);
    assert(a && "gc_moves: alloc failed");
    a->slots()[0] = Value::fromSigned48(7);
    a->slots()[1] = Value::fromSigned48(8);
    t.win[0] = Value::fromPtr(a->slots());

    Context      ctx         = t.context();
    void*        addr_before = t.win[0].asPtr();
    const size_t used_before = heap.used();
    const size_t freed       = heap.collect(&ctx);
    const size_t used_after  = heap.used();
    void*        addr_after  = t.win[0].asPtr();

    GcObject* a2 = GcObject::from_slots(addr_after);

    const bool ok =
        t.win[0].isPtr() &&
        addr_after != addr_before &&   // the survivor was physically relocated
        a2->slots()[0].asSigned48() == 7 &&
        a2->slots()[1].asSigned48() == 8 &&
        freed == 0 &&
        used_after == used_before;

    std::cout << std::format("addr_before={} addr_after={} moved={} used_before={} used_after={}\n",
        addr_before, addr_after, addr_before != addr_after, used_before, used_after);
    std::cout << std::format("  Scenario A (survivor relocated, intact):  {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_verify_heap -- positive exercise for the Debug-only Heap::verify_heap()
// post-collection self-check. A clean Debug run IS the guard: verify_heap()
// runs inside every collect() below (both the implicit ones from a full
// semispace and the explicit ones here) and aborts on any inconsistency. This
// test builds a mixed, cross-linked live set (array + struct + closure +
// odd-length string, with pointer links and an array<->closure cycle), churns
// unrooted garbage, forces repeated collections via a small semispace, and
// confirms the survivors stay intact by identity + values across every move.
// (KIND_MAP / KIND_VEC / KIND_BYTES headers are exercised by their own
// test_map / test_vec / test_bytes, which also collect and thus verify.)
// =============================================================================
inline void test_verify_heap() {
    std::cout << "=== verify_heap ===\n";

    Heap heap(8 * 1024); // small -> frequent collections
    GcTestCtx t(&heap, 4); // r0 array, r1 struct, r2 closure, r3 string
    Context ctx = t.context();

    // string -> r3 (odd length -> footprint padded; exercises pass-1 sizing).
    GcObject* s = heap.alloc_string_gc("odd-length!", &ctx);
    t.win[3] = Value::fromPtr(s->bytes());

    // struct { field0: string, field1: int } -> r1
    GcObject* st = heap.alloc_object_gc(/*type_id*/ 7, 2, &ctx);
    st->slots()[0] = t.win[3];                    // struct.field0 -> string
    st->slots()[1] = Value::fromSigned48(42);
    t.win[1] = Value::fromPtr(st->slots());

    // array[2] = { struct, <closure> } -> r0  (array[1] wired below to close a cycle)
    GcObject* arr = heap.alloc_slots_gc(GcObject::KIND_ARRAY, 2, &ctx);
    arr->slots()[0] = t.win[1];                   // array[0] -> struct
    t.win[0] = Value::fromPtr(arr->slots());

    // closure (1 capture) capturing the array -> r2
    GcObject* clo = heap.alloc_closure_gc(/*fn_id*/ 3, 1, &ctx);
    clo->slots()[0] = t.win[0];                   // capture -> array
    t.win[2] = Value::fromPtr(clo->slots());

    // Close the array<->closure cycle. Re-fetch the array through its root (the
    // closure alloc above may have collected and relocated it).
    GcObject::from_slots(t.win[0].asPtr())->slots()[1] = t.win[2]; // array[1] -> closure

    int  collections = 0;
    bool ok          = true;
    for (int i = 0; i < 500; ++i) {
        // Unrooted garbage -> interior holes for the next collect. alloc_*_gc
        // collects internally when the small semispace fills (verify runs there).
        (void)heap.alloc_slots_gc(GcObject::KIND_ARRAY, 2, &ctx);
        (void)heap.alloc_string_gc("garbage", &ctx);

        if ((i % 50) == 0) {
            heap.collect(&ctx); // explicit collection -> verify_heap() again
            ++collections;

            // Re-fetch every survivor through its (moved) root and check the
            // whole graph: identities, the cycle, header side-data, and values.
            GcObject* a2  = GcObject::from_slots(t.win[0].asPtr());
            GcObject* st2 = GcObject::from_slots(t.win[1].asPtr());
            GcObject* c2  = GcObject::from_slots(t.win[2].asPtr());
            GcObject* s2  = GcObject::from_slots(t.win[3].asPtr());

            ok = ok
                && a2->slots()[0].isPtr()  && GcObject::from_slots(a2->slots()[0].asPtr()) == st2  // array[0] -> struct
                && a2->slots()[1].isPtr()  && GcObject::from_slots(a2->slots()[1].asPtr()) == c2   // array[1] -> closure
                && c2->slots()[0].isPtr()  && GcObject::from_slots(c2->slots()[0].asPtr()) == a2   // closure cap -> array (cycle)
                && st2->slots()[0].isPtr() && GcObject::from_slots(st2->slots()[0].asPtr()) == s2  // struct.f0 -> string
                && st2->slots()[1].asSigned48() == 42
                && st2->object_type_id() == 7
                && c2->closure_fn_id() == 3
                && std::string_view(s2->bytes(), s2->string_length()) == "odd-length!";
        }
    }

    std::cout << std::format("  collections={} survivors intact + verify_heap clean: {}\n",
        collections, ok ? "PASS" : "FAIL");
    check(ok);
}

inline void test_gc_interleaved_reclaim() {
    std::cout << "=== gc_interleaved_reclaim ===\n";

    Heap heap(64 * 1024);
    GcTestCtx t(&heap, 2); // r0, r1 are live roots

    // A (live) | B (dead, interior hole) | C (live)
    GcObject* a = heap.alloc(GcObject::KIND_ARRAY, 2);       // 8 + 16 = 24
    GcObject* b = heap.alloc(GcObject::KIND_ARRAY, 3);       // 8 + 24 = 32 (dead)
    GcObject* c = heap.alloc(GcObject::KIND_ARRAY, 1);       // 8 +  8 = 16
    assert(a && b && c && "interleaved: alloc failed");

    a->slots()[0] = Value::fromSigned48(100);
    a->slots()[1] = Value::fromSigned48(101);
    b->slots()[0] = Value::fromSigned48(999); // no root -> garbage
    c->slots()[0] = Value::fromSigned48(200);

    t.win[0] = Value::fromPtr(a->slots());
    t.win[1] = Value::fromPtr(c->slots());
    // b is deliberately unrooted -- it is an interior hole between A and C.

    const size_t size_a = a->total_bytes();
    const size_t size_b = b->total_bytes();
    const size_t size_c = c->total_bytes();

    Context      ctx         = t.context();
    const size_t used_before = heap.used();
    const size_t freed       = heap.collect(&ctx);
    const size_t used_after  = heap.used();

    // Prove the reclaimed interior space is actually reusable: allocate again.
    GcObject* d = heap.alloc(GcObject::KIND_ARRAY, 2);
    const bool reused = (d != nullptr);

    GcObject* a2 = GcObject::from_slots(t.win[0].asPtr());
    GcObject* c2 = GcObject::from_slots(t.win[1].asPtr());

    const bool ok =
        used_before == size_a + size_b + size_c &&
        used_after  == size_a + size_c &&   // <-- interior B reclaimed; FAILS on suffix-only
        freed       == size_b &&
        a2->slots()[0].asSigned48() == 100 &&
        a2->slots()[1].asSigned48() == 101 &&
        c2->slots()[0].asSigned48() == 200 &&
        reused;

    std::cout << std::format("used_before={} used_after={} freed={} (interior B={})\n",
        used_before, used_after, freed, size_b);
    std::cout << std::format("  Scenario A (interior hole reclaimed):     {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

inline void test_gc_root_convergence() {
    std::cout << "=== gc_root_convergence ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;
    GcTestCtx t(&heap, 1); // r0 holds the shared string as a register root
    t.vm.interner = &interner; // interner is a strong root: collect() reaches it via ctx->vm

    // "shared" is referenced from TWO roots: a register AND the interner table.
    Context ctx = t.context();
    Value shared = interner.intern("shared", heap, &ctx);
    t.win[0] = shared;

    heap.collect(&ctx);

    // Both roots must have converged onto the single relocated object.
    Value from_reg     = t.win[0];
    Value from_interner = interner.find("shared");

    const bool ok =
        from_reg.isPtr() &&
        from_interner.isPtr() &&
        from_reg.asPtr() == from_interner.asPtr() && // idempotent forward -> one address
        std::string_view(GcObject::from_slots(from_reg.asPtr())->bytes(), 6) == "shared";

    std::cout << std::format("reg={} interner={} converged={}\n",
        from_reg.asPtr(), from_interner.asPtr(), from_reg.asPtr() == from_interner.asPtr());
    std::cout << std::format("  Scenario A (roots converge on one copy):  {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_gc_grow_for_oversized -- a single allocation larger than the whole
// (near-empty) semispace must succeed by growing the heap. This is the case
// that occupancy-driven growth alone misses: live is ~0, so the post-GC growth
// threshold never trips, yet the object still does not fit. alloc_slots_gc's
// last-resort grow_to_fit step is what closes it (see the retry ladder).
// =============================================================================
inline void test_gc_grow_for_oversized() {
    std::cout << "=== gc_grow_for_oversized ===\n";

    Heap heap(64); // tiny 64-byte semispace
    GcTestCtx t(&heap, 1);
    Context ctx = t.context();

    const size_t cap_before = heap.capacity();

    // 100 slots -> 8 + 800 = 808 bytes, far larger than the 64-byte semispace.
    GcObject* big = heap.alloc_slots_gc(GcObject::KIND_ARRAY, 100, &ctx);
    const bool allocated = (big != nullptr);
    if (allocated) {
        for (uint32_t i = 0; i < 100; ++i)
            big->slots()[i] = Value::fromSigned48(static_cast<int64_t>(i));
        t.win[0] = Value::fromPtr(big->slots()); // root it
    }

    const bool ok =
        allocated &&
        big->slot_count() == 100 &&
        big->slots()[0].asSigned48() == 0 &&
        big->slots()[99].asSigned48() == 99 &&
        heap.capacity() >= big->total_bytes() &&
        heap.capacity() > cap_before; // the heap actually grew

    std::cout << std::format("cap_before={} cap_after={} obj_bytes={}\n",
        cap_before, heap.capacity(), allocated ? big->total_bytes() : 0);
    std::cout << std::format("  Scenario A (oversized alloc grows heap):  {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_debug_dump -- smoke/regression test for the vm_dbg inspection helpers
// (vmcore/Debug.h): a heap holding a string, a Value array, and a fixed-shape
// struct is rendered to a string buffer, together with the register window that
// points at them. It asserts the dumps do not crash and surface the expected
// fields (kinds, the string bytes, a slot Value, the struct's type id, the
// register Ptr lines). Not a correctness proof of the VM -- a guard that the
// debug aids keep working (and a live usage example) as the heap layout evolves.
// =============================================================================
inline void test_debug_dump() {
    std::cout << "=== debug_dump ===\n";

    Heap heap(64 * 1024);
    GcTestCtx t(&heap, 3); // r0..r2 hold the three objects

    GcObject* s = heap.alloc_string("hello");
    GcObject* a = heap.alloc(GcObject::KIND_ARRAY, 2);
    GcObject* o = heap.alloc_object(/*type_id=*/7, /*n_fields=*/1);
    assert(s && a && o && "debug_dump: alloc failed");
    a->slots()[0] = Value::fromSigned48(42);
    a->slots()[1] = Value::fromDouble(3.5);
    o->slots()[0] = Value::fromBool(true);

    t.win[0] = Value::fromPtr(s->bytes());
    t.win[1] = Value::fromPtr(a->slots());
    t.win[2] = Value::fromPtr(o->slots());

    std::ostringstream reg_os, heap_os;
    Context ctx = t.context();
    vm_dbg::dump_registers(ctx, reg_os);
    vm_dbg::dump_heap(heap, heap_os);

    const std::string reg = reg_os.str();
    const std::string hp  = heap_os.str();
    std::cout << reg << hp;

    const bool ok =
        reg.find("r0 = Ptr(") != std::string::npos &&
        reg.find("r2 = Ptr(") != std::string::npos &&
        hp.find("STRING")     != std::string::npos &&
        hp.find("hello")      != std::string::npos &&
        hp.find("ARRAY")      != std::string::npos &&
        hp.find("Int(42)")    != std::string::npos &&
        hp.find("OBJECT")     != std::string::npos &&
        hp.find("type_id=7")  != std::string::npos;

    std::cout << std::format("  Scenario A (registers + heap dumped):     {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_gc_stress -- drive the copying collector through hundreds of collections
// under sustained allocation churn, so a forwarding bug in a rarer path surfaces
// (the other GC tests trigger only a handful of collections). The scenario keeps
// three kinds of persistent, register-rooted live state alive across the churn:
//
//   r0  -- the head of a bounded cons-list (2-slot arrays {value, tail}) that is
//          grown by prepending and periodically dropped whole as garbage, so every
//          collection has real interior/suffix garbage to reclaim and re-pack;
//   r1  -- a permanent 3-node CYCLE (n0->n1->n2->n0); only n0 is rooted, so the
//          collector must forward the whole ring and converge (idempotent forward),
//          repeatedly -- the direct regression for cyclic-graph forwarding;
//   r2  -- a persistent heap string (odd length -> padded footprint) that must
//          survive every move with its bytes intact.
//
// Plus per-iteration unrooted garbage and a periodic OVERSIZED block that forces
// grow_to_fit. The test itself obeys invariant #7: it re-fetches every pointer
// through the roots after each collection (pointers move). Invariants, checked
// both periodically AND at the end: the ring still closes by identity with intact
// values; the string is intact; the list is a strictly-decreasing chain ending in
// Nil of the expected length; two back-to-back final collections leave `used()`
// unchanged (the survivor set is a fixed point -- no floating garbage / no double
// copy); at least one collection actually reclaimed memory; and we really ran many
// collections (not a no-op loop).
// =============================================================================
inline void test_gc_stress() {
    std::cout << "=== gc_stress ===\n";

    Heap heap(8 * 1024); // small semispace -> frequent collections
    GcTestCtx t(&heap, 4); // r0 list head, r1 ring root, r2 string, r3 build scratch
    Context ctx = t.context();

    // Re-fetch through a ring root and verify the 3-cycle closes by identity with
    // intact values. `root` must be re-read from the (moving) register each call.
    auto ring_ok = [](Value root) -> bool {
        if (!root.isPtr()) return false;
        GcObject* a = GcObject::from_slots(root.asPtr());
        if (!a->slots()[1].isPtr()) return false;
        GcObject* b = GcObject::from_slots(a->slots()[1].asPtr());
        if (!b->slots()[1].isPtr()) return false;
        GcObject* c = GcObject::from_slots(b->slots()[1].asPtr());
        if (!c->slots()[1].isPtr()) return false;
        GcObject* back = GcObject::from_slots(c->slots()[1].asPtr());
        return back == a &&                                   // cycle closes by identity
            a->slots()[0].asSigned48() == 1000 &&
            b->slots()[0].asSigned48() == 1001 &&
            c->slots()[0].asSigned48() == 1002;
    };
    auto str_ok = [](Value root) -> bool {
        if (!root.isPtr()) return false;
        GcObject* s = GcObject::from_slots(root.asPtr());
        return std::string_view(s->bytes(), s->string_length()) == "persist";
    };

    // ---- Build the permanent 3-cycle. Each alloc may collect and relocate the
    // already-built nodes, so root every partial node in a register first, then
    // wire the ring by re-fetching through the roots (no alloc in between). ----
    GcObject* n = heap.alloc_slots_gc(GcObject::KIND_ARRAY, 2, &ctx);
    t.win[1] = Value::fromPtr(n->slots());                    // n0 -> r1
    n = heap.alloc_slots_gc(GcObject::KIND_ARRAY, 2, &ctx);
    t.win[3] = Value::fromPtr(n->slots());                    // n1 -> r3 (temp root)
    n = heap.alloc_slots_gc(GcObject::KIND_ARRAY, 2, &ctx);
    t.win[2] = Value::fromPtr(n->slots());                    // n2 -> r2 (temp root)
    {
        GcObject* a = GcObject::from_slots(t.win[1].asPtr());
        GcObject* b = GcObject::from_slots(t.win[3].asPtr());
        GcObject* c = GcObject::from_slots(t.win[2].asPtr());
        a->slots()[0] = Value::fromSigned48(1000); a->slots()[1] = t.win[3]; // n0.tail = n1
        b->slots()[0] = Value::fromSigned48(1001); b->slots()[1] = t.win[2]; // n1.tail = n2
        c->slots()[0] = Value::fromSigned48(1002); c->slots()[1] = t.win[1]; // n2.tail = n0
    }
    t.win[3] = Value{}; // drop the direct n1 root; n1/n2 stay reachable via n0's tail

    // Persistent odd-length string -> r2 (overwrites the n2 temp root; n2 still
    // reachable through n1.tail). alloc_string_gc may collect -> roots re-forwarded.
    n = heap.alloc_string_gc("persist", &ctx);
    t.win[2] = Value::fromPtr(n->bytes());

    t.win[0] = Value::fromNil(); // empty list to start

    constexpr int N = 20000; // iterations
    constexpr int L = 40;    // list length cap before it is dropped as garbage
    int    list_len    = 0;
    int    collections = 0;
    size_t freed_total = 0;
    bool   periodic_ok = true;

    for (int i = 0; i < N; ++i) {
        // (1) unrooted garbage churn (creates interior holes for the next collect)
        (void)heap.alloc_slots_gc(GcObject::KIND_ARRAY, 1, &ctx);
        (void)heap.alloc_slots_gc(GcObject::KIND_ARRAY, 3, &ctx);

        // (2) prepend a node. The alloc may collect first (re-forwarding r0), and
        //     the fresh node is post-collect; nothing allocates before we root it.
        GcObject* node = heap.alloc_slots_gc(GcObject::KIND_ARRAY, 2, &ctx);
        node->slots()[0] = Value::fromSigned48(i);
        node->slots()[1] = t.win[0];               // old head (Nil or already-forwarded Ptr)
        t.win[0] = Value::fromPtr(node->slots());
        ++list_len;

        // (3) drop the whole chain periodically -> lots of reclaimable garbage
        if (list_len >= L) { t.win[0] = Value::fromNil(); list_len = 0; }

        // (4) force grow_to_fit occasionally with an oversized, unrooted block
        if ((i % 2000) == 1999)
            (void)heap.alloc_slots_gc(GcObject::KIND_ARRAY, 4000, &ctx);

        // (5) collect periodically; verify the persistent state under repetition
        if ((i % 50) == 0) {
            freed_total += heap.collect(&ctx);
            ++collections;
            if (!ring_ok(t.win[1]) || !str_ok(t.win[2])) periodic_ok = false;
        }
    }

    // ---- Final invariants ----
    heap.collect(&ctx);
    const size_t used1 = heap.used();
    heap.collect(&ctx);
    const size_t used2 = heap.used(); // must equal used1: survivors are a fixed point

    const bool ring_final = ring_ok(t.win[1]);
    const bool str_final  = str_ok(t.win[2]);

    // List integrity: a strictly-decreasing chain of `list_len` nodes ending in Nil.
    bool list_ok = true;
    {
        Value cur = t.win[0];
        int steps = 0;
        int64_t prev = 0;
        bool first = true;
        bool consec = true;
        while (cur.isPtr() && steps < L + 5) {
            GcObject* nd = GcObject::from_slots(cur.asPtr());
            const int64_t v = nd->slots()[0].asSigned48();
            if (!first && v != prev - 1) consec = false;
            prev = v; first = false;
            cur = nd->slots()[1];
            ++steps;
        }
        list_ok = consec && cur.isNil() && steps == list_len;
    }

    const bool ok =
        periodic_ok &&
        collections >= 200 &&      // really stressed, not a no-op loop
        ring_final &&              // cyclic graph forwarded correctly to the end
        str_final &&               // string survived every move intact
        list_ok &&                 // list chain intact and correctly terminated
        used1 == used2 &&          // survivor set is a fixed point (no floating garbage)
        freed_total > 0;           // real reclamation happened

    std::cout << std::format("collections={} freed_total={} B used1={} used2={} list_len={}\n",
        collections, freed_total, used1, used2, list_len);
    std::cout << std::format("  ring={} string={} list={} fixed_point={}\n",
        ring_final, str_final, list_ok, used1 == used2);
    std::cout << std::format("  Scenario A (survives GC churn intact):    {}\n",
        ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_div_mod -- DIV_INT / MOD_INT: signed truncated division and remainder.
// Covers positive, negative (sign-of-dividend for %), and the Java MIN/-1 wrap
// edge (MIN_48 / -1 == MIN_48, MIN_48 % -1 == 0). MIN_48 is built at runtime as
// 1 << 47 so the test does not depend on the 48-bit constant loader.
// =============================================================================
inline void test_div_mod() {
    constexpr int64_t MIN_48 = -(int64_t(1) << 47);
    Assembler as;
    as.label("main");
    as.C2(OpCode::LOAD_CONST, 0, 17);
    as.C2(OpCode::LOAD_CONST, 1, 5);
    as.R6(OpCode::DIV_INT, 2, 0, 1);      // 17 / 5  = 3
    as.R6(OpCode::MOD_INT, 3, 0, 1);      // 17 % 5  = 2
    as.C2(OpCode::LOAD_CONST, 4, -17);
    as.R6(OpCode::DIV_INT, 6, 4, 1);      // -17 / 5 = -3 (toward zero)
    as.R6(OpCode::MOD_INT, 7, 4, 1);      // -17 % 5 = -2 (sign of dividend)
    // MIN_48 via 1 << 47, then MIN_48 / -1 and MIN_48 % -1
    as.C2(OpCode::LOAD_CONST, 8, 1);
    as.C2(OpCode::LOAD_CONST, 9, 47);
    as.R6(OpCode::SHL_INT, 10, 8, 9);     // r10 = MIN_48
    as.C2(OpCode::LOAD_CONST, 11, -1);
    as.R6(OpCode::DIV_INT, 12, 10, 11);   // MIN_48 / -1 = MIN_48 (wraps, no trap)
    as.R6(OpCode::MOD_INT, 13, 10, 11);   // MIN_48 % -1 = 0
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== div_mod ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode);
        auto* regs = res.get_reg_base();
        const int64_t r2 = regs[2].asSigned48(), r3 = regs[3].asSigned48();
        const int64_t r6 = regs[6].asSigned48(), r7 = regs[7].asSigned48();
        const int64_t r10 = regs[10].asSigned48();
        const int64_t r12 = regs[12].asSigned48(), r13 = regs[13].asSigned48();
        std::cout << std::format("17/5={} 17%5={} -17/5={} -17%5={}\n", r2, r3, r6, r7);
        std::cout << std::format("MIN_48={} MIN_48/-1={} MIN_48%-1={}\n", r10, r12, r13);
        const bool ok = r2 == 3 && r3 == 2 && r6 == -3 && r7 == -2 &&
                        r10 == MIN_48 && r12 == MIN_48 && r13 == 0;
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_div_by_zero_trap -- DIV_INT / MOD_INT by zero must raise a VM-level trap
// (std::runtime_error "Division by zero"), NOT a script-level exception. Catching
// that exception is the success condition.
// =============================================================================
inline void test_div_by_zero_trap() {
    std::cout << "=== div_by_zero_trap ===\n";
    auto traps = [](OpCode divlike) -> bool {
        Assembler as;
        as.label("main");
        as.C2(OpCode::LOAD_CONST, 0, 10);
        as.C2(OpCode::LOAD_CONST, 1, 0);
        as.R6(divlike, 2, 0, 1);           // 10 / 0  or  10 % 0
        as.J(OpCode::HALT);
        try {
            execute(as.assemble());
            return false;                  // no trap -> failure
        } catch (const std::exception& e) {
            // Phase-1 located message: "division by zero at line ? (in <script>)".
            return std::string_view(e.what()).find("division by zero") != std::string_view::npos;
        }
    };
    const bool div_ok = traps(OpCode::DIV_INT);
    const bool mod_ok = traps(OpCode::MOD_INT);
    std::cout << std::format("  DIV_INT by zero trapped: {}\n", div_ok ? "PASS" : "FAIL");
    std::cout << std::format("  MOD_INT by zero trapped: {}\n", mod_ok ? "PASS" : "FAIL");
    check(div_ok && mod_ok);
}

// =============================================================================
// test_receiver_faults -- Phase 1: the heap-typed fast-path opcodes raise a
// DEFINED, located abort (raise_located -> std::runtime_error) when handed a
// non-heap / wrong-kind receiver, instead of dereferencing a wild pointer. No
// line table is supplied here, so the message ends with the "line ?" fallback.
// =============================================================================
inline void test_receiver_faults() {
    std::cout << "=== receiver_faults (defined heap-op receiver checks) ===\n";
    // Build a tiny program applying `op` to an Int (7) receiver, run it, and report
    // whether it throws a message containing `needle`.
    auto faults_with = [](auto build, const char* needle) -> bool {
        Assembler as;
        as.label("main");
        as.load_const(0, 7);               // r0 = Int(7) -- never a valid heap receiver
        build(as);
        as.J(OpCode::HALT);
        Heap heap;
        try {
            execute(as.assemble(), &heap, nullptr, nullptr, 4);
            return false;                  // no trap -> failure
        } catch (const std::exception& e) {
            return std::string_view(e.what()).find(needle) != std::string_view::npos;
        }
    };
    const bool map_ok  = faults_with([](Assembler& as){ as.load_const(1, 0); as.MAP_GET(2, 0, 1); }, "non-map");
    const bool arr_ok  = faults_with([](Assembler& as){ as.load_const(1, 0); as.ARRAY_GET(2, 0, 1); }, "non-array");
    const bool prop_ok = faults_with([](Assembler& as){ as.GET_PROP(1, 0, 0); }, "non-struct");
    const bool len_ok  = faults_with([](Assembler& as){ as.LEN(1, 0); }, "non-array/non-map");
    const bool loc_ok  = faults_with([](Assembler& as){ as.LEN(1, 0); }, "line ?");  // no line table -> fallback
    std::cout << std::format("  MAP_GET on non-map     : {}\n", map_ok  ? "PASS" : "FAIL");
    std::cout << std::format("  ARRAY_GET on non-array : {}\n", arr_ok  ? "PASS" : "FAIL");
    std::cout << std::format("  GET_PROP on non-struct : {}\n", prop_ok ? "PASS" : "FAIL");
    std::cout << std::format("  LEN on non-collection  : {}\n", len_ok  ? "PASS" : "FAIL");
    std::cout << std::format("  'line ?' fallback       : {}\n", loc_ok ? "PASS" : "FAIL");
    check(map_ok && arr_ok && prop_ok && len_ok && loc_ok);
}

// =============================================================================
// test_neg -- NEG_INT: unary minus, including the MIN_48 wrap (-MIN_48 == MIN_48).
// =============================================================================
inline void test_neg() {
    constexpr int64_t MIN_48 = -(int64_t(1) << 47);
    Assembler as;
    as.label("main");
    as.C2(OpCode::LOAD_CONST, 0, 42);
    as.R6(OpCode::NEG_INT, 1, 0, 0);      // -42
    as.C2(OpCode::LOAD_CONST, 2, -7);
    as.R6(OpCode::NEG_INT, 3, 2, 0);      // 7
    as.C2(OpCode::LOAD_CONST, 4, 1);
    as.C2(OpCode::LOAD_CONST, 5, 47);
    as.R6(OpCode::SHL_INT, 6, 4, 5);      // r6 = MIN_48
    as.R6(OpCode::NEG_INT, 7, 6, 0);      // -MIN_48 = MIN_48 (wraps)
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== neg ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode);
        auto* regs = res.get_reg_base();
        const int64_t r1 = regs[1].asSigned48(), r3 = regs[3].asSigned48(), r7 = regs[7].asSigned48();
        std::cout << std::format("-42={} -(-7)={} -MIN_48={}\n", r1, r3, r7);
        const bool ok = r1 == -42 && r3 == 7 && r7 == MIN_48;
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_bitwise -- AND_INT / OR_INT / XOR_INT. Also shows ~x via XOR with -1.
// =============================================================================
inline void test_bitwise() {
    Assembler as;
    as.label("main");
    as.C2(OpCode::LOAD_CONST, 0, 12);     // 0b1100
    as.C2(OpCode::LOAD_CONST, 1, 10);     // 0b1010
    as.R6(OpCode::AND_INT, 2, 0, 1);      // 0b1000 = 8
    as.R6(OpCode::OR_INT,  3, 0, 1);      // 0b1110 = 14
    as.R6(OpCode::XOR_INT, 4, 0, 1);      // 0b0110 = 6
    as.C2(OpCode::LOAD_CONST, 5, -1);
    as.R6(OpCode::XOR_INT, 6, 0, 5);      // ~12 = -13
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== bitwise (and/or/xor) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode);
        auto* regs = res.get_reg_base();
        const int64_t r2 = regs[2].asSigned48(), r3 = regs[3].asSigned48();
        const int64_t r4 = regs[4].asSigned48(), r6 = regs[6].asSigned48();
        std::cout << std::format("12&10={} 12|10={} 12^10={} ~12={}\n", r2, r3, r4, r6);
        const bool ok = r2 == 8 && r3 == 14 && r4 == 6 && r6 == -13;
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_av_classification -- VM_Resources::address_in_stacks, the predicate that
// decides how an access violation is reported. Every AV used to be announced as
// "VM Stack Overflow (hardware guard page)", which is right for one cause and
// misleading for the rest: a dangling heap pointer faults inside memcpy at a heap
// address and was reported as a stack overflow. The fault address separates them,
// so this pins the decision function rather than trying to provoke a real fault
// (deliberately overflowing a stack in a test would be fragile and unportable).
// =============================================================================
inline void test_av_classification() {
    std::cout << "=== av_classification ===\n";

    VM_Resources res;
    Heap         heap(16 * 1024);

    // Inside each of the four stack regions -- a fault here IS a stack overflow.
    const bool reg_base_in = res.address_in_stacks(res.get_reg_base());
    const bool ret_base_in = res.address_in_stacks(res.get_ret_base());
    const bool fsz_base_in = res.address_in_stacks(res.get_frame_size_base());
    const bool clo_base_in = res.address_in_stacks(res.get_closure_base());

    // Past the COMMITTED end but still reserved: the page a growth-disabled Context
    // faults on. Must still classify as a stack overflow.
    const bool past_commit_in = res.address_in_stacks(res.reg_committed_end() + 16);

    // The guard page sits immediately past the RESERVED end -- also a stack overflow.
    const bool guard_in = res.address_in_stacks(res.reg_reserved_end());

    // A heap object is NOT in the stacks -- this is the case that used to be misreported.
    GcObject* obj = heap.alloc(GcObject::KIND_ARRAY, 2);
    const bool heap_out  = obj && !res.address_in_stacks(obj->slots());
    // Neither is a stack address of this very test, nor an obviously wild pointer.
    int        local     = 0;
    const bool local_out = !res.address_in_stacks(&local);
    const bool wild_out  = !res.address_in_stacks(reinterpret_cast<const void*>(uintptr_t{ 0x1234 }));

    const bool bases_ok = reg_base_in && ret_base_in && fsz_base_in && clo_base_in;
    const bool ok = bases_ok && past_commit_in && guard_in && heap_out && local_out && wild_out;

    std::cout << std::format("  all four stack bases classified in:  {}\n", bases_ok       ? "PASS" : "FAIL");
    std::cout << std::format("  reserved-but-uncommitted page in:    {}\n", past_commit_in ? "PASS" : "FAIL");
    std::cout << std::format("  trailing guard page in:              {}\n", guard_in       ? "PASS" : "FAIL");
    std::cout << std::format("  heap payload NOT in stacks:          {}\n", heap_out       ? "PASS" : "FAIL");
    std::cout << std::format("  native stack / wild ptr NOT in:      {}\n", (local_out && wild_out) ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_fault_probe -- the END-TO-END counterpart of test_av_classification, and the only
// thing that can show the fault frame around run_switch_loop actually works.
//
// WHY IT IS SEPARATE, AND OPT-IN. test_av_classification above deliberately does not
// fault: it pins the decision function with synthetic addresses. That is the right shape
// for a predicate, but it leaves the whole chain untested -- the handler (or SEH filter),
// the capture of the fault address, the escape back to normal context, and the throw.
// This test provokes a REAL hardware fault, so a regression here is a process kill rather
// than a failed assertion. Hence `vm_tests --fault-probe`: off by default, and its own
// ctest entry, so a crash cannot take the other hundred-odd tests with it.
//
// HOW IT REACHES A FAULT AT ALL. Not by recursing: with vm.resources set, GROW_CHECK
// grows on demand and, at the reserved cap, raises a clean LOCATED fault -- the hardware
// guard page is by design unreachable from a well-formed program. So the test installs a
// native that hands back a deliberately bad pointer and lets LEN dereference it. The
// object-header read inside GcObject::from_slots IS the fault: a known address, on the VM
// thread, with no lock held and no allocation in flight.
//
// Platform-NEUTRAL in intent: on POSIX the sigaction frame catches it, on Windows the SEH
// filter does -- which incidentally gives the Windows path its first end-to-end coverage.
// =============================================================================

// kind 0 -> deep inside the reserved-but-uncommitted register stack: mapped with no
//           access on both platforms and inside a VM stack region, so it must classify
//           as a stack overflow.
// kind 1 -> an address in no mapping at all: must classify as NOT a stack overflow. If
//           it ever became mapped the test fails cleanly ("no fault"), never silently.
inline Value native_fault_ptr(Value* args, uint8_t nargs, Context* ctx) {
    const int64_t kind = (nargs >= 1 && args[0].isInt()) ? args[0].asSigned48() : 0;
    if (kind == 0) {
        // Well past the committed end, so the header subtraction inside from_slots stays
        // in the reserved range instead of landing back on a committed page.
        return Value::fromPtr(ctx->vm->resources->reg_committed_end() + 4096);
    }
    return Value::fromPtr(reinterpret_cast<Value*>(uintptr_t{0x00007A00DEAD0000}));
}

inline bool fault_probe_once(int64_t kind, const char* want) {
    Assembler as;
    as.C2(OpCode::LOAD_CONST, 2, static_cast<int16_t>(kind));      // r2 = kind
    as.call_native_id(/*rd*/3, /*id_reg*/1, /*first_arg*/2, /*nargs*/1, /*id*/0);
    as.R6(OpCode::LEN, 4, 3, 0);              // r4 = len(r3) -- THE FAULTING ACCESS
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();

    std::vector<NativeFunc> ntab = { native_fault_ptr };
    try {
        auto res = execute(bytecode, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0,
                           nullptr, nullptr, nullptr, &ntab);
        (void)res;
        std::cout << std::format("  kind {}: NO fault raised -- the frame never fired\n", kind);
        return false;
    } catch (const std::exception& e) {
        const bool ok = std::string(e.what()).find(want) != std::string::npos;
        std::cout << std::format("  kind {}: {}\n    caught: {}\n",
                                 kind, ok ? "PASS" : "FAIL", e.what());
        return ok;
    }
}

inline void test_fault_probe() {
    std::cout << "=== fault_probe (a real hardware fault, classified) ===\n";
    // A recovered hardware fault can leave the Heap's root list pointing at a dead frame,
    // so each probe must run on a Heap that is dropped straight afterwards and never
    // reused. execute() building and dropping its own is exactly that.
    const bool stack_ok = fault_probe_once(0, "Stack Overflow");
    const bool wild_ok  = fault_probe_once(1, "NOT a stack overflow");
    // Third case: fault, recover, fault again. On Darwin the kernel's SA_ONSTACK flag is
    // sticky across a siglongjmp, so without the re-arm every fault after the first is
    // delivered on the normal stack. Only a SECOND fault can catch that.
    const bool again_ok = fault_probe_once(0, "Stack Overflow");
    std::cout << std::format("  recovered twice in a row:            {}\n",
                             again_ok ? "PASS" : "FAIL");
    check(stack_ok && wild_ok && again_ok);
}

// =============================================================================
// value-codec tests -- a value graph out of one heap and back into another.
//
// THE ORACLE, AND WHY IT IS NOT HAND-WRITTEN. Correctness here means "the rebuilt graph
// is structurally the original", and the temptation is to write that comparison next to
// the codec. That is exactly the differential that cannot fail: a walker written from
// the same understanding as the encoder agrees with it about anything both got wrong.
// So the oracle is the interpreter's OWN EQ_DEEP worker, reached through vcodec::deep_eq
// -- the same code a Skarn `==` runs. It compares across two heaps because it only
// dereferences pointers and never allocates, so neither graph can move under it.
//
// WHERE THE ORACLE CANNOT SEE A DIFFERENCE: the graph's shape. EQ_DEEP answers on a
// cyclic value, but co-inductively -- two values are equal when they unfold alike -- so
// equality would hold just as well if the codec had duplicated a shared subgraph or
// unrolled a cycle into a longer ring, and those are precisely the bugs worth catching.
// Cycles and sharing are therefore checked by POINTER IDENTITY in the rebuilt graph.
// =============================================================================

inline void test_value_codec_roundtrip() {
    std::cout << "=== value_codec_roundtrip (immediates, string, array, struct) ===\n";
    try {
        Heap      src(64 * 1024);
        GcTestCtx t(&src, 16);
        Context   sctx = t.context();

        // --- build: an Array[4] holding a String, a struct, a Double and Nil ----------
        // Every allocation is a safepoint, so each already-built object is re-fetched
        // through the rooted register it was published into (invariant #7).
        GcObject* arr = src.alloc_slots_gc(GcObject::KIND_ARRAY, 4, &sctx);
        t.win[0] = Value::fromPtr(arr->slots());
        GcObject* str = src.alloc_string_gc("hello codec", &sctx);
        GcObject::from_slots(t.win[0].asPtr())->slots()[0] = Value::fromPtr(str->bytes());
        GcObject* obj = src.alloc_object_gc(/*type_id*/ 7, 2, &sctx);
        obj->slots()[0] = Value::fromSigned48(-12345);
        obj->slots()[1] = Value::fromBool(true);
        Value* a = GcObject::from_slots(t.win[0].asPtr())->slots();
        a[1] = Value::fromPtr(obj->slots());
        a[2] = Value::fromDouble(3.5);
        a[3] = Value::fromNil();

        // decode() checks every struct against the image's type table, so the test needs
        // one in which id 7 has the two fields the object was built with.
        std::vector<StructType> types(8, StructType{ 0, "", {} });
        types[7] = StructType{ 2, "Pair", { "a", "b" } };

        const auto      buf = vcodec::encode(t.win[0]);
        Heap            dst(64 * 1024);
        RootedValuePool pool;
        const Value     rebuilt = vcodec::decode(buf.data(), buf.size(), dst, nullptr, pool,
                                                 types.data(), types.size());

        const bool eq_ok = vcodec::deep_eq(t.win[0], rebuilt, &sctx);
        // It must be a COPY: a codec that handed back the source pointer would satisfy
        // every equality check ever written for it.
        const bool moved_ok = rebuilt.asPtr() != t.win[0].asPtr();
        // The struct's nominal identity has to survive, or the rebuilt value is a
        // different type wearing the same fields.
        GcObject*  r_arr  = GcObject::from_slots(rebuilt.asPtr());
        GcObject*  r_obj  = GcObject::from_slots(r_arr->slots()[1].asPtr());
        const bool tid_ok = r_obj->object_type_id() == 7;

        std::cout << std::format("  deep_eq / distinct storage / type id: {} {} {}\n",
                                 eq_ok ? "PASS" : "FAIL", moved_ok ? "PASS" : "FAIL",
                                 tid_ok ? "PASS" : "FAIL");
        check(eq_ok && moved_ok && tid_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_value_codec_containers -- Vec, Map and Bytes, built by the REAL opcodes.
//
// These three are the header/backing pairs, and hand-assembling one in a test would
// bake this test's idea of their layout into the guard. So the program is built and run
// by the VM, and only the finished value is handed to the codec.
//
// The Map is deliberately STRING-keyed. Its backing is copied slot for slot with no
// rehash, which is only sound because a string key hashes by CONTENT (a pointer move
// does not change the hash) and every other permitted key hashes by its bits. If that
// ever stopped holding, a rebuilt map would still compare equal element-wise but would
// no longer FIND its keys -- so the test looks the key up through the map itself.
// =============================================================================
// A test-only native: encode the argument and rebuild it in the SAME heap, so a running
// program can then use the copy through the ordinary opcodes.
//
// Order matters and is the whole safety argument: encode() first, while nothing
// allocates and the source graph therefore cannot move; only then decode(), whose
// allocations may collect. The real Context is passed on, so those collections forward
// the running program's registers instead of reclaiming them. The pool dies at the end
// of the call, after which the value is held by the register CALL_NATIVE writes it to --
// and nothing allocates in between.
inline Value native_codec_roundtrip(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1) return Value::fromNil();
    const auto      buf = vcodec::encode(args[0]);
    RootedValuePool pool;
    return vcodec::decode(buf.data(), buf.size(), *ctx->vm->heap, ctx, pool,
                          ctx->vm->struct_types, ctx->vm->struct_type_count);
}

inline void test_value_codec_containers() {
    std::cout << "=== value_codec_containers (Vec / Map[String] / Bytes, in-VM) ===\n";
    try {
        Assembler as;
        as.label("main");
        as.VEC_NEW(0);                                        // r0 = []
        as.load_const(1, 42);      as.VEC_PUSH(0, 1);
        as.load_str(1, "in-vec");  as.VEC_PUSH(0, 1);
        as.MAP_NEW(2);                                        // r2 = #{}
        as.load_str(3, "alpha"); as.load_const(4, 1); as.MAP_SET(2, 3, 4);
        as.load_str(3, "beta");  as.load_const(4, 2); as.MAP_SET(2, 3, 4);
        as.VEC_PUSH(0, 2);                                    // the vec holds the map
        as.load_str(5, "some bytes"); as.BYTES_FROM_STR(6, 5);
        as.VEC_PUSH(0, 6);                                    // ... and the bytes

        as.call_native_id(/*rd*/8, /*id_reg*/7, /*first_arg*/0, /*nargs*/1, /*id*/0);  // r8 = copy
        as.R6(OpCode::EQ_DEEP, 9, 0, 8);                      // r9 = (original == copy)
        // Look a STRING KEY up through the REBUILT map. The backing is copied slot for
        // slot with no rehash, which is sound only because a string key hashes by
        // content; if that ever stopped holding, the bytes would still compare equal but
        // the lookup would miss -- so this is the check that would actually notice.
        as.load_const(10, 2); as.ARRAY_GET(11, 8, 10);        // r11 = copy[2] (the map; ARRAY_GET indexes a Vec too)
        as.load_str(12, "beta"); as.MAP_GET(13, 11, 12);      // r13 = copy_map["beta"]
        as.J(OpCode::HALT);

        Heap                    heap(64 * 1024);
        StringInterner          interner;
        const auto              slits = as.string_literals();
        std::vector<NativeFunc> ntab  = { native_codec_roundtrip };
        auto res = execute(as.assemble(), &heap, nullptr, &interner, 16, nullptr, nullptr,
                           &slits, nullptr, nullptr, nullptr, nullptr, 0, 0,
                           nullptr, nullptr, nullptr, &ntab);
        auto* regs = res.get_reg_base();

        const bool copy_ok   = regs[8].isPtr() && regs[8].asPtr() != regs[0].asPtr();
        const bool eq_ok     = regs[9].isBool() && regs[9].asBool();
        const bool lookup_ok = regs[13].isInt() && regs[13].asSigned48() == 2;

        std::cout << std::format("  distinct copy / EQ_DEEP says equal: {} {}\n",
                                 copy_ok ? "PASS" : "FAIL", eq_ok ? "PASS" : "FAIL");
        std::cout << std::format("  string key still found in the copy: {}\n",
                                 lookup_ok ? "PASS" : "FAIL");
        check(copy_ok && eq_ok && lookup_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_value_codec_strings_not_interned -- a received string meets an INTERNED literal.
//
// decode() rebuilds a string as a fresh, NOT interned object in the destination heap. That
// is sound only because nothing in the VM compares strings by identity: EQ/NE/LT fall back
// to a content compare, EQ_DEEP compares string leaves by content, and a map hashes and
// matches a string key by content. So the copy is run through every one of those against
// the interned literal of the same text. The test first proves the premise -- the literal
// IS the interner's object and the copy is NOT -- or every comparison below would pass by
// pointer identity and prove nothing.
// =============================================================================
inline void test_value_codec_strings_not_interned() {
    std::cout << "=== value_codec_strings_not_interned (copy vs interned literal) ===\n";
    try {
        Assembler as;
        as.label("main");
        as.load_str(0, "payload");                            // r0 = the interned literal
        as.call_native_id(/*rd*/1, /*id_reg*/2, /*first_arg*/0, /*nargs*/1, /*id*/0);  // r1 = copy
        as.R6(OpCode::EQ, 3, 1, 0);                           // r3 = copy == literal
        as.R6(OpCode::NE, 4, 1, 0);                           // r4 = copy != literal
        as.R6(OpCode::LT, 5, 1, 0);                           // r5 = copy <  literal
        as.R6(OpCode::LT, 6, 0, 1);                           // r6 = literal < copy
        as.VEC_NEW(7); as.VEC_PUSH(7, 0);                     // r7 = [literal]
        as.VEC_NEW(8); as.VEC_PUSH(8, 1);                     // r8 = [copy]
        as.R6(OpCode::EQ_DEEP, 9, 7, 8);                      // r9 = [literal] == [copy]
        as.MAP_NEW(10); as.load_const(11, 5); as.MAP_SET(10, 0, 11);
        as.MAP_GET(12, 10, 1);                                // r12 = {literal: 5}[copy]
        as.MAP_NEW(13); as.load_const(11, 6); as.MAP_SET(13, 1, 11);
        as.MAP_GET(14, 13, 0);                                // r14 = {copy: 6}[literal]
        as.J(OpCode::HALT);

        Heap                    heap(64 * 1024);
        StringInterner          interner;
        const auto              slits = as.string_literals();
        std::vector<NativeFunc> ntab  = { native_codec_roundtrip };
        auto res = execute(as.assemble(), &heap, nullptr, &interner, 16, nullptr, nullptr,
                           &slits, nullptr, nullptr, nullptr, nullptr, 0, 0,
                           nullptr, nullptr, nullptr, &ntab);
        auto* regs = res.get_reg_base();

        const Value interned  = interner.find("payload");
        const bool  premise   = interned.isPtr() && regs[0].isPtr() && regs[1].isPtr() &&
                                regs[0].asPtr() == interned.asPtr() &&
                                regs[1].asPtr() != regs[0].asPtr();
        const bool  cmp_ok    = regs[3].isBool() && regs[3].asBool() &&
                                regs[4].isBool() && !regs[4].asBool() &&
                                regs[5].isBool() && !regs[5].asBool() &&
                                regs[6].isBool() && !regs[6].asBool();
        const bool  deep_ok   = regs[9].isBool() && regs[9].asBool();
        const bool  map_ok    = regs[12].isInt() && regs[12].asSigned48() == 5 &&
                                regs[14].isInt() && regs[14].asSigned48() == 6;

        std::cout << std::format("  literal interned, copy distinct:     {}\n", premise ? "PASS" : "FAIL");
        std::cout << std::format("  EQ / NE / LT both ways by content:   {}\n", cmp_ok ? "PASS" : "FAIL");
        std::cout << std::format("  EQ_DEEP on a string leaf:            {}\n", deep_ok ? "PASS" : "FAIL");
        std::cout << std::format("  map key found in both directions:    {}\n", map_ok ? "PASS" : "FAIL");
        check(premise && cmp_ok && deep_ok && map_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_value_codec_sharing_and_cycles -- the two properties the node table exists for.
//
// Both are checked by POINTER IDENTITY, not by equality, and that is the point: a codec
// that duplicated a shared subgraph would still compare equal everywhere, and so would one
// that unrolled a cycle into a longer ring (EQ_DEEP compares cyclic values by how they unfold).
// =============================================================================
inline void test_value_codec_sharing_and_cycles() {
    std::cout << "=== value_codec_sharing_and_cycles ===\n";
    try {
        // ---- sharing: a diamond. Two slots of the parent point at ONE child. --------
        bool share_ok = false;
        {
            Heap      src(64 * 1024);
            GcTestCtx t(&src, 16);
            Context   sctx = t.context();
            GcObject* parent = src.alloc_slots_gc(GcObject::KIND_ARRAY, 2, &sctx);
            t.win[0] = Value::fromPtr(parent->slots());
            GcObject* child = src.alloc_string_gc("shared", &sctx);
            Value*    p = GcObject::from_slots(t.win[0].asPtr())->slots();
            p[0] = Value::fromPtr(child->bytes());
            p[1] = p[0];                                   // the SAME object twice

            const auto      buf = vcodec::encode(t.win[0]);
            Heap            dst(64 * 1024);
            RootedValuePool pool;
            const Value     rebuilt = vcodec::decode(buf.data(), buf.size(), dst, nullptr, pool, nullptr, 0);
            Value*          q = GcObject::from_slots(rebuilt.asPtr())->slots();
            share_ok = q[0].isPtr() && q[0].asPtr() == q[1].asPtr();
        }
        // Reported here, not at the end: a throw in the cycle block below would otherwise
        // swallow this result too, and then a failure tells you nothing about which half
        // broke. (Found by falsifying this very test.)
        std::cout << std::format("  shared child stays ONE object: {}\n", share_ok ? "PASS" : "FAIL");

        // ---- a cycle: an object whose field points back at itself -------------------
        bool cycle_ok = false;
        {
            Heap      src(64 * 1024);
            GcTestCtx t(&src, 16);
            Context   sctx = t.context();
            GcObject* node = src.alloc_slots_gc(GcObject::KIND_ARRAY, 2, &sctx);
            t.win[0] = Value::fromPtr(node->slots());
            Value*    n = GcObject::from_slots(t.win[0].asPtr())->slots();
            n[0] = Value::fromSigned48(99);
            n[1] = t.win[0];                               // self-reference

            const auto      buf = vcodec::encode(t.win[0]);   // must TERMINATE
            Heap            dst(64 * 1024);
            RootedValuePool pool;
            const Value     rebuilt = vcodec::decode(buf.data(), buf.size(), dst, nullptr, pool, nullptr, 0);
            Value*          m = GcObject::from_slots(rebuilt.asPtr())->slots();
            // The ring must close onto the REBUILT object, not onto the source.
            cycle_ok = m[0].isInt() && m[0].asSigned48() == 99 &&
                       m[1].isPtr() && m[1].asPtr() == rebuilt.asPtr();
        }

        std::cout << std::format("  cycle terminates and closes:   {}\n", cycle_ok ? "PASS" : "FAIL");
        check(share_ok && cycle_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_value_codec_refusals -- what must NOT travel, and what a bad buffer must do.
// =============================================================================
inline void test_value_codec_refusals() {
    std::cout << "=== value_codec_refusals ===\n";
    try {
        Heap      src(64 * 1024);
        GcTestCtx t(&src, 16);
        Context   sctx = t.context();

        // A closure: its captures are arbitrary and its function id belongs to one image.
        GcObject* clo = src.alloc_closure_gc(/*fn_id*/ 3, 1, &sctx);
        t.win[0] = Value::fromPtr(clo->slots());
        GcObject::from_slots(t.win[0].asPtr())->slots()[0] = Value::fromSigned48(1);
        bool closure_refused = false;
        try { (void)vcodec::encode(t.win[0]); }
        catch (const vcodec::ValueCodecError&) { closure_refused = true; }

        // A closure nested inside an otherwise fine graph must be refused just as hard.
        GcObject* arr = src.alloc_slots_gc(GcObject::KIND_ARRAY, 1, &sctx);
        t.win[1] = Value::fromPtr(arr->slots());
        GcObject::from_slots(t.win[1].asPtr())->slots()[0] = t.win[0];
        bool nested_refused = false;
        try { (void)vcodec::encode(t.win[1]); }
        catch (const vcodec::ValueCodecError&) { nested_refused = true; }

        // A Func IMMEDIATE is NOT a closure -- an id into the shared image, so it travels.
        t.win[2] = Value::fromFunc(11);
        bool func_ok = false;
        {
            const auto      buf = vcodec::encode(t.win[2]);
            Heap            dst(16 * 1024);
            RootedValuePool pool;
            const Value     v = vcodec::decode(buf.data(), buf.size(), dst, nullptr, pool, nullptr, 0);
            func_ok = v.isFunc() && v.asFuncId() == 11;
        }

        // A truncated buffer must raise, not read past its end.
        bool truncated_refused = false;
        {
            auto buf = vcodec::encode(Value::fromSigned48(7));
            buf.resize(buf.size() - 1);
            Heap            dst(16 * 1024);
            RootedValuePool pool;
            try { (void)vcodec::decode(buf.data(), buf.size(), dst, nullptr, pool, nullptr, 0); }
            catch (const vcodec::ValueCodecError&) { truncated_refused = true; }
        }

        std::cout << std::format("  closure / nested closure refused: {} {}\n",
                                 closure_refused ? "PASS" : "FAIL", nested_refused ? "PASS" : "FAIL");
        std::cout << std::format("  Func immediate travels:           {}\n", func_ok ? "PASS" : "FAIL");
        std::cout << std::format("  truncated buffer raises:          {}\n",
                                 truncated_refused ? "PASS" : "FAIL");
        check(closure_refused && nested_refused && func_ok && truncated_refused);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// The live bytes of a KIND_BYTES value, or a marker when it is not one.
inline std::string bytes_of(Value v) {
    if (!v.isPtr()) return "<not bytes>";
    const GcObject* hdr = GcObject::from_slots(v.asPtr());
    if (hdr->kind != GcObject::KIND_BYTES) return "<not bytes>";
    const GcObject* backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
    return std::string(backing->bytes(), static_cast<size_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48()));
}

// encode_bytes builds, without a heap, the buffer the active-socket I/O thread posts. It must be
// EXACTLY what encode() makes for that value -- so decode() needs no second path -- which the round
// trip checks byte for byte.
inline void test_value_codec_encode_bytes() {
    std::cout << "=== value_codec_encode_bytes ===\n";
    try {
        auto round = [](uint8_t tag, const std::string& data, bool* same) {
            const auto      buf = vcodec::encode_bytes(tag, data.data(), data.size());
            Heap            dst(16 * 1024);
            RootedValuePool pool;
            const Value     v = vcodec::decode(buf.data(), buf.size(), dst, nullptr, pool, nullptr, 0);
            *same = vcodec::encode(v) == buf;
            return bytes_of(v);
        };
        bool same1 = false, same2 = false;
        const bool content1 = round(1, "a line", &same1) == std::string("\x01" "a line");
        const bool content2 = round(2, "", &same2) == std::string("\x02");
        std::cout << std::format("  decodes to tag + data:            {} {}\n",
                                 content1 ? "PASS" : "FAIL", content2 ? "PASS" : "FAIL");
        std::cout << std::format("  identical to encode()'s buffer:   {} {}\n",
                                 same1 ? "PASS" : "FAIL", same2 ? "PASS" : "FAIL");
        check(content1 && content2 && same1 && same2);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_value_codec_malformed -- decode() refuses every object the VM could not have built.
//
// The buffers are written by hand, byte by byte, because encode() cannot produce a bad
// one. A hand-built VALID buffer comes first: if it were refused, every refusal below
// could be the builder's fault rather than the check's. Each bad buffer must fail with
// ITS OWN message, so a check that fires for the wrong reason does not count.
// =============================================================================
namespace codec_malformed {
struct Buf {
    std::vector<uint8_t> o;
    explicit Buf(uint32_t nodes) { byteio::put_u32(o, nodes); }
    Buf& node(uint8_t kind, uint32_t n) { byteio::put_u8(o, kind); byteio::put_u32(o, n); return *this; }
    Buf& object(uint32_t n, uint16_t tid) { node(GcObject::KIND_OBJECT, n); byteio::put_u16(o, tid); return *this; }
    Buf& string(std::string_view s) {
        node(GcObject::KIND_STRING, static_cast<uint32_t>(s.size()));
        o.insert(o.end(), s.begin(), s.end());
        return *this;
    }
    Buf& tag(Value::Type t) { byteio::put_u8(o, static_cast<uint8_t>(t)); return *this; }
    Buf& ptr(uint32_t idx) { tag(Value::Type::Pointer); byteio::put_u32(o, idx); return *this; }
    Buf& num(int64_t v) { tag(Value::Type::Integer); byteio::put_u64(o, static_cast<uint64_t>(v)); return *this; }
    Buf& undef() { return tag(Value::Type::Undef); }
};

// "" -> the buffer must decode; otherwise it must raise a ValueCodecError containing `want`.
inline bool expect(const char* label, const Buf& b, const char* want,
                   const std::vector<StructType>& types) {
    Heap            dst(16 * 1024);
    RootedValuePool pool;
    std::string     got;
    try { (void)vcodec::decode(b.o.data(), b.o.size(), dst, nullptr, pool, types.data(), types.size()); }
    catch (const vcodec::ValueCodecError& e) { got = e.what(); }
    const bool ok = *want ? got.find(want) != std::string::npos : got.empty();
    std::cout << std::format("  {:<34} {}{}\n", label, ok ? "PASS" : "FAIL",
                             ok ? "" : std::format("  (got: \"{}\")", got));
    return ok;
}
} // namespace codec_malformed

inline void test_value_codec_malformed() {
    using namespace codec_malformed;
    constexpr uint8_t ARR = GcObject::KIND_ARRAY, VEC = GcObject::KIND_VEC,
                      MAP = GcObject::KIND_MAP,   BYT = GcObject::KIND_BYTES;
    std::cout << "=== value_codec_malformed (hand-built bad buffers) ===\n";
    try {
        std::vector<StructType> types(8, StructType{ 0, "", {} });
        types[7] = StructType{ 2, "Pair", { "a", "b" } };
        bool ok = true;

        // Control: a Vec of one Int over a 2-slot backing, and a struct of type 7.
        ok &= expect("valid Vec (control)",
                     Buf(2).node(VEC, 2).node(ARR, 2).ptr(1).num(1).num(5).undef().ptr(0), "", types);
        ok &= expect("valid struct (control)",
                     Buf(1).object(2, 7).num(1).num(2).ptr(0), "", types);

        ok &= expect("struct type id outside the image",
                     Buf(1).object(2, 9).num(1).num(2).ptr(0), "not in this image", types);
        ok &= expect("struct with the wrong field count",
                     Buf(1).object(3, 7).num(1).num(2).num(3).ptr(0), "does not match its type", types);
        ok &= expect("Vec header with 3 slots",
                     Buf(1).node(VEC, 3).undef().num(0).num(0).ptr(0), "wrong size", types);
        ok &= expect("Vec backed by a string",
                     Buf(2).node(VEC, 2).string("xy").ptr(1).num(0).ptr(0), "wrong kind", types);
        ok &= expect("Vec count beyond capacity",
                     Buf(2).node(VEC, 2).node(ARR, 2).ptr(1).num(3).num(1).num(2).ptr(0),
                     "vector count out of range", types);
        ok &= expect("Bytes count beyond capacity",
                     Buf(2).node(BYT, 2).string("ab").ptr(1).num(3).ptr(0),
                     "byte buffer count out of range", types);
        // Two Vecs over ONE backing: a push through either would show up in the other.
        ok &= expect("backing shared by two Vecs",
                     Buf(4).node(ARR, 2).node(VEC, 2).node(VEC, 2).node(ARR, 2)
                           .ptr(1).ptr(2)                    // root array -> both Vecs
                           .ptr(3).num(0).ptr(3).num(0)      // both Vecs -> node 3
                           .undef().undef().ptr(0),
                     "backing is shared", types);
        ok &= expect("Map backing not a power of two",
                     Buf(2).node(MAP, 3).node(ARR, 6).ptr(1).num(0).num(0)
                           .undef().undef().undef().undef().undef().undef().ptr(0),
                     "power-of-two", types);
        // One pair, and it is taken: a probe for an absent key would never stop.
        ok &= expect("Map with no empty slot",
                     Buf(2).node(MAP, 3).node(ARR, 2).ptr(1).num(1).num(1).num(10).num(20).ptr(0),
                     "no empty slot", types);
        ok &= expect("Map count disagrees with its keys",
                     Buf(2).node(MAP, 3).node(ARR, 4).ptr(1).num(1).num(1)
                           .undef().undef().undef().undef().ptr(0),
                     "disagree with its keys", types);
        ok &= expect("Map key that is an array",
                     Buf(3).node(MAP, 3).node(ARR, 4).node(ARR, 0).ptr(1).num(1).num(1)
                           .ptr(2).num(1).undef().undef().ptr(0),
                     "neither a string nor an immediate", types);
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_rooted_pool_release -- a pool unregisters exactly its own slots, in linear time.
//
// Releasing a pool used to call remove_root once per slot, each a linear search plus an
// erase that shifts the rest: ~0.5 s for a 100 k-node decode, 80x the decode itself. Two
// things are checked. EXACTNESS: roots registered before and after the pool survive, and
// they are the ones that remain (removing each by hand must succeed, one at a time).
// SPEED: releasing 200 k slots must stay far below what the quadratic version needed.
// The bound is loose on purpose -- 200 ms, against seconds for the old code even in
// Release -- so the test does not become a timing flake on a busy machine.
// =============================================================================
inline void test_rooted_pool_release() {
    std::cout << "=== rooted_pool_release (exact, and linear) ===\n";
    try {
        Heap  heap(64 * 1024);
        Value before = Value::fromNil(), after = Value::fromNil();
        heap.add_root(&before);

        std::optional<RootedValuePool> pool;
        pool.emplace();
        pool->heap = &heap;
        pool->slots.resize(200000);
        for (Value& s : pool->slots) heap.add_root(&s);
        heap.add_root(&after);                         // registered AFTER the pool's block
        const auto t0 = std::chrono::steady_clock::now();
        pool.reset();                                  // the destructor, timed
        const double release_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0).count();

        const bool count_ok = heap.root_count() == 2;
        heap.remove_root(&before);
        const bool before_ok = heap.root_count() == 1;
        heap.remove_root(&after);
        const bool after_ok = heap.root_count() == 0;
        const bool fast_ok  = release_ms < 200.0;

        std::cout << std::format("  only the pool's roots removed: {} {} {}\n", count_ok ? "PASS" : "FAIL",
                                 before_ok ? "PASS" : "FAIL", after_ok ? "PASS" : "FAIL");
        std::cout << std::format("  200 k slots released in {:.2f} ms (< 200): {}\n",
                                 release_ms, fast_ok ? "PASS" : "FAIL");
        check(count_ok && before_ok && after_ok && fast_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_value_codec_collects -- the rebuild pass must survive collections MID-REBUILD.
//
// This is the test the rooted pool exists for. The destination heap starts far too
// small for the graph, so the allocation loop collects repeatedly while half the nodes
// are built and not yet wired. Every one of them is held only by its pool slot, and the
// collector rewrites those slots in place -- which is also why the patch pass reads its
// referents out of the pool rather than out of a saved pointer.
//
// Falsifier: swap the pool for a plain std::vector<GcObject*> and this goes wrong.
// =============================================================================
inline void test_value_codec_collects() {
    std::cout << "=== value_codec_collects (rebuild across collections) ===\n";
    try {
        Heap      src(256 * 1024);
        GcTestCtx t(&src, 16);
        Context   sctx = t.context();

        constexpr int N = 400;
        GcObject* spine = src.alloc_slots_gc(GcObject::KIND_ARRAY, N, &sctx);
        t.win[0] = Value::fromPtr(spine->slots());
        for (int i = 0; i < N; ++i) {
            // Each string is its own object, so the graph is N+1 nodes wide.
            GcObject* s = src.alloc_string_gc(std::format("node-{}", i), &sctx);
            GcObject::from_slots(t.win[0].asPtr())->slots()[i] = Value::fromPtr(s->bytes());
        }

        const auto buf = vcodec::encode(t.win[0]);

        Heap            dst(1024);            // deliberately tiny -> forced to collect/grow
        RootedValuePool pool;
        const Value     rebuilt = vcodec::decode(buf.data(), buf.size(), dst, nullptr, pool, nullptr, 0);

        const bool collected = dst.stats().collections > 0;
        const bool eq_ok     = vcodec::deep_eq(t.win[0], rebuilt, &sctx);
        std::cout << std::format("  destination really collected ({}x): {}\n",
                                 dst.stats().collections, collected ? "PASS" : "FAIL");
        std::cout << std::format("  graph still intact afterwards:      {}\n", eq_ok ? "PASS" : "FAIL");
        check(collected && eq_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_thread_slot_table -- the data structure behind the POSIX fault rendezvous.
//
// WHY IT IS TESTED HERE, ON A PLATFORM THAT DOES NOT USE IT. ThreadSlotTable is
// consumed only by FaultSignals.h, which is compiled only on POSIX -- so on Windows
// the production use is unreachable. The table itself is deliberately free of any
// POSIX declaration precisely so it can still be COMPILED and EXERCISED here: it is
// the part with invariants that can be got wrong (claim/release ordering, capacity
// exhaustion, one slot per thread), and the part that is cheap to test. What stays
// untestable on Windows is only the wiring into the signal handler.
//
// The `load` path is the one that runs in signal context, so the properties that
// matter are: a thread reads back exactly what IT stored, an unknown thread reads
// nullptr (which the handler treats as "not ours" and chains onward), and a full
// table REFUSES rather than corrupting -- degraded classification, never a wrong
// landing frame.
// =============================================================================
inline void test_thread_slot_table() {
    std::cout << "=== thread_slot_table ===\n";
    int a = 1, b = 2, c = 3;

    // --- single thread: claim, nest, release -----------------------------------
    ThreadSlotTable<int, 4> t;
    const bool empty_ok = t.load(7) == nullptr;              // nothing stored yet
    t.store(7, &a);
    const bool claim_ok = t.load(7) == &a;
    t.store(7, &b);                                          // re-arm (the nesting case)
    const bool nest_ok = t.load(7) == &b && t.load(8) == nullptr;
    t.store(7, &a);                                          // retract to the enclosing frame
    const bool unnest_ok = t.load(7) == &a;
    t.store(7, nullptr);                                     // release
    const bool release_ok = t.load(7) == nullptr;
    const bool reclaim_ok = t.store(9, &c) && t.load(9) == &c;   // slot reusable afterwards
    t.store(9, nullptr);

    // --- capacity: the last store must FAIL, not overwrite a stranger's slot ----
    ThreadSlotTable<int, 4> full;
    bool fill_ok = true;
    for (uintptr_t i = 1; i <= 4; ++i) fill_ok = fill_ok && full.store(i, &a);
    const bool overflow_ok = !full.store(5, &b) && full.load(5) == nullptr;
    bool intact_ok = true;                                    // the four holders are untouched
    for (uintptr_t i = 1; i <= 4; ++i) intact_ok = intact_ok && full.load(i) == &a;

    // --- several threads at once: each must only ever see its OWN value ---------
    // The real contention is claiming a free slot; each thread then hammers its own
    // entry so a slot stolen or aliased by another thread shows up as a mismatch.
    constexpr unsigned NT = 4;
    ThreadSlotTable<int, NT> shared;
    std::atomic<bool>        race_ok{ true };
    std::vector<std::thread> workers;
    std::vector<int>         values(NT);
    for (unsigned i = 0; i < NT; ++i) values[i] = static_cast<int>(100 + i);
    for (unsigned i = 0; i < NT; ++i) {
        workers.emplace_back([&shared, &race_ok, &values, i] {
            const uintptr_t tid = static_cast<uintptr_t>(i) + 1;   // 0 means FREE
            for (int round = 0; round < 2000; ++round) {
                if (!shared.store(tid, &values[i])) { race_ok = false; return; }
                if (shared.load(tid) != &values[i]) { race_ok = false; return; }
                shared.store(tid, nullptr);
            }
        });
    }
    for (auto& w : workers) w.join();

    std::cout << std::format("  empty / claim / nest / unnest:  {} {} {} {}\n",
                             empty_ok ? "PASS" : "FAIL", claim_ok ? "PASS" : "FAIL",
                             nest_ok ? "PASS" : "FAIL", unnest_ok ? "PASS" : "FAIL");
    std::cout << std::format("  release / slot reusable:        {} {}\n",
                             release_ok ? "PASS" : "FAIL", reclaim_ok ? "PASS" : "FAIL");
    std::cout << std::format("  full table refuses, holders ok: {} {}\n",
                             overflow_ok ? "PASS" : "FAIL", intact_ok ? "PASS" : "FAIL");
    std::cout << std::format("  {} threads, 2000 rounds each:     {}\n",
                             NT, race_ok.load() ? "PASS" : "FAIL");
    check(empty_ok && claim_ok && nest_ok && unnest_ok && release_ok && reclaim_ok &&
          fill_ok && overflow_ok && intact_ok && race_ok.load());
}

// =============================================================================
// test_concurrent_execute -- several execute() instances running AT THE SAME TIME,
// each with its own Heap, StringInterner and output stream, over ONE shared image.
//
// This is the claim the whole `concurrent-execute` work exists to establish, turned
// into a test: the per-instance state really is per-instance, and the read-only image
// really is shareable. Everything the interpreter mutates is reached through
// ctx->vm->..., so if that were not so, this is where it shows.
//
// WHY THE PROGRAM ALLOCATES. A loop that only adds would prove nothing about the
// heaps: it never collects, so two instances sharing one Heap would still agree. The
// body runs TO_STRING per iteration, which allocates a throwaway string, and each
// instance starts from a deliberately small semispace -- so every instance drives its
// OWN collector through many cycles while the others are mid-flight. The test asserts
// that collections actually happened, because a run that never collected would make
// the whole thing vacuous.
//
// HOW TO FALSIFY IT (and it must be falsified before it is believed): give every
// worker the SAME Heap and StringInterner instead of its own locals. The sums must
// then go wrong or the run must fault -- a probe that passes either way would be
// pinning nothing.
// =============================================================================
struct ChurnImage {
    static constexpr uint8_t FRAME = 4;          // r0 count, r1 acc, r2 temp, r3 alloc scratch
    static constexpr int16_t N     = 2000;
    static constexpr int64_t WANT  = int64_t(N) * (int64_t(N) + 1) / 2;

    std::vector<uint32_t>    bytecode;
    std::vector<std::string> string_literals;

    ChurnImage() {
        Assembler as;
        as.func("churn", FRAME);
        as.label("main");
        as.C2(OpCode::LOAD_CONST, FRAME + 0, N);   // args for the call slide by FRAME
        as.C2(OpCode::LOAD_CONST, FRAME + 1, 0);
        as.CALL("churn");
        as.load_str(1, "done");                    // r1/r2 are in the TOP frame
        as.R6(OpCode::PRINTLN, 2, 1, 0);           // -> this instance's own `out`
        as.J(OpCode::HALT);
        as.label("churn");
        as.C2(OpCode::LOAD_CONST, 2, 0);
        as.B (OpCode::BEQ_INT, 0, 2, "churn_base");
        as.R6(OpCode::TO_STRING, 3, 0, 0);         // allocate garbage -> drive the collector
        as.R6(OpCode::ADD_INT, 2, 1, 0);
        as.C2(OpCode::DECR, 0);
        as.R6(OpCode::MOV, 1, 2, 0);
        as.TCO_CALL("churn");
        as.label("churn_base");
        as.R6(OpCode::MOV, 0, 1, 0);
        as.J(OpCode::RET);
        bytecode        = as.assemble();
        string_literals = as.string_literals();
    }
};

struct ChurnResult {
    int64_t     sum         = -1;
    std::string text;
    uint64_t    collections = 0;
    bool        threw       = false;
    std::string what;
};

// One instance. Every piece of mutable VM state here is a LOCAL: that is the contract
// under test. Only `img` is shared, and it is const.
inline void churn_run(const ChurnImage& img, ChurnResult& out) {
    try {
        Heap               heap(8 * 1024);   // small semispace -> frequent collections
        StringInterner     interner;
        std::ostringstream os;
        auto res = execute(img.bytecode, &heap, nullptr, &interner, ChurnImage::FRAME,
                           nullptr, nullptr, &img.string_literals, nullptr, nullptr, &os);
        out.sum         = res.get_reg_base()[ChurnImage::FRAME].asSigned48();
        out.text        = os.str();
        out.collections = heap.stats().collections;
    } catch (const std::exception& e) {
        out.threw = true;
        out.what  = e.what();
    }
}

inline void test_concurrent_execute() {
    std::cout << "=== concurrent_execute (4 instances, one shared image) ===\n";
    const ChurnImage img;

    ChurnResult base;                          // the single-threaded reference
    churn_run(img, base);
    const bool base_ok = !base.threw && base.sum == ChurnImage::WANT && base.text == "done\n";
    std::cout << std::format("  baseline: sum={} printed-once={} collections={} -> {}\n",
                             base.sum, base.text == "done\n", base.collections,
                             base_ok ? "PASS" : "FAIL");

    constexpr unsigned NT = 4;
    std::vector<ChurnResult> results(NT);
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < NT; ++i)
        workers.emplace_back([&img, &results, i] { churn_run(img, results[i]); });
    for (auto& w : workers) w.join();

    bool all_ok = base_ok;
    for (unsigned i = 0; i < NT; ++i) {
        const ChurnResult& r = results[i];
        // Each instance must compute the right answer, print into ITS OWN stream exactly
        // once (a shared sink would give one thread two lines and another none), and have
        // really run its own collector.
        const bool ok = !r.threw && r.sum == ChurnImage::WANT && r.text == "done\n" &&
                        r.collections > 0;
        all_ok = all_ok && ok;
        if (r.threw)
            std::cout << std::format("  instance {}: threw: {}\n", i, r.what);
        else
            std::cout << std::format("  instance {}: sum={} printed-once={} collections={} -> {}\n",
                                     i, r.sum, r.text == "done\n", r.collections,
                                     ok ? "PASS" : "FAIL");
    }
    check(all_ok);
}

// =============================================================================
// fork-join tasks -- rawSpawn / rawJoin / rawTaskTake, driven by hand-assembled programs.
//
// Every program below shares ONE set of task functions (task_fns), appended after its main
// code, so each task runs a second execute() over the very image the parent runs -- the real
// arrangement. The parent's results are checked through its registers after HALT. The top
// frame is TF = 48, so every register the tests use for a heap value lies inside the scanned
// frame; r40..r43 are the scratch registers for native-call arguments.
// =============================================================================
namespace forkjoin {
constexpr uint8_t TF = 48;

// The task functions (all arity 1):
//   tsum(n)   -> 1 + 2 + ... + n, allocating a garbage string per step so the TASK heap collects
//   tfail(x)  -> PANIC "boom"
//   techo(x)  -> x
//   tshape(x) -> a Vec [x, "from-task"] built in the task heap
//   ttalk(x)  -> prints "child", returns x
inline void task_fns(Assembler& as) {
    as.label("tsum");
    as.C2(OpCode::LOAD_CONST, 1, 0);
    as.TCO_CALL("tsum_loop");
    as.label("tsum_loop");                       // (n = r0, acc = r1)
    as.C2(OpCode::LOAD_CONST, 2, 0);
    as.B (OpCode::BEQ_INT, 0, 2, "tsum_done");
    as.R6(OpCode::TO_STRING, 3, 0, 0);           // garbage -> the task heap collects
    as.R6(OpCode::ADD_INT, 1, 1, 0);
    as.C2(OpCode::DECR, 0);
    as.TCO_CALL("tsum_loop");
    as.label("tsum_done");
    as.R6(OpCode::MOV, 0, 1, 0);
    as.J(OpCode::RET);

    as.label("tfail");
    as.load_str(1, "boom");
    as.R6(OpCode::PANIC, 0, 1, 0);
    as.J(OpCode::RET);

    as.label("techo");
    as.J(OpCode::RET);

    as.label("tshape");
    as.VEC_NEW(1);
    as.VEC_PUSH(1, 0);
    as.load_str(2, "from-task");
    as.VEC_PUSH(1, 2);
    as.R6(OpCode::MOV, 0, 1, 0);
    as.J(OpCode::RET);

    as.label("ttalk");
    as.load_str(1, "child");
    as.R6(OpCode::PRINTLN, 2, 1, 0);
    as.J(OpCode::RET);

    // tdeep(n) = 0 if n == 0, else 1 + tdeep(n - 1): a real (non-tail) CALL chain n deep.
    as.label("tdeep");
    as.load_const(1, 0);
    as.B (OpCode::BEQ_INT, 0, 1, "tdeep_base");
    as.R6(OpCode::MOV, 3, 0, 0);
    as.C2(OpCode::DECR, 3);
    as.CALL("tdeep");
    as.load_const(1, 1);
    as.R6(OpCode::ADD_INT, 0, 3, 1);
    as.J(OpCode::RET);
    as.label("tdeep_base");
    as.load_const(0, 0);
    as.J(OpCode::RET);

    // The actor functions (all take one ignored start value; frame: r1 = own id, r2/r3 = native
    // arguments, r4 = mail kind, r5 = native-id scratch, r6 = message, r7 = temp):
    //   aecho(_)  -> for every message n, sends 2n to the MAIN program (address 0); ends at Stop
    //   acrash(_) -> PANIC "boom" at once
    //   astop(_)  -> ignores messages; at Stop prints "stopped" and ends
    as.label("aecho");
    as.call_native_id(1, 5, 0, 0, NATIVE_SELF_ID);
    as.label("aecho_loop");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);
    as.load_const(7, 3);
    as.B (OpCode::BEQ_INT, 4, 7, "aecho_done");
    as.load_const(7, 1);
    as.B (OpCode::BNE_INT, 4, 7, "aecho_loop");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.call_native_id(6, 5, 2, 1, NATIVE_MAIL_MSG);
    as.R6(OpCode::ADD_INT, 6, 6, 6);
    as.load_const(2, 0);
    as.R6(OpCode::MOV, 3, 6, 0);
    as.call_native_id(7, 5, 2, 2, NATIVE_SEND);
    as.J(OpCode::J, "aecho_loop");
    as.label("aecho_done");
    as.load_const(0, 0);
    as.J(OpCode::RET);

    as.label("acrash");
    as.load_str(1, "boom");
    as.R6(OpCode::PANIC, 0, 1, 0);
    as.J(OpCode::RET);

    // acrashon(_): waits for one message, then faults -- a crash the test can time.
    as.label("acrashon");
    as.call_native_id(1, 5, 0, 0, NATIVE_SELF_ID);
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);
    as.load_str(6, "boom on demand");
    as.R6(OpCode::PANIC, 0, 6, 0);
    as.J(OpCode::RET);

    // awatch(pid): monitors the actor its ARGUMENT addresses -- an actor it did not start -- and sends
    // the address from the report to the main program, so the test can check who it was told about.
    as.label("awatch");
    as.call_native_id(1, 5, 0, 0, NATIVE_SELF_ID);
    as.R6(OpCode::MOV, 2, 0, 0);
    as.R6(OpCode::MOV, 3, 1, 0);
    as.call_native_id(4, 5, 2, 2, NATIVE_MONITOR);
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, -1);
    as.call_native_id(6, 5, 2, 2, NATIVE_RECEIVE);
    as.R6(OpCode::MOV, 2, 1, 0);
    as.call_native_id(6, 5, 2, 1, NATIVE_MAIL_FROM);
    as.load_const(2, 0);
    as.R6(OpCode::MOV, 3, 6, 0);
    as.call_native_id(7, 5, 2, 2, NATIVE_SEND);
    as.load_const(0, 0);
    as.J(OpCode::RET);

    // abusy(inbox_id): an actor that NEVER receives -- its loop is its own work. It cannot be stopped by
    // mail, so it asks rawStopRequested instead, and ends itself. Then it reports 77 to the main program.
    // The argument is the inbox to ask about: 0 means "my own", anything else the slot it was started
    // into (whose id is not the actor's own).
    as.label("abusy");
    as.R6(OpCode::MOV, 1, 0, 0);
    as.load_const(7, 0);
    as.B (OpCode::BNE_INT, 1, 7, "abusy_loop");
    as.call_native_id(1, 5, 0, 0, NATIVE_SELF_ID);
    as.label("abusy_loop");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.call_native_id(4, 5, 2, 1, NATIVE_STOP_REQUESTED);
    as.B1(OpCode::BF, 4, "abusy_loop");
    as.load_const(2, 0);
    as.load_const(3, 77);
    as.call_native_id(7, 5, 2, 2, NATIVE_SEND);
    as.load_const(0, 0);
    as.J(OpCode::RET);

    // afill(target_id): sends to the inbox its ARGUMENT names, for ever, and never receives. Two of
    // these pointed at each other -- or one pointed at the main program, which points back -- is a
    // cycle of blocked sends, which ends in a located fault rather than in a wait nobody can break.
    as.label("afill");
    as.R6(OpCode::MOV, 1, 0, 0);            // r1 = the target inbox id (the start value)
    as.load_const(6, 1);
    as.label("afill_loop");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.R6(OpCode::MOV, 3, 6, 0);
    as.call_native_id(7, 5, 2, 2, NATIVE_SEND);
    as.J(OpCode::J, "afill_loop");

    // aselect(_): parks in a SELECT over its own inbox and a second one, with no deadline. Whatever
    // wakes it, it receives that inbox and sends the mail KIND to the main program -- so a test can
    // see that Stop reaches an actor sleeping in a select, not only one sleeping in a receive.
    as.label("aselect");
    as.call_native_id(1, 5, 0, 0, NATIVE_SELF_ID);
    as.load_const(2, 0);
    as.call_native_id(6, 5, 2, 1, NATIVE_NEW_INBOX);     // r6 = a second inbox
    as.VEC_NEW(7);
    as.VEC_PUSH(7, 1);
    as.VEC_PUSH(7, 6);
    as.R6(OpCode::MOV, 2, 7, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_SELECT);        // r4 = the index that woke us
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(7, 0);
    as.B (OpCode::BEQ_INT, 4, 7, "aselect_read");
    as.R6(OpCode::MOV, 2, 6, 0);
    as.label("aselect_read");
    as.load_const(3, 0);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);       // cannot wait: select said it is ready
    as.load_const(2, 0);
    as.R6(OpCode::MOV, 3, 4, 0);
    as.call_native_id(7, 5, 2, 2, NATIVE_SEND);
    as.load_const(0, 0);
    as.J(OpCode::RET);

    // aslot(inbox_id): like aecho, but it receives on the inbox its ARGUMENT names -- the slot it was
    // started into, whose id is not its own (rawSpawnInto). Doubles each message to the main program.
    as.label("aslot");
    as.R6(OpCode::MOV, 1, 0, 0);
    as.label("aslot_loop");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);
    as.load_const(7, 3);
    as.B (OpCode::BEQ_INT, 4, 7, "aslot_done");
    as.load_const(7, 1);
    as.B (OpCode::BNE_INT, 4, 7, "aslot_loop");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.call_native_id(6, 5, 2, 1, NATIVE_MAIL_MSG);
    as.R6(OpCode::ADD_INT, 6, 6, 6);
    as.load_const(2, 0);
    as.R6(OpCode::MOV, 3, 6, 0);
    as.call_native_id(7, 5, 2, 2, NATIVE_SEND);
    as.J(OpCode::J, "aslot_loop");
    as.label("aslot_done");
    as.load_const(0, 0);
    as.J(OpCode::RET);

    // aconn(_): receives a hand-off ticket, takes the connection, writes "hi", closes it, tells the
    // main program it is done (sends 1 to address 0), then waits for Stop.
    as.label("aconn");
    as.call_native_id(1, 5, 0, 0, NATIVE_SELF_ID);
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);
    as.R6(OpCode::MOV, 2, 1, 0);
    as.call_native_id(6, 5, 2, 1, NATIVE_MAIL_MSG);            // r6 = the ticket
    as.R6(OpCode::MOV, 2, 6, 0);
    as.call_native_id(7, 5, 2, 1, NATIVE_TAKE);                // r7 = the connection, here
    as.load_str(2, "hi");
    as.BYTES_FROM_STR(3, 2);
    as.R6(OpCode::MOV, 2, 7, 0);
    as.call_native_id(4, 5, 2, 2, NATIVE_TCP_SEND);
    as.R6(OpCode::MOV, 2, 7, 0);
    as.call_native_id(4, 5, 2, 1, NATIVE_TCP_CLOSE);
    as.load_const(2, 0);
    as.load_const(3, 1);
    as.call_native_id(4, 5, 2, 2, NATIVE_SEND);
    as.label("aconn_wait");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);
    as.load_const(7, 3);
    as.B (OpCode::BNE_INT, 4, 7, "aconn_wait");
    as.load_const(0, 0);
    as.J(OpCode::RET);

    as.label("astop");
    as.call_native_id(1, 5, 0, 0, NATIVE_SELF_ID);
    as.label("astop_loop");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);
    as.load_const(7, 3);
    as.B (OpCode::BNE_INT, 4, 7, "astop_loop");
    as.load_str(6, "stopped");
    as.R6(OpCode::PRINTLN, 7, 6, 0);
    as.load_const(0, 0);
    as.J(OpCode::RET);

    // Extra and bounded inboxes (same frame layout as above, plus r8 = gate inbox, r9 = the next
    // expected message, r10 = sum):
    //   areply(_)  -> every message is an inbox address; answers 7 there. Ends at Stop.
    //   agate(n)   -> makes an extra inbox (the GATE), sends its id to the main program, and waits on
    //                 it. Then:
    //                 * n > 0 (after "go"): consumes messages 1..n from its main inbox, checking their
    //                   order, sends the sum to the main program (-1 if out of order), waits for Stop;
    //                 * n = 0 (the gate got Stop): drains its main inbox until Stop, printing "A stopped"
    //                   -- or "A no stop" if two seconds pass without it.
    //   aflood(p)  -> sends 1 to the main program, then 5 to p (blocking if p's inbox is full), prints
    //                 "B done" once the send returns (whatever it answered), waits for Stop.
    as.label("areply");
    as.call_native_id(1, 5, 0, 0, NATIVE_SELF_ID);
    as.label("areply_loop");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);
    as.load_const(7, 3);
    as.B (OpCode::BEQ_INT, 4, 7, "areply_done");
    as.load_const(7, 1);
    as.B (OpCode::BNE_INT, 4, 7, "areply_loop");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.call_native_id(6, 5, 2, 1, NATIVE_MAIL_MSG);
    as.R6(OpCode::MOV, 2, 6, 0);
    as.load_const(3, 7);
    as.call_native_id(7, 5, 2, 2, NATIVE_SEND);
    as.J(OpCode::J, "areply_loop");
    as.label("areply_done");
    as.load_const(0, 0);
    as.J(OpCode::RET);

    as.label("agate");
    as.call_native_id(1, 5, 0, 0, NATIVE_SELF_ID);
    as.load_const(2, 0);
    as.call_native_id(8, 5, 2, 1, NATIVE_NEW_INBOX);           // r8 = the gate
    as.load_const(2, 0);
    as.R6(OpCode::MOV, 3, 8, 0);
    as.call_native_id(7, 5, 2, 2, NATIVE_SEND);                // tell the main program
    as.R6(OpCode::MOV, 2, 8, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);             // "go", or Stop
    as.load_const(7, 0);
    as.B (OpCode::BEQ_INT, 0, 7, "agate_drain");
    as.load_const(9, 1);
    as.load_const(10, 0);
    as.label("agate_loop");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);
    as.load_const(7, 3);
    as.B (OpCode::BEQ_INT, 4, 7, "agate_bad");
    as.load_const(7, 1);
    as.B (OpCode::BNE_INT, 4, 7, "agate_loop");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.call_native_id(6, 5, 2, 1, NATIVE_MAIL_MSG);
    as.B (OpCode::BNE_INT, 6, 9, "agate_bad");
    as.R6(OpCode::ADD_INT, 10, 10, 6);
    as.load_const(7, 1);
    as.R6(OpCode::ADD_INT, 9, 9, 7);
    as.B (OpCode::BNE_INT, 6, 0, "agate_loop");
    as.label("agate_report");
    as.load_const(2, 0);
    as.R6(OpCode::MOV, 3, 10, 0);
    as.call_native_id(7, 5, 2, 2, NATIVE_SEND);
    as.label("agate_wait");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);
    as.load_const(7, 3);
    as.B (OpCode::BNE_INT, 4, 7, "agate_wait");
    as.load_const(0, 0);
    as.J(OpCode::RET);
    as.label("agate_bad");
    as.load_const(10, -1);
    as.J(OpCode::J, "agate_report");
    as.label("agate_drain");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, 2000);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);
    as.load_const(7, 3);
    as.B (OpCode::BEQ_INT, 4, 7, "agate_stopped");
    as.load_const(7, 0);
    as.B (OpCode::BEQ_INT, 4, 7, "agate_nostop");
    as.J(OpCode::J, "agate_drain");
    as.label("agate_stopped");
    as.load_str(6, "A stopped");
    as.R6(OpCode::PRINTLN, 7, 6, 0);
    as.load_const(0, 0);
    as.J(OpCode::RET);
    as.label("agate_nostop");
    as.load_str(6, "A no stop");
    as.R6(OpCode::PRINTLN, 7, 6, 0);
    as.load_const(0, 0);
    as.J(OpCode::RET);

    as.label("aflood");
    as.call_native_id(1, 5, 0, 0, NATIVE_SELF_ID);
    as.load_const(2, 0);
    as.load_const(3, 1);
    as.call_native_id(7, 5, 2, 2, NATIVE_SEND);                // hello
    as.R6(OpCode::MOV, 2, 0, 0);
    as.load_const(3, 5);
    as.call_native_id(7, 5, 2, 2, NATIVE_SEND);                // may block
    as.load_str(6, "B done");                                  // past the send, whatever it answered
    as.R6(OpCode::PRINTLN, 7, 6, 0);
    as.label("aflood_wait");
    as.R6(OpCode::MOV, 2, 1, 0);
    as.load_const(3, -1);
    as.call_native_id(4, 5, 2, 2, NATIVE_RECEIVE);
    as.load_const(7, 3);
    as.B (OpCode::BNE_INT, 4, 7, "aflood_wait");
    as.load_const(0, 0);
    as.J(OpCode::RET);

    // tinbox(_): a TASK asking for an inbox (refused: tasks have no mail).
    as.label("tinbox");
    as.load_const(1, 0);
    as.call_native_id(2, 3, 1, 1, NATIVE_NEW_INBOX);
    as.J(OpCode::RET);
}

inline void declare_task_fns(Assembler& as) {
    as.declare_fn("tsum",   4, 1);
    as.func("tsum_loop", 4);
    as.declare_fn("tfail",  2, 1);
    as.declare_fn("techo",  1, 1);
    as.declare_fn("tshape", 4, 1);
    as.declare_fn("ttalk",  3, 1);
    as.declare_fn("tdeep",  3, 1);
    as.declare_fn("aecho",  8, 1);
    as.declare_fn("acrash", 2, 1);
    as.declare_fn("aslot",  8, 1);
    as.declare_fn("acrashon", 8, 1);
    as.declare_fn("awatch", 8, 1);
    as.declare_fn("abusy",  8, 1);
    as.declare_fn("aselect", 8, 1);
    as.declare_fn("afill",  8, 1);
    as.declare_fn("astop",  8, 1);
    as.declare_fn("aconn",  8, 1);
    as.declare_fn("areply", 8, 1);
    as.declare_fn("agate",  12, 1);
    as.declare_fn("aflood", 8, 1);
    as.declare_fn("tinbox", 4, 1);
}

// id -> r[rd]: spawnActorBounded fn(arg, capacity), where arg is already in r44.
inline void spawn_actor_bounded(Assembler& as, uint8_t rd, const char* fn, int64_t capacity) {
    as.LOAD_FN(43, as.func_id(fn));
    as.load_const(45, capacity);
    as.call_native_id(rd, 42, 43, 3, NATIVE_ACTOR_SPAWN_BOUNDED);
}

// r[rd] = native(r[ra]) / native(r[ra], r[rb]), through the argument scratch r40 / r41.
inline void call1(Assembler& as, uint8_t rd, uint16_t native, uint8_t ra) {
    as.R6(OpCode::MOV, 40, ra, 0);
    as.call_native_id(rd, 42, 40, 1, native);
}
inline void call2(Assembler& as, uint8_t rd, uint16_t native, uint8_t ra, uint8_t rb) {
    as.R6(OpCode::MOV, 40, ra, 0);
    as.R6(OpCode::MOV, 41, rb, 0);
    as.call_native_id(rd, 42, 40, 2, native);
}

// id -> r[rd]: rawNewSlot(capacity) -- an address with no actor yet.
inline void new_slot(Assembler& as, uint8_t rd, int64_t capacity) {
    as.load_const(43, capacity);
    as.call_native_id(rd, 42, 43, 1, NATIVE_NEW_SLOT);
}
// id -> r[rd]: rawSpawnInto(slot, fn, arg), with the slot's id in r[rslot] and as the start value
// (which is what an actor in a slot needs: the inbox it receives on is the slot, not itself).
inline void spawn_into(Assembler& as, uint8_t rd, uint8_t rslot, const char* fn) {
    as.R6(OpCode::MOV, 43, rslot, 0);
    as.LOAD_FN(44, as.func_id(fn));
    as.R6(OpCode::MOV, 45, rslot, 0);
    as.call_native_id(rd, 42, 43, 3, NATIVE_SPAWN_INTO);
}
// id -> r[rd]: spawnActor fn(arg), where arg is already in r41.
inline void spawn_actor(Assembler& as, uint8_t rd, const char* fn) {
    as.LOAD_FN(40, as.func_id(fn));
    as.call_native_id(rd, 42, 40, 2, NATIVE_ACTOR_SPAWN);
}
// ok -> r[rd]: send the Int `n` to the actor whose address is in r[rpid].
inline void send_int(Assembler& as, uint8_t rd, uint8_t rpid, int64_t n) {
    as.R6(OpCode::MOV, 40, rpid, 0);
    as.load_const(41, n);
    as.call_native_id(rd, 42, 40, 2, NATIVE_SEND);
}
// kind -> r[rkind]: receive on the MAIN inbox (address 0), waiting at most `ms` (-1 = forever).
inline void receive_main(Assembler& as, uint8_t rkind, int64_t ms) {
    as.load_const(40, 0);
    as.load_const(41, ms);
    as.call_native_id(rkind, 42, 40, 2, NATIVE_RECEIVE);
}
// The main inbox's current mail, read by `native` (MAIL_MSG / MAIL_FROM / MAIL_REASON) -> r[rd].
inline void mail_read(Assembler& as, uint8_t rd, uint16_t native) {
    as.load_const(40, 0);
    as.call_native_id(rd, 42, 40, 1, native);
}
inline void main_inbox(Assembler& as, uint8_t rd) {
    as.call_native_id(rd, 42, 40, 0, NATIVE_MAIN_INBOX);
}

// id -> r[rd]: spawn fn(arg), where arg is already in r41.
inline void spawn(Assembler& as, uint8_t rd, const char* fn) {
    as.LOAD_FN(40, as.func_id(fn));
    as.call_native_id(rd, 42, 40, 2, NATIVE_TASK_SPAWN);
}
// join the task whose id is in r[rid]: ok -> r[rok], result / message -> r[rres].
inline void join(Assembler& as, uint8_t rid, uint8_t rok, uint8_t rres) {
    as.R6(OpCode::MOV, 43, rid, 0);
    as.call_native_id(rok, 42, 43, 1, NATIVE_TASK_JOIN);
    as.call_native_id(rres, 42, 43, 1, NATIVE_TASK_TAKE);
}

struct Run {
    std::vector<Value> regs;
    std::string        printed;
    std::string        fault;     // non-empty iff execute() threw
};

// Assemble main (already emitted by the caller) + the task functions, and run it. The Heap
// outlives the copied registers, so pointers in `regs` stay readable for the checks.
inline Run run(Assembler& as, Heap& heap) {
    as.J(OpCode::HALT);
    task_fns(as);
    const auto code  = as.assemble();
    const auto slits = as.string_literals();
    const auto nat   = build_native_table();
    StringInterner     interner;
    std::ostringstream os;
    Run r;
    try {
        auto res = execute(code, &heap, nullptr, &interner, TF, nullptr, nullptr, &slits, nullptr,
                           &as.function_table(), &os, nullptr, 0, 0, nullptr, nullptr, nullptr, &nat);
        r.regs.assign(res.get_reg_base(), res.get_reg_base() + TF);
    } catch (const std::exception& e) { r.fault = e.what(); }
    r.printed = os.str();
    return r;
}

inline std::string str_of(Value v) {
    if (!v.isPtr()) return "<not a string>";
    const GcObject* o = GcObject::from_slots(v.asPtr());
    if (o->kind != GcObject::KIND_STRING) return "<not a string>";
    return std::string(o->bytes(), o->string_length());
}
} // namespace forkjoin

// Eight tasks at once, each summing on its own heap; every result must come back, in id order.
inline void test_task_parallel_sums() {
    using namespace forkjoin;
    std::cout << "=== task_parallel_sums (8 tasks, own heaps) ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        for (uint8_t i = 0; i < 8; ++i) {
            as.load_const(41, 1000 + 250 * i);
            spawn(as, static_cast<uint8_t>(10 + i), "tsum");
        }
        for (uint8_t i = 0; i < 8; ++i)
            join(as, static_cast<uint8_t>(10 + i), static_cast<uint8_t>(20 + i), static_cast<uint8_t>(30 + i));
        Heap heap;
        const Run r = run(as, heap);
        bool ok = r.fault.empty();
        for (int i = 0; ok && i < 8; ++i) {
            const int64_t n = 1000 + 250 * i;
            ok = r.regs[20 + i].isBool() && r.regs[20 + i].asBool() &&
                 r.regs[30 + i].isInt() && r.regs[30 + i].asSigned48() == n * (n + 1) / 2;
        }
        std::cout << std::format("  all 8 sums correct: {}{}\n", ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A task that faults: join says false, take gives the message, and the parent carries on.
inline void test_task_failure() {
    using namespace forkjoin;
    std::cout << "=== task_failure (a task fault is a value to the parent) ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        as.load_const(41, 0);  spawn(as, 10, "tfail");  join(as, 10, 20, 30);
        as.load_const(41, 10); spawn(as, 11, "tsum");   join(as, 11, 21, 31);
        Heap heap;
        const Run r = run(as, heap);
        const bool failed_ok = r.fault.empty() && r.regs[20].isBool() && !r.regs[20].asBool() &&
                               str_of(r.regs[30]).find("boom") != std::string::npos;
        const bool after_ok  = r.fault.empty() && r.regs[21].isBool() && r.regs[21].asBool() &&
                               r.regs[31].isInt() && r.regs[31].asSigned48() == 55;
        std::cout << std::format("  failed task -> false + message:  {}  (\"{}\")\n",
                                 failed_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? str_of(r.regs[30]) : r.fault);
        std::cout << std::format("  parent continues, next task ok:  {}\n", after_ok ? "PASS" : "FAIL");
        check(failed_ok && after_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// Heap values both ways: an argument built by the parent comes back equal but copied, and a
// value built in the task heap arrives intact.
inline void test_task_values() {
    using namespace forkjoin;
    std::cout << "=== task_values (heap values in and out) ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        as.VEC_NEW(5); as.load_const(6, 7); as.VEC_PUSH(5, 6);
        as.load_str(6, "hi"); as.VEC_PUSH(5, 6);                 // r5 = [7, "hi"]
        as.R6(OpCode::MOV, 41, 5, 0);  spawn(as, 10, "techo");  join(as, 10, 20, 30);
        as.R6(OpCode::EQ_DEEP, 31, 5, 30);                        // echo came back equal
        as.load_const(41, 3);          spawn(as, 11, "tshape"); join(as, 11, 21, 32);
        as.VEC_NEW(7); as.load_const(6, 3); as.VEC_PUSH(7, 6);
        as.load_str(6, "from-task"); as.VEC_PUSH(7, 6);           // r7 = [3, "from-task"]
        as.R6(OpCode::EQ_DEEP, 33, 7, 32);
        Heap heap;
        const Run r = run(as, heap);
        const bool echo_ok  = r.fault.empty() && r.regs[31].isBool() && r.regs[31].asBool() &&
                              r.regs[30].isPtr() && r.regs[30].asPtr() != r.regs[5].asPtr();
        const bool shape_ok = r.fault.empty() && r.regs[33].isBool() && r.regs[33].asBool();
        std::cout << std::format("  argument echoed, equal but copied: {}{}\n", echo_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  task-built Vec arrives intact:     {}\n", shape_ok ? "PASS" : "FAIL");
        check(echo_ok && shape_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A task's output reaches the parent's stream at join, not while it runs. A task nobody joins
// is still waited for when execute() ends (no crash, no hang) -- and what it printed is dropped.
inline void test_task_output_and_unjoined() {
    using namespace forkjoin;
    std::cout << "=== task_output_and_unjoined ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        as.load_const(41, 1);     spawn(as, 10, "ttalk"); join(as, 10, 20, 30);
        as.load_const(41, 2);     spawn(as, 11, "ttalk");          // never joined
        as.load_const(41, 20000); spawn(as, 12, "tsum");           // never joined, still running
        Heap heap;
        const Run r = run(as, heap);
        const bool out_ok = r.fault.empty() && r.printed == "child\n";
        std::cout << std::format("  joined task's output only, once:   {}  (\"{}\")\n",
                                 out_ok ? "PASS" : "FAIL", r.fault.empty() ? r.printed : r.fault);
        std::cout << "  unjoined tasks collected at the end: PASS (returned)\n";
        check(out_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A task starts with SMALL stacks (VM_Resources::ISOLATE_*: 1024 return frames), so a deep call
// chain in a task exercises the on-demand growth from that small start -- 200 000 frames is many
// doublings past it. Falsifier: a task whose stacks could not grow faults with "stack overflow".
inline void test_task_deep_recursion() {
    using namespace forkjoin;
    std::cout << "=== task_deep_recursion (stacks grow from a small start) ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        as.load_const(41, 200000);  spawn(as, 10, "tdeep");  join(as, 10, 20, 30);
        Heap heap;
        const Run r = run(as, heap);
        const bool ok = r.fault.empty() && r.regs[20].isBool() && r.regs[20].asBool() &&
                        r.regs[30].isInt() && r.regs[30].asSigned48() == 200000;
        std::cout << std::format("  200000 nested calls in a task: {}{}\n", ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? (r.regs[20].isBool() && !r.regs[20].asBool()
                                                        ? "  (task failed: " + str_of(r.regs[30]) + ")" : "")
                                                 : "  (fault: " + r.fault + ")");
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// Misuse faults with a located message instead of running anything.
inline void test_task_misuse() {
    using namespace forkjoin;
    std::cout << "=== task_misuse (located faults) ===\n";
    auto expect = [](const char* label, const Run& r, const char* want) {
        const bool ok = r.fault.find(want) != std::string::npos;
        std::cout << std::format("  {:<32} {}{}\n", label, ok ? "PASS" : "FAIL",
                                 ok ? "" : "  (got: \"" + r.fault + "\")");
        return ok;
    };
    try {
        bool ok = true;
        {   // an Int where the function should be
            Assembler as; declare_task_fns(as); as.label("main");
            as.load_const(40, 5); as.load_const(41, 1);
            as.call_native_id(10, 42, 40, 2, NATIVE_TASK_SPAWN);
            Heap heap;
            ok &= expect("spawn of a non-function", run(as, heap), "named top-level function");
        }
        {   // rawTaskInput outside a task
            Assembler as; declare_task_fns(as); as.label("main");
            as.call_native_id(10, 42, 43, 0, NATIVE_TASK_INPUT);
            Heap heap;
            ok &= expect("task input outside a task", run(as, heap), "not a task");
        }
        {   // joining twice
            Assembler as; declare_task_fns(as); as.label("main");
            as.load_const(41, 3); forkjoin::spawn(as, 10, "techo");
            forkjoin::join(as, 10, 20, 30);
            as.call_native_id(21, 42, 43, 1, NATIVE_TASK_JOIN);
            Heap heap;
            ok &= expect("joining a task twice", run(as, heap), "already joined");
        }
        {   // an id that was never handed out
            Assembler as; declare_task_fns(as); as.label("main");
            as.load_const(43, 7);
            as.call_native_id(20, 42, 43, 1, NATIVE_TASK_JOIN);
            Heap heap;
            ok &= expect("joining an unknown id", run(as, heap), "not a task of this program");
        }
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// actors -- rawSpawnActor / rawSend / rawReceive / rawMailMsg / rawMailFrom / rawMailReason /
// rawMainInbox / rawSelfId, on the same world-wide runtime as the tasks above. Each program's
// root takes the main inbox (address 0), which is where aecho replies. Every test also checks,
// by returning at all, that the end of the world stops the actors: aecho and astop loop on
// receive until they get Stop.
// =============================================================================

// A message there and a reply back, twice: the copy crosses heaps in both directions, and an
// actor keeps its state (it is still there for the second message).
inline void test_actor_ping_pong() {
    using namespace forkjoin;
    std::cout << "=== actor_ping_pong ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        as.load_const(41, 0);  spawn_actor(as, 11, "aecho");
        send_int(as, 12, 11, 21);
        receive_main(as, 13, -1);  mail_read(as, 14, NATIVE_MAIL_MSG);
        send_int(as, 15, 11, 50);
        receive_main(as, 16, -1);  mail_read(as, 17, NATIVE_MAIL_MSG);
        Heap heap;
        const Run r = run(as, heap);
        const bool ok = r.fault.empty() && r.regs[12].isBool() && r.regs[12].asBool() &&
                        r.regs[13].isInt() && r.regs[13].asSigned48() == 1 &&
                        r.regs[14].isInt() && r.regs[14].asSigned48() == 42 &&
                        r.regs[17].isInt() && r.regs[17].asSigned48() == 100;
        std::cout << std::format("  21 -> 42, then 50 -> 100:  {}{}\n", ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// An actor that faults is reported to its starter -- in the same stream as messages, with its id
// and the fault message -- and from then on a send to it is refused.
inline void test_actor_crash_reported() {
    using namespace forkjoin;
    std::cout << "=== actor_crash_reported ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        as.load_const(41, 0);  spawn_actor(as, 11, "acrash");
        receive_main(as, 13, -1);
        mail_read(as, 14, NATIVE_MAIL_FROM);
        mail_read(as, 15, NATIVE_MAIL_REASON);
        send_int(as, 16, 11, 1);
        Heap heap;
        const Run r = run(as, heap);
        const bool report_ok = r.fault.empty() && r.regs[13].isInt() && r.regs[13].asSigned48() == 2 &&
                               r.regs[14].isInt() && r.regs[11].isInt() &&
                               r.regs[14].asSigned48() == r.regs[11].asSigned48() &&
                               str_of(r.regs[15]).find("boom") != std::string::npos;
        const bool refused_ok = r.fault.empty() && r.regs[16].isBool() && !r.regs[16].asBool();
        std::cout << std::format("  exit report: who + why:     {}{}\n", report_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  send to it afterwards false: {}\n", refused_ok ? "PASS" : "FAIL");
        check(report_ok && refused_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A receive with a timeout on an empty inbox gives up with kind 0 (and waited at least roughly
// that long); and at the end of the program an actor waiting on receive gets Stop.
inline void test_actor_timeout_and_stop() {
    using namespace forkjoin;
    std::cout << "=== actor_timeout_and_stop ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        as.load_const(41, 0);  spawn_actor(as, 11, "astop");
        receive_main(as, 13, 40);
        Heap heap;
        const auto t0 = std::chrono::steady_clock::now();
        const Run r = run(as, heap);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const bool timeout_ok = r.fault.empty() && r.regs[13].isInt() && r.regs[13].asSigned48() == 0 && ms >= 30.0;
        const bool stop_ok    = r.fault.empty() && r.printed == "stopped\n";
        std::cout << std::format("  timed-out receive -> 0 ({:.0f} ms): {}{}\n", ms, timeout_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  actor stopped at the end:      {}  (\"{}\")\n", stop_ok ? "PASS" : "FAIL", r.printed);
        check(timeout_ok && stop_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// Many actors at once, each answering one message; every reply arrives (in some order).
inline void test_actor_many() {
    using namespace forkjoin;
    constexpr int N = 200;
    std::cout << "=== actor_many (" << N << " actors) ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        for (int i = 1; i <= N; ++i) {
            as.load_const(41, 0);  spawn_actor(as, 11, "aecho");
            send_int(as, 12, 11, i);
        }
        as.load_const(20, 0);                                   // r20 = sum of the replies
        for (int i = 1; i <= N; ++i) {
            receive_main(as, 13, -1);
            mail_read(as, 14, NATIVE_MAIL_MSG);
            as.R6(OpCode::ADD_INT, 20, 20, 14);
        }
        Heap heap;
        const Run r = run(as, heap);
        const bool ok = r.fault.empty() && r.regs[20].isInt() && r.regs[20].asSigned48() == int64_t{N} * (N + 1);
        std::cout << std::format("  sum of {} replies = {}: {}{}\n", N, int64_t{N} * (N + 1), ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A connection handed to an actor: the socket moves between two isolates' registries. Over loopback,
// with the port chosen by the OS. The root keeps its old descriptor and uses it on purpose, twice --
// the second time after a new connection has reused the very SLOT it named, which is what the
// descriptor's generation exists for: without it, the stale descriptor would reach the stranger.
inline void test_actor_socket_hand_off() {
    using namespace forkjoin;
    std::cout << "=== actor_socket_hand_off ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        as.load_const(20, 0);          call1(as, 21, NATIVE_TCP_LISTEN, 20);        // listener
        call1(as, 22, NATIVE_TCP_LOCAL_PORT, 21);                                   // its port
        as.load_str(23, "127.0.0.1");  call2(as, 24, NATIVE_TCP_CONNECT, 23, 22);   // client
        call1(as, 25, NATIVE_TCP_ACCEPT, 21);                                       // server side
        main_inbox(as, 10);
        as.load_const(41, 0);          spawn_actor(as, 11, "aconn");
        call1(as, 26, NATIVE_HAND_OFF, 25);                                         // ticket
        call2(as, 27, NATIVE_SEND, 11, 26);
        receive_main(as, 28, -1);      mail_read(as, 29, NATIVE_MAIL_MSG);          // the actor is done
        as.load_const(30, 16);         call2(as, 31, NATIVE_TCP_RECV, 24, 30);      // "hi"
        as.R6(OpCode::LEN, 32, 31, 0);
        as.load_str(33, "x");          as.BYTES_FROM_STR(34, 33);
        call2(as, 35, NATIVE_TCP_SEND, 25, 34);                                     // stale: refused
        call2(as, 36, NATIVE_TCP_CONNECT, 23, 22);                                  // reuses the slot
        call1(as, 37, NATIVE_TCP_ACCEPT, 21);
        call2(as, 38, NATIVE_TCP_SEND, 25, 34);                                     // still refused
        call1(as, 39, NATIVE_TAKE, 26);                                             // taken already
        Heap heap;
        const Run r = run(as, heap);
        const bool moved_ok  = r.fault.empty() && r.regs[29].isInt() && r.regs[29].asSigned48() == 1 &&
                               r.regs[32].isInt() && r.regs[32].asSigned48() == 2;
        const bool stale_ok  = r.fault.empty() &&
                               str_of(r.regs[35]).find("handed to another actor") != std::string::npos;
        const bool reused    = r.fault.empty() && r.regs[36].isInt() && r.regs[25].isInt() &&
                               (r.regs[36].asSigned48() & 0xFFFFFF) == (r.regs[25].asSigned48() & 0xFFFFFF);
        const bool stale2_ok = r.fault.empty() &&
                               str_of(r.regs[38]).find("handed to another actor") != std::string::npos;
        const bool twice_ok  = r.fault.empty() && str_of(r.regs[39]).find("already taken") != std::string::npos;
        std::cout << std::format("  actor answers on the moved connection: {}{}\n", moved_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  old descriptor refused:                {}  (\"{}\")\n", stale_ok ? "PASS" : "FAIL",
                                 str_of(r.regs[35]));
        std::cout << std::format("  ... also after its slot was reused:    {}{}\n", stale2_ok && reused ? "PASS" : "FAIL",
                                 reused ? "" : "  (the slot was not reused -- the test no longer proves it)");
        std::cout << std::format("  a second take refused:                 {}\n", twice_ok ? "PASS" : "FAIL");
        check(moved_ok && stale_ok && stale2_ok && reused && twice_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// Active sockets: the world's I/O thread reads an activated connection and delivers LINES into an
// inbox of capacity 2 -- three are ready at once, so the thread must pause and be resumed by the
// receives. Bytes recvLine had buffered come first; a line split across two sends is joined; "\r\n"
// ends a line too; the end of the stream arrives as its own event. Writing goes through the id; after
// close it is refused, and so is the old descriptor. A second connection is still active when the
// program ends: the world has to stop the I/O thread (the test would hang otherwise).
inline void test_active_socket() {
    using namespace forkjoin;
    std::cout << "=== active_socket ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        as.load_const(20, 0);          call1(as, 21, NATIVE_TCP_LISTEN, 20);        // listener
        call1(as, 22, NATIVE_TCP_LOCAL_PORT, 21);
        as.load_str(23, "127.0.0.1");  call2(as, 24, NATIVE_TCP_CONNECT, 23, 22);   // client
        call1(as, 25, NATIVE_TCP_ACCEPT, 21);                                       // server side
        as.load_const(20, 2);          call1(as, 26, NATIVE_NEW_INBOX, 20);         // capacity 2
        as.load_str(27, "ab\r\ncd\nef"); as.BYTES_FROM_STR(28, 27);
        call2(as, 29, NATIVE_TCP_SEND, 24, 28);
        as.load_str(30, "pre\n");      as.BYTES_FROM_STR(31, 30);                   // "already buffered"
        auto activate = [&](uint8_t rd, uint8_t rsock, int64_t mode, uint8_t rpending) {
            as.R6(OpCode::MOV, 43, rsock, 0);
            as.R6(OpCode::MOV, 44, 26, 0);
            as.load_const(45, mode);
            as.load_const(46, 100);
            as.R6(OpCode::MOV, 47, rpending, 0);
            as.call_native_id(rd, 42, 43, 5, NATIVE_ACTIVATE);
        };
        activate(32, 25, 1, 31);
        auto next = [&](uint8_t rd) {                   // at most 5 s: a lost event fails, never hangs
            as.load_const(20, 5000);
            call2(as, 19, NATIVE_RECEIVE, 26, 20);
            call1(as, rd, NATIVE_MAIL_MSG, 26);
        };
        next(1); next(2); next(3);
        as.load_str(27, "hi");         as.BYTES_FROM_STR(28, 27);
        call2(as, 33, NATIVE_ACTIVE_SEND, 32, 28);                                  // server -> client
        as.load_const(20, 16);         call2(as, 34, NATIVE_TCP_RECV, 24, 20);
        as.load_str(27, "gh\n");       as.BYTES_FROM_STR(28, 27);
        call2(as, 29, NATIVE_TCP_SEND, 24, 28);
        call1(as, 29, NATIVE_TCP_CLOSE, 24);
        next(4); next(5);
        call1(as, 35, NATIVE_ACTIVE_CLOSE, 32);
        call2(as, 36, NATIVE_ACTIVE_SEND, 32, 28);                                  // after close
        call2(as, 37, NATIVE_TCP_SEND, 25, 28);                                     // the old descriptor
        call2(as, 38, NATIVE_TCP_CONNECT, 23, 22);
        call1(as, 39, NATIVE_TCP_ACCEPT, 21);
        activate(18, 39, 0, 28);                                                    // left active
        Heap heap;
        const Run r = run(as, heap);
        const bool lines_ok  = r.fault.empty() && bytes_of(r.regs[1]) == "\x01pre" &&
                               bytes_of(r.regs[2]) == "\x01" "ab" && bytes_of(r.regs[3]) == "\x01" "cd" &&
                               bytes_of(r.regs[4]) == "\x01" "efgh";
        const bool closed_ok = r.fault.empty() && bytes_of(r.regs[5]) == "\x02";
        const bool send_ok   = r.fault.empty() && r.regs[33].isNil() && bytes_of(r.regs[34]) == "hi";
        const bool after_ok  = r.fault.empty() && r.regs[35].isNil() &&
                               str_of(r.regs[36]).find("closed") != std::string::npos;
        const bool stale_ok  = r.fault.empty() && str_of(r.regs[37]).find("activated") != std::string::npos;
        const bool second_ok = r.fault.empty() && r.regs[18].isInt();
        std::cout << std::format("  lines in order, paused and resumed:    {}{}\n", lines_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  end of stream is an event:             {}\n", closed_ok ? "PASS" : "FAIL");
        std::cout << std::format("  writing through the id:                {}\n", send_ok ? "PASS" : "FAIL");
        std::cout << std::format("  refused after close:                   {}  (\"{}\")\n", after_ok ? "PASS" : "FAIL",
                                 str_of(r.regs[36]));
        std::cout << std::format("  the old descriptor refused:            {}  (\"{}\")\n", stale_ok ? "PASS" : "FAIL",
                                 str_of(r.regs[37]));
        std::cout << std::format("  the world ends with one still active:  {}\n", second_ok ? "PASS" : "FAIL");
        check(lines_ok && closed_ok && send_ok && after_ok && stale_ok && second_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// An activated LISTENER: the I/O thread accepts, parks each connection in the world's hand-off table and
// delivers its ticket. Three clients connect while the inbox holds ONE event, so the thread must pause
// and be resumed; nothing else in this program makes tickets, so they are 1, 2, 3 in order. A taken
// connection must be BLOCKING (the listener was switched to non-blocking, and Windows and BSD pass that
// on): a 200 ms receive with nothing sent must time out, not fail at once. The old descriptor and a send
// to the listener's id are refused; after close a new client is refused; a second active listener is
// still open when the program ends.
inline void test_active_listener() {
    using namespace forkjoin;
    std::cout << "=== active_listener ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        as.load_const(20, 0);          call1(as, 21, NATIVE_TCP_LISTEN, 20);
        call1(as, 22, NATIVE_TCP_LOCAL_PORT, 21);
        as.load_str(23, "127.0.0.1");
        as.load_const(20, 1);          call1(as, 26, NATIVE_NEW_INBOX, 20);         // capacity 1
        call2(as, 30, NATIVE_ACTIVATE_LISTENER, 21, 26);
        call2(as, 24, NATIVE_TCP_CONNECT, 23, 22);
        call2(as, 25, NATIVE_TCP_CONNECT, 23, 22);
        call2(as, 27, NATIVE_TCP_CONNECT, 23, 22);
        auto next = [&](uint8_t rd) {                   // at most 5 s: a lost event fails, never hangs
            as.load_const(20, 5000);
            call2(as, 19, NATIVE_RECEIVE, 26, 20);
            call1(as, rd, NATIVE_MAIL_MSG, 26);
        };
        next(1); next(2); next(3);
        as.load_const(20, 1);          call1(as, 31, NATIVE_TAKE, 20);              // the first client
        as.load_const(20, 200);        call2(as, 32, NATIVE_TCP_SET_TIMEOUT, 31, 20);
        as.load_const(20, 16);         call2(as, 33, NATIVE_TCP_RECV, 31, 20);      // blocking: a timeout
        as.load_str(34, "hi");         as.BYTES_FROM_STR(35, 34);
        call2(as, 36, NATIVE_TCP_SEND, 24, 35);
        as.load_const(20, 16);         call2(as, 37, NATIVE_TCP_RECV, 31, 20);      // "hi"
        call1(as, 38, NATIVE_TCP_ACCEPT, 21);                                       // the old descriptor
        call2(as, 39, NATIVE_ACTIVE_SEND, 30, 35);                                  // not a connection
        call1(as, 4, NATIVE_ACTIVE_CLOSE, 30);
        as.load_const(20, 300);        call1(as, 5, NATIVE_SLEEP, 20);              // the thread closes it
        call2(as, 6, NATIVE_TCP_CONNECT, 23, 22);                                   // refused now
        as.load_const(20, 0);          call1(as, 7, NATIVE_TCP_LISTEN, 20);
        call2(as, 8, NATIVE_ACTIVATE_LISTENER, 7, 26);                              // left active
        Heap heap;
        const Run r = run(as, heap);
        const bool tickets_ok = r.fault.empty() && bytes_of(r.regs[1]) == "\x04" "1" &&
                                bytes_of(r.regs[2]) == "\x04" "2" && bytes_of(r.regs[3]) == "\x04" "3";
        const bool block_ok   = r.fault.empty() && r.regs[32].isNil() &&
                                str_of(r.regs[33]).find("timeout") != std::string::npos &&
                                bytes_of(r.regs[37]) == "hi";
        const bool stale_ok   = r.fault.empty() && str_of(r.regs[38]).find("activated") != std::string::npos;
        const bool nosend_ok  = r.fault.empty() && str_of(r.regs[39]).find("not a connection") != std::string::npos;
        const bool closed_ok  = r.fault.empty() && r.regs[4].isNil() &&
                                str_of(r.regs[6]).find("could not connect") != std::string::npos;
        const bool second_ok  = r.fault.empty() && r.regs[8].isInt();
        std::cout << std::format("  tickets 1, 2, 3 through an inbox of 1:  {}{}\n", tickets_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  the taken connection blocks:           {}  (\"{}\")\n", block_ok ? "PASS" : "FAIL",
                                 str_of(r.regs[33]));
        std::cout << std::format("  the old descriptor refused:            {}  (\"{}\")\n", stale_ok ? "PASS" : "FAIL",
                                 str_of(r.regs[38]));
        std::cout << std::format("  a send to the listener refused:        {}  (\"{}\")\n", nosend_ok ? "PASS" : "FAIL",
                                 str_of(r.regs[39]));
        std::cout << std::format("  closed: a new client is refused:       {}  (\"{}\")\n", closed_ok ? "PASS" : "FAIL",
                                 str_of(r.regs[6]));
        std::cout << std::format("  the world ends with one still active:  {}\n", second_ok ? "PASS" : "FAIL");
        check(tickets_ok && block_ok && stale_ok && nosend_ok && closed_ok && second_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A send deadline on an active connection whose peer never reads. The root sends 8 KB at a time until a
// send fails: once the socket buffers are full a send waits, and after 200 ms without progress it gives
// up and closes the connection. So the run takes at least 200 ms (and far less than the 5 s a lost
// deadline would need to reach the loop's cap), the next send finds the connection closed, and the peer,
// reading what did get through, reaches the end of the stream. Negative times and a listener's id are
// located faults.
inline void test_active_send_deadline() {
    using namespace forkjoin;
    std::cout << "=== active_send_deadline ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        as.load_const(20, 0);          call1(as, 21, NATIVE_TCP_LISTEN, 20);
        call1(as, 22, NATIVE_TCP_LOCAL_PORT, 21);
        as.load_str(23, "127.0.0.1");  call2(as, 24, NATIVE_TCP_CONNECT, 23, 22);   // the peer: never reads
        call1(as, 25, NATIVE_TCP_ACCEPT, 21);
        as.load_const(20, 4);          call1(as, 26, NATIVE_NEW_INBOX, 20);
        as.load_str(27, "");           as.BYTES_FROM_STR(31, 27);
        as.R6(OpCode::MOV, 43, 25, 0);
        as.R6(OpCode::MOV, 44, 26, 0);
        as.load_const(45, 0);
        as.load_const(46, 0);
        as.R6(OpCode::MOV, 47, 31, 0);
        as.call_native_id(32, 42, 43, 5, NATIVE_ACTIVATE);                          // raw chunks
        as.load_const(20, 200);        call2(as, 33, NATIVE_ACTIVE_SET_SEND_TIMEOUT, 32, 20);
        as.load_str(27, std::string(8192, 'x'));  as.BYTES_FROM_STR(28, 27);
        as.load_const(29, 0);          as.load_const(30, 5000);                     // at most 40 MB
        as.label("asd_send");
        call2(as, 34, NATIVE_ACTIVE_SEND, 32, 28);
        as.R6(OpCode::IS_NIL, 35, 34, 0);
        as.C2(OpCode::INCR, 29);
        as.B1(OpCode::BF, 35, "asd_failed");
        as.B(OpCode::BLT_INT, 29, 30, "asd_send");
        as.label("asd_failed");
        call2(as, 36, NATIVE_ACTIVE_SEND, 32, 28);                                  // closed now
        as.load_const(20, 2000);       call2(as, 19, NATIVE_TCP_SET_TIMEOUT, 24, 20);
        as.load_const(18, 0);          as.load_const(17, 100000);
        as.label("asd_drain");                                                      // the peer reads it all
        as.load_const(20, 65536);      call2(as, 37, NATIVE_TCP_RECV, 24, 20);
        as.R6(OpCode::LEN, 38, 37, 0);
        as.C2(OpCode::INCR, 18);
        as.load_const(16, 0);
        as.B(OpCode::BEQ_INT, 38, 16, "asd_drained");
        as.B(OpCode::BLT_INT, 18, 17, "asd_drain");
        as.label("asd_drained");
        Heap heap;
        const auto t0 = std::chrono::steady_clock::now();
        const Run r = run(as, heap);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const bool set_ok    = r.fault.empty() && r.regs[33].isNil();
        const bool gave_up   = r.fault.empty() && str_of(r.regs[34]).find("has not read for 200 ms") != std::string::npos &&
                               r.regs[29].isInt() && r.regs[29].asSigned48() < 5000 && ms >= 190.0 && ms < 5000.0;
        const bool closed_ok = r.fault.empty() && str_of(r.regs[36]).find("closed") != std::string::npos;
        const bool eof_ok    = r.fault.empty() && r.regs[37].isPtr() &&
                               GcObject::from_slots(r.regs[37].asPtr())->kind == GcObject::KIND_BYTES &&
                               bytes_of(r.regs[37]).empty();
        std::cout << std::format("  the deadline is set:                    {}{}\n", set_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  a send gives up after 200 ms ({:.0f} ms): {}  (\"{}\")\n", ms, gave_up ? "PASS" : "FAIL",
                                 str_of(r.regs[34]));
        std::cout << std::format("  the next send finds it closed:          {}  (\"{}\")\n", closed_ok ? "PASS" : "FAIL",
                                 str_of(r.regs[36]));
        std::cout << std::format("  the peer reaches the end of the stream: {}\n", eof_ok ? "PASS" : "FAIL");

        auto expect = [](const char* label, const Run& fr, const char* want) {
            const bool ok = fr.fault.find(want) != std::string::npos;
            std::cout << std::format("  {:<39} {}{}\n", label, ok ? "PASS" : "FAIL",
                                     ok ? "" : "  (got: \"" + fr.fault + "\")");
            return ok;
        };
        bool misuse_ok = true;
        {   // a negative time
            Assembler fa; declare_task_fns(fa); fa.label("main");
            fa.load_const(20, 1);  fa.load_const(21, -1);
            call2(fa, 22, NATIVE_ACTIVE_SET_SEND_TIMEOUT, 20, 21);
            Heap h;
            misuse_ok &= expect("a negative time", run(fa, h), "0 milliseconds or more");
        }
        {   // a listener's id
            Assembler fa; declare_task_fns(fa); fa.label("main");
            fa.load_const(20, 0);  call1(fa, 21, NATIVE_TCP_LISTEN, 20);
            fa.load_const(20, 1);  call1(fa, 26, NATIVE_NEW_INBOX, 20);
            call2(fa, 30, NATIVE_ACTIVATE_LISTENER, 21, 26);
            fa.load_const(20, 100);
            call2(fa, 22, NATIVE_ACTIVE_SET_SEND_TIMEOUT, 30, 20);
            Heap h;
            misuse_ok &= expect("a listener's id", run(fa, h), "not a connection");
        }
        check(set_ok && gave_up && closed_ok && eof_ok && misuse_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// Misuse faults with a located message.
inline void test_actor_misuse() {
    using namespace forkjoin;
    std::cout << "=== actor_misuse (located faults) ===\n";
    auto expect = [](const char* label, const Run& r, const char* want) {
        const bool ok = r.fault.find(want) != std::string::npos;
        std::cout << std::format("  {:<36} {}{}\n", label, ok ? "PASS" : "FAIL",
                                 ok ? "" : "  (got: \"" + r.fault + "\")");
        return ok;
    };
    try {
        bool ok = true;
        {   // the root receives without having taken its inbox
            Assembler as; declare_task_fns(as); as.label("main");
            receive_main(as, 13, 0);
            Heap heap;
            ok &= expect("receive before mainInbox", run(as, heap), "no inbox");
        }
        {   // the main inbox twice
            Assembler as; declare_task_fns(as); as.label("main");
            main_inbox(as, 10); main_inbox(as, 11);
            Heap heap;
            ok &= expect("mainInbox twice", run(as, heap), "already taken");
        }
        {   // reading a message when the last receive timed out
            Assembler as; declare_task_fns(as); as.label("main");
            main_inbox(as, 10); receive_main(as, 13, 0); mail_read(as, 14, NATIVE_MAIL_MSG);
            Heap heap;
            ok &= expect("reading a message that is not there", run(as, heap), "no message to read");
        }
        {   // receiving on an actor's inbox from the root
            Assembler as; declare_task_fns(as); as.label("main");
            main_inbox(as, 10);
            as.load_const(41, 0);  spawn_actor(as, 11, "aecho");
            as.R6(OpCode::MOV, 40, 11, 0); as.load_const(41, 0);
            as.call_native_id(13, 42, 40, 2, NATIVE_RECEIVE);
            Heap heap;
            ok &= expect("receiving on another actor's inbox", run(as, heap), "belongs to another actor");
        }
        {   // a negative capacity
            Assembler as; declare_task_fns(as); as.label("main");
            as.load_const(20, -1); call1(as, 21, NATIVE_NEW_INBOX, 20);
            Heap heap;
            ok &= expect("an inbox of capacity -1", run(as, heap), "capacity must be 0");
        }
        {   // receiving on an inbox after closing it
            Assembler as; declare_task_fns(as); as.label("main");
            as.load_const(20, 0); call1(as, 21, NATIVE_NEW_INBOX, 20);
            call1(as, 22, NATIVE_CLOSE_INBOX, 21);
            as.load_const(20, 0); call2(as, 23, NATIVE_RECEIVE, 21, 20);
            Heap heap;
            ok &= expect("receiving on a closed inbox", run(as, heap), "inbox is closed");
        }
        {   // the main inbox ends with its owner
            Assembler as; declare_task_fns(as); as.label("main");
            main_inbox(as, 10); as.load_const(20, 0); call1(as, 21, NATIVE_CLOSE_INBOX, 20);
            Heap heap;
            ok &= expect("closing the main inbox", run(as, heap), "main inbox cannot be closed");
        }
        {   // a send to one's own full inbox would wait forever
            Assembler as; declare_task_fns(as); as.label("main");
            as.load_const(20, 1); call1(as, 21, NATIVE_NEW_INBOX, 20);
            call2(as, 22, NATIVE_SEND, 21, 20);
            call2(as, 23, NATIVE_SEND, 21, 20);
            Heap heap;
            ok &= expect("a send to one's own full inbox", run(as, heap), "own inbox is full");
        }
        {   // a bounded actor needs room for at least one message
            Assembler as; declare_task_fns(as); as.label("main");
            as.load_const(44, 0); spawn_actor_bounded(as, 11, "astop", 0);
            Heap heap;
            ok &= expect("a bounded actor of capacity 0", run(as, heap), "at least 1");
        }
        {   // a task has no mail, so it cannot make an inbox: its join reports the fault
            Assembler as; declare_task_fns(as); as.label("main");
            as.load_const(41, 0); spawn(as, 10, "tinbox"); join(as, 10, 20, 30);
            Heap heap;
            const Run r = run(as, heap);
            const bool t_ok = r.fault.empty() && r.regs[20].isBool() && !r.regs[20].asBool() &&
                              str_of(r.regs[30]).find("only an actor or the main program") != std::string::npos;
            std::cout << std::format("  {:<36} {}{}\n", "an inbox made in a task", t_ok ? "PASS" : "FAIL",
                                     t_ok ? "" : "  (got: \"" + (r.fault.empty() ? str_of(r.regs[30]) : r.fault) + "\")");
            ok &= t_ok;
        }
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A second inbox: a request carries its address, and the reply arrives there -- not in the main inbox.
// After closing it, a send answers false and trySend "gone".
inline void test_actor_extra_inbox_reply() {
    using namespace forkjoin;
    std::cout << "=== actor_extra_inbox_reply ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        as.load_const(20, 0);  call1(as, 21, NATIVE_NEW_INBOX, 20);            // r21 = the reply inbox
        as.load_const(41, 0);  spawn_actor(as, 11, "areply");
        call2(as, 12, NATIVE_SEND, 11, 21);                                     // "answer at r21"
        as.load_const(22, -1); call2(as, 13, NATIVE_RECEIVE, 21, 22);
        call1(as, 14, NATIVE_MAIL_MSG, 21);
        receive_main(as, 15, 0);                                                // the main inbox: empty
        call1(as, 16, NATIVE_CLOSE_INBOX, 21);
        call2(as, 17, NATIVE_SEND, 21, 20);
        call2(as, 18, NATIVE_TRY_SEND, 21, 20);
        Heap heap;
        const Run r = run(as, heap);
        auto is_int = [&](int reg, int64_t v) { return r.regs[reg].isInt() && r.regs[reg].asSigned48() == v; };
        const bool id_ok    = r.fault.empty() && r.regs[21].isInt() && r.regs[11].isInt() &&
                              r.regs[21].asSigned48() > 0 && r.regs[21].asSigned48() != r.regs[11].asSigned48();
        const bool reply_ok = r.fault.empty() && is_int(13, 1) && is_int(14, 7) && is_int(15, 0);
        const bool close_ok = r.fault.empty() && r.regs[17].isBool() && !r.regs[17].asBool() && is_int(18, 2);
        std::cout << std::format("  a new inbox has its own id:          {}{}\n", id_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  the reply arrives there, not in main: {}\n", reply_ok ? "PASS" : "FAIL");
        std::cout << std::format("  closed: send false, trySend gone:    {}\n", close_ok ? "PASS" : "FAIL");
        check(id_ok && reply_ok && close_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// Back-pressure. A bounded actor (capacity 3) that is not yet reading: trySend fills it and then
// answers "full". After "go" (through the actor's own extra inbox), 36 more blocking sends follow
// while it reads; it checks that all 40 arrive in order. Then a bounded actor that has crashed
// answers trySend with "gone".
inline void test_actor_bounded_backpressure() {
    using namespace forkjoin;
    constexpr int N = 40;
    std::cout << "=== actor_bounded_backpressure ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        as.load_const(44, N);  spawn_actor_bounded(as, 11, "agate", 3);
        receive_main(as, 13, 10000);  mail_read(as, 14, NATIVE_MAIL_MSG);      // r14 = its gate
        for (int i = 1; i <= 4; ++i) {                                          // r21..r24
            as.load_const(20, i);
            call2(as, static_cast<uint8_t>(20 + i), NATIVE_TRY_SEND, 11, 20);
        }
        as.load_const(20, 0);  call2(as, 25, NATIVE_SEND, 14, 20);              // go
        as.R6(OpCode::MOV, 26, 25, 0);                                          // the go send: true
        for (int i = 4; i <= N; ++i) {                                          // r26 = AND of the sends
            as.load_const(20, i);
            call2(as, 27, NATIVE_SEND, 11, 20);
            as.R6(OpCode::AND_BOOL, 26, 26, 27);
        }
        receive_main(as, 28, 10000);  mail_read(as, 29, NATIVE_MAIL_MSG);      // the sum
        as.load_const(44, 0);  spawn_actor_bounded(as, 30, "acrash", 1);
        receive_main(as, 31, 10000);                                            // its exit report
        as.load_const(20, 1);  call2(as, 32, NATIVE_TRY_SEND, 30, 20);
        Heap heap;
        const Run r = run(as, heap);
        auto is_int = [&](int reg, int64_t v) { return r.regs[reg].isInt() && r.regs[reg].asSigned48() == v; };
        const bool full_ok = r.fault.empty() && is_int(21, 0) && is_int(22, 0) && is_int(23, 0) && is_int(24, 1);
        const bool flow_ok = r.fault.empty() && r.regs[26].isBool() && r.regs[26].asBool() &&
                             is_int(28, 1) && is_int(29, int64_t{N} * (N + 1) / 2);
        const bool gone_ok = r.fault.empty() && is_int(31, 2) && is_int(32, 2);
        std::cout << std::format("  capacity 3: three sent, the fourth full: {}{}\n", full_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  {} messages through, in order (sum {}): {}\n", N, int64_t{N} * (N + 1) / 2,
                                 flow_ok ? "PASS" : "FAIL");
        std::cout << std::format("  trySend to a crashed actor -> gone:     {}\n", gone_ok ? "PASS" : "FAIL");
        check(full_ok && flow_ok && gone_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// The end of the program with back-pressure in flight. Actor A (capacity 1) is full and waits on its
// extra GATE inbox; actor B is blocked sending to A. The world's end must (1) wake A on the gate,
// (2) get Stop into A's FULL main inbox (A drains it and prints "A stopped"; without Stop it would
// print "A no stop" after two seconds), and (3) let B's send go -- false, or true if A's drain made
// room first; either way B prints "B done" and the program ends.
inline void test_actor_bounded_shutdown() {
    using namespace forkjoin;
    std::cout << "=== actor_bounded_shutdown ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        as.load_const(44, 0);  spawn_actor_bounded(as, 11, "agate", 1);
        receive_main(as, 13, 10000);                                            // A is at its gate
        as.load_const(20, 1);  call2(as, 21, NATIVE_TRY_SEND, 11, 20);          // sent
        call2(as, 22, NATIVE_TRY_SEND, 11, 20);                                 // full
        as.R6(OpCode::MOV, 41, 11, 0);  spawn_actor(as, 12, "aflood");
        receive_main(as, 23, 10000);                                            // B's hello
        receive_main(as, 24, 100);                                              // give B time to block
        Heap heap;
        const auto t0 = std::chrono::steady_clock::now();
        const Run r = run(as, heap);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        auto is_int = [&](int reg, int64_t v) { return r.regs[reg].isInt() && r.regs[reg].asSigned48() == v; };
        const bool setup_ok = r.fault.empty() && is_int(21, 0) && is_int(22, 1) && is_int(23, 1);
        const bool a_ok     = r.printed.find("A stopped") != std::string::npos;
        const bool b_ok     = r.printed.find("B done") != std::string::npos;
        std::cout << std::format("  A full, B blocked on it:            {}{}\n", setup_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  Stop reached A through a full inbox: {}\n", a_ok ? "PASS" : "FAIL");
        std::cout << std::format("  B's send was let go, the end came:   {}  ({:.0f} ms, \"{}\")\n", b_ok ? "PASS" : "FAIL",
                                 ms, r.printed);
        check(setup_ok && a_ok && b_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A SLOT is an address that outlives its actor. The crashed actor's report names the SLOT (so a
// supervisor's comparison survives a restart), a send while the slot is empty waits in it, and the
// actor started into it next reads that mail and answers at the same address. Releasing the slot
// ends the address.
inline void test_actor_slot_restart() {
    using namespace forkjoin;
    std::cout << "=== actor_slot_restart ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        new_slot(as, 11, 0);
        as.load_const(41, 0);  spawn_into(as, 12, 11, "acrash");
        receive_main(as, 13, -1);                       // the crash report ...
        mail_read(as, 14, NATIVE_MAIL_FROM);            // ... names the SLOT, not the isolate
        send_int(as, 15, 11, 21);                       // nobody is in the slot: this waits in it
        spawn_into(as, 16, 11, "aslot");
        receive_main(as, 17, -1);  mail_read(as, 18, NATIVE_MAIL_MSG);
        send_int(as, 19, 11, 50);                       // the SAME address, a new actor
        receive_main(as, 20, -1);  mail_read(as, 21, NATIVE_MAIL_MSG);
        call1(as, 22, NATIVE_RELEASE_SLOT, 11);
        send_int(as, 23, 11, 1);
        Heap heap;
        const Run r = run(as, heap);
        auto is_int = [&](int reg, int64_t v) { return r.regs[reg].isInt() && r.regs[reg].asSigned48() == v; };
        const bool report_ok = r.fault.empty() && is_int(13, 2) && r.regs[14].isInt() && r.regs[11].isInt() &&
                               r.regs[14].asSigned48() == r.regs[11].asSigned48();
        const bool queued_ok = r.fault.empty() && r.regs[15].isBool() && r.regs[15].asBool() &&
                               is_int(17, 1) && is_int(18, 42);
        const bool same_ok   = r.fault.empty() && is_int(20, 1) && is_int(21, 100);
        const bool gone_ok   = r.fault.empty() && r.regs[23].isBool() && !r.regs[23].asBool();
        std::cout << std::format("  the exit report names the slot:   {}{}\n", report_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  a send while empty waits in it:   {}\n", queued_ok ? "PASS" : "FAIL");
        std::cout << std::format("  the address survives the restart: {}\n", same_ok ? "PASS" : "FAIL");
        std::cout << std::format("  released: a later send is false:  {}\n", gone_ok ? "PASS" : "FAIL");
        check(report_ok && queued_ok && same_ok && gone_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// What an actor did not read before it crashed is DROPPED: delivering the message that crashed it to
// its successor would crash that one too. Only what arrives afterwards waits for the new actor.
inline void test_actor_slot_drops_pending() {
    using namespace forkjoin;
    std::cout << "=== actor_slot_drops_pending ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        new_slot(as, 11, 0);
        send_int(as, 12, 11, 7);                        // waits in the slot ...
        as.load_const(41, 0);  spawn_into(as, 13, 11, "acrash");   // ... and this one never reads it
        receive_main(as, 14, -1);                       // the crash report
        send_int(as, 15, 11, 9);
        spawn_into(as, 16, 11, "aslot");
        receive_main(as, 17, -1);  mail_read(as, 18, NATIVE_MAIL_MSG);
        receive_main(as, 19, 50);                       // nothing else is coming
        Heap heap;
        const Run r = run(as, heap);
        auto is_int = [&](int reg, int64_t v) { return r.regs[reg].isInt() && r.regs[reg].asSigned48() == v; };
        const bool ok = r.fault.empty() && is_int(14, 2) && is_int(17, 1) && is_int(18, 18) && is_int(19, 0);
        std::cout << std::format("  the unread message is dropped:  {}{}\n", ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// rawStopActor tells an actor to end, wherever it waits. An address nobody answers at is false.
inline void test_actor_stop_actor() {
    using namespace forkjoin;
    std::cout << "=== actor_stop_actor ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        as.load_const(41, 0);  spawn_actor(as, 11, "aecho");
        send_int(as, 12, 11, 21);
        receive_main(as, 13, -1);  mail_read(as, 14, NATIVE_MAIL_MSG);
        call1(as, 15, NATIVE_STOP_ACTOR, 11);
        as.load_const(20, 987654);  call1(as, 16, NATIVE_STOP_ACTOR, 20);
        Heap heap;
        const Run r = run(as, heap);
        const bool alive_ok = r.fault.empty() && r.regs[14].isInt() && r.regs[14].asSigned48() == 42;
        const bool stop_ok  = r.fault.empty() && r.regs[15].isBool() && r.regs[15].asBool() &&
                              r.regs[16].isBool() && !r.regs[16].asBool();
        std::cout << std::format("  21 -> 42, then stopped:      {}{}\n", alive_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  a stranger's address: false: {}\n", stop_ok ? "PASS" : "FAIL");
        check(alive_ok && stop_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A MONITOR is told when an actor ends, in an inbox of the watcher's own: a crash with its message, and
// a normal end with "normal" -- which the starter's own report does not cover. It does not replace that
// report; both arrive.
inline void test_actor_monitor() {
    using namespace forkjoin;
    std::cout << "=== actor_monitor ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        as.load_const(43, 0);  as.call_native_id(11, 42, 43, 1, NATIVE_NEW_INBOX);   // r11 = the watch inbox
        as.load_const(41, 0);  spawn_actor(as, 12, "acrashon");
        call2(as, 13, NATIVE_MONITOR, 12, 11);                                        // still running: true
        send_int(as, 14, 12, 1);                                                      // now it faults
        as.load_const(43, -1); call2(as, 15, NATIVE_RECEIVE, 11, 43);                 // the monitor's report
        call1(as, 16, NATIVE_MAIL_FROM, 11);
        call1(as, 17, NATIVE_MAIL_REASON, 11);
        receive_main(as, 18, 10000);                                                  // the starter's own
        mail_read(as, 19, NATIVE_MAIL_FROM);
        // A normal end: stopActor on a monitored actor reports "normal" -- and only to the monitor.
        as.load_const(41, 0);  spawn_actor(as, 20, "aecho");
        call2(as, 21, NATIVE_MONITOR, 20, 11);
        call1(as, 22, NATIVE_STOP_ACTOR, 20);
        as.load_const(43, -1); call2(as, 23, NATIVE_RECEIVE, 11, 43);
        call1(as, 24, NATIVE_MAIL_FROM, 11);
        call1(as, 25, NATIVE_MAIL_REASON, 11);
        receive_main(as, 26, 50);                                                     // nothing for the starter
        Heap heap;
        const Run r = run(as, heap);
        auto is_int = [&](int reg, int64_t v) { return r.regs[reg].isInt() && r.regs[reg].asSigned48() == v; };
        auto same   = [&](int a, int b) { return r.regs[a].isInt() && r.regs[b].isInt() &&
                                                 r.regs[a].asSigned48() == r.regs[b].asSigned48(); };
        const bool crash_ok  = r.fault.empty() && r.regs[13].isBool() && r.regs[13].asBool() &&
                               is_int(15, 2) && same(16, 12) &&
                               str_of(r.regs[17]).find("boom on demand") != std::string::npos;
        const bool both_ok   = r.fault.empty() && is_int(18, 2) && same(19, 12);
        const bool normal_ok = r.fault.empty() && r.regs[21].isBool() && r.regs[21].asBool() &&
                               is_int(23, 2) && same(24, 20) && str_of(r.regs[25]) == "normal";
        const bool quiet_ok  = r.fault.empty() && is_int(26, 0);
        std::cout << std::format("  a crash reaches the monitor:      {}{}\n", crash_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  ... and the starter as well:      {}\n", both_ok ? "PASS" : "FAIL");
        std::cout << std::format("  a normal end reports \"normal\":    {}  (\"{}\")\n", normal_ok ? "PASS" : "FAIL",
                                 str_of(r.regs[25]));
        std::cout << std::format("  ... and is NOT sent to the starter: {}\n", quiet_ok ? "PASS" : "FAIL");
        check(crash_ok && both_ok && normal_ok && quiet_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// An actor that did NOT start another one can watch it -- which the starter's report cannot do -- and a
// monitor on an actor that has already ended reports at once, so a monitor always ends in one report.
inline void test_actor_monitor_others() {
    using namespace forkjoin;
    std::cout << "=== actor_monitor_others ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        as.load_const(41, 0);  spawn_actor(as, 11, "aecho");        // the actor being watched
        as.R6(OpCode::MOV, 41, 11, 0);  spawn_actor(as, 12, "awatch");
        call1(as, 13, NATIVE_STOP_ACTOR, 11);
        receive_main(as, 14, 10000);  mail_read(as, 15, NATIVE_MAIL_MSG);   // the watcher's word
        // An actor that has already ended: false, and the report comes at once.
        as.load_const(43, 0);  as.call_native_id(16, 42, 43, 1, NATIVE_NEW_INBOX);
        as.load_const(41, 0);  spawn_actor(as, 17, "acrash");
        receive_main(as, 18, 10000);                                        // its crash, so it is over
        call2(as, 19, NATIVE_MONITOR, 17, 16);
        as.load_const(43, 0); call2(as, 20, NATIVE_RECEIVE, 16, 43);
        call1(as, 21, NATIVE_MAIL_REASON, 16);
        Heap heap;
        const Run r = run(as, heap);
        auto is_int = [&](int reg, int64_t v) { return r.regs[reg].isInt() && r.regs[reg].asSigned48() == v; };
        const bool watcher_ok = r.fault.empty() && is_int(14, 1) && r.regs[15].isInt() && r.regs[11].isInt() &&
                                r.regs[15].asSigned48() == r.regs[11].asSigned48();
        const bool gone_ok    = r.fault.empty() && r.regs[19].isBool() && !r.regs[19].asBool() &&
                                is_int(20, 2) && str_of(r.regs[21]) == "gone";
        std::cout << std::format("  a non-starter is told of the end: {}{}\n", watcher_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  already ended: false, report now: {}  (\"{}\")\n", gone_ok ? "PASS" : "FAIL",
                                 str_of(r.regs[21]));
        check(watcher_ok && gone_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// An actor whose loop is its OWN work never reaches a receive, so mail cannot end it. rawStopRequested
// reads the flag every ending path sets, so such an actor can end itself -- and it consumes no mail.
inline void test_actor_stop_requested() {
    using namespace forkjoin;
    std::cout << "=== actor_stop_requested ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        call1(as, 11, NATIVE_STOP_REQUESTED, 10);       // the root's own inbox: nobody has asked
        as.load_const(41, 0);  spawn_actor(as, 12, "abusy");
        call1(as, 13, NATIVE_STOP_ACTOR, 12);
        receive_main(as, 14, -1);  mail_read(as, 15, NATIVE_MAIL_MSG);
        new_slot(as, 16, 0);                            // releasing a slot tells the actor in it too
        spawn_into(as, 17, 16, "abusy");
        call1(as, 18, NATIVE_RELEASE_SLOT, 16);
        receive_main(as, 19, -1);  mail_read(as, 20, NATIVE_MAIL_MSG);
        Heap heap;
        const Run r = run(as, heap);
        auto is_int = [&](int reg, int64_t v) { return r.regs[reg].isInt() && r.regs[reg].asSigned48() == v; };
        const bool quiet_ok = r.fault.empty() && r.regs[11].isBool() && !r.regs[11].asBool();
        const bool stop_ok  = r.fault.empty() && r.regs[13].isBool() && r.regs[13].asBool() &&
                              is_int(14, 1) && is_int(15, 77);
        const bool slot_ok  = r.fault.empty() && is_int(19, 1) && is_int(20, 77);
        std::cout << std::format("  nothing asked: false:            {}{}\n", quiet_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  a busy actor ends on the flag:   {}\n", stop_ok ? "PASS" : "FAIL");
        std::cout << std::format("  a released slot sets it as well: {}\n", slot_ok ? "PASS" : "FAIL");
        {   // ... and it may only be asked about one's OWN inbox
            Assembler bad;
            declare_task_fns(bad);
            bad.label("main");
            main_inbox(bad, 10);
            bad.load_const(41, 0);  spawn_actor(bad, 11, "aecho");
            call1(bad, 12, NATIVE_STOP_REQUESTED, 11);
            Heap bad_heap;
            const Run br = run(bad, bad_heap);
            const bool foreign_ok = br.fault.find("belongs to another actor") != std::string::npos;
            std::cout << std::format("  someone else's inbox: a fault:   {}  (\"{}\")\n",
                                     foreign_ok ? "PASS" : "FAIL", br.fault);
            check(quiet_ok && stop_ok && slot_ok && foreign_ok);
        }
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A receive waits on ONE inbox. rawSelect waits on SEVERAL and answers which of them has something,
// so an actor can serve its own mail and a reply inbox from one loop. It takes nothing out: the
// receive that follows reads the inbox the index names, and cannot wait.
inline void test_actor_select() {
    using namespace forkjoin;
    std::cout << "=== actor_select ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);                                    // the root's own inbox: address 0
        as.load_const(43, 0);  as.call_native_id(11, 42, 43, 1, NATIVE_NEW_INBOX);
        as.VEC_NEW(12);                                        // [ main, extra ] -- main wins a tie
        as.load_const(13, 0);
        as.VEC_PUSH(12, 13);
        as.VEC_PUSH(12, 11);
        auto select = [&](uint8_t rd, uint8_t rlist, int64_t ms) {
            as.R6(OpCode::MOV, 40, rlist, 0);
            as.load_const(41, ms);
            as.call_native_id(rd, 42, 40, 2, NATIVE_SELECT);
        };
        select(14, 12, 50);                                    // nothing yet: the deadline, so -1
        as.load_const(41, 0);  spawn_actor(as, 15, "aecho");
        send_int(as, 16, 15, 21);                              // it answers 42 to address 0
        select(17, 12, 10000);                                 // the MAIN inbox: index 0
        receive_main(as, 18, 0);                               // ... and this cannot wait
        mail_read(as, 19, NATIVE_MAIL_MSG);
        send_int(as, 20, 11, 7);                               // into our SECOND inbox
        select(21, 12, 10000);                                 // index 1
        as.R6(OpCode::MOV, 40, 11, 0);                         // and reading it is an ordinary receive
        as.load_const(41, 0);
        as.call_native_id(22, 42, 40, 2, NATIVE_RECEIVE);
        as.R6(OpCode::MOV, 40, 11, 0);
        as.call_native_id(23, 42, 40, 1, NATIVE_MAIL_MSG);
        Heap heap;
        const Run r = run(as, heap);
        auto is_int = [&](int reg, int64_t v) { return r.regs[reg].isInt() && r.regs[reg].asSigned48() == v; };
        const bool timeout_ok = r.fault.empty() && is_int(14, -1);
        const bool first_ok   = r.fault.empty() && is_int(17, 0) && is_int(18, 1) && is_int(19, 42);
        const bool second_ok  = r.fault.empty() && is_int(21, 1) && is_int(22, 1) && is_int(23, 7);
        std::cout << std::format("  nothing to read: the deadline:  {}{}\n", timeout_ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        std::cout << std::format("  mail in the first inbox:        {}\n", first_ok ? "PASS" : "FAIL");
        std::cout << std::format("  mail in the second inbox:       {}\n", second_ok ? "PASS" : "FAIL");
        bool rules_ok = true;
        {   // someone else's inbox in the list: refused exactly as a receive on it would be
            Assembler bad;
            declare_task_fns(bad);
            bad.label("main");
            main_inbox(bad, 10);
            bad.load_const(41, 0);  spawn_actor(bad, 11, "aecho");
            bad.VEC_NEW(12);
            bad.VEC_PUSH(12, 11);
            bad.R6(OpCode::MOV, 40, 12, 0);
            bad.load_const(41, 0);
            bad.call_native_id(13, 42, 40, 2, NATIVE_SELECT);
            Heap bad_heap;
            const Run br = run(bad, bad_heap);
            const bool foreign_ok = br.fault.find("belongs to another actor") != std::string::npos;
            std::cout << std::format("  someone else's inbox: a fault:  {}  (\"{}\")\n",
                                     foreign_ok ? "PASS" : "FAIL", br.fault);
            rules_ok = rules_ok && foreign_ok;
        }
        {   // watching nothing with no deadline: no event could ever end it, so it is a fault
            Assembler bad;
            declare_task_fns(bad);
            bad.label("main");
            main_inbox(bad, 10);
            bad.VEC_NEW(11);
            bad.R6(OpCode::MOV, 40, 11, 0);
            bad.load_const(41, -1);
            bad.call_native_id(12, 42, 40, 2, NATIVE_SELECT);
            Heap bad_heap;
            const Run br = run(bad, bad_heap);
            const bool empty_ok = br.fault.find("would wait forever") != std::string::npos;
            std::cout << std::format("  no inbox, no deadline: a fault: {}  (\"{}\")\n",
                                     empty_ok ? "PASS" : "FAIL", br.fault);
            rules_ok = rules_ok && empty_ok;
        }
        check(timeout_ok && first_ok && second_ok && rules_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A select is not a receive, so the ways an actor is ENDED must reach it too: Stop goes into every
// inbox an actor owns, and each of them wakes the pad the select sleeps on.
inline void test_actor_select_stop() {
    using namespace forkjoin;
    std::cout << "=== actor_select_stop ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        as.load_const(41, 0);  spawn_actor(as, 11, "aselect");   // parks in a select, forever
        call1(as, 12, NATIVE_STOP_ACTOR, 11);
        receive_main(as, 13, 10000);                              // it reports what woke it
        mail_read(as, 14, NATIVE_MAIL_MSG);
        Heap heap;
        const Run r = run(as, heap);
        auto is_int = [&](int reg, int64_t v) { return r.regs[reg].isInt() && r.regs[reg].asSigned48() == v; };
        const bool ok = r.fault.empty() && r.regs[12].isBool() && r.regs[12].asBool() &&
                        is_int(13, 1) && is_int(14, 3);          // MAIL_STOP, seen through the select
        std::cout << std::format("  Stop wakes a parked select:     {}{}\n", ok ? "PASS" : "FAIL",
                                 r.fault.empty() ? "" : "  (fault: " + r.fault + ")");
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// A send to one's OWN full inbox is already a located fault: only the waiting isolate could empty
// it. A CYCLE of such waits over several isolates is the same fact, and gets the same answer -- no
// in-band message can break it, since Stop and exit reports ignore the capacity and never touch the
// `space` condition variable. Here the main program and one actor each fill the other's inbox.
inline void test_actor_send_cycle() {
    using namespace forkjoin;
    std::cout << "=== actor_send_cycle ===\n";
    try {
        Assembler as;
        declare_task_fns(as);
        as.label("main");
        main_inbox(as, 10);
        as.load_const(43, 1);  as.call_native_id(11, 42, 43, 1, NATIVE_NEW_INBOX);  // bounded to one
        as.R6(OpCode::MOV, 44, 11, 0);                        // the actor sends back to it ...
        spawn_actor_bounded(as, 12, "afill", 1);              // ... and its own inbox holds one
        as.load_const(20, 7);
        as.label("cycle_loop");                               // ... while we fill the actor's
        as.R6(OpCode::MOV, 40, 12, 0);
        as.R6(OpCode::MOV, 41, 20, 0);
        as.call_native_id(21, 42, 40, 2, NATIVE_SEND);
        as.J(OpCode::J, "cycle_loop");
        Heap heap;
        const Run r = run(as, heap);
        const bool named  = r.fault.find("deadlock") != std::string::npos;
        const bool why    = r.fault.find("waiting for room in inbox") != std::string::npos;
        std::cout << std::format("  a send cycle is a fault:    {}  (\"{}\")\n",
                                 named ? "PASS" : "FAIL", r.fault);
        std::cout << std::format("  and it names the cycle:     {}\n", why ? "PASS" : "FAIL");
        check(named && why);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// Misusing a slot is a located fault in the caller, never a silently shared address.
inline void test_actor_slot_misuse() {
    using namespace forkjoin;
    std::cout << "=== actor_slot_misuse ===\n";
    try {
        auto expect = [](const char* label, const Run& r, const char* want) {
            const bool ok = r.fault.find(want) != std::string::npos;
            std::cout << std::format("  {:<28} {}  (\"{}\")\n", label, ok ? "PASS" : "FAIL", r.fault);
            return ok;
        };
        bool all = true;
        {   // two actors in one slot
            Assembler as;
            declare_task_fns(as);
            as.label("main");
            main_inbox(as, 10);
            new_slot(as, 11, 0);
            spawn_into(as, 12, 11, "aslot");
            spawn_into(as, 13, 11, "aslot");
            Heap heap;
            all &= expect("occupied slot:", run(as, heap), "already has an actor");
        }
        {   // a released slot is gone
            Assembler as;
            declare_task_fns(as);
            as.label("main");
            main_inbox(as, 10);
            new_slot(as, 11, 0);
            call1(as, 12, NATIVE_RELEASE_SLOT, 11);
            spawn_into(as, 13, 11, "aslot");
            Heap heap;
            all &= expect("released slot:", run(as, heap), "not a slot");
        }
        {   // an ordinary actor's address is not a slot
            Assembler as;
            declare_task_fns(as);
            as.label("main");
            main_inbox(as, 10);
            as.load_const(41, 0);  spawn_actor(as, 11, "aecho");
            spawn_into(as, 12, 11, "aslot");
            Heap heap;
            all &= expect("an actor's own address:", run(as, heap), "not a slot");
        }
        check(all);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// concurrent_fault_probe -- the two-thread version of test_fault_probe, and the ONE
// test that actually decides whether the POSIX rendezvous is per-thread.
//
// On Windows this passes without any of this branch's changes: the fault frame there
// is a plain SEH __try around the dispatch loop, so it is per-thread by construction.
// That is exactly why it is worth having here -- it establishes the baseline and pins
// it. On POSIX the rendezvous is a lookup keyed by thread id, and before that existed
// only the FIRST thread inside execute() could arm at all: every other instance lost
// fault classification, turning a guard-page hit into a raw process kill. This probe
// is what catches a regression back to that.
//
// It lives behind --fault-probe with its own sibling flag rather than in the ordinary
// suite, for the same reason test_fault_probe does: its failure mode is a process kill,
// and a crash here must not take the other hundred-odd tests down with it.
// =============================================================================
inline void test_concurrent_fault_probe() {
    constexpr unsigned NT = 4;
    std::cout << std::format("=== concurrent_fault_probe ({} threads faulting at once) ===\n", NT);
    std::vector<char>        ok(NT, 0);        // distinct elements: no shared writes
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < NT; ++i)
        workers.emplace_back([&ok, i] { ok[i] = fault_probe_once(0, "Stack Overflow") ? 1 : 0; });
    for (auto& w : workers) w.join();

    bool all_ok = true;
    for (unsigned i = 0; i < NT; ++i) {
        std::cout << std::format("  thread {}: {}\n", i, ok[i] ? "classified" : "NOT classified");
        all_ok = all_ok && ok[i] != 0;
    }
    check(all_ok);
}

// =============================================================================
// test_mov_take -- MOV_TAKE copies like MOV but EMPTIES the source. Used for the
// return-value fetch after a call, where a leftover copy would be a heap pointer
// in a slot no collection forwards again (see the opcode table).
// =============================================================================
inline void test_mov_take() {
    Assembler as;
    as.label("main");
    as.C2(OpCode::LOAD_CONST, 0, 42);
    as.C2(OpCode::LOAD_CONST, 1, 7);
    as.MOV_TAKE(2, 0);                       // r2 = r0, r0 emptied
    as.MOV_TAKE(3, 3);                       // degenerate rd == ra: must end up empty
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== mov_take ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto  res  = execute(bytecode);
        auto* regs = res.get_reg_base();
        const bool moved       = regs[2].isInt() && regs[2].asSigned48() == 42;
        const bool src_cleared = regs[0].isUndefined();
        const bool others_kept = regs[1].isInt() && regs[1].asSigned48() == 7;
        const bool self_take   = regs[3].isUndefined();   // copy then clear -> empty
        std::cout << std::format("  value moved to dest:     {}\n", moved       ? "PASS" : "FAIL");
        std::cout << std::format("  source left empty:       {}\n", src_cleared ? "PASS" : "FAIL");
        std::cout << std::format("  other registers intact:  {}\n", others_kept ? "PASS" : "FAIL");
        std::cout << std::format("  self-take (rd == ra):    {}\n", self_take   ? "PASS" : "FAIL");
        check(moved && src_cleared && others_kept && self_take);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_shifts -- SHL_INT, SHR_INT (arithmetic/signed), USHR_INT (logical).
// Covers: basic left shift; arithmetic right shift of a negative; the SHR/USHR
// divergence on a value with bit 47 set (arithmetic keeps the sign, logical does
// not); and a count >= 48 (defined: left shift out of the 48-bit range -> 0).
// =============================================================================
inline void test_shifts() {
    constexpr int64_t POS_2_46 = (int64_t(1) << 46);
    Assembler as;
    as.label("main");
    as.C2(OpCode::LOAD_CONST, 0, 1);
    as.C2(OpCode::LOAD_CONST, 1, 4);
    as.R6(OpCode::SHL_INT, 2, 0, 1);      // 1 << 4 = 16
    as.C2(OpCode::LOAD_CONST, 3, -16);
    as.C2(OpCode::LOAD_CONST, 4, 2);
    as.R6(OpCode::SHR_INT, 5, 3, 4);      // -16 >> 2 = -4 (arithmetic)
    // r9 = MIN_48 (bit 47 set) via 1 << 47
    as.C2(OpCode::LOAD_CONST, 6, 1);
    as.C2(OpCode::LOAD_CONST, 7, 47);
    as.R6(OpCode::SHL_INT, 9, 6, 7);      // r9 = MIN_48
    as.C2(OpCode::LOAD_CONST, 8, 1);
    as.R6(OpCode::SHR_INT,  11, 9, 8);    // MIN_48 >> 1  (arith) = -(2^46)
    as.R6(OpCode::USHR_INT, 12, 9, 8);    // MIN_48 >>> 1 (logical) = 2^46
    // count >= 48: 1 << 48 shifts out of the 48-bit range -> 0
    as.C2(OpCode::LOAD_CONST, 13, 48);
    as.R6(OpCode::SHL_INT, 15, 0, 13);    // 1 << 48 = 0 (defined)
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== shifts (shl/shr/ushr) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode);
        auto* regs = res.get_reg_base();
        const int64_t r2 = regs[2].asSigned48(), r5 = regs[5].asSigned48();
        const int64_t r11 = regs[11].asSigned48(), r12 = regs[12].asSigned48();
        const int64_t r15 = regs[15].asSigned48();
        std::cout << std::format("1<<4={} -16>>2={} MIN_48>>1={} MIN_48>>>1={} 1<<48={}\n",
                                 r2, r5, r11, r12, r15);
        const bool ok = r2 == 16 && r5 == -4 &&
                        r11 == -POS_2_46 && r12 == POS_2_46 && r15 == 0;
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_frame_gc_roots -- regression test for the frame_size GC-roots contract.
//
// Drives the real execute() / op_call / Heap::mark_roots path and forces a GC
// mid-program (via native_collect) at a point where two heap objects are live
// only through registers:
//
//   X -- held only in a TOP-LEVEL register (r0), live across a CALL.
//        Requires the top-level frame to be scanned as a GC root: consequence
//        (c) of the window_size contract. execute() must receive a non-zero
//        top_frame_size (here COLLECTOR_FRAME: X at r0 lies within [0, that), and
//        because the CALL now slides by the caller frame size the callee's window
//        opens at r[COLLECTOR_FRAME], leaving r0 untouched).
//
//   Z -- held only in a register of the CALLED function's frame, within its
//        declared frame_size. Requires the callee frame's declared frame_size
//        to cover it: consequence (b).
//
//   D -- a dead canary object, reachable from nothing.
//
// The collector function returns the number of bytes freed and stores it in a
// global. If both X and Z are scanned as roots, exactly D is reclaimed, so
// freed == D.total_bytes(). With the old behaviour (top-level frame size 0),
// X would also be collected and freed would be larger -- the test would fail.
// =============================================================================
// =============================================================================
// test_top_frame_validation -- the top-level region is now a validated frame when
// the codegen declares its size via Assembler::set_top_frame_size() (closes the
// window_size contract's consequence (c)). validate_frames() checks [0, first
// function) against the declared size with the same GC-roots rule as any function.
// Opt-in: bytecode that never declares it stays unchecked (backward compatible).
// =============================================================================
inline void test_top_frame_validation() {
    std::cout << "=== top_frame_validation ===\n";
    bool all_ok = true;

    // (1) Negative: top-level touches r2 as a pure local but declares frame size 2
    //     (legal indices [0,2)) -> under-declared -> assemble() must throw, naming <script>.
    {
        Assembler as;
        as.set_top_frame_size(2);
        as.label("main");
        as.R6(OpCode::MOV, 2, 0, 0);         // r2 -- a local above the declared frame
        as.J(OpCode::HALT);
        bool threw = false;
        try { (void)as.assemble(); }
        catch (const std::exception& e) {
            threw = std::string(e.what()).find("<script>") != std::string::npos;
        }
        std::cout << "  under-declared top frame throws: " << (threw ? "PASS" : "FAIL") << "\n";
        all_ok = all_ok && threw;
    }

    // (2) Positive: same body with frame size 3 -> r2 is in-frame -> assembles.
    {
        Assembler as;
        as.set_top_frame_size(3);
        as.label("main");
        as.R6(OpCode::MOV, 2, 0, 0);
        as.J(OpCode::HALT);
        bool ok = false;
        try { (void)as.assemble(); ok = true; } catch (const std::exception&) {}
        std::cout << "  correct top frame assembles: " << (ok ? "PASS" : "FAIL") << "\n";
        all_ok = all_ok && ok;
    }

    // (3) Overlap positive: frame size 2, but r2 is a legit outgoing CALL argument (the
    //     window opens at r[frame_size]) -> the same call-overlap widening applies as for
    //     a function region, so it must assemble.
    {
        Assembler as;
        as.func("f", 2);
        as.set_top_frame_size(2);
        as.label("main");
        as.R6(OpCode::MOV, 2, 0, 0);         // arg0 into the outgoing window at r[2]
        as.CALL("f");
        as.J(OpCode::HALT);
        as.label("f");
        as.J(OpCode::RET);
        bool ok = false;
        try { (void)as.assemble(); ok = true; } catch (const std::exception&) {}
        std::cout << "  call-overlap arg in top frame assembles: " << (ok ? "PASS" : "FAIL") << "\n";
        all_ok = all_ok && ok;
    }

    // (4) Opt-out: no set_top_frame_size() -> the top-level region stays unchecked even with
    //     a high register (backward compatibility with hand-assembled bytecode).
    {
        Assembler as;
        as.label("main");
        as.R6(OpCode::MOV, 5, 0, 0);         // high register, but no top frame declared
        as.J(OpCode::HALT);
        bool ok = false;
        try { (void)as.assemble(); ok = true; } catch (const std::exception&) {}
        std::cout << "  opt-out (no declaration) stays unchecked: " << (ok ? "PASS" : "FAIL") << "\n";
        all_ok = all_ok && ok;
    }

    check(all_ok);
}

inline void test_frame_gc_roots() {
    std::cout << "=== frame_gc_roots ===\n";

    constexpr uint8_t COLLECTOR_FRAME = 3; // collector uses r0(Z), r1(result), r2(fn ptr)

    Heap heap(64 * 1024);
    GlobalEnv globals(heap);
    const uint8_t g_freed = globals.define();

    // Dead canary: allocated but never rooted -- must be the ONLY thing collected.
    GcObject* dead = heap.alloc(GcObject::KIND_ARRAY, 2);
    assert(dead && "test_frame_gc_roots: dead canary alloc failed");
    const size_t dead_total = dead->total_bytes();

    Assembler as;
    as.func("collector", COLLECTOR_FRAME);

    as.label("main");
    as.ALLOC(0, 2);              // r0 = X  (top-level heap pointer, live across CALL)
    as.CALL("collector");
    as.J(OpCode::HALT);

    as.label("collector");
    as.ALLOC(0, 1);              // r0 = Z  (callee-frame heap pointer, live across GC)
    as.call_native_id(1, 2, 2, 0, 0);           // r1 = native_collect() (id 0) -> runs GC
    as.STORE_GLOBAL(g_freed, 1); // globals[g_freed] = freed byte count
    as.J(OpCode::RET);

    const auto bytecode = as.assemble();
    // Top-level frame size = COLLECTOR_FRAME: the slide (now the caller frame size)
    // opens collector's window at r[COLLECTOR_FRAME], so X at r0 survives and is
    // scanned, and Z (collector's r0) lands at r[COLLECTOR_FRAME] as before.
    const uint8_t top = COLLECTOR_FRAME;

    std::cout << "=== frame_gc_roots (top_frame_size = " << static_cast<int>(top) << ") ===\n";
    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main");
    dis.print();

    std::cout << "Running...\n";
    try {
        auto res   = execute_gc(bytecode, &heap, &globals, top);
        auto* regs = res.get_reg_base();

        // X lives in the top-level frame (r0); Z lives in the collector frame,
        // whose window starts at reg index COLLECTOR_FRAME.
        const Value x_val = regs[0];
        const Value z_val = regs[COLLECTOR_FRAME];
        const Value freed = globals.get(g_freed);

        GcObject* x_obj = x_val.isPtr() ? GcObject::from_slots(x_val.asPtr()) : nullptr;
        GcObject* z_obj = z_val.isPtr() ? GcObject::from_slots(z_val.asPtr()) : nullptr;

        const bool x_ok = x_obj &&
                          x_obj->kind == GcObject::KIND_ARRAY &&
                          x_obj->slot_count() == 2;
        const bool z_ok = z_obj &&
                          z_obj->kind == GcObject::KIND_ARRAY &&
                          z_obj->slot_count() == 1;
        const bool freed_ok = freed.isInt() &&
                              static_cast<size_t>(freed.asSigned48()) == dead_total;

        const bool ok = x_ok && z_ok && freed_ok;

        std::cout << std::format("freed={} (expect dead_total={})\n",
            freed.isInt() ? freed.asSigned48() : -1, dead_total);
        std::cout << std::format("  top-level register root survived (c): {}\n",
            x_ok ? "PASS" : "FAIL");
        std::cout << std::format("  callee-frame register root survived (b): {}\n",
            z_ok ? "PASS" : "FAIL");
        std::cout << std::format("  only the dead canary was collected:      {}\n",
            freed_ok ? "PASS" : "FAIL");
        check(ok);

        globals.set(g_freed, Value{});
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_load_str -- string literals via LOAD_STR. execute() pre-interns the
// assembler's literal texts into a GC-rooted string pool; LOAD_STR indexes it.
// Checks: r0 is a KIND_STRING with content "hi"; two identical literals dedup to
// one heap object (interning); EQ compares interned strings by pointer identity.
// =============================================================================
// =============================================================================
// test_const_array -- LOAD_CONST_ARRAY (id 117) + the const-array pool.
//
// A `const NAME: Array[T] = [...]` is a heap KIND_ARRAY that can't ride the
// pointer-free const pool, so execute() pre-builds each into a GC-ROOTED pool
// (like the string-literal pool) and LOAD_CONST_ARRAY indexes it. This checks:
// (a) the three scalar element kinds (Int/Double/Bool) round-trip through the
// pool, (b) LEN + ARRAY_GET work on a pooled const array, and (c) the SKBC CARR
// chunk serializes/deserializes the pool bit-exactly.
// =============================================================================
inline void test_const_array() {
    std::cout << "=== const arrays (LOAD_CONST_ARRAY) ===\n";

    Heap heap(64 * 1024);

    Assembler as;
    // Three const arrays exercising every permitted scalar element kind.
    const uint16_t ints = as.add_const_array(
        { Value::fromSigned48(10), Value::fromSigned48(20), Value::fromSigned48(30) });
    const uint16_t dbls = as.add_const_array(
        { Value::fromDouble(1.5), Value::fromDouble(2.5) });
    const uint16_t bools = as.add_const_array(
        { Value::fromBool(true), Value::fromBool(false) });

    as.label("main");
    as.load_const_array(0, ints);   // r0 = [10,20,30]
    as.LEN(1, 0);                   // r1 = 3
    as.load_const(2, 1);            // r2 = index 1
    as.ARRAY_GET(3, 0, 2);          // r3 = ints[1] = 20
    as.load_const_array(4, dbls);   // r4 = [1.5,2.5]
    as.load_const(5, 0);            // r5 = index 0
    as.ARRAY_GET(6, 4, 5);          // r6 = dbls[0] = 1.5
    as.load_const_array(7, bools);  // r7 = [true,false]
    as.load_const(8, 1);            // r8 = index 1
    as.ARRAY_GET(9, 7, 8);          // r9 = bools[1] = false
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        // r0/r4/r7 hold const-array pointers -> top_frame_size = 10 scans r0..r9 as roots.
        // const_arrays is the LAST execute() param, so every intermediate is left null/default.
        auto res = execute(bytecode, &heap, nullptr, nullptr, 10,
                           nullptr, nullptr, nullptr, nullptr, nullptr,   // const_pool..fn_table
                           nullptr, nullptr, 0, 0,                         // out, trait_table, width, methods
                           nullptr, nullptr, nullptr, nullptr,             // line, fn_names, column, native_table
                           nullptr, nullptr, nullptr,                      // script_args, in, function_modules
                           &as.const_arrays());
        auto* regs = res.get_reg_base();

        const bool len_ok  = regs[1].isInt()    && regs[1].asSigned48() == 3;
        const bool int_ok  = regs[3].isInt()    && regs[3].asSigned48() == 20;
        const bool dbl_ok  = regs[6].isDouble() && regs[6].asDouble()   == 1.5;
        const bool bool_ok = regs[9].isBool()   && regs[9].asBool()     == false;
        // r0 is a real heap KIND_ARRAY (the pooled instance).
        const bool kind_ok = regs[0].isPtr() &&
            GcObject::from_slots(regs[0].asPtr())->kind == GcObject::KIND_ARRAY;

        std::cout << std::format("  len == 3:            {}\n", len_ok  ? "PASS" : "FAIL");
        std::cout << std::format("  ints[1] == 20:       {}\n", int_ok  ? "PASS" : "FAIL");
        std::cout << std::format("  dbls[0] == 1.5:      {}\n", dbl_ok  ? "PASS" : "FAIL");
        std::cout << std::format("  bools[1] == false:   {}\n", bool_ok ? "PASS" : "FAIL");
        std::cout << std::format("  r0 is KIND_ARRAY:    {}\n", kind_ok ? "PASS" : "FAIL");
        check(len_ok && int_ok && dbl_ok && bool_ok && kind_ok);
    } catch (const std::exception& e) {
        record_fail(e.what());
    }

    // ---- SKBC CARR chunk: bit-exact serialize -> deserialize round-trip. ----
    try {
        bcio::ModuleImage img;
        img.bytecode     = bytecode;
        img.const_arrays = as.const_arrays();
        const auto bytes = bcio::serialize(img);
        const auto back  = bcio::deserialize(bytes);

        bool rt_ok = back.const_arrays.size() == img.const_arrays.size();
        for (size_t i = 0; rt_ok && i < img.const_arrays.size(); ++i) {
            rt_ok = back.const_arrays[i].size() == img.const_arrays[i].size();
            for (size_t j = 0; rt_ok && j < img.const_arrays[i].size(); ++j)
                rt_ok = std::memcmp(&back.const_arrays[i][j],
                                    &img.const_arrays[i][j], sizeof(Value)) == 0;
        }
        std::cout << std::format("  SKBC CARR round-trip: {}\n", rt_ok ? "PASS" : "FAIL");
        check(rt_ok);
    } catch (const std::exception& e) {
        record_fail(e.what());
    }
}

inline void test_load_str() {
    std::cout << "=== load_str (string literals) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.label("main");
    as.load_str(0, "hi");         // r0 = "hi"
    as.load_str(1, "hi");         // r1 = "hi" (same interned object)
    as.load_str(2, "bye");        // r2 = "bye"
    as.R6(OpCode::EQ, 3, 0, 1);   // r3 = (r0 == r1) -> true  (pointer identity)
    as.R6(OpCode::EQ, 4, 0, 2);   // r4 = (r0 == r2) -> false
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        // The top-level frame holds three string pointers, so it must be scanned as
        // a GC root: pass top_frame_size = 3 (harmless here -- no collection runs).
        auto res   = execute(bytecode, &heap, nullptr, &interner, 3, nullptr, nullptr,
                             &as.string_literals());
        auto* regs = res.get_reg_base();

        const bool is_str =
            regs[0].isPtr() &&
            GcObject::from_slots(regs[0].asPtr())->kind == GcObject::KIND_STRING;

        const GcObject* o = is_str ? GcObject::from_slots(regs[0].asPtr()) : nullptr;
        const bool content_ok =
            o && std::string_view(o->bytes(), o->string_length()) == "hi";

        // Deduped: two identical literals share one heap object.
        const bool identity_ok = regs[0].isPtr() && regs[1].isPtr() &&
                                 regs[0].asPtr() == regs[1].asPtr();

        const bool eq_ok =
            regs[3].isBool() && regs[3].asBool() == true &&
            regs[4].isBool() && regs[4].asBool() == false;

        const bool ok = is_str && content_ok && identity_ok && eq_ok;

        std::cout << std::format("  KIND_STRING + content \"hi\": {}\n", (is_str && content_ok) ? "PASS" : "FAIL");
        std::cout << std::format("  interned identity (r0==r1): {}\n", identity_ok ? "PASS" : "FAIL");
        std::cout << std::format("  EQ true/false:              {}\n", eq_ok ? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_string_concat -- ADD on two heap strings concatenates into a FRESH
// (non-interned) KIND_STRING. Drives a 40-deep concat chain under a tiny heap so
// alloc_string_gc's collect(+grow) retry path runs while the accumulator survives
// as a register root; asserts the final content and that the result pointer differs
// from the operand literals (a genuinely new object, not an interned share).
// =============================================================================
inline void test_string_concat() {
    std::cout << "=== string concat (ADD) ===\n";

    Heap heap(1024);                    // tiny: force collect/grow during the chain
    StringInterner interner;

    constexpr int N = 40;
    Assembler as;
    as.label("main");
    as.load_str(0, "ab");               // r0 = accumulator
    as.load_str(1, "cd");               // r1 = the piece to append (interned, rooted)
    for (int k = 0; k < N; ++k)
        as.R6(OpCode::ADD, 0, 0, 1);    // r0 = r0 + "cd"  (fresh string each time)
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::string expected = "ab";
    for (int k = 0; k < N; ++k) expected += "cd";

    std::cout << "Running...\n";
    try {
        // r0 (accumulator) + r1 held live across each concat's possible collection.
        auto res   = execute(bytecode, &heap, nullptr, &interner, 2, nullptr, nullptr,
                             &as.string_literals());
        auto* regs = res.get_reg_base();

        const bool is_str =
            regs[0].isPtr() &&
            GcObject::from_slots(regs[0].asPtr())->kind == GcObject::KIND_STRING;
        const GcObject* o = is_str ? GcObject::from_slots(regs[0].asPtr()) : nullptr;
        const bool content_ok =
            o && std::string_view(o->bytes(), o->string_length()) == expected;
        // The concat result is a new object, distinct from the "cd" literal (r1).
        const bool distinct = regs[0].isPtr() && regs[1].isPtr() &&
                              regs[0].asPtr() != regs[1].asPtr();

        const bool ok = is_str && content_ok && distinct;
        std::cout << std::format("  concatenated content ({} bytes): {}\n",
                                 expected.size(), (is_str && content_ok) ? "PASS" : "FAIL");
        std::cout << std::format("  result is a fresh (non-shared) object: {}\n",
                                 distinct ? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_string_number_concat -- number+string / string+number coercion in ADD. A
// numeric (Int or Double) operand is formatted to text (format_number) and
// concatenated into a fresh KIND_STRING. Covers both operand orders, a negative
// int, and the Double ".0" rule (2.0 -> "2.0", not "2").
// =============================================================================
inline void test_string_number_concat() {
    std::cout << "=== number+string concat (ADD coercion) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.label("main");
    as.load_str(0, "!");                 // r0 = "!"
    as.load_str(1, "n=");                // r1 = "n="
    as.load_const(2, 42);                // r2 = 42
    as.load_const(3, 7);                 // r3 = 7
    as.load_const(4, -3);                // r4 = -3
    as.load_double(5, 3.5);              // r5 = 3.5
    as.load_double(6, 2.0);              // r6 = 2.0  (integer-valued double)
    as.R6(OpCode::ADD, 10, 2, 0);        // r10 = 42 + "!"   -> "42!"
    as.R6(OpCode::ADD, 11, 1, 3);        // r11 = "n=" + 7   -> "n=7"
    as.R6(OpCode::ADD, 12, 4, 0);        // r12 = -3 + "!"   -> "-3!"
    as.R6(OpCode::ADD, 13, 5, 0);        // r13 = 3.5 + "!"  -> "3.5!"
    as.R6(OpCode::ADD, 14, 6, 0);        // r14 = 2.0 + "!"  -> "2.0!"
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, &interner, 16,
                             &as.constant_pool(), nullptr, &as.string_literals());
        auto* regs = res.get_reg_base();

        auto S = [&](int i, std::string_view want) {
            if (!regs[i].isPtr()) return false;
            const GcObject* o = GcObject::from_slots(regs[i].asPtr());
            return o->kind == GcObject::KIND_STRING &&
                   std::string_view(o->bytes(), o->string_length()) == want;
        };

        const bool ok =
            S(10, "42!") && S(11, "n=7") && S(12, "-3!") &&
            S(13, "3.5!") && S(14, "2.0!");

        std::cout << std::format("  int+string  \"42!\" : {}\n",  S(10, "42!")  ? "PASS" : "FAIL");
        std::cout << std::format("  string+int  \"n=7\" : {}\n",  S(11, "n=7")  ? "PASS" : "FAIL");
        std::cout << std::format("  negative    \"-3!\" : {}\n",  S(12, "-3!")  ? "PASS" : "FAIL");
        std::cout << std::format("  double     \"3.5!\" : {}\n",  S(13, "3.5!") ? "PASS" : "FAIL");
        std::cout << std::format("  double .0  \"2.0!\" : {}\n",  S(14, "2.0!") ? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_to_string -- the generic TO_STRING coercion. Atoms resolve to their name
// WITH a leading ':' (":ok") from the pre-interned atom pool; bool/nil to the
// constant "true"/"false"/"nil"; a string to itself (identity); Int/Double via
// format_number (incl. the Double ".0" rule). Exercises the atom_names table
// plumbed through execute() into the GC-rooted atom pool.
// =============================================================================
inline void test_to_string() {
    std::cout << "=== TO_STRING (generic to-string coercion) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.label("main");
    as.load_atom(0, "ok");                       // r0 = :ok
    as.load_atom(1, "error");                    // r1 = :error
    as.load_str(2, "hello");                     // r2 = "hello"
    as.load_const(3, 42);                        // r3 = 42
    as.load_const(4, -7);                        // r4 = -7
    as.load_double(5, 3.5);                      // r5 = 3.5
    as.load_double(6, 2.0);                      // r6 = 2.0 (integer-valued double)
    as.load_constant(7, Value::fromBool(true));  // r7 = true
    as.load_constant(8, Value::fromBool(false)); // r8 = false
    as.load_constant(9, Value::fromNil());       // r9 = nil
    as.R6(OpCode::TO_STRING, 10, 0, 0);          // r10 = toString(:ok)    -> ":ok"
    as.R6(OpCode::TO_STRING, 11, 1, 0);          // r11 = toString(:error) -> ":error"
    as.R6(OpCode::TO_STRING, 12, 2, 0);          // r12 = toString("hello")-> "hello"
    as.R6(OpCode::TO_STRING, 13, 3, 0);          // r13 = toString(42)     -> "42"
    as.R6(OpCode::TO_STRING, 14, 4, 0);          // r14 = toString(-7)     -> "-7"
    as.R6(OpCode::TO_STRING, 15, 5, 0);          // r15 = toString(3.5)    -> "3.5"
    as.R6(OpCode::TO_STRING, 16, 6, 0);          // r16 = toString(2.0)    -> "2.0"
    as.R6(OpCode::TO_STRING, 17, 7, 0);          // r17 = toString(true)   -> "true"
    as.R6(OpCode::TO_STRING, 18, 8, 0);          // r18 = toString(false)  -> "false"
    as.R6(OpCode::TO_STRING, 19, 9, 0);          // r19 = toString(nil)    -> "nil"
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, &interner, 20,
                             &as.constant_pool(), nullptr, &as.string_literals(),
                             &as.atom_names());
        auto* regs = res.get_reg_base();

        auto S = [&](int i, std::string_view want) {
            if (!regs[i].isPtr()) return false;
            const GcObject* o = GcObject::from_slots(regs[i].asPtr());
            return o->kind == GcObject::KIND_STRING &&
                   std::string_view(o->bytes(), o->string_length()) == want;
        };

        const bool ok =
            S(10, ":ok") && S(11, ":error") && S(12, "hello") &&
            S(13, "42")  && S(14, "-7")     && S(15, "3.5")   &&
            S(16, "2.0") && S(17, "true")   && S(18, "false") && S(19, "nil");

        std::cout << std::format("  atom   \":ok\"    : {}\n", S(10, ":ok")    ? "PASS" : "FAIL");
        std::cout << std::format("  atom   \":error\" : {}\n", S(11, ":error") ? "PASS" : "FAIL");
        std::cout << std::format("  string \"hello\"  : {}\n", S(12, "hello")  ? "PASS" : "FAIL");
        std::cout << std::format("  int    \"42\"     : {}\n", S(13, "42")     ? "PASS" : "FAIL");
        std::cout << std::format("  int    \"-7\"     : {}\n", S(14, "-7")     ? "PASS" : "FAIL");
        std::cout << std::format("  double \"3.5\"    : {}\n", S(15, "3.5")    ? "PASS" : "FAIL");
        std::cout << std::format("  dbl .0 \"2.0\"    : {}\n", S(16, "2.0")    ? "PASS" : "FAIL");
        std::cout << std::format("  bool   \"true\"   : {}\n", S(17, "true")   ? "PASS" : "FAIL");
        std::cout << std::format("  bool   \"false\"  : {}\n", S(18, "false")  ? "PASS" : "FAIL");
        std::cout << std::format("  nil    \"nil\"    : {}\n", S(19, "nil")    ? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_to_string_struct -- the struct branch of TO_STRING (the default field dump).
// Covers: a flat struct (Point { x: 1, y: 2 }), a nested struct (Line of two Points),
// a QUOTED string field + an atom field, an omitted field rendered as `undefined`,
// and the depth cap on a self-referential (cyclic) struct -- which must terminate
// with `... }` instead of overflowing the native stack.
// =============================================================================
inline void test_to_string_struct() {
    std::cout << "=== TO_STRING (struct default field dump) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.define_struct("Point", { "x", "y" });   // id 0
    as.define_struct("Line",  { "a", "b" });   // id 1 (a, b are Points)
    as.define_struct("Named", { "s", "tag" }); // id 2 (string + atom)
    as.define_struct("Node",  { "next" });     // id 3 (self-referential)

    const uint16_t px = as.field("Point", "x"), py = as.field("Point", "y");
    const uint16_t la = as.field("Line",  "a"), lb = as.field("Line",  "b");
    const uint16_t ns = as.field("Named", "s"), nt = as.field("Named", "tag");
    const uint16_t nn = as.field("Node",  "next");

    as.label("main");
    // r0 = Point { x: 1, y: 2 } ; r1 = its dump
    as.NEW_STRUCT(0, as.struct_id("Point"));
    as.load_const(10, 1); as.SET_PROP(0, px, 10);
    as.load_const(11, 2); as.SET_PROP(0, py, 11);
    as.R6(OpCode::TO_STRING, 1, 0, 0);

    // r4 = Line { a: Point{3,4}, b: Point{5,6} } ; r5 = its (nested) dump
    as.NEW_STRUCT(2, as.struct_id("Point"));
    as.load_const(10, 3); as.SET_PROP(2, px, 10);
    as.load_const(11, 4); as.SET_PROP(2, py, 11);
    as.NEW_STRUCT(3, as.struct_id("Point"));
    as.load_const(10, 5); as.SET_PROP(3, px, 10);
    as.load_const(11, 6); as.SET_PROP(3, py, 11);
    as.NEW_STRUCT(4, as.struct_id("Line"));
    as.SET_PROP(4, la, 2);
    as.SET_PROP(4, lb, 3);
    as.R6(OpCode::TO_STRING, 5, 4, 0);

    // r6 = Named { s: "hi", tag: :ok } ; r7 = its dump (string quoted, atom :ok)
    as.NEW_STRUCT(6, as.struct_id("Named"));
    as.load_str(12, "hi");   as.SET_PROP(6, ns, 12);
    as.load_atom(13, "ok");  as.SET_PROP(6, nt, 13);
    as.R6(OpCode::TO_STRING, 7, 6, 0);

    // r8 = Point { x: 7 } (y omitted -> Undefined) ; r9 = its dump
    as.NEW_STRUCT(8, as.struct_id("Point"));
    as.load_const(14, 7); as.SET_PROP(8, px, 14);
    as.R6(OpCode::TO_STRING, 9, 8, 0);

    // r15 = Node { next: <self> } ; r16 = its depth-capped dump (must not crash)
    as.NEW_STRUCT(15, as.struct_id("Node"));
    as.SET_PROP(15, nn, 15);                    // next = itself -> a cycle
    as.R6(OpCode::TO_STRING, 16, 15, 0);
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, &interner, 17,
                             &as.constant_pool(), &as.struct_types(),
                             &as.string_literals(), &as.atom_names());
        auto* regs = res.get_reg_base();

        auto S = [&](int i, std::string_view want) {
            if (!regs[i].isPtr()) return false;
            const GcObject* o = GcObject::from_slots(regs[i].asPtr());
            return o->kind == GcObject::KIND_STRING &&
                   std::string_view(o->bytes(), o->string_length()) == want;
        };

        const std::string_view flat   = "Point { x: 1, y: 2 }";
        const std::string_view nested = "Line { a: Point { x: 3, y: 4 }, b: Point { x: 5, y: 6 } }";
        const std::string_view named  = "Named { s: \"hi\", tag: :ok }";
        const std::string_view omit   = "Point { x: 7, y: undefined }";
        const std::string_view cyclic =
            "Node { next: Node { next: Node { next: Node { next: Node { ... } } } } }";

        const bool ok =
            S(1, flat) && S(5, nested) && S(7, named) && S(9, omit) && S(16, cyclic);

        std::cout << std::format("  flat   : {}\n", S(1, flat)   ? "PASS" : "FAIL");
        std::cout << std::format("  nested : {}\n", S(5, nested) ? "PASS" : "FAIL");
        std::cout << std::format("  named  : {}\n", S(7, named)  ? "PASS" : "FAIL");
        std::cout << std::format("  omit   : {}\n", S(9, omit)   ? "PASS" : "FAIL");
        std::cout << std::format("  cyclic : {}\n", S(16, cyclic)? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_to_string_array -- the default ARRAY element dump: `[e0, e1, ...]` (Rust-near).
// Covers a flat int array, a nested array (arrays recurse), a mixed array whose string
// element is QUOTED and whose atom element renders `:name`, an array as a STRUCT field
// (the struct dump now descends into the array instead of `<array>`), the empty array
// `[]`, and the depth-4 CYCLE cap on a self-referential array (a[0] = a) which must
// terminate as `[ ... ]` rather than overflow the host stack.
// =============================================================================
inline void test_to_string_array() {
    std::cout << "=== TO_STRING (array element dump) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.define_struct("Bag", { "items" });      // a struct with an array field

    as.label("main");
    // r0 = [1, 2, 3] ; r1 = its dump
    as.ALLOC(0, 3);
    as.load_const(2, 1); as.load_const(3, 0); as.ARRAY_SET(0, 3, 2);
    as.load_const(2, 2); as.load_const(3, 1); as.ARRAY_SET(0, 3, 2);
    as.load_const(2, 3); as.load_const(3, 2); as.ARRAY_SET(0, 3, 2);
    as.R6(OpCode::TO_STRING, 1, 0, 0);

    // r4 = [[1, 2], [3]] ; r5 = its (nested) dump
    as.ALLOC(6, 2);                                                 // inner0 = [1, 2]
    as.load_const(2, 1); as.load_const(3, 0); as.ARRAY_SET(6, 3, 2);
    as.load_const(2, 2); as.load_const(3, 1); as.ARRAY_SET(6, 3, 2);
    as.ALLOC(7, 1);                                                 // inner1 = [3]
    as.load_const(2, 3); as.load_const(3, 0); as.ARRAY_SET(7, 3, 2);
    as.ALLOC(4, 2);                                                 // outer
    as.load_const(3, 0); as.ARRAY_SET(4, 3, 6);
    as.load_const(3, 1); as.ARRAY_SET(4, 3, 7);
    as.R6(OpCode::TO_STRING, 5, 4, 0);

    // r8 = [10, "hi", :ok] ; r9 = its dump (string quoted, atom :ok)
    as.ALLOC(8, 3);
    as.load_const(2, 10); as.load_const(3, 0); as.ARRAY_SET(8, 3, 2);
    as.load_str(2, "hi");  as.load_const(3, 1); as.ARRAY_SET(8, 3, 2);
    as.load_atom(2, "ok"); as.load_const(3, 2); as.ARRAY_SET(8, 3, 2);
    as.R6(OpCode::TO_STRING, 9, 8, 0);

    // r12 = Bag { items: [7, 8] } ; r13 = its dump (struct descends into the array)
    as.ALLOC(10, 2);
    as.load_const(2, 7); as.load_const(3, 0); as.ARRAY_SET(10, 3, 2);
    as.load_const(2, 8); as.load_const(3, 1); as.ARRAY_SET(10, 3, 2);
    as.NEW_STRUCT(12, as.struct_id("Bag"));
    as.SET_PROP(12, as.field("Bag", "items"), 10);
    as.R6(OpCode::TO_STRING, 13, 12, 0);

    // r14 = [] ; r15 = its dump
    as.ALLOC(14, 0);
    as.R6(OpCode::TO_STRING, 15, 14, 0);

    // r16 = [<self>] ; r17 = its depth-capped dump (must not crash)
    as.ALLOC(16, 1);
    as.load_const(3, 0); as.ARRAY_SET(16, 3, 16);                   // a[0] = itself -> a cycle
    as.R6(OpCode::TO_STRING, 17, 16, 0);
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        // top_frame_size = 18: r0..r17 are all scanned as GC roots across the allocating
        // TO_STRING / ALLOC safepoints (every live array must survive).
        auto res   = execute(bytecode, &heap, nullptr, &interner, 18,
                             &as.constant_pool(), &as.struct_types(),
                             &as.string_literals(), &as.atom_names());
        auto* regs = res.get_reg_base();

        auto S = [&](int i, std::string_view want) {
            if (!regs[i].isPtr()) return false;
            const GcObject* o = GcObject::from_slots(regs[i].asPtr());
            return o->kind == GcObject::KIND_STRING &&
                   std::string_view(o->bytes(), o->string_length()) == want;
        };

        const std::string_view flat   = "[1, 2, 3]";
        const std::string_view nested = "[[1, 2], [3]]";
        const std::string_view mixed  = "[10, \"hi\", :ok]";
        const std::string_view field  = "Bag { items: [7, 8] }";
        const std::string_view empty  = "[]";
        const std::string_view cyclic = "[[[[[ ... ]]]]]";

        const bool ok =
            S(1, flat) && S(5, nested) && S(9, mixed) &&
            S(13, field) && S(15, empty) && S(17, cyclic);

        std::cout << std::format("  flat   : {}\n", S(1, flat)    ? "PASS" : "FAIL");
        std::cout << std::format("  nested : {}\n", S(5, nested)  ? "PASS" : "FAIL");
        std::cout << std::format("  mixed  : {}\n", S(9, mixed)   ? "PASS" : "FAIL");
        std::cout << std::format("  field  : {}\n", S(13, field)  ? "PASS" : "FAIL");
        std::cout << std::format("  empty  : {}\n", S(15, empty)  ? "PASS" : "FAIL");
        std::cout << std::format("  cyclic : {}\n", S(17, cyclic) ? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_to_string_vec -- the KIND_VEC default dump behind TO_STRING. A vector renders
// in the SAME surface form as an array `[e0, e1, ...]` (only the live prefix [0,count)),
// so this mirrors test_to_string_array: flat, a mixed row (quoted string + atom), a
// nested vec-of-vecs, a vector as a struct field, the empty `[]`, and the depth-4 cycle
// cap on a self-referential vector (v[0] = v via the dual-kind ARRAY_SET).
// =============================================================================
inline void test_to_string_vec() {
    std::cout << "=== TO_STRING (vector element dump) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.define_struct("Bag", { "items" });      // a struct with a vector field

    as.label("main");
    // r0 = vec[1, 2, 3] ; r1 = dump
    as.VEC_NEW(0);
    as.load_const(2, 1); as.VEC_PUSH(0, 2);
    as.load_const(2, 2); as.VEC_PUSH(0, 2);
    as.load_const(2, 3); as.VEC_PUSH(0, 2);
    as.R6(OpCode::TO_STRING, 1, 0, 0);

    // r4 = vec[10, "hi", :ok] ; r5 = dump (string quoted, atom :ok)
    as.VEC_NEW(4);
    as.load_const(2, 10); as.VEC_PUSH(4, 2);
    as.load_str(2, "hi"); as.VEC_PUSH(4, 2);
    as.load_atom(2, "ok"); as.VEC_PUSH(4, 2);
    as.R6(OpCode::TO_STRING, 5, 4, 0);

    // r8 = vec[ vec[1, 2], vec[3] ] ; r9 = nested dump
    as.VEC_NEW(6); as.load_const(2, 1); as.VEC_PUSH(6, 2); as.load_const(2, 2); as.VEC_PUSH(6, 2);
    as.VEC_NEW(7); as.load_const(2, 3); as.VEC_PUSH(7, 2);
    as.VEC_NEW(8); as.VEC_PUSH(8, 6); as.VEC_PUSH(8, 7);
    as.R6(OpCode::TO_STRING, 9, 8, 0);

    // r12 = Bag { items: vec[7, 8] } ; r13 = dump (struct descends into the vector)
    as.VEC_NEW(10); as.load_const(2, 7); as.VEC_PUSH(10, 2); as.load_const(2, 8); as.VEC_PUSH(10, 2);
    as.NEW_STRUCT(12, as.struct_id("Bag"));
    as.SET_PROP(12, as.field("Bag", "items"), 10);
    as.R6(OpCode::TO_STRING, 13, 12, 0);

    // r14 = vec[] ; r15 = dump
    as.VEC_NEW(14);
    as.R6(OpCode::TO_STRING, 15, 14, 0);

    // r16 = vec[<self>] ; r17 = depth-capped dump (must not crash)
    as.VEC_NEW(16);
    as.load_const(2, 0); as.VEC_PUSH(16, 2);           // count 1 (placeholder)
    as.load_const(3, 0); as.ARRAY_SET(16, 3, 16);      // v[0] = itself -> a cycle (dual-kind set)
    as.R6(OpCode::TO_STRING, 17, 16, 0);
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, &interner, 18,
                             &as.constant_pool(), &as.struct_types(),
                             &as.string_literals(), &as.atom_names());
        auto* regs = res.get_reg_base();

        auto S = [&](int i, std::string_view want) {
            if (!regs[i].isPtr()) return false;
            const GcObject* o = GcObject::from_slots(regs[i].asPtr());
            return o->kind == GcObject::KIND_STRING &&
                   std::string_view(o->bytes(), o->string_length()) == want;
        };

        const std::string_view flat   = "[1, 2, 3]";
        const std::string_view mixed  = "[10, \"hi\", :ok]";
        const std::string_view nested = "[[1, 2], [3]]";
        const std::string_view field  = "Bag { items: [7, 8] }";
        const std::string_view empty  = "[]";
        const std::string_view cyclic = "[[[[[ ... ]]]]]";

        const bool ok =
            S(1, flat) && S(5, mixed) && S(9, nested) &&
            S(13, field) && S(15, empty) && S(17, cyclic);

        std::cout << std::format("  flat   : {}\n", S(1, flat)    ? "PASS" : "FAIL");
        std::cout << std::format("  mixed  : {}\n", S(5, mixed)   ? "PASS" : "FAIL");
        std::cout << std::format("  nested : {}\n", S(9, nested)  ? "PASS" : "FAIL");
        std::cout << std::format("  field  : {}\n", S(13, field)  ? "PASS" : "FAIL");
        std::cout << std::format("  empty  : {}\n", S(15, empty)  ? "PASS" : "FAIL");
        std::cout << std::format("  cyclic : {}\n", S(17, cyclic) ? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_to_string_map -- the KIND_MAP default dump `#{k => v, ...}` behind TO_STRING.
// The dump walks the backing in HASH order, so insertion order is NOT preserved:
// single-entry maps are asserted exactly (unambiguous), the two-entry case accepts
// either order (exercises the ", " separator), and the rest cover the empty map, a
// quoted string key, an atom key, a nested map value, and the depth-4 cycle cap on a
// self-referential map (`m.[1] = m`).
// =============================================================================
inline void test_to_string_map() {
    std::cout << "=== TO_STRING (map dump) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.label("main");

    // r0 = #{} ; r1 = dump
    as.MAP_NEW(0);
    as.R6(OpCode::TO_STRING, 1, 0, 0);

    // r2 = #{1 => 100} ; r3 = dump  (scratch key/val in r20/r21)
    as.MAP_NEW(2);
    as.load_const(20, 1); as.load_const(21, 100); as.MAP_SET(2, 20, 21);
    as.R6(OpCode::TO_STRING, 3, 2, 0);

    // r4 = #{"name" => 1} ; r5 = dump  (string key -> quoted)
    as.MAP_NEW(4);
    as.load_str(20, "name"); as.load_const(21, 1); as.MAP_SET(4, 20, 21);
    as.R6(OpCode::TO_STRING, 5, 4, 0);

    // r6 = #{:ok => 1} ; r7 = dump  (atom key -> :ok)
    as.MAP_NEW(6);
    as.load_atom(20, "ok"); as.load_const(21, 1); as.MAP_SET(6, 20, 21);
    as.R6(OpCode::TO_STRING, 7, 6, 0);

    // r8 = #{1 => 10, 2 => 20} ; r9 = dump  (two entries -> either hash order)
    as.MAP_NEW(8);
    as.load_const(20, 1); as.load_const(21, 10); as.MAP_SET(8, 20, 21);
    as.load_const(20, 2); as.load_const(21, 20); as.MAP_SET(8, 20, 21);
    as.R6(OpCode::TO_STRING, 9, 8, 0);

    // r10 = #{1 => #{2 => 3}} ; r12 = dump  (nested map value; inner in r11)
    as.MAP_NEW(11);
    as.load_const(20, 2); as.load_const(21, 3); as.MAP_SET(11, 20, 21);
    as.MAP_NEW(10);
    as.load_const(20, 1); as.MAP_SET(10, 20, 11);            // value = inner map
    as.R6(OpCode::TO_STRING, 12, 10, 0);

    // r13 = #{1 => <self>} ; r14 = depth-capped dump (must not crash)
    as.MAP_NEW(13);
    as.load_const(20, 1); as.MAP_SET(13, 20, 13);            // m[1] = itself -> a cycle
    as.R6(OpCode::TO_STRING, 14, 13, 0);
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        // top_frame_size = 22: every live map (r0..r13) + the scratch key/val (r20/r21)
        // is GC-scanned across the allocating MAP_NEW/MAP_SET/TO_STRING safepoints.
        auto res   = execute(bytecode, &heap, nullptr, &interner, 22,
                             &as.constant_pool(), &as.struct_types(),
                             &as.string_literals(), &as.atom_names());
        auto* regs = res.get_reg_base();

        auto str_at = [&](int i) -> std::string {
            if (!regs[i].isPtr()) return "<not-a-ptr>";
            const GcObject* o = GcObject::from_slots(regs[i].asPtr());
            if (o->kind != GcObject::KIND_STRING) return "<not-a-string>";
            return std::string(o->bytes(), o->string_length());
        };
        auto S = [&](int i, std::string_view want) {
            const std::string got = str_at(i);
            const bool ok = got == want;
            if (!ok) std::cout << std::format("    (got \"{}\", want \"{}\")\n", got, want);
            return ok;
        };

        const std::string two = str_at(9);
        const bool two_ok = (two == "#{1 => 10, 2 => 20}") || (two == "#{2 => 20, 1 => 10}");
        if (!two_ok) std::cout << std::format("    (got \"{}\")\n", two);

        const bool empty_ok  = S(1,  "#{}");
        const bool flat_ok   = S(3,  "#{1 => 100}");
        const bool strk_ok   = S(5,  "#{\"name\" => 1}");
        const bool atomk_ok  = S(7,  "#{:ok => 1}");
        const bool nested_ok = S(12, "#{1 => #{2 => 3}}");
        const bool cyclic_ok = S(14, "#{1 => #{1 => #{1 => #{1 => #{ ... }}}}}");

        const bool ok = empty_ok && flat_ok && strk_ok && atomk_ok &&
                        two_ok && nested_ok && cyclic_ok;

        std::cout << std::format("  empty  : {}\n", empty_ok  ? "PASS" : "FAIL");
        std::cout << std::format("  flat   : {}\n", flat_ok   ? "PASS" : "FAIL");
        std::cout << std::format("  str key: {}\n", strk_ok   ? "PASS" : "FAIL");
        std::cout << std::format("  atom k : {}\n", atomk_ok  ? "PASS" : "FAIL");
        std::cout << std::format("  two    : {}\n", two_ok    ? "PASS" : "FAIL");
        std::cout << std::format("  nested : {}\n", nested_ok ? "PASS" : "FAIL");
        std::cout << std::format("  cyclic : {}\n", cyclic_ok ? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_to_string_tuple -- the DumpStyle::Tuple render behind TO_STRING: a KIND_OBJECT
// whose struct type is tagged Tuple prints positionally as `(e0, e1)` instead of the
// `Name { ... }` field dump. Covers a pair, the 1-tuple trailing comma `(7,)`, the empty
// `()`, a nested tuple, a quoted string + atom element, and a tuple AS a struct field.
// (The compiler tags its $TupleN fiction type Tuple; here we drive it directly via
// define_struct(..., DumpStyle::Tuple).)
// =============================================================================
inline void test_to_string_tuple() {
    std::cout << "=== TO_STRING (tuple dump) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.define_struct("Pair", { "_0", "_1" }, DumpStyle::Tuple);   // id 0
    as.define_struct("One",  { "_0" },        DumpStyle::Tuple);  // id 1
    as.define_struct("Zero", { },             DumpStyle::Tuple);  // id 2
    as.define_struct("Wrap", { "t" });                            // id 3 (plain struct)

    as.label("main");
    // r0 = (1, 2) ; r1 = dump  (scratch value in r20)
    as.NEW_STRUCT(0, as.struct_id("Pair"));
    as.load_const(20, 1); as.SET_PROP(0, 0, 20);
    as.load_const(20, 2); as.SET_PROP(0, 1, 20);
    as.R6(OpCode::TO_STRING, 1, 0, 0);

    // r2 = (7,) 1-tuple ; r3 = dump
    as.NEW_STRUCT(2, as.struct_id("One"));
    as.load_const(20, 7); as.SET_PROP(2, 0, 20);
    as.R6(OpCode::TO_STRING, 3, 2, 0);

    // r4 = () empty tuple ; r5 = dump
    as.NEW_STRUCT(4, as.struct_id("Zero"));
    as.R6(OpCode::TO_STRING, 5, 4, 0);

    // r6 = (1, (2, 3)) nested ; r7 = dump  (inner tuple in r8)
    as.NEW_STRUCT(8, as.struct_id("Pair"));
    as.load_const(20, 2); as.SET_PROP(8, 0, 20);
    as.load_const(20, 3); as.SET_PROP(8, 1, 20);
    as.NEW_STRUCT(6, as.struct_id("Pair"));
    as.load_const(20, 1); as.SET_PROP(6, 0, 20);
    as.SET_PROP(6, 1, 8);
    as.R6(OpCode::TO_STRING, 7, 6, 0);

    // r9 = ("hi", :ok) ; r10 = dump  (string quoted, atom :ok)
    as.NEW_STRUCT(9, as.struct_id("Pair"));
    as.load_str(20, "hi");  as.SET_PROP(9, 0, 20);
    as.load_atom(20, "ok"); as.SET_PROP(9, 1, 20);
    as.R6(OpCode::TO_STRING, 10, 9, 0);

    // r11 = Wrap { t: (1, 2) } ; r12 = dump  (tuple as a struct field; inner in r13)
    as.NEW_STRUCT(13, as.struct_id("Pair"));
    as.load_const(20, 1); as.SET_PROP(13, 0, 20);
    as.load_const(20, 2); as.SET_PROP(13, 1, 20);
    as.NEW_STRUCT(11, as.struct_id("Wrap"));
    as.SET_PROP(11, as.field("Wrap", "t"), 13);
    as.R6(OpCode::TO_STRING, 12, 11, 0);
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        // top_frame_size = 21: r0..r20 are scanned across the allocating NEW_STRUCT/TO_STRING.
        auto res   = execute(bytecode, &heap, nullptr, &interner, 21,
                             &as.constant_pool(), &as.struct_types(),
                             &as.string_literals(), &as.atom_names());
        auto* regs = res.get_reg_base();

        auto S = [&](int i, std::string_view want) {
            if (!regs[i].isPtr()) return false;
            const GcObject* o = GcObject::from_slots(regs[i].asPtr());
            const bool ok = o->kind == GcObject::KIND_STRING &&
                            std::string_view(o->bytes(), o->string_length()) == want;
            if (!ok && o->kind == GcObject::KIND_STRING)
                std::cout << std::format("    (got \"{}\", want \"{}\")\n",
                    std::string_view(o->bytes(), o->string_length()), want);
            return ok;
        };

        const bool ok =
            S(1, "(1, 2)") && S(3, "(7,)") && S(5, "()") &&
            S(7, "(1, (2, 3))") && S(10, "(\"hi\", :ok)") &&
            S(12, "Wrap { t: (1, 2) }");

        std::cout << std::format("  pair   : {}\n", S(1, "(1, 2)")             ? "PASS" : "FAIL");
        std::cout << std::format("  one    : {}\n", S(3, "(7,)")               ? "PASS" : "FAIL");
        std::cout << std::format("  zero   : {}\n", S(5, "()")                 ? "PASS" : "FAIL");
        std::cout << std::format("  nested : {}\n", S(7, "(1, (2, 3))")        ? "PASS" : "FAIL");
        std::cout << std::format("  mixed  : {}\n", S(10, "(\"hi\", :ok)")     ? "PASS" : "FAIL");
        std::cout << std::format("  field  : {}\n", S(12, "Wrap { t: (1, 2) }")? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_to_string_list -- the DumpStyle::List render behind TO_STRING: a `$List`-style
// cons cell prints as `[e0, e1, ...]` via an ITERATIVE spine walk (one depth level, so a
// long flat list renders fully). Covers [1, 2, 3], the empty list (= nil -> "nil"), a
// nested list-of-lists, quoted string elements, and a hand-built CYCLIC list, which must
// terminate via the TO_STRING_MAX_LIST spine cap (not hang / overflow).
// =============================================================================
inline void test_to_string_list() {
    std::cout << "=== TO_STRING (cons-list dump) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.define_struct("Cell", { "head", "tail" }, DumpStyle::List);   // id 0

    as.label("main");
    // [1, 2, 3] = Cell{1, Cell{2, Cell{3, nil}}}  (c3=r2, c2=r1, c1=r0; scratch r20)
    as.NEW_STRUCT(2, as.struct_id("Cell"));
    as.load_const(20, 3); as.SET_PROP(2, 0, 20);
    as.load_constant(20, Value::fromNil()); as.SET_PROP(2, 1, 20);
    as.NEW_STRUCT(1, as.struct_id("Cell"));
    as.load_const(20, 2); as.SET_PROP(1, 0, 20);
    as.SET_PROP(1, 1, 2);
    as.NEW_STRUCT(0, as.struct_id("Cell"));
    as.load_const(20, 1); as.SET_PROP(0, 0, 20);
    as.SET_PROP(0, 1, 1);
    as.R6(OpCode::TO_STRING, 3, 0, 0);            // r3 = "[1, 2, 3]"

    // empty list = nil ; r5 = dump ("nil")
    as.load_constant(4, Value::fromNil());
    as.R6(OpCode::TO_STRING, 5, 4, 0);

    // [[1], [2]] : inner1=[1] in r6, inner2=[2] in r7, outer head/tail in r8/r9
    as.NEW_STRUCT(6, as.struct_id("Cell"));
    as.load_const(20, 1); as.SET_PROP(6, 0, 20);
    as.load_constant(20, Value::fromNil()); as.SET_PROP(6, 1, 20);
    as.NEW_STRUCT(7, as.struct_id("Cell"));
    as.load_const(20, 2); as.SET_PROP(7, 0, 20);
    as.load_constant(20, Value::fromNil()); as.SET_PROP(7, 1, 20);
    as.NEW_STRUCT(9, as.struct_id("Cell"));       // tail cell -> inner2
    as.SET_PROP(9, 0, 7);
    as.load_constant(20, Value::fromNil()); as.SET_PROP(9, 1, 20);
    as.NEW_STRUCT(8, as.struct_id("Cell"));       // head cell -> inner1
    as.SET_PROP(8, 0, 6);
    as.SET_PROP(8, 1, 9);
    as.R6(OpCode::TO_STRING, 10, 8, 0);           // r10 = "[[1], [2]]"

    // ["a", "b"] -> quoted string elements
    as.NEW_STRUCT(12, as.struct_id("Cell"));
    as.load_str(20, "b"); as.SET_PROP(12, 0, 20);
    as.load_constant(20, Value::fromNil()); as.SET_PROP(12, 1, 20);
    as.NEW_STRUCT(11, as.struct_id("Cell"));
    as.load_str(20, "a"); as.SET_PROP(11, 0, 20);
    as.SET_PROP(11, 1, 12);
    as.R6(OpCode::TO_STRING, 13, 11, 0);          // r13 = "[\"a\", \"b\"]"

    // cyclic: c0=Cell{1, c1}, c1=Cell{2, c0} -> spine cap must terminate it
    as.NEW_STRUCT(14, as.struct_id("Cell"));      // c0
    as.NEW_STRUCT(15, as.struct_id("Cell"));      // c1
    as.load_const(20, 1); as.SET_PROP(14, 0, 20); as.SET_PROP(14, 1, 15);
    as.load_const(20, 2); as.SET_PROP(15, 0, 20); as.SET_PROP(15, 1, 14);
    as.R6(OpCode::TO_STRING, 16, 14, 0);          // r16 = bounded cyclic dump
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        // top_frame_size = 21: every live cell (r0..r15) + scratch r20 is GC-scanned.
        auto res   = execute(bytecode, &heap, nullptr, &interner, 21,
                             &as.constant_pool(), &as.struct_types(),
                             &as.string_literals(), &as.atom_names());
        auto* regs = res.get_reg_base();

        auto str_at = [&](int i) -> std::string {
            if (!regs[i].isPtr()) return "<not-a-ptr>";
            const GcObject* o = GcObject::from_slots(regs[i].asPtr());
            if (o->kind != GcObject::KIND_STRING) return "<not-a-string>";
            return std::string(o->bytes(), o->string_length());
        };
        auto S = [&](int i, std::string_view want) {
            const std::string got = str_at(i);
            const bool ok = got == want;
            if (!ok) std::cout << std::format("    (got \"{}\", want \"{}\")\n", got, want);
            return ok;
        };

        // cyclic: bounded ("[1, 2, 1, 2, ..." then ", ...]" once the cap trips)
        const std::string cyc = str_at(16);
        const bool cyc_ok = cyc.starts_with("[1, 2,") && cyc.ends_with(", ...]");
        if (!cyc_ok) std::cout << std::format("    (cyclic got \"{}\")\n", cyc);

        const bool flat_ok   = S(3,  "[1, 2, 3]");
        const bool empty_ok  = S(5,  "nil");            // empty list = nil
        const bool nested_ok = S(10, "[[1], [2]]");
        const bool strel_ok  = S(13, "[\"a\", \"b\"]");

        const bool ok = flat_ok && empty_ok && nested_ok && strel_ok && cyc_ok;

        std::cout << std::format("  flat   : {}\n", flat_ok   ? "PASS" : "FAIL");
        std::cout << std::format("  empty  : {}\n", empty_ok  ? "PASS" : "FAIL");
        std::cout << std::format("  nested : {}\n", nested_ok ? "PASS" : "FAIL");
        std::cout << std::format("  str el : {}\n", strel_ok  ? "PASS" : "FAIL");
        std::cout << std::format("  cyclic : {}\n", cyc_ok    ? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_print -- PRINT / PRINTLN to a REDIRECTABLE sink. execute()'s out parameter
// points vm->out at a captured std::ostringstream, so the exact bytes are asserted
// without touching stdout. Covers: a raw PRINT on a LOAD_STR string (no coercion,
// no newline), PRINTLN (adds '\n'), the TO_STRING + PRINTLN path for a number and
// an atom (print output == toString output by construction), the 0-arg println()
// lowering (empty string + PRINTLN -> a bare newline), and the Nil result of PRINT
// (so print(x) is an expression yielding nil).
// =============================================================================
inline void test_print() {
    std::cout << "=== PRINT / PRINTLN (redirectable sink) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.label("main");
    as.load_str(0, "hi");          as.PRINT(1, 0);                 // out += "hi"   (no newline)
    as.load_str(2, "world");       as.PRINTLN(3, 2);               // out += "world\n"
    as.load_const(4, 42);          as.R6(OpCode::TO_STRING, 4, 4, 0);
                                   as.PRINTLN(5, 4);               // out += "42\n"
    as.load_atom(6, "ok");         as.R6(OpCode::TO_STRING, 6, 6, 0);
                                   as.PRINTLN(7, 6);               // out += ":ok\n"
    as.load_str(8, "");            as.PRINTLN(9, 8);               // out += "\n"  (bare println())
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        std::ostringstream oss;
        // top_frame_size = 10: r0..r9 scanned as GC roots across the TO_STRING allocs.
        auto res   = execute(bytecode, &heap, nullptr, &interner, 10,
                             &as.constant_pool(), &as.struct_types(),
                             &as.string_literals(), &as.atom_names(), nullptr, &oss);
        auto* regs = res.get_reg_base();

        const std::string got  = oss.str();
        const std::string want = "hiworld\n42\n:ok\n\n";
        const bool out_ok      = (got == want);
        const bool nil_ok      = regs[1].isNil();   // PRINT sets rd = Nil

        std::cout << std::format("  output : {}  ({} bytes)\n", out_ok ? "PASS" : "FAIL", got.size());
        std::cout << std::format("  nil rd : {}\n", nil_ok ? "PASS" : "FAIL");
        check(out_ok && nil_ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_panic -- the PANIC opcode: abort with a LOCATED fault carrying the message
// string in ra. It never returns (raise_located throws a VmFault), so execute()
// propagates the exception; the message must contain the panic text.
// =============================================================================
inline void test_panic() {
    std::cout << "=== PANIC (located abort) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.label("main");
    as.load_str(0, "boom");
    as.PANIC(0);                    // abort with "boom"; never returns
    as.J(OpCode::HALT);             // unreachable
    const auto bytecode = as.assemble();

    std::cout << "Running (expect throw)...\n";
    bool threw = false, msg_ok = false;
    try {
        execute(bytecode, &heap, nullptr, &interner, 1,
                &as.constant_pool(), &as.struct_types(),
                &as.string_literals(), &as.atom_names());
    }
    catch (const std::exception& e) {
        threw  = true;
        msg_ok = std::string(e.what()).find("boom") != std::string::npos;
        std::cout << "  caught: " << e.what() << "\n";
    }
    std::cout << std::format("  threw  : {}\n", threw ? "PASS" : "FAIL");
    std::cout << std::format("  message: {}\n", msg_ok ? "PASS" : "FAIL");
    check(threw && msg_ok);
}

// =============================================================================
// test_string_compare -- generic LT/LE dispatch. Lexicographic (unsigned byte,
// prefix < longer) on strings, and the numeric path still works through the same
// opcodes. (`>`/`>=` are compiler-side operand swaps, so the VM only has LT/LE.)
// =============================================================================
inline void test_string_compare() {
    std::cout << "=== string compare (LT/LE) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.label("main");
    as.load_str(0, "apple");
    as.load_str(1, "banana");
    as.R6(OpCode::LT, 2, 0, 1);         // "apple" < "banana"  -> true
    as.R6(OpCode::LT, 3, 1, 0);         // "banana" < "apple"  -> false
    as.load_str(4, "ab");
    as.load_str(5, "abc");
    as.R6(OpCode::LT, 6, 4, 5);         // "ab" < "abc" (prefix) -> true
    as.load_str(7, "x");
    as.load_str(8, "x");
    as.R6(OpCode::LT, 9, 7, 8);         // "x" < "x"  -> false
    as.R6(OpCode::LE, 10, 7, 8);        // "x" <= "x" -> true
    as.load_const(11, 3);
    as.load_const(12, 5);
    as.R6(OpCode::LT, 13, 11, 12);      // 3 < 5 (numeric dispatch) -> true
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, &interner, 14, nullptr, nullptr,
                             &as.string_literals());
        auto* regs = res.get_reg_base();
        auto B = [&](int i, bool want) { return regs[i].isBool() && regs[i].asBool() == want; };

        const bool str_ok = B(2, true) && B(3, false) && B(6, true) && B(9, false) && B(10, true);
        const bool num_ok = B(13, true);
        std::cout << std::format("  lexicographic LT/LE: {}\n", str_ok ? "PASS" : "FAIL");
        std::cout << std::format("  numeric dispatch:    {}\n", num_ok ? "PASS" : "FAIL");
        check(str_ok && num_ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_string_content_eq -- the strategy-B regression: EQ compares heap strings by
// CONTENT, not pointer identity. Builds a NON-interned string via concat and EQ-s it
// against the interned literal of equal content: pointers differ, EQ must still be
// true (and NE false). Without content-EQ this would wrongly report not-equal.
// =============================================================================
inline void test_string_content_eq() {
    std::cout << "=== string content equality (EQ) ===\n";

    Heap heap(64 * 1024);
    StringInterner interner;

    Assembler as;
    as.label("main");
    as.load_str(0, "foo");
    as.load_str(1, "bar");
    as.R6(OpCode::ADD, 2, 0, 1);        // r2 = "foobar" (fresh, NOT interned)
    as.load_str(3, "foobar");           // r3 = interned literal "foobar"
    as.R6(OpCode::EQ, 4, 2, 3);         // content-equal -> true
    as.R6(OpCode::NE, 5, 2, 3);         // -> false
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, &interner, 6, nullptr, nullptr,
                             &as.string_literals());
        auto* regs = res.get_reg_base();

        const bool distinct_ptr = regs[2].isPtr() && regs[3].isPtr() &&
                                  regs[2].asPtr() != regs[3].asPtr();
        const bool eq_ok = regs[4].isBool() && regs[4].asBool() == true &&
                           regs[5].isBool() && regs[5].asBool() == false;
        std::cout << std::format("  distinct pointers, equal content: {}\n", distinct_ptr ? "PASS" : "FAIL");
        std::cout << std::format("  EQ true / NE false by content:    {}\n", eq_ok ? "PASS" : "FAIL");
        check(distinct_ptr && eq_ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_struct -- fixed-shape struct basics: NEW_STRUCT / SET_PROP / GET_PROP /
// IS_OBJECT. Defines Point{ x, y }, constructs one, round-trips both fields, and
// checks IS_OBJECT (true for the struct, false for an int). No GC involved.
// =============================================================================
inline void test_struct() {
    std::cout << "=== struct ===\n";

    Heap heap(64 * 1024);

    Assembler as;
    const uint16_t Point = as.define_struct("Point", { "x", "y" });

    as.label("main");
    as.NEW_STRUCT(0, Point);                        // r0 = new Point
    as.load_const(1, 111);
    as.load_const(2, 222);
    as.SET_PROP(0, as.field("Point", "x"), 1);      // r0.x = 111
    as.SET_PROP(0, as.field("Point", "y"), 2);      // r0.y = 222
    as.GET_PROP(3, 0, as.field("Point", "x"));      // r3 = r0.x
    as.GET_PROP(4, 0, as.field("Point", "y"));      // r4 = r0.y
    as.IS_OBJECT(5, 0);                             // r5 = IS_OBJECT(r0) -> true
    as.IS_OBJECT(6, 1);                             // r6 = IS_OBJECT(r1) -> false (int)
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main");
    dis.print();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, nullptr, 0, nullptr, &as.struct_types());
        auto* regs = res.get_reg_base();

        const bool is_ptr = regs[0].isPtr();
        GcObject*  obj    = is_ptr ? GcObject::from_slots(regs[0].asPtr()) : nullptr;

        const bool shape_ok =
            is_ptr && obj &&
            obj->kind == GcObject::KIND_OBJECT &&
            obj->slot_count() == 2 &&
            obj->object_type_id() == Point;

        const bool fields_ok =
            obj &&
            obj->slots()[0].asSigned48() == 111 &&
            obj->slots()[1].asSigned48() == 222 &&
            regs[3].asSigned48() == 111 &&
            regs[4].asSigned48() == 222;

        const bool isobj_ok =
            regs[5].isBool() && regs[5].asBool() == true &&
            regs[6].isBool() && regs[6].asBool() == false;

        const bool ok = shape_ok && fields_ok && isobj_ok;

        std::cout << std::format("  shape (kind/arity/type id):  {}\n", shape_ok ? "PASS" : "FAIL");
        std::cout << std::format("  field round-trip:            {}\n", fields_ok ? "PASS" : "FAIL");
        std::cout << std::format("  IS_OBJECT true/false:        {}\n", isobj_ok ? "PASS" : "FAIL");
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_eq_deep -- the EQ_DEEP opcode: STRUCTURAL (deep / value) equality behind
// the surface `==` on a composite. Proves the VM half of "structural ==" in
// isolation (no compiler involved): two separately-built equal composites compare
// EQUAL where the reference-identity EQ would say false. Covers structs (equal /
// unequal / nested), distinct type-ids (the enum None-vs-Some shape), arrays,
// vectors, byte buffers, ORDER-INDEPENDENT maps, same-pointer short-circuit, the
// fresh-string content path, and CYCLIC graphs (Block G), which are answered rather
// than refused. The closure trap is defensive (checker-forbidden) and not exercised here.
// =============================================================================
inline void test_eq_deep() {
    std::cout << "=== eq_deep (EQ_DEEP structural equality) ===\n";

    // ---- Block A: structs -- equal, unequal, nested (a compiler never sees this) ----
    {
        Heap heap(256 * 1024);
        Assembler as;
        const uint16_t Point = as.define_struct("Point", { "x", "y" });
        const uint16_t Line  = as.define_struct("Line",  { "a", "b" });
        as.label("main");
        // Two SEPARATE Point{1,2} objects -> distinct pointers, structurally equal.
        as.NEW_STRUCT(0, Point);
        as.load_const(10, 1); as.SET_PROP(0, as.field("Point","x"), 10);
        as.load_const(10, 2); as.SET_PROP(0, as.field("Point","y"), 10);
        as.NEW_STRUCT(1, Point);
        as.load_const(10, 1); as.SET_PROP(1, as.field("Point","x"), 10);
        as.load_const(10, 2); as.SET_PROP(1, as.field("Point","y"), 10);
        // A third Point{1,3} -> differs in one field.
        as.NEW_STRUCT(2, Point);
        as.load_const(10, 1); as.SET_PROP(2, as.field("Point","x"), 10);
        as.load_const(10, 3); as.SET_PROP(2, as.field("Point","y"), 10);
        as.R6(OpCode::EQ_DEEP, 3, 0, 1);   // r3 = Point{1,2} == Point{1,2} -> true
        as.R6(OpCode::EQ_DEEP, 4, 0, 2);   // r4 = Point{1,2} == Point{1,3} -> false
        // Nested: Line{ Point{1,2}, Point{1,3} } built twice (fresh Points each time).
        as.NEW_STRUCT(5, Line);  as.SET_PROP(5, as.field("Line","a"), 0); as.SET_PROP(5, as.field("Line","b"), 2);
        as.NEW_STRUCT(6, Line);
        as.NEW_STRUCT(7, Point); as.load_const(10,1); as.SET_PROP(7, as.field("Point","x"),10); as.load_const(10,2); as.SET_PROP(7, as.field("Point","y"),10);
        as.NEW_STRUCT(8, Point); as.load_const(10,1); as.SET_PROP(8, as.field("Point","x"),10); as.load_const(10,3); as.SET_PROP(8, as.field("Point","y"),10);
        as.SET_PROP(6, as.field("Line","a"), 7); as.SET_PROP(6, as.field("Line","b"), 8);
        as.R6(OpCode::EQ_DEEP, 9, 5, 6);   // r9 = Line{P{1,2},P{1,3}} == same -> true
        // Break the nested one: change r8.y to 4.
        as.load_const(10, 4); as.SET_PROP(8, as.field("Point","y"), 10);
        as.R6(OpCode::EQ_DEEP, 11, 5, 6);  // r11 = now differs -> false
        as.J(OpCode::HALT);
        std::cout << "Running Block A (structs)...\n";
        try {
            auto res = execute(as.assemble(), &heap, nullptr, nullptr, 16, nullptr, &as.struct_types());
            auto* r = res.get_reg_base();
            auto B = [&](int i, bool want){ return r[i].isBool() && r[i].asBool() == want; };
            const bool ok = B(3,true) && B(4,false) && B(9,true) && B(11,false);
            std::cout << std::format("  Point{{1,2}}==Point{{1,2}} (distinct) : {}\n", B(3,true)?"PASS":"FAIL");
            std::cout << std::format("  Point{{1,2}}==Point{{1,3}}           : {}\n", B(4,false)?"PASS":"FAIL");
            std::cout << std::format("  nested Line equal / unequal       : {}\n", (B(9,true)&&B(11,false))?"PASS":"FAIL");
            check(ok);
        } catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block B: distinct type-ids (the enum None-vs-Some / nullary shape) ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        const uint16_t None = as.define_struct("None", {});          // 0-field variant (like enum None)
        const uint16_t Some = as.define_struct("Some", { "v" });     // 1-field variant (like Some(v))
        as.label("main");
        as.NEW_STRUCT(0, None);                                      // None #1
        as.NEW_STRUCT(1, None);                                      // None #2 (distinct pointer)
        as.NEW_STRUCT(2, Some); as.load_const(10, 7); as.SET_PROP(2, as.field("Some","v"), 10);  // Some(7)
        as.NEW_STRUCT(3, Some); as.load_const(10, 7); as.SET_PROP(3, as.field("Some","v"), 10);  // Some(7) #2
        as.R6(OpCode::EQ_DEEP, 4, 0, 1);   // None == None -> true (same type-id, 0 fields)
        as.R6(OpCode::EQ_DEEP, 5, 0, 2);   // None == Some(7) -> false (different type-id)
        as.R6(OpCode::EQ_DEEP, 6, 2, 3);   // Some(7) == Some(7) -> true
        as.J(OpCode::HALT);
        std::cout << "Running Block B (enum-shape type-ids)...\n";
        try {
            auto res = execute(as.assemble(), &heap, nullptr, nullptr, 16, nullptr, &as.struct_types());
            auto* r = res.get_reg_base();
            auto B = [&](int i, bool want){ return r[i].isBool() && r[i].asBool() == want; };
            const bool ok = B(4,true) && B(5,false) && B(6,true);
            std::cout << std::format("  None==None true, None==Some false, Some(7)==Some(7) true : {}\n", ok?"PASS":"FAIL");
            check(ok);
        } catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block C: arrays -- equal / element-differ / length-differ ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        auto fill3 = [&](uint8_t dst, int a, int b, int c){
            as.ALLOC(dst, 3);
            as.load_const(10, static_cast<int16_t>(a)); as.load_const(11,0); as.ARRAY_SET(dst,11,10);
            as.load_const(10, static_cast<int16_t>(b)); as.load_const(11,1); as.ARRAY_SET(dst,11,10);
            as.load_const(10, static_cast<int16_t>(c)); as.load_const(11,2); as.ARRAY_SET(dst,11,10);
        };
        fill3(0, 1,2,3);
        fill3(1, 1,2,3);
        fill3(2, 1,2,4);
        as.ALLOC(3, 2); as.load_const(10,1); as.load_const(11,0); as.ARRAY_SET(3,11,10); as.load_const(10,2); as.load_const(11,1); as.ARRAY_SET(3,11,10); // [1,2]
        as.R6(OpCode::EQ_DEEP, 4, 0, 1);   // [1,2,3]==[1,2,3] -> true
        as.R6(OpCode::EQ_DEEP, 5, 0, 2);   // [1,2,3]==[1,2,4] -> false
        as.R6(OpCode::EQ_DEEP, 6, 0, 3);   // [1,2,3]==[1,2]   -> false (length)
        as.J(OpCode::HALT);
        std::cout << "Running Block C (arrays)...\n";
        try {
            auto res = execute(as.assemble(), &heap, nullptr, nullptr, 16);
            auto* r = res.get_reg_base();
            auto B = [&](int i, bool want){ return r[i].isBool() && r[i].asBool() == want; };
            const bool ok = B(4,true) && B(5,false) && B(6,false);
            std::cout << std::format("  array equal / differ / length : {}\n", ok?"PASS":"FAIL");
            check(ok);
        } catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block D: vectors + byte buffers ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.VEC_NEW(0); as.load_const(10,10); as.VEC_PUSH(0,10); as.load_const(10,20); as.VEC_PUSH(0,10);  // [10,20]
        as.VEC_NEW(1); as.load_const(10,10); as.VEC_PUSH(1,10); as.load_const(10,20); as.VEC_PUSH(1,10);  // [10,20]
        as.VEC_NEW(2); as.load_const(10,10); as.VEC_PUSH(2,10); as.load_const(10,99); as.VEC_PUSH(2,10);  // [10,99]
        as.R6(OpCode::EQ_DEEP, 3, 0, 1);   // vec equal -> true
        as.R6(OpCode::EQ_DEEP, 4, 0, 2);   // vec differ -> false
        // Bytes via a fresh copy of a string ("AB") vs another fresh copy; and "AB" vs "AC".
        as.load_str(5, "AB"); as.BYTES_FROM_STR(6, 5);   // r6 = bytes("AB")
        as.load_str(7, "AB"); as.BYTES_FROM_STR(8, 7);   // r8 = bytes("AB") (distinct)
        as.load_str(9, "AC"); as.BYTES_FROM_STR(12, 9);  // r12 = bytes("AC")
        as.R6(OpCode::EQ_DEEP, 13, 6, 8);  // bytes equal -> true
        as.R6(OpCode::EQ_DEEP, 14, 6, 12); // bytes differ -> false
        as.J(OpCode::HALT);
        std::cout << "Running Block D (vec + bytes)...\n";
        try {
            StringInterner interner;
            auto res = execute(as.assemble(), &heap, nullptr, &interner, 16, nullptr, nullptr, &as.string_literals());
            auto* r = res.get_reg_base();
            auto B = [&](int i, bool want){ return r[i].isBool() && r[i].asBool() == want; };
            const bool ok = B(3,true) && B(4,false) && B(13,true) && B(14,false);
            std::cout << std::format("  vec equal / differ            : {}\n", (B(3,true)&&B(4,false))?"PASS":"FAIL");
            std::cout << std::format("  bytes equal / differ          : {}\n", (B(13,true)&&B(14,false))?"PASS":"FAIL");
            check(ok);
        } catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block E: maps -- ORDER-INDEPENDENT equal, value-differ, size-differ ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        // A = {1:100, 2:200, 3:300} inserted 1,2,3
        as.MAP_NEW(0);
        as.load_const(10,1); as.load_const(11,100); as.MAP_SET(0,10,11);
        as.load_const(10,2); as.load_const(11,200); as.MAP_SET(0,10,11);
        as.load_const(10,3); as.load_const(11,300); as.MAP_SET(0,10,11);
        // B = same entries inserted 3,1,2 (different order)
        as.MAP_NEW(1);
        as.load_const(10,3); as.load_const(11,300); as.MAP_SET(1,10,11);
        as.load_const(10,1); as.load_const(11,100); as.MAP_SET(1,10,11);
        as.load_const(10,2); as.load_const(11,200); as.MAP_SET(1,10,11);
        // C = {1:100, 2:200, 3:999} -- one value differs
        as.MAP_NEW(2);
        as.load_const(10,1); as.load_const(11,100); as.MAP_SET(2,10,11);
        as.load_const(10,2); as.load_const(11,200); as.MAP_SET(2,10,11);
        as.load_const(10,3); as.load_const(11,999); as.MAP_SET(2,10,11);
        // D = {1:100, 2:200} -- smaller
        as.MAP_NEW(3);
        as.load_const(10,1); as.load_const(11,100); as.MAP_SET(3,10,11);
        as.load_const(10,2); as.load_const(11,200); as.MAP_SET(3,10,11);
        as.R6(OpCode::EQ_DEEP, 4, 0, 1);   // order-independent equal -> true
        as.R6(OpCode::EQ_DEEP, 5, 0, 2);   // value differs -> false
        as.R6(OpCode::EQ_DEEP, 6, 0, 3);   // size differs -> false
        as.J(OpCode::HALT);
        std::cout << "Running Block E (maps, order-independent)...\n";
        try {
            auto res = execute(as.assemble(), &heap, nullptr, nullptr, 16);
            auto* r = res.get_reg_base();
            auto B = [&](int i, bool want){ return r[i].isBool() && r[i].asBool() == want; };
            const bool ok = B(4,true) && B(5,false) && B(6,false);
            std::cout << std::format("  map order-indep / value / size : {}\n", ok?"PASS":"FAIL");
            check(ok);
        } catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block F: same-pointer short-circuit + fresh-string content path ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.ALLOC(0, 1); as.load_const(10, 5); as.load_const(11, 0); as.ARRAY_SET(0, 11, 10); // r0 = [5]
        as.R6(OpCode::EQ_DEEP, 1, 0, 0);   // same object -> true
        // s1 interned, s2 = a FRESH copy (distinct pointer, same content) via a bytes round-trip.
        as.load_str(2, "hello");
        as.BYTES_FROM_STR(3, 2); as.BYTES_TO_STR(4, 3);   // r4 = fresh "hello"
        as.load_str(5, "world");
        as.R6(OpCode::EQ_DEEP, 6, 2, 4);   // "hello" == fresh "hello" -> true (content)
        as.R6(OpCode::EQ_DEEP, 7, 2, 5);   // "hello" == "world"       -> false
        as.J(OpCode::HALT);
        std::cout << "Running Block F (same-ptr + string content)...\n";
        try {
            StringInterner interner;
            auto res = execute(as.assemble(), &heap, nullptr, &interner, 16, nullptr, nullptr, &as.string_literals());
            auto* r = res.get_reg_base();
            auto B = [&](int i, bool want){ return r[i].isBool() && r[i].asBool() == want; };
            const bool ok = B(1,true) && B(6,true) && B(7,false);
            std::cout << std::format("  same-pointer -> true          : {}\n", B(1,true)?"PASS":"FAIL");
            std::cout << std::format("  fresh-string content equal    : {}\n", (B(6,true)&&B(7,false))?"PASS":"FAIL");
            check(ok);
        } catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block G: CYCLIC graphs -- answered, never refused ----
    // Two separately built self-cycles are equal; so is a self-cycle against a two-node ring
    // holding the same data (they unfold alike); a differing value is unequal. The last case is
    // the field-order guard: W{x, n} pushes `n` last, so the walk meets the cycle first and must
    // still come back to the differing `x`.
    {
        Heap heap(64 * 1024);
        Assembler as;
        const uint16_t Node = as.define_struct("Node", { "v", "next" });
        const uint16_t W    = as.define_struct("W",    { "x", "n" });
        as.label("main");
        auto node = [&](uint8_t dst, int16_t v) {
            as.NEW_STRUCT(dst, Node); as.load_const(10, v); as.SET_PROP(dst, as.field("Node","v"), 10);
        };
        const uint16_t NEXT = as.field("Node", "next");
        node(0, 1); as.SET_PROP(0, NEXT, 0);                          // r0: self-cycle, v=1
        node(1, 1); as.SET_PROP(1, NEXT, 1);                          // r1: another one
        node(2, 1); node(3, 1);
        as.SET_PROP(2, NEXT, 3); as.SET_PROP(3, NEXT, 2);             // r2 <-> r3: two-node ring, v=1
        node(4, 2); as.SET_PROP(4, NEXT, 4);                          // r4: self-cycle, v=2
        as.R6(OpCode::EQ_DEEP, 5, 0, 1);   // equal self-cycles        -> true
        as.R6(OpCode::EQ_DEEP, 6, 0, 2);   // self-cycle vs 2-ring     -> true (unfold alike)
        as.R6(OpCode::EQ_DEEP, 7, 0, 4);   // v=1 vs v=2               -> false
        as.NEW_STRUCT(8, W); as.load_const(10, 1); as.SET_PROP(8, as.field("W","x"), 10); as.SET_PROP(8, as.field("W","n"), 0);
        as.NEW_STRUCT(9, W); as.load_const(10, 2); as.SET_PROP(9, as.field("W","x"), 10); as.SET_PROP(9, as.field("W","n"), 1);
        as.R6(OpCode::EQ_DEEP, 11, 8, 9);  // W{x:1,n:cycle} vs W{x:2,n:cycle} -> false
        as.J(OpCode::HALT);
        std::cout << "Running Block G (cyclic graphs)...\n";
        try {
            auto res = execute(as.assemble(), &heap, nullptr, nullptr, 16, nullptr, &as.struct_types());
            auto* r = res.get_reg_base();
            auto B = [&](int i, bool want){ return r[i].isBool() && r[i].asBool() == want; };
            const bool ok = B(5,true) && B(6,true) && B(7,false) && B(11,false);
            std::cout << std::format("  equal self-cycles -> true         : {}\n", B(5,true)?"PASS":"FAIL");
            std::cout << std::format("  self-cycle vs 2-ring -> true      : {}\n", B(6,true)?"PASS":"FAIL");
            std::cout << std::format("  differing cycles -> false         : {}\n", B(7,false)?"PASS":"FAIL");
            std::cout << std::format("  difference behind a cycle -> false: {}\n", B(11,false)?"PASS":"FAIL");
            check(ok);
        } catch (const std::exception& e) { record_fail(e.what()); }
    }
}

// =============================================================================
// test_get_type_id -- GET_TYPE_ID reads a struct's runtime type id into an Int
// register (the primitive a multi-type `match` dispatches on), and yields the -1
// sentinel for any non-struct operand.
// =============================================================================
inline void test_get_type_id() {
    std::cout << "=== get_type_id ===\n";

    Heap heap(64 * 1024);

    Assembler as;
    const uint16_t Point  = as.define_struct("Point",  { "x", "y" });
    const uint16_t Circle = as.define_struct("Circle", { "r" });

    as.label("main");
    as.NEW_STRUCT(0, Point);              // r0 = Point
    as.NEW_STRUCT(1, Circle);             // r1 = Circle
    as.load_const(2, 42);                 // r2 = Int (not a struct)
    as.load_constant(3, Value::fromNil());// r3 = nil (not a struct)
    as.GET_TYPE_ID(4, 0);                 // r4 = type id of Point   -> Point
    as.GET_TYPE_ID(5, 1);                 // r5 = type id of Circle  -> Circle
    as.GET_TYPE_ID(6, 2);                 // r6 = type id of Int     -> -1
    as.GET_TYPE_ID(7, 3);                 // r7 = type id of nil     -> -1
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main");
    dis.print();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, nullptr, 0, &as.constant_pool(), &as.struct_types());
        auto* regs = res.get_reg_base();

        const bool ok =
            regs[4].isInt() && regs[4].asSigned48() == static_cast<int64_t>(Point)  &&
            regs[5].isInt() && regs[5].asSigned48() == static_cast<int64_t>(Circle) &&
            regs[6].isInt() && regs[6].asSigned48() == -1 &&
            regs[7].isInt() && regs[7].asSigned48() == -1 &&
            Point != Circle;

        std::cout << std::format("  Point id={} Circle id={} Int->{} nil->{}\n",
                                 regs[4].asSigned48(), regs[5].asSigned48(),
                                 regs[6].asSigned48(), regs[7].asSigned48());
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_i2d -- I2D widens a 48-bit Int to a double. The coercion the static front
// end emits at an N1 boundary (an Int value flowing into a Double context).
// =============================================================================
inline void test_i2d() {
    std::cout << "=== i2d ===\n";

    Heap heap(64 * 1024);

    Assembler as;
    as.label("main");
    as.load_const(0, 42);                 // r0 = Int 42
    as.I2D(1, 0);                         // r1 = Double 42.0
    as.load_const(2, -7);                 // r2 = Int -7
    as.I2D(3, 2);                         // r3 = Double -7.0
    as.load_const(4, 0);                  // r4 = Int 0
    as.I2D(5, 4);                         // r5 = Double 0.0
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main");
    dis.print();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, nullptr, 0, &as.constant_pool());
        auto* regs = res.get_reg_base();

        const bool ok =
            regs[1].isDouble() && regs[1].asDouble() == 42.0 &&
            regs[3].isDouble() && regs[3].asDouble() == -7.0 &&
            regs[5].isDouble() && regs[5].asDouble() == 0.0;

        std::cout << std::format("  42->{} -7->{} 0->{}\n",
                                 regs[1].asDouble(), regs[3].asDouble(), regs[5].asDouble());
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_d2i -- D2I saturating Double->Int (the `toInt` builtin). Java semantics:
// NaN -> 0, +-inf / overflow -> MAX/MIN_48, else truncate toward zero. Total, no
// trap. The mirror of test_i2d.
// =============================================================================
inline void test_d2i() {
    std::cout << "=== d2i ===\n";

    Heap heap(64 * 1024);

    Assembler as;
    as.label("main");
    as.load_double(0, 3.9);            as.D2I(0, 0);   // 3
    as.load_double(1, -3.9);           as.D2I(1, 1);   // -3
    as.load_double(2, 0.0);            as.D2I(2, 2);   // 0
    as.load_double(3, 5.0);            as.D2I(3, 3);   // 5
    as.load_double(4, std::nan(""));   as.D2I(4, 4);   // NaN -> 0
    as.load_double(5, INFINITY);       as.D2I(5, 5);   // +inf -> MAX_48
    as.load_double(6, -INFINITY);      as.D2I(6, 6);   // -inf -> MIN_48
    as.load_double(7, 1e20);           as.D2I(7, 7);   // overflow -> MAX_48
    as.load_double(8, -1e20);          as.D2I(8, 8);   // underflow -> MIN_48
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main");
    dis.print();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, nullptr, 0, &as.constant_pool());
        auto* regs = res.get_reg_base();

        constexpr int64_t MAX_48 =  140737488355327LL;   //  2^47 - 1
        constexpr int64_t MIN_48 = -140737488355328LL;   // -2^47
        const bool ok =
            regs[0].isInt() && regs[0].asSigned48() == 3 &&
            regs[1].isInt() && regs[1].asSigned48() == -3 &&
            regs[2].isInt() && regs[2].asSigned48() == 0 &&
            regs[3].isInt() && regs[3].asSigned48() == 5 &&
            regs[4].isInt() && regs[4].asSigned48() == 0 &&
            regs[5].isInt() && regs[5].asSigned48() == MAX_48 &&
            regs[6].isInt() && regs[6].asSigned48() == MIN_48 &&
            regs[7].isInt() && regs[7].asSigned48() == MAX_48 &&
            regs[8].isInt() && regs[8].asSigned48() == MIN_48;

        std::cout << std::format("  3.9->{} -3.9->{} nan->{} +inf->{} -inf->{}\n",
                                 regs[0].asSigned48(), regs[1].asSigned48(),
                                 regs[4].asSigned48(), regs[5].asSigned48(), regs[6].asSigned48());
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_dround -- DROUND rounds a Double by the MODE in the flags field:
// 0=floor 1=ceil 2=trunc 3=round(ties away) 4=roundHalfToEven(ties to even).
// The half-way cases separate mode 3 from mode 4 (2.5 -> 3 vs 2; 3.5 -> 4 vs 4).
// =============================================================================
inline void test_dround() {
    std::cout << "=== dround ===\n";

    Heap heap(64 * 1024);

    Assembler as;
    as.label("main");
    as.load_double(0, 2.7);            as.DROUND(0, 0, 0);   // floor -> 2
    as.load_double(1, -2.3);           as.DROUND(1, 1, 0);   // floor -> -3
    as.load_double(2, 2.3);            as.DROUND(2, 2, 1);   // ceil  -> 3
    as.load_double(3, -2.7);           as.DROUND(3, 3, 1);   // ceil  -> -2
    as.load_double(4, 2.7);            as.DROUND(4, 4, 2);   // trunc -> 2
    as.load_double(5, -2.7);           as.DROUND(5, 5, 2);   // trunc -> -2
    as.load_double(6, 2.5);            as.DROUND(6, 6, 3);   // round -> 3  (away)
    as.load_double(7, -2.5);           as.DROUND(7, 7, 3);   // round -> -3 (away)
    as.load_double(8, 2.4);            as.DROUND(8, 8, 3);   // round -> 2
    as.load_double(9, 2.5);            as.DROUND(9, 9, 4);   // even  -> 2
    as.load_double(10, 3.5);           as.DROUND(10, 10, 4); // even  -> 4
    as.load_double(11, -2.5);          as.DROUND(11, 11, 4); // even  -> -2
    as.load_double(12, std::nan(""));  as.DROUND(12, 12, 0); // NaN passes through
    as.load_double(13, INFINITY);      as.DROUND(13, 13, 1); // +inf passes through
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();

    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main");
    dis.print();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, nullptr, 0, &as.constant_pool());
        auto* r    = res.get_reg_base();

        auto isD = [](const Value& v, double x) { return v.isDouble() && v.asDouble() == x; };
        const bool ok =
            isD(r[0], 2.0) && isD(r[1], -3.0) &&
            isD(r[2], 3.0) && isD(r[3], -2.0) &&
            isD(r[4], 2.0) && isD(r[5], -2.0) &&
            isD(r[6], 3.0) && isD(r[7], -3.0) && isD(r[8], 2.0) &&
            isD(r[9], 2.0) && isD(r[10], 4.0) && isD(r[11], -2.0) &&
            (r[12].isDouble() && r[12].asDouble() != r[12].asDouble()) &&   // NaN
            (r[13].isDouble() && r[13].asDouble() == INFINITY);

        std::cout << std::format("  floor(2.7)={} ceil(-2.7)={} round(2.5)={} even(2.5)={} even(3.5)={}\n",
                                 r[0].asDouble(), r[3].asDouble(), r[6].asDouble(),
                                 r[9].asDouble(), r[10].asDouble());
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_get_kind -- GET_KIND reads the heap-object kind byte, distinguishing an
// ARRAY from a MAP (both non-structs, so GET_TYPE_ID reports -1 for each). The
// primitive the compiler's `for … in …` lowering dispatches on. Also -1 for a
// non-heap value.
// =============================================================================
inline void test_get_kind() {
    std::cout << "=== get_kind ===\n";

    Heap heap(64 * 1024);

    Assembler as;
    const uint16_t Point = as.define_struct("Point", { "x", "y" });

    as.label("main");
    as.ALLOC(0, 2);                        // r0 = array   -> KIND_ARRAY (0)
    as.NEW_STRUCT(1, Point);               // r1 = struct  -> KIND_OBJECT (2)
    as.MAP_NEW(2);                         // r2 = map     -> KIND_MAP (4)
    as.load_const(3, 42);                  // r3 = Int     -> -1
    as.load_constant(4, Value::fromNil()); // r4 = nil     -> -1
    as.GET_KIND(5, 0);
    as.GET_KIND(6, 1);
    as.GET_KIND(7, 2);
    as.GET_KIND(8, 3);
    as.GET_KIND(9, 4);
    as.J(OpCode::HALT);

    const auto bytecode = as.assemble();
    const uint8_t top = 10;                // scan r0..r9 (heap roots held across MAP_NEW alloc)

    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main");
    dis.print();

    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, &heap, nullptr, nullptr, top, &as.constant_pool(), &as.struct_types());
        auto* regs = res.get_reg_base();

        const bool ok =
            regs[5].isInt() && regs[5].asSigned48() == GcObject::KIND_ARRAY  &&
            regs[6].isInt() && regs[6].asSigned48() == GcObject::KIND_OBJECT &&
            regs[7].isInt() && regs[7].asSigned48() == GcObject::KIND_MAP    &&
            regs[8].isInt() && regs[8].asSigned48() == -1 &&
            regs[9].isInt() && regs[9].asSigned48() == -1 &&
            GcObject::KIND_ARRAY != GcObject::KIND_MAP;

        std::cout << std::format("  array->{} struct->{} map->{} int->{} nil->{}\n",
                                 regs[5].asSigned48(), regs[6].asSigned48(), regs[7].asSigned48(),
                                 regs[8].asSigned48(), regs[9].asSigned48());
        check(ok);
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_struct_gc -- the moving-GC guard for KIND_OBJECT. A struct's heap-pointer
// FIELD must be forwarded through the Cheney scan of the object payload.
//
//   Box{ inner } is built at top level (r0); its `inner` field is set to a heap
//   array (r1), then r1 is CLOBBERED so `inner` is reachable ONLY through the
//   struct field. A collection is forced mid-call (native_collect): the struct
//   survives via the top-level register root and -- crucially -- its `inner`
//   field survives only because the collector scans/forwards the object payload.
//   A dead canary is the ONLY thing that may be reclaimed, so freed == its size.
// =============================================================================
inline void test_struct_gc() {
    std::cout << "=== struct_gc ===\n";

    constexpr uint8_t COLLECTOR_FRAME = 3; // collector uses r1(result), r2(fn ptr)

    Heap heap(64 * 1024);
    GlobalEnv globals(heap);
    const uint8_t g_freed = globals.define();

    // Dead canary: allocated but never rooted -- must be the ONLY thing collected.
    GcObject* dead = heap.alloc(GcObject::KIND_ARRAY, 2);
    assert(dead && "test_struct_gc: dead canary alloc failed");
    const size_t dead_total = dead->total_bytes();

    Assembler as;
    const uint16_t Box = as.define_struct("Box", { "inner" });
    as.func("collector", COLLECTOR_FRAME);

    as.label("main");
    as.NEW_STRUCT(0, Box);                          // r0 = Box (top-level, live across CALL+GC)
    as.ALLOC(1, 4);                                 // r1 = inner array[4]
    as.SET_PROP(0, as.field("Box", "inner"), 1);    // r0.inner = r1
    as.load_const(1, 0);                            // clobber r1: inner reachable ONLY via r0.inner
    as.CALL("collector");
    as.J(OpCode::HALT);

    as.label("collector");
    as.call_native_id(1, 2, 2, 0, 0);               // r1 = collect() (id 0) -> runs GC
    as.STORE_GLOBAL(g_freed, 1);
    as.J(OpCode::RET);

    const auto bytecode = as.assemble();
    // Top-level frame size = COLLECTOR_FRAME: scans r0..r2 as roots (r0=Box) and the
    // slide opens collector's window at r[COLLECTOR_FRAME], leaving r0 untouched.
    const uint8_t top = COLLECTOR_FRAME;

    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main");
    dis.print();

    std::cout << "Running...\n";
    try {
        auto res   = execute_gc(bytecode, &heap, &globals, top, &as.struct_types());
        auto* regs = res.get_reg_base();

        const Value box_val = regs[0];
        GcObject*   box     = box_val.isPtr() ? GcObject::from_slots(box_val.asPtr()) : nullptr;

        const bool box_ok = box &&
                            box->kind == GcObject::KIND_OBJECT &&
                            box->slot_count() == 1 &&
                            box->object_type_id() == Box;

        GcObject* inner = nullptr;
        if (box) {
            const Value inner_val = box->slots()[0];
            inner = inner_val.isPtr() ? GcObject::from_slots(inner_val.asPtr()) : nullptr;
        }
        const bool inner_ok = inner &&
                              inner->kind == GcObject::KIND_ARRAY &&
                              inner->slot_count() == 4;

        const Value freed    = globals.get(g_freed);
        const bool  freed_ok = freed.isInt() &&
                               static_cast<size_t>(freed.asSigned48()) == dead_total;

        const bool ok = box_ok && inner_ok && freed_ok;

        std::cout << std::format("freed={} (expect dead_total={})\n",
            freed.isInt() ? freed.asSigned48() : -1, dead_total);
        std::cout << std::format("  struct survived collection:          {}\n", box_ok ? "PASS" : "FAIL");
        std::cout << std::format("  heap field forwarded via payload:    {}\n", inner_ok ? "PASS" : "FAIL");
        std::cout << std::format("  only the dead canary was collected:  {}\n", freed_ok ? "PASS" : "FAIL");
        check(ok);

        globals.set(g_freed, Value{});
    }
    catch (const std::exception& e) {
        record_fail(e.what());
    }
}

// =============================================================================
// test_array -- fixed-size arrays: ARRAY_GET/ARRAY_SET/LEN/ANEW/AFILL + the empty
// array + the out-of-bounds SEH trap + a heap value stored in an array surviving a
// collection. KIND_ARRAY storage already existed (ALLOC); these opcodes add
// random access on top of it.
// =============================================================================
inline void test_array() {
    std::cout << "=== array (ARRAY_GET/SET/LEN/ANEW/AFILL) ===\n";

    // ---- Part A: core get/set/len/anew/afill/empty (one program) ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.ALLOC(0, 3);                       // r0 = array(3)
        as.load_const(2, 10); as.load_const(1, 0); as.ARRAY_SET(0, 1, 2); // a[0] = 10
        as.load_const(2, 20); as.load_const(1, 1); as.ARRAY_SET(0, 1, 2); // a[1] = 20
        as.load_const(2, 30); as.load_const(1, 2); as.ARRAY_SET(0, 1, 2); // a[2] = 30
        as.load_const(1, 0); as.ARRAY_GET(3, 0, 1);   // r3 = a[0]
        as.load_const(1, 1); as.ARRAY_GET(4, 0, 1);   // r4 = a[1]
        as.load_const(1, 2); as.ARRAY_GET(5, 0, 1);   // r5 = a[2]
        as.LEN(6, 0);                          // r6 = len(a) = 3
        as.load_const(7, 5); as.ANEW(8, 7);    // r8 = array(5) (dynamic count)
        as.LEN(9, 8);                          // r9 = len = 5
        as.ALLOC(10, 3); as.load_const(11, 7); as.AFILL(10, 11); // b = array(3), fill 7
        as.load_const(1, 0); as.ARRAY_GET(12, 10, 1); // r12 = b[0]
        as.load_const(1, 2); as.ARRAY_GET(13, 10, 1); // r13 = b[2]
        as.ALLOC(14, 0);                       // r14 = array(0) (empty)
        as.LEN(15, 14);                        // r15 = len = 0
        as.J(OpCode::HALT);

        const auto bytecode = as.assemble();
        std::cout << "Running Part A...\n";
        try {
            auto res   = execute(bytecode, &heap, nullptr, nullptr, 16);
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) {
                return regs[i].isInt() && regs[i].asSigned48() == want;
            };
            const bool ok = I(3, 10) && I(4, 20) && I(5, 30) && I(6, 3) &&
                            I(9, 5) && I(12, 7) && I(13, 7) && I(15, 0);
            std::cout << std::format("  get a[0..2]=10,20,30 : {}\n", (I(3,10)&&I(4,20)&&I(5,30)) ? "PASS" : "FAIL");
            std::cout << std::format("  len(a)=3             : {}\n", I(6, 3) ? "PASS" : "FAIL");
            std::cout << std::format("  anew(5) len=5        : {}\n", I(9, 5) ? "PASS" : "FAIL");
            std::cout << std::format("  afill 7 -> b[0],b[2] : {}\n", (I(12,7)&&I(13,7)) ? "PASS" : "FAIL");
            std::cout << std::format("  empty array len=0    : {}\n", I(15, 0) ? "PASS" : "FAIL");
            check(ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part B: out-of-bounds traps (positive over-index AND negative index) ----
    {
        auto oob_traps = [](int64_t idx) -> bool {
            Heap heap(64 * 1024);
            Assembler as;
            as.label("main");
            as.ALLOC(0, 2);                     // array(2), valid indices 0..1
            as.load_const(1, static_cast<int16_t>(idx));
            as.ARRAY_GET(2, 0, 1);              // a[idx] -> must trap
            as.J(OpCode::HALT);
            const auto bc = as.assemble();
            try { execute(bc, &heap, nullptr, nullptr, 3); return false; }
            catch (const std::exception& e) {
                return std::string_view(e.what()).find("out of bounds") != std::string_view::npos;
            }
        };
        std::cout << "Running Part B (OOB)...\n";
        const bool over_ok = oob_traps(5);
        const bool neg_ok  = oob_traps(-1);
        std::cout << std::format("  a[5] on len-2 traps  : {}\n", over_ok ? "PASS" : "FAIL");
        std::cout << std::format("  a[-1] traps          : {}\n", neg_ok ? "PASS" : "FAIL");
        check(over_ok && neg_ok);
    }

    // ---- Part C: a struct stored in an array survives a mid-call collection ----
    {
        constexpr uint8_t COLLECTOR_FRAME = 3;
        Heap heap(64 * 1024);
        GlobalEnv globals(heap);

        Assembler as;
        const uint16_t Point = as.define_struct("Point", { "x" });
        as.func("collector", COLLECTOR_FRAME);

        as.label("main");
        as.ALLOC(0, 1);                                 // r0 = array(1)  [survivor]
        as.NEW_STRUCT(1, Point);                        // r1 = Point (temp)
        as.load_const(2, 99); as.SET_PROP(1, as.field("Point", "x"), 2); // Point.x = 99
        as.load_const(2, 0); as.ARRAY_SET(0, 2, 1);     // a[0] = Point
        as.load_const(1, 0); as.load_const(2, 0);       // clobber r1/r2: Point only via a[0]
        as.CALL("collector");
        as.J(OpCode::HALT);

        as.label("collector");
        as.call_native_id(1, 2, 2, 0, 0);               // run GC mid-call (id 0 = native_collect)
        as.J(OpCode::RET);

        const auto bytecode = as.assemble();
        std::cout << "Running Part C (GC root)...\n";
        try {
            auto res   = execute_gc(bytecode, &heap, &globals, COLLECTOR_FRAME, &as.struct_types());
            auto* regs = res.get_reg_base();
            const Value arr_val = regs[0];
            GcObject*   arr     = arr_val.isPtr() ? GcObject::from_slots(arr_val.asPtr()) : nullptr;
            const bool  arr_ok  = arr && arr->kind == GcObject::KIND_ARRAY && arr->slot_count() == 1;

            GcObject* pt = nullptr;
            if (arr) {
                const Value pv = arr->slots()[0];
                pt = pv.isPtr() ? GcObject::from_slots(pv.asPtr()) : nullptr;
            }
            const bool pt_ok = pt && pt->kind == GcObject::KIND_OBJECT &&
                               pt->object_type_id() == Point &&
                               pt->slots()[0].isInt() && pt->slots()[0].asSigned48() == 99;

            std::cout << std::format("  array survived collection      : {}\n", arr_ok ? "PASS" : "FAIL");
            std::cout << std::format("  struct in slot forwarded (x=99): {}\n", pt_ok ? "PASS" : "FAIL");
            check(arr_ok && pt_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }
}

// =============================================================================
// test_vec -- the growable vector type (KIND_VEC): VEC_NEW / VEC_PUSH / VEC_POP,
// the grow (doubling) path, empty-pop -> Nil, and a struct pushed into a vector
// surviving a mid-call collection (header + backing scanned as GC roots). Slice 1
// reads back via VEC_POP + the header slots directly (v[i]/len land in Slice 2).
// The vector header payload is Value[2] = {backing (slot 0), count (slot 1)}.
// =============================================================================
inline void test_vec() {
    std::cout << "=== vec (VEC_NEW/PUSH/POP + grow + GC) ===\n";
    constexpr uint32_t VEC_BACKING = 0;   // op_vec.h VEC_SLOT_BACKING
    constexpr uint32_t VEC_COUNT   = 1;   // op_vec.h VEC_SLOT_COUNT

    // ---- Part A: push 10 (forces one grow 8->16), pop the last three LIFO ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.VEC_NEW(0);                                  // r0 = vec()
        for (int16_t k = 0; k < 10; ++k) {              // push 0..9 (9th push grows 8->16)
            as.load_const(1, k);
            as.VEC_PUSH(0, 1);
        }
        as.VEC_POP(2, 0);                               // r2 = 9
        as.VEC_POP(3, 0);                               // r3 = 8
        as.VEC_POP(4, 0);                               // r4 = 7
        as.J(OpCode::HALT);

        const auto bytecode = as.assemble();
        std::cout << "Running Part A...\n";
        try {
            auto res   = execute(bytecode, &heap, nullptr, nullptr, 8);
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) {
                return regs[i].isInt() && regs[i].asSigned48() == want;
            };
            const Value  vv  = regs[0];
            GcObject*    vec = vv.isPtr() ? GcObject::from_slots(vv.asPtr()) : nullptr;
            const bool   kind_ok = vec && vec->kind == GcObject::KIND_VEC;
            int64_t      count   = kind_ok ? vec->slots()[VEC_COUNT].asSigned48() : -1;
            GcObject*    backing = kind_ok ? GcObject::from_slots(vec->slots()[VEC_BACKING].asPtr()) : nullptr;
            const bool   cap_ok  = backing && backing->kind == GcObject::KIND_ARRAY &&
                                   backing->slot_count() == 16;   // grew once
            const bool   pop_ok  = I(2, 9) && I(3, 8) && I(4, 7);
            const bool   cnt_ok  = count == 7;                    // 10 pushed - 3 popped
            std::cout << std::format("  kind == KIND_VEC     : {}\n", kind_ok ? "PASS" : "FAIL");
            std::cout << std::format("  pop LIFO 9,8,7       : {}\n", pop_ok ? "PASS" : "FAIL");
            std::cout << std::format("  count == 7           : {}\n", cnt_ok ? "PASS" : "FAIL");
            std::cout << std::format("  backing grew to 16   : {}\n", cap_ok ? "PASS" : "FAIL");
            check(kind_ok && pop_ok && cnt_ok && cap_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part B: pop on an empty vector -> Nil, count stays 0 ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.VEC_NEW(0);
        as.VEC_POP(1, 0);                               // r1 = Nil (empty)
        as.J(OpCode::HALT);
        const auto bytecode = as.assemble();
        std::cout << "Running Part B (empty pop)...\n";
        try {
            auto res   = execute(bytecode, &heap, nullptr, nullptr, 4);
            auto* regs = res.get_reg_base();
            GcObject*  vec = regs[0].isPtr() ? GcObject::from_slots(regs[0].asPtr()) : nullptr;
            const bool nil_ok = regs[1].isNil();
            const bool cnt_ok = vec && vec->kind == GcObject::KIND_VEC &&
                                vec->slots()[VEC_COUNT].asSigned48() == 0;
            std::cout << std::format("  empty pop -> nil     : {}\n", nil_ok ? "PASS" : "FAIL");
            std::cout << std::format("  count still 0        : {}\n", cnt_ok ? "PASS" : "FAIL");
            check(nil_ok && cnt_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part C: a struct pushed into a vector survives a mid-call collection ----
    {
        constexpr uint8_t COLLECTOR_FRAME = 3;
        Heap heap(64 * 1024);
        GlobalEnv globals(heap);

        Assembler as;
        const uint16_t Point = as.define_struct("Point", { "x" });
        as.func("collector", COLLECTOR_FRAME);

        as.label("main");
        as.VEC_NEW(0);                                  // r0 = vec()  [survivor]
        as.NEW_STRUCT(1, Point);                        // r1 = Point (temp)
        as.load_const(2, 99); as.SET_PROP(1, as.field("Point", "x"), 2); // Point.x = 99
        as.VEC_PUSH(0, 1);                              // vec.push(Point)
        as.load_const(1, 0); as.load_const(2, 0);       // clobber: Point only via the vec
        as.CALL("collector");
        as.J(OpCode::HALT);

        as.label("collector");
        as.call_native_id(1, 2, 2, 0, 0);               // run GC mid-call (id 0 = native_collect)
        as.J(OpCode::RET);

        const auto bytecode = as.assemble();
        std::cout << "Running Part C (GC root)...\n";
        try {
            auto res   = execute_gc(bytecode, &heap, &globals, COLLECTOR_FRAME, &as.struct_types());
            auto* regs = res.get_reg_base();
            GcObject*  vec = regs[0].isPtr() ? GcObject::from_slots(regs[0].asPtr()) : nullptr;
            const bool vec_ok = vec && vec->kind == GcObject::KIND_VEC &&
                                vec->slots()[VEC_COUNT].asSigned48() == 1;
            GcObject*  pt = nullptr;
            if (vec_ok) {
                GcObject*   backing = GcObject::from_slots(vec->slots()[VEC_BACKING].asPtr());
                const Value pv      = backing->slots()[0];
                pt = pv.isPtr() ? GcObject::from_slots(pv.asPtr()) : nullptr;
            }
            const bool pt_ok = pt && pt->kind == GcObject::KIND_OBJECT &&
                               pt->object_type_id() == Point &&
                               pt->slots()[0].isInt() && pt->slots()[0].asSigned48() == 99;
            std::cout << std::format("  vector survived collection     : {}\n", vec_ok ? "PASS" : "FAIL");
            std::cout << std::format("  struct in vec forwarded (x=99) : {}\n", pt_ok ? "PASS" : "FAIL");
            check(vec_ok && pt_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part D: dual-kind ARRAY_GET / ARRAY_SET / LEN over a KIND_VEC ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.VEC_NEW(0);
        as.load_const(1, 5); as.VEC_PUSH(0, 1);
        as.load_const(1, 6); as.VEC_PUSH(0, 1);
        as.load_const(1, 7); as.VEC_PUSH(0, 1);         // v = [5, 6, 7]
        as.LEN(2, 0);                                   // r2 = len(v) = 3
        as.load_const(1, 1); as.ARRAY_GET(3, 0, 1);     // r3 = v[1] = 6
        as.load_const(4, 42); as.load_const(1, 0); as.ARRAY_SET(0, 1, 4); // v[0] = 42
        as.load_const(1, 0); as.ARRAY_GET(5, 0, 1);     // r5 = v[0] = 42
        as.J(OpCode::HALT);
        const auto bytecode = as.assemble();
        std::cout << "Running Part D (v[i]/len)...\n";
        try {
            auto res   = execute(bytecode, &heap, nullptr, nullptr, 8);
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) {
                return regs[i].isInt() && regs[i].asSigned48() == want;
            };
            const bool len_ok = I(2, 3);
            const bool get_ok = I(3, 6);
            const bool set_ok = I(5, 42);
            std::cout << std::format("  len(v) == 3          : {}\n", len_ok ? "PASS" : "FAIL");
            std::cout << std::format("  v[1] == 6            : {}\n", get_ok ? "PASS" : "FAIL");
            std::cout << std::format("  v[0]=42 then read    : {}\n", set_ok ? "PASS" : "FAIL");
            check(len_ok && get_ok && set_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part E: bounds are vs count, NOT the backing capacity ----
    // v holds 3 elements in a capacity-8 backing; v[3] must trap even though slot 3
    // physically exists in the backing.
    {
        auto vec_oob_traps = [](int64_t idx) -> bool {
            Heap heap(64 * 1024);
            Assembler as;
            as.label("main");
            as.VEC_NEW(0);
            as.load_const(1, 5); as.VEC_PUSH(0, 1);
            as.load_const(1, 6); as.VEC_PUSH(0, 1);
            as.load_const(1, 7); as.VEC_PUSH(0, 1);     // count 3, capacity 8
            as.load_const(1, static_cast<int16_t>(idx));
            as.ARRAY_GET(2, 0, 1);                      // v[idx] -> must trap
            as.J(OpCode::HALT);
            const auto bc = as.assemble();
            try { execute(bc, &heap, nullptr, nullptr, 4); return false; }
            catch (const std::exception& e) {
                return std::string_view(e.what()).find("out of bounds") != std::string_view::npos;
            }
        };
        std::cout << "Running Part E (bounds vs count)...\n";
        const bool over_ok = vec_oob_traps(3);          // within capacity, past count
        const bool neg_ok  = vec_oob_traps(-1);
        std::cout << std::format("  v[3] (count 3) traps : {}\n", over_ok ? "PASS" : "FAIL");
        std::cout << std::format("  v[-1] traps          : {}\n", neg_ok ? "PASS" : "FAIL");
        check(over_ok && neg_ok);
    }

    // ---- Part F: VEC_NEW_CAP -- a capacity hint (count 0), honored, no regrow within it;
    // capacity 0 still grows on the first push (the vec_grow zero floor). ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.load_const(1, 20); as.VEC_NEW_CAP(0, 1);     // r0 = capacity-20 vec, count 0
        as.load_const(1, 5); as.VEC_PUSH(0, 1);
        as.load_const(1, 6); as.VEC_PUSH(0, 1);
        as.load_const(1, 7); as.VEC_PUSH(0, 1);         // 3 pushes: count 3, backing stays 20
        as.load_const(2, 0); as.VEC_NEW_CAP(3, 2);      // r3 = capacity-0 vec
        as.load_const(2, 9); as.VEC_PUSH(3, 2);         // first push grows 0 -> VEC_INITIAL_CAP (8)
        as.J(OpCode::HALT);
        const auto bytecode = as.assemble();
        std::cout << "Running Part F (VEC_NEW_CAP capacity hint)...\n";
        try {
            auto  res  = execute(bytecode, &heap, nullptr, nullptr, 8);
            auto* regs = res.get_reg_base();
            bool capped_ok = false, count_ok = false, no_regrow = false, zero_ok = false;
            if (regs[0].isPtr()) {
                GcObject* v = GcObject::from_slots(regs[0].asPtr());
                if (v->kind == GcObject::KIND_VEC) {
                    GcObject* b = GcObject::from_slots(v->slots()[VEC_BACKING].asPtr());
                    capped_ok = true;
                    count_ok  = v->slots()[VEC_COUNT].asSigned48() == 3;
                    no_regrow = b->slot_count() == 20;   // capacity 20 honored; 3 pushes didn't grow
                }
            }
            if (regs[3].isPtr()) {
                GcObject* v = GcObject::from_slots(regs[3].asPtr());
                if (v->kind == GcObject::KIND_VEC) {
                    GcObject* b = GcObject::from_slots(v->slots()[VEC_BACKING].asPtr());
                    zero_ok = v->slots()[VEC_COUNT].asSigned48() == 1 && b->slot_count() == 8;
                }
            }
            std::cout << std::format("  cap 20 honored       : {}\n", (capped_ok && no_regrow) ? "PASS" : "FAIL");
            std::cout << std::format("  count 3 (len 0 init) : {}\n", count_ok ? "PASS" : "FAIL");
            std::cout << std::format("  cap 0 grows on push  : {}\n", zero_ok ? "PASS" : "FAIL");
            check(capped_ok && no_regrow && count_ok && zero_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part G: a negative VEC_NEW_CAP capacity is a located trap ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.load_const(1, -1); as.VEC_NEW_CAP(0, 1);
        as.J(OpCode::HALT);
        const auto bc = as.assemble();
        std::cout << "Running Part G (negative capacity traps)...\n";
        bool trap_ok = false;
        try { execute(bc, &heap, nullptr, nullptr, 2); }
        catch (const std::exception& e) {
            trap_ok = std::string_view(e.what()).find("capacity") != std::string_view::npos;
        }
        std::cout << std::format("  vec(-1) traps        : {}\n", trap_ok ? "PASS" : "FAIL");
        check(trap_ok);
    }
}

// =============================================================================
// test_bytes -- the growable byte buffer (KIND_BYTES): BYTES_NEW_CAP, the tri-kind
// push/pop/index/len (a byte reads/writes as an Int 0..255, masked on write), the
// grow + memcpy path interleaved with the moving collector (the KIND_STRING backing
// must forward with its bytes intact), GET_KIND == 6, and the bounds-vs-count trap.
// The string bridges toBytes/fromBytes and the hex toString are covered end-to-end
// in static_compiler_tests (strings are natural there). See op_bytes.h.
// =============================================================================
inline void test_bytes() {
    std::cout << "=== bytes (KIND_BYTES: BYTES_NEW_CAP/push/pop/index/len + grow + GC) ===\n";
    constexpr uint32_t BYTES_BACKING = 0;   // op_bytes.h BYTES_SLOT_BACKING
    constexpr uint32_t BYTES_COUNT   = 1;   // op_bytes.h BYTES_SLOT_COUNT

    // ---- Part A: bytes(4), push 5 (forces grow 4->8), push masking, index get/set, pop ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.load_const(9, 4);
        as.BYTES_NEW_CAP(0, 9);                         // r0 = bytes(4)
        const int16_t vals[5] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x1FF }; // last is masked -> 0xFF
        for (int k = 0; k < 5; ++k) { as.load_const(1, vals[k]); as.VEC_PUSH(0, 1); }
        as.load_const(2, 0); as.ARRAY_GET(3, 0, 2);     // r3 = b[0] = 0xDE
        as.load_const(2, 4); as.ARRAY_GET(4, 0, 2);     // r4 = b[4] = 0xFF (pushed 0x1FF, masked)
        as.load_const(5, 0x122); as.load_const(2, 1); as.ARRAY_SET(0, 2, 5); // b[1] = 0x122 & 0xFF
        as.load_const(2, 1); as.ARRAY_GET(6, 0, 2);     // r6 = b[1] = 0x22
        as.LEN(7, 0);                                   // r7 = 5
        as.VEC_POP(8, 0);                               // r8 = 0xFF (last), count -> 4
        as.J(OpCode::HALT);

        const auto bytecode = as.assemble();
        std::cout << "Running Part A...\n";
        try {
            auto res   = execute(bytecode, &heap, nullptr, nullptr, 10);
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) { return regs[i].isInt() && regs[i].asSigned48() == want; };
            const Value  bv  = regs[0];
            GcObject*    buf = bv.isPtr() ? GcObject::from_slots(bv.asPtr()) : nullptr;
            const bool   kind_ok = buf && buf->kind == GcObject::KIND_BYTES;
            const int64_t count  = kind_ok ? buf->slots()[BYTES_COUNT].asSigned48() : -1;
            GcObject*    backing = kind_ok ? GcObject::from_slots(buf->slots()[BYTES_BACKING].asPtr()) : nullptr;
            const bool   back_ok = backing && backing->kind == GcObject::KIND_STRING &&
                                   backing->string_length() == 8;      // grew 4 -> 8
            const bool   get_ok  = I(3, 0xDE) && I(4, 0xFF) && I(6, 0x22);
            const bool   len_ok  = I(7, 5) && count == 4;              // 5 pushed, 1 popped
            const bool   pop_ok  = I(8, 0xFF);
            std::cout << std::format("  kind == KIND_BYTES     : {}\n", kind_ok ? "PASS" : "FAIL");
            std::cout << std::format("  push mask + index get  : {}\n", get_ok ? "PASS" : "FAIL");
            std::cout << std::format("  index set masked       : {}\n", I(6, 0x22) ? "PASS" : "FAIL");
            std::cout << std::format("  len 5 / count 4        : {}\n", len_ok ? "PASS" : "FAIL");
            std::cout << std::format("  pop == 0xFF            : {}\n", pop_ok ? "PASS" : "FAIL");
            std::cout << std::format("  backing grew to 8      : {}\n", back_ok ? "PASS" : "FAIL");
            check(kind_ok && get_ok && len_ok && pop_ok && back_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part B: pop on an empty buffer -> Nil, count stays 0 ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.load_const(1, 0); as.BYTES_NEW_CAP(0, 1);    // bytes(0)
        as.VEC_POP(1, 0);                               // Nil (empty)
        as.J(OpCode::HALT);
        const auto bytecode = as.assemble();
        std::cout << "Running Part B (empty pop)...\n";
        try {
            auto res   = execute(bytecode, &heap, nullptr, nullptr, 4);
            auto* regs = res.get_reg_base();
            GcObject*  buf = regs[0].isPtr() ? GcObject::from_slots(regs[0].asPtr()) : nullptr;
            const bool nil_ok = regs[1].isNil();
            const bool cnt_ok = buf && buf->kind == GcObject::KIND_BYTES &&
                                buf->slots()[BYTES_COUNT].asSigned48() == 0;
            std::cout << std::format("  empty pop -> nil     : {}\n", nil_ok ? "PASS" : "FAIL");
            std::cout << std::format("  count still 0        : {}\n", cnt_ok ? "PASS" : "FAIL");
            check(nil_ok && cnt_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part C: a byte buffer survives a mid-call collection with its bytes intact ----
    // Pushing 10 bytes into a bytes(2) forces grows 2->4->8->16; the collector must
    // forward the KIND_BYTES header AND its KIND_STRING backing (moved as opaque bytes).
    {
        constexpr uint8_t COLLECTOR_FRAME = 3;
        Heap heap(64 * 1024);
        GlobalEnv globals(heap);
        Assembler as;
        as.func("collector", COLLECTOR_FRAME);
        as.label("main");
        as.load_const(1, 2); as.BYTES_NEW_CAP(0, 1);    // r0 = bytes(2)  [survivor]
        for (int k = 0; k < 10; ++k) { as.load_const(1, static_cast<int16_t>(100 + k)); as.VEC_PUSH(0, 1); }
        as.load_const(1, 0);                            // clobber temp
        as.CALL("collector");
        as.J(OpCode::HALT);
        as.label("collector");
        as.call_native_id(1, 2, 2, 0, 0);               // run GC mid-call (id 0 = native_collect)
        as.J(OpCode::RET);
        const auto bytecode = as.assemble();
        std::cout << "Running Part C (GC root)...\n";
        try {
            auto res   = execute_gc(bytecode, &heap, &globals, COLLECTOR_FRAME);
            auto* regs = res.get_reg_base();
            GcObject*  buf = regs[0].isPtr() ? GcObject::from_slots(regs[0].asPtr()) : nullptr;
            const bool cnt_ok = buf && buf->kind == GcObject::KIND_BYTES &&
                                buf->slots()[BYTES_COUNT].asSigned48() == 10;
            bool bytes_ok = false;
            if (cnt_ok) {
                GcObject* backing = GcObject::from_slots(buf->slots()[BYTES_BACKING].asPtr());
                const unsigned char b0 = static_cast<unsigned char>(backing->bytes()[0]);
                const unsigned char b9 = static_cast<unsigned char>(backing->bytes()[9]);
                bytes_ok = backing->kind == GcObject::KIND_STRING && b0 == 100 && b9 == 109;
            }
            std::cout << std::format("  buffer survived collection     : {}\n", cnt_ok ? "PASS" : "FAIL");
            std::cout << std::format("  bytes intact (b[0]=100,b[9]=109): {}\n", bytes_ok ? "PASS" : "FAIL");
            check(cnt_ok && bytes_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part D: GET_KIND == 6 (KIND_BYTES) and dual-kind LEN ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.load_const(1, 3); as.BYTES_NEW_CAP(0, 1);
        as.load_const(1, 1); as.VEC_PUSH(0, 1);
        as.load_const(1, 2); as.VEC_PUSH(0, 1);         // count 2
        as.GET_KIND(2, 0);                              // r2 = 6
        as.LEN(3, 0);                                   // r3 = 2
        as.J(OpCode::HALT);
        const auto bytecode = as.assemble();
        std::cout << "Running Part D (GET_KIND/len)...\n";
        try {
            auto res   = execute(bytecode, &heap, nullptr, nullptr, 4);
            auto* regs = res.get_reg_base();
            const bool kind_ok = regs[2].isInt() && regs[2].asSigned48() == GcObject::KIND_BYTES;
            const bool len_ok  = regs[3].isInt() && regs[3].asSigned48() == 2;
            std::cout << std::format("  GET_KIND == 6        : {}\n", kind_ok ? "PASS" : "FAIL");
            std::cout << std::format("  len == 2             : {}\n", len_ok ? "PASS" : "FAIL");
            check(kind_ok && len_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part E: bounds are vs count, NOT the backing capacity; negative traps ----
    {
        auto bytes_oob_traps = [](int64_t idx) -> bool {
            Heap heap(64 * 1024);
            Assembler as;
            as.label("main");
            as.load_const(1, 8); as.BYTES_NEW_CAP(0, 1);        // capacity 8, count 0
            as.load_const(1, 7); as.VEC_PUSH(0, 1);
            as.load_const(1, 8); as.VEC_PUSH(0, 1);             // count 2, capacity 8
            as.load_const(1, static_cast<int16_t>(idx));
            as.ARRAY_GET(2, 0, 1);                              // b[idx] -> must trap
            as.J(OpCode::HALT);
            const auto bc = as.assemble();
            try { execute(bc, &heap, nullptr, nullptr, 4); return false; }
            catch (const std::exception& e) {
                return std::string_view(e.what()).find("out of bounds") != std::string_view::npos;
            }
        };
        std::cout << "Running Part E (bounds vs count)...\n";
        const bool over_ok = bytes_oob_traps(2);        // within capacity, past count
        const bool neg_ok  = bytes_oob_traps(-1);
        std::cout << std::format("  b[2] (count 2) traps : {}\n", over_ok ? "PASS" : "FAIL");
        std::cout << std::format("  b[-1] traps          : {}\n", neg_ok ? "PASS" : "FAIL");
        check(over_ok && neg_ok);
    }
}

// =============================================================================
// test_bytes_append -- the bulk byte-blit ops BYTES_APPEND (whole String/Bytes) and
// BYTES_APPEND_RANGE (a Bytes sub-range). Covers value equality, growth across
// capacity, src == dst self-append, an empty source no-op, GC survival, and the
// receiver / bounds traps.
// =============================================================================
inline void test_bytes_append() {
    std::cout << "=== bytes_append (BYTES_APPEND / BYTES_APPEND_RANGE) ===\n";

    auto str_of = [](Value v) -> std::string {
        if (!v.isPtr()) return "<not-ptr>";
        GcObject* o = GcObject::from_slots(v.asPtr());
        if (o->kind != GcObject::KIND_STRING) return "<not-str>";
        return std::string(o->bytes(), o->string_length());
    };

    // ---- Part A: append whole String src + whole Bytes src + a Bytes sub-range ----
    {
        Heap heap(64 * 1024);
        StringInterner interner;
        Assembler as;
        as.label("main");
        as.load_const(1, 0); as.BYTES_NEW_CAP(0, 1);      // r0 = bytes()  (dst)
        as.load_str(2, "foo"); as.BYTES_APPEND(0, 2);     // dst += "foo"
        as.load_str(3, "BAR"); as.BYTES_FROM_STR(4, 3);   // r4 = bytes("BAR")
        as.BYTES_APPEND(0, 4);                            // dst += bytes("BAR")  -> "fooBAR"
        as.load_const(5, 1); as.load_const(6, 3);
        as.BYTES_APPEND_RANGE(0, 4, 5, 6);                // dst += r4[1,3) = "AR" -> "fooBARAR"
        as.BYTES_TO_STR(7, 0);                            // r7 = fromBytes(dst)
        as.J(OpCode::HALT);
        const auto bytecode = as.assemble();
        std::cout << "Running Part A (whole + whole + range)...\n";
        try {
            auto res = execute(bytecode, &heap, nullptr, &interner, 8, nullptr, nullptr,
                               &as.string_literals());
            const std::string got = str_of(res.get_reg_base()[7]);
            const bool ok = got == "fooBARAR";
            std::cout << std::format("  \"{}\" == fooBARAR : {}\n", got, ok ? "PASS" : "FAIL");
            check(ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part B: growth across capacity + src == dst self-append ----
    {
        Heap heap(64 * 1024);
        StringInterner interner;
        Assembler as;
        as.label("main");
        as.load_const(1, 2); as.BYTES_NEW_CAP(0, 1);      // r0 = bytes(2) (cap 2)
        as.load_str(2, "abcd"); as.BYTES_APPEND(0, 2);    // dst = "abcd" (grew 2->4)
        as.BYTES_APPEND(0, 0);                            // self-append -> "abcdabcd"
        as.BYTES_TO_STR(3, 0);
        as.J(OpCode::HALT);
        const auto bytecode = as.assemble();
        std::cout << "Running Part B (grow + self-append)...\n";
        try {
            auto res = execute(bytecode, &heap, nullptr, &interner, 4, nullptr, nullptr,
                               &as.string_literals());
            const std::string got = str_of(res.get_reg_base()[3]);
            const bool ok = got == "abcdabcd";
            std::cout << std::format("  \"{}\" == abcdabcd : {}\n", got, ok ? "PASS" : "FAIL");
            check(ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part C: empty source is a no-op (uses a fresh empty buffer as the source) ----
    {
        Heap heap(64 * 1024);
        StringInterner interner;
        Assembler as;
        as.label("main");
        as.load_const(1, 0); as.BYTES_NEW_CAP(0, 1);      // r0 = dst = bytes()
        as.load_str(2, "hi"); as.BYTES_APPEND(0, 2);      // dst = "hi"
        as.load_const(3, 0); as.BYTES_NEW_CAP(5, 3);      // r5 = empty bytes() (empty src)
        as.BYTES_APPEND(0, 5);                            // no-op
        as.BYTES_APPEND_RANGE(0, 5, 1, 1);                // r1 holds 0; [0,0) no-op
        as.BYTES_TO_STR(6, 0);
        as.J(OpCode::HALT);
        const auto bytecode = as.assemble();
        std::cout << "Running Part C (empty no-op)...\n";
        try {
            auto res = execute(bytecode, &heap, nullptr, &interner, 7, nullptr, nullptr,
                               &as.string_literals());
            const std::string got = str_of(res.get_reg_base()[6]);
            const bool ok = got == "hi";
            std::cout << std::format("  \"{}\" == hi (empty no-op) : {}\n", got, ok ? "PASS" : "FAIL");
            check(ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part D: a buffer built by BYTES_APPEND survives a mid-call collection ----
    {
        constexpr uint8_t COLLECTOR_FRAME = 3;
        Heap heap(64 * 1024);
        GlobalEnv globals(heap);
        StringInterner interner;
        Assembler as;
        as.func("collector", COLLECTOR_FRAME);
        as.label("main");
        as.load_const(1, 1); as.BYTES_NEW_CAP(0, 1);      // r0 = bytes(1)  [survivor]
        as.load_str(2, "abcdefghij"); as.BYTES_APPEND(0, 2);  // grows 1->..->16 via append
        as.load_const(2, 0);                              // clobber temp
        as.CALL("collector");
        as.BYTES_TO_STR(2, 0);
        as.J(OpCode::HALT);
        as.label("collector");
        as.call_native_id(1, 2, 2, 0, 0);                 // run GC mid-call (id 0 = native_collect)
        as.J(OpCode::RET);
        const auto bytecode = as.assemble();
        std::cout << "Running Part D (GC survival)...\n";
        try {
            // execute_gc() doesn't forward string_literals, so spell out the full call
            // (interner + string_literals for LOAD_STR, GC_NTAB for native_collect).
            auto res = execute(bytecode, &heap, &globals, &interner, COLLECTOR_FRAME,
                               nullptr, nullptr, &as.string_literals(), nullptr, nullptr,
                               nullptr, nullptr, 0, 0, nullptr, nullptr, nullptr, &GC_NTAB);
            const std::string got = str_of(res.get_reg_base()[2]);
            const bool ok = got == "abcdefghij";
            std::cout << std::format("  \"{}\" survived GC : {}\n", got, ok ? "PASS" : "FAIL");
            check(ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Part E: receiver + bounds traps ----
    {
        auto append_traps = [](bool range, int64_t lo, int64_t hi, bool bad_dst) -> bool {
            Heap heap(64 * 1024);
            StringInterner interner;
            Assembler as;
            as.label("main");
            as.load_const(1, 0); as.BYTES_NEW_CAP(0, 1);  // r0 = dst bytes()
            as.load_str(2, "ab"); as.BYTES_FROM_STR(3, 2);// r3 = bytes("ab"), len 2
            if (bad_dst) { as.load_const(0, 5); }         // r0 now an Int -> bad dst
            if (range) {
                as.load_const(4, static_cast<int16_t>(lo));
                as.load_const(5, static_cast<int16_t>(hi));
                as.BYTES_APPEND_RANGE(0, 3, 4, 5);
            } else {
                as.BYTES_APPEND(0, 3);
            }
            as.J(OpCode::HALT);
            const auto bc = as.assemble();
            try { execute(bc, &heap, nullptr, &interner, 6, nullptr, nullptr, &as.string_literals());
                  return false; }
            catch (const std::exception&) { return true; }
        };
        std::cout << "Running Part E (traps)...\n";
        const bool bad_dst_ok = append_traps(false, 0, 0, true);   // appendBytes into an Int
        const bool oob_hi_ok  = append_traps(true, 0, 3, false);   // hi 3 > len 2
        const bool oob_lo_ok  = append_traps(true, -1, 1, false);  // lo < 0
        std::cout << std::format("  bad dst traps        : {}\n", bad_dst_ok ? "PASS" : "FAIL");
        std::cout << std::format("  range hi>len traps   : {}\n", oob_hi_ok ? "PASS" : "FAIL");
        std::cout << std::format("  range lo<0 traps     : {}\n", oob_lo_ok ? "PASS" : "FAIL");
        check(bad_dst_ok && oob_hi_ok && oob_lo_ok);
    }
}

// =============================================================================
// test_map -- the dynamic map type (KIND_MAP): MAP_NEW / MAP_GET / MAP_SET, the
// dual-kind LEN, key identity (content strings + SameValueZero immediates), the
// grow/rehash path interleaved with the moving collector, and the invalid-key
// trap. Six blocks, each with its own Heap. See op_map.h.
// =============================================================================
inline void test_map() {
    std::cout << "=== map (MAP_NEW/GET/SET/LEN) ===\n";

    // ---- Block A: core int-key set / get / miss / overwrite / len ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.load_const(1, 1); as.load_const(2, 100); as.MAP_SET(0, 1, 2);   // m[1] = 100
        as.load_const(1, 2); as.load_const(2, 200); as.MAP_SET(0, 1, 2);   // m[2] = 200
        as.load_const(1, 1); as.MAP_GET(3, 0, 1);                          // r3 = m[1] = 100
        as.load_const(1, 2); as.MAP_GET(4, 0, 1);                          // r4 = m[2] = 200
        as.load_const(1, 9); as.MAP_GET(5, 0, 1);                          // r5 = m[9] = nil (miss)
        as.load_const(1, 1); as.load_const(2, 111); as.MAP_SET(0, 1, 2);   // m[1] = 111 (overwrite)
        as.load_const(1, 1); as.MAP_GET(6, 0, 1);                          // r6 = 111
        as.LEN(7, 0);                                                      // r7 = 2 (count, not 3)
        as.J(OpCode::HALT);

        std::cout << "Running Block A (core)...\n";
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, nullptr, 8);
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) { return regs[i].isInt() && regs[i].asSigned48() == want; };
            const bool ok = I(3, 100) && I(4, 200) && regs[5].isNil() && I(6, 111) && I(7, 2);
            std::cout << std::format("  get m[1],m[2]=100,200 : {}\n", (I(3,100)&&I(4,200)) ? "PASS" : "FAIL");
            std::cout << std::format("  miss m[9] -> nil      : {}\n", regs[5].isNil() ? "PASS" : "FAIL");
            std::cout << std::format("  overwrite m[1]=111    : {}\n", I(6, 111) ? "PASS" : "FAIL");
            std::cout << std::format("  len = 2 (count)       : {}\n", I(7, 2) ? "PASS" : "FAIL");
            check(ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block M: MAP_GET_OR_TRAP -- present returns the value; a missing key TRAPS ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.load_const(1, 5); as.load_const(2, 50); as.MAP_SET(0, 1, 2);    // m[5] = 50
        as.load_const(1, 5); as.MAP_GET_OR_TRAP(3, 0, 1);                  // r3 = m[5] = 50 (present)
        as.J(OpCode::HALT);

        std::cout << "Running Block M (MAP_GET_OR_TRAP)...\n";
        bool present_ok = false, miss_traps = false;
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, nullptr, 4);
            auto* regs = res.get_reg_base();
            present_ok = regs[3].isInt() && regs[3].asSigned48() == 50;
        }
        catch (const std::exception& e) { record_fail(e.what()); }

        // A missing key aborts with a located "map key not found" fault (mirrors ARRAY_GET OOB).
        Heap heap2(64 * 1024);
        Assembler as2;
        as2.label("main");
        as2.MAP_NEW(0);
        as2.load_const(1, 5); as2.load_const(2, 50); as2.MAP_SET(0, 1, 2); // m[5] = 50
        as2.load_const(1, 9); as2.MAP_GET_OR_TRAP(3, 0, 1);                // m[9] -> TRAP
        as2.J(OpCode::HALT);
        try {
            execute(as2.assemble(), &heap2, nullptr, nullptr, 4);
        }
        catch (const std::exception& e) {
            miss_traps = std::string_view(e.what()).find("map key not found") != std::string_view::npos;
        }
        std::cout << std::format("  present m[5] = 50     : {}\n", present_ok ? "PASS" : "FAIL");
        std::cout << std::format("  missing m[9] traps    : {}\n", miss_traps ? "PASS" : "FAIL");
        check(present_ok && miss_traps);
    }

    // ---- Block B: key types bool/atom/nil, Int(1) != Double(1.0), nil-value ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.load_constant(1, Value::fromBool(true));  as.load_const(2, 1); as.MAP_SET(0, 1, 2); // m[true]=1
        as.load_constant(1, Value::fromBool(false)); as.load_const(2, 2); as.MAP_SET(0, 1, 2); // m[false]=2
        as.load_constant(1, Value::fromAtom(7));     as.load_const(2, 3); as.MAP_SET(0, 1, 2); // m[:7]=3
        as.load_constant(1, Value::fromNil());       as.load_const(2, 4); as.MAP_SET(0, 1, 2); // m[nil]=4
        as.load_const(1, 1);       as.load_const(2, 10); as.MAP_SET(0, 1, 2);                  // m[Int 1]=10
        as.load_double(1, 1.0);    as.load_const(2, 20); as.MAP_SET(0, 1, 2);                  // m[Dbl 1.0]=20
        as.load_const(1, 42); as.load_constant(2, Value::fromNil()); as.MAP_SET(0, 1, 2);      // m[42]=nil
        as.load_constant(1, Value::fromBool(true));  as.MAP_GET(3,  0, 1);   // 1
        as.load_constant(1, Value::fromBool(false)); as.MAP_GET(4,  0, 1);   // 2
        as.load_constant(1, Value::fromAtom(7));     as.MAP_GET(5,  0, 1);   // 3
        as.load_constant(1, Value::fromNil());       as.MAP_GET(6,  0, 1);   // 4
        as.load_const(1, 1);    as.MAP_GET(7,  0, 1);                        // 10  (int key)
        as.load_double(1, 1.0); as.MAP_GET(8,  0, 1);                        // 20  (double key: distinct!)
        as.load_const(1, 42);   as.MAP_GET(9,  0, 1);                        // nil (value is nil)
        as.LEN(10, 0);                                                       // 7 distinct keys
        as.J(OpCode::HALT);

        std::cout << "Running Block B (key types, Int!=Double)...\n";
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, nullptr, 12, &as.constant_pool());
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) { return regs[i].isInt() && regs[i].asSigned48() == want; };
            const bool types_ok = I(3,1) && I(4,2) && I(5,3) && I(6,4);
            const bool split_ok  = I(7,10) && I(8,20);           // Int(1) and Double(1.0) are separate keys
            const bool nilval_ok = regs[9].isNil();              // stored nil is indistinguishable from a miss
            const bool len_ok    = I(10, 7);
            std::cout << std::format("  bool/atom/nil keys    : {}\n", types_ok ? "PASS" : "FAIL");
            std::cout << std::format("  Int(1)!=Double(1.0)   : {}\n", split_ok ? "PASS" : "FAIL");
            std::cout << std::format("  m[42]=nil reads nil   : {}\n", nilval_ok ? "PASS" : "FAIL");
            std::cout << std::format("  len = 7 distinct keys : {}\n", len_ok ? "PASS" : "FAIL");
            check(types_ok && split_ok && nilval_ok && len_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block C: SameValueZero -- -0.0/+0.0 collapse, NaN keys collapse ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.load_double(1, 0.0);  as.load_const(2, 1); as.MAP_SET(0, 1, 2);  // m[0.0]=1
        as.load_double(1, -0.0); as.load_const(2, 2); as.MAP_SET(0, 1, 2);  // m[-0.0]=2 -> same key (overwrite)
        as.load_double(1, 0.0);  as.MAP_GET(3, 0, 1);                       // 2
        as.load_constant(1, Value::nan()); as.load_const(2, 5); as.MAP_SET(0, 1, 2); // m[NaN]=5
        as.load_constant(1, Value::nan()); as.load_const(2, 6); as.MAP_SET(0, 1, 2); // m[NaN]=6 -> same key
        as.load_constant(1, Value::nan()); as.MAP_GET(4, 0, 1);             // 6
        as.LEN(5, 0);                                                       // 2 (zero-key + nan-key)
        as.J(OpCode::HALT);

        std::cout << "Running Block C (SameValueZero)...\n";
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, nullptr, 8, &as.constant_pool());
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) { return regs[i].isInt() && regs[i].asSigned48() == want; };
            const bool ok = I(3, 2) && I(4, 6) && I(5, 2);
            std::cout << std::format("  -0.0/+0.0 one key     : {}\n", (I(3,2)&&I(5,2)) ? "PASS" : "FAIL");
            std::cout << std::format("  NaN keys collapse     : {}\n", I(4, 6) ? "PASS" : "FAIL");
            check(ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block D: string keys hash/compare by CONTENT (concat == literal) ----
    {
        Heap heap(64 * 1024);
        StringInterner interner;
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.load_str(1, "hello"); as.load_const(2, 1); as.MAP_SET(0, 1, 2);   // m["hello"]=1
        // GET via a FRESH non-interned concat "hel"+"lo" -> must hit by content.
        as.load_str(3, "hel"); as.load_str(4, "lo"); as.R6(OpCode::ADD, 5, 3, 4); // r5 = "hello" (fresh)
        as.MAP_GET(6, 0, 5);                                                 // 1
        // SET via a fresh concat key, GET via the literal -> must hit by content.
        as.load_str(3, "wor"); as.load_str(4, "ld"); as.R6(OpCode::ADD, 7, 3, 4);  // r7 = "world" (fresh)
        as.load_const(2, 2); as.MAP_SET(0, 7, 2);                            // m["world"(fresh)]=2
        as.load_str(1, "world"); as.MAP_GET(8, 0, 1);                        // 2
        as.LEN(9, 0);                                                        // 2
        as.J(OpCode::HALT);

        std::cout << "Running Block D (content string keys)...\n";
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, &interner, 12,
                                 nullptr, nullptr, &as.string_literals());
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) { return regs[i].isInt() && regs[i].asSigned48() == want; };
            const bool ok = I(6, 1) && I(8, 2) && I(9, 2);
            std::cout << std::format("  get concat->literal   : {}\n", I(6, 1) ? "PASS" : "FAIL");
            std::cout << std::format("  set concat/get literal: {}\n", I(8, 2) ? "PASS" : "FAIL");
            std::cout << std::format("  len = 2               : {}\n", I(9, 2) ? "PASS" : "FAIL");
            check(ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block E: grow / rehash interleaved with GC under a tiny heap ----
    {
        Heap heap(8 * 1024);                       // tiny -> the grows collect
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.load_const(1, 0);                       // i
        as.load_const(5, 40);                      // limit
        as.label("loop");
        as.B(OpCode::BGE_INT, 1, 5, "done");       // while i < 40
        as.MAP_SET(0, 1, 1);                       // m[i] = i  (rd=value=r1, ra=map=r0, rb=key=r1)
        as.C2(OpCode::INCR, 1);
        as.J(OpCode::J, "loop");
        as.label("done");
        as.load_const(2, 0);  as.MAP_GET(6, 0, 2); // m[0]  = 0
        as.load_const(2, 7);  as.MAP_GET(7, 0, 2); // m[7]  = 7
        as.load_const(2, 39); as.MAP_GET(8, 0, 2); // m[39] = 39
        as.load_const(2, 40); as.MAP_GET(9, 0, 2); // m[40] = nil (never set)
        as.LEN(10, 0);                             // 40
        as.J(OpCode::HALT);

        std::cout << "Running Block E (grow + GC)...\n";
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, nullptr, 12);
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) { return regs[i].isInt() && regs[i].asSigned48() == want; };
            const bool ok = I(6, 0) && I(7, 7) && I(8, 39) && regs[9].isNil() && I(10, 40);
            std::cout << std::format("  40 keys survive grows : {}\n", (I(6,0)&&I(7,7)&&I(8,39)) ? "PASS" : "FAIL");
            std::cout << std::format("  len = 40              : {}\n", I(10, 40) ? "PASS" : "FAIL");
            check(ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block F: an invalid key type (a non-string heap object) traps ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.ALLOC(1, 2);                            // r1 = array -> not a permitted key
        as.load_const(2, 5);
        as.MAP_SET(0, 1, 2);                       // must trap "Invalid map key type"
        as.J(OpCode::HALT);

        std::cout << "Running Block F (invalid key trap)...\n";
        bool trapped = false;
        try { execute(as.assemble(), &heap, nullptr, nullptr, 4); }
        catch (const std::exception& e) {
            trapped = std::string_view(e.what()).find("invalid map key") != std::string_view::npos;
        }
        std::cout << std::format("  array key traps       : {}\n", trapped ? "PASS" : "FAIL");
        check(trapped);
    }

    // ---- Block G: MAP_HAS -- presence test distinct from value (B4) ----
    {
        Heap heap(64 * 1024);
        StringInterner interner;
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.load_const(1, 1);  as.load_const(2, 100); as.MAP_SET(0, 1, 2);          // m[1] = 100
        as.load_const(1, 7);  as.load_constant(2, Value::fromNil()); as.MAP_SET(0, 1, 2); // m[7] = nil
        as.load_const(1, 1);  as.MAP_HAS(3, 0, 1);                                  // r3 = has(m,1)  = true
        as.load_const(1, 9);  as.MAP_HAS(4, 0, 1);                                  // r4 = has(m,9)  = false (absent)
        as.load_const(1, 7);  as.MAP_HAS(5, 0, 1);                                  // r5 = has(m,7)  = true  (value is nil!)
        as.load_const(1, 7);  as.MAP_GET(6, 0, 1);                                  // r6 = m[7]      = nil
        // string key by CONTENT: set via literal, test presence via a fresh concat.
        as.load_str(1, "hello"); as.load_const(2, 5); as.MAP_SET(0, 1, 2);          // m["hello"] = 5
        as.load_str(7, "hel"); as.load_str(8, "lo"); as.R6(OpCode::ADD, 9, 7, 8);   // r9 = "hello" (fresh)
        as.MAP_HAS(10, 0, 9);                                                       // r10 = true  (content hit)
        as.load_str(1, "nope"); as.MAP_HAS(11, 0, 1);                               // r11 = false
        as.J(OpCode::HALT);

        std::cout << "Running Block G (MAP_HAS presence)...\n";
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, &interner, 12,
                                 &as.constant_pool(), nullptr, &as.string_literals());
            auto* regs = res.get_reg_base();
            auto B = [&](int i, bool want) { return regs[i].isBool() && regs[i].asBool() == want; };
            const bool present_ok = B(3, true) && B(4, false);
            const bool nilval_ok  = B(5, true) && regs[6].isNil();   // key present, value nil
            const bool string_ok  = B(10, true) && B(11, false);     // content hit / miss
            std::cout << std::format("  has present / absent  : {}\n", present_ok ? "PASS" : "FAIL");
            std::cout << std::format("  has key->nil is true  : {}\n", nilval_ok ? "PASS" : "FAIL");
            std::cout << std::format("  has string by content : {}\n", string_ok ? "PASS" : "FAIL");
            check(present_ok && nilval_ok && string_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block H: MAP_HAS on an invalid key type traps ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.ALLOC(1, 2);                            // r1 = array -> not a permitted key
        as.MAP_HAS(2, 0, 1);                       // must trap "Invalid map key type"
        as.J(OpCode::HALT);

        std::cout << "Running Block H (MAP_HAS invalid key trap)...\n";
        bool trapped = false;
        try { execute(as.assemble(), &heap, nullptr, nullptr, 4); }
        catch (const std::exception& e) {
            trapped = std::string_view(e.what()).find("invalid map key") != std::string_view::npos;
        }
        std::cout << std::format("  has array key traps   : {}\n", trapped ? "PASS" : "FAIL");
        check(trapped);
    }

    // ---- Block I: MAP_DELETE -- remove, tombstone-chain integrity, reuse (B5) ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        // Five int keys 1..5 -> 10..50 (5 keys stay under the cap-8 grow threshold, so
        // the collision structure survives -- deleting a middle key must not break the
        // probe chains of the others).
        as.load_const(1, 1); as.load_const(2, 10); as.MAP_SET(0, 1, 2);
        as.load_const(1, 2); as.load_const(2, 20); as.MAP_SET(0, 1, 2);
        as.load_const(1, 3); as.load_const(2, 30); as.MAP_SET(0, 1, 2);
        as.load_const(1, 4); as.load_const(2, 40); as.MAP_SET(0, 1, 2);
        as.load_const(1, 5); as.load_const(2, 50); as.MAP_SET(0, 1, 2);
        as.load_const(1, 3);  as.MAP_DELETE(3, 0, 1);   // r3 = true  (was present)
        as.load_const(1, 3);  as.MAP_DELETE(4, 0, 1);   // r4 = false (already gone)
        as.load_const(1, 99); as.MAP_DELETE(5, 0, 1);   // r5 = false (never present)
        as.load_const(1, 3);  as.MAP_HAS(6, 0, 1);      // r6 = false
        as.load_const(1, 3);  as.MAP_GET(7, 0, 1);      // r7 = nil
        as.load_const(1, 1);  as.MAP_GET(8, 0, 1);      // 10  survivors still reachable
        as.load_const(1, 2);  as.MAP_GET(9, 0, 1);      // 20  (past the tombstone)
        as.load_const(1, 4);  as.MAP_GET(10, 0, 1);     // 40
        as.load_const(1, 5);  as.MAP_GET(11, 0, 1);     // 50
        as.LEN(12, 0);                                  // 4   (count decremented)
        // Reinsert key 3 -> the tombstone slot is reused (used did not grow).
        as.load_const(1, 3);  as.load_const(2, 33); as.MAP_SET(0, 1, 2);
        as.load_const(1, 3);  as.MAP_GET(13, 0, 1);     // 33
        as.LEN(14, 0);                                  // 5
        as.J(OpCode::HALT);

        std::cout << "Running Block I (MAP_DELETE)...\n";
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, nullptr, 16);
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) { return regs[i].isInt() && regs[i].asSigned48() == want; };
            auto B = [&](int i, bool want) { return regs[i].isBool() && regs[i].asBool() == want; };
            const bool ret_ok      = B(3, true) && B(4, false) && B(5, false);
            const bool gone_ok     = B(6, false) && regs[7].isNil();
            const bool survivors_ok= I(8, 10) && I(9, 20) && I(10, 40) && I(11, 50) && I(12, 4);
            const bool reuse_ok    = I(13, 33) && I(14, 5);
            std::cout << std::format("  delete present/absent : {}\n", ret_ok ? "PASS" : "FAIL");
            std::cout << std::format("  deleted key gone      : {}\n", gone_ok ? "PASS" : "FAIL");
            std::cout << std::format("  chain intact + len 4  : {}\n", survivors_ok ? "PASS" : "FAIL");
            std::cout << std::format("  tombstone reuse       : {}\n", reuse_ok ? "PASS" : "FAIL");
            check(ret_ok && gone_ok && survivors_ok && reuse_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block J: MAP_DELETE on an invalid key type traps ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.ALLOC(1, 2);                            // r1 = array -> not a permitted key
        as.MAP_DELETE(2, 0, 1);                    // must trap "Invalid map key type"
        as.J(OpCode::HALT);

        std::cout << "Running Block J (MAP_DELETE invalid key trap)...\n";
        bool trapped = false;
        try { execute(as.assemble(), &heap, nullptr, nullptr, 4); }
        catch (const std::exception& e) {
            trapped = std::string_view(e.what()).find("invalid map key") != std::string_view::npos;
        }
        std::cout << std::format("  delete array key traps: {}\n", trapped ? "PASS" : "FAIL");
        check(trapped);
    }

    // ---- Block K: MAP_KEYS / MAP_VALUES -- snapshot to arrays + alignment (B6) ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.load_const(1, 1); as.load_const(2, 10); as.MAP_SET(0, 1, 2);
        as.load_const(1, 2); as.load_const(2, 20); as.MAP_SET(0, 1, 2);
        as.load_const(1, 3); as.load_const(2, 30); as.MAP_SET(0, 1, 2);
        as.MAP_KEYS(3, 0);                              // r3 = [keys]  (hash order)
        as.MAP_VALUES(4, 0);                            // r4 = [values]
        as.LEN(5, 3);                                   // r5 = 3
        as.LEN(6, 4);                                   // r6 = 3
        // Fetch keys[0..2] (r7,r8,r9) and values[0..2] (r10,r11,r12) at aligned indices.
        as.load_const(1, 0); as.ARRAY_GET(7, 3, 1);  as.ARRAY_GET(10, 4, 1);
        as.load_const(1, 1); as.ARRAY_GET(8, 3, 1);  as.ARRAY_GET(11, 4, 1);
        as.load_const(1, 2); as.ARRAY_GET(9, 3, 1);  as.ARRAY_GET(12, 4, 1);
        // Keys are a permutation of {1,2,3} -> sum 6; values of {10,20,30} -> sum 60.
        as.R6(OpCode::ADD, 13, 7, 8);  as.R6(OpCode::ADD, 13, 13, 9);   // r13 = 6
        as.R6(OpCode::ADD, 14, 10, 11); as.R6(OpCode::ADD, 14, 14, 12); // r14 = 60
        // Alignment: m[keys[i]] == values[i] for each i.
        as.MAP_GET(15, 0, 7); as.R6(OpCode::EQ, 15, 15, 10);
        as.MAP_GET(16, 0, 8); as.R6(OpCode::EQ, 16, 16, 11);
        as.MAP_GET(17, 0, 9); as.R6(OpCode::EQ, 17, 17, 12);
        as.J(OpCode::HALT);

        std::cout << "Running Block K (MAP_KEYS/MAP_VALUES)...\n";
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, nullptr, 18);
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) { return regs[i].isInt() && regs[i].asSigned48() == want; };
            auto B = [&](int i, bool want) { return regs[i].isBool() && regs[i].asBool() == want; };
            const bool len_ok   = I(5, 3) && I(6, 3);
            const bool set_ok   = I(13, 6) && I(14, 60);    // right keys / values (order-free)
            const bool align_ok = B(15, true) && B(16, true) && B(17, true);
            std::cout << std::format("  keys/values len = 3  : {}\n", len_ok ? "PASS" : "FAIL");
            std::cout << std::format("  key/value sets right : {}\n", set_ok ? "PASS" : "FAIL");
            std::cout << std::format("  keys[i]/values[i] aln: {}\n", align_ok ? "PASS" : "FAIL");
            check(len_ok && set_ok && align_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block L: keys/values of an empty map, and after a delete ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.MAP_KEYS(1, 0);   as.LEN(2, 1);              // empty -> r2 = 0
        as.MAP_VALUES(3, 0); as.LEN(4, 3);              // empty -> r4 = 0
        // Fill 3, delete 1 -> live count 2 reflected by keys()/values().
        as.load_const(5, 1); as.load_const(6, 1); as.MAP_SET(0, 5, 6);
        as.load_const(5, 2); as.load_const(6, 2); as.MAP_SET(0, 5, 6);
        as.load_const(5, 3); as.load_const(6, 3); as.MAP_SET(0, 5, 6);
        as.load_const(5, 2); as.MAP_DELETE(7, 0, 5);    // remove key 2
        as.MAP_KEYS(8, 0);   as.LEN(9, 8);              // r9 = 2
        as.MAP_VALUES(10, 0); as.LEN(11, 10);           // r11 = 2
        as.J(OpCode::HALT);

        std::cout << "Running Block L (empty / after-delete keys)...\n";
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, nullptr, 12);
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) { return regs[i].isInt() && regs[i].asSigned48() == want; };
            const bool empty_ok = I(2, 0) && I(4, 0);
            const bool after_ok = I(9, 2) && I(11, 2);
            std::cout << std::format("  empty keys/values 0  : {}\n", empty_ok ? "PASS" : "FAIL");
            std::cout << std::format("  after delete len = 2 : {}\n", after_ok ? "PASS" : "FAIL");
            check(empty_ok && after_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }
}

// =============================================================================
// test_map_iter_next -- the no-copy live map cursor (MAP_ITER_NEXT / MAP_KEY_AT /
// MAP_VAL_AT, ids 109-111). Build a map, delete one key (creating a tombstone the
// scan must skip), then drive the cursor to exhaustion accumulating key-sum,
// value-sum and a visit count (all order-independent, since the walk is in hash
// order). Verifies: every live pair visited exactly once, tombstones skipped, and
// the -1 terminator. A second block checks an OOB MAP_KEY_AT index traps.
// =============================================================================
inline void test_map_iter_next() {
    std::cout << "=== map iter (MAP_ITER_NEXT/KEY_AT/VAL_AT) ===\n";

    // ---- Block A: walk live pairs (with a tombstone in the middle) ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);                                              // r0 = map
        as.load_const(1, 1); as.load_const(2, 10); as.MAP_SET(0, 1, 2);   // m[1]=10
        as.load_const(1, 2); as.load_const(2, 20); as.MAP_SET(0, 1, 2);   // m[2]=20
        as.load_const(1, 3); as.load_const(2, 30); as.MAP_SET(0, 1, 2);   // m[3]=30
        as.load_const(1, 2); as.MAP_DELETE(9, 0, 1);                // remove key 2 -> tombstone
        // accumulators + cursor
        as.load_const(10, 0);   // cursor
        as.load_const(14, 0);   // key sum
        as.load_const(15, 0);   // value sum
        as.load_const(16, 0);   // visit count
        as.load_const(17, 0);   // constant 0 (for the j<0 test)
        as.load_const(18, 1);   // constant 1
        as.label("loop");
        as.MAP_ITER_NEXT(11, 0, 10);                 // r11 = next live index (or -1)
        as.B(OpCode::BLT_INT, 11, 17, "end");        // r11 < 0 -> done
        as.MAP_KEY_AT(12, 0, 11);                    // r12 = key
        as.MAP_VAL_AT(13, 0, 11);                    // r13 = value
        as.R6(OpCode::ADD_INT, 14, 14, 12);          // key sum += key
        as.R6(OpCode::ADD_INT, 15, 15, 13);          // value sum += value
        as.R6(OpCode::ADD_INT, 16, 16, 18);          // count += 1
        as.R6(OpCode::ADD_INT, 10, 11, 18);          // cursor = index + 1
        as.J(OpCode::J, "loop");
        as.label("end");
        as.J(OpCode::HALT);

        std::cout << "Running Block A (walk live pairs, skip tombstone)...\n";
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, nullptr, 20);
            auto* regs = res.get_reg_base();
            auto I = [&](int i, int64_t want) { return regs[i].isInt() && regs[i].asSigned48() == want; };
            const bool count_ok = I(16, 2);          // 2 live entries (1 and 3)
            const bool keys_ok  = I(14, 4);          // 1 + 3
            const bool vals_ok  = I(15, 40);         // 10 + 30
            const bool term_ok  = I(11, -1);         // loop exited on the -1 terminator
            std::cout << std::format("  count = 2 (skip tomb): {}\n", count_ok ? "PASS" : "FAIL");
            std::cout << std::format("  key sum = 4          : {}\n", keys_ok ? "PASS" : "FAIL");
            std::cout << std::format("  value sum = 40       : {}\n", vals_ok ? "PASS" : "FAIL");
            std::cout << std::format("  terminator = -1      : {}\n", term_ok ? "PASS" : "FAIL");
            check(count_ok && keys_ok && vals_ok && term_ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block B: an empty map yields -1 on the first MAP_ITER_NEXT ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.load_const(1, 0);
        as.MAP_ITER_NEXT(2, 0, 1);                   // r2 = -1 (no live entries)
        as.J(OpCode::HALT);

        std::cout << "Running Block B (empty map -> -1)...\n";
        try {
            auto res   = execute(as.assemble(), &heap, nullptr, nullptr, 4);
            auto* regs = res.get_reg_base();
            const bool ok = regs[2].isInt() && regs[2].asSigned48() == -1;
            std::cout << std::format("  empty -> -1          : {}\n", ok ? "PASS" : "FAIL");
            check(ok);
        }
        catch (const std::exception& e) { record_fail(e.what()); }
    }

    // ---- Block C: an out-of-range MAP_KEY_AT index traps (defensive guard) ----
    {
        Heap heap(64 * 1024);
        Assembler as;
        as.label("main");
        as.MAP_NEW(0);
        as.load_const(1, 1); as.load_const(2, 10); as.MAP_SET(0, 1, 2);
        as.load_const(3, 1000); as.MAP_KEY_AT(4, 0, 3);   // index 1000 >> cap -> TRAP
        as.J(OpCode::HALT);

        std::cout << "Running Block C (OOB index traps)...\n";
        bool traps = false;
        try {
            execute(as.assemble(), &heap, nullptr, nullptr, 8);
        }
        catch (const std::exception& e) {
            traps = std::string_view(e.what()).find("map iteration index out of range") != std::string_view::npos;
        }
        std::cout << std::format("  OOB index traps      : {}\n", traps ? "PASS" : "FAIL");
        check(traps);
    }
}

// =============================================================================
// test_closure_gc -- the moving-GC guard for KIND_CLOSURE (Milestone A1). A
// closure's by-value CAPTURE that is itself a heap pointer must be forwarded
// through the Cheney scan of the closure payload (which is scanned like a
// KIND_ARRAY / KIND_OBJECT), and the 16-bit function id in the header `_pad`
// must ride forward()'s memcpy intact.
//
//   A heap array `inner` is captured into closure slot 0; slot 1 holds a plain
//   Int. The closure is the ONLY root (win[0]); `inner` is reachable solely
//   through the capture. A dead canary is allocated but never rooted. After a
//   collect(): the closure is physically relocated (still fn_id-tagged), its
//   capture[0] is forwarded to the moved `inner` (contents intact), capture[1]'s
//   Int is preserved, and the dead canary is the ONLY thing reclaimed.
//
// This is a pure heap-level test (no bytecode): MAKE_CLOSURE / LOAD_CAPTURE do
// not exist until A3/A4, so the closure is built directly via heap.alloc_closure.
// =============================================================================
inline void test_closure_gc() {
    std::cout << "=== closure_gc ===\n";

    constexpr uint16_t FN_ID = 42; // arbitrary function-table id, must survive the move

    Heap heap(64 * 1024);
    GcTestCtx t(&heap, 1); // r0 (the closure) is the only live root

    // The captured heap array -- reachable ONLY through the closure capture slot.
    GcObject* inner = heap.alloc(GcObject::KIND_ARRAY, 3);
    assert(inner && "closure_gc: inner alloc failed");
    inner->slots()[0] = Value::fromSigned48(11);
    inner->slots()[1] = Value::fromSigned48(22);
    inner->slots()[2] = Value::fromSigned48(33);

    // The closure: 2 captures. slot0 = inner (heap ptr), slot1 = a plain Int.
    GcObject* clo = heap.alloc_closure(FN_ID, 2);
    assert(clo && "closure_gc: closure alloc failed");
    clo->slots()[0] = Value::fromPtr(inner->slots());
    clo->slots()[1] = Value::fromSigned48(7);

    // Dead canary: allocated but never rooted -- must be the ONLY thing collected.
    GcObject* dead = heap.alloc(GcObject::KIND_ARRAY, 2);
    assert(dead && "closure_gc: dead canary alloc failed");
    const size_t dead_total = dead->total_bytes();

    // Root ONLY the closure. `inner` survives solely via the closure payload scan.
    t.win[0] = Value::fromPtr(clo->slots());

    Context      ctx         = t.context();
    void*        clo_before  = t.win[0].asPtr();
    const size_t freed       = heap.collect(&ctx);

    // Re-fetch everything THROUGH the root -- pointers moved.
    GcObject* clo2 = t.win[0].isPtr() ? GcObject::from_slots(t.win[0].asPtr()) : nullptr;

    const bool clo_ok = clo2 &&
                        clo2->kind == GcObject::KIND_CLOSURE &&
                        clo2->slot_count() == 2 &&
                        clo2->closure_fn_id() == FN_ID &&
                        t.win[0].asPtr() != clo_before; // physically relocated

    GcObject* inner2 = nullptr;
    if (clo2) {
        const Value cap0 = clo2->slots()[0];
        inner2 = cap0.isPtr() ? GcObject::from_slots(cap0.asPtr()) : nullptr;
    }
    const bool inner_ok = inner2 &&
                          inner2->kind == GcObject::KIND_ARRAY &&
                          inner2->slot_count() == 3 &&
                          inner2->slots()[0].asSigned48() == 11 &&
                          inner2->slots()[1].asSigned48() == 22 &&
                          inner2->slots()[2].asSigned48() == 33;

    const bool cap1_ok = clo2 &&
                         clo2->slots()[1].isInt() &&
                         clo2->slots()[1].asSigned48() == 7;

    const bool freed_ok = freed == dead_total;

    const bool ok = clo_ok && inner_ok && cap1_ok && freed_ok;

    std::cout << std::format("freed={} (expect dead_total={})\n", freed, dead_total);
    std::cout << std::format("  closure survived + fn_id intact:       {}\n", clo_ok ? "PASS" : "FAIL");
    std::cout << std::format("  captured heap ptr forwarded + intact:  {}\n", inner_ok ? "PASS" : "FAIL");
    std::cout << std::format("  captured Int preserved:                {}\n", cap1_ok ? "PASS" : "FAIL");
    std::cout << std::format("  only the dead canary was collected:    {}\n", freed_ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_frame_size_root_boundary -- the NEGATIVE half of the frame_size GC-roots
// contract. test_frame_gc_roots proves a pointer INSIDE [0, frame_size) survives;
// this proves a pointer at or ABOVE frame_size is NOT a root, which is the other
// half of the contract and had no coverage.
//
// It matters because that is exactly where a dead frame's leftovers live: once a
// frame returns, its registers sit above the caller's frame_size and nothing
// forwards them any more. A collector that scanned them would resurrect dead
// objects here -- and, once such a leftover has gone stale across a collection,
// would follow a dangling pointer instead (the memory-safety hole this contract
// exists to prevent).
//
// The out-of-frame slot deliberately points at a VALID but otherwise unreachable
// object, so a regression fails as a clean "it survived" assertion rather than by
// dereferencing garbage and taking the whole suite down with it.
// =============================================================================
inline void test_frame_size_root_boundary() {
    std::cout << "=== frame_size_root_boundary ===\n";

    Heap heap(64 * 1024);
    GcTestCtx t(&heap, 2);   // ONLY r0 and r1 are roots

    GcObject* live = heap.alloc(GcObject::KIND_ARRAY, 2);
    assert(live && "root_boundary: live alloc failed");
    live->slots()[0] = Value::fromSigned48(101);
    live->slots()[1] = Value::fromSigned48(202);

    // Unreachable except through r2, which lies ABOVE frame_size (2) -- so it must die.
    GcObject* out_of_frame = heap.alloc(GcObject::KIND_ARRAY, 3);
    assert(out_of_frame && "root_boundary: out_of_frame alloc failed");
    const size_t out_total = out_of_frame->total_bytes();

    // Plain dead canary, referenced by nothing at all.
    GcObject* dead = heap.alloc(GcObject::KIND_ARRAY, 2);
    assert(dead && "root_boundary: dead canary alloc failed");
    const size_t dead_total = dead->total_bytes();

    t.win[0] = Value::fromPtr(live->slots());          // in-frame  -> must survive
    t.win[1] = Value::fromSigned48(9);                 // in-frame immediate
    t.win[2] = Value::fromPtr(out_of_frame->slots());  // OUT of frame -> must NOT be a root

    Context      ctx           = t.context();
    void*        live_before   = t.win[0].asPtr();
    const void*  out_before    = t.win[2].asPtr();
    const size_t freed         = heap.collect(&ctx);

    GcObject* live2 = t.win[0].isPtr() ? GcObject::from_slots(t.win[0].asPtr()) : nullptr;
    const bool live_ok = live2 &&
                         live2->kind == GcObject::KIND_ARRAY &&
                         live2->slot_count() == 2 &&
                         live2->slots()[0].asSigned48() == 101 &&
                         live2->slots()[1].asSigned48() == 202 &&
                         t.win[0].asPtr() != live_before;      // physically relocated

    // Both unrooted objects must be gone: the canary AND the one reachable only
    // from the out-of-frame slot.
    const bool freed_ok = freed == dead_total + out_total;

    // The out-of-frame slot must be left completely alone -- not forwarded, not cleared.
    const bool untouched_ok = t.win[2].isPtr() && t.win[2].asPtr() == out_before;

    const bool ok = live_ok && freed_ok && untouched_ok;

    std::cout << std::format("freed={} (expect dead={} + out_of_frame={} = {})\n",
                             freed, dead_total, out_total, dead_total + out_total);
    std::cout << std::format("  in-frame root survived + forwarded:      {}\n", live_ok ? "PASS" : "FAIL");
    std::cout << std::format("  out-of-frame object was NOT retained:    {}\n", freed_ok ? "PASS" : "FAIL");
    std::cout << std::format("  out-of-frame slot left untouched:        {}\n", untouched_ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_closure_roots -- the GC-root guard for the parallel closure stack +
// current_closure (Milestone A2). Both are new strong root sources scanned by
// forward_vm_external_roots: the innermost activation's closure (vm.current_closure)
// and every saved caller closure on the closure stack (closure_stack_base[0..ret_depth)).
//
//   Closure A is installed as the current closure; Closure B is pushed onto the
//   closure stack as a saved caller closure (with ret_depth == 1). Each captures a
//   heap array reachable ONLY through its payload. A Context with one saved frame
//   that scans ZERO registers is built by hand, so the closures survive SOLELY via
//   the two new closure roots -- not via any register frame. After a collect(): both
//   closures are relocated (fn_id intact), their captures forwarded via the payload
//   scan, and only the dead canary is reclaimed.
//
// A2 installs no closure through bytecode yet (MAKE_CLOSURE/CALL_INDIRECT-on-closure
// land in A3/A4), so this drives the root scan directly at the VM level.
// =============================================================================
inline void test_closure_roots() {
    std::cout << "=== closure_roots ===\n";

    constexpr uint16_t FN_A = 5;
    constexpr uint16_t FN_B = 9;

    Heap heap(64 * 1024);
    VM_Resources res;
    VM vm;
    vm.heap               = &heap;
    vm.closure_stack_base = res.get_closure_base();

    // Two captured heap arrays, each reachable ONLY through a closure capture slot.
    GcObject* inner_a = heap.alloc(GcObject::KIND_ARRAY, 2);
    GcObject* inner_b = heap.alloc(GcObject::KIND_ARRAY, 2);
    assert(inner_a && inner_b && "closure_roots: inner alloc failed");
    inner_a->slots()[0] = Value::fromSigned48(111);
    inner_a->slots()[1] = Value::fromSigned48(222);
    inner_b->slots()[0] = Value::fromSigned48(333);
    inner_b->slots()[1] = Value::fromSigned48(444);

    GcObject* cloA = heap.alloc_closure(FN_A, 1); // -> current_closure root
    GcObject* cloB = heap.alloc_closure(FN_B, 1); // -> closure-stack root
    assert(cloA && cloB && "closure_roots: closure alloc failed");
    cloA->slots()[0] = Value::fromPtr(inner_a->slots());
    cloB->slots()[0] = Value::fromPtr(inner_b->slots());

    // Dead canary: unrooted -> must be the ONLY thing reclaimed.
    GcObject* dead = heap.alloc(GcObject::KIND_ARRAY, 3);
    assert(dead && "closure_roots: dead canary alloc failed");
    const size_t dead_total = dead->total_bytes();

    // Install A as the innermost activation's closure; push B as a saved caller closure.
    vm.current_closure        = Value::fromPtr(cloA->slots());
    res.get_closure_base()[0] = Value::fromPtr(cloB->slots());

    // One saved return frame that scans ZERO registers (frame size 0), so the closures
    // are reachable SOLELY via the two closure roots -- never a register frame.
    res.get_frame_size_base()[0]     = 0;
    res.get_ret_base()[0].old_ip     = nullptr;            // unused when frame size is 0
    res.get_ret_base()[0].old_window = res.get_reg_base();

    Context ctx{};
    ctx.window_ptr         = res.get_reg_base();
    ctx.vm                 = &vm;
    ctx.ret_stack_base     = res.get_ret_base();
    ctx.ret_stack_ptr      = res.get_ret_base() + 1;                    // ret_depth == 1
    ctx.ret_stack_limit    = res.get_ret_base() + VM_Resources::RET_FRAME_COUNT;
    ctx.frame_size_ptr     = res.get_frame_size_base() + 1;            // parallel to ret_stack_ptr
    ctx.current_frame_size = 0;                                        // current frame scans 0 registers

    void*        a_before = vm.current_closure.asPtr();
    void*        b_before = res.get_closure_base()[0].asPtr();
    const size_t freed    = heap.collect(&ctx);

    // Re-fetch THROUGH the roots -- pointers moved.
    GcObject* a2 = vm.current_closure.isPtr()
                 ? GcObject::from_slots(vm.current_closure.asPtr()) : nullptr;
    GcObject* b2 = res.get_closure_base()[0].isPtr()
                 ? GcObject::from_slots(res.get_closure_base()[0].asPtr()) : nullptr;

    const bool a_ok = a2 && a2->kind == GcObject::KIND_CLOSURE &&
                      a2->closure_fn_id() == FN_A &&
                      vm.current_closure.asPtr() != a_before;
    const bool b_ok = b2 && b2->kind == GcObject::KIND_CLOSURE &&
                      b2->closure_fn_id() == FN_B &&
                      res.get_closure_base()[0].asPtr() != b_before;

    GcObject* ia = (a2 && a2->slots()[0].isPtr())
                 ? GcObject::from_slots(a2->slots()[0].asPtr()) : nullptr;
    GcObject* ib = (b2 && b2->slots()[0].isPtr())
                 ? GcObject::from_slots(b2->slots()[0].asPtr()) : nullptr;
    const bool ia_ok = ia && ia->slots()[0].asSigned48() == 111 && ia->slots()[1].asSigned48() == 222;
    const bool ib_ok = ib && ib->slots()[0].asSigned48() == 333 && ib->slots()[1].asSigned48() == 444;

    const bool freed_ok = freed == dead_total;

    const bool ok = a_ok && b_ok && ia_ok && ib_ok && freed_ok;
    std::cout << std::format("freed={} (expect dead_total={})\n", freed, dead_total);
    std::cout << std::format("  current_closure root forwarded:      {}\n", a_ok ? "PASS" : "FAIL");
    std::cout << std::format("  closure-stack root forwarded:        {}\n", b_ok ? "PASS" : "FAIL");
    std::cout << std::format("  A capture forwarded via payload:     {}\n", ia_ok ? "PASS" : "FAIL");
    std::cout << std::format("  B capture forwarded via payload:     {}\n", ib_ok ? "PASS" : "FAIL");
    std::cout << std::format("  only the dead canary collected:      {}\n", freed_ok ? "PASS" : "FAIL");
    check(ok);
}

// =============================================================================
// test_cross_frame_call -- regression test for the DECOUPLED window_size contract
// (consequence (a)). A caller with a LARGE frame calls a callee with a SMALL frame.
//
//   Top-level frame size = 3: r0, r1, r2 hold three live locals (10, 20, 30).
//   It calls "smallee", whose frame size is 1 (it uses only r0), and which just
//   writes 999 into its r0 and returns.
//
// Under the OLD design the slide was the CALLEE's size (1), so smallee.r0 would map
// to the caller's r1 and clobber the live local 20 -- and the result would not
// appear at r3. Under the decoupled design the slide is the CALLER's frame size (3),
// so smallee's window opens at r3: r0..r2 are untouched and the result lands at r3.
// This test therefore passes ONLY with the decoupled contract.
// =============================================================================
inline void test_cross_frame_call() {
    constexpr uint8_t TOP_FRAME    = 3;  // top-level reserves r0..r2 for live locals
    constexpr uint8_t SMALLEE_FRAME = 1; // callee uses only r0

    Assembler as;
    as.func("smallee", SMALLEE_FRAME);

    as.label("main");
    as.C2(OpCode::LOAD_CONST, 0, 10);
    as.C2(OpCode::LOAD_CONST, 1, 20);
    as.C2(OpCode::LOAD_CONST, 2, 30);
    as.CALL("smallee");                  // slide = TOP_FRAME (3): smallee.r0 == top.r3
    as.J(OpCode::HALT);

    as.label("smallee");
    as.C2(OpCode::LOAD_CONST, 0, 999);
    as.J(OpCode::RET);

    const auto bytecode = as.assemble();
    std::cout << "=== cross_frame_call (caller frame 3 -> callee frame 1) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size());
    dis.add_label(0, "main"); dis.add_label(4, "smallee");
    dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, TOP_FRAME);
        auto* regs = res.get_reg_base();
        const int64_t r0 = regs[0].asSigned48();
        const int64_t r1 = regs[1].asSigned48();
        const int64_t r2 = regs[2].asSigned48();
        const int64_t r3 = regs[TOP_FRAME].asSigned48(); // smallee's r0 via the overlap
        std::cout << std::format("caller locals: r0={} r1={} r2={} (expect 10 20 30)\n", r0, r1, r2);
        std::cout << std::format("callee result: r{}={} (expect 999)\n",
            static_cast<int>(TOP_FRAME), r3);
        const bool ok = r0 == 10 && r1 == 20 && r2 == 30 && r3 == 999;
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// Small helper for the double tests -- all expected results below are exact in
// binary, but a tolerance keeps the checks robust against incidental rounding.
static bool approx(double a, double b) { return std::fabs(a - b) < 1e-12; }

// =============================================================================
// test_load_double -- the constant pool + LOAD_CONST_POOL. Loads a double, the
// canonicalized -0.0 (-> 0.0), and an arbitrary non-double Value (an Int) through
// the pool, proving it materializes values a LOAD_CONST chain cannot build.
// =============================================================================
inline void test_load_double() {
    Assembler as;
    as.label("main");
    as.load_double(0, 3.14159);
    as.load_double(1, -0.0);                          // fromDouble maps -0.0 -> 0.0
    as.load_constant(2, Value::fromSigned48(42));     // arbitrary Value via pool
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== load_double (constant pool) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, 0, &as.constant_pool());
        auto* regs = res.get_reg_base();
        const bool r0_ok = regs[0].isDouble() && approx(regs[0].asDouble(), 3.14159);
        const bool r1_ok = regs[1].isDouble() && regs[1].asDouble() == 0.0;
        const bool r2_ok = regs[2].isInt()    && regs[2].asSigned48() == 42;
        std::cout << std::format("r0={} r1={} r2={}\n", regs[0], regs[1], regs[2]);
        check(r0_ok && r1_ok && r2_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_double_arith -- generic ADD/SUB/MUL/DIV/NEG on two doubles (both-double
// path). All operands and results are exact binary fractions.
// =============================================================================
inline void test_double_arith() {
    Assembler as;
    as.label("main");
    as.load_double(0, 1.5);
    as.load_double(1, 0.25);
    as.R6(OpCode::ADD, 2, 0, 1);      // 1.75
    as.R6(OpCode::SUB, 3, 0, 1);      // 1.25
    as.R6(OpCode::MUL, 4, 0, 1);      // 0.375
    as.R6(OpCode::DIV, 5, 0, 1);      // 6.0
    as.R6(OpCode::NEG, 6, 0, 0);      // -1.5
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== double_arith (add/sub/mul/div/neg) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, 0, &as.constant_pool());
        auto* regs = res.get_reg_base();
        const double r2 = regs[2].asDouble(), r3 = regs[3].asDouble();
        const double r4 = regs[4].asDouble(), r5 = regs[5].asDouble(), r6 = regs[6].asDouble();
        std::cout << std::format("1.5+0.25={} 1.5-0.25={} 1.5*0.25={} 1.5/0.25={} -1.5={}\n",
                                 r2, r3, r4, r5, r6);
        const bool ok = regs[2].isDouble() && regs[3].isDouble() && regs[4].isDouble() &&
                        regs[5].isDouble() && regs[6].isDouble() &&
                        approx(r2, 1.75) && approx(r3, 1.25) && approx(r4, 0.375) &&
                        approx(r5, 6.0) && approx(r6, -1.5);
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_num_promotion -- the int->double promotion rule on the generic ops:
//   - Int + Double and Double + Int both promote to Double.
//   - Int + Int stays Int (wrapping arithmetic).
//   - Generic DIV on two Ints does truncating integer division (Int result);
//     with a Double operand it does IEEE division (Double result).
//   - Double division by 0.0 yields +inf, NOT a trap.
// =============================================================================
inline void test_num_promotion() {
    Assembler as;
    as.label("main");
    as.C2(OpCode::LOAD_CONST, 0, 2);      // Int 2
    as.load_double(1, 0.5);               // Dbl 0.5
    as.R6(OpCode::ADD, 2, 0, 1);          // 2 + 0.5   -> Dbl 2.5
    as.R6(OpCode::ADD, 3, 1, 0);          // 0.5 + 2   -> Dbl 2.5 (order-independent)
    as.C2(OpCode::LOAD_CONST, 4, 40);     // Int 40
    as.R6(OpCode::ADD, 5, 0, 4);          // 2 + 40    -> Int 42
    as.C2(OpCode::LOAD_CONST, 6, 7);      // Int 7
    as.C2(OpCode::LOAD_CONST, 7, 2);      // Int 2
    as.R6(OpCode::DIV, 8, 6, 7);          // 7 / 2     -> Int 3  (truncating)
    as.load_double(9, 2.0);               // Dbl 2.0
    as.R6(OpCode::DIV, 10, 6, 9);         // 7 / 2.0   -> Dbl 3.5
    as.load_double(11, 0.0);              // Dbl 0.0
    as.R6(OpCode::DIV, 12, 10, 11);       // 3.5 / 0.0 -> +inf (no trap)
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== num_promotion (int<->double) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, 0, &as.constant_pool());
        auto* regs = res.get_reg_base();
        const bool r2_ok  = regs[2].isDouble()  && approx(regs[2].asDouble(), 2.5);
        const bool r3_ok  = regs[3].isDouble()  && approx(regs[3].asDouble(), 2.5);
        const bool r5_ok  = regs[5].isInt()     && regs[5].asSigned48() == 42;
        const bool r8_ok  = regs[8].isInt()     && regs[8].asSigned48() == 3;
        const bool r10_ok = regs[10].isDouble() && approx(regs[10].asDouble(), 3.5);
        const bool r12_ok = regs[12].isDouble() && std::isinf(regs[12].asDouble());
        std::cout << std::format("2+0.5={} 0.5+2={} 2+40={} 7/2={} 7/2.0={} 3.5/0.0={}\n",
                                 regs[2], regs[3], regs[5], regs[8], regs[10], regs[12]);
        const bool ok = r2_ok && r3_ok && r5_ok && r8_ok && r10_ok && r12_ok;
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_type_checks -- IS_INT / IS_DOUBLE / IS_BOOL / IS_NIL / IS_UNDEF. Also
// exercises loading Bool / Nil / Undefined Values through the constant pool.
// =============================================================================
inline void test_type_checks() {
    Assembler as;
    as.label("main");
    as.C2(OpCode::LOAD_CONST, 0, 7);                    // Int
    as.load_double(1, 1.5);                             // Double
    as.load_constant(2, Value::fromBool(true));         // Bool
    as.load_constant(3, Value::fromNil());              // Nil
    as.load_constant(4, Value::fromUndefined());        // Undefined
    as.R6(OpCode::IS_INT,    10, 0, 0);   // true
    as.R6(OpCode::IS_INT,    11, 1, 0);   // false
    as.R6(OpCode::IS_DOUBLE, 12, 1, 0);   // true
    as.R6(OpCode::IS_DOUBLE, 13, 0, 0);   // false
    as.R6(OpCode::IS_BOOL,   14, 2, 0);   // true
    as.R6(OpCode::IS_NIL,    15, 3, 0);   // true
    as.R6(OpCode::IS_NIL,    16, 0, 0);   // false
    as.R6(OpCode::IS_UNDEF,  17, 4, 0);   // true
    as.R6(OpCode::IS_UNDEF,  18, 0, 0);   // false
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== type_checks (is_int/double/bool/nil/undef) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, 0, &as.constant_pool());
        auto* regs = res.get_reg_base();
        const bool ok =
            regs[10].asBool() == true  && regs[11].asBool() == false &&
            regs[12].asBool() == true  && regs[13].asBool() == false &&
            regs[14].asBool() == true  &&
            regs[15].asBool() == true  && regs[16].asBool() == false &&
            regs[17].asBool() == true  && regs[18].asBool() == false;
        std::cout << std::format(
            "IS_INT(7)={} IS_INT(1.5)={} IS_DOUBLE(1.5)={} IS_BOOL(true)={} "
            "IS_NIL(nil)={} IS_UNDEF(undef)={}\n",
            regs[10].asBool(), regs[11].asBool(), regs[12].asBool(),
            regs[14].asBool(), regs[15].asBool(), regs[17].asBool());
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_to_bool -- TO_BOOL coerces any Value to its truth value per the committed
// truthiness rules. Falsy: false, Int(0), Double(0.0) (incl. normalized -0.0),
// Nil, Undefined. Truthy: everything else -- true, nonzero int/double, NaN, atoms.
// =============================================================================
inline void test_to_bool() {
    Assembler as;
    as.label("main");
    as.load_constant(0, Value::fromBool(false));       // -> false
    as.load_constant(1, Value::fromBool(true));        // -> true
    as.C2(OpCode::LOAD_CONST, 2, 0);                   // Int(0)   -> false
    as.C2(OpCode::LOAD_CONST, 3, 5);                   // Int(5)   -> true
    as.C2(OpCode::LOAD_CONST, 4, -3);                  // Int(-3)  -> true
    as.load_double(5, 0.0);                            // 0.0      -> false
    as.load_double(6, -0.0);                           // -0.0     -> false (normalized)
    as.load_double(7, 2.5);                            // 2.5      -> true
    as.load_constant(8, Value::nan());                 // NaN      -> true
    as.load_constant(9, Value::fromNil());             // nil      -> false
    as.load_constant(10, Value::fromUndefined());      // undef    -> false
    as.load_constant(11, Value::fromAtom(7));          // :atom    -> true
    for (uint8_t i = 0; i <= 11; ++i)
        as.R6(OpCode::TO_BOOL, static_cast<uint8_t>(20 + i), i, 0);
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== to_bool (truthiness coercion) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, 0, &as.constant_pool());
        auto* regs = res.get_reg_base();
        const bool ok =
            regs[20].asBool() == false && regs[21].asBool() == true  &&
            regs[22].asBool() == false && regs[23].asBool() == true  &&
            regs[24].asBool() == true  &&
            regs[25].asBool() == false && regs[26].asBool() == false &&
            regs[27].asBool() == true  && regs[28].asBool() == true  &&  // NaN is truthy
            regs[29].asBool() == false && regs[30].asBool() == false &&
            regs[31].asBool() == true;
        std::cout << std::format(
            "false={} true={} Int0={} Int5={} 0.0={} NaN={} nil={} undef={} atom={}\n",
            regs[20].asBool(), regs[21].asBool(), regs[22].asBool(), regs[23].asBool(),
            regs[25].asBool(), regs[28].asBool(), regs[29].asBool(), regs[30].asBool(),
            regs[31].asBool());
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_lnot -- LNOT is the exact negation of TO_BOOL (`!x` with truthiness). Same
// value table as test_to_bool, results inverted: falsy -> true, truthy -> false.
// The generic counterpart of the bool-only NOT_BOOL fast path.
// =============================================================================
inline void test_lnot() {
    Assembler as;
    as.label("main");
    as.load_constant(0, Value::fromBool(false));       // -> true
    as.load_constant(1, Value::fromBool(true));        // -> false
    as.C2(OpCode::LOAD_CONST, 2, 0);                   // Int(0)   -> true
    as.C2(OpCode::LOAD_CONST, 3, 5);                   // Int(5)   -> false
    as.C2(OpCode::LOAD_CONST, 4, -3);                  // Int(-3)  -> false
    as.load_double(5, 0.0);                            // 0.0      -> true
    as.load_double(6, -0.0);                           // -0.0     -> true (normalized)
    as.load_double(7, 2.5);                            // 2.5      -> false
    as.load_constant(8, Value::nan());                 // NaN      -> false (NaN is truthy)
    as.load_constant(9, Value::fromNil());             // nil      -> true
    as.load_constant(10, Value::fromUndefined());      // undef    -> true
    as.load_constant(11, Value::fromAtom(7));          // :atom    -> false
    for (uint8_t i = 0; i <= 11; ++i)
        as.R6(OpCode::LNOT, static_cast<uint8_t>(20 + i), i, 0);
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== lnot (!x with truthiness) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, 0, &as.constant_pool());
        auto* regs = res.get_reg_base();
        const bool ok =
            regs[20].asBool() == true  && regs[21].asBool() == false &&
            regs[22].asBool() == true  && regs[23].asBool() == false &&
            regs[24].asBool() == false &&
            regs[25].asBool() == true  && regs[26].asBool() == true  &&
            regs[27].asBool() == false && regs[28].asBool() == false &&  // !NaN is false
            regs[29].asBool() == true  && regs[30].asBool() == true  &&
            regs[31].asBool() == false;
        std::cout << std::format(
            "!false={} !true={} !Int0={} !Int5={} !0.0={} !NaN={} !nil={} !undef={} !atom={}\n",
            regs[20].asBool(), regs[21].asBool(), regs[22].asBool(), regs[23].asBool(),
            regs[25].asBool(), regs[28].asBool(), regs[29].asBool(), regs[30].asBool(),
            regs[31].asBool());
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_equality -- the general EQ / NE ops (Value::operator==). Correct for EVERY
// type: atom/nil/bool bit-identity, numeric promotion (Int(1) == Double(1.0)),
// NaN != NaN -- and crucially an atom is NOT equal to an int that shares its low 48
// bits (EQ compares the full tagged value, unlike the int-only SET_EQ).
// =============================================================================
inline void test_equality() {
    Assembler as;
    as.label("main");
    as.load_constant(0, Value::fromAtom(5));           // :atom#5
    as.load_constant(1, Value::fromAtom(5));           // :atom#5 (same id)
    as.load_constant(2, Value::fromAtom(9));           // :atom#9 (different)
    // fromAtom(5) low 48 bits = (5<<8)|3 = 0x503 = 1283. An Int(1283) shares those low
    // bits but has a different tag -> must compare UNEQUAL.
    as.C2(OpCode::LOAD_CONST, 3, 1283);                // Int(1283)
    as.C2(OpCode::LOAD_CONST, 4, 1);                   // Int(1)
    as.load_double(5, 1.0);                            // Double(1.0)
    as.load_constant(6, Value::fromNil());             // nil
    as.load_constant(7, Value::fromUndefined());       // undefined
    as.load_constant(8, Value::fromBool(true));        // true
    as.load_constant(9, Value::fromBool(false));       // false
    as.load_constant(10, Value::nan());                // NaN
    as.load_constant(11, Value::nan());                // NaN
    as.R6(OpCode::EQ, 20, 0, 1);   // atom==atom (same)   -> true
    as.R6(OpCode::EQ, 21, 0, 2);   // atom==atom (diff)   -> false
    as.R6(OpCode::EQ, 22, 0, 3);   // atom==int low-bits  -> false (tag differs)
    as.R6(OpCode::EQ, 23, 4, 5);   // Int(1)==Double(1.0) -> true  (promotion)
    as.R6(OpCode::EQ, 24, 6, 6);   // nil==nil            -> true
    as.R6(OpCode::EQ, 25, 6, 7);   // nil==undefined      -> false
    as.R6(OpCode::EQ, 26, 8, 8);   // true==true          -> true
    as.R6(OpCode::EQ, 27, 8, 9);   // true==false         -> false
    as.R6(OpCode::EQ, 28, 10, 11); // NaN==NaN            -> false
    as.R6(OpCode::NE, 29, 0, 2);   // atom!=atom (diff)   -> true
    as.R6(OpCode::NE, 30, 10, 11); // NaN!=NaN            -> true
    as.R6(OpCode::NE, 31, 0, 1);   // atom!=atom (same)   -> false
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== equality (general EQ / NE) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, 0, &as.constant_pool());
        auto* regs = res.get_reg_base();
        const bool ok =
            regs[20].asBool() == true  && regs[21].asBool() == false &&
            regs[22].asBool() == false &&                                   // atom != int (tag!)
            regs[23].asBool() == true  &&                                   // promotion
            regs[24].asBool() == true  && regs[25].asBool() == false &&
            regs[26].asBool() == true  && regs[27].asBool() == false &&
            regs[28].asBool() == false &&                                   // NaN == NaN is false
            regs[29].asBool() == true  && regs[30].asBool() == true  &&     // NaN != NaN is true
            regs[31].asBool() == false;
        std::cout << std::format(
            "atom==atom={} atom!=int={} 1==1.0={} nil==nil={} true==true={} NaN==NaN={} NaN!=NaN={}\n",
            regs[20].asBool(), !regs[22].asBool(), regs[23].asBool(), regs[24].asBool(),
            regs[26].asBool(), regs[28].asBool(), regs[30].asBool());
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_func_value -- the Func immediate (a first-class SCRIPT function value): a
// GC-invisible TAG_SPECIAL sub-tag carrying a 16-bit function-table id. Verifies
// the id round-trip, that it is distinct from the neighbouring Atom sub-tag and
// from the pointer tags (so the GC never treats it as a heap object), its
// truthiness, identity-by-id equality, and its formatter rendering. This is the
// C1 slice of the closures work -- no opcode yet, just the value representation.
// =============================================================================
inline void test_func_value() {
    std::cout << "=== func value (first-class function immediate) ===\n";
    const Value f5  = Value::fromFunc(5);
    const Value f5b = Value::fromFunc(5);
    const Value f6  = Value::fromFunc(6);
    const Value fmax = Value::fromFunc(0xFFFF);
    try {
        const bool ok =
            f5.isFunc() && f5.asFuncId() == 5 &&
            fmax.asFuncId() == 0xFFFF &&
            f5.type() == Value::Type::Func &&
            // GC-invisible + not confused with the Atom sub-tag that shares low bits
            !f5.isPtr() && !f5.isFuncPtr() && !f5.isAtom() && !f5.isNil() &&
            !Value::fromAtom(5).isFunc() &&
            f5.isTruthy() &&
            // identity by id: same id equal, different id unequal, atom!=func
            (f5 == f5b) && !(f5 == f6) && !(f5 == Value::fromAtom(5)) &&
            // formatter
            std::format("{}", f6) == "Func(#6)";
        std::cout << std::format("f5={} f6={} fmax_id={}\n", f5, f6, fmax.asFuncId());
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_function_table -- the C2 slice: the assembler builds a per-function side
// table (indexed by fn id) from declare_fn() registrations, resolving each entry's
// code_offset from its label during assemble(). No opcode reads it yet (that is
// C4/CALL_INDIRECT); this verifies the table is assembled correctly and that
// func_id() hands back the ids the runtime will index with.
// =============================================================================
inline void test_function_table() {
    std::cout << "=== function table (C2: per-function side table build) ===\n";
    Assembler as;
    // Two first-class-declared functions with distinct frame sizes / arities.
    const uint16_t id_f = as.declare_fn("f", /*frame_size*/ 2, /*arity*/ 1);
    const uint16_t id_g = as.declare_fn("g", /*frame_size*/ 3, /*arity*/ 2);
    as.label("main");
    as.J(OpCode::HALT);
    const uint32_t off_f = static_cast<uint32_t>(as.instruction_count());
    as.label("f");
    as.C2(OpCode::LOAD_CONST, 0, 0);
    as.J(OpCode::RET);
    const uint32_t off_g = static_cast<uint32_t>(as.instruction_count());
    as.label("g");
    as.C2(OpCode::LOAD_CONST, 0, 0);
    as.J(OpCode::RET);
    try {
        (void)as.assemble();                 // resolves each entry's code_offset
        const auto& tab = as.function_table();
        const bool ok =
            tab.size() == 2 &&
            as.func_id("f") == id_f && as.func_id("g") == id_g &&
            tab[id_f].code_offset == off_f && tab[id_f].frame_size == 2 &&
            tab[id_f].arity == 1 && tab[id_f].ncaptures == 0 &&
            tab[id_f].self_slot == FnInfo::NO_SELF &&
            tab[id_g].code_offset == off_g && tab[id_g].frame_size == 3 &&
            tab[id_g].arity == 2 && tab[id_g].ncaptures == 0;
        std::cout << std::format("f: off={} fs={} arity={}; g: off={} fs={} arity={}\n",
            tab[id_f].code_offset, tab[id_f].frame_size, tab[id_f].arity,
            tab[id_g].code_offset, tab[id_g].frame_size, tab[id_g].arity);
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_load_fn -- the C3 slice: the LOAD_FN opcode materializes a first-class
// function value (a Func immediate) from a function-table id. Verifies the value
// lands in the register as a Func carrying the id declare_fn() assigned. The call
// side (CALL_INDIRECT) is C4; here we only check the value is produced.
// =============================================================================
inline void test_load_fn() {
    Assembler as;
    const uint16_t id_f = as.declare_fn("f", /*frame_size*/ 1, /*arity*/ 0);
    const uint16_t id_g = as.declare_fn("g", /*frame_size*/ 1, /*arity*/ 0);
    as.label("main");
    as.LOAD_FN(0, id_f);          // r0 = Func(#f)
    as.LOAD_FN(1, id_g);          // r1 = Func(#g)
    as.J(OpCode::HALT);
    as.label("f");
    as.J(OpCode::RET);
    as.label("g");
    as.J(OpCode::RET);
    const auto bytecode = as.assemble();
    std::cout << "=== load_fn (first-class function value) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, 0,
                             nullptr, nullptr, nullptr, nullptr, &as.function_table());
        auto* regs = res.get_reg_base();
        const bool ok =
            regs[0].isFunc() && regs[0].asFuncId() == id_f &&
            regs[1].isFunc() && regs[1].asFuncId() == id_g &&
            !(regs[0] == regs[1]);               // distinct function values
        std::cout << std::format("r0={} r1={}\n", regs[0], regs[1]);
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_call_indirect -- the C4 slice: CALL_INDIRECT calls a function VALUE held in
// a register (a Func immediate), recovering the callee's frame size + entry point
// from the function table at run time. Two programs:
//   (A) higher-order: one apply(f, x) = f(x) dispatches to DIFFERENT functions
//       (inc / dbl) purely by the Func value passed in -- the essence of
//       first-class functions.
//   (B) indirect self-recursion: fact loads its own Func via LOAD_FN and calls
//       itself with CALL_INDIRECT -- recursion routed through the function table.
// The immediate path only; the KIND_CLOSURE path is Milestone A.
// =============================================================================
inline void test_call_indirect() {
    std::cout << "=== call_indirect (first-class / higher-order / indirect recursion) ===\n";
    bool ok = true;

    // ---- (A) higher-order apply(f, x) = f(x) ----
    try {
        Assembler as;
        const uint16_t id_inc   = as.declare_fn("inc",   /*fs*/ 2, /*arity*/ 1);
        const uint16_t id_dbl   = as.declare_fn("dbl",   /*fs*/ 2, /*arity*/ 1);
        (void)                    as.declare_fn("apply", /*fs*/ 2, /*arity*/ 2);
        as.label("main");                       // top_frame_size = 2 (args at r2, r3)
        as.LOAD_FN(2, id_inc);                  // arg0 = Func(inc)
        as.C2(OpCode::LOAD_CONST, 3, 20);       // arg1 = 20
        as.CALL("apply");                       // apply(inc, 20) -> r2
        as.R6(OpCode::MOV, 0, 2, 0);            // r0 = 21
        as.LOAD_FN(2, id_dbl);                  // arg0 = Func(dbl)
        as.C2(OpCode::LOAD_CONST, 3, 21);       // arg1 = 21
        as.CALL("apply");                       // apply(dbl, 21) -> r2
        as.R6(OpCode::MOV, 1, 2, 0);            // r1 = 42
        as.J(OpCode::HALT);
        as.label("apply");                      // apply(f=r0, x=r1)
        as.R6(OpCode::MOV, 2, 1, 0);            // arg = x at r[frame_size]=r2
        as.CALL_INDIRECT(0, 1);                 // f(x); result at r2
        as.R6(OpCode::MOV, 0, 2, 0);            // result -> r0
        as.J(OpCode::RET);
        as.label("inc");                        // inc(x) = x + 1
        as.C2(OpCode::LOAD_CONST, 1, 1);
        as.R6(OpCode::ADD_INT, 0, 0, 1);
        as.J(OpCode::RET);
        as.label("dbl");                        // dbl(x) = x + x
        as.R6(OpCode::ADD_INT, 0, 0, 0);
        as.J(OpCode::RET);
        const auto bc = as.assemble();
        Disassembler dis(bc.data(), bc.size()); dis.add_label(0, "main"); dis.print();
        auto res   = execute(bc, nullptr, nullptr, nullptr, 2,
                             nullptr, nullptr, nullptr, nullptr, &as.function_table());
        auto* regs = res.get_reg_base();
        std::cout << std::format("apply(inc,20)={}  apply(dbl,21)={}\n",
                                 regs[0].asSigned48(), regs[1].asSigned48());
        ok = ok && regs[0].asSigned48() == 21 && regs[1].asSigned48() == 42;
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    // ---- (B) indirect self-recursion: fact(5) via LOAD_FN + CALL_INDIRECT ----
    try {
        Assembler as;
        const uint16_t id_fact = as.declare_fn("fact", /*fs*/ 3, /*arity*/ 1);
        as.label("main");                       // top_frame_size = 1 (arg at r1)
        as.LOAD_FN(0, id_fact);                 // r0 = Func(fact)
        as.C2(OpCode::LOAD_CONST, 1, 5);        // arg = 5 at r1
        as.CALL_INDIRECT(0, 1);                 // fact(5); result at r1
        as.J(OpCode::HALT);
        as.label("fact");                       // fact(n=r0)
        as.C2(OpCode::LOAD_CONST, 1, 1);
        as.B(OpCode::BGE_INT, 1, 0, "fact_base"); // if 1 >= n -> base
        as.R6(OpCode::SUB_INT, 3, 0, 1);        // r3 = n - 1 (arg at r[frame_size]=r3)
        as.LOAD_FN(2, id_fact);                 // r2 = Func(fact)
        as.CALL_INDIRECT(2, 1);                 // fact(n-1); result at r3
        as.R6(OpCode::MUL_INT, 0, 0, 3);        // r0 = n * fact(n-1)
        as.J(OpCode::RET);
        as.label("fact_base");
        as.C2(OpCode::LOAD_CONST, 0, 1);
        as.J(OpCode::RET);
        const auto bc = as.assemble();
        Disassembler dis(bc.data(), bc.size()); dis.add_label(0, "main"); dis.print();
        auto res   = execute(bc, nullptr, nullptr, nullptr, 1,
                             nullptr, nullptr, nullptr, nullptr, &as.function_table());
        const int64_t fact5 = res.get_reg_base()[1].asSigned48();
        std::cout << std::format("fact(5) via indirect recursion = {}\n", fact5);
        ok = ok && fact5 == 120;
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    check(ok);
}

// =============================================================================
// test_proto_resolve -- the trait-dispatch VM primitive (Slice 1). PROTO_RESOLVE
// folds the receiver's runtime type into one dense key, indexes the flat trait
// table (fn_id[method_id * width + dense_id]) and yields a Func value, which the
// following CALL_INDIRECT dispatches like any first-class function. Three parts:
//   (A/B) SINGLE method `show` (id 0) with TWO impls -- Show for Point (a struct,
//         show(p) = p.x + p.y) and Show for Int (show(x) = x * 2). One program
//         resolves+calls show on a Point AND on an Int; correct results prove the
//         dense-key dispatch reaches the right impl across the struct/immediate
//         type worlds.
//   (C)   MISS: resolving `show` on a Nil receiver (a type with no impl) traps
//         with "No trait implementation for type".
// The table is hand-built here (the compiler builds it in a later slice).
// =============================================================================
inline void test_proto_resolve() {
    std::cout << "=== proto_resolve (trait dispatch: struct + immediate, and miss) ===\n";
    bool ok = true;

    // ---- (A/B) dispatch `show` to Show-for-Point and Show-for-Int ----
    try {
        Heap heap(64 * 1024);
        Assembler as;
        const uint16_t Point   = as.define_struct("Point", { "x", "y" });
        const uint16_t sx      = as.field("Point", "x");
        const uint16_t sy      = as.field("Point", "y");
        const uint16_t id_spt  = as.declare_fn("show_point", /*fs*/ 3, /*arity*/ 1);
        const uint16_t id_sint = as.declare_fn("show_int",   /*fs*/ 2, /*arity*/ 1);

        constexpr uint16_t M_SHOW = 0;          // the single global method id

        as.label("main");                       // top_frame_size = 4 (locals r0..r3, args at r4)
        // Point{1,2} in r0
        as.NEW_STRUCT(0, Point);
        as.load_const(1, 1); as.SET_PROP(0, sx, 1);
        as.load_const(1, 2); as.SET_PROP(0, sy, 1);
        // show(point) -> r0
        as.PROTO_RESOLVE(3, 0, M_SHOW);         // r3 = Func(show_point)
        as.R6(OpCode::MOV, 4, 0, 0);            // arg0 = point at r4
        as.CALL_INDIRECT(3, 1);                 // show_point(point) -> r4
        as.R6(OpCode::MOV, 0, 4, 0);            // r0 = 3
        // show(21) -> r1
        as.load_const(1, 21);
        as.PROTO_RESOLVE(3, 1, M_SHOW);         // r3 = Func(show_int)
        as.R6(OpCode::MOV, 4, 1, 0);            // arg0 = 21 at r4
        as.CALL_INDIRECT(3, 1);                 // show_int(21) -> r4
        as.R6(OpCode::MOV, 1, 4, 0);            // r1 = 42
        as.J(OpCode::HALT);

        as.label("show_point");                 // show_point(p=r0) = p.x + p.y
        as.GET_PROP(1, 0, sx);
        as.GET_PROP(2, 0, sy);
        as.R6(OpCode::ADD_INT, 0, 1, 2);
        as.J(OpCode::RET);

        as.label("show_int");                   // show_int(x=r0) = x * 2
        as.R6(OpCode::ADD_INT, 0, 0, 0);
        as.J(OpCode::RET);

        const auto bc = as.assemble();

        // Build the trait table: width = BUILTIN_COUNT + #structs; one method row.
        const uint32_t width = BUILTIN_COUNT + static_cast<uint32_t>(as.struct_types().size());
        const uint32_t methods = 1;
        std::vector<uint16_t> trait_table(static_cast<size_t>(width) * methods, TRAIT_METHOD_NONE);
        trait_table[M_SHOW * width + TID_INT]                 = id_sint;   // Show for Int
        trait_table[M_SHOW * width + (BUILTIN_COUNT + Point)] = id_spt;    // Show for Point

        Disassembler dis(bc.data(), bc.size()); dis.add_label(0, "main"); dis.print();
        auto res = execute(bc, &heap, nullptr, nullptr, /*top*/ 4,
                           &as.constant_pool(), &as.struct_types(), nullptr, nullptr,
                           &as.function_table(), nullptr,
                           &trait_table, width, methods);
        auto* regs = res.get_reg_base();
        std::cout << std::format("show(Point{{1,2}})={}  show(21)={}\n",
                                 regs[0].asSigned48(), regs[1].asSigned48());
        ok = ok && regs[0].asSigned48() == 3 && regs[1].asSigned48() == 42;
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    // ---- (C) miss: show on a Nil receiver traps ----
    try {
        Assembler as;
        constexpr uint16_t M_SHOW = 0;
        as.label("main");                        // fs = 2, no CALL slide needed
        as.load_constant(0, Value::fromNil());   // r0 = nil (dense = TID_NIL, no impl)
        as.PROTO_RESOLVE(1, 0, M_SHOW);          // -> miss -> trap
        as.J(OpCode::HALT);
        const auto bc = as.assemble();

        const uint32_t width   = BUILTIN_COUNT;  // no structs here
        const uint32_t methods = 1;
        std::vector<uint16_t> trait_table(static_cast<size_t>(width) * methods, TRAIT_METHOD_NONE);
        trait_table[M_SHOW * width + TID_INT] = 0;   // only Int implemented; Nil is a miss

        bool trapped = false;
        try {
            execute(bc, nullptr, nullptr, nullptr, /*top*/ 2,
                    &as.constant_pool(), nullptr, nullptr, nullptr, nullptr, nullptr,
                    &trait_table, width, methods);
        } catch (const std::exception& e) {
            trapped = true;
            std::cout << std::format("miss trapped as expected: {}\n", e.what());
        }
        ok = ok && trapped;
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    // ---- (D) dispatch on a KIND_VEC receiver (the TID_VEC dense column) ----
    try {
        Heap heap(64 * 1024);
        Assembler as;
        const uint16_t id_vlen = as.declare_fn("vlen", /*fs*/ 2, /*arity*/ 1);
        constexpr uint16_t M_LEN = 0;

        as.label("main");                       // top_frame_size = 4
        as.VEC_NEW(0);                          // r0 = vec()
        as.load_const(1, 10); as.VEC_PUSH(0, 1);
        as.load_const(1, 20); as.VEC_PUSH(0, 1);
        as.load_const(1, 30); as.VEC_PUSH(0, 1);
        as.PROTO_RESOLVE(3, 0, M_LEN);          // r3 = Func(vlen) via the TID_VEC column
        as.R6(OpCode::MOV, 4, 0, 0);            // arg0 = vec at r4
        as.CALL_INDIRECT(3, 1);                 // vlen(vec) -> r4
        as.R6(OpCode::MOV, 0, 4, 0);            // r0 = 3
        as.J(OpCode::HALT);

        as.label("vlen");                       // vlen(v=r0) = len(v)
        as.R6(OpCode::LEN, 0, 0, 0);
        as.J(OpCode::RET);

        const auto bc = as.assemble();
        const uint32_t width   = BUILTIN_COUNT;  // no structs
        const uint32_t methods = 1;
        std::vector<uint16_t> trait_table(static_cast<size_t>(width) * methods, TRAIT_METHOD_NONE);
        trait_table[M_LEN * width + TID_VEC] = id_vlen;   // impl for Vec

        auto res = execute(bc, &heap, nullptr, nullptr, /*top*/ 4,
                           &as.constant_pool(), &as.struct_types(), nullptr, nullptr,
                           &as.function_table(), nullptr,
                           &trait_table, width, methods);
        auto* regs = res.get_reg_base();
        std::cout << std::format("vlen(vec[10,20,30])={}\n", regs[0].asSigned48());
        ok = ok && regs[0].asSigned48() == 3;
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    check(ok);
}

// =============================================================================
// test_resolve_call -- the FUSED PROTO_RESOLVE + CALL_INDIRECT primitive
// (RESOLVE_CALL, id 112). Identical dispatch to test_proto_resolve, but a SINGLE
// op: the receiver is the outgoing arg 0 (window[frame_size]), so no Func register
// and no separate resolve. Parts:
//   (A/B) `show` (id 0) with two impls -- Show for Point (show(p)=p.x+p.y) and
//         Show for Int (show(x)=x*2). One program fuse-dispatches show on a Point
//         AND on an Int -> the dense-key dispatch reaches the right impl across the
//         struct/immediate worlds through the fused op.
//   (C)   MISS: fused show on a Nil receiver (no impl) traps, exactly like
//         PROTO_RESOLVE's miss.
// =============================================================================
inline void test_resolve_call() {
    std::cout << "=== resolve_call (FUSED resolve+call: struct + immediate, and miss) ===\n";
    bool ok = true;

    // ---- (A/B) fuse-dispatch `show` to Show-for-Point and Show-for-Int ----
    try {
        Heap heap(64 * 1024);
        Assembler as;
        const uint16_t Point   = as.define_struct("Point", { "x", "y" });
        const uint16_t sx      = as.field("Point", "x");
        const uint16_t sy      = as.field("Point", "y");
        const uint16_t id_spt  = as.declare_fn("show_point", /*fs*/ 3, /*arity*/ 1);
        const uint16_t id_sint = as.declare_fn("show_int",   /*fs*/ 2, /*arity*/ 1);
        constexpr uint16_t M_SHOW = 0;

        as.label("main");                       // top_frame_size = 4 (locals r0..r3, args at r4)
        as.NEW_STRUCT(0, Point);
        as.load_const(1, 1); as.SET_PROP(0, sx, 1);
        as.load_const(1, 2); as.SET_PROP(0, sy, 1);
        // show(point): place the receiver as arg 0 at r4, then ONE fused op resolves + calls.
        as.R6(OpCode::MOV, 4, 0, 0);            // arg0 = point at r4 (= window[frame_size])
        as.RESOLVE_CALL(1, M_SHOW);             // resolve show for typeof(r4)=Point, call -> r4
        as.R6(OpCode::MOV, 0, 4, 0);            // r0 = 3
        // show(21): arg0 = 21 at r4
        as.load_const(1, 21);
        as.R6(OpCode::MOV, 4, 1, 0);
        as.RESOLVE_CALL(1, M_SHOW);             // resolve show for Int, call -> r4
        as.R6(OpCode::MOV, 1, 4, 0);            // r1 = 42
        as.J(OpCode::HALT);

        as.label("show_point");                 // show_point(p=r0) = p.x + p.y
        as.GET_PROP(1, 0, sx);
        as.GET_PROP(2, 0, sy);
        as.R6(OpCode::ADD_INT, 0, 1, 2);
        as.J(OpCode::RET);

        as.label("show_int");                   // show_int(x=r0) = x * 2
        as.R6(OpCode::ADD_INT, 0, 0, 0);
        as.J(OpCode::RET);

        const auto bc = as.assemble();
        const uint32_t width   = BUILTIN_COUNT + static_cast<uint32_t>(as.struct_types().size());
        const uint32_t methods = 1;
        std::vector<uint16_t> trait_table(static_cast<size_t>(width) * methods, TRAIT_METHOD_NONE);
        trait_table[M_SHOW * width + TID_INT]                 = id_sint;   // Show for Int
        trait_table[M_SHOW * width + (BUILTIN_COUNT + Point)] = id_spt;    // Show for Point

        Disassembler dis(bc.data(), bc.size()); dis.add_label(0, "main"); dis.print();
        auto res = execute(bc, &heap, nullptr, nullptr, /*top*/ 4,
                           &as.constant_pool(), &as.struct_types(), nullptr, nullptr,
                           &as.function_table(), nullptr,
                           &trait_table, width, methods);
        auto* regs = res.get_reg_base();
        std::cout << std::format("show(Point{{1,2}})={}  show(21)={}\n",
                                 regs[0].asSigned48(), regs[1].asSigned48());
        ok = ok && regs[0].asSigned48() == 3 && regs[1].asSigned48() == 42;
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    // ---- (C) miss: fused show on a Nil receiver traps ----
    try {
        Assembler as;
        constexpr uint16_t M_SHOW = 0;
        as.label("main");                        // top_frame_size = 2 (locals r0,r1, arg at r2)
        as.load_constant(0, Value::fromNil());   // r0 = nil (dense = TID_NIL, no impl)
        as.R6(OpCode::MOV, 2, 0, 0);             // arg0 = nil at r2 (= window[frame_size])
        as.RESOLVE_CALL(1, M_SHOW);              // -> miss -> trap
        as.J(OpCode::HALT);
        const auto bc = as.assemble();

        const uint32_t width   = BUILTIN_COUNT;  // no structs here
        const uint32_t methods = 1;
        std::vector<uint16_t> trait_table(static_cast<size_t>(width) * methods, TRAIT_METHOD_NONE);
        trait_table[M_SHOW * width + TID_INT] = 0;   // only Int implemented; Nil is a miss

        bool trapped = false;
        try {
            execute(bc, nullptr, nullptr, nullptr, /*top*/ 2,
                    &as.constant_pool(), nullptr, nullptr, nullptr, nullptr, nullptr,
                    &trait_table, width, methods);
        } catch (const std::exception& e) {
            trapped = true;
            std::cout << std::format("miss trapped as expected: {}\n", e.what());
        }
        ok = ok && trapped;
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    check(ok);
}

// =============================================================================
// test_resolve_tco_call -- the FUSED tail resolve-and-call (RESOLVE_TCO_CALL, id
// 113). The tail sibling of RESOLVE_CALL: it resolves a trait method for the
// receiver AND reuses the current frame in place. Two scenarios:
//   (A) O(1)-stack tail recursion THROUGH the fused op. step(n, acc) is a trait
//       method (M_STEP) whose Int impl tail-recurses via RESOLVE_TCO_CALL down to
//       n==0, summing 1..N. The receiver = arg 0 stays an Int every iteration, so
//       each pass re-resolves M_STEP for TID_INT. N = 1,000,000 >> the 16384-frame
//       return stack, so completing at all proves the frame is REUSED (no push) --
//       exactly the TCO guarantee, but with resolution fused in. main kicks it off
//       with a NON-tail RESOLVE_CALL (which pushes the one frame the final RET pops).
//   (B) a miss: RESOLVE_TCO_CALL on a Nil receiver (TID_NIL has no impl) traps
//       BEFORE any frame reuse -- it reads the receiver at window[0] and resolves.
// The ONE asymmetry vs RESOLVE_CALL is exercised here: the receiver is read at
// window[0] (reused-frame arg 0), not window[frame_size].
// =============================================================================
inline void test_resolve_tco_call() {
    std::cout << "=== resolve_tco_call (FUSED tail resolve+call: O(1) stack, and miss) ===\n";
    bool ok = true;

    // ---- (A) sum 1..N via a trait method tail-recursing through RESOLVE_TCO_CALL ----
    try {
        constexpr int64_t N = 1'000'000;
        Heap heap(64 * 1024);
        Assembler as;
        // step(n=r0, acc=r1): frame 3 (r2 = scratch for the base-case compare). The tail args
        // (n', acc') are already in [0, 2) after the ADD/DECR, so the reused frame needs no MOVs.
        const uint16_t id_step = as.declare_fn("step", /*fs*/ 3, /*arity*/ 2);
        constexpr uint16_t M_STEP = 0;

        as.label("main");                          // top_frame_size = 2 (locals r0,r1; args at r2,r3)
        as.load_constant(0, Value::fromSigned48(N)); // r0 = N
        as.C2(OpCode::LOAD_CONST, 1, 0);           // r1 = 0 (acc)
        as.R6(OpCode::MOV, 2, 0, 0);               // arg0 = N at r2 (= window[frame_size])
        as.R6(OpCode::MOV, 3, 1, 0);               // arg1 = 0 at r3
        as.RESOLVE_CALL(2, M_STEP);                // NON-tail: push a frame, resolve step for Int, call
        as.R6(OpCode::MOV, 0, 2, 0);               // r0 = result (at window[frame_size])
        as.J(OpCode::HALT);

        as.label("step");                          // step(n=r0, acc=r1)
        as.C2(OpCode::LOAD_CONST, 2, 0);           // r2 = 0
        as.B(OpCode::BEQ_INT, 0, 2, "step_base");  // if n == 0 -> return acc
        as.R6(OpCode::ADD_INT, 1, 1, 0);           // acc' = acc + n   (reads OLD n first)
        as.C2(OpCode::DECR, 0);                    // n'   = n - 1     (in place; arg0 stays r0)
        as.RESOLVE_TCO_CALL(2, M_STEP);            // tail: reuse frame, resolve step for typeof(r0)=Int
        as.label("step_base");
        as.R6(OpCode::MOV, 0, 1, 0);               // return acc
        as.J(OpCode::RET);

        const auto bc = as.assemble();
        const uint32_t width   = BUILTIN_COUNT;    // no structs
        const uint32_t methods = 1;
        std::vector<uint16_t> trait_table(static_cast<size_t>(width) * methods, TRAIT_METHOD_NONE);
        trait_table[M_STEP * width + TID_INT] = id_step;   // step for Int

        Disassembler dis(bc.data(), bc.size()); dis.add_label(0, "main"); dis.print();
        auto res = execute(bc, &heap, nullptr, nullptr, /*top*/ 2,
                           &as.constant_pool(), nullptr, nullptr, nullptr,
                           &as.function_table(), nullptr,
                           &trait_table, width, methods);
        const int64_t result   = res.get_reg_base()[0].asSigned48();
        const int64_t expected = N * (N + 1) / 2;  // 500000500000
        std::cout << std::format("step-sum 1..{} = {} (expect {})\n", N, result, expected);
        ok = ok && result == expected;
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    // ---- (B) miss: fused tail step on a Nil receiver traps ----
    try {
        Assembler as;
        constexpr uint16_t M_STEP = 0;
        as.label("main");                          // top_frame_size = 1 (receiver = window[0])
        as.load_constant(0, Value::fromNil());     // r0 = nil (dense = TID_NIL, no impl)
        as.RESOLVE_TCO_CALL(1, M_STEP);            // reads window[0] = nil -> miss -> trap
        as.J(OpCode::HALT);
        const auto bc = as.assemble();

        const uint32_t width   = BUILTIN_COUNT;
        const uint32_t methods = 1;
        std::vector<uint16_t> trait_table(static_cast<size_t>(width) * methods, TRAIT_METHOD_NONE);
        trait_table[M_STEP * width + TID_INT] = 0;   // only Int implemented; Nil is a miss

        bool trapped = false;
        try {
            execute(bc, nullptr, nullptr, nullptr, /*top*/ 1,
                    &as.constant_pool(), nullptr, nullptr, nullptr, nullptr, nullptr,
                    &trait_table, width, methods);
        } catch (const std::exception& e) {
            trapped = true;
            std::cout << std::format("miss trapped as expected: {}\n", e.what());
        }
        ok = ok && trapped;
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    check(ok);
}

// =============================================================================
// test_make_closure -- MAKE_CLOSURE construction (Milestone A3). Builds a
// KIND_CLOSURE from bytecode and inspects the resulting heap object directly
// (calling it is A4). Two scenarios:
//   (A) plain captures: a 2-capture closure grabs r0/r1 into its payload slots.
//   (B) self-capture back-patch: a function declared with a self_slot has that
//       slot overwritten with the closure itself (the direct self-recursive-lambda
//       machinery, whose compiler side is A6). slots[self_slot] must point at the
//       closure object it lives in.
// The captured/closed-over functions have minimal RET bodies -- not exercised here.
// =============================================================================
inline void test_make_closure() {
    std::cout << "=== make_closure ===\n";
    bool ok = true;

    try {
        Heap heap(64 * 1024);

        Assembler as;
        // (A) 2 captures, no self slot. (B) 2 captures, self_slot = 1.
        const uint16_t plain = as.declare_fn("plain_fn", /*fs*/ 2, /*arity*/ 1, /*ncaptures*/ 2);
        const uint16_t selfy = as.declare_fn("self_fn",  /*fs*/ 2, /*arity*/ 0, /*ncaptures*/ 2,
                                             /*self_slot*/ 1);

        as.label("main");                         // top-level frame: r0..r3 live across the allocs
        as.load_const(0, 111);                    // r0 = capture 0
        as.load_const(1, 222);                    // r1 = capture 1
        as.MAKE_CLOSURE(2, plain, 0);             // r2 = closure(plain_fn) capturing r0, r1
        as.load_const(0, 333);                    // reuse r0 for scenario B's capture 0
        as.MAKE_CLOSURE(3, selfy, 0);             // r3 = closure(self_fn); slot1 back-patched to self
        as.J(OpCode::HALT);

        as.label("plain_fn");                     // minimal body (not called in A3)
        as.J(OpCode::RET);
        as.label("self_fn");
        as.J(OpCode::RET);

        const auto bytecode = as.assemble();
        Disassembler dis(bytecode.data(), bytecode.size());
        dis.add_label(0, "main");
        dis.print();

        // top_frame_size = 4: r0..r3 are GC roots during the MAKE_CLOSURE allocations
        // (r2 holds closure A across the second alloc's potential collection).
        auto res   = execute(bytecode, &heap, nullptr, nullptr, /*top*/ 4,
                             nullptr, nullptr, nullptr, nullptr, &as.function_table());
        auto* regs = res.get_reg_base();

        // --- (A) plain closure ---
        const Value a_val = regs[2];
        GcObject*   a     = a_val.isPtr() ? GcObject::from_slots(a_val.asPtr()) : nullptr;
        const bool a_ok = a &&
                          a->kind == GcObject::KIND_CLOSURE &&
                          a->closure_fn_id() == plain &&
                          a->slot_count() == 2 &&
                          a->slots()[0].asSigned48() == 111 &&
                          a->slots()[1].asSigned48() == 222;

        // --- (B) self-capturing closure ---
        const Value b_val = regs[3];
        GcObject*   b     = b_val.isPtr() ? GcObject::from_slots(b_val.asPtr()) : nullptr;
        const bool b_ok = b &&
                          b->kind == GcObject::KIND_CLOSURE &&
                          b->closure_fn_id() == selfy &&
                          b->slot_count() == 2 &&
                          b->slots()[0].asSigned48() == 333 &&
                          b->slots()[1].isPtr() &&
                          b->slots()[1].asPtr() == b_val.asPtr(); // slot1 == the closure itself

        std::cout << std::format("  (A) plain closure captures [r0,r1]:  {}\n", a_ok ? "PASS" : "FAIL");
        std::cout << std::format("  (B) self_slot back-patched to self:  {}\n", b_ok ? "PASS" : "FAIL");
        ok = a_ok && b_ok;
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    check(ok);
}

// =============================================================================
// test_load_capture -- CALLING a capturing closure end to end (Milestone A4):
// MAKE_CLOSURE builds it, CALL_INDIRECT branches on the KIND_CLOSURE tag and installs
// it as current_closure, and LOAD_CAPTURE reads its by-value captures inside the body.
//
//   combine(x) = x + cap0 + cap1, closed over (cap0=10, cap1=20). It is called TWICE
//   through the same closure value (combine(5)=35, combine(100)=130) to confirm
//   current_closure is installed on entry and restored on return each time.
// =============================================================================
inline void test_load_capture() {
    std::cout << "=== load_capture (calling a capturing closure) ===\n";
    bool ok = true;

    try {
        Heap heap(64 * 1024);

        Assembler as;
        // combine(x=r0) closes over 2 captures; body uses r0 (param), r1/r2 (capture temps).
        const uint16_t combine = as.declare_fn("combine", /*fs*/ 3, /*arity*/ 1, /*ncaptures*/ 2);

        as.label("main");                         // top_frame_size = 3 (args at r3)
        as.load_const(0, 10);                     // r0 = cap0
        as.load_const(1, 20);                     // r1 = cap1
        as.MAKE_CLOSURE(2, combine, 0);           // r2 = closure(combine) capturing r0,r1
        as.load_const(3, 5);                      // arg at r[top_frame_size]=r3
        as.CALL_INDIRECT(2, 1);                   // combine(5) -> r3
        as.R6(OpCode::MOV, 0, 3, 0);              // r0 = 35
        as.load_const(3, 100);                    // arg at r3
        as.CALL_INDIRECT(2, 1);                   // combine(100) -> r3 (same closure)
        as.R6(OpCode::MOV, 1, 3, 0);              // r1 = 130
        as.J(OpCode::HALT);

        as.label("combine");                      // combine(x=r0) = x + cap0 + cap1
        as.LOAD_CAPTURE(1, 0);                    // r1 = captures[0] = cap0
        as.LOAD_CAPTURE(2, 1);                    // r2 = captures[1] = cap1
        as.R6(OpCode::ADD_INT, 0, 0, 1);          // r0 = x + cap0
        as.R6(OpCode::ADD_INT, 0, 0, 2);          // r0 = x + cap0 + cap1
        as.J(OpCode::RET);

        const auto bytecode = as.assemble();
        Disassembler dis(bytecode.data(), bytecode.size());
        dis.add_label(0, "main");
        dis.print();

        auto res   = execute(bytecode, &heap, nullptr, nullptr, /*top*/ 3,
                             nullptr, nullptr, nullptr, nullptr, &as.function_table());
        auto* regs = res.get_reg_base();

        const int64_t r0 = regs[0].asSigned48();
        const int64_t r1 = regs[1].asSigned48();
        std::cout << std::format("combine(5)={} (expect 35)  combine(100)={} (expect 130)\n", r0, r1);
        ok = (r0 == 35) && (r1 == 130);
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    check(ok);
}

// =============================================================================
// test_closure_self_recursion -- a self-recursive closure end to end: the VM-side
// composition of A3 (the MAKE_CLOSURE self_slot back-patch) and A4 (LOAD_CAPTURE +
// the CALL_INDIRECT closure path). A6 is only the COMPILER emitting this pattern.
//
//   factc is declared with self_slot 0, so MAKE_CLOSURE stores the closure ITSELF
//   into capture slot 0. Its body recurses by LOAD_CAPTURE-ing that self reference
//   and CALL_INDIRECT-ing it: factc(n) = n <= 1 ? 1 : n * self(n-1). factc(5) = 120.
// =============================================================================
inline void test_closure_self_recursion() {
    std::cout << "=== closure_self_recursion ===\n";
    bool ok = true;

    try {
        Heap heap(64 * 1024);

        Assembler as;
        // 1 capture (slot 0 = self); body uses r0(n), r1(temp/self), r3(arg/result).
        const uint16_t factc = as.declare_fn("factc", /*fs*/ 3, /*arity*/ 1,
                                             /*ncaptures*/ 1, /*self_slot*/ 0);

        as.label("main");                         // top_frame_size = 2 (arg at r2)
        as.MAKE_CLOSURE(0, factc, 1);             // r0 = self-recursive closure (slot0 = self)
        as.load_const(2, 5);                      // arg at r[top]=r2
        as.CALL_INDIRECT(0, 1);                   // factc(5) -> r2
        as.J(OpCode::HALT);

        as.label("factc");                        // factc(n=r0)
        as.load_const(1, 1);
        as.B(OpCode::BGE_INT, 1, 0, "factc_base"); // if 1 >= n -> base (return 1)
        as.R6(OpCode::SUB_INT, 3, 0, 1);          // r3 = n - 1 (arg at r[frame_size]=r3)
        as.LOAD_CAPTURE(1, 0);                    // r1 = self (captures[0])
        as.CALL_INDIRECT(1, 1);                   // self(n-1) -> r3
        as.R6(OpCode::MUL_INT, 0, 0, 3);          // r0 = n * factc(n-1)
        as.J(OpCode::RET);
        as.label("factc_base");
        as.load_const(0, 1);                      // return 1
        as.J(OpCode::RET);

        const auto bytecode = as.assemble();
        Disassembler dis(bytecode.data(), bytecode.size());
        dis.add_label(0, "main");
        dis.print();

        auto res   = execute(bytecode, &heap, nullptr, nullptr, /*top*/ 2,
                             nullptr, nullptr, nullptr, nullptr, &as.function_table());
        const int64_t fact5 = res.get_reg_base()[2].asSigned48();
        std::cout << std::format("factc(5) via self-recursive closure = {} (expect 120)\n", fact5);
        ok = (fact5 == 120);
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    check(ok);
}

// =============================================================================
// test_tco_call_indirect_func -- TCO_CALL_INDIRECT over a Func immediate. A
// tail-recursive loop(n, acc) = n == 0 ? acc : loop(n - 1, acc + n) tail-calls
// ITSELF indirectly (via a LOAD_FN'd Func value in a HIGH register, above the
// [0, nargs) argument window) with TCO_CALL_INDIRECT. Run over 1,000,000
// iterations: without in-place frame reuse this overflows the return stack long
// before finishing, so a correct sum is the O(1)-stack proof (the indirect analogue
// of test_sum_tco). Entry is a single CALL_INDIRECT (one frame); every recursion is
// a TCO_CALL_INDIRECT (no push).
// =============================================================================
inline void test_tco_call_indirect_func() {
    std::cout << "=== tco_call_indirect (Func immediate, O(1) stack) ===\n";
    bool ok = true;
    try {
        constexpr int64_t N = 1'000'000;
        Assembler as;
        // loop(n=r0, acc=r1): frame 5 so the callee Func sits at r4, above [0, 2).
        const uint16_t id_loop = as.declare_fn("loop", /*fs*/ 5, /*arity*/ 2);

        as.label("main");                         // top_frame_size = 1 (callee at r0, args r1/r2)
        as.LOAD_FN(0, id_loop);                   // r0 = Func(loop)
        as.load_const(1, N);                      // arg0 = n  (at r1)
        as.C2(OpCode::LOAD_CONST, 2, 0);          // arg1 = acc = 0 (at r2)
        as.CALL_INDIRECT(0, 2);                   // loop(N, 0) -> result at r1
        as.J(OpCode::HALT);

        as.label("loop");                         // loop(n=r0, acc=r1)
        as.C2(OpCode::LOAD_CONST, 2, 0);
        as.B(OpCode::BEQ_INT, 0, 2, "loop_base"); // if n == 0 -> return acc
        as.R6(OpCode::ADD_INT, 1, 1, 0);          // acc' = acc + n   (reads old n first)
        as.C2(OpCode::DECR, 0);                   // n'   = n - 1     (in place)
        as.LOAD_FN(4, id_loop);                   // r4 = Func(loop)  (callee, above [0,2))
        as.TCO_CALL_INDIRECT(4, 2);               // tail-call loop(n', acc')
        as.label("loop_base");
        as.R6(OpCode::MOV, 0, 1, 0);              // return acc
        as.J(OpCode::RET);

        const auto bc = as.assemble();
        Disassembler dis(bc.data(), bc.size()); dis.add_label(0, "main"); dis.print();
        auto res = execute(bc, nullptr, nullptr, nullptr, 1,
                           nullptr, nullptr, nullptr, nullptr, &as.function_table());
        const int64_t result   = res.get_reg_base()[1].asSigned48();
        const int64_t expected = N * (N + 1) / 2;   // sum 1..N = 500000500000
        std::cout << std::format("loop({}, 0) = {} (expect {})\n", N, result, expected);
        ok = (result == expected);
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    check(ok);
}

// =============================================================================
// test_tco_call_indirect_closure -- TCO_CALL_INDIRECT over a KIND_CLOSURE, under
// GC churn. A self-recursive closure loopc(n, acc) captures [self (slot 0), k
// (slot 1)] and tail-recurses via LOAD_CAPTURE(self) + TCO_CALL_INDIRECT, adding
// k each step: loopc(N, 0) = N (k == 1). Each iteration ALLOCs a throwaway array
// to churn a small heap, forcing many collections DURING the tail loop -- so the
// closure held in current_closure (and the self capture inside it) must be
// forwarded in place and stay readable across every collect(). Proves both the
// O(1)-stack tail loop AND that the closure activation state survives GC.
// =============================================================================
inline void test_tco_call_indirect_closure() {
    std::cout << "=== tco_call_indirect (KIND_CLOSURE, GC churn) ===\n";
    bool ok = true;
    try {
        constexpr int64_t N = 50'000;
        Heap heap(64 * 1024);                     // small on purpose: forces collections

        Assembler as;
        // loopc(n=r0, acc=r1): 2 captures (slot0 = self, slot1 = k); frame 6 so the
        // ALLOC dest (r5) and the self callee (r4, above [0,2)) both fit.
        const uint16_t id_loopc = as.declare_fn("loopc", /*fs*/ 6, /*arity*/ 2,
                                                /*ncaptures*/ 2, /*self_slot*/ 0);

        as.label("main");                         // top_frame_size = 3 (closure r0; args r3/r4)
        as.C2(OpCode::LOAD_CONST, 1, 0);          // slot0 source placeholder (overwritten by self)
        as.C2(OpCode::LOAD_CONST, 2, 1);          // slot1 source = k = 1
        as.MAKE_CLOSURE(0, id_loopc, 1);          // r0 = closure; capture window [r1, r2)
        as.load_const(3, N);                      // arg0 = n   (at r3)
        as.C2(OpCode::LOAD_CONST, 4, 0);          // arg1 = acc (at r4)
        as.CALL_INDIRECT(0, 2);                   // loopc(N, 0) -> result at r3
        as.J(OpCode::HALT);

        as.label("loopc");                        // loopc(n=r0, acc=r1)
        as.C2(OpCode::LOAD_CONST, 2, 0);
        as.B(OpCode::BEQ_INT, 0, 2, "loopc_base"); // if n == 0 -> return acc
        as.C2(OpCode::ALLOC, 5, 8);               // churn: throwaway 8-slot array at r5
        as.LOAD_CAPTURE(2, 1);                    // r2 = k (captures[1])
        as.R6(OpCode::ADD_INT, 1, 1, 2);          // acc' = acc + k
        as.C2(OpCode::DECR, 0);                   // n'   = n - 1
        as.LOAD_CAPTURE(4, 0);                    // r4 = self (captures[0]); callee, above [0,2)
        as.TCO_CALL_INDIRECT(4, 2);               // tail-call self(n', acc')
        as.label("loopc_base");
        as.R6(OpCode::MOV, 0, 1, 0);              // return acc
        as.J(OpCode::RET);

        const auto bc = as.assemble();
        Disassembler dis(bc.data(), bc.size()); dis.add_label(0, "main"); dis.print();
        auto res = execute(bc, &heap, nullptr, nullptr, 3,
                           nullptr, nullptr, nullptr, nullptr, &as.function_table());
        const int64_t result = res.get_reg_base()[3].asSigned48();
        std::cout << std::format("loopc({}, 0) with k=1 = {} (expect {})\n", N, result, N);
        ok = (result == N);
    } catch (const std::exception& e) { record_fail(e.what()); ok = false; }

    check(ok);
}

// =============================================================================
// test_mod -- generic promoting MOD. int/int mirrors MOD_INT (sign of dividend,
// VM-level trap on %0); a double operand takes std::fmod (sign of dividend,
// x % 0.0 -> NaN, no trap); mixed int/double promotes to double.
// =============================================================================
inline void test_mod() {
    Assembler as;
    as.label("main");
    as.C2(OpCode::LOAD_CONST, 0, 7);
    as.C2(OpCode::LOAD_CONST, 1, 3);
    as.R6(OpCode::MOD, 2, 0, 1);      // 7 % 3   = 1   (Int)
    as.C2(OpCode::LOAD_CONST, 3, -7);
    as.R6(OpCode::MOD, 4, 3, 1);      // -7 % 3  = -1  (Int, sign of dividend)
    as.load_double(5, 5.5);
    as.load_double(6, 2.0);
    as.R6(OpCode::MOD, 7, 5, 6);      // 5.5 % 2.0 = 1.5 (Dbl, fmod)
    as.load_double(8, 0.0);
    as.R6(OpCode::MOD, 9, 5, 8);      // 5.5 % 0.0 = NaN (Dbl, no trap)
    as.R6(OpCode::MOD, 10, 0, 6);     // 7 % 2.0   = 1.0 (Dbl, promoted)
    as.J(OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << "=== mod (generic promoting %) ===\n";
    Disassembler dis(bytecode.data(), bytecode.size()); dis.add_label(0, "main"); dis.print();
    std::cout << "Running...\n";
    try {
        auto res   = execute(bytecode, nullptr, nullptr, nullptr, 0, &as.constant_pool());
        auto* regs = res.get_reg_base();
        const bool r2_ok  = regs[2].isInt()    && regs[2].asSigned48() == 1;
        const bool r4_ok  = regs[4].isInt()    && regs[4].asSigned48() == -1;
        const bool r7_ok  = regs[7].isDouble() && approx(regs[7].asDouble(), 1.5);
        const bool r9_ok  = regs[9].isDouble() && std::isnan(regs[9].asDouble());
        const bool r10_ok = regs[10].isDouble() && approx(regs[10].asDouble(), 1.0);
        std::cout << std::format("7%3={} -7%3={} 5.5%2.0={} 5.5%0.0={} 7%2.0={}\n",
                                 regs[2], regs[4], regs[7], regs[9], regs[10]);
        const bool nontrap_ok = r2_ok && r4_ok && r7_ok && r9_ok && r10_ok;

        // int % 0 must raise the VM-level "Division by zero" trap (like MOD_INT).
        Assembler tas;
        tas.label("main");
        tas.C2(OpCode::LOAD_CONST, 0, 10);
        tas.C2(OpCode::LOAD_CONST, 1, 0);
        tas.R6(OpCode::MOD, 2, 0, 1);     // 10 % 0 -> trap
        tas.J(OpCode::HALT);
        bool trap_ok = false;
        try {
            execute(tas.assemble());
        } catch (const std::exception& e) {
            trap_ok = std::string_view(e.what()).find("division by zero") != std::string_view::npos;
        }
        std::cout << std::format("int %% 0 trapped: {}\n", trap_ok ? "PASS" : "FAIL");
        check(nontrap_ok && trap_ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_num_compare -- the six generic promoting SET comparison ops (EQ/NE/LT/LE/
// GT/GE_NUM -> Bool). Covers double/double, mixed int/double promotion, and the
// IEEE NaN rules (ordered relations and == false, != true).
// =============================================================================
inline void test_num_compare() {
    std::cout << "=== num_compare (SET_*_NUM) ===\n";
    // Returns the Bool result of `op` applied to Values va, vb via the pool.
    auto cmp = [](OpCode opc, Value va, Value vb) -> bool {
        Assembler as;
        as.label("main");
        as.load_constant(0, va);
        as.load_constant(1, vb);
        as.R6(opc, 2, 0, 1);
        as.J(OpCode::HALT);
        auto res = execute(as.assemble(), nullptr, nullptr, nullptr, 0, &as.constant_pool());
        return res.get_reg_base()[2].asBool();
    };
    const Value d15 = Value::fromDouble(1.5);
    const Value d25 = Value::fromDouble(2.5);
    const Value d20 = Value::fromDouble(2.0);
    const Value i1  = Value::fromSigned48(1);
    const Value i2  = Value::fromSigned48(2);
    const Value nan = Value::nan();

    struct Case { const char* name; bool got; bool want; };
    const Case cases[] = {
        { "1.5 <  2.5",        cmp(OpCode::SET_LT_NUM, d15, d25), true  },
        { "2.5 <  1.5",        cmp(OpCode::SET_LT_NUM, d25, d15), false },
        { "2.0 <= 2.0",        cmp(OpCode::SET_LE_NUM, d20, d20), true  },
        { "2.5 >  1.5",        cmp(OpCode::SET_GT_NUM, d25, d15), true  },
        { "1.5 >= 2.5",        cmp(OpCode::SET_GE_NUM, d15, d25), false },
        { "2.0 == 2.0",        cmp(OpCode::SET_EQ_NUM, d20, d20), true  },
        { "1.5 != 2.5",        cmp(OpCode::SET_NE_NUM, d15, d25), true  },
        { "Int2 == Dbl2.0",    cmp(OpCode::SET_EQ_NUM, i2,  d20), true  },
        { "Int1 <  Dbl1.5",    cmp(OpCode::SET_LT_NUM, i1,  d15), true  },
        { "NaN == NaN",        cmp(OpCode::SET_EQ_NUM, nan, nan), false },
        { "NaN != NaN",        cmp(OpCode::SET_NE_NUM, nan, nan), true  },
        { "NaN <  1.5",        cmp(OpCode::SET_LT_NUM, nan, d15), false },
        { "NaN >= 1.5",        cmp(OpCode::SET_GE_NUM, nan, d15), false },
    };
    bool ok = true;
    try {
        for (const auto& c : cases) {
            const bool pass = c.got == c.want;
            ok = ok && pass;
            std::cout << std::format("  {:<16} -> {}  (expect {})  {}\n",
                c.name, c.got, c.want, pass ? "PASS" : "FAIL");
        }
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_num_branch -- the six generic promoting compare-and-branch ops (BEQ/BNE/
// BLT/BLE/BGT/BGE_NUM). Same IEEE semantics as the SET family; a NaN operand
// makes the ordered branches and BEQ_NUM fall through while BNE_NUM is taken.
// =============================================================================
inline void test_num_branch() {
    std::cout << "=== num_branch (B*_NUM) ===\n";
    // Returns true iff the branch `op` is taken for operands va, vb.
    auto taken = [](OpCode opc, Value va, Value vb) -> bool {
        Assembler as;
        as.label("main");
        as.load_constant(0, va);
        as.load_constant(1, vb);
        as.C2(OpCode::LOAD_CONST, 2, 0);        // default: not taken
        as.B(opc, 0, 1, "taken");
        as.J(OpCode::J, "done");
        as.label("taken");
        as.C2(OpCode::LOAD_CONST, 2, 1);
        as.label("done");
        as.J(OpCode::HALT);
        auto res = execute(as.assemble(), nullptr, nullptr, nullptr, 0, &as.constant_pool());
        return res.get_reg_base()[2].asSigned48() == 1;
    };
    const Value d15 = Value::fromDouble(1.5);
    const Value d25 = Value::fromDouble(2.5);
    const Value d20 = Value::fromDouble(2.0);
    const Value i1  = Value::fromSigned48(1);
    const Value i2  = Value::fromSigned48(2);
    const Value nan = Value::nan();

    struct Case { const char* name; bool got; bool want; };
    const Case cases[] = {
        { "BLT 1.5,2.5",       taken(OpCode::BLT_NUM, d15, d25), true  },
        { "BLT 2.5,1.5",       taken(OpCode::BLT_NUM, d25, d15), false },
        { "BLE 2.0,2.0",       taken(OpCode::BLE_NUM, d20, d20), true  },
        { "BGT 2.5,1.5",       taken(OpCode::BGT_NUM, d25, d15), true  },
        { "BGE 2.0,2.0",       taken(OpCode::BGE_NUM, d20, d20), true  },
        { "BGE 1.5,2.5",       taken(OpCode::BGE_NUM, d15, d25), false },
        { "BEQ 2.0,2.0",       taken(OpCode::BEQ_NUM, d20, d20), true  },
        { "BNE 1.5,2.5",       taken(OpCode::BNE_NUM, d15, d25), true  },
        { "BEQ Int2,Dbl2.0",   taken(OpCode::BEQ_NUM, i2,  d20), true  },
        { "BLT Int1,Dbl1.5",   taken(OpCode::BLT_NUM, i1,  d15), true  },
        { "BEQ NaN,NaN",       taken(OpCode::BEQ_NUM, nan, nan), false },
        { "BNE NaN,NaN",       taken(OpCode::BNE_NUM, nan, nan), true  },
        { "BLT NaN,1.5",       taken(OpCode::BLT_NUM, nan, d15), false },
        { "BGE NaN,1.5",       taken(OpCode::BGE_NUM, nan, d15), false },
    };
    bool ok = true;
    try {
        for (const auto& c : cases) {
            const bool pass = c.got == c.want;
            ok = ok && pass;
            std::cout << std::format("  {:<18} -> {}  (expect {})  {}\n",
                c.name, c.got, c.want, pass ? "PASS" : "FAIL");
        }
        check(ok);
    } catch (const std::exception& e) { record_fail(e.what()); }
}

// =============================================================================
// test_tco_canary -- big-loop sanity check (Release-only).
//
// HISTORICAL rationale (now retired): under the old threaded tail-call dispatch this
// was a stack-SAFETY guard. That model's safety hinged on MSVC turning
// `return dispatch_table[next](ctx);` into a `jmp`, not a `call`; if that broke
// (toolset bump, /O change, one stray non-__fastcall handler) the native C++ stack
// grew one frame PER INSTRUCTION EXECUTED and a long-running program silently
// overflowed the thread stack. That dispatch model was retired to a benchmark
// project (vm_bench, since removed); the shipping runtime dispatches via
// run_switch_loop (Interpreter.h), whose plain `for(;;){ switch }` loop
// CANNOT grow the native stack per instruction. So this test can no longer overflow
// -- the stack-safety concern it was built for no longer exists here.
//
// What it still checks: run ~10M instructions in a tight, non-recursive, int-only
// loop (no CALL/TCO_CALL, no ALLOC) and assert it runs to completion with r0 == 0 --
// a cheap smoke test that the hot dispatch path executes a long flat loop correctly.
//
// Release-only (by its own #ifdef): under /Od Debug those ~10M instructions are just
// a slow, no-value run -- the correctness they exercise is covered by other tests,
// and the large instruction count only ever mattered for the now-impossible native-
// stack growth. Debug prints SKIPPED. Registered LAST in main() by convention (under
// the old model a regression here could hard-crash the process before the summary
// printed; harmless now, but the ordering is kept).
// =============================================================================
inline void test_tco_canary() {
    std::cout << "=== tco_canary ===\n";
#ifdef _DEBUG
    std::cout << "SKIPPED (Release-only: ~10M-instruction smoke loop, no value under /Od Debug)\n";
    return;  // no check() -- counts as a (vacuous) passed test in Debug
#else
    // ~10M executed instructions: 2 per iteration (DECR + BNE) over N iterations.
    constexpr int64_t N = 5'000'000;
    Assembler as;
    as.label("main");
    as.load_const(0, N);                   // r0 = N (auto-chains LOAD_CONST + WIDE)
    as.C2(OpCode::LOAD_CONST, 1, 0);       // r1 = 0 (compare target)
    as.label("loop");
    as.C2(OpCode::DECR, 0);                // r0 = r0 - 1
    as.B (OpCode::BNE_INT, 0, 1, "loop");  // if r0 != 0 goto loop (backward branch)
    as.J (OpCode::HALT);
    const auto bytecode = as.assemble();
    std::cout << std::format("Running {} instructions in a flat-stack loop...\n",
                             2 * N);
    try {
        auto res = execute(bytecode);
        const int64_t r0 = res.get_reg_base()[0].asSigned48();
        // r0 == 0 proves the flat loop ran to completion (all ~10M instructions),
        // rather than exiting early -- the smoke check on the hot dispatch path.
        std::cout << std::format("Survived; r0 = {}\n", r0);
        check(r0 == 0);
    } catch (const std::exception& e) { record_fail(e.what()); }
#endif
}
