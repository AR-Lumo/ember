// Generics: parsing, inference, monomorphization and the errors.
//
// The design under test is C++'s rather than Rust's. Ember has no traits,
// so a type parameter carries no guarantees and a generic body cannot be
// checked in the abstract; each instantiation is checked as if written
// out by hand. These tests pin both halves of that bargain: what a
// template accepts once instantiated, and how a failure is reported back
// to the call that caused it.

#include "test_harness.hpp"

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/nodes.hpp"
#include "ember/codegen/codegen.hpp"
#include "ember/parser/parser.hpp"
#include "ember/typeck/typeck.hpp"
#include "ember/typeck/types.hpp"

#include <string>
#include <vector>

namespace {

using ember::ast::SourceFile;
using ember::typeck::CheckResult;

SourceFile make_source(std::string contents) {
    return SourceFile{"test.em", std::move(contents)};
}

CheckResult check(const SourceFile& source) {
    ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    if (!parsed.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "test fixture does not parse:\n" +
                                ember::ast::render_all(parsed.diagnostics, source));
    }
    return ember::typeck::check(*parsed.program, source);
}

void accept(const std::string& contents) {
    const SourceFile source = make_source(contents);
    const CheckResult result = check(source);
    if (!result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this program to type-check, but:\n" +
                                ember::ast::render_all(result.diagnostics, source));
    }
}

std::vector<ember::ast::Diagnostic> reject(const std::string& contents) {
    const SourceFile source = make_source(contents);
    CheckResult result = check(source);
    if (result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this program to be rejected, but it type-checked");
    }
    return std::move(result.diagnostics);
}

std::string first_error(const std::string& contents) { return reject(contents).at(0).message; }

/// The `display_name` of every instantiation, in order, joined by spaces.
std::string instantiations_of(const std::string& contents) {
    const SourceFile source = make_source(contents);
    const CheckResult result = check(source);

    std::string out;
    for (const ember::typeck::Instantiation& instance : result.instantiations) {
        if (!out.empty()) {
            out += ' ';
        }
        out += instance.info.display_name;
    }
    return out;
}

/// The emitted LLVM IR, for checking what monomorphization produced.
std::string compile_ir(const std::string& contents) {
    const SourceFile source = make_source(contents);
    const ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    const CheckResult checked = ember::typeck::check(*parsed.program, source);
    if (!checked.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "fixture does not type-check:\n" +
                                ember::ast::render_all(checked.diagnostics, source));
    }
    const ember::codegen::CompileResult compiled =
        ember::codegen::compile_to_string(*parsed.program, checked, source);
    if (!compiled.ok()) {
        ::ember::test::fail(__FILE__, __LINE__, "codegen failed");
    }
    return compiled.assembly;
}

const char* const kMax =
    "pub fn max<T>(a: T, b: T) -> T {\n"
    "    if a > b {\n"
    "        return a;\n"
    "    }\n"
    "    return b;\n"
    "}\n";

}  // namespace

// ---------------------------------------------------------------------
// Generic functions
// ---------------------------------------------------------------------

EMBER_TEST(generics_instantiate_one_function_at_several_types) {
    accept(std::string{kMax} +
           "pub fn main() {\n"
           "    println(max(1, 2));\n"
           "    println(max(1.5, 2.5));\n"
           "}\n");
}

EMBER_TEST(generics_record_one_instantiation_per_distinct_argument_list) {
    // Calling with the same types twice must reuse the instantiation,
    // or every call site would emit its own copy of the function.
    EMBER_CHECK_EQ(instantiations_of(std::string{kMax} +
                                     "pub fn main() {\n"
                                     "    println(max(1, 2));\n"
                                     "    println(max(3, 4));\n"
                                     "    println(max(1.5, 2.5));\n"
                                     "}\n"),
                   std::string{"max<int> max<float>"});
}

EMBER_TEST(generics_accept_a_parameter_with_no_operations_at_all) {
    // `identity` never operates on its argument, so `T` can be anything,
    // including types with no comparison or arithmetic.
    accept("pub fn identity<T>(v: T) -> T { return v; }\n"
           "struct Point { pub x: int, }\n"
           "pub fn main() {\n"
           "    println(identity(1));\n"
           "    println(identity(\"s\"));\n"
           "    println(identity(true));\n"
           "    let p = identity(Point { x: 1 });\n"
           "    println(p.x);\n"
           "}\n");
}

EMBER_TEST(generics_infer_through_a_reference_parameter) {
    accept("pub fn first<T>(values: &[T; 3]) -> T { return values[0]; }\n"
           "pub fn main() {\n"
           "    let xs: [int; 3] = [1, 2, 3];\n"
           "    println(first(xs));\n"
           "}\n");
}

EMBER_TEST(generics_infer_through_an_array_element_type) {
    EMBER_CHECK_EQ(instantiations_of("pub fn first<T>(v: &[T; 2]) -> T { return v[0]; }\n"
                                     "pub fn main() {\n"
                                     "    let a: [int; 2] = [1, 2];\n"
                                     "    let b: [float; 2] = [1.5, 2.5];\n"
                                     "    println(first(a));\n"
                                     "    println(first(b));\n"
                                     "}\n"),
                   std::string{"first<int> first<float>"});
}

EMBER_TEST(generics_support_several_type_parameters) {
    EMBER_CHECK_EQ(instantiations_of("pub fn pick<A, B>(a: A, b: B) -> A { return a; }\n"
                                     "pub fn main() { println(pick(1, \"s\")); }\n"),
                   std::string{"pick<int, string>"});
}

EMBER_TEST(generics_allow_a_generic_function_to_call_another) {
    accept("pub fn identity<T>(v: T) -> T { return v; }\n"
           "pub fn twice<T>(v: T) -> T { return identity(identity(v)); }\n"
           "pub fn main() { println(twice(1)); }\n");
}

EMBER_TEST(generics_terminate_on_a_self_recursive_template) {
    // The instantiation is registered before its body is checked, so a
    // template that calls itself at the same type finds the existing
    // entry instead of recursing forever.
    accept("pub fn countdown<T>(v: T, n: int) -> T {\n"
           "    if n <= 0 {\n"
           "        return v;\n"
           "    }\n"
           "    return countdown(v, n - 1);\n"
           "}\n"
           "pub fn main() { println(countdown(7, 3)); }\n");
}

// ---------------------------------------------------------------------
// Generic structs
// ---------------------------------------------------------------------

EMBER_TEST(generics_instantiate_a_struct_at_several_types) {
    accept("struct Pair<T> { pub a: T, pub b: T, }\n"
           "pub fn main() {\n"
           "    let i: Pair<int> = Pair { a: 1, b: 2 };\n"
           "    let f: Pair<float> = Pair { a: 1.5, b: 2.5 };\n"
           "    println(i.a);\n"
           "    println(f.b);\n"
           "}\n");
}

EMBER_TEST(generics_infer_a_struct_literals_arguments_from_its_fields) {
    // A literal never spells its type arguments out: in an expression
    // `Pair<int> { }` would be indistinguishable from comparisons.
    accept("struct Pair<T> { pub a: T, pub b: T, }\n"
           "pub fn main() {\n"
           "    let p = Pair { a: 1, b: 2 };\n"
           "    println(p.a);\n"
           "}\n");
}

EMBER_TEST(generics_treat_two_instantiations_as_different_types) {
    EMBER_CHECK_EQ(first_error("struct Pair<T> { pub a: T, pub b: T, }\n"
                               "pub fn take(p: Pair<int>) { }\n"
                               "pub fn main() { take(Pair { a: 1.5, b: 2.5 }); }\n"),
                   std::string{"type mismatch"});
}

EMBER_TEST(generics_support_a_struct_with_several_parameters) {
    accept("struct Entry<K, V> { pub key: K, pub value: V, }\n"
           "pub fn value_of<K, V>(e: Entry<K, V>) -> V { return e.value; }\n"
           "pub fn main() {\n"
           "    let e: Entry<string, int> = Entry { key: \"k\", value: 1 };\n"
           "    println(value_of(e));\n"
           "}\n");
}

EMBER_TEST(generics_infer_a_parameter_through_a_generic_struct) {
    EMBER_CHECK_EQ(instantiations_of("struct Pair<T> { pub a: T, pub b: T, }\n"
                                     "pub fn left<T>(p: Pair<T>) -> T { return p.a; }\n"
                                     "pub fn main() {\n"
                                     "    let i: Pair<int> = Pair { a: 1, b: 2 };\n"
                                     "    println(left(i));\n"
                                     "}\n"),
                   std::string{"left<int>"});
}

EMBER_TEST(generics_reject_the_wrong_number_of_type_arguments) {
    EMBER_CHECK_EQ(first_error("struct Pair<T> { pub a: T, pub b: T, }\n"
                               "pub fn take(p: Pair<int, float>) { }\n"),
                   std::string{"`Pair` takes 1 type argument but 2 were given"});
}

EMBER_TEST(generics_reject_type_arguments_on_a_plain_struct) {
    EMBER_CHECK_EQ(first_error("struct Point { pub x: int, }\n"
                               "pub fn take(p: Point<int>) { }\n"),
                   std::string{"`Point` is not a generic type"});
}

// ---------------------------------------------------------------------
// Errors reported against the instantiation
// ---------------------------------------------------------------------

EMBER_TEST(generics_report_a_body_error_against_the_call_that_caused_it) {
    // The heart of the C++ model: `a > b` is fine for `int` and not for
    // a struct, so the error can only be found once `T` is known. The
    // note is what makes that navigable.
    const std::vector<ember::ast::Diagnostic> errors =
        reject("struct Point { pub x: int, }\n" + std::string{kMax} +
               "pub fn main() {\n"
               "    let p = Point { x: 1 };\n"
               "    println(max(p, p).x);\n"
               "}\n");

    EMBER_CHECK_EQ(errors.at(0).message, std::string{"cannot compare values of type `Point`"});
    EMBER_CHECK_MSG(errors.at(0).notes.at(0).find("instantiated as `max<Point>`") !=
                        std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
}

EMBER_TEST(generics_report_conflicting_inference_as_a_mismatch) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject("pub fn pick<T>(a: T, b: T) -> T { return a; }\n"
               "pub fn main() { println(pick(1, 2.5)); }\n");
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"type mismatch"});
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `float`"});
}

EMBER_TEST(generics_reject_a_parameter_nothing_could_determine) {
    // There is no turbofish, so a parameter appearing in neither the
    // arguments nor the return type could never be worked out. Better to
    // say so at the declaration than to fail at every call.
    const std::vector<ember::ast::Diagnostic> errors =
        reject("pub fn make<T>() -> int { return 1; }\n");
    EMBER_CHECK_EQ(errors.at(0).message,
                   std::string{"type parameter `T` cannot be inferred"});
}

EMBER_TEST(generics_infer_a_constructor_from_what_it_is_bound_to) {
    // A parameter used only in the return type is fine, because the
    // context supplies it - which is the only way to write a constructor
    // for a generic type, and the same rule `new_vec()` already had.
    accept("struct Stack<T> { pub items: Vec<T>, }\n"
           "pub fn new_stack<T>() -> Stack<T> {\n"
           "    let items: Vec<T> = new_vec();\n"
           "    return Stack { items: items };\n"
           "}\n"
           "pub fn main() {\n"
           "    let s: Stack<int> = new_stack();\n"
           "    println(len(s.items));\n"
           "}\n");
}

EMBER_TEST(generics_say_where_a_constructors_type_has_to_come_from) {
    // Without an annotation there is nothing to infer from, and the
    // error says which way out there is.
    const std::vector<ember::ast::Diagnostic> errors =
        reject("struct Stack<T> { pub items: Vec<T>, }\n"
               "pub fn new_stack<T>() -> Stack<T> {\n"
               "    let items: Vec<T> = new_vec();\n"
               "    return Stack { items: items };\n"
               "}\n"
               "pub fn main() { let s = new_stack(); }\n");
    EMBER_CHECK_EQ(errors.at(0).message,
                   std::string{"cannot infer type parameter `T`"});

    bool says_annotate = false;
    for (const std::string& note : errors.at(0).notes) {
        says_annotate = says_annotate || note.find("annotate") != std::string::npos;
    }
    EMBER_CHECK_MSG(says_annotate, "the error should say an annotation would settle it");
}

EMBER_TEST(generics_still_prefer_the_arguments_over_the_binding) {
    // The arguments are unified first, so a hint cannot override what a
    // call actually passed - it only fills in what was left.
    const std::vector<ember::ast::Diagnostic> errors =
        reject("pub fn id<T>(value: T) -> T { return value; }\n"
               "pub fn main() { let x: float = id(1); }\n");
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"expected `float`, found `int`"});
}

EMBER_TEST(generics_reject_a_struct_literal_that_determines_nothing) {
    EMBER_CHECK_EQ(first_error("struct Holder<T> { pub items: [T; 0], }\n"
                               "pub fn main() { let h = Holder { items: [] }; }\n"),
                   std::string{"cannot infer the element type of an empty array"});
}

EMBER_TEST(generics_reject_the_wrong_argument_count_to_a_template) {
    EMBER_CHECK_EQ(first_error("pub fn pair<T>(a: T, b: T) -> T { return a; }\n"
                               "pub fn main() { println(pair(1)); }\n"),
                   std::string{"this function takes 2 arguments but 1 was supplied"});
}

EMBER_TEST(generics_reject_duplicate_type_parameters) {
    EMBER_CHECK_EQ(first_error("pub fn f<T, T>(a: T, b: T) -> T { return a; }\n"),
                   std::string{"duplicate type parameter `T`"});
}

EMBER_TEST(generics_reject_a_type_parameter_that_shadows_a_struct) {
    EMBER_CHECK_EQ(first_error("struct T { pub x: int, }\n"
                               "pub fn f<T>(a: T) -> T { return a; }\n"),
                   std::string{"type parameter `T` shadows a type"});
}

// ---------------------------------------------------------------------
// Generic `impl` blocks and generic methods
//
// `impl<T> Pair<T>` needs no inference for `T`: the receiver is a
// `Pair<int>`, so `T` is `int` and there is nothing to work out. A
// method's *own* parameters are a different matter and are inferred from
// the arguments, exactly as a free function's are.
// ---------------------------------------------------------------------

namespace {

const char* const kPair =
    "struct Pair<T> { pub left: T, pub right: T, }\n"
    "impl<T> Pair<T> {\n"
    "    pub fn first(self) -> T { return self.left; }\n"
    "    pub fn swapped(self) -> Pair<T> {\n"
    "        return Pair { left: self.right, right: self.left };\n"
    "    }\n"
    "}\n";

}  // namespace

EMBER_TEST(generic_impl_takes_its_type_from_the_receiver) {
    accept(std::string{kPair} +
           "pub fn main() {\n"
           "    let p = Pair { left: 3, right: 7 };\n"
           "    println(p.first());\n"
           "}\n");
}

EMBER_TEST(generic_impl_serves_every_instantiation) {
    accept(std::string{kPair} +
           "pub fn main() {\n"
           "    println(Pair { left: 3, right: 7 }.first());\n"
           "    println(Pair { left: 1.5, right: 2.5 }.first());\n"
           "    println(Pair { left: true, right: false }.first());\n"
           "}\n");
}

EMBER_TEST(generic_impl_method_may_return_its_own_type) {
    accept(std::string{kPair} +
           "pub fn main() {\n"
           "    let p = Pair { left: 3, right: 7 };\n"
           "    println(p.swapped().first());\n"
           "}\n");
}

EMBER_TEST(generic_impl_reports_a_return_of_the_wrong_type) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject("struct Pair<T> { pub left: T, }\n"
               "impl<T> Pair<T> { pub fn first(self) -> T { return 1; } }\n"
               "pub fn main() { println(Pair { left: 1.5 }.first()); }\n");
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"expected `float`, found `int`"});
}

EMBER_TEST(generic_method_infers_its_own_parameters_from_the_call) {
    // On a struct with no parameters of its own, so the only thing to
    // infer is the method's.
    accept("struct Counter { pub n: int, }\n"
           "impl Counter {\n"
           "    pub fn bigger_of<T>(self, a: T, b: T) -> T {\n"
           "        if self.n > 0 { return a; }\n"
           "        return b;\n"
           "    }\n"
           "}\n"
           "pub fn main() {\n"
           "    let c = Counter { n: 1 };\n"
           "    println(c.bigger_of(10, 20));\n"
           "    println(c.bigger_of(1.5, 2.5));\n"
           "}\n");
}

EMBER_TEST(generic_method_may_add_parameters_to_a_generic_impl) {
    accept("struct Pair<T> { pub left: T, }\n"
           "impl<T> Pair<T> {\n"
           "    pub fn tagged<U>(self, tag: U) -> U { return tag; }\n"
           "}\n"
           "pub fn main() {\n"
           "    println(Pair { left: 1 }.tagged(\"a\"));\n"
           "}\n");
}

EMBER_TEST(generic_method_reports_a_parameter_it_cannot_infer) {
    EMBER_CHECK_EQ(first_error("struct Counter { pub n: int, }\n"
                               "impl Counter { pub fn make<T>(self) -> T { return self.n; } }\n"),
                   std::string{"type parameter `T` cannot be inferred"});
}

EMBER_TEST(generic_method_reports_one_parameter_asked_to_be_two_things) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject("struct Counter { pub n: int, }\n"
               "impl Counter { pub fn same<T>(self, a: T, b: T) -> T { return a; } }\n"
               "pub fn main() { println(Counter { n: 1 }.same(1, 1.5)); }\n");
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `float`"});
}

EMBER_TEST(generic_impl_refuses_a_parameter_shadowing_the_blocks) {
    EMBER_CHECK_EQ(first_error("struct Pair<T> { pub left: T, }\n"
                               "impl<T> Pair<T> { pub fn f<T>(self) -> T { return self.left; } }\n"),
                   std::string{"type parameter `T` shadows the `impl` block's"});
}

EMBER_TEST(generic_impl_requires_the_type_parameters_to_line_up) {
    EMBER_CHECK_EQ(first_error("struct Pair<T, U> { pub left: T, pub right: U, }\n"
                               "impl<T> Pair<T> { pub fn f(self) -> T { return self.left; } }\n"),
                   std::string{"`Pair` takes 2 type parameters but this `impl` declares 1"});
}

EMBER_TEST(generic_impl_on_a_type_that_is_not_generic_says_so) {
    EMBER_CHECK_EQ(first_error("struct Point { pub x: int, }\n"
                               "impl<T> Point<T> { pub fn f(self) -> int { return self.x; } }\n"),
                   std::string{"`Point` is not generic"});
}

EMBER_TEST(plain_impl_on_a_generic_type_says_what_to_write) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject("struct Pair<T> { pub left: T, }\n"
               "impl Pair { pub fn f(self) -> int { return 1; } }\n");
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"`Pair` is generic"});
    EMBER_CHECK_MSG(errors.at(0).notes.at(0).find("impl<T> Pair<T>") != std::string::npos,
                    errors.at(0).notes.at(0));
}

EMBER_TEST(generic_impl_methods_mangle_one_symbol_per_instantiation) {
    if (!ember::codegen::is_available()) {
        return;
    }
    // Monomorphization made concrete: two receivers, two functions, no
    // boxing and no dispatch.
    const std::string ir = compile_ir(std::string{kPair} +
                                      "pub fn main() {\n"
                                      "    println(Pair { left: 3, right: 7 }.first());\n"
                                      "    println(Pair { left: 1.5, right: 2.5 }.first());\n"
                                      "}\n");
    EMBER_CHECK_MSG(ir.find("@Pair_first__int(") != std::string::npos, ir);
    EMBER_CHECK_MSG(ir.find("@Pair_first__float(") != std::string::npos, ir);
}

// ---------------------------------------------------------------------
// Monomorphization reaches the object code
// ---------------------------------------------------------------------

EMBER_TEST(generics_emit_one_function_per_instantiation) {
    if (!ember::codegen::is_available()) {
        return;
    }
    const std::string ir = compile_ir(std::string{kMax} +
                                      "pub fn main() {\n"
                                      "    println(max(1, 2));\n"
                                      "    println(max(1.5, 2.5));\n"
                                      "}\n");

    EMBER_CHECK_MSG(ir.find("@max__int") != std::string::npos, "no `max__int` in:\n" + ir);
    EMBER_CHECK_MSG(ir.find("@max__float") != std::string::npos, "no `max__float` in:\n" + ir);
    // No `max` survives unmangled: a template is never emitted as itself.
    EMBER_CHECK_MSG(ir.find("define i64 @max(") == std::string::npos,
                    "an uninstantiated template was emitted");
}

EMBER_TEST(generics_emit_native_types_with_no_boxing) {
    if (!ember::codegen::is_available()) {
        return;
    }
    const std::string ir = compile_ir(std::string{kMax} +
                                      "pub fn main() { println(max(1, 2)); }\n");
    // The int instantiation works on i64 directly - the point of
    // monomorphizing rather than boxing. The linkage is `linkonce_odr`
    // because separate compilation emits each copy into every object
    // that demands it; see separate_tests.cpp.
    EMBER_CHECK_MSG(ir.find("define linkonce_odr i64 @max__int(i64") != std::string::npos,
                    "expected a native i64 signature in:\n" + ir);
}

EMBER_TEST(generics_emit_a_separate_struct_layout_per_instantiation) {
    if (!ember::codegen::is_available()) {
        return;
    }
    const std::string ir = compile_ir("struct Pair<T> { pub a: T, pub b: T, }\n"
                                      "pub fn left<T>(p: Pair<T>) -> T { return p.a; }\n"
                                      "pub fn main() {\n"
                                      "    let i: Pair<int> = Pair { a: 1, b: 2 };\n"
                                      "    let f: Pair<float> = Pair { a: 1.5, b: 2.5 };\n"
                                      "    println(left(i));\n"
                                      "    println(left(f));\n"
                                      "}\n");

    EMBER_CHECK_MSG(ir.find("{ i64, i64 }") != std::string::npos,
                    "no int layout for Pair in:\n" + ir);
    EMBER_CHECK_MSG(ir.find("{ double, double }") != std::string::npos,
                    "no float layout for Pair in:\n" + ir);
}

EMBER_TEST(generics_leave_a_program_without_generics_unchanged) {
    // The instance machinery must not disturb ordinary code: everything
    // outside a template still lives in the root instance.
    const SourceFile source = make_source("pub fn main() { println(1 + 2); }\n");
    const CheckResult result = check(source);
    EMBER_CHECK(result.ok());
    EMBER_CHECK_EQ(result.instantiations.size(), std::size_t{0});
}
