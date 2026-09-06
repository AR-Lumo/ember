# soliton::typeck - one pipeline stage, per the project layout in section 2.

add_library(soliton_typeck STATIC
    src/typeck.cpp
    src/types.cpp
)
add_library(soliton::typeck ALIAS soliton_typeck)

target_include_directories(soliton_typeck PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(soliton_typeck PUBLIC cxx_std_20)

target_link_libraries(soliton_typeck PUBLIC soliton::ast)
