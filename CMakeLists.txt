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

# Silence any residual unknown-attribute warnings from the MSVC attribute shims
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wno-unknown-attributes)
endif()

# ---- Prelude embed generation ------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MANIFEST   "${CMAKE_SOURCE_DIR}/static_compiler/std/modules.manifest")
set(STD_DIR    "${CMAKE_SOURCE_DIR}/static_compiler/std")
set(EMBED_INC  "${CMAKE_BINARY_DIR}/prelude_embed.inc")
set(GEN_SCRIPT "${CMAKE_SOURCE_DIR}/static_compiler/gen_prelude_embed.py")

file(GLOB SKN_FILES "${STD_DIR}/*.skn")

add_custom_command(
    OUTPUT  "${EMBED_INC}"
    COMMAND "${Python3_EXECUTABLE}" "${GEN_SCRIPT}"
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
