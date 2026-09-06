// Modules: namespacing, imports, and the visibility rules that `pub`
// finally enforces.
//
// These drive the checker directly with several in-memory modules, so
// they test resolution rather than the filesystem; loading from disk is
// covered end to end by the `modules` golden case.

#include "test_harness.hpp"

#include "cinder/ast/diagnostic.hpp"
#include "cinder/ast/nodes.hpp"
#include "cinder/codegen/codegen.hpp"
#include "cinder/parser/parser.hpp"
#include "cinder/typeck/typeck.hpp"

#include <string>
#include <utility>
#include <vector>

namespace {

using cinder::ast::SourceMap;
using cinder::typeck::CheckResult;

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
    std::vector<cinder::parser::ParseResult> parsed;
    CheckResult checked;
};

Program check_modules(const std::vector<Source>& modules,
                      const std::vector<std::vector<std::string>>& imports) {
    Program program;

    for (const Source& module : modules) {
        const cinder::ast::FileId id = program.sources.add(
            (module.name.empty() ? std::string{"main"} : module.name) + ".ci", module.text);
        cinder::parser::ParseResult parsed =
            cinder::parser::parse_source(program.sources.file(id));
        if (!parsed.ok()) {
            ::cinder::test::fail(__FILE__, __LINE__,
                                "test fixture does not parse:\n" +
                                    cinder::ast::render_all(parsed.diagnostics, program.sources));
        }
        program.parsed.push_back(std::move(parsed));
    }

    std::vector<cinder::typeck::ModuleInput> inputs;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        inputs.push_back(cinder::typeck::ModuleInput{modules[i].name,
                                                    program.parsed[i].program.get(),
                                                    imports[i]});
    }

    program.checked = cinder::typeck::check(inputs, program.sources);
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
        ::cinder::test::fail(__FILE__, __LINE__,
                            "expected these modules to check, but:\n" +
                                cinder::ast::render_all(program.checked.diagnostics,
                                                       program.sources));
    }
}

std::vector<cinder::ast::Diagnostic> reject(const std::vector<Source>& modules) {
    Program program = check_modules(modules, all_import_all(modules));
    if (program.checked.ok()) {
        ::cinder::test::fail(__FILE__, __LINE__,
                            "expected these modules to be rejected, but they checked");
    }
    return std::move(program.checked.diagnostics);
}

std::string first_error(const std::vector<Source>& modules) {
    return reject(modules).at(0).message;
}

/// Lowers a checked multi-module program to LLVM IR text, for the tests
/// that are about what a module name becomes at the symbol level.
std::string compile_modules(const std::vector<Source>& modules) {
    Program program = check_modules(modules, all_import_all(modules));
    if (!program.checked.ok()) {
        ::cinder::test::fail(__FILE__, __LINE__,
                            "expected these modules to check, but:\n" +
                                cinder::ast::render_all(program.checked.diagnostics,
                                                       program.sources));
    }

    std::vector<cinder::codegen::ModuleInput> inputs;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        inputs.push_back(
            cinder::codegen::ModuleInput{modules[i].name, program.parsed[i].program.get()});
    }

    const cinder::codegen::CompileResult compiled =
        cinder::codegen::compile_to_string(inputs, program.checked, {});
    if (!compiled.ok()) {
        ::cinder::test::fail(__FILE__, __LINE__,
                            "codegen failed:\n" +
                                cinder::ast::render_all(compiled.diagnostics, program.sources));
    }
    return compiled.assembly;
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

CINDER_TEST(modules_reach_a_public_function) {
    accept({{"geometry", kGeometry},
            {"", "pub fn main() {\n"
                 "    let p = geometry::Point { x: 3, y: 4 };\n"
                 "    println(geometry::magnitude_sq(p));\n"
                 "}\n"}});
}

CINDER_TEST(modules_reach_a_public_struct_and_constant) {
    accept({{"geometry", kGeometry},
            {"", "pub fn main() {\n"
                 "    let p: geometry::Point = geometry::ORIGIN;\n"
                 "    println(p.x);\n"
                 "}\n"}});
}

CINDER_TEST(modules_keep_their_own_namespace) {
    // Two modules may each declare `helper`; the qualified names differ,
    // so neither collides with the other or with the entry module.
    accept({{"a", "pub fn helper() -> int { return 1; }\n"},
            {"b", "pub fn helper() -> int { return 2; }\n"},
            {"", "pub fn helper() -> int { return 3; }\n"
                 "pub fn main() {\n"
                 "    println(a::helper() + b::helper() + helper());\n"
                 "}\n"}});
}

CINDER_TEST(modules_let_two_modules_declare_the_same_type_name) {
    accept({{"a", "pub struct Value { pub n: int, }\n"},
            {"b", "pub struct Value { pub n: int, }\n"},
            {"", "pub fn main() {\n"
                 "    let x = a::Value { n: 1 };\n"
                 "    let y = b::Value { n: 2 };\n"
                 "    println(x.n + y.n);\n"
                 "}\n"}});
}

CINDER_TEST(modules_treat_same_named_types_from_two_modules_as_distinct) {
    CINDER_CHECK_EQ(first_error({{"a", "pub struct Value { pub n: int, }\n"},
                                {"b", "pub struct Value { pub n: int, }\n"},
                                {"", "pub fn take(v: a::Value) { }\n"
                                     "pub fn main() { take(b::Value { n: 1 }); }\n"}}),
                   std::string{"type mismatch"});
}

CINDER_TEST(modules_resolve_a_module_qualified_name_inside_that_module) {
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

CINDER_TEST(modules_reject_calling_a_private_function) {
    const std::vector<cinder::ast::Diagnostic> errors =
        reject({{"geometry", kGeometry},
                {"", "pub fn main() { println(geometry::private_helper()); }\n"}});
    CINDER_CHECK_EQ(errors.at(0).message,
                   std::string{"function `geometry::private_helper` is private"});
    CINDER_CHECK_MSG(errors.at(0).notes.at(0).find("declared at") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
}

CINDER_TEST(modules_reject_naming_a_private_type) {
    CINDER_CHECK_EQ(first_error({{"geometry", kGeometry},
                                {"", "pub fn take(p: geometry::PrivatePoint) { }\n"}}),
                   std::string{"type `geometry::PrivatePoint` is private"});
}

CINDER_TEST(modules_allow_a_module_to_use_its_own_private_items) {
    accept({{"geometry",
             std::string{kGeometry} +
                 "pub fn uses_private() -> int { return private_helper() + PRIVATE_LIMIT; }\n"},
            {"", "pub fn main() { println(geometry::uses_private()); }\n"}});
}

CINDER_TEST(modules_reject_reading_a_private_field) {
    CINDER_CHECK_EQ(first_error({{"shapes", "pub struct Circle { pub r: int, hidden: int, }\n"
                                           "pub fn make() -> Circle {\n"
                                           "    return Circle { r: 1, hidden: 2 };\n"
                                           "}\n"},
                                {"", "pub fn main() {\n"
                                     "    let c = shapes::make();\n"
                                     "    println(c.hidden);\n"
                                     "}\n"}}),
                   std::string{"field `hidden` of `shapes::Circle` is private"});
}

CINDER_TEST(modules_reject_constructing_a_struct_with_private_fields) {
    // Every field must be given a value and a private one cannot be, so
    // the type is simply not constructible from outside.
    CINDER_CHECK_EQ(first_error({{"shapes", "pub struct Circle { pub r: int, hidden: int, }\n"},
                                {"", "pub fn main() {\n"
                                     "    let c = shapes::Circle { r: 1 };\n"
                                     "    println(c.r);\n"
                                     "}\n"}}),
                   std::string{"`shapes::Circle` cannot be constructed from outside its module"});
}

CINDER_TEST(modules_allow_a_module_to_construct_its_own_private_fields) {
    accept({{"shapes", "pub struct Circle { pub r: int, hidden: int, }\n"
                       "pub fn make() -> Circle { return Circle { r: 1, hidden: 2 }; }\n"},
            {"", "pub fn main() { println(shapes::make().r); }\n"}});
}

CINDER_TEST(modules_reject_a_private_constant) {
    CINDER_CHECK_EQ(first_error({{"config", "const SECRET: int = 1;\npub const OPEN: int = 2;\n"},
                                {"", "pub fn main() { println(config::SECRET); }\n"}}),
                   std::string{"constant `config::SECRET` is private"});
}

// ---------------------------------------------------------------------
// Imports
// ---------------------------------------------------------------------

CINDER_TEST(modules_require_an_import_before_a_module_can_be_named) {
    // `geometry` is loaded, but this module did not import it.
    Program program = check_modules(
        {{"geometry", kGeometry},
         {"", "pub fn main() { println(geometry::magnitude_sq(geometry::ORIGIN)); }\n"}},
        {{}, {}});

    CINDER_CHECK(!program.checked.ok());
    CINDER_CHECK_EQ(program.checked.diagnostics.at(0).message,
                   std::string{"module `geometry` is not imported here"});
}

CINDER_TEST(modules_do_not_make_imports_transitive) {
    // `mid` imports `geometry`; the entry module imports only `mid`, so
    // it still cannot name `geometry`.
    Program program = check_modules(
        {{"geometry", kGeometry},
         {"mid", "pub fn doubled(p: geometry::Point) -> int {\n"
                 "    return geometry::magnitude_sq(p) * 2;\n"
                 "}\n"},
         {"", "pub fn main() { println(geometry::ORIGIN.x); }\n"}},
        {{}, {"geometry"}, {"mid"}});

    CINDER_CHECK(!program.checked.ok());
    CINDER_CHECK_EQ(program.checked.diagnostics.at(0).message,
                   std::string{"module `geometry` is not imported here"});
}

CINDER_TEST(modules_report_an_unknown_module) {
    CINDER_CHECK_EQ(first_error({{"", "pub fn main() { println(nowhere::thing()); }\n"}}),
                   std::string{"cannot find module `nowhere`"});
}

CINDER_TEST(modules_resolve_a_cycle_between_two_modules) {
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

CINDER_TEST(modules_qualify_symbol_names_so_two_modules_can_share_one) {
    // Both declare `helper`; the emitted symbols must differ or the
    // linker would pick one arbitrarily.
    Program program = check_modules({{"a", "pub fn helper() -> int { return 1; }\n"},
                                     {"b", "pub fn helper() -> int { return 2; }\n"},
                                     {"", "pub fn main() { println(a::helper()); }\n"}},
                                    all_import_all({{"a", ""}, {"b", ""}, {"", ""}}));

    CINDER_CHECK(program.checked.ok());
    CINDER_CHECK_EQ(program.checked.functions.at("a::helper").mangled_name,
                   std::string{"a__helper"});
    CINDER_CHECK_EQ(program.checked.functions.at("b::helper").mangled_name,
                   std::string{"b__helper"});
}

CINDER_TEST(modules_leave_the_entry_modules_symbols_unprefixed) {
    Program program =
        check_modules({{"", "pub fn helper() -> int { return 1; }\npub fn main() { }\n"}}, {{}});

    CINDER_CHECK(program.checked.ok());
    CINDER_CHECK_EQ(program.checked.functions.at("helper").mangled_name, std::string{"helper"});
    CINDER_CHECK_EQ(program.checked.functions.at("main").mangled_name, std::string{"main"});
}

CINDER_TEST(modules_resolve_an_unqualified_name_in_its_own_module_only) {
    // `magnitude_sq` is unqualified in the entry module, where it does
    // not exist, so it must not silently resolve to geometry's.
    CINDER_CHECK_EQ(first_error({{"geometry", kGeometry},
                                {"", "pub fn main() { println(magnitude_sq(geometry::ORIGIN)); }\n"}}),
                   std::string{"cannot find function `magnitude_sq`"});
}

CINDER_TEST(modules_let_a_local_shadow_nothing_across_modules) {
    // A local in one module is invisible to another, even by name.
    CINDER_CHECK_EQ(first_error({{"a", "pub fn f() -> int { let hidden = 1; return hidden; }\n"},
                                {"", "pub fn main() { println(hidden); }\n"}}),
                   std::string{"cannot find value `hidden`"});
}

// ---------------------------------------------------------------------
// Generics across modules
// ---------------------------------------------------------------------

CINDER_TEST(modules_instantiate_a_generic_from_another_module) {
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

CINDER_TEST(modules_give_an_instantiation_the_declaring_modules_symbol) {
    Program program =
        check_modules({{"util", "pub fn identity<T>(v: T) -> T { return v; }\n"},
                       {"", "pub fn main() { println(util::identity(1)); }\n"}},
                      {{}, {"util"}});

    CINDER_CHECK(program.checked.ok());
    CINDER_CHECK_EQ(program.checked.instantiations.size(), std::size_t{1});
    CINDER_CHECK_EQ(program.checked.instantiations.front().info.mangled_name,
                   std::string{"util__identity__int"});
}

CINDER_TEST(modules_check_a_template_body_as_its_own_module) {
    // The template uses a private helper from its own module; that has
    // to stay legal when instantiated from somewhere else.
    accept({{"util", "fn twice(n: int) -> int { return n * 2; }\n"
                     "pub fn doubled_len<T>(v: &[T; 2]) -> int { return twice(2); }\n"},
            {"", "pub fn main() {\n"
                 "    let xs: [int; 2] = [1, 2];\n"
                 "    println(util::doubled_len(xs));\n"
                 "}\n"}});
}

CINDER_TEST(modules_reject_a_private_generic_function) {
    CINDER_CHECK_EQ(first_error({{"util", "fn identity<T>(v: T) -> T { return v; }\n"},
                                {"", "pub fn main() { println(util::identity(1)); }\n"}}),
                   std::string{"function `util::identity` is private"});
}

// ---------------------------------------------------------------------
// Single-file programs are unaffected
// ---------------------------------------------------------------------

CINDER_TEST(modules_leave_a_single_file_program_unchanged) {
    const cinder::ast::SourceFile source{"solo.ci",
                                        "pub fn helper() -> int { return 1; }\n"
                                        "pub fn main() { println(helper()); }\n"};
    const cinder::parser::ParseResult parsed = cinder::parser::parse_source(source);
    const CheckResult checked = cinder::typeck::check(*parsed.program, source);

    CINDER_CHECK(checked.ok());
    CINDER_CHECK_EQ(checked.functions.at("helper").mangled_name, std::string{"helper"});
    CINDER_CHECK_EQ(checked.functions.at("helper").module, std::string{});
}

// ---------------------------------------------------------------------
// Nested module paths
//
// A module's name is its whole path, and everything downstream treats
// that as one opaque name: `shapes::geometry::Point` resolves, checks
// and mangles exactly as `geometry::Point` does. Nesting buys
// unambiguous names and nothing else - it is not a privacy model, and
// `shapes::geometry` has no special relationship to `shapes`.
// ---------------------------------------------------------------------

CINDER_TEST(nested_modules_qualify_their_items_by_the_whole_path) {
    accept({
        Source{"shapes::geometry",
               "pub struct Point { pub x: int, }\n"
               "pub fn origin() -> Point { return Point { x: 0 }; }\n"},
        Source{"", "pub fn main() {\n"
                   "    let p = shapes::geometry::origin();\n"
                   "    println(p.x);\n"
                   "}\n"},
    });
}

CINDER_TEST(nested_modules_keep_two_leaves_of_the_same_name_apart) {
    accept({
        Source{"math", "pub fn value() -> int { return 1; }\n"},
        Source{"shapes::math", "pub fn value() -> int { return 2; }\n"},
        Source{"", "pub fn main() { println(math::value() + shapes::math::value()); }\n"},
    });
}

CINDER_TEST(nested_modules_enforce_pub_the_same_way) {
    // Nesting changes how a module is named, not who may reach into it.
    const std::vector<cinder::ast::Diagnostic> errors = reject({
        Source{"shapes::geometry", "fn hidden() -> int { return 1; }\n"},
        Source{"", "pub fn main() { println(shapes::geometry::hidden()); }\n"},
    });
    CINDER_CHECK_EQ(errors.at(0).message,
                   std::string{"function `shapes::geometry::hidden` is private"});
}

CINDER_TEST(nested_modules_give_a_parent_no_special_access) {
    // `shapes` is not a module here, and even if it were it would get
    // nothing extra. There is no nesting *semantics*, only nesting
    // names.
    const std::vector<cinder::ast::Diagnostic> errors = reject({
        Source{"shapes", "pub fn peek() -> int { return shapes::detail::hidden(); }\n"},
        Source{"shapes::detail", "fn hidden() -> int { return 1; }\n"},
        Source{"", "pub fn main() { println(shapes::peek()); }\n"},
    });
    CINDER_CHECK_EQ(errors.at(0).message,
                   std::string{"function `shapes::detail::hidden` is private"});
}

CINDER_TEST(nested_modules_mangle_to_linker_safe_symbols) {
    if (!cinder::codegen::is_available()) {
        return;
    }
    // `::` is not something a linker will accept, so the path becomes
    // `__` - and the two `square`s stay distinct.
    const std::string ir = compile_modules({
        Source{"math", "pub fn square(n: int) -> int { return -1; }\n"},
        Source{"shapes::detail::math", "pub fn square(n: int) -> int { return n * n; }\n"},
        Source{"", "pub fn main() {\n"
                   "    println(math::square(5));\n"
                   "    println(shapes::detail::math::square(5));\n"
                   "}\n"},
    });
    CINDER_CHECK_MSG(ir.find("@math__square(") != std::string::npos, ir);
    CINDER_CHECK_MSG(ir.find("@shapes__detail__math__square(") != std::string::npos, ir);
    CINDER_CHECK_MSG(ir.find("::") == std::string::npos,
                    "a module path leaked into a symbol name:\n" + ir);
}
