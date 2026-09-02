# Ember — top-level build.
#
# The compiler is split into one static library per pipeline stage (§2),
# plus a thin `ember` driver binary in src/. Each stage library is
# exported under an `ember::` alias so downstream targets link against
# the namespaced name rather than the raw target.

cmake_minimum_required(VERSION 3.20)

project(ember
    VERSION 0.1.0
    DESCRIPTION "Ember: a statically-typed, compiled language"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put binaries somewhere predictable so the test harness and the docs can
# refer to a stable path.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- LLVM ------------------------------------------------------------
#
# LLVM is only needed by Phase 4 (codegen). Phases 0-3 build and test
# without it, so a missing LLVM is a warning rather than a hard error:
# ember::codegen then compiles as a stub that reports the backend is
# unavailable. Set EMBER_REQUIRE_LLVM=ON to make it fatal.
option(EMBER_REQUIRE_LLVM "Fail configuration if LLVM is not found" OFF)

find_package(LLVM CONFIG QUIET)
if(LLVM_FOUND)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")
    set(EMBER_HAVE_LLVM ON)
elseif(EMBER_REQUIRE_LLVM)
    message(FATAL_ERROR
        "LLVM not found and EMBER_REQUIRE_LLVM=ON. "
        "Point CMake at an LLVM install with -DLLVM_DIR=<prefix>/lib/cmake/llvm.")
else()
    message(WARNING
        "LLVM not found - building without the codegen backend. "
        "Phases 0-3 (lexer, parser, type checker) are unaffected; "
        "`ember build` will report that codegen is unavailable. "
        "Configure with -DLLVM_DIR=<prefix>/lib/cmake/llvm once LLVM is installed.")
    set(EMBER_HAVE_LLVM OFF)
endif()

add_subdirectory(libs/ast)
add_subdirectory(libs/lexer)
add_subdirectory(libs/parser)
add_subdirectory(libs/typeck)
add_subdirectory(libs/codegen)
add_subdirectory(libs/std)
add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

message(STATUS "Ember ${PROJECT_VERSION} configured (C++${CMAKE_CXX_STANDARD}, LLVM: ${EMBER_HAVE_LLVM})")
