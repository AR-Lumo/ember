/// ember::link - turning object files into an executable, in process.
///
/// The driver used to shell out to whichever C++ compiler built it, at
/// the absolute path CMake baked in. That works exactly once, on the
/// machine that did the build: move `ember` anywhere else and linking
/// fails because `C:/msys64/ucrt64/bin/g++.exe` is not there. A
/// compiler that cannot produce a program on a machine it was not built
/// on is not a toolchain anyone can install.
///
/// So LLD is linked into the driver and called as a library, and the
/// startup objects and system archives it needs are shipped beside the
/// binary. Nothing outside the Ember install is consulted.
#ifndef EMBER_LINK_LINK_HPP
#define EMBER_LINK_LINK_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace ember::link {

/// Whether this build has a linker compiled into it.
///
/// False when LLD was not found at configure time; the driver then
/// falls back to an external linker, as it always did.
bool is_available();

/// The absolute path of the running executable.
///
/// Everything else is found relative to this rather than to the working
/// directory, so `ember` works when invoked through a symlink, through
/// PATH, or from anywhere at all.
std::filesystem::path executable_path();

/// The files the linker needs, and where they were found.
struct Toolchain {
    /// The Ember runtime archive - `println`, allocation, panics.
    std::filesystem::path runtime;
    /// Directories to search for `-l` archives and startup objects.
    std::vector<std::filesystem::path> library_paths;
    /// True when everything needed was located.
    bool complete = false;
    /// What was looked for and not found, for the error message.
    std::string note;
};

/// Locate the runtime and system archives.
///
/// An installed layout wins: `<prefix>/lib/ember` beside
/// `<prefix>/bin/ember`. Failing that, the build tree's own paths, so
/// the compiler is usable straight out of `build/` without installing.
Toolchain discover();

struct Result {
    bool ok = false;
    /// Whatever the linker said. Empty on success.
    std::string diagnostics;
};

/// Link `objects` plus the Ember runtime into an executable.
Result link_executable(const std::vector<std::filesystem::path>& objects,
                       const std::filesystem::path& output,
                       const Toolchain& toolchain);

}  // namespace ember::link

#endif  // EMBER_LINK_LINK_HPP
