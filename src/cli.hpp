// Command-line surface for the `cinder` driver (§6).
//
// Argument parsing lives here rather than inside main() so the test
// binary can link it directly and assert on the exact wording of usage
// errors. It is hand-written rather than delegated to a library: the
// surface is three subcommands, and doing it by hand keeps the project
// dependency-free and keeps full control of diagnostics, matching the
// parser philosophy in §2.

#ifndef CINDER_CLI_HPP
#define CINDER_CLI_HPP

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cinder::cli {

/// Process exit codes. 0/1/2 follows the C convention: success, the work
/// failed, the invocation was wrong.
inline constexpr int kExitSuccess = 0;
inline constexpr int kExitCompileError = 1;
inline constexpr int kExitUsage = 2;

/// Text printed for `--help` and after a usage error.
extern const std::string_view kUsage;

enum class CommandKind {
    Build,
    Run,
    Check,
    /// Resolve and download what the manifest depends on, without
    /// building anything.
    Fetch,
    /// Record this package's current version in the registry index.
    Publish,
    /// Write out a module's public interface.
    Interface,
    Help,
    Version,
};

/// How to drive the back end. Both flags exist because compilation is
/// now incremental: without them there is no way to see what was reused
/// and no way to distrust it.
struct BuildOptions {
    /// Say which modules were compiled and which came from the cache.
    bool verbose = false;
    /// Ignore cached objects and lower every module again.
    bool fresh = false;
    /// What `-O` asked for, 0 to 3. Zero is the default, as it is for
    /// every C compiler: an unoptimized build compiles faster and its
    /// generated code still resembles the source it came from.
    unsigned optimization_level = 0;
    /// `--link` arguments: object files or libraries to hand the linker
    /// alongside what cinder compiled. This is how a module imported
    /// through an interface gets a body.
    std::vector<std::filesystem::path> link;
    /// `--lib`: compile to an object file rather than an executable, and
    /// do not ask for a `main`. This is the other half of shipping a
    /// library - the object that an interface describes.
    bool library = false;
    /// `--whole-program`: fold every module into one object instead of
    /// compiling them separately. Gives up incremental builds and gets
    /// back what separate compilation cost - a cross-module call becomes
    /// a direct call in one LLVM module, which `-O` can then inline.
    bool whole_program = false;

    friend bool operator==(const BuildOptions&, const BuildOptions&) = default;
};

/// A successfully parsed command line.
struct Command {
    CommandKind kind = CommandKind::Help;
    /// Input `.ci` file. Empty for Help and Version.
    std::filesystem::path input;
    /// `-o` argument. Only ever set for Build.
    std::optional<std::filesystem::path> output;
    /// Ignored by Check, Help and Version.
    BuildOptions build;
    /// `--module-path` arguments, in the order given. Unlike the build
    /// options this belongs to `check` as well: finding a module is the
    /// front end's problem, not the back end's.
    std::vector<std::filesystem::path> module_path;
    /// `--update`: re-resolve git dependencies instead of using the
    /// revisions `cinder.lock` pinned.
    bool update = false;
    /// `--dry-run`: say what `publish` would record, and record nothing.
    bool dry_run = false;

    friend bool operator==(const Command&, const Command&) = default;
};

/// Why a command line was rejected. The message is the part after
/// "error: " so callers can render it however they like.
struct UsageError {
    std::string message;

    friend bool operator==(const UsageError&, const UsageError&) = default;
};

using ParseResult = std::variant<Command, UsageError>;

/// Parse the arguments *after* the program name.
ParseResult parse_args(std::span<const std::string_view> args);

/// Default output path for `build` when `-o` was not given: the input
/// path with the platform executable suffix in place of `.ci`.
std::filesystem::path default_output_path(const std::filesystem::path& input);

/// Tell the driver where its own binary is, so a rebuilt compiler does
/// not reuse object files the previous one produced.
///
/// The version string alone cannot carry this: two builds of the
/// compiler from different sources share a version until someone
/// remembers to bump it. `argv[0]` is the portable way to ask, and a
/// path that resolves to nothing simply contributes nothing.
void set_compiler_path(const std::filesystem::path& path);

/// e.g. "cinder 0.1.0".
std::string version_string();

/// Assembles the directories an imported module is looked for in,
/// after the directory of the file that imported it.
///
/// In order: what `--module-path` asked for, then the packages the
/// manifest resolved to, then `CINDER_MODULE_PATH` from the environment,
/// then an `cinder_modules` directory beside the entry file if one
/// exists. Explicit beats declared beats ambient beats conventional,
/// which is the order every toolchain settles on eventually.
std::vector<std::filesystem::path> module_search_path(
    const std::filesystem::path& entry, const std::vector<std::filesystem::path>& requested,
    const std::vector<std::filesystem::path>& packages = {});

/// What resolving a manifest produced.
struct PackageResolution {
    /// One `src` directory per resolved package, in dependency order.
    std::vector<std::filesystem::path> search_path;
    /// False when a manifest was found but could not be used. A program
    /// with no manifest at all succeeds with nothing resolved.
    bool ok = true;
};

/// Find the manifest above `entry`, resolve what it depends on, fetch
/// anything missing, and write `cinder.lock`.
///
/// A program with no manifest is not an error: most of them are one
/// file and depend on nothing. Diagnostics go to stderr.
PackageResolution resolve_packages(const std::filesystem::path& entry, bool update,
                                   bool verbose);

/// `cinder fetch` (§6): resolve and download, and stop there. Returns a
/// process exit code.
int fetch_packages(const std::filesystem::path& from, bool update);

/// `cinder interface <file.ci> [-o <out.cii>]` (§6): write the module's
/// public surface, with the implementations taken out.
///
/// The file this produces can be imported in place of the module it
/// describes, so a program can be built against a library it does not
/// have the source of - provided it links the library's object.
int write_interface(const std::filesystem::path& input,
                    const std::optional<std::filesystem::path>& output,
                    const std::vector<std::filesystem::path>& module_path = {});

/// `cinder publish` (§6): record this package's version in the registry
/// index, and stop short of pushing it.
///
/// Everything up to the push is a local, reversible act: the index entry
/// is written and committed in cinder's own checkout of the index, and
/// the command prints the `git push` that would make it public. Sending
/// it is the author's to do — publishing is irreversible in the way that
/// matters, since a version, once out, has to go on meaning what it
/// meant.
int publish_package(const std::filesystem::path& from, bool dry_run);

/// Run the front end over `input` - lex, parse, type-check - printing
/// any diagnostics to stderr in the §7 format. Returns a process exit
/// code: 0 when the program is clean, kExitCompileError otherwise.
///
/// This is `cinder check` (§6). `cinder build` runs the same front end and
/// then hands the checked program to codegen.
int check_file(const std::filesystem::path& input,
               const std::vector<std::filesystem::path>& module_path = {},
               bool update = false);

/// Compile `input` to a native executable at `output` (§6, `cinder build`).
///
/// Each module is lowered to its own object file, cached under `.cinder`
/// beside the entry source and reused when nothing it depends on has
/// changed. `options.fresh` skips the cache.
int build_file(const std::filesystem::path& input, const std::filesystem::path& output,
               const BuildOptions& options = {},
               const std::vector<std::filesystem::path>& module_path = {},
               bool update = false);

/// Compile `input` to a temporary executable, run it, and return its
/// exit code (§6, `cinder run`).
int run_file(const std::filesystem::path& input, const BuildOptions& options = {},
             const std::vector<std::filesystem::path>& module_path = {},
             bool update = false);

/// The linker command the build uses, for diagnostics and the README.
std::string linker_command();

}  // namespace cinder::cli

#endif  // CINDER_CLI_HPP
