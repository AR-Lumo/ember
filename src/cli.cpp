#include "cli.hpp"

#include "ember/ast/ast.hpp"
#include "ember/ast/diagnostic.hpp"
#include "ember/ast/span.hpp"
#include "ember/codegen/codegen.hpp"
#include "ember/manifest/manifest.hpp"
#include "ember/manifest/registry.hpp"
#include "ember/parser/parser.hpp"
#include "ember/typeck/typeck.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

/// `--module-path <dir>`, spelled `-L` as well because that is what
/// every other toolchain calls it. Consumes two arguments when it
/// matches, so it reports how many it took.
///
/// Recognized by `check` too: where a module lives is a question the
/// front end asks, and `ember check` runs the front end.
std::optional<std::variant<std::size_t, UsageError>> parse_module_path_flag(
    std::span<const std::string_view> args, std::size_t at,
    std::vector<std::filesystem::path>& into) {
    const std::string_view arg = args[at];
    if (arg != "-L" && arg != "--module-path") {
        return std::nullopt;
    }
    if (at + 1 >= args.size()) {
        return usage_error("`" + std::string{arg} + "` requires a directory argument");
    }
    into.emplace_back(std::string{args[at + 1]});
    return std::size_t{2};
}

/// `--update`: ignore the revisions `ember.lock` pinned and resolve git
/// dependencies afresh. Like `--module-path` it belongs to every
/// subcommand that reads a program, `check` included.
bool parse_update_flag(std::string_view arg, bool& update) {
    if (arg != "--update") {
        return false;
    }
    update = true;
    return true;
}

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
    std::vector<std::filesystem::path> module_path;
    bool update = false;

    for (std::size_t i = 0; i < args.size();) {
        const std::string_view arg = args[i];

        if (const auto taken = parse_module_path_flag(args, i, module_path)) {
            if (const auto* error = std::get_if<UsageError>(&*taken)) {
                return *error;
            }
            i += std::get<std::size_t>(*taken);
            continue;
        }
        if (parse_update_flag(arg, update)) {
            i += 1;
            continue;
        }
        if (kind == CommandKind::Run) {
            const FlagOutcome outcome = parse_build_flag(arg, options);
            if (outcome.error.has_value()) {
                return usage_error(*outcome.error);
            }
            if (outcome.recognized) {
                i += 1;
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
        i += 1;
    }

    if (!input.has_value()) {
        return usage_error("`" + std::string{subcommand} + "` requires an input file");
    }

    Command command;
    command.kind = kind;
    command.input = *input;
    command.build = options;
    command.module_path = std::move(module_path);
    command.update = update;
    return command;
}

ParseResult parse_build(std::span<const std::string_view> args) {
    std::optional<std::filesystem::path> input;
    std::optional<std::filesystem::path> output;
    BuildOptions options;
    std::vector<std::filesystem::path> module_path;
    bool update = false;

    for (std::size_t i = 0; i < args.size();) {
        const std::string_view arg = args[i];

        if (const auto taken = parse_module_path_flag(args, i, module_path)) {
            if (const auto* error = std::get_if<UsageError>(&*taken)) {
                return *error;
            }
            i += std::get<std::size_t>(*taken);
            continue;
        }
        if (parse_update_flag(arg, update)) {
            i += 1;
            continue;
        }

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
    command.module_path = std::move(module_path);
    command.update = update;
    return command;
}

/// `ember fetch [--update]`. The odd one out: it takes no input file,
/// because what it works on is the manifest, found by walking up from
/// wherever it was run.
ParseResult parse_fetch(std::span<const std::string_view> args) {
    Command command;
    command.kind = CommandKind::Fetch;

    for (const std::string_view arg : args) {
        if (parse_update_flag(arg, command.update)) {
            continue;
        }
        if (is_flag(arg)) {
            return usage_error("unknown option `" + std::string{arg} + "`");
        }
        return usage_error("`fetch` takes no input file, only the manifest it finds");
    }
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
    "    ember fetch                           resolve and download dependencies\n"
    "\n"
    "OPTIONS:\n"
    "    -o, --output <path>   output path for `build` (default: input stem)\n"
    "    -L, --module-path <dir>\n"
    "                          also look here for imported modules (repeatable)\n"
    "        --update          re-resolve git dependencies, ignoring `ember.lock`\n"
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
        return Command{CommandKind::Help, {}, std::nullopt, {}, {}};
    }
    if (first == "-V" || first == "--version" || first == "version") {
        return Command{CommandKind::Version, {}, std::nullopt, {}, {}};
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
    if (first == "fetch") {
        return parse_fetch(rest);
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

/// The separator `EMBER_MODULE_PATH` uses, which is whatever the
/// platform already uses for `PATH`.
constexpr char kPathSeparator =
#ifdef _WIN32
    ';';
#else
    ':';
#endif

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
FrontEnd run_front_end(const std::filesystem::path& input,
                       const std::vector<std::filesystem::path>& module_path, bool update,
                       bool verbose) {
    FrontEnd result;

    // Dependencies first: a package that will not resolve is not
    // something to discover halfway through checking a program that
    // imports it.
    const PackageResolution packages = resolve_packages(input, update, verbose);
    if (!packages.ok) {
        return result;
    }

    // Loading pulls in every module the entry file imports, transitively.
    result.loaded = parser::load_program(
        input, result.sources, module_search_path(input, module_path, packages.search_path));
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

/// Runs a command and returns its standard output, with the exit code.
///
/// `run_command` above only reports whether something worked; asking git
/// which commit it checked out needs the answer as well.
struct CapturedCommand {
    int exit_code = 0;
    std::string output;
};

CapturedCommand capture_command(const std::string& command) {
    const std::string redirected = command + " 2>&1";
#ifdef _WIN32
    const std::string wrapped = "\"" + redirected + "\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    FILE* pipe = popen(redirected.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return CapturedCommand{-1, "cannot start `" + command + "`"};
    }

    CapturedCommand result;
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

/// Trims whitespace, since every git command ends its answer with a
/// newline.
std::string trimmed(std::string text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
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

// -------------------------------------------------------------------
// Packages
// -------------------------------------------------------------------

/// Clones a git dependency into the build cache and checks out what was
/// asked for.
///
/// Shelling out to `git` rather than linking a library: the user has one
/// already, it is the only thing that understands every URL a repository
/// might live behind, and vendoring an implementation of git to avoid
/// starting one process would be a poor trade (§9: what would C do).
class GitFetcher {
public:
    GitFetcher(std::filesystem::path cache, bool verbose)
        : cache_(std::move(cache)), verbose_(verbose) {}

    manifest::Fetched operator()(const manifest::Dependency& dependency,
                                 const std::string& rev) {
        // Resolution walks the graph more than once while it settles on
        // versions, and asks for the same commit each time. Cloning and
        // checking out are cheap the second time; a `git fetch` over the
        // network is not.
        const std::string key = dependency.location + "@" + rev;
        const auto done = already_.find(key);
        if (done != already_.end()) {
            return done->second;
        }
        return already_.emplace(key, materialize(dependency, rev)).first->second;
    }

private:
    manifest::Fetched materialize(const manifest::Dependency& dependency,
                                  const std::string& rev) {
        std::error_code code;
        std::filesystem::create_directories(cache_, code);

        const std::filesystem::path checkout =
            cache_ / (dependency.name + "-" +
                      hex(hash_into(kHashSeed, dependency.location), 16));

        manifest::Fetched fetched;
        if (!std::filesystem::is_directory(checkout / ".git", code)) {
            std::cerr << "fetching " << dependency.name << " (" << dependency.location
                      << ")\n";
            std::filesystem::remove_all(checkout, code);
            const CapturedCommand cloned = capture_command(
                "git clone --quiet " + quote(std::filesystem::path{dependency.location}) +
                " " + quote(checkout));
            if (cloned.exit_code != 0) {
                fetched.error = git_failure("clone", cloned);
                return fetched;
            }
        }

        // Only reach the network when the answer could have changed.
        //
        // A full commit hash cannot move, so having it locally is proof
        // enough - which is the case after the first build, because the
        // lockfile pins hashes. A tag or a branch can point somewhere
        // new at any time, and having *a* `v1.0.0` locally says nothing
        // about whether it is still the `v1.0.0` upstream has.
        const std::string at = " -C " + quote(checkout) + " ";
        const bool have_it =
            is_commit_hash(rev) && capture_command("git" + at + "rev-parse --verify --quiet " +
                                                   quote_arg(rev + "^{commit}"))
                                           .exit_code == 0;
        if (!have_it) {
            if (verbose_) {
                std::cerr << "updating " << dependency.name << " (" << rev
                          << " is not a pinned commit we already have)\n";
            }
            // `--force` because a moved tag is exactly what is being
            // looked for here; without it git refuses to update one.
            const CapturedCommand updated =
                capture_command("git" + at + "fetch --quiet --force --tags origin");
            if (updated.exit_code != 0) {
                fetched.error = git_failure("fetch", updated);
                return fetched;
            }
        }

        const CapturedCommand checked_out =
            capture_command("git" + at + "checkout --quiet --detach " + quote_arg(rev));
        if (checked_out.exit_code != 0) {
            fetched.error = git_failure("checkout " + rev, checked_out);
            return fetched;
        }

        const CapturedCommand head = capture_command("git" + at + "rev-parse HEAD");
        if (head.exit_code != 0) {
            fetched.error = git_failure("rev-parse", head);
            return fetched;
        }

        fetched.root = checkout;
        fetched.rev = trimmed(head.output);
        return fetched;
    }

    std::filesystem::path cache_;
    bool verbose_ = false;
    /// What has already been fetched this run, by repository and
    /// revision.
    std::map<std::string, manifest::Fetched> already_;

    /// A revision or URL goes to a shell, so it is quoted like a path.
    /// The whole revspec has to be inside the quotes: `^` is cmd.exe's
    /// escape character, and `v1.0.0"^{commit}` loses the brace.
    static std::string quote_arg(const std::string& text) { return "\"" + text + "\""; }

    /// Whether `rev` is a full commit hash, and so cannot ever name
    /// different code than it did last time.
    static bool is_commit_hash(const std::string& rev) {
        if (rev.size() != 40) {
            return false;
        }
        for (const char c : rev) {
            if (std::isxdigit(static_cast<unsigned char>(c)) == 0) {
                return false;
            }
        }
        return true;
    }

    static std::string git_failure(const std::string& what, const CapturedCommand& result) {
        const std::string said = trimmed(result.output);
        return "`git " + what + "` failed" + (said.empty() ? "" : ": " + said);
    }
};

/// Looks packages up in the registry index the manifest configured.
///
/// The index is a directory of one file per package. It may be a
/// directory on this machine, or a git repository - which is how
/// crates.io's index works, and means publishing one needs a git host
/// rather than a server.
///
/// A cloned index is refreshed only when asked, not on every build. A
/// stale index cannot make a build wrong: the lockfile pins what was
/// chosen, so the index only matters when something is being chosen for
/// the first time or `--update` says to choose again.
class RegistryIndex {
public:
    RegistryIndex(std::string configured, std::filesystem::path cache, GitFetcher& fetcher,
                  bool update, ast::SourceMap& sources)
        : configured_(std::move(configured)),
          cache_(std::move(cache)),
          fetcher_(fetcher),
          update_(update),
          sources_(sources) {}

    manifest::IndexLookup operator()(const std::string& name) {
        manifest::IndexLookup lookup;

        const std::optional<std::filesystem::path> directory = locate();
        if (!directory.has_value()) {
            lookup.error = error_;
            return lookup;
        }

        const std::filesystem::path file =
            *directory / (name + std::string{manifest::kIndexExtension});
        const std::optional<ast::SourceFile> source = ast::SourceFile::load(file);
        if (!source.has_value()) {
            return lookup;  // no entry is not an error; the caller says so better
        }

        const ast::FileId id = sources_.add(file.string(), source->contents());
        manifest::IndexResult parsed =
            manifest::parse_index_entry(sources_.file(id), name);
        if (!parsed.diagnostics.empty()) {
            std::cerr << ast::render_all(parsed.diagnostics, sources_);
            lookup.error = "the index entry for `" + name + "` could not be read";
            return lookup;
        }
        lookup.entry = std::move(parsed.entry);
        return lookup;
    }

private:
    std::string configured_;
    std::filesystem::path cache_;
    GitFetcher& fetcher_;
    bool update_ = false;
    ast::SourceMap& sources_;

    std::optional<std::filesystem::path> resolved_;
    std::string error_;
    bool tried_ = false;

    /// Materializes the index once, whatever kind it is.
    std::optional<std::filesystem::path> locate() {
        if (tried_) {
            return resolved_;
        }
        tried_ = true;

        if (configured_.empty()) {
            error_ = "no registry is configured";
            return std::nullopt;
        }

        // A directory that exists is used as it is; anything else is a
        // repository to clone. That is the whole rule.
        std::error_code code;
        const std::filesystem::path as_directory{configured_};
        if (std::filesystem::is_directory(as_directory, code)) {
            resolved_ = as_directory;
            return resolved_;
        }

        const std::filesystem::path checkout =
            cache_ / ("index-" + hex(hash_into(kHashSeed, configured_), 16));
        if (std::filesystem::is_directory(checkout / ".git", code) && !update_) {
            resolved_ = checkout;
            return resolved_;
        }

        manifest::Dependency index;
        index.name = "index";
        index.kind = manifest::SourceKind::Git;
        index.location = configured_;
        index.rev = "HEAD";

        std::cerr << "fetching the registry index (" << configured_ << ")\n";
        const manifest::Fetched fetched = fetcher_(index, "HEAD");
        if (!fetched.ok()) {
            error_ = "cannot read the registry index: " + fetched.error;
            return std::nullopt;
        }
        resolved_ = fetched.root;
        return resolved_;
    }
};

/// Reads `ember.lock` beside the manifest, if there is one.
manifest::Lock read_lock(const std::filesystem::path& directory, ast::SourceMap& sources,
                         bool& ok) {
    const std::filesystem::path path = directory / std::filesystem::path{manifest::kLockName};
    const std::optional<ast::SourceFile> file = ast::SourceFile::load(path);
    if (!file.has_value()) {
        return {};
    }

    const ast::FileId id = sources.add(path.string(), file->contents());
    manifest::LockResult parsed = manifest::parse_lock(sources.file(id));
    if (!parsed.ok()) {
        // Quietly ignoring a broken lock would turn a repeatable build
        // into an unrepeatable one without saying so.
        std::cerr << ast::render_all(parsed.diagnostics, sources);
        ok = false;
    }
    return std::move(parsed.lock);
}

/// Writes the lock, but only when it would say something new - so a
/// build of an unchanged project does not keep touching a checked-in
/// file.
void write_lock_if_changed(const std::filesystem::path& directory,
                           const std::vector<manifest::ResolvedPackage>& packages) {
    const std::filesystem::path path = directory / std::filesystem::path{manifest::kLockName};
    const std::string contents = manifest::write_lock(packages);

    if (const std::optional<ast::SourceFile> existing = ast::SourceFile::load(path)) {
        if (existing->contents() == contents) {
            return;
        }
    }
    std::ofstream out(path, std::ios::binary);
    out << contents;
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

std::vector<std::filesystem::path> module_search_path(
    const std::filesystem::path& entry, const std::vector<std::filesystem::path>& requested,
    const std::vector<std::filesystem::path>& packages) {
    std::vector<std::filesystem::path> search = requested;

    // A dependency the manifest declared outranks the environment: it is
    // part of the project, and the environment is whatever the shell
    // happened to be carrying.
    search.insert(search.end(), packages.begin(), packages.end());

    if (const char* environment = std::getenv("EMBER_MODULE_PATH")) {
        const std::string text{environment};
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t end = text.find(kPathSeparator, start);
            const std::string piece =
                text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!piece.empty()) {
                search.emplace_back(piece);
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    // The conventional place, so a vendored dependency needs no flag at
    // all: drop it in and `import` finds it.
    std::error_code ignored;
    const std::filesystem::path vendored = entry.parent_path() / "ember_modules";
    if (std::filesystem::is_directory(vendored, ignored)) {
        search.push_back(vendored);
    }
    return search;
}

PackageResolution resolve_packages(const std::filesystem::path& entry, bool update,
                                   bool verbose) {
    PackageResolution result;

    // Most Ember programs are one file and depend on nothing, so having
    // no manifest is the ordinary case rather than a mistake.
    const std::optional<std::filesystem::path> manifest_path =
        manifest::find_manifest(entry.parent_path());
    if (!manifest_path.has_value()) {
        return result;
    }

    ast::SourceMap sources;
    const std::optional<ast::SourceFile> file = ast::SourceFile::load(*manifest_path);
    if (!file.has_value()) {
        std::cerr << "error: cannot read `" << manifest_path->string() << "`\n";
        result.ok = false;
        return result;
    }

    const ast::FileId id = sources.add(manifest_path->string(), file->contents());
    manifest::ManifestResult parsed = manifest::parse_manifest(sources.file(id));
    if (!parsed.diagnostics.empty()) {
        std::cerr << ast::render_all(parsed.diagnostics, sources);
    }
    if (!parsed.manifest.has_value()) {
        result.ok = false;
        return result;
    }
    if (!parsed.diagnostics.empty()) {
        result.ok = false;
        return result;
    }

    const std::filesystem::path root = parsed.manifest->root;

    manifest::Lock lock;
    if (!update) {
        bool lock_ok = true;
        lock = read_lock(root, sources, lock_ok);
        if (!lock_ok) {
            result.ok = false;
            return result;
        }
    }

    const std::filesystem::path cache = root / ".ember" / "packages";
    GitFetcher fetcher{cache, verbose};
    RegistryIndex index{parsed.manifest->registry_index, cache, fetcher, update, sources};

    manifest::Resolution resolution = manifest::resolve(*parsed.manifest, lock,
                                                        std::ref(fetcher), std::ref(index),
                                                        sources);
    if (!resolution.ok()) {
        std::cerr << ast::render_all(resolution.diagnostics, sources);
        result.ok = false;
        return result;
    }

    if (verbose) {
        for (const manifest::ResolvedPackage& package : resolution.packages) {
            std::cerr << "  package " << package.name
                      << (package.version.empty() ? "" : " " + package.version) << " ("
                      << manifest::source_kind_name(package.kind) << ")\n";
        }
    }

    write_lock_if_changed(root, resolution.packages);
    result.search_path = resolution.search_path();
    return result;
}

int fetch_packages(const std::filesystem::path& from, bool update) {
    const std::optional<std::filesystem::path> manifest_path = manifest::find_manifest(from);
    if (!manifest_path.has_value()) {
        std::cerr << "error: no `" << manifest::kManifestName << "` here or above\n";
        std::cerr << "note: `fetch` works on a package; create a manifest to make this one\n";
        return kExitCompileError;
    }

    // `fetch` exists to say what it did, so it reports as it goes.
    const PackageResolution resolved =
        resolve_packages(*manifest_path, update, /*verbose=*/true);
    if (!resolved.ok) {
        return kExitCompileError;
    }

    std::cerr << (resolved.search_path.empty()
                      ? "nothing to fetch\n"
                      : std::to_string(resolved.search_path.size()) + " package" +
                            (resolved.search_path.size() == 1 ? "" : "s") + " ready\n");
    return kExitSuccess;
}

int check_file(const std::filesystem::path& input,
               const std::vector<std::filesystem::path>& module_path, bool update) {
    return run_front_end(input, module_path, update, /*verbose=*/false).ok
               ? kExitSuccess
               : kExitCompileError;
}

int build_file(const std::filesystem::path& input, const std::filesystem::path& output,
               const BuildOptions& options,
               const std::vector<std::filesystem::path>& module_path, bool update) {
    const FrontEnd front_end = run_front_end(input, module_path, update, options.verbose);
    if (!front_end.ok) {
        return kExitCompileError;
    }
    return emit_executable(front_end, output, options);
}

int run_file(const std::filesystem::path& input, const BuildOptions& options,
             const std::vector<std::filesystem::path>& module_path, bool update) {
    const FrontEnd front_end = run_front_end(input, module_path, update, options.verbose);
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
