# Cinder — top-level build.
#
# The compiler is split into one static library per pipeline stage (§2),
# plus a thin `cinder` driver binary in src/. Each stage library is
# exported under an `cinder::` alias so downstream targets link against
# the namespaced name rather than the raw target.

cmake_minimum_required(VERSION 3.20)

project(cinder
    VERSION 0.1.0
    DESCRIPTION "Cinder: a statically-typed, compiled language"
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

# --- A driver that runs without the toolchain that built it ----------
#
# By default `cinder.exe` pulls libstdc++-6.dll, libgcc_s_seh-1.dll,
# libwinpthread-1.dll, zlib1.dll and libzstd.dll out of the MSYS2
# prefix: copy the binary to a machine without MSYS2 and it dies before
# reaching `main`. A compiler nobody can install is not a toolchain.
#
# Linking those statically leaves only what Windows itself ships - the
# UCRT (`api-ms-win-crt-*`, present on Windows 10 and later) plus
# kernel32, advapi32, shell32, ole32 and ntdll. The binary gets fatter,
# which is the right trade: it is already 120 MB of static LLVM.
option(CINDER_STATIC_DRIVER
       "Link the driver against static runtimes so it needs no toolchain DLLs"
       ON)

if(CINDER_STATIC_DRIVER AND MINGW)
    # FindZLIB honours this, so `ZLIB::ZLIB` resolves to libz.a rather
    # than the zlib1.dll import library. zstd is not so obliging and is
    # dealt with after LLVM has been found.
    set(ZLIB_USE_STATIC_LIBS ON)
endif()

# --- LLVM ------------------------------------------------------------
#
# LLVM is only needed by Phase 4 (codegen). Phases 0-3 build and test
# without it, so a missing LLVM is a warning rather than a hard error:
# cinder::codegen then compiles as a stub that reports the backend is
# unavailable. Set CINDER_REQUIRE_LLVM=ON to make it fatal.
option(CINDER_REQUIRE_LLVM "Fail configuration if LLVM is not found" OFF)

find_package(LLVM CONFIG QUIET)
if(LLVM_FOUND)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")
    set(CINDER_HAVE_LLVM ON)
elseif(CINDER_REQUIRE_LLVM)
    message(FATAL_ERROR
        "LLVM not found and CINDER_REQUIRE_LLVM=ON. "
        "Point CMake at an LLVM install with -DLLVM_DIR=<prefix>/lib/cmake/llvm.")
else()
    message(WARNING
        "LLVM not found - building without the codegen backend. "
        "Phases 0-3 (lexer, parser, type checker) are unaffected; "
        "`cinder build` will report that codegen is unavailable. "
        "Configure with -DLLVM_DIR=<prefix>/lib/cmake/llvm once LLVM is installed.")
    set(CINDER_HAVE_LLVM OFF)
endif()

# LLVMWindowsManifest - which lldCOFF needs, to merge the manifest into
# the executables it produces - links libxml2. Same problem as zstd, and
# the same fix: retarget the imported library at the static archive.
# Static libxml2 needs iconv and bcrypt, which the shared build resolved
# for itself; `Libs.private` in libxml-2.0.pc is where that is written
# down.
if(CINDER_STATIC_DRIVER AND MINGW AND TARGET LibXml2::LibXml2)
    set(_ember_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    find_library(CINDER_XML2_STATIC NAMES xml2)
    find_library(CINDER_ICONV_STATIC NAMES iconv)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_ember_saved_suffixes})

    if(CINDER_XML2_STATIC AND CINDER_ICONV_STATIC)
        set_target_properties(LibXml2::LibXml2 PROPERTIES
            IMPORTED_IMPLIB "${CINDER_XML2_STATIC}"
            IMPORTED_LOCATION "${CINDER_XML2_STATIC}"
            INTERFACE_LINK_LIBRARIES "${CINDER_ICONV_STATIC};bcrypt")
        message(STATUS "Static libxml2: ${CINDER_XML2_STATIC}")
    else()
        message(WARNING
            "CINDER_STATIC_DRIVER is ON but no static libxml2 was found; "
            "the driver will still need libxml2-16.dll at run time.")
    endif()
endif()

# LLVMSupport's exported interface names `zstd::libzstd_shared`
# explicitly - an import library, given by absolute path, so `-static`
# on the link line cannot displace it. Retarget the imported library at
# the static archive. On MinGW the link uses IMPORTED_IMPLIB, but set
# both so the target is coherent whichever CMake reads.
if(CINDER_STATIC_DRIVER AND MINGW AND TARGET zstd::libzstd_shared)
    set(_ember_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    find_library(CINDER_ZSTD_STATIC NAMES zstd)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_ember_saved_suffixes})

    if(CINDER_ZSTD_STATIC)
        set_target_properties(zstd::libzstd_shared PROPERTIES
            IMPORTED_IMPLIB "${CINDER_ZSTD_STATIC}"
            IMPORTED_LOCATION "${CINDER_ZSTD_STATIC}")
        message(STATUS "Static zstd: ${CINDER_ZSTD_STATIC}")
    else()
        message(WARNING
            "CINDER_STATIC_DRIVER is ON but no static zstd was found; "
            "the driver will still need libzstd.dll at run time.")
    endif()
endif()

# --- Where this toolchain keeps its startup objects ------------------
#
# Asked of the compiler rather than assumed: crtbegin.o sits in gcc's
# own version-stamped directory and crt2.o in the mingw prefix, and
# neither path is guessable from the other.
if(MINGW)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-libgcc-file-name
        OUTPUT_VARIABLE CINDER_LIBGCC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=crt2.o
        OUTPUT_VARIABLE CINDER_CRT2
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    get_filename_component(CINDER_GCC_DIR "${CINDER_LIBGCC}" DIRECTORY)
    get_filename_component(CINDER_CRT_DIR "${CINDER_CRT2}" DIRECTORY)
endif()

add_subdirectory(libs/ast)
add_subdirectory(libs/lexer)
add_subdirectory(libs/parser)
add_subdirectory(libs/typeck)
add_subdirectory(libs/codegen)
add_subdirectory(libs/manifest)
add_subdirectory(libs/std)
# After libs/std: the linker needs the runtime archive it links in.
add_subdirectory(libs/link)
add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

message(STATUS "Cinder ${PROJECT_VERSION} configured (C++${CMAKE_CXX_STANDARD}, LLVM: ${CINDER_HAVE_LLVM})")

# --- Installing ------------------------------------------------------
#
# The layout `cinder::link` looks for, relative to the running binary:
#
#     <prefix>/bin/cinder.exe
#     <prefix>/lib/cinder/libcinder_std.a       the Cinder runtime
#     <prefix>/lib/cinder/crt2.o, crtbegin.o, ...   startup objects
#     <prefix>/lib/cinder/libmsvcrt.a, ...          system archives
#
# Everything the linker opens is in that one directory, so an install is
# a directory tree with nothing outside it - copy it to a machine with
# no compiler and it works. That is the whole point of the exercise.
install(TARGETS cinder RUNTIME DESTINATION bin)
install(FILES "$<TARGET_FILE:cinder_std>" DESTINATION lib/cinder)

if(MINGW AND CINDER_GCC_DIR AND CINDER_CRT_DIR)
    # From the mingw prefix: the C runtime, the Win32 import libraries,
    # and libstdc++ (which the Cinder runtime needs for std::to_chars -
    # the shortest round-tripping float formatting that `println` uses).
    set(CINDER_SYSROOT_CRT
        crt2.o
        default-manifest.o
        libmingw32.a
        libmingwex.a
        libmsvcrt.a
        libpthread.a
        libadvapi32.a
        libkernel32.a
        libshell32.a
        libuser32.a
        libstdc++.a)

    # From gcc's own directory: the compiler support library and the
    # objects that bracket the startup code.
    set(CINDER_SYSROOT_GCC
        crtbegin.o
        crtend.o
        libgcc.a
        libgcc_eh.a)

    foreach(CINDER_FILE IN LISTS CINDER_SYSROOT_CRT)
        install(FILES "${CINDER_CRT_DIR}/${CINDER_FILE}" DESTINATION lib/cinder)
    endforeach()
    foreach(CINDER_FILE IN LISTS CINDER_SYSROOT_GCC)
        install(FILES "${CINDER_GCC_DIR}/${CINDER_FILE}" DESTINATION lib/cinder)
    endforeach()

    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION share/cinder)
endif()
