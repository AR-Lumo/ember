# ember::std - one pipeline stage, per the project layout in section 2.

add_library(ember_std STATIC src/std.cpp)
add_library(ember::std ALIAS ember_std)

target_include_directories(ember_std PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ember_std PUBLIC cxx_std_20)
