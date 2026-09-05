# The `ember` driver binary.
#
# Argument parsing is split into a small static library so the test
# binary can link it and assert on usage errors directly; main.cpp stays
# a thin shell around it.

add_library(ember_driver STATIC cli.cpp)
add_library(ember::driver ALIAS ember_driver)

target_include_directories(ember_driver PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ember_driver PUBLIC cxx_std_20)
target_link_libraries(ember_driver
    PUBLIC
        ember::ast
        ember::lexer
        ember::parser
        ember::typeck
        ember::codegen
        ember::manifest
)

# The driver has to know where the runtime it links into every compiled
# program lives, and which toolchain does the linking. Both are baked in
# at build time rather than searched for at run time.
target_compile_definitions(ember_driver PRIVATE
    EMBER_RUNTIME_LIBRARY="$<TARGET_FILE:ember_std>"
    EMBER_LINKER="${CMAKE_CXX_COMPILER}"
)
add_dependencies(ember_driver ember_std)

add_executable(ember main.cpp)

target_link_libraries(ember
    PRIVATE
        ember::driver
        ember::ast
        ember::lexer
        ember::parser
        ember::typeck
        ember::codegen
        ember::manifest
)
