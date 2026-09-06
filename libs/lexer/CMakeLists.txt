# soliton::lexer - one pipeline stage, per the project layout in section 2.

add_library(soliton_lexer STATIC
    src/lexer.cpp
    src/token.cpp
)
add_library(soliton::lexer ALIAS soliton_lexer)

target_include_directories(soliton_lexer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(soliton_lexer PUBLIC cxx_std_20)

target_link_libraries(soliton_lexer PUBLIC soliton::ast)
