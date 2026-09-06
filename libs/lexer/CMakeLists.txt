# cinder::lexer - one pipeline stage, per the project layout in section 2.

add_library(cinder_lexer STATIC
    src/lexer.cpp
    src/token.cpp
)
add_library(cinder::lexer ALIAS cinder_lexer)

target_include_directories(cinder_lexer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(cinder_lexer PUBLIC cxx_std_20)

target_link_libraries(cinder_lexer PUBLIC cinder::ast)
