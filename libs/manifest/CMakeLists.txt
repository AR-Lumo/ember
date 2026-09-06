# soliton::manifest — `soliton.toml`, dependency resolution, and the lock.
#
# Depends only on soliton::ast, for spans and the §7 diagnostic renderer:
# a broken manifest gets the same carets a broken program does. Fetching
# is injected rather than linked, so this library never shells out and
# stays testable without a network or a git binary.

add_library(soliton_manifest STATIC
    src/toml.cpp
    src/version.cpp
    src/registry.cpp
    src/manifest.cpp
)
add_library(soliton::manifest ALIAS soliton_manifest)

target_include_directories(soliton_manifest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(soliton_manifest PUBLIC cxx_std_20)
target_link_libraries(soliton_manifest PUBLIC soliton::ast)
