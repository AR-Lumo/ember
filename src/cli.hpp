// Command-line surface for the `ember` driver (§6).
//
// Argument parsing lives here rather than inside main() so the test
// binary can link it directly and assert on the exact wording of usage
// errors. It is hand-written rather than delegated to a library: the
// surface is three subcommands, and doing it by hand keeps the project
// dependency-free and keeps full control of diagnostics, matching the
// parser philosophy in §2.

#ifndef EMBER_CLI_HPP
#define EMBER_CLI_HPP

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace ember::cli {

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

    friend bool operator==(const BuildOptions&, const BuildOptions&) = default;
};

/// A successfully parsed command line.
struct Command {
    CommandKind kind = CommandKind::Help;
    /// Input `.em` file. Empty for Help and Version.
    std::filesystem::path input;
    /// `-o` argument. Only ever set for Build.
    std::optional<std::filesystem::path> output;
    /// Ignored by Check, Help and Version.
    BuildOptions build;

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
/// path with the platform executable suffix in place of `.em`.
std::filesystem::path default_output_path(const std::filesystem::path& input);

/// Tell the driver where its own binary is, so a rebuilt compiler does
/// not reuse object files the previous one produced.
///
/// The version string alone cannot carry this: two builds of the
/// compiler from different sources share a version until someone
/// remembers to bump it. `argv[0]` is the portable way to ask, and a
/// path that resolves to nothing simply contributes nothing.
void set_compiler_path(const std::filesystem::path& path);

/// e.g. "ember 0.1.0".
std::string version_string();

/// Run the front end over `input` - lex, parse, type-check - printing
/// any diagnostics to stderr in the §7 format. Returns a process exit
/// code: 0 when the program is clean, kExitCompileError otherwise.
///
/// This is `ember check` (§6). `ember build` runs the same front end and
/// then hands the checked program to codegen.
int check_file(const std::filesystem::path& input);

/// Compile `input` to a native executable at `output` (§6, `ember build`).
///
/// Each module is lowered to its own object file, cached under `.ember`
/// beside the entry source and reused when nothing it depends on has
/// changed. `options.fresh` skips the cache.
int build_file(const std::filesystem::path& input, const std::filesystem::path& output,
               const BuildOptions& options = {});

/// Compile `input` to a temporary executable, run it, and return its
/// exit code (§6, `ember run`).
int run_file(const std::filesystem::path& input, const BuildOptions& options = {});

/// The linker command the build uses, for diagnostics and the README.
std::string linker_command();

}  // namespace ember::cli

#endif  // EMBER_CLI_HPP
