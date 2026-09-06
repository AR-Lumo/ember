// Closures: function values, capture by value, and how they sit inside
// the ownership model.
//
// A closure is an owned value like a `Vec`, because a capturing one
// holds a heap environment. Calling it is a use rather than a move, so
// it can be called repeatedly; passing it by value moves it.

#include "test_harness.hpp"

#include "soliton/ast/diagnostic.hpp"
#include "soliton/ast/nodes.hpp"
#include "soliton/codegen/codegen.hpp"
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

CheckResult check(const SourceFile& source) {
    soliton::parser::ParseResult parsed = soliton::parser::parse_source(source);
    if (!parsed.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "test fixture does not parse:\n" +
                                soliton::ast::render_all(parsed.diagnostics, source));
    }
    return soliton::typeck::check(*parsed.program, source);
}

void accept(const std::string& contents) {
    const SourceFile source = make_source(contents);
    const CheckResult result = check(source);
    if (!result.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "expected this program to type-check, but:\n" +
                                soliton::ast::render_all(result.diagnostics, source));
    }
}

std::vector<soliton::ast::Diagnostic> reject(const std::string& contents) {
    const SourceFile source = make_source(contents);
    CheckResult result = check(source);
    if (result.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "expected this program to be rejected, but it type-checked");
    }
    return std::move(result.diagnostics);
}

std::string first_error(const std::string& contents) { return reject(contents).at(0).message; }

std::string in_main(const std::string& body) {
    return "pub fn main() {\n    " + body + "\n}\n";
}

std::string compile_ir(const std::string& contents) {
    const SourceFile source = make_source(contents);
    const soliton::parser::ParseResult parsed = soliton::parser::parse_source(source);
    const CheckResult checked = soliton::typeck::check(*parsed.program, source);
    if (!checked.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "fixture does not type-check:\n" +
                                soliton::ast::render_all(checked.diagnostics, source));
    }
    const soliton::codegen::CompileResult compiled =
        soliton::codegen::compile_to_string(*parsed.program, checked, source);
    if (!compiled.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__, "codegen failed");
    }
    return compiled.assembly;
}

}  // namespace

// ---------------------------------------------------------------------
// Writing and calling a closure
// ---------------------------------------------------------------------

SOLITON_TEST(closures_can_be_declared_and_called) {
    accept(in_main("let double = |x: int| -> int { return x * 2; };\n"
                   "    println(double(21));"));
}

SOLITON_TEST(closures_take_no_parameters_when_written_with_empty_bars) {
    // `||` is one token, so an empty parameter list has to be told apart
    // from a logical or by where it appears.
    accept(in_main("let answer = || -> int { return 42; };\n    println(answer());"));
}

SOLITON_TEST(closures_may_return_nothing) {
    accept(in_main("let shout = |text: string| { println(text); };\n    shout(\"hi\");"));
}

SOLITON_TEST(closures_still_allow_logical_or) {
    // The `||` token has two jobs now; this is the other one.
    accept(in_main("let a = true || false;\n    println(a);"));
}

SOLITON_TEST(closures_can_be_called_more_than_once) {
    // Calling reads the closure rather than consuming it.
    accept(in_main("let f = |x: int| -> int { return x; };\n"
                   "    println(f(1));\n"
                   "    println(f(2));"));
}

SOLITON_TEST(closures_have_a_function_type) {
    accept("pub fn apply(f: &fn(int) -> int, x: int) -> int { return f(x); }\n" +
           in_main("let double = |x: int| -> int { return x * 2; };\n"
                   "    println(apply(double, 4));"));
}

SOLITON_TEST(closures_can_be_returned_from_a_function) {
    // The capture is copied into the closure, so it outlives the
    // function that built it.
    accept("pub fn scaler(factor: int) -> fn(int) -> int {\n"
           "    return |x: int| -> int { return x * factor; };\n"
           "}\n" +
           in_main("let triple = scaler(3);\n    println(triple(14));"));
}

SOLITON_TEST(closures_may_take_no_arguments_in_their_type) {
    accept("pub fn run(f: &fn() -> int) -> int { return f(); }\n" +
           in_main("let answer = || -> int { return 42; };\n    println(run(answer));"));
}

// ---------------------------------------------------------------------
// Captures
// ---------------------------------------------------------------------

SOLITON_TEST(closures_capture_a_local_by_value) {
    accept(in_main("let offset = 100;\n"
                   "    let shift = |x: int| -> int { return x + offset; };\n"
                   "    println(shift(5));"));
}

SOLITON_TEST(closures_record_only_what_the_body_mentions) {
    const SourceFile source = make_source(in_main(
        "let used = 1;\n"
        "    let unused = 2;\n"
        "    let f = |x: int| -> int { return x + used; };\n"
        "    println(f(0) + unused);"));
    const CheckResult result = check(source);
    SOLITON_CHECK(result.ok());

    // Exactly one capture, and it is the one the body used.
    SOLITON_CHECK_EQ(result.closure_captures.size(), std::size_t{1});
    SOLITON_CHECK_EQ(result.closure_captures.begin()->second.size(), std::size_t{1});
}

SOLITON_TEST(closures_do_not_capture_their_own_parameters) {
    const SourceFile source =
        make_source(in_main("let f = |x: int, y: int| -> int { return x + y; };\n"
                            "    println(f(1, 2));"));
    const CheckResult result = check(source);
    SOLITON_CHECK(result.ok());
    SOLITON_CHECK_EQ(result.closure_captures.begin()->second.size(), std::size_t{0});
}

SOLITON_TEST(closures_do_not_capture_their_own_locals) {
    const SourceFile source = make_source(
        in_main("let f = |x: int| -> int {\n"
                "        let scratch = 10;\n"
                "        return x + scratch;\n"
                "    };\n"
                "    println(f(1));"));
    const CheckResult result = check(source);
    SOLITON_CHECK(result.ok());
    SOLITON_CHECK_EQ(result.closure_captures.begin()->second.size(), std::size_t{0});
}

SOLITON_TEST(closures_capture_through_nesting) {
    // A name from two scopes out is captured by both closures, so the
    // inner one can still see it.
    accept(in_main("let base = 10;\n"
                   "    let outer = |x: int| -> int {\n"
                   "        let inner = |y: int| -> int { return y + base; };\n"
                   "        return inner(x);\n"
                   "    };\n"
                   "    println(outer(5));"));
}

SOLITON_TEST(closures_capture_an_owned_value_by_taking_it) {
    accept(in_main("let mut v: Vec<int> = new_vec();\n"
                   "    push(v, 1);\n"
                   "    let f = || -> int { return len(v); };\n"
                   "    println(f());\n"
                   "    println(f());"));
}

SOLITON_TEST(closures_move_an_owned_capture_out_of_the_enclosing_scope) {
    // Capturing by value means taking it. The closure owns it now, so
    // the scope that had it does not.
    SOLITON_CHECK_EQ(first_error(in_main("let mut v: Vec<int> = new_vec();\n"
                                       "    let f = || -> int { return len(v); };\n"
                                       "    println(len(v));")),
                   std::string{"use of moved value `v`"});
}

SOLITON_TEST(closures_do_not_move_a_capture_that_copies) {
    // An `int` is copied into the environment, so the original is still
    // there afterwards.
    accept(in_main("let n = 5;\n"
                   "    let f = || -> int { return n; };\n"
                   "    println(f());\n"
                   "    println(n);"));
}

SOLITON_TEST(closures_with_owned_captures_carry_a_drop_function) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // What is inside an environment cannot be worked out from the
    // closure's type - two closures of the same `fn() -> int` may
    // capture quite different things - so the closure carries its own
    // way of taking the environment apart.
    const std::string owned = compile_ir(in_main("let mut v: Vec<int> = new_vec();\n"
                                                 "    let f = || -> int { return len(v); };\n"
                                                 "    println(f());"));
    SOLITON_CHECK_MSG(owned.find("soliton_closure_drop_") != std::string::npos,
                    "no drop function for an owned capture:\n" + owned);

    // And a closure with nothing to drop carries none.
    const std::string plain = compile_ir(in_main("let n = 5;\n"
                                                 "    let f = || -> int { return n; };\n"
                                                 "    println(f());"));
    SOLITON_CHECK_MSG(plain.find("soliton_closure_drop_") == std::string::npos,
                    "a copyable capture needs no drop function:\n" + plain);
}

// ---------------------------------------------------------------------
// Parameter types that write themselves
//
// A function's parameters are never inferred, because a function has no
// context to take them from. A closure always has one: it is being
// passed to something, or bound to something, and that says what its
// parameters are.
// ---------------------------------------------------------------------

namespace {

const char* const kMap =
    "pub fn map_in_place(values: &Vec<int>, f: &fn(int) -> int) {\n"
    "    let mut i = 0;\n"
    "    while i < len(values) {\n"
    "        values[i] = f(values[i]);\n"
    "        i = i + 1;\n"
    "    }\n"
    "}\n";

}  // namespace

SOLITON_TEST(closures_take_their_parameter_types_from_what_they_are_passed_to) {
    accept(std::string{kMap} +
           in_main("let mut v: Vec<int> = new_vec();\n"
                   "    push(v, 1);\n"
                   "    map_in_place(v, |x| { return x * 2; });"));
}

SOLITON_TEST(closures_take_their_return_type_the_same_way) {
    // No `-> int` written, and the body returns one because the
    // parameter type said it must.
    accept("pub fn apply(f: &fn(int) -> int, value: int) -> int { return f(value); }\n" +
           in_main("println(apply(|x| { return x + 1; }, 41));"));
}

SOLITON_TEST(closures_still_accept_types_written_out) {
    accept(std::string{kMap} +
           in_main("let mut v: Vec<int> = new_vec();\n"
                   "    map_in_place(v, |x: int| -> int { return x * 2; });"));
}

SOLITON_TEST(closures_check_an_inferred_body_against_the_expected_type) {
    // The types came from the context, so the body has to honour them.
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(std::string{kMap} +
               in_main("let mut v: Vec<int> = new_vec();\n"
                       "    map_in_place(v, |x| { return true; });"));
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `bool`"});
}

SOLITON_TEST(closures_report_a_parameter_with_nothing_to_infer_from) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let f = |x| { println(x); };\n    f(1);"));
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"cannot work out the type of `x`"});
    SOLITON_CHECK_MSG(errors.at(0).notes.at(0).find("what it is passed to") != std::string::npos,
                    errors.at(0).notes.at(0));
}

// ---------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------

SOLITON_TEST(closures_report_a_wrong_argument_type) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let f = |x: int| -> int { return x; };\n    println(f(true));"));
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `bool`"});
}

SOLITON_TEST(closures_report_a_wrong_argument_count) {
    SOLITON_CHECK_EQ(first_error(in_main("let f = |x: int| -> int { return x; };\n"
                                       "    println(f(1, 2));")),
                   std::string{"this function takes 1 argument but 2 were supplied"});
}

SOLITON_TEST(closures_report_a_missing_return) {
    SOLITON_CHECK_EQ(first_error(in_main("let f = |x: int| -> int { println(x); };")),
                   std::string{"missing return"});
}

SOLITON_TEST(closures_report_a_body_returning_the_wrong_type) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let f = |x: int| -> int { return true; };"));
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `bool`"});
}

SOLITON_TEST(closures_reject_calling_a_non_function_value) {
    SOLITON_CHECK_EQ(first_error(in_main("let x = 1;\n    println(x(2));")),
                   std::string{"`x` is not callable"});
}

SOLITON_TEST(closures_reject_a_type_mismatch_against_a_function_type) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject("pub fn take(f: &fn(int) -> int) { }\n" +
               in_main("let f = |x: bool| -> bool { return x; };\n    take(f);"));
    SOLITON_CHECK_EQ(errors.at(0).label,
                   std::string{"expected `&fn(int) -> int`, found `fn(bool) -> bool`"});
}

// ---------------------------------------------------------------------
// Closures are owned values
// ---------------------------------------------------------------------

SOLITON_TEST(closures_move_when_passed_by_value) {
    SOLITON_CHECK_EQ(first_error("pub fn consume(f: fn(int) -> int) { }\n" +
                               in_main("let f = |x: int| -> int { return x; };\n"
                                       "    consume(f);\n"
                                       "    println(f(1));")),
                   std::string{"use of moved value `f`"});
}

SOLITON_TEST(closures_do_not_move_when_borrowed) {
    accept("pub fn look(f: &fn(int) -> int) -> int { return f(1); }\n" +
           in_main("let f = |x: int| -> int { return x; };\n"
                   "    println(look(f));\n"
                   "    println(f(2));"));
}

SOLITON_TEST(closures_move_on_assignment) {
    SOLITON_CHECK_EQ(first_error(in_main("let f = |x: int| -> int { return x; };\n"
                                       "    let g = f;\n"
                                       "    println(f(1));")),
                   std::string{"use of moved value `f`"});
}

// ---------------------------------------------------------------------
// What codegen emits
// ---------------------------------------------------------------------

SOLITON_TEST(closures_lift_to_real_functions) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const std::string ir = compile_ir(in_main("let f = |x: int| -> int { return x * 2; };\n"
                                              "    println(f(21));"));
    SOLITON_CHECK_MSG(ir.find("soliton_closure_") != std::string::npos,
                    "no lifted closure function in:\n" + ir);
}

SOLITON_TEST(closures_with_no_captures_allocate_nothing) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // A capture-free closure has a null environment, so building one
    // touches the heap not at all.
    const std::string ir = compile_ir(in_main("let f = |x: int| -> int { return x; };\n"
                                              "    println(f(1));"));
    SOLITON_CHECK_MSG(ir.find("soliton_alloc") == std::string::npos,
                    "a capture-free closure should not allocate:\n" + ir);
}

SOLITON_TEST(closures_with_captures_allocate_and_free_an_environment) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const std::string ir = compile_ir(in_main("let n = 5;\n"
                                              "    let f = |x: int| -> int { return x + n; };\n"
                                              "    println(f(1));"));
    SOLITON_CHECK_MSG(ir.find("soliton_alloc") != std::string::npos,
                    "a capturing closure should allocate its environment:\n" + ir);
    SOLITON_CHECK_MSG(ir.find("soliton_free") != std::string::npos,
                    "a capturing closure should free its environment:\n" + ir);
}
