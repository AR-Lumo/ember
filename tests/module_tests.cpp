// Modules: namespacing, imports, and the visibility rules that `pub`
// finally enforces.
//
// These drive the checker directly with several in-memory modules, so
// they test resolution rather than the filesystem; loading from disk is
// covered end to end by the `modules` golden case.

#include "test_harness.hpp"

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/nodes.hpp"
#include "ember/parser/parser.hpp"
#include "ember/typeck/typeck.hpp"

#include <string>
#include <utility>
#include <vector>

namespace {

using ember::ast::SourceMap;
using ember::typeck::CheckResult;

/// One module of a test program: its name and its source.
struct Source {
    std::string name;
    std::string text;
};

/// Parse and check several modules together, the way the driver does.
///
/// Every module implicitly imports every other one here unless a test
/// says otherwise, because most of them are about what happens *after*
/// the import; the import rules themselves are tested by passing an
/// explicit import list.
struct Program {
    SourceMap sources;
    std::vector<ember::parser::ParseResult> parsed;
    CheckResult checked;
};

Program check_modules(const std::vector<Source>& modules,
                      const std::vector<std::vector<std::string>>& imports) {
    Program program;

    for (const Source& module : modules) {
        const ember::ast::FileId id = program.sources.add(
            (module.name.empty() ? std::string{"main"} : module.name) + ".em", module.text);
        ember::parser::ParseResult parsed =
            ember::parser::parse_source(program.sources.file(id));
        if (!parsed.ok()) {
            ::ember::test::fail(__FILE__, __LINE__,
                                "test fixture does not parse:\n" +
                                    ember::ast::render_all(parsed.diagnostics, program.sources));
        }
        program.parsed.push_back(std::move(parsed));
    }

    std::vector<ember::typeck::ModuleInput> inputs;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        inputs.push_back(ember::typeck::ModuleInput{modules[i].name,
                                                    program.parsed[i].program.get(),
                                                    imports[i]});
    }

    program.checked = ember::typeck::check(inputs, program.sources);
    return program;
}

/// Every module imports every other. The common case for these tests.
std::vector<std::vector<std::string>> all_import_all(const std::vector<Source>& modules) {
    std::vector<std::vector<std::string>> imports;
    for (const Source& module : modules) {
        std::vector<std::string> names;
        for (const Source& other : modules) {
            if (other.name != module.name && !other.name.empty()) {
                names.push_back(other.name);
            }
        }
        imports.push_back(std::move(names));
    }
    return imports;
}

void accept(const std::vector<Source>& modules) {
    Program program = check_modules(modules, all_import_all(modules));
    if (!program.checked.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected these modules to check, but:\n" +
                                ember::ast::render_all(program.checked.diagnostics,
                                                       program.sources));
    }
}

std::vector<ember::ast::Diagnostic> reject(const std::vector<Source>& modules) {
    Program program = check_modules(modules, all_import_all(modules));
    if (program.checked.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected these modules to be rejected, but they checked");
    }
    return std::move(program.checked.diagnostics);
}

std::string first_error(const std::vector<Source>& modules) {
    return reject(modules).at(0).message;
}

const char* const kGeometry =
    "pub struct Point { pub x: int, pub y: int, }\n"
    "pub const ORIGIN: Point = Point { x: 0, y: 0 };\n"
    "pub fn magnitude_sq(p: Point) -> int { return p.x * p.x + p.y * p.y; }\n"
    "fn private_helper() -> int { return 1; }\n"
    "struct PrivatePoint { pub x: int, }\n"
    "pub const PRIVATE_LIMIT: int = 5;\n";

}  // namespace

// ---------------------------------------------------------------------
// Reaching across a module boundary
// ---------------------------------------------------------------------

EMBER_TEST(modules_reach_a_public_function) {
    accept({{"geometry", kGeometry},
            {"", "pub fn main() {\n"
                 "    let p = geometry::Point { x: 3, y: 4 };\n"
                 "    println(geometry::magnitude_sq(p));\n"
                 "}\n"}});
}

EMBER_TEST(modules_reach_a_public_struct_and_constant) {
    accept({{"geometry", kGeometry},
            {"", "pub fn main() {\n"
                 "    let p: geometry::Point = geometry::ORIGIN;\n"
                 "    println(p.x);\n"
                 "}\n"}});
}

EMBER_TEST(modules_keep_their_own_namespace) {
    // Two modules may each declare `helper`; the qualified names differ,
    // so neither collides with the other or with the entry module.
    accept({{"a", "pub fn helper() -> int { return 1; }\n"},
            {"b", "pub fn helper() -> int { return 2; }\n"},
            {"", "pub fn helper() -> int { return 3; }\n"
                 "pub fn main() {\n"
                 "    println(a::helper() + b::helper() + helper());\n"
                 "}\n"}});
}

EMBER_TEST(modules_let_two_modules_declare_the_same_type_name) {
    accept({{"a", "pub struct Value { pub n: int, }\n"},
            {"b", "pub struct Value { pub n: int, }\n"},
            {"", "pub fn main() {\n"
                 "    let x = a::Value { n: 1 };\n"
                 "    let y = b::Value { n: 2 };\n"
                 "    println(x.n + y.n);\n"
                 "}\n"}});
}

EMBER_TEST(modules_treat_same_named_types_from_two_modules_as_distinct) {
    EMBER_CHECK_EQ(first_error({{"a", "pub struct Value { pub n: int, }\n"},
                                {"b", "pub struct Value { pub n: int, }\n"},
                                {"", "pub fn take(v: a::Value) { }\n"
                                     "pub fn main() { take(b::Value { n: 1 }); }\n"}}),
                   std::string{"type mismatch"});
}

EMBER_TEST(modules_resolve_a_module_qualified_name_inside_that_module) {
    // `geometry::magnitude_sq` written inside `geometry` itself is legal
    // and reaches even private items, being the same module.
    accept({{"geometry",
             std::string{kGeometry} +
                 "pub fn twice(p: Point) -> int {\n"
                 "    return geometry::magnitude_sq(p) + geometry::private_helper();\n"
                 "}\n"},
            {"", "pub fn main() { println(geometry::twice(geometry::ORIGIN)); }\n"}});
}

// ---------------------------------------------------------------------
// Visibility - what `pub` now means
// ---------------------------------------------------------------------

EMBER_TEST(modules_reject_calling_a_private_function) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject({{"geometry", kGeometry},
                {"", "pub fn main() { println(geometry::private_helper()); }\n"}});
    EMBER_CHECK_EQ(errors.at(0).message,
                   std::string{"function `geometry::private_helper` is private"});
    EMBER_CHECK_MSG(errors.at(0).notes.at(0).find("declared at") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
}

EMBER_TEST(modules_reject_naming_a_private_type) {
    EMBER_CHECK_EQ(first_error({{"geometry", kGeometry},
                                {"", "pub fn take(p: geometry::PrivatePoint) { }\n"}}),
                   std::string{"type `geometry::PrivatePoint` is private"});
}

EMBER_TEST(modules_allow_a_module_to_use_its_own_private_items) {
    accept({{"geometry",
             std::string{kGeometry} +
                 "pub fn uses_private() -> int { return private_helper() + PRIVATE_LIMIT; }\n"},
            {"", "pub fn main() { println(geometry::uses_private()); }\n"}});
}

EMBER_TEST(modules_reject_reading_a_private_field) {
    EMBER_CHECK_EQ(first_error({{"shapes", "pub struct Circle { pub r: int, hidden: int, }\n"
                                           "pub fn make() -> Circle {\n"
                                           "    return Circle { r: 1, hidden: 2 };\n"
                                           "}\n"},
                                {"", "pub fn main() {\n"
                                     "    let c = shapes::make();\n"
                                     "    println(c.hidden);\n"
                                     "}\n"}}),
                   std::string{"field `hidden` of `shapes::Circle` is private"});
}

EMBER_TEST(modules_reject_constructing_a_struct_with_private_fields) {
    // Every field must be given a value and a private one cannot be, so
    // the type is simply not constructible from outside.
    EMBER_CHECK_EQ(first_error({{"shapes", "pub struct Circle { pub r: int, hidden: int, }\n"},
                                {"", "pub fn main() {\n"
                                     "    let c = shapes::Circle { r: 1 };\n"
                                     "    println(c.r);\n"
                                     "}\n"}}),
                   std::string{"`shapes::Circle` cannot be constructed from outside its module"});
}

EMBER_TEST(modules_allow_a_module_to_construct_its_own_private_fields) {
    accept({{"shapes", "pub struct Circle { pub r: int, hidden: int, }\n"
                       "pub fn make() -> Circle { return Circle { r: 1, hidden: 2 }; }\n"},
            {"", "pub fn main() { println(shapes::make().r); }\n"}});
}

EMBER_TEST(modules_reject_a_private_constant) {
    EMBER_CHECK_EQ(first_error({{"config", "const SECRET: int = 1;\npub const OPEN: int = 2;\n"},
                                {"", "pub fn main() { println(config::SECRET); }\n"}}),
                   std::string{"constant `config::SECRET` is private"});
}

// ---------------------------------------------------------------------
// Imports
// ---------------------------------------------------------------------

EMBER_TEST(modules_require_an_import_before_a_module_can_be_named) {
    // `geometry` is loaded, but this module did not import it.
    Program program = check_modules(
        {{"geometry", kGeometry},
         {"", "pub fn main() { println(geometry::magnitude_sq(geometry::ORIGIN)); }\n"}},
        {{}, {}});

    EMBER_CHECK(!program.checked.ok());
    EMBER_CHECK_EQ(program.checked.diagnostics.at(0).message,
                   std::string{"module `geometry` is not imported here"});
}

EMBER_TEST(modules_do_not_make_imports_transitive) {
    // `mid` imports `geometry`; the entry module imports only `mid`, so
    // it still cannot name `geometry`.
    Program program = check_modules(
        {{"geometry", kGeometry},
         {"mid", "pub fn doubled(p: geometry::Point) -> int {\n"
                 "    return geometry::magnitude_sq(p) * 2;\n"
                 "}\n"},
         {"", "pub fn main() { println(geometry::ORIGIN.x); }\n"}},
        {{}, {"geometry"}, {"mid"}});

    EMBER_CHECK(!program.checked.ok());
    EMBER_CHECK_EQ(program.checked.diagnostics.at(0).message,
                   std::string{"module `geometry` is not imported here"});
}

EMBER_TEST(modules_report_an_unknown_module) {
    EMBER_CHECK_EQ(first_error({{"", "pub fn main() { println(nowhere::thing()); }\n"}}),
                   std::string{"cannot find module `nowhere`"});
}

EMBER_TEST(modules_resolve_a_cycle_between_two_modules) {
    // Every module is collected before any body is checked, so neither
    // has to be declared first.
    accept({{"a", "pub struct Ping { pub n: int, }\n"
                  "pub fn ping(n: int) -> int { return b::pong(n); }\n"},
            {"b", "pub fn pong(n: int) -> int { return n + 1; }\n"
                  "pub fn make() -> a::Ping { return a::Ping { n: 1 }; }\n"},
            {"", "pub fn main() { println(a::ping(1) + b::make().n); }\n"}});
}

// ---------------------------------------------------------------------
// Symbols and unqualified resolution
// ---------------------------------------------------------------------

EMBER_TEST(modules_qualify_symbol_names_so_two_modules_can_share_one) {
    // Both declare `helper`; the emitted symbols must differ or the
    // linker would pick one arbitrarily.
    Program program = check_modules({{"a", "pub fn helper() -> int { return 1; }\n"},
                                     {"b", "pub fn helper() -> int { return 2; }\n"},
                                     {"", "pub fn main() { println(a::helper()); }\n"}},
                                    all_import_all({{"a", ""}, {"b", ""}, {"", ""}}));

    EMBER_CHECK(program.checked.ok());
    EMBER_CHECK_EQ(program.checked.functions.at("a::helper").mangled_name,
                   std::string{"a__helper"});
    EMBER_CHECK_EQ(program.checked.functions.at("b::helper").mangled_name,
                   std::string{"b__helper"});
}

EMBER_TEST(modules_leave_the_entry_modules_symbols_unprefixed) {
    Program program =
        check_modules({{"", "pub fn helper() -> int { return 1; }\npub fn main() { }\n"}}, {{}});

    EMBER_CHECK(program.checked.ok());
    EMBER_CHECK_EQ(program.checked.functions.at("helper").mangled_name, std::string{"helper"});
    EMBER_CHECK_EQ(program.checked.functions.at("main").mangled_name, std::string{"main"});
}

EMBER_TEST(modules_resolve_an_unqualified_name_in_its_own_module_only) {
    // `magnitude_sq` is unqualified in the entry module, where it does
    // not exist, so it must not silently resolve to geometry's.
    EMBER_CHECK_EQ(first_error({{"geometry", kGeometry},
                                {"", "pub fn main() { println(magnitude_sq(geometry::ORIGIN)); }\n"}}),
                   std::string{"cannot find function `magnitude_sq`"});
}

EMBER_TEST(modules_let_a_local_shadow_nothing_across_modules) {
    // A local in one module is invisible to another, even by name.
    EMBER_CHECK_EQ(first_error({{"a", "pub fn f() -> int { let hidden = 1; return hidden; }\n"},
                                {"", "pub fn main() { println(hidden); }\n"}}),
                   std::string{"cannot find value `hidden`"});
}

// ---------------------------------------------------------------------
// Generics across modules
// ---------------------------------------------------------------------

EMBER_TEST(modules_instantiate_a_generic_from_another_module) {
    accept({{"util", "pub fn max<T>(a: T, b: T) -> T {\n"
                     "    if a > b {\n"
                     "        return a;\n"
                     "    }\n"
                     "    return b;\n"
                     "}\n"},
            {"", "pub fn main() {\n"
                 "    println(util::max(1, 2));\n"
                 "    println(util::max(1.5, 2.5));\n"
                 "}\n"}});
}

EMBER_TEST(modules_give_an_instantiation_the_declaring_modules_symbol) {
    Program program =
        check_modules({{"util", "pub fn identity<T>(v: T) -> T { return v; }\n"},
                       {"", "pub fn main() { println(util::identity(1)); }\n"}},
                      {{}, {"util"}});

    EMBER_CHECK(program.checked.ok());
    EMBER_CHECK_EQ(program.checked.instantiations.size(), std::size_t{1});
    EMBER_CHECK_EQ(program.checked.instantiations.front().info.mangled_name,
                   std::string{"util__identity__int"});
}

EMBER_TEST(modules_check_a_template_body_as_its_own_module) {
    // The template uses a private helper from its own module; that has
    // to stay legal when instantiated from somewhere else.
    accept({{"util", "fn twice(n: int) -> int { return n * 2; }\n"
                     "pub fn doubled_len<T>(v: &[T; 2]) -> int { return twice(2); }\n"},
            {"", "pub fn main() {\n"
                 "    let xs: [int; 2] = [1, 2];\n"
                 "    println(util::doubled_len(xs));\n"
                 "}\n"}});
}

EMBER_TEST(modules_reject_a_private_generic_function) {
    EMBER_CHECK_EQ(first_error({{"util", "fn identity<T>(v: T) -> T { return v; }\n"},
                                {"", "pub fn main() { println(util::identity(1)); }\n"}}),
                   std::string{"function `util::identity` is private"});
}

// ---------------------------------------------------------------------
// Single-file programs are unaffected
// ---------------------------------------------------------------------

EMBER_TEST(modules_leave_a_single_file_program_unchanged) {
    const ember::ast::SourceFile source{"solo.em",
                                        "pub fn helper() -> int { return 1; }\n"
                                        "pub fn main() { println(helper()); }\n"};
    const ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    const CheckResult checked = ember::typeck::check(*parsed.program, source);

    EMBER_CHECK(checked.ok());
    EMBER_CHECK_EQ(checked.functions.at("helper").mangled_name, std::string{"helper"});
    EMBER_CHECK_EQ(checked.functions.at("helper").module, std::string{});
}
