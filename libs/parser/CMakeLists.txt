# soliton::parser - one pipeline stage, per the project layout in section 2.

add_library(soliton_parser STATIC src/parser.cpp)
add_library(soliton::parser ALIAS soliton_parser)

target_include_directories(soliton_parser PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(soliton_parser PUBLIC cxx_std_20)

target_link_libraries(soliton_parser PUBLIC soliton::ast soliton::lexer)
