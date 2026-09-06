# The `soliton` driver binary.
#
# Argument parsing is split into a small static library so the test
# binary can link it and assert on usage errors directly; main.cpp stays
# a thin shell around it.

add_library(soliton_driver STATIC cli.cpp)
add_library(soliton::driver ALIAS soliton_driver)

target_include_directories(soliton_driver PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(soliton_driver PUBLIC cxx_std_20)
target_link_libraries(soliton_driver
    PUBLIC
        soliton::ast
        soliton::lexer
        soliton::parser
        soliton::typeck
        soliton::codegen
        soliton::manifest
        soliton::link
)

# The driver has to know where the runtime it links into every compiled
# program lives, and which toolchain does the linking. Both are baked in
# at build time rather than searched for at run time.
target_compile_definitions(soliton_driver PRIVATE
    SOLITON_RUNTIME_LIBRARY="$<TARGET_FILE:soliton_std>"
    SOLITON_LINKER="${CMAKE_CXX_COMPILER}"
)
add_dependencies(soliton_driver soliton_std)

add_executable(soliton main.cpp)

target_link_libraries(soliton
    PRIVATE
        soliton::driver
        soliton::ast
        soliton::lexer
        soliton::parser
        soliton::typeck
        soliton::codegen
        soliton::manifest
        soliton::link
)

# Declared at the top level, because forcing static zlib and zstd has to
# happen before find_package(LLVM).
if(SOLITON_STATIC_DRIVER AND MINGW)
    target_link_options(soliton PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
    )
endif()

# The manifest stub's objects, if this build has LLD in it.
#
# An OBJECT library's objects do not travel through a static library:
# CMake gives them to whatever links the object library *directly*, and
# soliton_link is a static archive in between. So the executables name
# them here. Without this the stub compiles, defines exactly the right
# symbols, and is never looked at.
if(TARGET soliton_link_stubs)
    target_sources(soliton PRIVATE $<TARGET_OBJECTS:soliton_link_stubs>)
endif()
