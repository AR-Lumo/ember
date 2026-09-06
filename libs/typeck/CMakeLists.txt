# cinder::typeck - one pipeline stage, per the project layout in section 2.

add_library(cinder_typeck STATIC
    src/typeck.cpp
    src/types.cpp
)
add_library(cinder::typeck ALIAS cinder_typeck)

target_include_directories(cinder_typeck PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(cinder_typeck PUBLIC cxx_std_20)

target_link_libraries(cinder_typeck PUBLIC cinder::ast)
