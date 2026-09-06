# cinder::parser - one pipeline stage, per the project layout in section 2.

add_library(cinder_parser STATIC src/parser.cpp)
add_library(cinder::parser ALIAS cinder_parser)

target_include_directories(cinder_parser PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(cinder_parser PUBLIC cxx_std_20)

target_link_libraries(cinder_parser PUBLIC cinder::ast cinder::lexer)
