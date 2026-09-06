# Golden-file snapshot tests, driven by CTest.
#
# The golden directory is baked in as a compile definition so the test
# binary works regardless of the directory CTest runs it from.

add_executable(cinder_snapshot_tests
    snapshot_tests.cpp
    lexer_tests.cpp
    parser_tests.cpp
    typeck_tests.cpp
    generics_tests.cpp
    module_tests.cpp
    ownership_tests.cpp
    closure_tests.cpp
    separate_tests.cpp
    optimization_tests.cpp
    search_path_tests.cpp
    package_tests.cpp
    interface_tests.cpp
    string_tests.cpp
    link_tests.cpp
    contracts_tests.cpp
    units_tests.cpp
    effects_tests.cpp
)

target_include_directories(cinder_snapshot_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(cinder_snapshot_tests
    PRIVATE
        CINDER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
        # Phase 4 tests drive the real compiler binary end to end.
        CINDER_BINARY="$<TARGET_FILE:cinder>"
)

add_dependencies(cinder_snapshot_tests cinder cinder_std)

target_link_libraries(cinder_snapshot_tests
    PRIVATE
        cinder::driver
        cinder::ast
        cinder::lexer
        cinder::parser
        cinder::typeck
        cinder::codegen
        cinder::manifest
        cinder::link
)

add_test(NAME snapshot_tests COMMAND cinder_snapshot_tests)

# `cinder --version` must at least start up and exit cleanly.
add_test(NAME cli_smoke COMMAND cinder --version)
set_tests_properties(cli_smoke PROPERTIES PASS_REGULAR_EXPRESSION "^cinder [0-9]+\.[0-9]+\.[0-9]+")

# End-to-end checks of the driver itself: a clean program must exit 0,
# and a broken one must exit non-zero with a diagnostic on stderr.
add_test(NAME cli_check_accepts_a_valid_program
         COMMAND cinder check "${CMAKE_SOURCE_DIR}/examples/point.ci")

add_test(NAME cli_check_rejects_a_broken_program
         COMMAND cinder check "${CMAKE_CURRENT_SOURCE_DIR}/golden/type_errors.ci")
set_tests_properties(cli_check_rejects_a_broken_program PROPERTIES WILL_FAIL TRUE)

# A multi-file program must build and run from the entry file alone.
add_test(NAME cli_runs_a_multi_module_program
         COMMAND cinder run "${CMAKE_SOURCE_DIR}/examples/modules/main.ci")
set_tests_properties(cli_runs_a_multi_module_program
                     PROPERTIES PASS_REGULAR_EXPRESSION "distance_sq: 25")

# The manifest stub's objects, if this build has LLD in it.
#
# An OBJECT library's objects do not travel through a static library:
# CMake gives them to whatever links the object library *directly*, and
# cinder_link is a static archive in between. So the executables name
# them here. Without this the stub compiles, defines exactly the right
# symbols, and is never looked at.
if(TARGET cinder_link_stubs)
    target_sources(cinder_snapshot_tests PRIVATE $<TARGET_OBJECTS:cinder_link_stubs>)
endif()
