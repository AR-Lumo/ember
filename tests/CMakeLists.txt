# Golden-file snapshot tests, driven by CTest.
#
# The golden directory is baked in as a compile definition so the test
# binary works regardless of the directory CTest runs it from.

add_executable(ember_snapshot_tests
    snapshot_tests.cpp
    lexer_tests.cpp
    parser_tests.cpp
    typeck_tests.cpp
    generics_tests.cpp
    module_tests.cpp
    ownership_tests.cpp
)

target_include_directories(ember_snapshot_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(ember_snapshot_tests
    PRIVATE
        EMBER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
        # Phase 4 tests drive the real compiler binary end to end.
        EMBER_BINARY="$<TARGET_FILE:ember>"
)

add_dependencies(ember_snapshot_tests ember ember_std)

target_link_libraries(ember_snapshot_tests
    PRIVATE
        ember::driver
        ember::ast
        ember::lexer
        ember::parser
        ember::typeck
        ember::codegen
)

add_test(NAME snapshot_tests COMMAND ember_snapshot_tests)

# `ember --version` must at least start up and exit cleanly.
add_test(NAME cli_smoke COMMAND ember --version)
set_tests_properties(cli_smoke PROPERTIES PASS_REGULAR_EXPRESSION "^ember [0-9]+\.[0-9]+\.[0-9]+")

# End-to-end checks of the driver itself: a clean program must exit 0,
# and a broken one must exit non-zero with a diagnostic on stderr.
add_test(NAME cli_check_accepts_a_valid_program
         COMMAND ember check "${CMAKE_SOURCE_DIR}/examples/point.em")

add_test(NAME cli_check_rejects_a_broken_program
         COMMAND ember check "${CMAKE_CURRENT_SOURCE_DIR}/golden/type_errors.em")
set_tests_properties(cli_check_rejects_a_broken_program PROPERTIES WILL_FAIL TRUE)

# A multi-file program must build and run from the entry file alone.
add_test(NAME cli_runs_a_multi_module_program
         COMMAND ember run "${CMAKE_SOURCE_DIR}/examples/modules/main.em")
set_tests_properties(cli_runs_a_multi_module_program
                     PROPERTIES PASS_REGULAR_EXPRESSION "distance_sq: 25")
