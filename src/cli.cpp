#include "cli.hpp"

#include "ember/ast/ast.hpp"
#include "ember/ast/diagnostic.hpp"
#include "ember/ast/span.hpp"
#include "ember/codegen/codegen.hpp"
#include "ember/parser/parser.hpp"
#include "ember/typeck/typeck.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <system_error>

// Baked in by CMake so the driver can find the runtime it has to link
// into every compiled program, and the toolchain that does the linking.
#ifndef EMBER_RUNTIME_LIBRARY
#define EMBER_RUNTIME_LIBRARY ""
#endif
#ifndef EMBER_LINKER
#define EMBER_LINKER "c++"
#endif

namespace ember::cli {
namespace {

UsageError usage_error(std::string message) { return UsageError{std::move(message)}; }

/// Ember only compiles `.em` files. Renaming the language means changing
/// ember::ast::kFileExtension; this check follows automatically.
std::variant<std::filesystem::path, UsageError> check_extension(std::string_view arg) {
    const std::filesystem::path path{std::string{arg}};
    const std::string expected{ember::ast::kFileExtension};
    const std::string extension = path.extension().string();
    if (extension.size() > 1 && extension.substr(1) == expected) {
        return path;
    }
    return usage_error("expected a `." + expected + "` source file, found `" +
                       std::string{arg} + "`");
}

bool is_flag(std::string_view arg) { return !arg.empty() && arg.front() == '-'; }

/// `run` and `check` both take exactly one positional argument.
ParseResult parse_single_input(CommandKind kind, std::string_view subcommand,
                               std::span<const std::string_view> args) {
    if (args.empty()) {
        return usage_error("`" + std::string{subcommand} + "` requires an input file");
    }
    if (is_flag(args.front())) {
        return usage_error("unknown option `" + std::string{args.front()} + "`");
    }
    if (args.size() > 1) {
        return usage_error("unexpected extra argument `" + std::string{args[1]} + "`");
    }

    auto checked = check_extension(args.front());
    if (const auto* error = std::get_if<UsageError>(&checked)) {
        return *error;
    }

    Command command;
    command.kind = kind;
    command.input = std::get<std::filesystem::path>(checked);
    return command;
}

ParseResult parse_build(std::span<const std::string_view> args) {
    std::optional<std::filesystem::path> input;
    std::optional<std::filesystem::path> output;

    for (std::size_t i = 0; i < args.size();) {
        const std::string_view arg = args[i];
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= args.size()) {
                return usage_error("`-o` requires a path argument");
            }
            if (output.has_value()) {
                return usage_error("`-o` given more than once");
            }
            output = std::filesystem::path{std::string{args[i + 1]}};
            i += 2;
        } else if (is_flag(arg)) {
            return usage_error("unknown option `" + std::string{arg} + "`");
        } else {
            if (input.has_value()) {
                return usage_error("unexpected extra argument `" + std::string{arg} + "`");
            }
            auto checked = check_extension(arg);
            if (const auto* error = std::get_if<UsageError>(&checked)) {
                return *error;
            }
            input = std::get<std::filesystem::path>(checked);
            i += 1;
        }
    }

    if (!input.has_value()) {
        return usage_error("`build` requires an input file");
    }

    Command command;
    command.kind = CommandKind::Build;
    command.input = *input;
    command.output = output;
    return command;
}

}  // namespace

const std::string_view kUsage =
    "ember - the Ember compiler\n"
    "\n"
    "USAGE:\n"
    "    ember build <file.em> [-o <output>]   compile to a native executable\n"
    "    ember run <file.em>                   compile and run in one step\n"
    "    ember check <file.em>                 type-check only, no codegen\n"
    "\n"
    "OPTIONS:\n"
    "    -o, --output <path>   output path for `build` (default: input stem)\n"
    "    -h, --help            print this message\n"
    "    -V, --version         print version information";

ParseResult parse_args(std::span<const std::string_view> args) {
    if (args.empty()) {
        return usage_error("no subcommand given");
    }

    const std::string_view first = args.front();
    if (first == "-h" || first == "--help" || first == "help") {
        return Command{CommandKind::Help, {}, std::nullopt};
    }
    if (first == "-V" || first == "--version" || first == "version") {
        return Command{CommandKind::Version, {}, std::nullopt};
    }

    const std::span<const std::string_view> rest = args.subspan(1);
    if (first == "build") {
        return parse_build(rest);
    }
    if (first == "run") {
        return parse_single_input(CommandKind::Run, "run", rest);
    }
    if (first == "check") {
        return parse_single_input(CommandKind::Check, "check", rest);
    }
    return usage_error("unknown subcommand `" + std::string{first} + "`");
}

std::filesystem::path default_output_path(const std::filesystem::path& input) {
    std::filesystem::path output = input;
#ifdef _WIN32
    output.replace_extension("exe");
#else
    output.replace_extension();
#endif
    return output;
}

std::string version_string() { return "ember " + std::string{ember::ast::version()}; }

namespace {

/// Everything the back end needs from a successful front-end run.
struct FrontEnd {
    std::optional<ast::SourceFile> source;
    parser::ParseResult parsed;
    typeck::CheckResult checked;
    bool ok = false;
};

/// Lex, parse and type-check, reporting to stderr and stopping at the
/// first stage that fails: a bad token stream makes the parse
/// meaningless, and a bad tree makes the types meaningless, so
/// continuing would only bury the real error.
FrontEnd run_front_end(const std::filesystem::path& input) {
    FrontEnd result;

    result.source = ast::SourceFile::load(input);
    if (!result.source.has_value()) {
        std::cerr << "error: cannot read `" << input.string() << "`\n";
        return result;
    }

    result.parsed = parser::parse_source(*result.source);
    if (!result.parsed.ok()) {
        std::cerr << ast::render_all(result.parsed.diagnostics, *result.source);
        return result;
    }

    result.checked = typeck::check(*result.parsed.program, *result.source);
    if (!result.checked.ok()) {
        std::cerr << ast::render_all(result.checked.diagnostics, *result.source);
        return result;
    }

    result.ok = true;
    return result;
}

/// Quote a path for the shell. The project path itself contains spaces,
/// so this is not optional.
std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

/// Run a command line, returning its exit code.
int run_command(const std::string& command) {
#ifdef _WIN32
    // cmd.exe strips the outer quotes of a command that begins with one,
    // so a fully quoted command needs a second pair around the whole
    // thing to survive.
    const std::string wrapped = "\"" + command + "\"";
#else
    const std::string& wrapped = command;
#endif
    return std::system(wrapped.c_str());
}

/// A scratch path beside the output, removed when this object dies.
class ScratchFile {
public:
    explicit ScratchFile(std::filesystem::path path) : path_(std::move(path)) {}

    ~ScratchFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// Link an object file against the Ember runtime to produce `output`.
/// Returns an exit code.
int link_executable(const std::filesystem::path& object, const std::filesystem::path& output) {
    const std::filesystem::path runtime{EMBER_RUNTIME_LIBRARY};
    if (runtime.empty() || !std::filesystem::exists(runtime)) {
        std::cerr << "error: cannot find the Ember runtime library\n";
        std::cerr << "note: expected it at `" << runtime.string() << "`\n";
        return kExitCompileError;
    }

    const std::string command = quote(std::filesystem::path{EMBER_LINKER}) + " " +
                                quote(object) + " " + quote(runtime) + " -o " + quote(output);

    if (run_command(command) != 0) {
        std::cerr << "error: linking failed\n";
        std::cerr << "note: the link step was: " << command << "\n";
        return kExitCompileError;
    }
    return kExitSuccess;
}

/// Compile a checked program all the way to a native executable.
int emit_executable(const FrontEnd& front_end, const std::filesystem::path& output) {
    if (!codegen::is_available()) {
        std::cerr << "error: this build of ember has no code generator\n";
        std::cerr << "note: the compiler was built without LLVM; reconfigure with "
                     "-DLLVM_DIR=<prefix>/lib/cmake/llvm\n";
        return kExitCompileError;
    }

    // An executable needs an entry point. This is not a type error, so
    // it is checked here rather than by `ember check`.
    const std::vector<ast::Diagnostic> entry = codegen::verify_entry_point(front_end.checked);
    if (!entry.empty()) {
        std::cerr << ast::render_all(entry, *front_end.source);
        return kExitCompileError;
    }

    std::filesystem::path object = output;
    object += ".o";
    const ScratchFile scratch{object};

    codegen::CompileOptions options;
    options.output = codegen::OutputKind::Object;
    options.module_name = front_end.source->path();

    const codegen::CompileResult compiled =
        codegen::compile(*front_end.parsed.program, front_end.checked, *front_end.source,
                         scratch.path(), options);
    if (!compiled.ok()) {
        std::cerr << ast::render_all(compiled.diagnostics, *front_end.source);
        return kExitCompileError;
    }

    return link_executable(scratch.path(), output);
}

}  // namespace

std::string linker_command() { return EMBER_LINKER; }

int check_file(const std::filesystem::path& input) {
    return run_front_end(input).ok ? kExitSuccess : kExitCompileError;
}

int build_file(const std::filesystem::path& input, const std::filesystem::path& output) {
    const FrontEnd front_end = run_front_end(input);
    if (!front_end.ok) {
        return kExitCompileError;
    }
    return emit_executable(front_end, output);
}

int run_file(const std::filesystem::path& input) {
    const FrontEnd front_end = run_front_end(input);
    if (!front_end.ok) {
        return kExitCompileError;
    }

    // Build beside the source under a name unlikely to collide, run it,
    // and take it away again.
    std::random_device entropy;
    std::filesystem::path executable =
        std::filesystem::temp_directory_path() /
        ("ember-run-" + std::to_string(entropy()) + std::string{".exe"});

    const ScratchFile scratch{executable};
    if (emit_executable(front_end, scratch.path()) != kExitSuccess) {
        return kExitCompileError;
    }

    // The program's own exit code is this process's exit code, so `ember
    // run` is transparent to whatever it launched.
    return run_command(quote(scratch.path()));
}

}  // namespace ember::cli
