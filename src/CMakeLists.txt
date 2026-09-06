# The `cinder` driver binary.
#
# Argument parsing is split into a small static library so the test
# binary can link it and assert on usage errors directly; main.cpp stays
# a thin shell around it.

add_library(cinder_driver STATIC cli.cpp)
add_library(cinder::driver ALIAS cinder_driver)

target_include_directories(cinder_driver PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(cinder_driver PUBLIC cxx_std_20)
target_link_libraries(cinder_driver
    PUBLIC
        cinder::ast
        cinder::lexer
        cinder::parser
        cinder::typeck
        cinder::codegen
        cinder::manifest
        cinder::link
)

# The driver has to know where the runtime it links into every compiled
# program lives, and which toolchain does the linking. Both are baked in
# at build time rather than searched for at run time.
target_compile_definitions(cinder_driver PRIVATE
    CINDER_RUNTIME_LIBRARY="$<TARGET_FILE:cinder_std>"
    CINDER_LINKER="${CMAKE_CXX_COMPILER}"
)
add_dependencies(cinder_driver cinder_std)

add_executable(cinder main.cpp)

target_link_libraries(cinder
    PRIVATE
        cinder::driver
        cinder::ast
        cinder::lexer
        cinder::parser
        cinder::typeck
        cinder::codegen
        cinder::manifest
        cinder::link
)

# Declared at the top level, because forcing static zlib and zstd has to
# happen before find_package(LLVM).
if(CINDER_STATIC_DRIVER AND MINGW)
    target_link_options(cinder PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
    )
endif()

# The manifest stub's objects, if this build has LLD in it.
#
# An OBJECT library's objects do not travel through a static library:
# CMake gives them to whatever links the object library *directly*, and
# cinder_link is a static archive in between. So the executables name
# them here. Without this the stub compiles, defines exactly the right
# symbols, and is never looked at.
if(TARGET cinder_link_stubs)
    target_sources(cinder PRIVATE $<TARGET_OBJECTS:cinder_link_stubs>)
endif()
