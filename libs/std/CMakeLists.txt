# soliton::std - one pipeline stage, per the project layout in section 2.

add_library(soliton_std STATIC src/std.cpp)
add_library(soliton::std ALIAS soliton_std)

target_include_directories(soliton_std PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(soliton_std PUBLIC cxx_std_20)
