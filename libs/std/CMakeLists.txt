# cinder::std - one pipeline stage, per the project layout in section 2.

add_library(cinder_std STATIC src/std.cpp)
add_library(cinder::std ALIAS cinder_std)

target_include_directories(cinder_std PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(cinder_std PUBLIC cxx_std_20)
