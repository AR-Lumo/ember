# ember::ast - one pipeline stage, per the project layout in section 2.

add_library(ember_ast STATIC
    src/ast.cpp
    src/span.cpp
    src/diagnostic.cpp
    src/nodes.cpp
    src/printer.cpp
)
add_library(ember::ast ALIAS ember_ast)

target_include_directories(ember_ast PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ember_ast PUBLIC cxx_std_20)
