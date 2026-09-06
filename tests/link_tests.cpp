// A compiler that carries its own linker.
//
// Ember used to link by shelling out to whichever C++ compiler built it,
// at the absolute path CMake recorded. That works on exactly one
// machine. Everywhere else - a colleague's laptop, a CI image, a user
// who downloaded a release - `C:/msys64/ucrt64/bin/g++.exe` does not
// exist and nothing can be compiled.
//
// So LLD is linked into the driver, and the startup objects and system
// archives it needs are shipped beside the binary. Two properties have
// to hold, and both are checked here rather than assumed:
//
//   1. The compiler finds its own toolchain, relative to the running
//      executable, without consulting anything outside its install.
//
//   2. The programs it produces import nothing but Windows' own DLLs.
//      This is the one that fails silently: a GNU-style `-l` prefers an
//      import library over an archive, so a single wrong flag gives a
//      program that builds cleanly, runs on the machine that built it,
//      and dies with 0xC0000135 anywhere else. The PE import table is
//      read directly, because that is where the answer actually lives.

#include "test_harness.hpp"

#include "ember/link/link.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct ProcessResult {
    int exit_code = 0;
    std::string output;
};

/// Run a command, capturing stdout and stderr together.
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
        ::ember::test::fail(__FILE__, __LINE__, "cannot start: " + command);
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

/// Every DLL named in a PE image's import directory.
///
/// Walking the file rather than running a tool: the test has to work
/// wherever the suite does, and `objdump` is exactly the sort of
/// external toolchain dependency this change exists to remove.
///
/// Returns an empty vector if the file is not a PE image or has no
/// imports, which the callers treat as a failure rather than a pass.
std::vector<std::string> imported_dlls(const fs::path& image) {
    std::ifstream file{image, std::ios::binary};
    if (!file) {
        return {};
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>{file},
                                  std::istreambuf_iterator<char>{}};

    const auto read32 = [&bytes](std::size_t at) -> std::uint32_t {
        if (at + 4 > bytes.size()) {
            return 0;
        }
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + at, 4);
        return value;
    };
    const auto read16 = [&bytes](std::size_t at) -> std::uint16_t {
        if (at + 2 > bytes.size()) {
            return 0;
        }
        std::uint16_t value = 0;
        std::memcpy(&value, bytes.data() + at, 2);
        return value;
    };

    if (bytes.size() < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z') {
        return {};
    }
    const std::size_t pe = read32(0x3c);
    if (pe + 24 > bytes.size() || read32(pe) != 0x00004550) {  // "PE\0\0"
        return {};
    }

    const std::size_t coff = pe + 4;
    const std::uint16_t sections = read16(coff + 2);
    const std::uint16_t optional_size = read16(coff + 16);
    const std::size_t optional = coff + 20;
    if (optional_size == 0) {
        return {};
    }

    // PE32+ (0x20b) puts the data directories 16 bytes further along
    // than PE32 (0x10b), because several fields widen to 64 bits.
    const std::uint16_t magic = read16(optional);
    const std::size_t directories = optional + (magic == 0x20b ? 112 : 96);
    const std::size_t import_rva = read32(directories + 8);  // entry 1
    if (import_rva == 0) {
        return {};
    }

    // Section headers follow the optional header; each is 40 bytes and
    // carries the mapping from a virtual address back to a file offset.
    struct Section {
        std::uint32_t virtual_address;
        std::uint32_t virtual_size;
        std::uint32_t raw_pointer;
    };
    std::vector<Section> table;
    const std::size_t section_start = optional + optional_size;
    for (std::uint16_t i = 0; i < sections; ++i) {
        const std::size_t at = section_start + std::size_t{i} * 40;
        table.push_back(Section{read32(at + 12), read32(at + 8), read32(at + 20)});
    }

    const auto to_offset = [&table](std::uint32_t rva) -> std::size_t {
        for (const Section& section : table) {
            if (rva >= section.virtual_address &&
                rva < section.virtual_address + section.virtual_size) {
                return section.raw_pointer + (rva - section.virtual_address);
            }
        }
        return 0;
    };

    std::vector<std::string> names;
    // Import descriptors are 20 bytes each; the name RVA is at +12, and
    // an all-zero descriptor ends the list.
    for (std::size_t at = to_offset(static_cast<std::uint32_t>(import_rva));; at += 20) {
        if (at == 0 || at + 20 > bytes.size()) {
            break;
        }
        const std::uint32_t name_rva = read32(at + 12);
        if (name_rva == 0 && read32(at) == 0) {
            break;
        }
        const std::size_t name_at = to_offset(name_rva);
        if (name_at == 0 || name_at >= bytes.size()) {
            break;
        }
        std::string name;
        for (std::size_t i = name_at; i < bytes.size() && bytes[i] != '\0'; ++i) {
            name.push_back(bytes[i]);
        }
        if (name.empty()) {
            break;
        }
        names.push_back(name);
    }
    return names;
}

/// Lowercase, so a comparison does not turn on KERNEL32 vs kernel32.
std::string lowered(std::string text) {
    for (char& letter : text) {
        if (letter >= 'A' && letter <= 'Z') {
            letter = static_cast<char>(letter - 'A' + 'a');
        }
    }
    return text;
}

/// Does Windows itself ship this DLL?
///
/// The UCRT api-sets and the handful of core system libraries are
/// present on every Windows 10 or later machine. Anything else came from
/// somebody's toolchain and has to be shipped or eliminated.
bool ships_with_windows(const std::string& dll) {
    const std::string name = lowered(dll);
    if (name.rfind("api-ms-win-", 0) == 0) {
        return true;
    }
    static const char* kSystem[] = {"kernel32.dll", "advapi32.dll", "shell32.dll", "user32.dll",
                                    "ole32.dll",    "ntdll.dll",    "ucrtbase.dll", "msvcrt.dll",
                                    "oleaut32.dll", "gdi32.dll",    "ws2_32.dll",  "bcrypt.dll"};
    for (const char* known : kSystem) {
        if (name == known) {
            return true;
        }
    }
    return false;
}

}  // namespace

EMBER_TEST(the_linker_is_compiled_in) {
    // If this fails the build found no LLD, and every other guarantee
    // here is void: the driver falls back to an external linker and the
    // compiler only works where a C++ toolchain is installed.
    EMBER_CHECK(ember::link::is_available());
}

EMBER_TEST(the_compiler_knows_where_it_is) {
    const fs::path self = ember::link::executable_path();
    EMBER_CHECK(!self.empty());
    EMBER_CHECK(fs::exists(self));

    // Everything is found relative to this, so it has to be the real
    // binary rather than argv[0] or the working directory.
    EMBER_CHECK(self.is_absolute());
}

EMBER_TEST(the_toolchain_is_complete) {
    if (!ember::link::is_available()) {
        return;
    }
    const ember::link::Toolchain toolchain = ember::link::discover();
    EMBER_CHECK_MSG(toolchain.complete, toolchain.note);
    EMBER_CHECK(fs::exists(toolchain.runtime));
    EMBER_CHECK(!toolchain.library_paths.empty());

    // Both halves of the startup sequence have to be reachable. They do
    // not live in the same directory in a build tree, which is what the
    // first version of this got wrong.
    bool crt2 = false;
    bool crtbegin = false;
    for (const fs::path& directory : toolchain.library_paths) {
        crt2 = crt2 || fs::exists(directory / "crt2.o");
        crtbegin = crtbegin || fs::exists(directory / "crtbegin.o");
    }
    EMBER_CHECK(crt2);
    EMBER_CHECK(crtbegin);
}

EMBER_TEST(the_import_reader_agrees_with_a_known_image) {
    // Calibration. `ember.exe` is a PE image that certainly imports
    // kernel32; if this fails, a failure below means the reader is
    // broken rather than the linker, which is worth being able to tell
    // apart.
    const std::vector<std::string> dlls = imported_dlls(fs::path{EMBER_BINARY});
    EMBER_CHECK(!dlls.empty());

    bool kernel32 = false;
    for (const std::string& dll : dlls) {
        kernel32 = kernel32 || lowered(dll) == "kernel32.dll";
    }
    EMBER_CHECK(kernel32);
}

EMBER_TEST(the_compiler_itself_needs_no_toolchain_dlls) {
    // The compiler has to start on a machine that has never had a
    // toolchain installed. It used to need libstdc++-6.dll,
    // libgcc_s_seh-1.dll, libwinpthread-1.dll, zlib1.dll and
    // libzstd.dll, all from an MSYS2 prefix.
    const std::vector<std::string> dlls = imported_dlls(fs::path{EMBER_BINARY});
    EMBER_CHECK(!dlls.empty());

    for (const std::string& dll : dlls) {
        EMBER_CHECK_MSG(ships_with_windows(dll),
                        "ember.exe imports `" + dll +
                            "`, which Windows does not ship - it would have to be installed "
                            "alongside the compiler, or linked statically");
    }
}

EMBER_TEST(programs_it_produces_need_no_toolchain_dlls) {
    if (!ember::link::is_available()) {
        return;
    }
    // The failure this guards against is quiet: a GNU-style `-l` prefers
    // an import library over an archive, so dropping `-Bstatic` yields a
    // program that builds, runs where it was built, and dies with
    // 0xC0000135 anywhere else. Nothing short of reading the import
    // table notices.
    const fs::path directory =
        fs::temp_directory_path() / ("ember-link-test-" + std::to_string(std::rand()));
    std::error_code failed;
    fs::create_directories(directory, failed);

    const fs::path source = directory / "main.em";
    {
        std::ofstream out{source};
        out << "pub fn main() {\n";
        // A float and a string, so the runtime's allocation and its
        // std::to_chars formatting are both pulled in - those are what
        // reach for libstdc++.
        out << "    println(1.5);\n";
        out << "    let mut text: String = new_string();\n";
        out << "    push_str(text, \"ok\");\n";
        out << "    println(text);\n";
        out << "}\n";
    }

    const fs::path output = directory / "main.exe";
    const ProcessResult built =
        run_process(quoted(fs::path{EMBER_BINARY}) + " build " + quoted(source) + " -o " +
                    quoted(output));
    EMBER_CHECK_MSG(built.exit_code == 0, "build failed:\n" + built.output);
    EMBER_CHECK_MSG(fs::exists(output), "no executable was produced");

    const std::vector<std::string> dlls = imported_dlls(output);
    EMBER_CHECK_MSG(!dlls.empty(), "the produced program has no import table at all");
    for (const std::string& dll : dlls) {
        EMBER_CHECK_MSG(ships_with_windows(dll),
                        "a compiled Ember program imports `" + dll +
                            "`, so it will not start on a machine without that toolchain");
    }

    fs::remove_all(directory, failed);
}
