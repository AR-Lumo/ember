# ember::lexer - one pipeline stage, per the project layout in section 2.

add_library(ember_lexer STATIC
    src/lexer.cpp
    src/token.cpp
)
add_library(ember::lexer ALIAS ember_lexer)

target_include_directories(ember_lexer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ember_lexer PUBLIC cxx_std_20)

target_link_libraries(ember_lexer PUBLIC ember::ast)
