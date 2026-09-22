// vMachine.cpp : the test-suite executable (vMachine.exe).
//
// This translation unit drives the test suite and links the vmcore static
// library for the runtime (execute(), declared in Execute.h). The switch
// interpreter inlines all opcode handling in vmcore's single TU (vmcore.cpp),
// so there are no handler symbols to duplicate -- just include Execute.h and
// link vmcore.

#include <iostream>
#include <stdexcept>
#include <string>     // argv comparison for --fault-probe

#include "Execute.h"
#include "Tests.h"

// =============================================================================
// run -- drives one test, backstopping any exception the test does not handle
// itself so the rest of the suite still runs and the throw counts as a failure.
// (Tests with their own try/catch record via record_fail(); the no-catch tests
// -- test_gc, the string tests -- rely on this wrapper.) See the test harness
// in Tests.h.
// =============================================================================
static void run(const char* name, void (*fn)()) {
    const int failed_before = g_test_stats.failed;
    try {
        fn();
    } catch (const std::exception& e) {
        std::cerr << "[" << name << "] threw: " << e.what() << "\n";
        ++g_test_stats.failed;
    } catch (...) {
        std::cerr << "[" << name << "] threw a non-std exception\n";
        ++g_test_stats.failed;
    }
    // Whole-test verdict: the test failed iff it produced any failed assertion
    // (check()==false / record_fail) or an escaped throw -- all of which bump
    // g_test_stats.failed. This makes the tally strictly per-test, independent of
    // how many check() calls a test happens to make.
    if (g_test_stats.failed == failed_before) ++g_test_stats.tests_passed;
    else                                      ++g_test_stats.tests_failed;
    std::cout << "\n";
}

int main(int argc, char** argv) {
    // --fault-probe runs ONLY the real-hardware-fault test, and nothing else.
    //
    // It is separated because it is the one test whose failure mode is a process kill
    // rather than a failed assertion: it deliberately dereferences a bad pointer to prove
    // that the fault frame around run_switch_loop classifies and recovers. Mixing it into
    // the ordinary run would mean a regression there destroys every other result in the
    // same invocation. See test_fault_probe in Tests.h.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--fault-probe") continue;
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
        // The VM declines to install its handler under a sanitizer (doing so would
        // destroy the sanitizer's own reporting), so the probe cannot pass there. Skip it
        // rather than report a failure that says nothing about the code.
        std::cout << "fault_probe: SKIPPED (sanitizer build keeps its own handler)\n";
        return 0;
#  endif
#endif
        run("fault_probe", test_fault_probe);
        // The multi-threaded sibling rides along in the same invocation rather than
        // getting its own flag: it provokes the same class of fault for the same reason,
        // so it belongs in the one run that is allowed to die. Order matters -- if the
        // single-threaded frame is broken, there is nothing to learn from the concurrent
        // one, and it will not run.
        run("concurrent_fault_probe", test_concurrent_fault_probe);
        const TestStats& fs = g_test_stats;
        std::cout << "==== fault probe: " << fs.tests_passed << " passed, "
                  << fs.tests_failed << " failed ====\n";
        return fs.tests_failed == 0 ? 0 : 1;
    }

    run("far_call_relaxation",      test_far_call_relaxation);
    run("factorial",                test_factorial);
    run("sum_tco",                  test_sum_tco);
    run("load_const_wide",          test_load_const_wide);
    run("set_and_branch",           test_set_and_branch);
    run("cmov",                     test_cmov);
    run("native_registry",          test_native_registry);
    run("call_native_high_regs",    test_call_native_high_registers);
    run("native_time",              test_native_time);
    run("native_procctx",           test_native_procctx);
    run("native_dirops",            test_native_dirops);
    run("native_stdin",             test_native_stdin);
    run("native_process",           test_native_process);
    run("gc",                       test_gc);
    run("globals",                  test_globals);
    run("alloc",                    test_alloc);
    run("alloc_gc_retry",           test_alloc_gc_retry);
    run("alloc_global_root",        test_alloc_global_root);
    run("string_gc",                test_string_gc);
    run("string_pool_policy",       test_string_pool_policy);
    run("string_hash_table",        test_string_hash_table);
    run("string_interner",          test_string_interner);
    run("string_interner_gc_root",  test_string_interner_gc_root);
    run("string_interner_gc_retry", test_string_interner_gc_retry);
    run("frame_gc_roots",           test_frame_gc_roots);
    run("frame_size_root_boundary", test_frame_size_root_boundary);
    run("mov_take",                 test_mov_take);
    run("av_classification",        test_av_classification);
    run("load_str",                 test_load_str);
    run("string_concat",            test_string_concat);
    run("string_number_concat",     test_string_number_concat);
    run("to_string",                test_to_string);
    run("to_string_struct",         test_to_string_struct);
    run("to_string_array",          test_to_string_array);
    run("to_string_vec",            test_to_string_vec);
    run("to_string_map",            test_to_string_map);
    run("to_string_tuple",          test_to_string_tuple);
    run("to_string_list",           test_to_string_list);
    run("print",                    test_print);
    run("panic",                    test_panic);
    run("string_compare",           test_string_compare);
    run("string_content_eq",        test_string_content_eq);
    run("struct",                   test_struct);
    run("struct_gc",                test_struct_gc);
    run("array",                    test_array);
    run("const_array",              test_const_array);
    run("vec",                      test_vec);
    run("bytes",                    test_bytes);
    run("bytes_append",             test_bytes_append);
    run("map",                      test_map);
    run("map_iter_next",            test_map_iter_next);
    run("eq_deep",                  test_eq_deep);
    run("closure_gc",               test_closure_gc);
    run("closure_roots",            test_closure_roots);
    run("get_type_id",              test_get_type_id);
    run("i2d",                      test_i2d);
    run("d2i",                      test_d2i);
    run("dround",                   test_dround);
    run("get_kind",                 test_get_kind);
    run("string_alignment",         test_string_alignment);
    run("gc_moves",                 test_gc_moves);
    run("verify_heap",              test_verify_heap);
    run("top_frame_validation",     test_top_frame_validation);
    run("gc_interleaved_reclaim",   test_gc_interleaved_reclaim);
    run("gc_root_convergence",      test_gc_root_convergence);
    run("gc_grow_for_oversized",    test_gc_grow_for_oversized);
    run("debug_dump",               test_debug_dump);
    run("gc_stress",                test_gc_stress);
    run("cross_frame_call",         test_cross_frame_call);
    run("div_mod",                  test_div_mod);
    run("div_by_zero_trap",         test_div_by_zero_trap);
    run("receiver_faults",          test_receiver_faults);
    run("neg",                      test_neg);
    run("bitwise",                  test_bitwise);
    run("shifts",                   test_shifts);
    run("load_double",              test_load_double);
    run("double_arith",             test_double_arith);
    run("num_promotion",            test_num_promotion);
    run("type_checks",              test_type_checks);
    run("mod",                      test_mod);
    run("num_compare",              test_num_compare);
    run("num_branch",               test_num_branch);
    run("to_bool",                  test_to_bool);
    run("lnot",                     test_lnot);
    run("equality",                 test_equality);
    run("func_value",               test_func_value);
    run("function_table",           test_function_table);
    run("load_fn",                  test_load_fn);
    run("call_indirect",            test_call_indirect);
    run("proto_resolve",            test_proto_resolve);
    run("resolve_call",             test_resolve_call);
    run("resolve_tco_call",         test_resolve_tco_call);
    run("make_closure",             test_make_closure);
    run("load_capture",             test_load_capture);
    run("closure_self_recursion",   test_closure_self_recursion);
    run("tco_call_indirect_func",   test_tco_call_indirect_func);
    run("tco_call_indirect_closure",test_tco_call_indirect_closure);
    run("value_codec_roundtrip",    test_value_codec_roundtrip);
    run("value_codec_containers",   test_value_codec_containers);
    run("value_codec_strings",      test_value_codec_strings_not_interned);
    run("value_codec_sharing",      test_value_codec_sharing_and_cycles);
    run("value_codec_refusals",     test_value_codec_refusals);
    run("value_codec_malformed",    test_value_codec_malformed);
    run("rooted_pool_release",      test_rooted_pool_release);
    run("value_codec_collects",     test_value_codec_collects);
    run("thread_slot_table",        test_thread_slot_table);
    run("concurrent_execute",       test_concurrent_execute);
    run("task_parallel_sums",       test_task_parallel_sums);
    run("task_failure",             test_task_failure);
    run("task_values",              test_task_values);
    run("task_output_and_unjoined", test_task_output_and_unjoined);
    run("task_deep_recursion",      test_task_deep_recursion);
    run("task_misuse",              test_task_misuse);
    run("actor_ping_pong",          test_actor_ping_pong);
    run("actor_crash_reported",     test_actor_crash_reported);
    run("actor_timeout_and_stop",   test_actor_timeout_and_stop);
    run("actor_many",               test_actor_many);
    run("actor_misuse",             test_actor_misuse);
    run("actor_socket_hand_off",    test_actor_socket_hand_off);
    run("actor_extra_inbox_reply",  test_actor_extra_inbox_reply);
    run("actor_bounded_backpressure", test_actor_bounded_backpressure);
    run("actor_bounded_shutdown",   test_actor_bounded_shutdown);
    // Registered LAST on purpose: on the switch dispatcher a runaway loop can no
    // longer overflow the native stack, but the canary still guards its ~10M-instr
    // survival + r0 == 0 invariant (Release only; Debug prints SKIPPED).
    run("tco_canary",               test_tco_canary);

    const TestStats& s = g_test_stats;
    std::cout << "==== tests: " << s.tests_passed << " passed, " << s.tests_failed
              << " failed  (checks: " << s.passed << " passed, " << s.failed
              << " failed) ====\n";
    return s.tests_failed == 0 ? 0 : 1;
}
