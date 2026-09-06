#include "soliton/link/link.hpp"

#include <system_error>

#if SOLITON_HAVE_LLD
#include "lld/Common/Driver.h"

#include "llvm/Support/raw_ostream.h"

// Only the MinGW driver is linked in. It accepts GNU-style arguments -
// `-l`, `-L`, startup objects in order - and drives the COFF backend,
// which is what the archives shipped beside the compiler expect.
LLD_HAS_DRIVER(mingw)
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace soliton::link {
namespace {

namespace fs = std::filesystem;

/// The startup objects, in the order the link line wants them.
///
/// These are not all in one place in a build tree: crt2.o and
/// default-manifest.o come from the mingw prefix, crtbegin.o and
/// crtend.o from gcc's own directory. So each is searched for
/// separately rather than assuming one directory holds them all - an
/// assumption that happens to hold in an install and not in a build.
constexpr const char* kStartObjects[] = {"crt2.o", "crtbegin.o"};
constexpr const char* kEndObjects[] = {"default-manifest.o", "crtend.o"};

/// The system archives, in the order `g++ -static` uses.
///
/// Two things here are copied rather than reasoned out, because both
/// are load-bearing and both fail in ways that are miserable to debug.
///
/// The tail repeats: libgcc and libmsvcrt refer to each other, and a
/// GNU-style link resolves an archive only against what is undefined
/// when it reaches it, so one pass is not enough. The failure mode is an
/// undefined symbol from deep inside the C runtime.
///
/// And it is `-lgcc -lgcc_eh`, not the `-lgcc_s -lgcc` that a default
/// `g++ -v` shows. On MinGW `libgcc_s.a` is the *import library* for
/// libgcc_s_seh-1.dll, so asking for it links against a DLL the target
/// machine will not have. libgcc_eh.a is the static unwinder that
/// replaces it. The failure mode is a program that builds cleanly and
/// then will not start.
constexpr const char* kSystemLibraries[] = {
    "-lstdc++",   "-lmingw32", "-lgcc",      "-lgcc_eh",  "-lmingwex", "-lmsvcrt",
    "-lkernel32", "-lpthread", "-ladvapi32", "-lshell32", "-luser32",  "-lkernel32",
    // Second pass, for the cycles above.
    "-lmingw32",  "-lgcc",     "-lgcc_eh",   "-lmingwex", "-lmsvcrt",  "-lkernel32",
};

/// Split a `|`-separated list of paths baked in at build time.
///
/// `|` rather than `;` or `:`: a Windows path contains `:` and CMake
/// lists use `;`, so both would need escaping to survive the trip
/// through a compile definition.
std::vector<fs::path> split_paths(std::string_view packed) {
    std::vector<fs::path> paths;
    while (!packed.empty()) {
        const std::size_t separator = packed.find('|');
        const std::string_view piece = packed.substr(0, separator);
        if (!piece.empty()) {
            paths.emplace_back(piece);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        packed.remove_prefix(separator + 1);
    }
    return paths;
}

/// The first of `paths` holding `name`, or an empty path.
fs::path resolve(const char* name, const std::vector<fs::path>& paths) {
    std::error_code failed;
    for (const fs::path& directory : paths) {
        const fs::path candidate = directory / name;
        if (fs::exists(candidate, failed)) {
            return candidate;
        }
    }
    return {};
}

/// The first startup object that cannot be found, or nullptr.
const char* missing_object(const std::vector<fs::path>& paths) {
    for (const char* object : kStartObjects) {
        if (resolve(object, paths).empty()) {
            return object;
        }
    }
    for (const char* object : kEndObjects) {
        if (resolve(object, paths).empty()) {
            return object;
        }
    }
    return nullptr;
}

}  // namespace

bool is_available() {
#if SOLITON_HAVE_LLD
    return true;
#else
    return false;
#endif
}

fs::path executable_path() {
#ifdef _WIN32
    // MAX_PATH is not the limit any more, so grow until it fits rather
    // than truncating silently - which is what GetModuleFileNameW does
    // when the buffer is too small.
    std::wstring buffer(512, L'\0');
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            return fs::path{buffer};
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code failed;
    const fs::path self = fs::read_symlink("/proc/self/exe", failed);
    return failed ? fs::path{} : self;
#endif
}

Toolchain discover() {
    Toolchain toolchain;
    std::error_code failed;

    // An installed tree first: `<prefix>/bin/soliton` beside
    // `<prefix>/lib/soliton`. This is the case that has to work on a
    // machine that has never seen a build.
    const fs::path self = executable_path();
    if (!self.empty()) {
        const fs::path installed = self.parent_path().parent_path() / "lib" / "soliton";
        const std::vector<fs::path> candidate{installed};
        if (fs::exists(installed / "libsoliton_std.a", failed) &&
            missing_object(candidate) == nullptr) {
            toolchain.runtime = installed / "libsoliton_std.a";
            toolchain.library_paths = candidate;
            toolchain.complete = true;
            return toolchain;
        }
    }

    // Otherwise the build tree, so the compiler works out of `build/`
    // without being installed first.
#ifdef SOLITON_RUNTIME_LIBRARY
    toolchain.runtime = fs::path{SOLITON_RUNTIME_LIBRARY};
#endif
#ifdef SOLITON_BUILD_LIBRARY_PATHS
    toolchain.library_paths = split_paths(SOLITON_BUILD_LIBRARY_PATHS);
#endif

    if (toolchain.runtime.empty() || !fs::exists(toolchain.runtime, failed)) {
        toolchain.note = "the Soliton runtime archive is missing (looked for `" +
                         toolchain.runtime.string() + "`)";
        return toolchain;
    }
    if (const char* absent = missing_object(toolchain.library_paths)) {
        toolchain.note = std::string{"`"} + absent + "` was not on the library path";
        return toolchain;
    }
    toolchain.complete = true;
    return toolchain;
}

Result link_executable(const std::vector<fs::path>& objects, const fs::path& output,
                       const Toolchain& toolchain) {
#if !SOLITON_HAVE_LLD
    (void)objects;
    (void)output;
    (void)toolchain;
    return Result{false, "this build of Soliton has no linker compiled into it"};
#else
    if (const char* absent = missing_object(toolchain.library_paths)) {
        return Result{false, std::string{"`"} + absent + "` was not on the library path"};
    }

    std::vector<std::string> arguments{
        "ld.lld",
        // i386pep is the 64-bit PE target, named the way GNU ld names
        // it - which is what the MinGW driver expects.
        "-m",
        "i386pep",
        // -Bstatic, not the -Bdynamic g++ passes. A GNU-style `-l`
        // prefers the import library when both are present, so
        // `-lstdc++` against a full mingw prefix picks
        // libstdc++.dll.a and the program that comes out needs
        // libstdc++-6.dll to start - which is the dependency on
        // somebody else's toolchain that this whole exercise is about
        // removing. Soliton's programs link their runtime in.
        "-Bstatic",
        "-o",
        output.string(),
    };
    for (const char* object : kStartObjects) {
        arguments.push_back(resolve(object, toolchain.library_paths).string());
    }
    for (const fs::path& directory : toolchain.library_paths) {
        arguments.push_back("-L" + directory.string());
    }
    for (const fs::path& object : objects) {
        arguments.push_back(object.string());
    }
    arguments.push_back(toolchain.runtime.string());
    for (const char* library : kSystemLibraries) {
        arguments.push_back(library);
    }
    for (const char* object : kEndObjects) {
        arguments.push_back(resolve(object, toolchain.library_paths).string());
    }

    std::vector<const char*> argv;
    argv.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        argv.push_back(argument.c_str());
    }

    std::string captured;
    llvm::raw_string_ostream stream{captured};
    const lld::DriverDef drivers[] = {{lld::MinGW, &lld::mingw::link}};

    // lldMain rather than calling the driver directly: it is the
    // re-entrant entry point, which matters because `soliton build` links
    // once per invocation and the process outlives the link.
    const lld::Result linked = lld::lldMain(argv, stream, stream, drivers);
    stream.flush();

    Result result;
    result.ok = linked.retCode == 0;
    if (!result.ok) {
        result.diagnostics = captured;
    }
    return result;
#endif
}

}  // namespace soliton::link
