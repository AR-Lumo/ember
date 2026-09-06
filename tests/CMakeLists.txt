# Golden-file snapshot tests, driven by CTest.
#
# The golden directory is baked in as a compile definition so the test
# binary works regardless of the directory CTest runs it from.

add_executable(soliton_snapshot_tests
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

target_include_directories(soliton_snapshot_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(soliton_snapshot_tests
    PRIVATE
        SOLITON_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
        # Phase 4 tests drive the real compiler binary end to end.
        SOLITON_BINARY="$<TARGET_FILE:soliton>"
)

add_dependencies(soliton_snapshot_tests soliton soliton_std)

target_link_libraries(soliton_snapshot_tests
    PRIVATE
        soliton::driver
        soliton::ast
        soliton::lexer
        soliton::parser
        soliton::typeck
        soliton::codegen
        soliton::manifest
        soliton::link
)

add_test(NAME snapshot_tests COMMAND soliton_snapshot_tests)

# `soliton --version` must at least start up and exit cleanly.
add_test(NAME cli_smoke COMMAND soliton --version)
set_tests_properties(cli_smoke PROPERTIES PASS_REGULAR_EXPRESSION "^soliton [0-9]+\.[0-9]+\.[0-9]+")

# End-to-end checks of the driver itself: a clean program must exit 0,
# and a broken one must exit non-zero with a diagnostic on stderr.
add_test(NAME cli_check_accepts_a_valid_program
         COMMAND soliton check "${CMAKE_SOURCE_DIR}/examples/point.sn")

add_test(NAME cli_check_rejects_a_broken_program
         COMMAND soliton check "${CMAKE_CURRENT_SOURCE_DIR}/golden/type_errors.sn")
set_tests_properties(cli_check_rejects_a_broken_program PROPERTIES WILL_FAIL TRUE)

# A multi-file program must build and run from the entry file alone.
add_test(NAME cli_runs_a_multi_module_program
         COMMAND soliton run "${CMAKE_SOURCE_DIR}/examples/modules/main.sn")
set_tests_properties(cli_runs_a_multi_module_program
                     PROPERTIES PASS_REGULAR_EXPRESSION "distance_sq: 25")

# The manifest stub's objects, if this build has LLD in it.
#
# An OBJECT library's objects do not travel through a static library:
# CMake gives them to whatever links the object library *directly*, and
# soliton_link is a static archive in between. So the executables name
# them here. Without this the stub compiles, defines exactly the right
# symbols, and is never looked at.
if(TARGET soliton_link_stubs)
    target_sources(soliton_snapshot_tests PRIVATE $<TARGET_OBJECTS:soliton_link_stubs>)
endif()
