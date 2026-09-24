cmake_minimum_required(VERSION 3.21)
project(Skarn CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Apple Silicon: arm64 only, macOS 15+ (Sequoia)
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES     "arm64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0"  CACHE STRING "" FORCE)
    # Suppress libc++ availability guards so features present in the SDK are
    # usable regardless of the deployment target annotation.
    add_compile_definitions(_LIBCPP_DISABLE_AVAILABILITY)
endif()

# NOTE: there is deliberately no -Wno-unknown-attributes here.
#
# It was needed while MSVC-only attributes were still written out raw, and it hid what it
# suppressed: [[msvc::forceinline]] on the NaN-boxing predicates in Value.h and on the
# bytecode decoders in Instruction.h is IGNORED by Clang and GCC, so the hottest inlining
# hints in the project were silently dropped on every non-MSVC build while the warning
# that said so was turned off. Every hint now goes through SKARN_FORCEINLINE /
# SKARN_NOINLINE (Inline.h), which expands to the spelling each toolchain acts on, so
# there is nothing left to suppress -- and if someone reintroduces a raw attribute, the
# warning is there to say so.

# ---- Prelude embed generation ------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MANIFEST   "${CMAKE_SOURCE_DIR}/static_compiler/std/modules.manifest")
set(STD_DIR    "${CMAKE_SOURCE_DIR}/static_compiler/std")
set(EMBED_INC  "${CMAKE_BINARY_DIR}/prelude_embed.inc")
set(GEN_SCRIPT "${CMAKE_SOURCE_DIR}/static_compiler/gen_prelude_embed.py")

file(GLOB SKN_FILES "${STD_DIR}/*.skn")

# MSVC refuses a string literal longer than about 16 KB (C2026), so there each module is
# embedded as adjacent literals of about 4 KB, which the compiler concatenates.
set(EMBED_OPTIONS)
if(MSVC)
    set(EMBED_OPTIONS --chunk 4096)
endif()

add_custom_command(
    OUTPUT  "${EMBED_INC}"
    COMMAND "${Python3_EXECUTABLE}" "${GEN_SCRIPT}" ${EMBED_OPTIONS}
            "${MANIFEST}" "${STD_DIR}" "${EMBED_INC}"
    DEPENDS "${GEN_SCRIPT}" "${MANIFEST}" ${SKN_FILES}
    COMMENT "Generating prelude_embed.inc from std/*.skn"
)
add_custom_target(gen_prelude ALL DEPENDS "${EMBED_INC}")

# ---- vmcore static library ---------------------------------------------------
file(GLOB VMCORE_SOURCES "vmcore/*.cpp")
add_library(vmcore STATIC ${VMCORE_SOURCES})
target_include_directories(vmcore PUBLIC vmcore)
target_compile_features(vmcore PUBLIC cxx_std_20)
if(NOT WIN32)
    target_link_libraries(vmcore PUBLIC pthread)
endif()

# ---- skarnclib -- the static compiler (library, no main) ---------------------
file(GLOB COMPILER_SOURCES "static_compiler/*.cpp")
add_library(skarnclib STATIC ${COMPILER_SOURCES})
target_include_directories(skarnclib PUBLIC static_compiler)
# The generated prelude_embed.inc lives in the build directory
target_include_directories(skarnclib PUBLIC "${CMAKE_BINARY_DIR}")
target_link_libraries(skarnclib PUBLIC vmcore)
add_dependencies(skarnclib gen_prelude)

# ---- skarnvm -- driver: compiles + runs Skarn scripts ------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/static_vmrun/main.cpp")
    add_executable(skarnvm static_vmrun/main.cpp)
    target_include_directories(skarnvm PRIVATE static_vmrun)
    target_link_libraries(skarnvm PRIVATE skarnclib vmcore)
endif()

# ---- test executables --------------------------------------------------------
# Both are their own runner: main() IS the suite, it prints an "N passed, M failed"
# summary and exits non-zero on any failure, so ctest needs nothing but the exit code.
# They are the only thing that can show a port of this VM is correct, so they build by
# default rather than behind an option.
enable_testing()

add_executable(vm_tests vm_tests/main.cpp)
target_include_directories(vm_tests PRIVATE vm_tests)
target_link_libraries(vm_tests PRIVATE vmcore)
add_test(NAME vm_tests COMMAND vm_tests)

# The real-hardware-fault probe, as its OWN ctest entry. It deliberately dereferences a
# bad pointer to prove that run_switch's fault frame classifies and recovers, so its
# failure mode is a process kill rather than a failed assertion -- keeping it out of the
# main invocation means a regression costs one test result instead of all of them.
add_test(NAME vm_fault_probe COMMAND vm_tests --fault-probe)

file(GLOB COMPILER_TEST_SOURCES "static_compiler_tests/*.cpp")
add_executable(static_compiler_tests ${COMPILER_TEST_SOURCES})
target_include_directories(static_compiler_tests PRIVATE static_compiler_tests)
target_link_libraries(static_compiler_tests PRIVATE skarnclib vmcore)
add_test(NAME static_compiler_tests COMMAND static_compiler_tests)

# ---- skarn_lsp -- the language server (LSP over stdio) ----------------------
# Uses the front end only, so it is as portable as skarnclib. `--selftest` is its suite.
file(GLOB LSP_SOURCES "skarn_lsp/*.cpp")
add_executable(skarn_lsp ${LSP_SOURCES})
target_include_directories(skarn_lsp PRIVATE skarn_lsp)
target_link_libraries(skarn_lsp PRIVATE skarnclib vmcore)
add_test(NAME skarn_lsp_selftest COMMAND skarn_lsp --selftest)

# ---- the documentation and demo gates ----------------------------------------
# Python runners over the built driver: the guide claims, every code block of the guides,
# the programs under examples/ (exact output), the guides' internal links, the section
# names cited from source, and the demos (type-check + self-tests). Each exits non-zero on
# any failure. They need only the Python 3 found above for the prelude embed.
if(TARGET skarnvm)
    set(SKARN_DRIVER --exe $<TARGET_FILE:skarnvm>)
    add_test(NAME doc_claims
             COMMAND "${Python3_EXECUTABLE}" tests/guide_claims/run_guide_claims.py ${SKARN_DRIVER} --no-examples
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME doc_examples
             COMMAND "${Python3_EXECUTABLE}" tests/guide_claims/run_guide_examples.py ${SKARN_DRIVER}
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME examples
             COMMAND "${Python3_EXECUTABLE}" tests/run_examples.py ${SKARN_DRIVER}
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME doc_links
             COMMAND "${Python3_EXECUTABLE}" tests/guide_claims/check_anchors.py
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME doc_anchors
             COMMAND "${Python3_EXECUTABLE}" tests/check_doc_anchors.py
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME demos
             COMMAND "${Python3_EXECUTABLE}" tests/run_demos.py ${SKARN_DRIVER}
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

# ---- stack reserve for the front-end executables ----------------------------
# Parsing and checking are recursive descent: one level of expression nesting costs up to
# ~21 KB of stack in a debug build, and MSVC reserves only 1 MB by default -- a long pipe
# chain overflowed it. The checker's own depth limit is sized against this reserve. A POSIX
# main thread already gets 8 MB, so only the MSVC link needs saying. (vm_tests runs no front
# end and is left alone.)
if(MSVC)
    foreach(front_end_target skarnvm static_compiler_tests skarn_lsp)
        if(TARGET ${front_end_target})
            target_link_options(${front_end_target} PRIVATE /STACK:8388608)
        endif()
    endforeach()
endif()

# The switch rot-guards in static_compiler/Codegen.cpp are `#pragma warning(error: 4062)`,
# which only MSVC honours. Keep the guard alive on the Clang/GCC side with its equivalent,
# so an enumerator added to an AST node still fails the build instead of falling through.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(skarnclib PRIVATE -Werror=switch)
endif()
