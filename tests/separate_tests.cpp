// Separate compilation: one object file per module.
//
// Before this, every module was folded into a single LLVM module and a
// single object file. That made every cross-module call a direct call,
// but it also meant no module could be built without rebuilding all of
// them - and a library you cannot build on its own is a library you
// cannot ship.
//
// Two halves are tested here. The first is codegen: with a target module
// set, only that module's functions get bodies and everything else
// becomes a declaration for the linker to resolve. The second is the
// driver: object files are cached under `.soliton` beside the entry
// source and reused when nothing they depend on has changed.
//
// Monomorphized generics are the interesting case, because a copy of
// `max<int>` belongs to no single module: the template is written in one
// and demanded from others. Each copy goes into every object that needs
// it under `linkonce_odr` linkage, and the linker keeps one - the same
// bargain C++ strikes with template instantiations.

#include "test_harness.hpp"

#include "soliton/ast/diagnostic.hpp"
#include "soliton/ast/nodes.hpp"
#include "soliton/ast/span.hpp"
#include "soliton/codegen/codegen.hpp"
#include "soliton/parser/parser.hpp"
#include "soliton/typeck/typeck.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using soliton::ast::SourceMap;

/// One module of a test program: its name, its source, and what it
/// imports. An empty name is the entry module.
struct Source {
    std::string name;
    std::string text;
    std::vector<std::string> imports;
};

/// A parsed and checked multi-module program, kept alive so the trees
/// codegen reads from outlive the lowering.
struct Program {
    SourceMap sources;
    std::vector<soliton::parser::ParseResult> parsed;
    soliton::typeck::CheckResult checked;
    std::vector<soliton::codegen::ModuleInput> inputs;
};

Program check_program(const std::vector<Source>& modules) {
    Program program;

    for (const Source& module : modules) {
        const soliton::ast::FileId id = program.sources.add(
            (module.name.empty() ? std::string{"main"} : module.name) + ".sn", module.text);
        soliton::parser::ParseResult parsed =
            soliton::parser::parse_source(program.sources.file(id));
        if (!parsed.ok()) {
            ::soliton::test::fail(__FILE__, __LINE__,
                                "test fixture does not parse:\n" +
                                    soliton::ast::render_all(parsed.diagnostics, program.sources));
        }
        program.parsed.push_back(std::move(parsed));
    }

    std::vector<soliton::typeck::ModuleInput> checker_inputs;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        checker_inputs.push_back(soliton::typeck::ModuleInput{
            modules[i].name, program.parsed[i].program.get(), modules[i].imports});
    }

    program.checked = soliton::typeck::check(checker_inputs, program.sources);
    if (!program.checked.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "test fixture does not type-check:\n" +
                                soliton::ast::render_all(program.checked.diagnostics,
                                                       program.sources));
    }

    for (std::size_t i = 0; i < modules.size(); ++i) {
        program.inputs.push_back(
            soliton::codegen::ModuleInput{modules[i].name, program.parsed[i].program.get()});
    }
    return program;
}

/// Lower `modules` as one object file's worth of IR. `target` names the
/// module whose bodies belong in it; unset means the whole program.
std::string lower(const std::vector<Source>& modules,
                  std::optional<std::string> target = std::nullopt) {
    const Program program = check_program(modules);

    soliton::codegen::CompileOptions options;
    options.target_module = std::move(target);

    const soliton::codegen::CompileResult compiled =
        soliton::codegen::compile_to_string(program.inputs, program.checked, options);
    if (!compiled.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "codegen failed:\n" +
                                soliton::ast::render_all(compiled.diagnostics, program.sources));
    }
    return compiled.assembly;
}

bool has(const std::string& ir, const std::string& needle) {
    return ir.find(needle) != std::string::npos;
}

// A two-module program: the entry module calls into `geometry`.
const std::vector<Source> kTwoModules = {
    Source{"geometry",
           "pub fn double_it(n: int) -> int {\n"
           "    return n * 2;\n"
           "}\n",
           {}},
    Source{"",
           "import geometry;\n"
           "\n"
           "pub fn main() {\n"
           "    println(geometry::double_it(21));\n"
           "}\n",
           {"geometry"}},
};

}  // namespace

// ---------------------------------------------------------------------
// What lands in each object file
// ---------------------------------------------------------------------

SOLITON_TEST(separate_compilation_defines_only_the_target_modules_functions) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const std::string ir = lower(kTwoModules, "geometry");
    SOLITON_CHECK_MSG(has(ir, "define i64 @geometry__double_it("),
                    "geometry's own function should have a body here:\n" + ir);
    SOLITON_CHECK_MSG(!has(ir, "define i32 @main("),
                    "the entry module's body leaked into geometry's object:\n" + ir);
}

SOLITON_TEST(separate_compilation_declares_what_it_calls_across_a_module_boundary) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // This is the whole mechanism: the entry object refers to
    // `geometry::double_it` by symbol and lets the linker find the body.
    const std::string ir = lower(kTwoModules, "");
    SOLITON_CHECK_MSG(has(ir, "define i32 @main("), "no entry point in:\n" + ir);
    SOLITON_CHECK_MSG(has(ir, "declare i64 @geometry__double_it("),
                    "the imported function should be a declaration here:\n" + ir);
    SOLITON_CHECK_MSG(!has(ir, "define i64 @geometry__double_it("),
                    "geometry's body was emitted twice:\n" + ir);
}

SOLITON_TEST(separate_compilation_puts_the_entry_point_in_one_object_only) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // Two `main`s would be a duplicate symbol, which is the failure this
    // whole split is most likely to cause.
    SOLITON_CHECK(has(lower(kTwoModules, ""), "define i32 @main("));
    SOLITON_CHECK(!has(lower(kTwoModules, "geometry"), "define i32 @main("));
}

SOLITON_TEST(whole_program_compilation_still_defines_every_module) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // With no target the old behaviour is unchanged, which is what the
    // golden `.ll` snapshots read.
    const std::string ir = lower(kTwoModules);
    SOLITON_CHECK(has(ir, "define i32 @main("));
    SOLITON_CHECK(has(ir, "define i64 @geometry__double_it("));
}

// ---------------------------------------------------------------------
// Monomorphized generics, which belong to no one module
// ---------------------------------------------------------------------

namespace {

/// `compare` writes a generic; `alpha` and `beta` each instantiate it at
/// `int`, and the entry module instantiates it at `float`.
const std::vector<Source> kSharedGeneric = {
    Source{"compare",
           "pub fn max<T>(a: T, b: T) -> T {\n"
           "    if a > b {\n"
           "        return a;\n"
           "    }\n"
           "    return b;\n"
           "}\n",
           {}},
    Source{"alpha",
           "import compare;\n"
           "pub fn best() -> int { return compare::max(3, 7); }\n",
           {"compare"}},
    Source{"beta",
           "import compare;\n"
           "pub fn best() -> int { return compare::max(11, 4); }\n",
           {"compare"}},
    Source{"",
           "import alpha;\n"
           "import beta;\n"
           "import compare;\n"
           "pub fn main() {\n"
           "    println(alpha::best());\n"
           "    println(beta::best());\n"
           "    println(compare::max(2.5, 1.5));\n"
           "}\n",
           {"alpha", "beta", "compare"}},
};

}  // namespace

SOLITON_TEST(separate_compilation_copies_an_instantiation_into_every_module_that_uses_it) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // Neither `alpha` nor `beta` can assume the other was built, so both
    // carry `max<int>` and the linker discards one.
    SOLITON_CHECK(has(lower(kSharedGeneric, "alpha"), "define linkonce_odr i64 @compare__max__int("));
    SOLITON_CHECK(has(lower(kSharedGeneric, "beta"), "define linkonce_odr i64 @compare__max__int("));
}

SOLITON_TEST(separate_compilation_leaves_an_instantiation_out_of_modules_that_do_not_use_it) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // The module that *wrote* the template emits nothing for it: a
    // generic function is not code until someone picks its types.
    const std::string compare = lower(kSharedGeneric, "compare");
    SOLITON_CHECK_MSG(!has(compare, "define linkonce_odr i64 @compare__max__int("),
                    "an unused instantiation was emitted into its template's module:\n" +
                        compare);

    // And `float` is only demanded by the entry module.
    SOLITON_CHECK(!has(lower(kSharedGeneric, "alpha"), "@compare__max__float("));
    SOLITON_CHECK(has(lower(kSharedGeneric, ""), "define linkonce_odr double @compare__max__float("));
}

SOLITON_TEST(separate_compilation_follows_demand_through_a_generic_calling_a_generic) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // `max3<int>` is demanded by the entry module and demands `max<int>`
    // in turn, so the entry object needs both. Nothing demands them of
    // `util`, so its object stays empty of either.
    const std::vector<Source> modules = {
        Source{"util",
               "pub fn max<T>(a: T, b: T) -> T {\n"
               "    if a > b {\n"
               "        return a;\n"
               "    }\n"
               "    return b;\n"
               "}\n"
               "pub fn max3<T>(a: T, b: T, c: T) -> T {\n"
               "    return max(max(a, b), c);\n"
               "}\n",
               {}},
        Source{"",
               "import util;\n"
               "pub fn main() { println(util::max3(1, 5, 3)); }\n",
               {"util"}},
    };

    const std::string entry = lower(modules, "");
    SOLITON_CHECK_MSG(has(entry, "define linkonce_odr i64 @util__max3__int("),
                    "the demanded instantiation is missing:\n" + entry);
    SOLITON_CHECK_MSG(has(entry, "define linkonce_odr i64 @util__max__int("),
                    "an instantiation demanded by another one is missing:\n" + entry);

    const std::string util = lower(modules, "util");
    SOLITON_CHECK(!has(util, "define linkonce_odr i64 @util__max3__int("));
    SOLITON_CHECK(!has(util, "define linkonce_odr i64 @util__max__int("));
}

SOLITON_TEST(separate_compilation_records_which_modules_demand_an_instantiation) {
    // The checker's own answer, without going through codegen.
    const Program program = check_program(kSharedGeneric);

    std::optional<std::set<std::string>> at_int;
    std::optional<std::set<std::string>> at_float;
    for (const soliton::typeck::Instantiation& instance : program.checked.instantiations) {
        if (instance.info.display_name == "compare::max<int>") {
            at_int = instance.demanded_by;
        } else if (instance.info.display_name == "compare::max<float>") {
            at_float = instance.demanded_by;
        }
    }

    SOLITON_CHECK_MSG(at_int.has_value(), "no `compare::max<int>` instantiation");
    SOLITON_CHECK_MSG(at_float.has_value(), "no `compare::max<float>` instantiation");
    SOLITON_CHECK_EQ(*at_int, (std::set<std::string>{"alpha", "beta"}));
    SOLITON_CHECK_EQ(*at_float, (std::set<std::string>{""}));
}

// ---------------------------------------------------------------------
// The driver: caching object files between builds
// ---------------------------------------------------------------------

namespace {

struct ProcessResult {
    int exit_code = 0;
    std::string output;
};

/// Run a command, capturing stdout *and* stderr - progress reporting
/// goes to stderr, and that is what these tests read.
ProcessResult run_process(const std::string& command) {
    const std::string redirected = command + " 2>&1";
#ifdef _WIN32
    // cmd.exe eats the outer quotes of a command that starts with one.
    const std::string wrapped = "\"" + redirected + "\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    FILE* pipe = popen(redirected.c_str(), "r");
#endif
    if (pipe == nullptr) {
        ::soliton::test::fail(__FILE__, __LINE__, "cannot start: " + command);
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

/// A directory of `.sn` files, removed when the test ends.
class Workspace {
public:
    Workspace() {
        static int counter = 0;
        root_ = fs::temp_directory_path() /
                ("soliton-separate-" + std::to_string(++counter) + "-" +
                 std::to_string(static_cast<unsigned long long>(
                     std::hash<std::string>{}(__FILE__))));
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        fs::create_directories(root_, ignored);
    }

    ~Workspace() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    void write(const std::string& name, const std::string& contents) const {
        std::ofstream out(root_ / name, std::ios::binary);
        out << contents;
    }

    fs::path path(const std::string& name) const { return root_ / name; }

    /// `soliton run <entry> --verbose`, so the result carries both the
    /// program's output and the report of what was compiled.
    ProcessResult run(const std::string& entry, const std::string& flags = "") const {
        return run_process(quoted(fs::path{SOLITON_BINARY}) + " run " + quoted(path(entry)) +
                           " --verbose" + (flags.empty() ? "" : " " + flags));
    }

private:
    fs::path root_;
};

/// Fills a workspace with the three-module program these tests edit.
void write_program(const Workspace& workspace) {
    workspace.write("shapes.sn",
                    "pub struct Point { pub x: int, pub y: int, }\n"
                    "pub fn sum(p: Point) -> int { return p.x + p.y; }\n");
    workspace.write("counter.sn", "pub fn next(n: int) -> int { return n + 1; }\n");
    workspace.write("main.sn",
                    "import shapes;\n"
                    "import counter;\n"
                    "pub fn main() {\n"
                    "    let p = shapes::Point { x: 3, y: 4 };\n"
                    "    println(shapes::sum(p));\n"
                    "    println(counter::next(41));\n"
                    "}\n");
}

bool compiled(const ProcessResult& result, const std::string& file) {
    return result.output.find("compiling " + file) != std::string::npos;
}

bool cached(const ProcessResult& result, const std::string& file) {
    return result.output.find("cached  " + file) != std::string::npos;
}

}  // namespace

SOLITON_TEST(incremental_build_compiles_every_module_the_first_time) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const Workspace workspace;
    write_program(workspace);

    const ProcessResult first = workspace.run("main.sn");
    SOLITON_CHECK_MSG(first.exit_code == 0, "build failed:\n" + first.output);
    SOLITON_CHECK_MSG(compiled(first, "main.sn"), first.output);
    SOLITON_CHECK_MSG(compiled(first, "shapes.sn"), first.output);
    SOLITON_CHECK_MSG(compiled(first, "counter.sn"), first.output);
}

SOLITON_TEST(incremental_build_reuses_every_object_when_nothing_changed) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const Workspace workspace;
    write_program(workspace);
    workspace.run("main.sn");

    const ProcessResult again = workspace.run("main.sn");
    SOLITON_CHECK_MSG(again.exit_code == 0, "rebuild failed:\n" + again.output);
    SOLITON_CHECK_MSG(cached(again, "main.sn"), again.output);
    SOLITON_CHECK_MSG(cached(again, "shapes.sn"), again.output);
    SOLITON_CHECK_MSG(cached(again, "counter.sn"), again.output);

    // Reusing objects must not change what the program does.
    SOLITON_CHECK_MSG(again.output.find("7") != std::string::npos, again.output);
    SOLITON_CHECK_MSG(again.output.find("42") != std::string::npos, again.output);
}

SOLITON_TEST(incremental_build_recompiles_a_changed_module_and_its_dependents) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const Workspace workspace;
    write_program(workspace);
    workspace.run("main.sn");

    // A struct that changes shape changes the code generated in every
    // module that uses it, so `main` has to be rebuilt too - which is
    // why the cache key covers a module's imports, not just its own
    // source. `counter` is untouched and stays cached.
    workspace.write("shapes.sn",
                    "pub struct Point { pub tag: int, pub x: int, pub y: int, }\n"
                    "pub fn sum(p: Point) -> int { return p.x + p.y; }\n");
    workspace.write("main.sn",
                    "import shapes;\n"
                    "import counter;\n"
                    "pub fn main() {\n"
                    "    let p = shapes::Point { tag: 0, x: 3, y: 4 };\n"
                    "    println(shapes::sum(p));\n"
                    "    println(counter::next(41));\n"
                    "}\n");

    const ProcessResult after = workspace.run("main.sn");
    SOLITON_CHECK_MSG(after.exit_code == 0, "rebuild failed:\n" + after.output);
    SOLITON_CHECK_MSG(compiled(after, "shapes.sn"), after.output);
    SOLITON_CHECK_MSG(compiled(after, "main.sn"), after.output);
    SOLITON_CHECK_MSG(cached(after, "counter.sn"),
                    "an unrelated module was rebuilt:\n" + after.output);
    SOLITON_CHECK_MSG(after.output.find("7") != std::string::npos, after.output);
}

SOLITON_TEST(incremental_build_leaves_a_module_alone_when_only_a_sibling_changed) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const Workspace workspace;
    write_program(workspace);
    workspace.run("main.sn");

    // `shapes` does not import `counter`, so it cannot be affected.
    workspace.write("counter.sn", "pub fn next(n: int) -> int { return n + 1 + 0; }\n");

    const ProcessResult after = workspace.run("main.sn");
    SOLITON_CHECK_MSG(cached(after, "shapes.sn"),
                    "a module that imports nothing changed was rebuilt:\n" + after.output);
    SOLITON_CHECK_MSG(compiled(after, "counter.sn"), after.output);
    SOLITON_CHECK_MSG(compiled(after, "main.sn"), after.output);
}

SOLITON_TEST(incremental_build_can_be_told_to_ignore_the_cache) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const Workspace workspace;
    write_program(workspace);
    workspace.run("main.sn");

    const ProcessResult fresh = workspace.run("main.sn", "--fresh");
    SOLITON_CHECK_MSG(fresh.exit_code == 0, "rebuild failed:\n" + fresh.output);
    SOLITON_CHECK_MSG(compiled(fresh, "shapes.sn"), fresh.output);
    SOLITON_CHECK_MSG(compiled(fresh, "counter.sn"), fresh.output);
    SOLITON_CHECK_MSG(compiled(fresh, "main.sn"), fresh.output);
}

SOLITON_TEST(separate_compilation_links_a_generic_two_modules_both_instantiated) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // The tests above read IR; this one reads the linker's mind. Two
    // objects each define `compare__max__int`, and a duplicate symbol
    // would fail the link - `linkonce_odr` is what makes it not.
    const Workspace workspace;
    workspace.write("compare.sn",
                    "pub fn max<T>(a: T, b: T) -> T {\n"
                    "    if a > b {\n"
                    "        return a;\n"
                    "    }\n"
                    "    return b;\n"
                    "}\n");
    workspace.write("alpha.sn",
                    "import compare;\n"
                    "pub fn best() -> int { return compare::max(3, 7); }\n");
    workspace.write("beta.sn",
                    "import compare;\n"
                    "pub fn best() -> int { return compare::max(11, 4); }\n");
    workspace.write("main.sn",
                    "import alpha;\n"
                    "import beta;\n"
                    "pub fn main() {\n"
                    "    println(alpha::best());\n"
                    "    println(beta::best());\n"
                    "}\n");

    const ProcessResult result = workspace.run("main.sn");
    SOLITON_CHECK_MSG(result.exit_code == 0, "build or run failed:\n" + result.output);
    SOLITON_CHECK_MSG(result.output.find("7") != std::string::npos, result.output);
    SOLITON_CHECK_MSG(result.output.find("11") != std::string::npos, result.output);
}

SOLITON_TEST(incremental_build_caches_each_optimization_level_separately) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // Flipping between `-O0` while working and `-O2` to check something
    // must not recompile the program each way round, so the level is
    // part of the object's name rather than its fingerprint.
    const Workspace workspace;
    write_program(workspace);

    workspace.run("main.sn", "-O2");
    workspace.run("main.sn", "-O0");

    const ProcessResult again = workspace.run("main.sn", "-O2");
    SOLITON_CHECK_MSG(cached(again, "main.sn"),
                    "switching back to -O2 recompiled:\n" + again.output);
    SOLITON_CHECK_MSG(cached(again, "shapes.sn"), again.output);

    int objects = 0;
    std::error_code ignored;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(workspace.path(".soliton"), ignored)) {
        objects += entry.path().extension() == ".o" ? 1 : 0;
    }
    // Three modules at two levels, and neither level swept the other.
    SOLITON_CHECK_EQ(objects, 6);
}

SOLITON_TEST(incremental_build_keeps_one_object_per_module) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const Workspace workspace;
    write_program(workspace);
    workspace.run("main.sn");

    // Editing repeatedly must not grow the cache: each build sweeps the
    // objects the previous fingerprints left behind.
    for (int i = 0; i < 3; ++i) {
        workspace.write("counter.sn",
                        "pub fn next(n: int) -> int { return n + 1 + " + std::to_string(i) +
                            " - " + std::to_string(i) + "; }\n");
        workspace.run("main.sn");
    }

    int objects = 0;
    std::error_code ignored;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(workspace.path(".soliton"), ignored)) {
        objects += entry.path().extension() == ".o" ? 1 : 0;
    }
    SOLITON_CHECK_EQ(objects, 3);
}

SOLITON_TEST(whole_program_builds_one_object_and_caches_nothing) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // The trade separate compilation made, offered back: no per-module
    // objects and no cache, and in exchange a cross-module call is a
    // direct call inside one LLVM module for `-O` to inline.
    const Workspace workspace;
    write_program(workspace);

    const ProcessResult built = workspace.run("main.sn", "--whole-program");
    SOLITON_CHECK_MSG(built.exit_code == 0, "build failed:\n" + built.output);
    SOLITON_CHECK_MSG(built.output.find("as one unit") != std::string::npos, built.output);
    SOLITON_CHECK_MSG(!fs::exists(workspace.path(".soliton")),
                    "a whole-program build should cache nothing");

    // And the program it produces is the same program.
    SOLITON_CHECK_MSG(built.output.find("7") != std::string::npos, built.output);
    SOLITON_CHECK_MSG(built.output.find("42") != std::string::npos, built.output);
}

SOLITON_TEST(whole_program_and_separate_builds_agree) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const Workspace workspace;
    write_program(workspace);

    const ProcessResult separate = workspace.run("main.sn", "-O2");
    const ProcessResult together = workspace.run("main.sn", "-O2 --whole-program");

    // Compare only what the program printed: the progress reporting
    // differs between the two by design.
    SOLITON_CHECK_MSG(separate.output.find("7") != std::string::npos, separate.output);
    SOLITON_CHECK_MSG(together.output.find("7") != std::string::npos, together.output);
    SOLITON_CHECK_MSG(separate.output.find("42") != std::string::npos, separate.output);
    SOLITON_CHECK_MSG(together.output.find("42") != std::string::npos, together.output);
}
