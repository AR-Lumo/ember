// Interface files: a module's public surface, with the implementations
// taken out.
//
// This is what a program needs in order to *use* a module without having
// the module's source — the last thing standing between Ember and
// shipping a compiled library. An interface plus an object file is a
// library; `ember interface` writes the first and `ember build --lib`
// writes the second.
//
// Two decisions run through everything below.
//
// **A generic keeps its body.** Ember monomorphizes, so a copy of
// `twice<int>` is generated wherever it is first used, and generating it
// needs the body. That is the same bargain C++ strikes by putting
// templates in headers, and it has the same consequence: a generic's
// implementation is part of its interface.
//
// **The interface is cut out of the source, not printed from the tree.**
// Every item knows the span it came from, so a signature is the text up
// to the body and a generic is the text of the whole thing. Nothing is
// re-rendered, so nothing can be rendered wrong.

#include "test_harness.hpp"

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/interface.hpp"
#include "ember/ast/nodes.hpp"
#include "ember/ast/span.hpp"
#include "ember/codegen/codegen.hpp"
#include "ember/parser/parser.hpp"
#include "ember/typeck/typeck.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using ember::ast::SourceFile;

/// Parses `contents` and writes its interface, failing the test if it
/// will not parse.
ember::ast::InterfaceResult interface_of(const std::string& contents) {
    const SourceFile source{"textkit.em", contents};
    const ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    if (!parsed.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "fixture does not parse:\n" +
                                ember::ast::render_all(parsed.diagnostics, source));
    }
    return ember::ast::write_interface(*parsed.program, source);
}

/// The interface of a module that is meant to have one.
std::string written(const std::string& contents) {
    const ember::ast::InterfaceResult result = interface_of(contents);
    if (!result.ok()) {
        const SourceFile source{"textkit.em", contents};
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected an interface, but:\n" +
                                ember::ast::render_all(result.diagnostics, source));
    }
    return result.contents;
}

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

/// Whether `text` is a module in its own right.
bool checks(const std::string& text) {
    const SourceFile source{"textkit.emi", text};
    const ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    if (!parsed.ok()) {
        return false;
    }
    return ember::typeck::check(*parsed.program, source).ok();
}

}  // namespace

// ---------------------------------------------------------------------
// What goes in and what stays out
// ---------------------------------------------------------------------

EMBER_TEST(interface_keeps_public_signatures_and_drops_their_bodies) {
    const std::string text = written(
        "pub fn shout(text: string) -> String {\n"
        "    let mut out: String = new_string();\n"
        "    push_str(out, text);\n"
        "    return out;\n"
        "}\n");
    EMBER_CHECK_MSG(has(text, "pub fn shout(text: string) -> String;"), text);
    EMBER_CHECK_MSG(!has(text, "push_str"), "the body should not be here:\n" + text);
}

EMBER_TEST(interface_leaves_out_what_is_not_public) {
    const std::string text = written("fn hidden() -> int { return 1; }\n"
                                     "pub fn shown() -> int { return 2; }\n");
    EMBER_CHECK_MSG(has(text, "shown"), text);
    EMBER_CHECK_MSG(!has(text, "hidden"), "a private function leaked:\n" + text);
}

EMBER_TEST(interface_keeps_a_generic_body) {
    // Monomorphization happens at the use site, so the body has to
    // travel with the declaration.
    const std::string text = written(
        "pub fn twice<T>(value: T) -> Vec<T> {\n"
        "    let mut out: Vec<T> = new_vec();\n"
        "    push(out, value);\n"
        "    push(out, value);\n"
        "    return out;\n"
        "}\n");
    EMBER_CHECK_MSG(has(text, "pub fn twice<T>(value: T) -> Vec<T> {"), text);
    EMBER_CHECK_MSG(has(text, "push(out, value);"),
                    "a generic without its body cannot be instantiated:\n" + text);
}

EMBER_TEST(interface_keeps_a_struct_whole) {
    // A struct's fields are its layout, and its layout is what a caller
    // has to agree with.
    const std::string text = written("pub struct Point { pub x: int, pub y: int, }\n");
    EMBER_CHECK_MSG(has(text, "pub x: int"), text);
}

EMBER_TEST(interface_keeps_a_constant_and_its_value) {
    // A caller may fold it, so the value is part of the interface.
    const std::string text = written("pub const LIMIT: int = 42;\n");
    EMBER_CHECK_MSG(has(text, "42"), text);
}

EMBER_TEST(interface_rebuilds_an_impl_block_around_its_public_methods) {
    const std::string text = written(
        "pub struct Point { pub x: int, }\n"
        "impl Point {\n"
        "    pub fn get(self) -> int { return self.x; }\n"
        "    fn secret(self) -> int { return self.x * 2; }\n"
        "}\n");
    EMBER_CHECK_MSG(has(text, "impl Point {"), text);
    EMBER_CHECK_MSG(has(text, "pub fn get(self) -> int;"), text);
    EMBER_CHECK_MSG(!has(text, "secret"), "a private method leaked:\n" + text);
}

EMBER_TEST(interface_reports_a_module_with_nothing_public) {
    const ember::ast::InterfaceResult result =
        interface_of("fn hidden() -> int { return 1; }\n");
    EMBER_CHECK(!result.ok());
    EMBER_CHECK_EQ(result.diagnostics.at(0).message,
                   std::string{"this module has no public interface"});
}

// ---------------------------------------------------------------------
// A public signature has to be usable from outside
// ---------------------------------------------------------------------

EMBER_TEST(interface_refuses_a_public_signature_naming_a_private_type) {
    // Legal inside the module, meaningless outside it: a caller cannot
    // name `Secret`, so it could not call the function even holding the
    // declaration.
    const ember::ast::InterfaceResult result = interface_of(
        "struct Secret { pub x: int, }\n"
        "pub fn make() -> Secret { return Secret { x: 1 }; }\n");
    EMBER_CHECK(!result.ok());
    EMBER_CHECK_EQ(result.diagnostics.at(0).message,
                   std::string{"`make` cannot be part of an interface"});
    EMBER_CHECK_EQ(result.diagnostics.at(0).label, std::string{"`Secret` is not `pub`"});
}

EMBER_TEST(interface_looks_inside_compound_types) {
    const ember::ast::InterfaceResult result = interface_of(
        "struct Secret { pub x: int, }\n"
        "pub fn take(items: &Vec<Secret>) -> int { return len(items); }\n");
    EMBER_CHECK_MSG(!result.ok(), "a private type inside a `&Vec<...>` should still count");
}

EMBER_TEST(interface_accepts_the_builtin_types) {
    // `Vec` and `String` parse as ordinary named types, so they have to
    // be recognised or every signature using one looks like a leak.
    EMBER_CHECK(interface_of("pub fn f(a: &Vec<int>, b: String) -> String { return b; }\n").ok());
}

EMBER_TEST(interface_accepts_a_generic_parameter_as_a_type) {
    EMBER_CHECK(interface_of("pub fn id<T>(value: T) -> T { return value; }\n").ok());
}

// ---------------------------------------------------------------------
// The result is a module
// ---------------------------------------------------------------------

EMBER_TEST(interface_output_is_itself_a_valid_module) {
    // The whole approach rests on this: the interface is the author's
    // own Ember, cut at spans, so it parses because it already did.
    const std::string text = written(
        "pub struct Point { pub x: int, pub y: int, }\n"
        "pub const ORIGIN: Point = Point { x: 0, y: 0 };\n"
        "pub fn sum(p: Point) -> int { return p.x + p.y; }\n"
        "pub fn id<T>(value: T) -> T { return value; }\n"
        "impl Point {\n"
        "    pub fn get(self) -> int { return self.x; }\n"
        "}\n");
    EMBER_CHECK_MSG(checks(text), "the interface does not check:\n" + text);
}

EMBER_TEST(interface_declarations_need_no_return_on_every_path) {
    // A signature with no body has no paths to check, and demanding a
    // return from one would make every interface an error.
    EMBER_CHECK(checks("pub fn f() -> int;\n"));
}

// ---------------------------------------------------------------------
// Compiling against one
// ---------------------------------------------------------------------

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

/// A throwaway tree of files, removed when the test ends.
class Workspace {
public:
    Workspace() {
        static const unsigned run = std::random_device{}();
        static int counter = 0;
        root_ = fs::temp_directory_path() /
                ("ember-interface-" + std::to_string(run) + "-" + std::to_string(++counter));
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

    void write(const std::string& relative, const std::string& contents) const {
        const fs::path target = root_ / relative;
        std::error_code ignored;
        fs::create_directories(target.parent_path(), ignored);
        std::ofstream out(target, std::ios::binary);
        out << contents;
    }

    fs::path path(const std::string& relative) const { return root_ / relative; }

    std::string ember(const std::string& arguments) const {
        return run_process("cd " + quoted(root_) + " && " + quoted(fs::path{EMBER_BINARY}) +
                           " " + arguments)
            .output;
    }

private:
    fs::path root_;
};

const char* const kLibrary =
    "fn decorate(text: string, mark: string) -> String {\n"
    "    let mut out: String = new_string();\n"
    "    push_str(out, text);\n"
    "    push_str(out, mark);\n"
    "    return out;\n"
    "}\n"
    "\n"
    "pub fn shout(text: string) -> String { return decorate(text, \"!\"); }\n"
    "\n"
    "pub fn twice<T>(value: T) -> Vec<T> {\n"
    "    let mut out: Vec<T> = new_vec();\n"
    "    push(out, value);\n"
    "    push(out, value);\n"
    "    return out;\n"
    "}\n";

}  // namespace

EMBER_TEST(interface_lets_a_program_build_without_the_librarys_source) {
    if (!ember::codegen::is_available()) {
        return;
    }
    // The point of the whole feature. The library is compiled and
    // described, its source is deleted, and a program is built against
    // what is left.
    const Workspace workspace;
    workspace.write("lib/textkit.em", kLibrary);
    workspace.write("app/main.em",
                    "import textkit;\n"
                    "pub fn main() {\n"
                    "    println(textkit::shout(\"ember\"));\n"
                    "    println(len(textkit::twice(7)));\n"
                    "}\n");

    workspace.ember("interface lib/textkit.em -o dist/textkit.emi");
    workspace.ember("build --lib lib/textkit.em -o dist/textkit.o");
    EMBER_CHECK_MSG(fs::exists(workspace.path("dist/textkit.emi")), "no interface was written");
    EMBER_CHECK_MSG(fs::exists(workspace.path("dist/textkit.o")), "no object was written");

    std::error_code ignored;
    fs::remove_all(workspace.path("lib"), ignored);

    const std::string ran =
        workspace.ember("run app/main.em -L dist --link dist/textkit.o");
    EMBER_CHECK_MSG(ran.find("ember!") != std::string::npos, ran);
    // The generic was monomorphized in the *consumer's* object, from the
    // body the interface carried.
    EMBER_CHECK_MSG(ran.find("2") != std::string::npos, ran);
}

EMBER_TEST(interface_source_is_preferred_when_both_are_there) {
    if (!ember::codegen::is_available()) {
        return;
    }
    // A module you have the source of is the module. An interface must
    // never quietly stand in for something that could be compiled.
    const Workspace workspace;
    workspace.write("greeter.em", "pub fn value() -> int { return 1; }\n");
    workspace.write("greeter.emi", "pub fn value() -> int;\n");
    workspace.write("main.em",
                    "import greeter;\npub fn main() { println(greeter::value()); }\n");

    const std::string ran = workspace.ember("run main.em");
    EMBER_CHECK_MSG(ran.find("1") != std::string::npos,
                    "the source should have been used, and needs no `--link`:\n" + ran);
}

EMBER_TEST(interface_build_as_a_library_needs_no_main) {
    if (!ember::codegen::is_available()) {
        return;
    }
    const Workspace workspace;
    workspace.write("textkit.em", kLibrary);

    const std::string without = workspace.ember("build textkit.em -o out.exe");
    EMBER_CHECK_MSG(without.find("no `main` function found") != std::string::npos, without);

    const std::string with = workspace.ember("build --lib textkit.em -o textkit.o");
    EMBER_CHECK_MSG(with.find("error") == std::string::npos, with);
    EMBER_CHECK_MSG(fs::exists(workspace.path("textkit.o")), "no object was written");
}

EMBER_TEST(interface_library_symbols_carry_the_module_name) {
    if (!ember::codegen::is_available()) {
        return;
    }
    // A library's symbols are the ones consumers link against, and those
    // carry the module prefix - which comes from the file name, the same
    // rule `import` uses to find it.
    const Workspace workspace;
    workspace.write("textkit.em", kLibrary);
    workspace.ember("build --lib textkit.em -o textkit.o");

    const ProcessResult symbols =
        run_process("nm --defined-only " + quoted(workspace.path("textkit.o")));
    if (symbols.exit_code != 0) {
        return;  // no nm on this machine; the end-to-end test still covers it
    }
    EMBER_CHECK_MSG(symbols.output.find("textkit__shout") != std::string::npos,
                    symbols.output);
}
