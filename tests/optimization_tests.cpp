// `-O`: running LLVM's optimization pipeline over each module.
//
// The pipeline itself was written along with codegen and then sat
// unreachable for four releases - `CompileOptions::optimization_level`
// existed, defaulted to zero, and nothing ever set it, so every Ember
// program ever compiled was `-O0`. These tests exist so that cannot
// quietly happen again: one checks the flag reaches codegen, one checks
// codegen does something with it, and one checks the program still
// prints what it printed before.
//
// The last is the one that matters. An optimizer that changes a
// program's output is not an optimizer, it is a bug, and the usual cause
// is IR that promised something the generated code does not honour.

#include "test_harness.hpp"

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/nodes.hpp"
#include "ember/ast/span.hpp"
#include "ember/codegen/codegen.hpp"
#include "ember/parser/parser.hpp"
#include "ember/typeck/typeck.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

/// Lower one program at a given optimization level.
std::string lower(const std::string& contents, unsigned level) {
    const ember::ast::SourceFile source{"test.em", contents};
    const ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    if (!parsed.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "fixture does not parse:\n" +
                                ember::ast::render_all(parsed.diagnostics, source));
    }
    const ember::typeck::CheckResult checked =
        ember::typeck::check(*parsed.program, source);
    if (!checked.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "fixture does not type-check:\n" +
                                ember::ast::render_all(checked.diagnostics, source));
    }

    ember::codegen::CompileOptions options;
    options.optimization_level = level;

    const ember::codegen::CompileResult compiled =
        ember::codegen::compile_to_string(*parsed.program, checked, source, options);
    if (!compiled.ok()) {
        ::ember::test::fail(__FILE__, __LINE__, "codegen failed");
    }
    return compiled.assembly;
}

const char* const kAddition =
    "pub fn add(a: int, b: int) -> int {\n"
    "    let total = a + b;\n"
    "    return total;\n"
    "}\n"
    "pub fn main() { println(add(2, 3)); }\n";

}  // namespace

EMBER_TEST(optimization_is_off_by_default) {
    if (!ember::codegen::is_available()) {
        return;
    }
    // Locals live in `alloca` slots and are loaded and stored on every
    // use, which is what clang -O0 emits and what makes the IR readable
    // beside the source it came from.
    EMBER_CHECK_MSG(lower(kAddition, 0).find("alloca") != std::string::npos,
                    "an unoptimized build should still be storing locals in slots");
}

EMBER_TEST(optimization_promotes_locals_out_of_memory) {
    if (!ember::codegen::is_available()) {
        return;
    }
    // mem2reg is the first thing any pipeline does, so this is the
    // cheapest way to prove the pipeline ran at all.
    const std::string ir = lower(kAddition, 2);
    EMBER_CHECK_MSG(ir.find("alloca") == std::string::npos,
                    "no pass appears to have run:\n" + ir);
}

EMBER_TEST(optimization_levels_are_distinguishable) {
    if (!ember::codegen::is_available()) {
        return;
    }
    // Not an assertion about what each level does - that is LLVM's
    // business - only that the number is carried through rather than
    // rounded to "on".
    EMBER_CHECK(lower(kAddition, 0) != lower(kAddition, 1));
}

// ---------------------------------------------------------------------
// The output must not change
// ---------------------------------------------------------------------

namespace {

struct ProcessResult {
    int exit_code = 0;
    std::string output;
};

ProcessResult run_process(const std::string& command) {
#ifdef _WIN32
    const std::string wrapped = "\"" + command + " 2>&1\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
    if (pipe == nullptr) {
        ::ember::test::fail(__FILE__, __LINE__, "cannot start: " + command);
    }

    ProcessResult result;
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output += buffer.data();
    }

#ifdef _WIN32
    result.exit_code = _pclose(pipe);
#else
    result.exit_code = pclose(pipe);
#endif
    return result;
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

/// Every example program: the single-file ones, and the `main.em` of
/// each directory that holds one.
///
/// Discovered rather than listed, so adding an example puts it under
/// this comparison automatically - a list here would go stale the first
/// time somebody forgot it.
std::vector<fs::path> example_programs() {
    std::vector<fs::path> programs;
    std::error_code code;
    const fs::path examples = fs::path{EMBER_GOLDEN_DIR}.parent_path().parent_path() / "examples";

    for (const fs::directory_entry& entry : fs::directory_iterator(examples, code)) {
        if (entry.path().extension() == ".em") {
            programs.push_back(entry.path());
            continue;
        }
        if (!entry.is_directory()) {
            continue;
        }
        // A plain multi-file example, or a package with its modules in
        // `src` the way a manifest expects.
        for (const fs::path& candidate : {entry.path() / "main.em",
                                          entry.path() / "src" / "main.em"}) {
            if (fs::exists(candidate)) {
                programs.push_back(candidate);
                break;
            }
        }
    }
    return programs;
}

}  // namespace

EMBER_TEST(optimization_does_not_change_what_a_program_prints) {
    if (!ember::codegen::is_available()) {
        return;
    }
    const std::vector<fs::path> programs = example_programs();
    EMBER_CHECK_MSG(!programs.empty(), "found no example programs to compare");

    for (const fs::path& program : programs) {
        const std::string command =
            quoted(fs::path{EMBER_BINARY}) + " run " + quoted(program) + " ";

        const ProcessResult unoptimized = run_process(command + "-O0");
        const ProcessResult optimized = run_process(command + "-O3");

        EMBER_CHECK_MSG(unoptimized.exit_code == optimized.exit_code,
                        program.filename().string() + " exits differently under -O3");
        EMBER_CHECK_MSG(unoptimized.output == optimized.output,
                        program.filename().string() + " prints differently under -O3:\n-O0:\n" +
                            unoptimized.output + "-O3:\n" + optimized.output);
    }
}
