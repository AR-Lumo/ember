# soliton::ast - one pipeline stage, per the project layout in section 2.

add_library(soliton_ast STATIC
    src/ast.cpp
    src/span.cpp
    src/diagnostic.cpp
    src/nodes.cpp
    src/printer.cpp
    src/interface.cpp
)
add_library(soliton::ast ALIAS soliton_ast)

target_include_directories(soliton_ast PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(soliton_ast PUBLIC cxx_std_20)
