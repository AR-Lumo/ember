// Phase 3 unit tests: the type checker.
//
// §8 asks for two sets: programs that must pass, and programs that must
// fail with a specific error. Both are here, one test per error class
// the spec names, plus the §4 rules that have no error class of their
// own (no implicit numeric conversion, nominal structs, methods scoped
// to their type).

#include "test_harness.hpp"

#include "soliton/ast/diagnostic.hpp"
#include "soliton/ast/nodes.hpp"
#include "soliton/parser/parser.hpp"
#include "soliton/typeck/typeck.hpp"
#include "soliton/typeck/types.hpp"

#include <string>
#include <vector>

namespace {

using soliton::ast::SourceFile;
using soliton::typeck::CheckResult;

SourceFile make_source(std::string contents) {
    return SourceFile{"test.sn", std::move(contents)};
}

/// Parse and check. Parsing is asserted clean so a typo in a test
/// fixture surfaces as a parse failure rather than a confusing type
/// error.
CheckResult check(const SourceFile& source) {
    soliton::parser::ParseResult parsed = soliton::parser::parse_source(source);
    if (!parsed.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "test fixture does not parse:\n" +
                                soliton::ast::render_all(parsed.diagnostics, source));
    }
    return soliton::typeck::check(*parsed.program, source);
}

/// Check a program that must be accepted.
void accept(const std::string& contents) {
    const SourceFile source = make_source(contents);
    const CheckResult result = check(source);
    if (!result.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "expected this program to type-check, but:\n" +
                                soliton::ast::render_all(result.diagnostics, source));
    }
}

/// Check a program that must be rejected, returning its diagnostics.
std::vector<soliton::ast::Diagnostic> reject(const std::string& contents) {
    const SourceFile source = make_source(contents);
    CheckResult result = check(source);
    if (result.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "expected this program to be rejected, but it type-checked");
    }
    return std::move(result.diagnostics);
}

/// The headline of the first diagnostic.
std::string first_error(const std::string& contents) {
    return reject(contents).at(0).message;
}

/// A whole program built around a function body, to keep the fixtures
/// down to the line under test.
std::string in_main(const std::string& body) {
    return "pub fn main() {\n    " + body + "\n}\n";
}

const char* const kPointProgram =
    "struct Point {\n"
    "    pub x: int,\n"
    "    pub y: int,\n"
    "}\n"
    "\n"
    "impl Point {\n"
    "    pub fn distance_sq(&self, other: Point) -> int {\n"
    "        let dx = self.x - other.x;\n"
    "        let dy = self.y - other.y;\n"
    "        return dx * dx + dy * dy;\n"
    "    }\n"
    "}\n";

}  // namespace

// ---------------------------------------------------------------------
// Programs that must be accepted
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_accepts_the_spec_example_program) {
    // The §3 example, which is the Phase 4 milestone.
    accept(std::string{kPointProgram} +
           "\n"
           "const ORIGIN: Point = Point { x: 0, y: 0 };\n"
           "\n"
           "pub fn main() {\n"
           "    let p = Point { x: 3, y: 4 };\n"
           "    println(p.distance_sq(ORIGIN));\n"
           "}\n");
}

SOLITON_TEST(typeck_infers_let_types_from_the_initializer) {
    accept(in_main("let a = 1;\n"
                   "    let b = 1.5;\n"
                   "    let c = true;\n"
                   "    let d = \"hi\";\n"
                   "    println(a);\n"
                   "    println(b);\n"
                   "    println(c);\n"
                   "    println(d);"));
}

SOLITON_TEST(typeck_accepts_a_matching_let_annotation) {
    accept(in_main("let a: int = 1;\n    let b: float = 1.5;\n    println(a);\n    println(b);"));
}

SOLITON_TEST(typeck_accepts_arithmetic_on_matching_numeric_types) {
    accept(in_main("let a = 1 + 2 * 3 - 4 / 2 % 3;\n"
                   "    let b = 1.5 + 2.5 * 3.0;\n"
                   "    println(a);\n"
                   "    println(b);"));
}

SOLITON_TEST(typeck_accepts_control_flow_with_bool_conditions) {
    accept(in_main("let mut i = 0;\n"
                   "    while i < 10 {\n"
                   "        if i % 2 == 0 && i > 2 {\n"
                   "            println(i);\n"
                   "        } else {\n"
                   "            println(0);\n"
                   "        }\n"
                   "        i = i + 1;\n"
                   "    }"));
}

SOLITON_TEST(typeck_accepts_arrays_and_indexing) {
    accept(in_main("let xs: [int; 3] = [1, 2, 3];\n"
                   "    println(xs[0]);\n"
                   "    println(len(xs));"));
}

SOLITON_TEST(typeck_accepts_shadowing_in_a_nested_scope) {
    // Rust allows an inner scope to rebind a name (§9: ergonomics).
    accept(in_main("let x = 1;\n"
                   "    if true {\n"
                   "        let x = \"shadowed\";\n"
                   "        println(x);\n"
                   "    }\n"
                   "    println(x);"));
}

SOLITON_TEST(typeck_accepts_a_reference_parameter_given_a_value) {
    // v1 has no address-of operator, so a value has to satisfy `&T` or
    // the type would be unusable.
    accept(std::string{kPointProgram} +
           "pub fn read(p: &Point) -> int {\n"
           "    return p.x;\n"
           "}\n"
           "pub fn main() {\n"
           "    let p = Point { x: 1, y: 2 };\n"
           "    println(read(p));\n"
           "}\n");
}

SOLITON_TEST(typeck_accepts_a_void_function_falling_off_the_end) {
    accept("pub fn nothing() { }\npub fn main() { nothing(); }\n");
}

SOLITON_TEST(typeck_accepts_a_return_on_every_path_of_an_if_else) {
    accept("pub fn pick(flag: bool) -> int {\n"
           "    if flag {\n"
           "        return 1;\n"
           "    } else {\n"
           "        return 2;\n"
           "    }\n"
           "}\n");
}

// ---------------------------------------------------------------------
// Type mismatches (§7's worked example)
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_rejects_a_let_annotation_that_does_not_match) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let x: int = \"hello\";"));
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"type mismatch"});
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `string`"});
}

SOLITON_TEST(typeck_renders_the_spec_worked_example_verbatim) {
    // §7 shows this exact diagnostic. Reproducing it end to end is the
    // strongest check that the format is right.
    std::string contents;
    for (int i = 0; i < 10; ++i) {
        contents += "//\n";
    }
    contents += "pub fn main() {\n    let x: int = \"hello\";\n}\n";
    const SourceFile source{"file.sn", contents};

    const CheckResult result = check(source);
    SOLITON_CHECK_EQ(soliton::ast::render(result.diagnostics.at(0), source),
                   std::string{"error: type mismatch\n"
                               "  --> file.sn:12:18\n"
                               "   |\n"
                               "12 |     let x: int = \"hello\";\n"
                               "   |                  ^^^^^^^ expected `int`, found `string`\n"});
}

SOLITON_TEST(typeck_rejects_a_non_bool_condition) {
    const std::vector<soliton::ast::Diagnostic> errors = reject(in_main("if 1 { }"));
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"type mismatch"});
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `bool`, found `int`"});
}

SOLITON_TEST(typeck_rejects_a_returned_value_of_the_wrong_type) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject("pub fn f() -> int { return true; }\n");
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"type mismatch"});
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `bool`"});
}

SOLITON_TEST(typeck_rejects_assigning_the_wrong_type) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let mut x = 1;\n    x = \"no\";"));
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `string`"});
}

SOLITON_TEST(typeck_rejects_a_non_int_index) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let xs: [int; 2] = [1, 2];\n    println(xs[true]);"));
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `bool`"});
}

// ---------------------------------------------------------------------
// No implicit numeric conversion (§4)
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_rejects_mixing_int_and_float) {
    const std::vector<soliton::ast::Diagnostic> errors = reject(in_main("let x = 1 + 1.5;"));
    SOLITON_CHECK_EQ(errors.at(0).message,
                   std::string{"cannot apply `+` to `int` and `float`"});
    SOLITON_CHECK_EQ(errors.at(0).notes.at(0),
                   std::string{"`int` and `float` never mix implicitly in Soliton (§4)"});
}

// ---------------------------------------------------------------------
// Explicit conversion with `as` (§4)
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_accepts_casts_between_the_numeric_types) {
    accept(in_main("let a = 1 as float;\n"
                   "    let b = 2.5 as int;\n"
                   "    println(a);\n"
                   "    println(b);"));
}

SOLITON_TEST(typeck_lets_a_cast_bridge_int_and_float_arithmetic) {
    // The whole reason `as` has to exist: §4 requires the conversion to
    // be explicit, so there must be a way to write it.
    accept(in_main("let total = 50;\n"
                   "    let count = 5;\n"
                   "    println(total as float / count as float);"));
}

SOLITON_TEST(typeck_accepts_an_identity_cast) {
    accept(in_main("let a = 1 as int;\n    println(a);"));
}

SOLITON_TEST(typeck_gives_a_cast_the_target_type) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let x: int = 1 as float;"));
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `float`"});
}

SOLITON_TEST(typeck_accepts_casts_between_int_and_bool) {
    // C's conventions, which §9 says to follow on the low-level
    // questions: a `bool` is 0 or 1, and an `int` is whether it is not
    // zero.
    accept(in_main("let n = true as int;\n"
                   "    let b = 0 as bool;\n"
                   "    println(n);\n"
                   "    println(b);"));
}

SOLITON_TEST(typeck_rejects_casts_between_unrelated_types) {
    // `float` has no such convention, so it is left out rather than
    // guessed at.
    const std::vector<soliton::ast::Diagnostic> errors = reject(in_main("let x = 1.5 as bool;"));
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"cannot cast `float` to `bool`"});
    SOLITON_CHECK_EQ(errors.at(0).notes.at(0),
                   std::string{"`as` converts between `int` and `float`, and between `int` "
                               "and `bool`"});
}

SOLITON_TEST(typeck_rejects_casting_a_struct) {
    SOLITON_CHECK_EQ(first_error(std::string{kPointProgram} +
                               "pub fn main() {\n"
                               "    let p = Point { x: 1, y: 2 };\n"
                               "    println(p as int);\n"
                               "}\n"),
                   std::string{"cannot cast `Point` to `int`"});
}

SOLITON_TEST(typeck_rejects_casting_a_string) {
    SOLITON_CHECK_EQ(first_error(in_main("let x = \"12\" as int;")),
                   std::string{"cannot cast `string` to `int`"});
}

SOLITON_TEST(typeck_rejects_a_float_annotation_on_an_int_literal) {
    const std::vector<soliton::ast::Diagnostic> errors = reject(in_main("let x: float = 1;"));
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `float`, found `int`"});
}

SOLITON_TEST(typeck_rejects_remainder_on_floats) {
    // C's `%` is integer-only; §9 says follow C on the low-level rules.
    SOLITON_CHECK_EQ(first_error(in_main("let x = 1.5 % 2.0;")),
                   std::string{"cannot apply `%` to `float` and `float`"});
}

SOLITON_TEST(typeck_rejects_arithmetic_on_non_numeric_types) {
    SOLITON_CHECK_EQ(first_error(in_main("let x = \"a\" + \"b\";")),
                   std::string{"cannot apply `+` to `string` and `string`"});
    SOLITON_CHECK_EQ(first_error(in_main("let x = true + false;")),
                   std::string{"cannot apply `+` to `bool` and `bool`"});
}

SOLITON_TEST(typeck_rejects_logical_operators_on_non_bools) {
    SOLITON_CHECK_EQ(first_error(in_main("let x = 1 && 2;")),
                   std::string{"cannot apply `&&` to `int` and `int`"});
}

SOLITON_TEST(typeck_rejects_negating_a_bool_and_not_ing_a_number) {
    SOLITON_CHECK_EQ(first_error(in_main("let x = -true;")),
                   std::string{"cannot apply `-` to `bool`"});
    SOLITON_CHECK_EQ(first_error(in_main("let x = !1;")), std::string{"cannot apply `!` to `int`"});
}

// ---------------------------------------------------------------------
// Undefined identifiers
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_rejects_an_unknown_variable) {
    const std::vector<soliton::ast::Diagnostic> errors = reject(in_main("println(nope);"));
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"cannot find value `nope`"});
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"not found in this scope"});
}

SOLITON_TEST(typeck_rejects_an_unknown_function) {
    SOLITON_CHECK_EQ(first_error(in_main("nope();")),
                   std::string{"cannot find function `nope`"});
}

SOLITON_TEST(typeck_rejects_an_unknown_type) {
    SOLITON_CHECK_EQ(first_error("pub fn f(p: Nope) { }\n"),
                   std::string{"cannot find type `Nope`"});
}

SOLITON_TEST(typeck_suggests_a_close_variable_name) {
    // §8 lists suggestions under Phase 5, but the candidate list already
    // exists here, so it costs nothing.
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let count = 1;\n    println(cout);"));
    SOLITON_CHECK_EQ(errors.at(0).notes.at(0), std::string{"did you mean `count`?"});
}

SOLITON_TEST(typeck_does_not_leak_a_binding_out_of_its_scope) {
    SOLITON_CHECK_EQ(first_error(in_main("if true {\n"
                                       "        let inner = 1;\n"
                                       "    }\n"
                                       "    println(inner);")),
                   std::string{"cannot find value `inner`"});
}

// ---------------------------------------------------------------------
// Wrong argument count and types
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_rejects_too_few_arguments) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject("pub fn add(a: int, b: int) -> int { return a + b; }\n"
               "pub fn main() { println(add(1)); }\n");
    SOLITON_CHECK_EQ(errors.at(0).message,
                   std::string{"this function takes 2 arguments but 1 was supplied"});
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected 2, found 1"});
}

SOLITON_TEST(typeck_rejects_too_many_arguments) {
    SOLITON_CHECK_EQ(first_error("pub fn one(a: int) { }\npub fn main() { one(1, 2); }\n"),
                   std::string{"this function takes 1 argument but 2 were supplied"});
}

SOLITON_TEST(typeck_rejects_an_argument_of_the_wrong_type) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject("pub fn one(a: int) { }\npub fn main() { one(true); }\n");
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"type mismatch"});
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `bool`"});
}

SOLITON_TEST(typeck_counts_method_arguments_without_the_receiver) {
    // `self` is filled by the receiver, so a one-argument method is
    // called with one argument, not two.
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(std::string{kPointProgram} +
               "pub fn main() {\n"
               "    let p = Point { x: 1, y: 2 };\n"
               "    println(p.distance_sq());\n"
               "}\n");
    SOLITON_CHECK_EQ(errors.at(0).message,
                   std::string{"this method takes 1 argument but 0 were supplied"});
}

// ---------------------------------------------------------------------
// Missing return
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_rejects_a_function_that_can_fall_off_the_end) {
    const std::vector<soliton::ast::Diagnostic> errors = reject("pub fn f() -> int { }\n");
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"missing return"});
    SOLITON_CHECK_EQ(errors.at(0).label,
                   std::string{"this function must return `int` on every path"});
}

SOLITON_TEST(typeck_rejects_an_if_without_an_else_as_the_only_return) {
    SOLITON_CHECK_EQ(first_error("pub fn f(flag: bool) -> int {\n"
                               "    if flag {\n"
                               "        return 1;\n"
                               "    }\n"
                               "}\n"),
                   std::string{"missing return"});
}

SOLITON_TEST(typeck_does_not_count_a_while_loop_as_a_guaranteed_return) {
    // The condition may be false on the first test, so control can reach
    // the end of the body.
    SOLITON_CHECK_EQ(first_error("pub fn f() -> int {\n"
                               "    while true {\n"
                               "        return 1;\n"
                               "    }\n"
                               "}\n"),
                   std::string{"missing return"});
}

SOLITON_TEST(typeck_rejects_a_bare_return_from_a_value_function) {
    SOLITON_CHECK_EQ(first_error("pub fn f() -> int { return; }\n"),
                   std::string{"missing return value"});
}

SOLITON_TEST(typeck_rejects_returning_a_value_from_a_void_function) {
    SOLITON_CHECK_EQ(first_error("pub fn f() { return 1; }\n"),
                   std::string{"returning a value from a function with no return type"});
}

// ---------------------------------------------------------------------
// Duplicate definitions
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_rejects_two_functions_with_the_same_name) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject("pub fn f() { }\npub fn f() { }\n");
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"duplicate definition of function `f`"});
    SOLITON_CHECK_MSG(errors.at(0).notes.at(0).find("previously defined at") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
}

SOLITON_TEST(typeck_rejects_two_structs_with_the_same_name) {
    SOLITON_CHECK_EQ(first_error("struct S { x: int, }\nstruct S { y: int, }\n"),
                   std::string{"duplicate definition of type `S`"});
}

SOLITON_TEST(typeck_rejects_duplicate_struct_fields) {
    SOLITON_CHECK_EQ(first_error("struct S { x: int, x: int, }\n"),
                   std::string{"duplicate definition of field `x`"});
}

SOLITON_TEST(typeck_rejects_two_methods_with_the_same_name) {
    SOLITON_CHECK_EQ(first_error("struct S { x: int, }\n"
                               "impl S {\n"
                               "    fn f(&self) { }\n"
                               "    fn f(&self) { }\n"
                               "}\n"),
                   std::string{"duplicate definition of method `f`"});
}

SOLITON_TEST(typeck_rejects_redeclaring_a_binding_in_the_same_scope) {
    SOLITON_CHECK_EQ(first_error(in_main("let x = 1;\n    let x = 2;")),
                   std::string{"duplicate definition of `x`"});
}

SOLITON_TEST(typeck_rejects_duplicate_parameters) {
    SOLITON_CHECK_EQ(first_error("pub fn f(a: int, a: int) { }\n"),
                   std::string{"duplicate definition of parameter `a`"});
}

SOLITON_TEST(typeck_rejects_redefining_an_intrinsic) {
    SOLITON_CHECK_EQ(first_error("pub fn println(x: int) { }\n"),
                   std::string{"cannot redefine the built-in `println`"});
}

// ---------------------------------------------------------------------
// Methods, scoped to their type (§8)
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_rejects_an_unknown_method) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(std::string{kPointProgram} +
               "pub fn main() {\n"
               "    let p = Point { x: 1, y: 2 };\n"
               "    println(p.area());\n"
               "}\n");
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"no method `area` on type `Point`"});
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"unknown method"});
}

SOLITON_TEST(typeck_keeps_methods_out_of_the_global_function_namespace) {
    // §8 is explicit: `impl Point { fn f }` registers `f` on Point, not
    // as a free function.
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(std::string{kPointProgram} + "pub fn main() { distance_sq(); }\n");
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"cannot find function `distance_sq`"});
    SOLITON_CHECK_MSG(errors.at(0).notes.at(0).find("is a method on `Point`") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
}

SOLITON_TEST(typeck_does_not_share_methods_between_structs) {
    SOLITON_CHECK_EQ(first_error("struct A { x: int, }\n"
                               "struct B { x: int, }\n"
                               "impl A { fn only_on_a(&self) -> int { return self.x; } }\n"
                               "pub fn main() {\n"
                               "    let b = B { x: 1 };\n"
                               "    println(b.only_on_a());\n"
                               "}\n"),
                   std::string{"no method `only_on_a` on type `B`"});
}

SOLITON_TEST(typeck_distinguishes_a_field_from_a_method_in_diagnostics) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(std::string{kPointProgram} +
               "pub fn main() {\n"
               "    let p = Point { x: 1, y: 2 };\n"
               "    println(p.x());\n"
               "}\n");
    SOLITON_CHECK_MSG(errors.at(0).notes.at(0).find("is a field, not a method") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
}

SOLITON_TEST(typeck_resolves_a_method_through_a_reference_receiver) {
    // `&self` makes the receiver a `&Point`, so a call on it has to see
    // through the reference.
    accept(std::string{kPointProgram} +
           "impl Point {\n"
           "    pub fn twice(&self, other: Point) -> int {\n"
           "        return self.distance_sq(other) * 2;\n"
           "    }\n"
           "}\n");
}

SOLITON_TEST(typeck_gives_methods_the_mangled_name_codegen_will_emit) {
    // §4 says methods are sugar for plain functions; this is where that
    // becomes a concrete symbol name for Phase 4.
    const SourceFile source = make_source(kPointProgram);
    const CheckResult result = check(source);
    const auto method = result.methods.find(std::make_pair("Point", "distance_sq"));

    SOLITON_CHECK(method != result.methods.end());
    SOLITON_CHECK_EQ(method->second.mangled_name, std::string{"Point_distance_sq"});
    SOLITON_CHECK_EQ(method->second.param_types.size(), std::size_t{2});
    SOLITON_CHECK_EQ(soliton::typeck::to_string(method->second.param_types.at(0)),
                   std::string{"&Point"});
}

// ---------------------------------------------------------------------
// Structs and fields
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_rejects_an_unknown_field) {
    SOLITON_CHECK_EQ(first_error(std::string{kPointProgram} +
                               "pub fn main() {\n"
                               "    let p = Point { x: 1, y: 2 };\n"
                               "    println(p.z);\n"
                               "}\n"),
                   std::string{"no field `z` on type `Point`"});
}

SOLITON_TEST(typeck_rejects_a_struct_literal_with_a_missing_field) {
    SOLITON_CHECK_EQ(first_error(std::string{kPointProgram} +
                               "pub fn main() { let p = Point { x: 1 }; }\n"),
                   std::string{"missing field `y` in initializer of `Point`"});
}

SOLITON_TEST(typeck_rejects_a_struct_literal_with_an_unknown_field) {
    SOLITON_CHECK_EQ(first_error(std::string{kPointProgram} +
                               "pub fn main() { let p = Point { x: 1, y: 2, z: 3 }; }\n"),
                   std::string{"`Point` has no field `z`"});
}

SOLITON_TEST(typeck_rejects_a_repeated_field_initializer) {
    SOLITON_CHECK_EQ(first_error(std::string{kPointProgram} +
                               "pub fn main() { let p = Point { x: 1, x: 2, y: 3 }; }\n"),
                   std::string{"field `x` is initialized more than once"});
}

SOLITON_TEST(typeck_treats_structs_as_nominal) {
    // §4: identical layouts are still different types.
    SOLITON_CHECK_EQ(first_error("struct A { x: int, }\n"
                               "struct B { x: int, }\n"
                               "pub fn take_a(a: A) { }\n"
                               "pub fn main() { take_a(B { x: 1 }); }\n"),
                   std::string{"type mismatch"});
}

SOLITON_TEST(typeck_rejects_a_struct_that_contains_itself_by_value) {
    SOLITON_CHECK_EQ(first_error("struct Node { next: Node, }\n"),
                   std::string{"recursive type `Node` has infinite size"});
}

SOLITON_TEST(typeck_rejects_mutually_recursive_structs) {
    // A -> B -> A is just as unsized as A -> A, and reaches codegen as
    // "cannot allocate unsized type" if it is not caught here.
    const std::vector<soliton::ast::Diagnostic> errors =
        reject("struct A { pub b: B, }\nstruct B { pub a: A, }\n");
    SOLITON_CHECK_EQ(errors.size(), std::size_t{1});
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"recursive type `A` has infinite size"});
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"`A` contains `B`, which contains `A`"});
}

SOLITON_TEST(typeck_rejects_recursion_through_an_array_field) {
    // The cycle runs through `[N; 2]`, so the check has to look through
    // array element types, not just direct struct fields.
    const std::vector<soliton::ast::Diagnostic> errors =
        reject("struct N { pub kids: [N; 2], }\n");
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"recursive type `N` has infinite size"});
}

SOLITON_TEST(typeck_rejects_a_longer_recursion_cycle) {
    SOLITON_CHECK_EQ(first_error("struct A { pub b: B, }\n"
                               "struct B { pub c: C, }\n"
                               "struct C { pub a: A, }\n"),
                   std::string{"recursive type `A` has infinite size"});
}

SOLITON_TEST(typeck_allows_recursion_broken_by_a_reference) {
    // A pointer has a fixed size, so this one is finite. It is also the
    // fix the diagnostic recommends, so it had better be accepted.
    accept("struct Node { pub value: int, pub next: &Node, }\n"
           "pub fn main() { println(1); }\n");
}

SOLITON_TEST(typeck_allows_a_struct_containing_a_different_struct) {
    accept("struct Inner { pub v: int, }\n"
           "struct Outer { pub a: Inner, pub b: Inner, }\n"
           "pub fn main() {\n"
           "    let o = Outer { a: Inner { v: 1 }, b: Inner { v: 2 } };\n"
           "    println(o.a.v);\n"
           "}\n");
}

SOLITON_TEST(typeck_allows_two_structs_sharing_a_field_type) {
    // Diamond, not a cycle: both reach `Leaf`, neither reaches itself.
    accept("struct Leaf { pub v: int, }\n"
           "struct Left { pub leaf: Leaf, }\n"
           "struct Right { pub leaf: Leaf, }\n"
           "struct Top { pub l: Left, pub r: Right, }\n"
           "pub fn main() { println(1); }\n");
}

// ---------------------------------------------------------------------
// Mutability
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_rejects_assigning_to_an_immutable_binding) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let x = 1;\n    x = 2;"));
    SOLITON_CHECK_EQ(errors.at(0).message,
                   std::string{"cannot assign to immutable binding `x`"});
    SOLITON_CHECK_MSG(errors.at(0).notes.at(0).find("let mut x") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
}

SOLITON_TEST(typeck_rejects_assigning_through_an_immutable_struct_binding) {
    SOLITON_CHECK_EQ(first_error(std::string{kPointProgram} +
                               "pub fn main() {\n"
                               "    let p = Point { x: 1, y: 2 };\n"
                               "    p.x = 5;\n"
                               "}\n"),
                   std::string{"cannot assign to immutable binding `p`"});
}

SOLITON_TEST(typeck_accepts_assigning_through_a_mutable_struct_binding) {
    accept(std::string{kPointProgram} +
           "pub fn main() {\n"
           "    let mut p = Point { x: 1, y: 2 };\n"
           "    p.x = 5;\n"
           "    println(p.x);\n"
           "}\n");
}

SOLITON_TEST(typeck_rejects_assigning_to_a_constant) {
    SOLITON_CHECK_EQ(first_error("const LIMIT: int = 10;\npub fn main() { LIMIT = 20; }\n"),
                   std::string{"cannot assign to constant `LIMIT`"});
}

SOLITON_TEST(typeck_rejects_assigning_to_a_parameter) {
    // Parameters get their own wording: "add `mut`" is not available
    // for one, so suggesting it would be bad advice.
    const std::vector<soliton::ast::Diagnostic> errors =
        reject("pub fn f(a: int) { a = 2; }\n");
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"cannot assign to parameter `a`"});
    SOLITON_CHECK_MSG(errors.at(0).notes.at(0).find("let mut` binding") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
}

SOLITON_TEST(typeck_allows_writing_through_a_reference_parameter) {
    // §4 makes `&T` a non-owning pointer with no borrow checker, so
    // `mut` governs rebinding the pointer, not writing to the pointee.
    // Without this, no function could modify its caller's data at all.
    accept("pub fn zero_first(values: &[int; 3]) {\n"
           "    values[0] = 0;\n"
           "}\n"
           "pub fn main() {\n"
           "    let mut xs: [int; 3] = [1, 2, 3];\n"
           "    zero_first(xs);\n"
           "    println(xs[0]);\n"
           "}\n");
}

SOLITON_TEST(typeck_allows_writing_to_a_field_through_a_reference) {
    accept("struct Point { pub x: int, }\n"
           "pub fn reset(p: &Point) {\n"
           "    p.x = 0;\n"
           "}\n"
           "pub fn main() {\n"
           "    let mut p = Point { x: 5 };\n"
           "    reset(p);\n"
           "    println(p.x);\n"
           "}\n");
}

SOLITON_TEST(typeck_still_rejects_rebinding_a_reference_parameter) {
    // Writing through the pointer is fine; replacing the pointer is not.
    SOLITON_CHECK_EQ(first_error("struct Point { pub x: int, }\n"
                               "pub fn swap_target(p: &Point, q: &Point) {\n"
                               "    p = q;\n"
                               "}\n"),
                   std::string{"cannot assign to parameter `p`"});
}

SOLITON_TEST(typeck_rejects_an_invalid_assignment_target) {
    SOLITON_CHECK_EQ(first_error(in_main("1 = 2;")), std::string{"invalid assignment target"});
}

// ---------------------------------------------------------------------
// Intrinsics (§5)
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_accepts_println_on_every_printable_type) {
    accept(in_main("println(1);\n"
                   "    println(1.5);\n"
                   "    println(true);\n"
                   "    println(\"hi\");\n"
                   "    print(1);"));
}

SOLITON_TEST(typeck_rejects_printing_a_struct) {
    SOLITON_CHECK_EQ(first_error(std::string{kPointProgram} +
                               "pub fn main() {\n"
                               "    let p = Point { x: 1, y: 2 };\n"
                               "    println(p);\n"
                               "}\n"),
                   std::string{"cannot print a value of type `Point`"});
}

SOLITON_TEST(typeck_takes_an_empty_array_element_type_from_the_annotation) {
    // A `let` annotation is now pushed down into the initializer, so an
    // expression with nothing else to go on can take its type from it.
    // This used to be an error with no way to satisfy it.
    accept(in_main("let xs: [int; 0] = [];\n    println(len(xs));"));
}

SOLITON_TEST(typeck_rejects_an_empty_array_literal_with_nothing_to_infer_from) {
    // Without an annotation there is still no element type to take, and
    // that must be reported exactly once rather than cascading.
    const std::vector<soliton::ast::Diagnostic> errors = reject(in_main("let xs = [];"));
    SOLITON_CHECK_EQ(errors.size(), std::size_t{1});
    SOLITON_CHECK_EQ(errors.at(0).message,
                   std::string{"cannot infer the element type of an empty array"});
}

SOLITON_TEST(typeck_rejects_len_on_a_non_array) {
    SOLITON_CHECK_EQ(first_error(in_main("println(len(1));")),
                   std::string{"cannot take the length of `int`"});
}

SOLITON_TEST(typeck_rejects_println_with_the_wrong_argument_count) {
    SOLITON_CHECK_EQ(first_error(in_main("println(1, 2);")),
                   std::string{"this function takes 1 argument but 2 were supplied"});
}

// ---------------------------------------------------------------------
// Error recovery
// ---------------------------------------------------------------------

SOLITON_TEST(typeck_reports_several_errors_in_one_run) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let a: int = \"x\";\n"
                       "    let b: bool = 1;\n"
                       "    let c: float = true;"));
    SOLITON_CHECK_EQ(errors.size(), std::size_t{3});
}

SOLITON_TEST(typeck_does_not_cascade_from_one_bad_expression) {
    // `nope` is undefined once; using its value afterwards must stay
    // quiet, because the poison type is compatible with everything.
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let x = nope + 1;\n    let y: bool = x;\n    println(y);"));
    SOLITON_CHECK_EQ(errors.size(), std::size_t{1});
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"cannot find value `nope`"});
}

SOLITON_TEST(typeck_records_a_type_for_every_expression_it_visits) {
    // Phase 4 reads these back, so a gap here is a codegen crash later.
    const SourceFile source = make_source(std::string{kPointProgram} +
                                          "pub fn main() {\n"
                                          "    let p = Point { x: 3, y: 4 };\n"
                                          "    println(p.distance_sq(p));\n"
                                          "}\n");
    const CheckResult result = check(source);
    SOLITON_CHECK(result.ok());
    SOLITON_CHECK(result.expr_types.size() > 10);

    for (const auto& [expr, type] : result.expr_types) {
        SOLITON_CHECK_MSG(type != nullptr, "an expression was recorded with a null type");
    }
}
