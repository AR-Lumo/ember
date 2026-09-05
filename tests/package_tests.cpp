// The package manager: `ember.toml`, dependency resolution, `ember.lock`.
//
// A package is a directory with a manifest and its modules in `src/`.
// Depending on one puts that `src/` on the module search path — which is
// all a dependency has ever been here, and the reason the search path
// was built first.
//
// Two things are worth stating plainly because they shape every test
// below. There is **no registry**, so a dependency names a directory or
// a git repository outright. And there is **no version solving**: two
// packages wanting different revisions of a third is an error naming
// both, not a negotiation. Ember has no index to search for a version
// that would satisfy them, and pretending to solve without one would be
// worse than saying so.
//
// Fetching is injected, so everything here runs without a network — the
// git path is exercised end to end by the driver tests instead.

#include "test_harness.hpp"

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/span.hpp"
#include "ember/codegen/codegen.hpp"
#include "ember/manifest/manifest.hpp"
#include "ember/manifest/toml.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using ember::ast::Diagnostic;
using ember::ast::SourceFile;
using ember::manifest::SourceKind;

// -------------------------------------------------------------------
// Manifest parsing
// -------------------------------------------------------------------

ember::manifest::ManifestResult parse(const std::string& text,
                                      const std::string& path = "/project/ember.toml") {
    return ember::manifest::parse_manifest(SourceFile{path, text});
}

/// Parses a manifest that is meant to be good.
ember::manifest::Manifest good(const std::string& text) {
    ember::manifest::ManifestResult result = parse(text);
    if (!result.ok()) {
        const SourceFile source{"/project/ember.toml", text};
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this manifest to parse, but:\n" +
                                ember::ast::render_all(result.diagnostics, source));
    }
    return std::move(*result.manifest);
}

/// Parses a manifest that is meant to be bad, and returns the first
/// complaint.
std::string bad(const std::string& text) {
    ember::manifest::ManifestResult result = parse(text);
    if (result.diagnostics.empty()) {
        ::ember::test::fail(__FILE__, __LINE__, "expected this manifest to be rejected");
    }
    return result.diagnostics.front().message;
}

const char* const kHeader =
    "[package]\n"
    "name = \"app\"\n"
    "version = \"0.1.0\"\n";

}  // namespace

EMBER_TEST(manifest_reads_a_package_name_and_version) {
    const ember::manifest::Manifest manifest = good(kHeader);
    EMBER_CHECK_EQ(manifest.name, std::string{"app"});
    EMBER_CHECK_EQ(manifest.version, std::string{"0.1.0"});
    EMBER_CHECK_EQ(manifest.root, fs::path{"/project"});
    EMBER_CHECK_EQ(manifest.source_directory(), fs::path{"/project/src"});
}

EMBER_TEST(manifest_ignores_comments_and_blank_lines) {
    const ember::manifest::Manifest manifest = good("# a package\n"
                                                    "\n"
                                                    "[package]   # named here\n"
                                                    "name = \"app\"\n"
                                                    "\n"
                                                    "version = \"0.1.0\"\n");
    EMBER_CHECK_EQ(manifest.name, std::string{"app"});
}

EMBER_TEST(manifest_reads_a_path_dependency) {
    const ember::manifest::Manifest manifest =
        good(std::string{kHeader} + "\n[dependencies]\ntextkit = { path = \"../textkit\" }\n");
    EMBER_CHECK_EQ(manifest.dependencies.size(), std::size_t{1});
    EMBER_CHECK(manifest.dependencies.at(0).kind == SourceKind::Path);
    EMBER_CHECK_EQ(manifest.dependencies.at(0).name, std::string{"textkit"});
    EMBER_CHECK_EQ(manifest.dependencies.at(0).location, std::string{"../textkit"});
}

EMBER_TEST(manifest_reads_a_git_dependency) {
    const ember::manifest::Manifest manifest =
        good(std::string{kHeader} +
             "\n[dependencies]\nhttpkit = { git = \"https://example.invalid/h\", rev = \"v1\" }\n");
    EMBER_CHECK(manifest.dependencies.at(0).kind == SourceKind::Git);
    EMBER_CHECK_EQ(manifest.dependencies.at(0).location,
                   std::string{"https://example.invalid/h"});
    EMBER_CHECK_EQ(manifest.dependencies.at(0).rev, std::string{"v1"});
}

EMBER_TEST(manifest_requires_a_package_section) {
    EMBER_CHECK_EQ(bad("[dependencies]\n"),
                   std::string{"manifest has no `[package]` section"});
}

EMBER_TEST(manifest_requires_a_name_and_a_version) {
    EMBER_CHECK_EQ(bad("[package]\nversion = \"0.1.0\"\n"),
                   std::string{"package has no name"});
    EMBER_CHECK_EQ(bad("[package]\nname = \"app\"\n"), std::string{"package has no version"});
}

EMBER_TEST(manifest_rejects_a_name_that_is_not_an_identifier) {
    // A package name is also a module name, so it has to be one.
    EMBER_CHECK_EQ(bad("[package]\nname = \"my-app\"\nversion = \"1\"\n"),
                   std::string{"`my-app` is not a usable package name"});
}

EMBER_TEST(manifest_rejects_a_bare_version_string) {
    // The Cargo spelling for a registry dependency, which is the thing
    // Ember most conspicuously does not have.
    const std::string message =
        bad(std::string{kHeader} + "\n[dependencies]\nserde = \"1.0\"\n");
    EMBER_CHECK_EQ(message, std::string{"dependency `serde` is not a table"});

    const ember::manifest::ManifestResult result =
        parse(std::string{kHeader} + "\n[dependencies]\nserde = \"1.0\"\n");
    bool mentions_registry = false;
    for (const std::string& note : result.diagnostics.front().notes) {
        mentions_registry = mentions_registry || note.find("registry") != std::string::npos;
    }
    EMBER_CHECK_MSG(mentions_registry, "the error should say why a version alone cannot work");
}

EMBER_TEST(manifest_rejects_a_dependency_with_two_sources) {
    EMBER_CHECK_EQ(
        bad(std::string{kHeader} +
            "\n[dependencies]\nx = { path = \"../x\", git = \"https://example.invalid/x\" }\n"),
        std::string{"dependency `x` has two sources"});
}

EMBER_TEST(manifest_rejects_a_dependency_with_no_source) {
    EMBER_CHECK_EQ(bad(std::string{kHeader} + "\n[dependencies]\nx = { }\n"),
                   std::string{"dependency `x` has no source"});
}

EMBER_TEST(manifest_requires_a_revision_on_a_git_dependency) {
    // Without one the build is whatever somebody pushed this morning.
    EMBER_CHECK_EQ(
        bad(std::string{kHeader} +
            "\n[dependencies]\nx = { git = \"https://example.invalid/x\" }\n"),
        std::string{"git dependency `x` has no revision"});
}

EMBER_TEST(manifest_rejects_a_revision_on_a_path_dependency) {
    EMBER_CHECK_EQ(
        bad(std::string{kHeader} + "\n[dependencies]\nx = { path = \"../x\", rev = \"v1\" }\n"),
        std::string{"a path dependency cannot have a revision"});
}

EMBER_TEST(manifest_rejects_depending_on_itself) {
    EMBER_CHECK_EQ(bad(std::string{kHeader} + "\n[dependencies]\napp = { path = \"../app\" }\n"),
                   std::string{"a package cannot depend on itself"});
}

EMBER_TEST(manifest_rejects_what_it_does_not_understand) {
    // Silence would let somebody believe a key took effect.
    EMBER_CHECK_EQ(bad("[package]\nname = \"app\"\nversion = \"1\"\nauthors = \"me\"\n"),
                   std::string{"unknown key `authors` in `[package]`"});
    EMBER_CHECK_EQ(bad(std::string{kHeader} + "\n[features]\nfast = \"yes\"\n"),
                   std::string{"unknown section `[features]`"});
}

// -------------------------------------------------------------------
// The TOML subset itself
// -------------------------------------------------------------------

EMBER_TEST(toml_reports_an_unterminated_string) {
    EMBER_CHECK_EQ(bad("[package]\nname = \"app\n"), std::string{"unterminated string"});
}

EMBER_TEST(toml_reports_a_value_that_is_not_a_string) {
    // No numbers, no booleans: the subset is strings and inline tables.
    EMBER_CHECK_EQ(bad("[package]\nname = app\n"), std::string{"expected a quoted string"});
}

EMBER_TEST(toml_reports_a_duplicate_key_and_a_duplicate_section) {
    EMBER_CHECK_EQ(bad("[package]\nname = \"a\"\nname = \"b\"\n"),
                   std::string{"duplicate key `name`"});
    EMBER_CHECK_EQ(bad("[package]\nname = \"a\"\nversion = \"1\"\n[package]\n"),
                   std::string{"duplicate section `package`"});
}

EMBER_TEST(toml_reports_an_unclosed_section_header) {
    EMBER_CHECK_EQ(bad("[package\nname = \"a\"\n"), std::string{"unclosed section header"});
}

EMBER_TEST(toml_points_at_the_part_that_is_wrong) {
    // Manifest errors get the same §7 treatment a program does, which is
    // most of why the manifest is parsed here rather than by a library.
    const ember::manifest::ManifestResult result =
        parse(std::string{kHeader} + "\n[dependencies]\nserde = \"1.0\"\n");
    const SourceFile source{"/project/ember.toml",
                            std::string{kHeader} + "\n[dependencies]\nserde = \"1.0\"\n"};
    const std::string rendered = ember::ast::render(result.diagnostics.front(), source);
    EMBER_CHECK_MSG(rendered.find("ember.toml:6:9") != std::string::npos, rendered);
    // One caret per character of `"1.0"`, quotes included.
    EMBER_CHECK_MSG(rendered.find("^^^^^ expected") != std::string::npos, rendered);
}

// -------------------------------------------------------------------
// Resolution
// -------------------------------------------------------------------

namespace {

/// A tree of packages on disk, removed when the test ends.
class Workspace {
public:
    Workspace() {
        // Unique per *run*, not just per test. Some of these hold git
        // repositories, whose leftovers cannot always be deleted (see
        // below); a stale one under a name a later run reused would make
        // that run fail for reasons that have nothing to do with it.
        static const unsigned run = std::random_device{}();
        static int counter = 0;
        root_ = fs::temp_directory_path() /
                ("ember-packages-" + std::to_string(run) + "-" + std::to_string(++counter));
        remove_tree(root_);
        std::error_code ignored;
        fs::create_directories(root_, ignored);
    }

    ~Workspace() { remove_tree(root_); }

    /// Deletes a tree that may contain a git repository.
    ///
    /// git marks its object files read-only, and Windows will not delete
    /// a read-only file however much the caller insists, so the
    /// permissions come off first.
    static void remove_tree(const fs::path& path) {
        std::error_code ignored;
        if (!fs::exists(path, ignored)) {
            return;
        }
        for (fs::recursive_directory_iterator it{
                 path, fs::directory_options::skip_permission_denied, ignored},
             end;
             it != end; it.increment(ignored)) {
            fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ignored);
        }
        fs::remove_all(path, ignored);
    }

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    void write(const std::string& relative, const std::string& contents) const {
        const fs::path target = root_ / relative;
        std::error_code ignored;
        fs::create_directories(target.parent_path(), ignored);
        std::ofstream out(target, std::ios::binary);
        out << contents;
    }

    /// A complete package: a manifest, a `src` directory, and a module.
    void package(const std::string& name, const std::string& dependencies = {}) const {
        write(name + "/ember.toml", "[package]\nname = \"" + name +
                                        "\"\nversion = \"1.0.0\"\n" +
                                        (dependencies.empty()
                                             ? std::string{}
                                             : "\n[dependencies]\n" + dependencies));
        write(name + "/src/" + name + ".em", "pub fn value() -> int { return 1; }\n");
    }

    fs::path path(const std::string& relative) const { return root_ / relative; }

private:
    fs::path root_;
};

/// A fetcher that hands back a directory already in the workspace, so
/// the git path can be exercised without git.
ember::manifest::Fetcher fake_fetcher(const Workspace& workspace,
                                      std::vector<std::string>* log = nullptr) {
    return [&workspace, log](const ember::manifest::Dependency& dependency,
                             const std::string& rev) {
        if (log != nullptr) {
            log->push_back(dependency.name + "@" + rev);
        }
        ember::manifest::Fetched fetched;
        fetched.root = workspace.path(dependency.location);
        // A real fetch turns a tag into the commit it names; this stands
        // in for that, so locking can be tested.
        fetched.rev = rev == "v1" ? std::string(40, 'a') : rev;
        return fetched;
    };
}

ember::manifest::Manifest load(const Workspace& workspace, const std::string& name) {
    const std::optional<SourceFile> file =
        SourceFile::load(workspace.path(name + "/ember.toml"));
    if (!file.has_value()) {
        ::ember::test::fail(__FILE__, __LINE__, "no manifest for " + name);
    }
    ember::manifest::ManifestResult result = ember::manifest::parse_manifest(*file);
    if (!result.manifest.has_value()) {
        ::ember::test::fail(__FILE__, __LINE__, "manifest for " + name + " does not parse");
    }
    return std::move(*result.manifest);
}

ember::manifest::Resolution resolve(const Workspace& workspace, const std::string& root,
                                    const ember::manifest::Lock& lock = {},
                                    std::vector<std::string>* log = nullptr) {
    ember::ast::SourceMap sources;
    return ember::manifest::resolve(load(workspace, root), lock, fake_fetcher(workspace, log),
                                    sources);
}

/// The names of what resolution found, in order.
std::vector<std::string> names(const ember::manifest::Resolution& resolution) {
    std::vector<std::string> out;
    for (const ember::manifest::ResolvedPackage& package : resolution.packages) {
        out.push_back(package.name);
    }
    return out;
}

}  // namespace

EMBER_TEST(resolve_finds_a_path_dependency) {
    const Workspace workspace;
    workspace.package("app", "textkit = { path = \"../textkit\" }\n");
    workspace.package("textkit");

    const ember::manifest::Resolution resolution = resolve(workspace, "app");
    EMBER_CHECK(resolution.ok());
    EMBER_CHECK_EQ(names(resolution), (std::vector<std::string>{"textkit"}));

    // Bound to a local first: `search_path()` returns by value, and a
    // reference into a temporary would dangle before the comparison.
    const std::vector<fs::path> search = resolution.search_path();
    EMBER_CHECK_EQ(search.at(0), workspace.path("textkit/src"));
}

EMBER_TEST(resolve_follows_dependencies_of_dependencies) {
    const Workspace workspace;
    workspace.package("app", "textkit = { path = \"../textkit\" }\n");
    workspace.package("textkit", "mathkit = { path = \"../mathkit\" }\n");
    workspace.package("mathkit");

    EMBER_CHECK_EQ(names(resolve(workspace, "app")),
                   (std::vector<std::string>{"textkit", "mathkit"}));
}

EMBER_TEST(resolve_visits_a_shared_dependency_once) {
    // A diamond is ordinary: both want the same thing from the same
    // place, so there is nothing to reconcile.
    const Workspace workspace;
    workspace.package("app",
                      "left = { path = \"../left\" }\nright = { path = \"../right\" }\n");
    workspace.package("left", "shared = { path = \"../shared\" }\n");
    workspace.package("right", "shared = { path = \"../shared\" }\n");
    workspace.package("shared");

    EMBER_CHECK_EQ(names(resolve(workspace, "app")),
                   (std::vector<std::string>{"left", "right", "shared"}));
}

EMBER_TEST(resolve_terminates_on_a_cycle_between_packages) {
    // The same bargain the module loader strikes: each is visited once,
    // so the walk ends rather than spinning.
    const Workspace workspace;
    workspace.package("app", "a = { path = \"../a\" }\n");
    workspace.package("a", "b = { path = \"../b\" }\n");
    workspace.package("b", "a = { path = \"../a\" }\n");

    const ember::manifest::Resolution resolution = resolve(workspace, "app");
    EMBER_CHECK(resolution.ok());
    EMBER_CHECK_EQ(names(resolution), (std::vector<std::string>{"a", "b"}));
}

EMBER_TEST(resolve_reports_two_packages_wanting_different_versions) {
    // With no registry there is nothing to solve, so the choice goes
    // back to whoever wrote the manifests - with both sides named.
    const Workspace workspace;
    workspace.package("app",
                      "left = { path = \"../left\" }\nright = { path = \"../right\" }\n");
    workspace.package("left", "shared = { path = \"../shared\" }\n");
    workspace.package("right", "shared = { path = \"../other/shared\" }\n");
    workspace.package("shared");
    workspace.package("other/shared");

    const ember::manifest::Resolution resolution = resolve(workspace, "app");
    EMBER_CHECK(!resolution.ok());
    EMBER_CHECK_EQ(resolution.diagnostics.at(0).message,
                   std::string{"two packages want different versions of `shared`"});
    EMBER_CHECK_EQ(resolution.diagnostics.at(0).notes.size(), std::size_t{2});
}

EMBER_TEST(resolve_reports_a_path_that_is_not_there) {
    const Workspace workspace;
    workspace.package("app", "textkit = { path = \"../textkit\" }\n");

    const ember::manifest::Resolution resolution = resolve(workspace, "app");
    EMBER_CHECK_EQ(resolution.diagnostics.at(0).message,
                   std::string{"cannot find package `textkit`"});
}

EMBER_TEST(resolve_reports_a_directory_with_no_manifest) {
    const Workspace workspace;
    workspace.package("app", "textkit = { path = \"../textkit\" }\n");
    workspace.write("textkit/src/textkit.em", "pub fn value() -> int { return 1; }\n");

    const ember::manifest::Resolution resolution = resolve(workspace, "app");
    EMBER_CHECK_EQ(resolution.diagnostics.at(0).message,
                   std::string{"package `textkit` has no manifest"});
}

EMBER_TEST(resolve_reports_a_package_that_calls_itself_something_else) {
    // The directory name has no authority; the manifest does. A mismatch
    // means `import textkit;` would not find what the author meant.
    const Workspace workspace;
    workspace.package("app", "textkit = { path = \"../textkit\" }\n");
    workspace.write("textkit/ember.toml", "[package]\nname = \"other\"\nversion = \"1\"\n");
    workspace.write("textkit/src/other.em", "pub fn value() -> int { return 1; }\n");

    const ember::manifest::Resolution resolution = resolve(workspace, "app");
    EMBER_CHECK_EQ(resolution.diagnostics.at(0).message,
                   std::string{"package `textkit` calls itself `other`"});
}

EMBER_TEST(resolve_reports_a_package_with_no_source_directory) {
    const Workspace workspace;
    workspace.package("app", "textkit = { path = \"../textkit\" }\n");
    workspace.write("textkit/ember.toml", "[package]\nname = \"textkit\"\nversion = \"1\"\n");

    const ember::manifest::Resolution resolution = resolve(workspace, "app");
    EMBER_CHECK_EQ(resolution.diagnostics.at(0).message,
                   std::string{"package `textkit` has no source directory"});
}

EMBER_TEST(resolve_reports_what_a_fetcher_could_not_get) {
    const Workspace workspace;
    workspace.package("app", "httpkit = { git = \"https://example.invalid/h\", rev = \"v1\" }\n");

    ember::ast::SourceMap sources;
    const ember::manifest::Resolution resolution = ember::manifest::resolve(
        load(workspace, "app"), {},
        [](const ember::manifest::Dependency&, const std::string&) {
            ember::manifest::Fetched failed;
            failed.error = "no such host";
            return failed;
        },
        sources);

    EMBER_CHECK_EQ(resolution.diagnostics.at(0).message,
                   std::string{"cannot fetch package `httpkit`"});
    EMBER_CHECK_EQ(resolution.diagnostics.at(0).notes.at(0), std::string{"no such host"});
}

// -------------------------------------------------------------------
// The lockfile
// -------------------------------------------------------------------

EMBER_TEST(lock_pins_the_revision_a_git_dependency_resolved_to) {
    const Workspace workspace;
    workspace.package("app", "mathkit = { git = \"mathkit\", rev = \"v1\" }\n");
    workspace.package("mathkit");

    std::vector<std::string> asked;
    const ember::manifest::Resolution first = resolve(workspace, "app", {}, &asked);
    EMBER_CHECK(first.ok());
    EMBER_CHECK_EQ(asked.at(0), std::string{"mathkit@v1"});
    EMBER_CHECK_EQ(first.packages.at(0).resolved_rev, std::string(40, 'a'));

    // Round-trip that through a lockfile, and the tag is not asked for
    // again — the commit it named is.
    const std::string text = ember::manifest::write_lock(first.packages);
    const ember::manifest::LockResult lock =
        ember::manifest::parse_lock(SourceFile{"/project/ember.lock", text});
    EMBER_CHECK(lock.ok());

    asked.clear();
    const ember::manifest::Resolution second = resolve(workspace, "app", lock.lock, &asked);
    EMBER_CHECK(second.ok());
    EMBER_CHECK_EQ(asked.at(0), "mathkit@" + std::string(40, 'a'));
}

EMBER_TEST(lock_is_ignored_when_it_pins_a_different_source) {
    // Changing the url in the manifest has to win over what the lock
    // says, or an edit would silently do nothing.
    const Workspace workspace;
    workspace.package("app", "mathkit = { git = \"mathkit\", rev = \"v1\" }\n");
    workspace.package("mathkit");

    ember::manifest::Lock lock;
    lock.entries.push_back(ember::manifest::LockEntry{"mathkit", SourceKind::Git,
                                                      "https://elsewhere.invalid/mathkit",
                                                      std::string(40, 'b')});

    std::vector<std::string> asked;
    resolve(workspace, "app", lock, &asked);
    EMBER_CHECK_EQ(asked.at(0), std::string{"mathkit@v1"});
}

EMBER_TEST(lock_is_written_sorted_by_name) {
    // So the file does not churn when resolution order changes but the
    // answer does not.
    std::vector<ember::manifest::ResolvedPackage> packages;
    for (const char* name : {"zeta", "alpha", "mid"}) {
        ember::manifest::ResolvedPackage package;
        package.name = name;
        package.location = name;
        packages.push_back(std::move(package));
    }

    const std::string text = ember::manifest::write_lock(packages);
    EMBER_CHECK(text.find("[alpha]") < text.find("[mid]"));
    EMBER_CHECK(text.find("[mid]") < text.find("[zeta]"));
}

EMBER_TEST(lock_reports_an_entry_it_cannot_use) {
    // Silently ignoring a broken lock would turn a repeatable build into
    // an unrepeatable one without saying so.
    const ember::manifest::LockResult result = ember::manifest::parse_lock(
        SourceFile{"/project/ember.lock", "[mathkit]\nrev = \"abc\"\n"});
    EMBER_CHECK(!result.ok());
    EMBER_CHECK_EQ(result.diagnostics.at(0).message,
                   std::string{"incomplete lock entry for `mathkit`"});
}

// -------------------------------------------------------------------
// Finding the manifest
// -------------------------------------------------------------------

EMBER_TEST(find_manifest_walks_up_from_a_source_file) {
    const Workspace workspace;
    workspace.package("app");

    const std::optional<fs::path> found =
        ember::manifest::find_manifest(workspace.path("app/src"));
    EMBER_CHECK(found.has_value());
    EMBER_CHECK_EQ(*found, workspace.path("app/ember.toml"));
}

EMBER_TEST(find_manifest_gives_up_at_the_root) {
    const Workspace workspace;
    workspace.write("lonely/main.em", "pub fn main() { }\n");

    // A program with no manifest is the ordinary case, not an error.
    EMBER_CHECK(!ember::manifest::find_manifest(workspace.path("lonely")).has_value());
}

// -------------------------------------------------------------------
// End to end, through the driver
//
// Everything above injects its fetching. These drive the real `ember`
// binary against a real git repository built in a temporary directory,
// which is the only way to cover the part that shells out to git — and
// the part that had the bugs. No network: git clones a local path
// perfectly well.
// -------------------------------------------------------------------

namespace {

struct ProcessResult {
    int exit_code = 0;
    std::string output;
};

ProcessResult run_process(const std::string& command) {
    const std::string redirected = command + " 2>&1";
#ifdef _WIN32
    const std::string wrapped = "\"" + redirected + "\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    FILE* pipe = popen(redirected.c_str(), "r");
#endif
    if (pipe == nullptr) {
        ::ember::test::fail(__FILE__, __LINE__, "cannot start: " + command);
    }
    ProcessResult result;
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        result.output += buffer;
    }
#ifdef _WIN32
    result.exit_code = _pclose(pipe);
#else
    result.exit_code = pclose(pipe);
#endif
    return result;
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

/// Whether there is a git to shell out to. Without one these tests have
/// nothing to say, and saying nothing beats failing a build for it.
bool git_is_available() {
    static const bool available = run_process("git --version").exit_code == 0;
    return available;
}

/// Runs a git command inside `directory`.
void git(const fs::path& directory, const std::string& arguments) {
    const ProcessResult result =
        run_process("git -C " + quoted(directory) + " " + arguments);
    if (result.exit_code != 0) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "git " + arguments + " failed:\n" + result.output);
    }
}

/// Builds a real repository holding one package, and returns its path.
fs::path make_repository(const Workspace& workspace, const std::string& name,
                         const std::string& body) {
    workspace.write(name + "/ember.toml",
                    "[package]\nname = \"" + name + "\"\nversion = \"1.0.0\"\n");
    workspace.write(name + "/src/" + name + ".em", body);

    const fs::path root = workspace.path(name);
    git(root, "init --quiet .");
    git(root, "config user.email tests@example.invalid");
    git(root, "config user.name Tests");
    git(root, "add -A");
    git(root, "commit --quiet -m \"first\"");
    git(root, "tag v1.0.0");
    return root;
}

std::string run_ember(const fs::path& directory, const std::string& arguments) {
    return run_process("cd " + quoted(directory) + " && " + quoted(fs::path{EMBER_BINARY}) +
                       " " + arguments)
        .output;
}

}  // namespace

EMBER_TEST(packages_are_fetched_built_and_run_end_to_end) {
    if (!git_is_available() || !ember::codegen::is_available()) {
        return;
    }
    const Workspace workspace;
    make_repository(workspace, "mathkit", "pub fn square(n: int) -> int { return n * n; }\n");

    workspace.write("app/ember.toml",
                    "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                    "\n[dependencies]\nmathkit = { git = \"" +
                        workspace.path("mathkit").generic_string() +
                        "\", rev = \"v1.0.0\" }\n");
    workspace.write("app/src/main.em",
                    "import mathkit;\npub fn main() { println(mathkit::square(9)); }\n");

    const std::string fetched = run_ember(workspace.path("app"), "fetch");
    EMBER_CHECK_MSG(fetched.find("1 package ready") != std::string::npos, fetched);
    EMBER_CHECK_MSG(fs::exists(workspace.path("app/ember.lock")), "no lockfile was written");

    const std::string ran = run_ember(workspace.path("app"), "run src/main.em");
    EMBER_CHECK_MSG(ran.find("81") != std::string::npos, ran);
}

EMBER_TEST(packages_keep_building_the_commit_the_lock_pinned) {
    if (!git_is_available() || !ember::codegen::is_available()) {
        return;
    }
    // The whole point of a lockfile: the manifest asks for a tag, the
    // tag moves, and the build does not.
    const Workspace workspace;
    const fs::path repository =
        make_repository(workspace, "mathkit", "pub fn square(n: int) -> int { return n * n; }\n");

    workspace.write("app/ember.toml",
                    "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                    "\n[dependencies]\nmathkit = { git = \"" + repository.generic_string() +
                        "\", rev = \"v1.0.0\" }\n");
    workspace.write("app/src/main.em",
                    "import mathkit;\npub fn main() { println(mathkit::square(9)); }\n");

    EMBER_CHECK_MSG(run_ember(workspace.path("app"), "run src/main.em").find("81") !=
                        std::string::npos,
                    "the first build should work");

    // Move the tag somewhere that answers differently.
    workspace.write("mathkit/src/mathkit.em", "pub fn square(n: int) -> int { return 0; }\n");
    git(repository, "add -A");
    git(repository, "commit --quiet -m \"square is now wrong\"");
    git(repository, "tag --force v1.0.0");

    const std::string locked = run_ember(workspace.path("app"), "run src/main.em");
    EMBER_CHECK_MSG(locked.find("81") != std::string::npos,
                    "the lock should have pinned the old commit:\n" + locked);

    // Checking that 81 is *gone*, not just that a zero turned up: a
    // stray digit in an error message would pass the weaker test.
    const std::string updated = run_ember(workspace.path("app"), "run src/main.em --update");
    EMBER_CHECK_MSG(updated.find("81") == std::string::npos &&
                        updated.find("0") != std::string::npos,
                    "`--update` should take the moved tag:\n" + updated);

    // And the new commit sticks, rather than flapping back.
    const std::string after = run_ember(workspace.path("app"), "run src/main.em");
    EMBER_CHECK_MSG(after.find("81") == std::string::npos &&
                        after.find("0") != std::string::npos,
                    "the updated lock should hold:\n" + after);
}

EMBER_TEST(packages_report_a_repository_that_is_not_there) {
    if (!git_is_available()) {
        return;
    }
    const Workspace workspace;
    workspace.write("app/ember.toml",
                    "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                    "\n[dependencies]\nnope = { git = \"" +
                        workspace.path("no-such-repository").generic_string() +
                        "\", rev = \"v1.0.0\" }\n");
    workspace.write("app/src/main.em", "pub fn main() { println(1); }\n");

    const std::string output = run_ember(workspace.path("app"), "check src/main.em");
    EMBER_CHECK_MSG(output.find("cannot fetch package `nope`") != std::string::npos, output);
}

EMBER_TEST(a_program_with_no_manifest_still_builds) {
    if (!ember::codegen::is_available()) {
        return;
    }
    // Most Ember programs are one file and depend on nothing. Requiring
    // a manifest for those would be a tax on the common case.
    const Workspace workspace;
    workspace.write("lonely/main.em", "pub fn main() { println(7); }\n");

    const std::string output = run_ember(workspace.path("lonely"), "run main.em");
    EMBER_CHECK_MSG(output.find("7") != std::string::npos, output);
}
