# cinder::ast - one pipeline stage, per the project layout in section 2.

add_library(cinder_ast STATIC
    src/ast.cpp
    src/span.cpp
    src/diagnostic.cpp
    src/nodes.cpp
    src/printer.cpp
    src/interface.cpp
)
add_library(cinder::ast ALIAS cinder_ast)

target_include_directories(cinder_ast PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(cinder_ast PUBLIC cxx_std_20)
