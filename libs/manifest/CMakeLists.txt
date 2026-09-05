# ember::manifest — `ember.toml`, dependency resolution, and the lock.
#
# Depends only on ember::ast, for spans and the §7 diagnostic renderer:
# a broken manifest gets the same carets a broken program does. Fetching
# is injected rather than linked, so this library never shells out and
# stays testable without a network or a git binary.

add_library(ember_manifest STATIC
    src/toml.cpp
    src/manifest.cpp
)
add_library(ember::manifest ALIAS ember_manifest)

target_include_directories(ember_manifest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ember_manifest PUBLIC cxx_std_20)
target_link_libraries(ember_manifest PUBLIC ember::ast)
