# ember::parser - one pipeline stage, per the project layout in section 2.

add_library(ember_parser STATIC src/parser.cpp)
add_library(ember::parser ALIAS ember_parser)

target_include_directories(ember_parser PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ember_parser PUBLIC cxx_std_20)

target_link_libraries(ember_parser PUBLIC ember::ast ember::lexer)
