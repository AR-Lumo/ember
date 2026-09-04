#include "cli.hpp"

#include "ember/ast/ast.hpp"
#include "ember/ast/diagnostic.hpp"
#include "ember/ast/span.hpp"
#include "ember/codegen/codegen.hpp"
#include "ember/parser/parser.hpp"
#include "ember/typeck/typeck.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <vector>

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

/// What the shared build-flag parser made of one argument.
struct FlagOutcome {
    /// Whether this argument was one of the build flags at all.
    bool recognized = false;
    /// Set when it was recognized but malformed, so a bad `-O` reports
    /// what is wrong with it instead of "unknown option".
    std::optional<std::string> error;
};

/// Flags that mean the same thing to `build` and `run`, because both of
/// them compile.
FlagOutcome parse_build_flag(std::string_view arg, BuildOptions& options) {
    if (arg == "-v" || arg == "--verbose") {
        options.verbose = true;
        return FlagOutcome{true, std::nullopt};
    }
    if (arg == "--fresh") {
        options.fresh = true;
        return FlagOutcome{true, std::nullopt};
    }

    // `-O2`, spelled as C spells it (§9). Lowercase `-o` is the output
    // path and is handled by `build` alone, so the case matters.
    if (arg.rfind("-O", 0) == 0) {
        const std::string_view level = arg.substr(2);
        if (level.size() == 1 && level.front() >= '0' && level.front() <= '3') {
            options.optimization_level = static_cast<unsigned>(level.front() - '0');
            return FlagOutcome{true, std::nullopt};
        }
        if (level.empty()) {
            // gcc reads a bare `-O` as `-O1` and clang as `-O2`. With
            // no agreement to follow, guessing would be worse than
            // asking.
            return FlagOutcome{
                true, "`-O` requires a level, e.g. `-O2` (gcc and clang disagree on what "
                      "a bare `-O` means, so ember does not guess)"};
        }
        return FlagOutcome{true, "expected an optimization level `-O0` through `-O3`, found `" +
                                     std::string{arg} + "`"};
    }
    return FlagOutcome{false, std::nullopt};
}

/// `run` and `check` both take exactly one positional argument. `run`
/// also takes the build flags, because it compiles before it runs;
/// `check` never reaches the back end, so it does not.
ParseResult parse_single_input(CommandKind kind, std::string_view subcommand,
                               std::span<const std::string_view> args) {
    std::optional<std::filesystem::path> input;
    BuildOptions options;

    for (const std::string_view arg : args) {
        if (kind == CommandKind::Run) {
            const FlagOutcome outcome = parse_build_flag(arg, options);
            if (outcome.error.has_value()) {
                return usage_error(*outcome.error);
            }
            if (outcome.recognized) {
                continue;
            }
        }
        if (is_flag(arg)) {
            return usage_error("unknown option `" + std::string{arg} + "`");
        }
        if (input.has_value()) {
            return usage_error("unexpected extra argument `" + std::string{arg} + "`");
        }
        auto checked = check_extension(arg);
        if (const auto* error = std::get_if<UsageError>(&checked)) {
            return *error;
        }
        input = std::get<std::filesystem::path>(checked);
    }

    if (!input.has_value()) {
        return usage_error("`" + std::string{subcommand} + "` requires an input file");
    }

    Command command;
    command.kind = kind;
    command.input = *input;
    command.build = options;
    return command;
}

ParseResult parse_build(std::span<const std::string_view> args) {
    std::optional<std::filesystem::path> input;
    std::optional<std::filesystem::path> output;
    BuildOptions options;

    for (std::size_t i = 0; i < args.size();) {
        const std::string_view arg = args[i];
        const FlagOutcome outcome = parse_build_flag(arg, options);
        if (outcome.error.has_value()) {
            return usage_error(*outcome.error);
        }
        if (outcome.recognized) {
            i += 1;
        } else if (arg == "-o" || arg == "--output") {
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
    command.build = options;
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
    "    -O0 .. -O3            optimization level (default: -O0)\n"
    "    -v, --verbose         report which modules were compiled and which were cached\n"
    "        --fresh           recompile every module, ignoring cached object files\n"
    "    -h, --help            print this message\n"
    "    -V, --version         print version information";

ParseResult parse_args(std::span<const std::string_view> args) {
    if (args.empty()) {
        return usage_error("no subcommand given");
    }

    const std::string_view first = args.front();
    if (first == "-h" || first == "--help" || first == "help") {
        return Command{CommandKind::Help, {}, std::nullopt, {}};
    }
    if (first == "-V" || first == "--version" || first == "version") {
        return Command{CommandKind::Version, {}, std::nullopt, {}};
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

namespace {

/// Where this process's own executable is, as `argv[0]` gave it.
std::filesystem::path& compiler_path() {
    static std::filesystem::path path;
    return path;
}

}  // namespace

void set_compiler_path(const std::filesystem::path& path) { compiler_path() = path; }

std::string version_string() { return "ember " + std::string{ember::ast::version()}; }

namespace {

/// Everything the back end needs from a successful front-end run.
struct FrontEnd {
    /// Every file the program is made of, so a diagnostic from any
    /// module can be rendered against the right source.
    ast::SourceMap sources;
    parser::LoadResult loaded;
    typeck::CheckResult checked;
    bool ok = false;

    /// The modules in the shape codegen wants.
    std::vector<codegen::ModuleInput> codegen_modules() const {
        std::vector<codegen::ModuleInput> modules;
        for (const parser::Module& module : loaded.modules) {
            modules.push_back(codegen::ModuleInput{module.name, module.program.get()});
        }
        return modules;
    }
};

/// Load, parse and type-check a whole program, reporting to stderr and
/// stopping at the first stage that fails: a bad token stream makes the
/// parse meaningless, and a bad tree makes the types meaningless, so
/// continuing would only bury the real error.
FrontEnd run_front_end(const std::filesystem::path& input) {
    FrontEnd result;

    // Loading pulls in every module the entry file imports, transitively.
    result.loaded = parser::load_program(input, result.sources);
    if (!result.loaded.ok()) {
        std::cerr << ast::render_all(result.loaded.diagnostics, result.sources);
        return result;
    }

    std::vector<typeck::ModuleInput> modules;
    for (const parser::Module& module : result.loaded.modules) {
        modules.push_back(
            typeck::ModuleInput{module.name, module.program.get(), module.imports});
    }

    result.checked = typeck::check(modules, result.sources);
    if (!result.checked.ok()) {
        std::cerr << ast::render_all(result.checked.diagnostics, result.sources);
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

/// Link the program's object files against the Ember runtime to produce
/// `output`. Returns an exit code.
int link_executable(const std::vector<std::filesystem::path>& objects,
                    const std::filesystem::path& output) {
    const std::filesystem::path runtime{EMBER_RUNTIME_LIBRARY};
    if (runtime.empty() || !std::filesystem::exists(runtime)) {
        std::cerr << "error: cannot find the Ember runtime library\n";
        std::cerr << "note: expected it at `" << runtime.string() << "`\n";
        return kExitCompileError;
    }

    std::string command = quote(std::filesystem::path{EMBER_LINKER});
    for (const std::filesystem::path& object : objects) {
        command += " " + quote(object);
    }
    command += " " + quote(runtime) + " -o " + quote(output);

    if (run_command(command) != 0) {
        std::cerr << "error: linking failed\n";
        std::cerr << "note: the link step was: " << command << "\n";
        return kExitCompileError;
    }
    return kExitSuccess;
}

/// A 64-bit FNV-1a hash.
///
/// This has to notice that a file changed, not resist someone trying to
/// make it miss, so the cheapest thing that mixes well is the right
/// tool (§9: what would C do).
std::uint64_t hash_into(std::uint64_t seed, std::string_view bytes) {
    std::uint64_t hash = seed;
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 0x00000100000001b3ULL;
    }
    return hash;
}

constexpr std::uint64_t kHashSeed = 0xcbf29ce484222325ULL;

std::string hex(std::uint64_t value, int digits) {
    std::string out(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = "0123456789abcdef"[value & 0xf];
        value >>= 4;
    }
    return out;
}

/// Every module reachable from `start` through imports, including it.
///
/// Reachability rather than a dependency order, because Ember lets two
/// modules import each other: there may be no order to walk, but the
/// reachable set is well defined either way.
std::set<std::size_t> reachable_from(const std::vector<parser::Module>& modules,
                                     const std::map<std::string, std::size_t>& by_name,
                                     std::size_t start) {
    std::set<std::size_t> seen{start};
    std::vector<std::size_t> pending{start};
    while (!pending.empty()) {
        const std::size_t current = pending.back();
        pending.pop_back();
        for (const std::string& import : modules[current].imports) {
            const auto found = by_name.find(import);
            if (found != by_name.end() && seen.insert(found->second).second) {
                pending.push_back(found->second);
            }
        }
    }
    return seen;
}

/// One module's share of the build.
struct ModuleBuild {
    std::string name;
    std::filesystem::path source;
    std::filesystem::path object;
    /// Whether the object was already on disk from an earlier build.
    bool cached = false;

    /// What to call this in progress output: the file, not the module,
    /// because the entry module has no name.
    std::string label() const { return source.filename().string(); }
};

/// Where object files are kept between builds: beside the entry file,
/// the way Cargo puts `target/` beside `Cargo.toml`.
///
/// Falls back to the system temporary directory when the source tree is
/// not writable, so compiling a read-only checkout still works - it just
/// does not get to keep anything.
std::filesystem::path cache_directory(const std::filesystem::path& entry) {
    std::error_code code;
    std::filesystem::path directory = entry.parent_path() / ".ember";
    std::filesystem::create_directories(directory, code);
    if (!code) {
        return directory;
    }
    directory = std::filesystem::temp_directory_path() / "ember-cache";
    std::filesystem::create_directories(directory, code);
    return directory;
}

/// Names each module's object file after everything that decides
/// whether it is still valid.
///
/// The fingerprint covers the module's own source *and* the source of
/// everything it can reach through imports, because a struct that
/// changes shape in one module changes the code generated in another.
/// It also covers the compiler, so upgrading ember invalidates the lot.
///
/// Putting the fingerprint in the file name rather than in a manifest
/// beside it means a cache hit is just a file existing: there is no
/// window in which the name and the contents disagree.
std::vector<ModuleBuild> plan_build(const FrontEnd& front_end,
                                    const std::filesystem::path& cache,
                                    const codegen::CompileOptions& options) {
    const std::vector<parser::Module>& modules = front_end.loaded.modules;

    std::map<std::string, std::size_t> by_name;
    std::vector<std::uint64_t> source_hashes(modules.size(), kHashSeed);
    for (std::size_t i = 0; i < modules.size(); ++i) {
        by_name.emplace(modules[i].name, i);
        source_hashes[i] =
            hash_into(kHashSeed, front_end.sources.file(modules[i].file).contents());
    }

    // Anything that changes the generated code and is not a source file:
    // the compiler that will do the lowering. The optimization level is
    // deliberately *not* here - it goes in the object's name below.
    std::uint64_t base =
        hash_into(hash_into(kHashSeed, version_string()), codegen::llvm_version());

    // A compiler rebuilt from different sources usually still reports
    // the same version, so its size and timestamp stand in for what the
    // version number does not say.
    std::error_code code;
    const std::uintmax_t size = std::filesystem::file_size(compiler_path(), code);
    if (!code) {
        base = hash_into(base, std::to_string(size));
        base = hash_into(
            base, std::to_string(std::filesystem::last_write_time(compiler_path(), code)
                                     .time_since_epoch()
                                     .count()));
    }

    std::vector<ModuleBuild> plan;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        // Sorted, so the fingerprint does not depend on load order.
        std::map<std::string, std::uint64_t> inputs;
        for (const std::size_t reached : reachable_from(modules, by_name, i)) {
            inputs.emplace(modules[reached].path.string(), source_hashes[reached]);
        }

        std::uint64_t fingerprint = base;
        for (const auto& [path, hash] : inputs) {
            fingerprint = hash_into(fingerprint, path);
            fingerprint = hash_into(fingerprint, hex(hash, 16));
        }

        // The stem alone can collide across directories, so the path
        // goes in the name too; the whole thing is the sweep prefix
        // below.
        //
        // The optimization level is part of the prefix rather than the
        // fingerprint so that each level keeps its own objects and
        // sweeps only its own: flipping between `-O0` while working and
        // `-O2` to check something does not recompile the program each
        // way round.
        const std::string prefix = modules[i].path.stem().string() + "-" +
                                   hex(hash_into(kHashSeed, modules[i].path.string()), 8) + "-O" +
                                   std::to_string(options.optimization_level);

        ModuleBuild build;
        build.name = modules[i].name;
        build.source = modules[i].path;
        build.object = cache / (prefix + "-" + hex(fingerprint, 16) + ".o");
        build.cached = std::filesystem::exists(build.object);
        plan.push_back(std::move(build));
    }
    return plan;
}

/// Removes the objects a module left behind under older fingerprints,
/// so the cache stays proportional to the source tree rather than to the
/// number of times it has been edited.
void sweep_stale_objects(const std::filesystem::path& cache,
                         const std::vector<ModuleBuild>& plan) {
    std::error_code code;
    for (const ModuleBuild& build : plan) {
        // Everything up to the fingerprint identifies the source file:
        // the name ends in `-` plus 16 hex digits plus `.o`.
        const std::string name = build.object.filename().string();
        const std::string prefix = name.substr(0, name.size() - std::size_t{19});

        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(cache, code)) {
            const std::string candidate = entry.path().filename().string();
            if (candidate != name && candidate.rfind(prefix, 0) == 0 &&
                entry.path().extension() == ".o") {
                std::filesystem::remove(entry.path(), code);
            }
        }
    }
}

/// Compile a checked program all the way to a native executable.
int emit_executable(const FrontEnd& front_end, const std::filesystem::path& output,
                    const BuildOptions& build) {
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
        std::cerr << ast::render_all(entry, front_end.sources);
        return kExitCompileError;
    }

    codegen::CompileOptions options;
    options.output = codegen::OutputKind::Object;
    options.optimization_level = build.optimization_level;

    const std::filesystem::path cache =
        cache_directory(front_end.loaded.modules.front().path);
    const std::vector<ModuleBuild> plan = plan_build(front_end, cache, options);
    const std::vector<codegen::ModuleInput> inputs = front_end.codegen_modules();

    std::vector<std::filesystem::path> objects;
    for (const ModuleBuild& module : plan) {
        objects.push_back(module.object);

        if (module.cached && !build.fresh) {
            if (build.verbose) {
                std::cerr << "  cached  " << module.label() << "\n";
            }
            continue;
        }
        if (build.verbose) {
            std::cerr << "compiling " << module.label() << "\n";
        }

        options.module_name = module.source.string();
        options.target_module = module.name;

        // Written under a private name and moved into place, so a second
        // ember running over the same sources cannot be caught reading a
        // half-written object.
        std::random_device entropy;
        const std::filesystem::path partial =
            module.object.string() + ".tmp" + std::to_string(entropy());
        const codegen::CompileResult compiled =
            codegen::compile(inputs, front_end.checked, partial, options);
        if (!compiled.ok()) {
            std::error_code ignored;
            std::filesystem::remove(partial, ignored);
            std::cerr << ast::render_all(compiled.diagnostics, front_end.sources);
            return kExitCompileError;
        }

        std::error_code code;
        std::filesystem::rename(partial, module.object, code);
        if (code) {
            // Losing the race is fine: whoever won wrote the same bytes,
            // because the name is a hash of everything that went in.
            std::filesystem::remove(partial, code);
            if (!std::filesystem::exists(module.object)) {
                std::cerr << "error: cannot write `" << module.object.string() << "`\n";
                return kExitCompileError;
            }
        }
    }

    sweep_stale_objects(cache, plan);

    if (build.verbose) {
        std::cerr << " linking  " << output.filename().string() << "\n";
    }
    return link_executable(objects, output);
}

}  // namespace

std::string linker_command() { return EMBER_LINKER; }

int check_file(const std::filesystem::path& input) {
    return run_front_end(input).ok ? kExitSuccess : kExitCompileError;
}

int build_file(const std::filesystem::path& input, const std::filesystem::path& output,
               const BuildOptions& options) {
    const FrontEnd front_end = run_front_end(input);
    if (!front_end.ok) {
        return kExitCompileError;
    }
    return emit_executable(front_end, output, options);
}

int run_file(const std::filesystem::path& input, const BuildOptions& options) {
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
    if (emit_executable(front_end, scratch.path(), options) != kExitSuccess) {
        return kExitCompileError;
    }

    // The program's own exit code is this process's exit code, so `ember
    // run` is transparent to whatever it launched.
    return run_command(quote(scratch.path()));
}

}  // namespace ember::cli
