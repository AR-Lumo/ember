// The module search path: where `import foo;` looks for `foo`.
//
// Until now it looked in exactly one place — beside the file that wrote
// the import — which meant a program could only ever use modules kept in
// its own directory. That is fine for a program written all at once and
// hopeless for one that depends on code it did not write, so it was the
// real thing standing between Ember and a package manager.
//
// The order is deliberate and the tests below pin it down: the
// importer's own directory always wins, so adding a dependency can never
// quietly take over a name the program was already using. Only then does
// the search path get a turn, each directory tried twice — as a file,
// and as a directory holding a file of the same name. The second form is
// what lets a package be more than one file, because its own private
// modules are then siblings and resolve by the first rule.

#include "test_harness.hpp"

#include "cli.hpp"
#include "ember/ast/diagnostic.hpp"
#include "ember/ast/span.hpp"
#include "ember/parser/parser.hpp"

#include <cstdlib>
#include <filesystem>
#include <random>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// A throwaway tree of `.em` files, removed when the test ends.
class Workspace {
public:
    Workspace() {
        // Unique per run as well as per test, so a directory a previous
        // run failed to clean up cannot be mistaken for this one's.
        static const unsigned run = std::random_device{}();
        static int counter = 0;
        root_ = fs::temp_directory_path() /
                ("ember-search-path-" + std::to_string(run) + "-" + std::to_string(++counter));
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

    /// Writes `contents` to `relative`, creating directories as needed.
    void write(const std::string& relative, const std::string& contents) const {
        const fs::path target = root_ / relative;
        std::error_code ignored;
        fs::create_directories(target.parent_path(), ignored);
        std::ofstream out(target, std::ios::binary);
        out << contents;
    }

    fs::path path(const std::string& relative) const { return root_ / relative; }

private:
    fs::path root_;
};

/// Loads `entry` with the given search path, failing the test if it did
/// not load cleanly.
ember::parser::LoadResult load(const Workspace& workspace, const std::string& entry,
                               const ember::parser::ModulePath& search = {}) {
    ember::ast::SourceMap sources;
    ember::parser::LoadResult result =
        ember::parser::load_program(workspace.path(entry), sources, search);
    if (!result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this program to load, but:\n" +
                                ember::ast::render_all(result.diagnostics, sources));
    }
    return result;
}

/// Loads `entry` expecting failure, and returns the diagnostics.
std::vector<ember::ast::Diagnostic> load_failure(
    const Workspace& workspace, const std::string& entry,
    const ember::parser::ModulePath& search = {}) {
    ember::ast::SourceMap sources;
    ember::parser::LoadResult result =
        ember::parser::load_program(workspace.path(entry), sources, search);
    if (result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected loading to fail, but it succeeded");
    }
    return std::move(result.diagnostics);
}

/// The file a named module was loaded from.
fs::path source_of(const ember::parser::LoadResult& loaded, const std::string& name) {
    for (const ember::parser::Module& module : loaded.modules) {
        if (module.name == name) {
            return module.path;
        }
    }
    return {};
}

/// Sets or clears an environment variable, whichever way the platform
/// spells it. Passing null clears.
void set_environment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
}

/// Clears `EMBER_MODULE_PATH` for the duration of a test and puts back
/// whatever was there, so a developer with one set does not fail the
/// suite.
class ScopedModulePathEnvironment {
public:
    explicit ScopedModulePathEnvironment(const char* value) {
        if (const char* existing = std::getenv("EMBER_MODULE_PATH")) {
            previous_ = existing;
            had_previous_ = true;
        }
        set_environment("EMBER_MODULE_PATH", value);
    }

    ~ScopedModulePathEnvironment() {
        set_environment("EMBER_MODULE_PATH", had_previous_ ? previous_.c_str() : nullptr);
    }

    ScopedModulePathEnvironment(const ScopedModulePathEnvironment&) = delete;
    ScopedModulePathEnvironment& operator=(const ScopedModulePathEnvironment&) = delete;

private:
    std::string previous_;
    bool had_previous_ = false;
};

const char* const kGreeter = "pub fn hello() -> int { return 1; }\n";

const char* const kUsesGreeter =
    "import greeter;\n"
    "pub fn main() { println(greeter::hello()); }\n";

}  // namespace

// ---------------------------------------------------------------------
// The old behaviour, unchanged
// ---------------------------------------------------------------------

EMBER_TEST(search_path_still_finds_a_module_beside_its_importer) {
    // With no search path at all, this is the whole of the old rule.
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);
    workspace.write("greeter.em", kGreeter);

    const ember::parser::LoadResult loaded = load(workspace, "main.em");
    EMBER_CHECK_EQ(loaded.modules.size(), std::size_t{2});
    EMBER_CHECK_EQ(source_of(loaded, "greeter"), workspace.path("greeter.em"));
}

// ---------------------------------------------------------------------
// The search path
// ---------------------------------------------------------------------

EMBER_TEST(search_path_finds_a_module_as_a_file_in_a_search_directory) {
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);
    workspace.write("vendor/greeter.em", kGreeter);

    const ember::parser::LoadResult loaded =
        load(workspace, "main.em", {workspace.path("vendor")});
    EMBER_CHECK_EQ(source_of(loaded, "greeter"), workspace.path("vendor/greeter.em"));
}

EMBER_TEST(search_path_finds_a_module_shipped_as_a_directory) {
    // `<dir>/greeter/greeter.em`, which is how a package with more than
    // one file has to be laid out.
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);
    workspace.write("vendor/greeter/greeter.em", kGreeter);

    const ember::parser::LoadResult loaded =
        load(workspace, "main.em", {workspace.path("vendor")});
    EMBER_CHECK_EQ(source_of(loaded, "greeter"), workspace.path("vendor/greeter/greeter.em"));
}

EMBER_TEST(search_path_lets_a_package_keep_private_modules_of_its_own) {
    // This is the point of the directory form: `casing` is found beside
    // `greeter.em`, inside the package, without being on any search
    // path itself. A package can be more than one file.
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);
    workspace.write("vendor/greeter/greeter.em",
                    "import casing;\n"
                    "pub fn hello() -> int { return casing::shout(); }\n");
    workspace.write("vendor/greeter/casing.em", "pub fn shout() -> int { return 7; }\n");

    const ember::parser::LoadResult loaded =
        load(workspace, "main.em", {workspace.path("vendor")});
    EMBER_CHECK_EQ(source_of(loaded, "casing"), workspace.path("vendor/greeter/casing.em"));
}

EMBER_TEST(search_path_tries_its_directories_in_order) {
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);
    workspace.write("first/greeter.em", kGreeter);
    workspace.write("second/greeter.em", kGreeter);

    const ember::parser::LoadResult loaded =
        load(workspace, "main.em", {workspace.path("first"), workspace.path("second")});
    EMBER_CHECK_EQ(source_of(loaded, "greeter"), workspace.path("first/greeter.em"));
}

EMBER_TEST(search_path_prefers_a_file_to_a_directory_in_the_same_place) {
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);
    workspace.write("vendor/greeter.em", kGreeter);
    workspace.write("vendor/greeter/greeter.em", kGreeter);

    const ember::parser::LoadResult loaded =
        load(workspace, "main.em", {workspace.path("vendor")});
    EMBER_CHECK_EQ(source_of(loaded, "greeter"), workspace.path("vendor/greeter.em"));
}

EMBER_TEST(search_path_never_shadows_a_module_beside_the_importer) {
    // The rule that makes adding a dependency safe: a package cannot
    // take over a name the program is already using for its own module.
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);
    workspace.write("greeter.em", kGreeter);
    workspace.write("vendor/greeter.em", kGreeter);

    const ember::parser::LoadResult loaded =
        load(workspace, "main.em", {workspace.path("vendor")});
    EMBER_CHECK_EQ(source_of(loaded, "greeter"), workspace.path("greeter.em"));
}

// ---------------------------------------------------------------------
// When it cannot be found
// ---------------------------------------------------------------------

EMBER_TEST(search_path_says_everywhere_it_looked) {
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);

    const std::vector<ember::ast::Diagnostic> errors =
        load_failure(workspace, "main.em", {workspace.path("vendor")});
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"cannot find module `greeter`"});

    // Beside the importer, then the search directory two ways.
    EMBER_CHECK_EQ(errors.at(0).notes.size(), std::size_t{3});
    EMBER_CHECK_MSG(errors.at(0).notes.at(0).find("main.em") == std::string::npos ||
                        errors.at(0).notes.at(0).find("greeter.em") != std::string::npos,
                    "first place looked should be beside the importer: " +
                        errors.at(0).notes.at(0));
}

EMBER_TEST(search_path_suggests_the_flag_when_none_was_given) {
    // Only worth saying when they have not already configured one.
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);

    const std::vector<ember::ast::Diagnostic> errors = load_failure(workspace, "main.em");
    bool suggests = false;
    for (const std::string& note : errors.at(0).notes) {
        suggests = suggests || note.find("--module-path") != std::string::npos;
    }
    EMBER_CHECK_MSG(suggests, "no note pointing at `--module-path`");
}

EMBER_TEST(search_path_does_not_suggest_the_flag_when_one_was_given) {
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);

    const std::vector<ember::ast::Diagnostic> errors =
        load_failure(workspace, "main.em", {workspace.path("vendor")});
    for (const std::string& note : errors.at(0).notes) {
        EMBER_CHECK_MSG(note.find("--module-path") == std::string::npos,
                        "suggested a flag that was already used: " + note);
    }
}

// ---------------------------------------------------------------------
// Two files, one name
// ---------------------------------------------------------------------

EMBER_TEST(search_path_reports_two_files_claiming_one_module_name) {
    // Module names are global, so a package's private module can collide
    // with one of the program's. Before the search path this was hard to
    // arrange; now it is a thing that will happen, and silently using
    // whichever loaded first would produce nonsense errors later.
    const Workspace workspace;
    workspace.write("main.em",
                    "import greeter;\n"
                    "import casing;\n"
                    "pub fn main() { println(greeter::hello() + casing::shout()); }\n");
    workspace.write("casing.em", "pub fn shout() -> int { return 1; }\n");
    workspace.write("vendor/greeter/greeter.em",
                    "import casing;\n"
                    "pub fn hello() -> int { return casing::shout(); }\n");
    workspace.write("vendor/greeter/casing.em", "pub fn shout() -> int { return 2; }\n");

    const std::vector<ember::ast::Diagnostic> errors =
        load_failure(workspace, "main.em", {workspace.path("vendor")});
    EMBER_CHECK_EQ(errors.at(0).message,
                   std::string{"two files claim the module `casing`"});
    EMBER_CHECK_EQ(errors.at(0).notes.size(), std::size_t{2});
}

EMBER_TEST(search_path_allows_two_modules_to_import_the_same_file) {
    // The same module reached twice is ordinary; only two *different*
    // files under one name are a problem.
    const Workspace workspace;
    workspace.write("main.em",
                    "import greeter;\n"
                    "import shared;\n"
                    "pub fn main() { println(greeter::hello() + shared::value()); }\n");
    workspace.write("greeter.em",
                    "import shared;\n"
                    "pub fn hello() -> int { return shared::value(); }\n");
    workspace.write("shared.em", "pub fn value() -> int { return 3; }\n");

    const ember::parser::LoadResult loaded = load(workspace, "main.em");
    EMBER_CHECK_EQ(loaded.modules.size(), std::size_t{3});
}

// ---------------------------------------------------------------------
// Where the driver gets its search path from
// ---------------------------------------------------------------------

EMBER_TEST(module_search_path_uses_an_ember_modules_directory_beside_the_entry) {
    // The conventional place, so a vendored dependency needs no flag:
    // drop it in and `import` finds it.
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);
    workspace.write("ember_modules/greeter/greeter.em", kGreeter);
    const ScopedModulePathEnvironment environment{nullptr};

    const std::vector<fs::path> search =
        ember::cli::module_search_path(workspace.path("main.em"), {});
    EMBER_CHECK_EQ(search.size(), std::size_t{1});
    EMBER_CHECK_EQ(search.at(0), workspace.path("ember_modules"));

    const ember::parser::LoadResult loaded = load(workspace, "main.em", search);
    EMBER_CHECK_EQ(source_of(loaded, "greeter"),
                   workspace.path("ember_modules/greeter/greeter.em"));
}

EMBER_TEST(module_search_path_ignores_an_ember_modules_that_is_not_there) {
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);
    const ScopedModulePathEnvironment environment{nullptr};

    EMBER_CHECK(ember::cli::module_search_path(workspace.path("main.em"), {}).empty());
}

EMBER_TEST(module_search_path_reads_the_environment) {
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);
    const ScopedModulePathEnvironment environment{"/one"};

    const std::vector<fs::path> search =
        ember::cli::module_search_path(workspace.path("main.em"), {});
    EMBER_CHECK_EQ(search.size(), std::size_t{1});
    EMBER_CHECK_EQ(search.at(0), fs::path{"/one"});
}

EMBER_TEST(module_search_path_puts_the_explicit_flag_first) {
    // Explicit beats ambient beats conventional.
    const Workspace workspace;
    workspace.write("main.em", kUsesGreeter);
    workspace.write("ember_modules/greeter/greeter.em", kGreeter);
    const ScopedModulePathEnvironment environment{"/from-the-environment"};

    const std::vector<fs::path> search =
        ember::cli::module_search_path(workspace.path("main.em"), {fs::path{"/from-the-flag"}});
    EMBER_CHECK_EQ(search.size(), std::size_t{3});
    EMBER_CHECK_EQ(search.at(0), fs::path{"/from-the-flag"});
    EMBER_CHECK_EQ(search.at(1), fs::path{"/from-the-environment"});
    EMBER_CHECK_EQ(search.at(2), workspace.path("ember_modules"));
}

// ---------------------------------------------------------------------
// Nested module paths
//
// `shapes::geometry` is the file `shapes/geometry.em`. The path is
// resolved against the root the *importing module* was found under, not
// against the directory it happens to sit in, so it means the same thing
// written anywhere — which is what makes it a name rather than a
// direction.
// ---------------------------------------------------------------------

EMBER_TEST(nested_paths_are_directory_paths) {
    const Workspace workspace;
    workspace.write("main.em",
                    "import shapes::geometry;\n"
                    "pub fn main() { println(shapes::geometry::value()); }\n");
    workspace.write("shapes/geometry.em", "pub fn value() -> int { return 1; }\n");

    const ember::parser::LoadResult loaded = load(workspace, "main.em");
    EMBER_CHECK_EQ(source_of(loaded, "shapes::geometry"),
                   workspace.path("shapes/geometry.em"));
}

EMBER_TEST(nested_paths_go_as_deep_as_they_like) {
    const Workspace workspace;
    workspace.write("main.em",
                    "import a::b::c::d;\npub fn main() { println(a::b::c::d::value()); }\n");
    workspace.write("a/b/c/d.em", "pub fn value() -> int { return 1; }\n");

    const ember::parser::LoadResult loaded = load(workspace, "main.em");
    EMBER_CHECK_EQ(source_of(loaded, "a::b::c::d"), workspace.path("a/b/c/d.em"));
}

EMBER_TEST(nested_paths_mean_the_same_thing_from_a_nested_file) {
    // The one that decides the design. `shapes/geometry.em` writes the
    // full path, exactly as `main.em` does, and gets the same file -
    // rather than `shapes/shapes/detail/math.em`, which is what a path
    // relative to the importer would have meant.
    const Workspace workspace;
    workspace.write("main.em",
                    "import shapes::geometry;\n"
                    "pub fn main() { println(shapes::geometry::value()); }\n");
    workspace.write("shapes/geometry.em",
                    "import shapes::detail::math;\n"
                    "pub fn value() -> int { return shapes::detail::math::square(3); }\n");
    workspace.write("shapes/detail/math.em", "pub fn square(n: int) -> int { return n * n; }\n");

    const ember::parser::LoadResult loaded = load(workspace, "main.em");
    EMBER_CHECK_EQ(source_of(loaded, "shapes::detail::math"),
                   workspace.path("shapes/detail/math.em"));
}

EMBER_TEST(nested_paths_let_a_leaf_name_repeat) {
    // The whole reason for having them: two modules called `math` that
    // are not the same module.
    const Workspace workspace;
    workspace.write("main.em",
                    "import math;\n"
                    "import shapes::math;\n"
                    "pub fn main() { println(math::value() + shapes::math::value()); }\n");
    workspace.write("math.em", "pub fn value() -> int { return 1; }\n");
    workspace.write("shapes/math.em", "pub fn value() -> int { return 2; }\n");

    const ember::parser::LoadResult loaded = load(workspace, "main.em");
    EMBER_CHECK_EQ(loaded.modules.size(), std::size_t{3});
    EMBER_CHECK_EQ(source_of(loaded, "math"), workspace.path("math.em"));
    EMBER_CHECK_EQ(source_of(loaded, "shapes::math"), workspace.path("shapes/math.em"));
}

EMBER_TEST(nested_paths_do_not_need_their_prefix_to_exist) {
    // Nesting is a naming device. `shapes` is not a module, is not
    // imported, and does not have to be anything at all.
    const Workspace workspace;
    workspace.write("main.em",
                    "import shapes::geometry;\n"
                    "pub fn main() { println(shapes::geometry::value()); }\n");
    workspace.write("shapes/geometry.em", "pub fn value() -> int { return 1; }\n");

    const ember::parser::LoadResult loaded = load(workspace, "main.em");
    EMBER_CHECK_EQ(loaded.modules.size(), std::size_t{2});
}

EMBER_TEST(nested_paths_resolve_against_a_package_root) {
    // A package found as `<dir>/name/name.em` keeps resolving its own
    // modules against `<dir>/name`, so it can name its internals after
    // itself without them landing outside the package.
    const Workspace workspace;
    workspace.write("main.em",
                    "import greeter;\npub fn main() { println(greeter::value()); }\n");
    workspace.write("vendor/greeter/greeter.em",
                    "import greeter::casing;\n"
                    "pub fn value() -> int { return greeter::casing::shout(); }\n");
    workspace.write("vendor/greeter/greeter/casing.em",
                    "pub fn shout() -> int { return 7; }\n");

    const ember::parser::LoadResult loaded =
        load(workspace, "main.em", {workspace.path("vendor")});
    EMBER_CHECK_EQ(source_of(loaded, "greeter::casing"),
                   workspace.path("vendor/greeter/greeter/casing.em"));
}

EMBER_TEST(nested_paths_say_where_they_looked) {
    const Workspace workspace;
    workspace.write("main.em",
                    "import shapes::nowhere;\npub fn main() { println(1); }\n");

    const std::vector<ember::ast::Diagnostic> errors = load_failure(workspace, "main.em");
    EMBER_CHECK_EQ(errors.at(0).message,
                   std::string{"cannot find module `shapes::nowhere`"});
    EMBER_CHECK_MSG(errors.at(0).notes.at(0).find("nowhere") != std::string::npos,
                    errors.at(0).notes.at(0));
}
