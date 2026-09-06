# ember::link - the linker, compiled into the driver.
#
# Ember used to shell out to `${CMAKE_CXX_COMPILER}`, recorded as an
# absolute path at build time. That makes the compiler unusable off the
# machine that built it. LLD is linked in instead, so `ember` carries
# its own linker and consults nothing outside its own install.

add_library(ember_link STATIC src/link.cpp)
add_library(ember::link ALIAS ember_link)

target_include_directories(ember_link PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ember_link PUBLIC cxx_std_20)

# Where the startup objects and system archives live in *this* build
# tree, so the compiler works out of `build/` without being installed.
# The directories themselves are found at the top level.
if(MINGW)
    if(EMBER_GCC_DIR AND EMBER_CRT_DIR)
        # `|` rather than `;` or `:`: CMake lists use `;` and a Windows
        # path contains `:`, so either would need escaping to survive
        # the trip through a compile definition.
        target_compile_definitions(ember_link PRIVATE
            EMBER_BUILD_LIBRARY_PATHS="${EMBER_GCC_DIR}|${EMBER_CRT_DIR}")
        target_compile_definitions(ember_link PRIVATE
            EMBER_RUNTIME_LIBRARY="$<TARGET_FILE:ember_std>")
        add_dependencies(ember_link ember_std)
        message(STATUS "Link paths: ${EMBER_GCC_DIR}, ${EMBER_CRT_DIR}")
    else()
        message(WARNING
            "Could not ask ${CMAKE_CXX_COMPILER} where its startup objects live; "
            "`ember build` will not be able to link out of the build tree.")
    endif()
endif()

# --- LLD -------------------------------------------------------------
#
# Optional, like LLVM: without it the driver falls back to an external
# linker and says so.
set(EMBER_HAVE_LLD OFF)

# MINGW as well as LLVM: the link line this library builds is
# MinGW-specific - `-m i386pep`, crt2.o, libmsvcrt - and the driver only
# knows where the pieces live on that platform. Claiming a linker we
# cannot actually drive would turn a working fallback into a hard error
# everywhere else, so elsewhere Ember still invokes an external linker
# and says so.
if(EMBER_HAVE_LLVM AND MINGW)
    find_library(EMBER_LLD_MINGW  lldMinGW  HINTS ${LLVM_LIBRARY_DIRS})
    find_library(EMBER_LLD_COFF   lldCOFF   HINTS ${LLVM_LIBRARY_DIRS})
    find_library(EMBER_LLD_COMMON lldCommon HINTS ${LLVM_LIBRARY_DIRS})

    if(EMBER_LLD_MINGW AND EMBER_LLD_COFF AND EMBER_LLD_COMMON)
        set(EMBER_HAVE_LLD ON)
        message(STATUS "Found LLD (${EMBER_LLD_MINGW})")
    else()
        message(WARNING
            "LLD was not found next to LLVM. The driver will fall back to an "
            "external linker, which means the compiler only works on a machine "
            "with a C++ toolchain installed. Install lld to fix this.")
    endif()
endif()

if(EMBER_HAVE_LLD)
    # Replaces LLVMWindowsManifest, which cannot be linked statically
    # here - see the comment at the top of windows_manifest_stub.cpp.
    #
    # An OBJECT library, not a source of ember_link: a static archive is
    # searched only for symbols that are undefined when the linker
    # reaches it, and ember_link comes *before* liblldCOFF.a on the
    # command line - the reference does not exist yet. An object file is
    # linked in whether or not anything has asked for it, so ordering
    # stops mattering.
    add_library(ember_link_stubs OBJECT src/windows_manifest_stub.cpp)
    target_include_directories(ember_link_stubs SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_features(ember_link_stubs PRIVATE cxx_std_20)
    separate_arguments(EMBER_STUB_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
    target_compile_definitions(ember_link_stubs PRIVATE ${EMBER_STUB_DEFINITIONS})
    if(NOT LLVM_ENABLE_RTTI AND NOT MSVC)
        target_compile_options(ember_link_stubs PRIVATE -fno-rtti)
    endif()

    target_compile_definitions(ember_link PUBLIC EMBER_HAVE_LLD=1)
    target_include_directories(ember_link SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

    separate_arguments(EMBER_LLD_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
    target_compile_definitions(ember_link PRIVATE ${EMBER_LLD_LLVM_DEFINITIONS})

    # Order matters for static archives: the MinGW driver calls into the
    # COFF driver, which calls into lldCommon.
    #
    # AllTargets* rather than nativecodegen: lldCOFF's driver calls
    # LLVMInitialize<Target>AsmParser for every target LLVM was built
    # with, unconditionally, so that an LTO bitcode input can name any
    # of them. The references exist whether or not that path is taken,
    # so they all have to resolve. This is what makes the binary large.
    llvm_map_components_to_libnames(EMBER_LLD_LLVM_LIBS
        lto
        passes
        object
        option
        windowsdriver
        libdriver
        debuginfopdb
        debuginfocodeview
        support
        targetparser
        core
        AllTargetsAsmParsers
        AllTargetsCodeGens
        AllTargetsDescs
        AllTargetsInfos
    )
    target_link_libraries(ember_link PUBLIC
        ember_link_stubs
        ${EMBER_LLD_MINGW}
        ${EMBER_LLD_COFF}
        ${EMBER_LLD_COMMON}
        ${EMBER_LLD_LLVM_LIBS}
    )

    # LLD, like LLVM, is built without RTTI by default.
    if(NOT LLVM_ENABLE_RTTI AND NOT MSVC)
        target_compile_options(ember_link PRIVATE -fno-rtti)
    endif()
else()
    target_compile_definitions(ember_link PUBLIC EMBER_HAVE_LLD=0)
endif()
