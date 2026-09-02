# ember::typeck - one pipeline stage, per the project layout in section 2.

add_library(ember_typeck STATIC
    src/typeck.cpp
    src/types.cpp
)
add_library(ember::typeck ALIAS ember_typeck)

target_include_directories(ember_typeck PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ember_typeck PUBLIC cxx_std_20)

target_link_libraries(ember_typeck PUBLIC ember::ast)
