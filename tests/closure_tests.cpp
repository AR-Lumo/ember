// Closures: function values, capture by value, and how they sit inside
// the ownership model.
//
// A closure is an owned value like a `Vec`, because a capturing one
// holds a heap environment. Calling it is a use rather than a move, so
// it can be called repeatedly; passing it by value moves it.

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

std::string in_main(const std::string& body) {
    return "pub fn main() {\n    " + body + "\n}\n";
}

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

}  // namespace

// ---------------------------------------------------------------------
// Writing and calling a closure
// ---------------------------------------------------------------------

EMBER_TEST(closures_can_be_declared_and_called) {
    accept(in_main("let double = |x: int| -> int { return x * 2; };\n"
                   "    println(double(21));"));
}

EMBER_TEST(closures_take_no_parameters_when_written_with_empty_bars) {
    // `||` is one token, so an empty parameter list has to be told apart
    // from a logical or by where it appears.
    accept(in_main("let answer = || -> int { return 42; };\n    println(answer());"));
}

EMBER_TEST(closures_may_return_nothing) {
    accept(in_main("let shout = |text: string| { println(text); };\n    shout(\"hi\");"));
}

EMBER_TEST(closures_still_allow_logical_or) {
    // The `||` token has two jobs now; this is the other one.
    accept(in_main("let a = true || false;\n    println(a);"));
}

EMBER_TEST(closures_can_be_called_more_than_once) {
    // Calling reads the closure rather than consuming it.
    accept(in_main("let f = |x: int| -> int { return x; };\n"
                   "    println(f(1));\n"
                   "    println(f(2));"));
}

EMBER_TEST(closures_have_a_function_type) {
    accept("pub fn apply(f: &fn(int) -> int, x: int) -> int { return f(x); }\n" +
           in_main("let double = |x: int| -> int { return x * 2; };\n"
                   "    println(apply(double, 4));"));
}

EMBER_TEST(closures_can_be_returned_from_a_function) {
    // The capture is copied into the closure, so it outlives the
    // function that built it.
    accept("pub fn scaler(factor: int) -> fn(int) -> int {\n"
           "    return |x: int| -> int { return x * factor; };\n"
           "}\n" +
           in_main("let triple = scaler(3);\n    println(triple(14));"));
}

EMBER_TEST(closures_may_take_no_arguments_in_their_type) {
    accept("pub fn run(f: &fn() -> int) -> int { return f(); }\n" +
           in_main("let answer = || -> int { return 42; };\n    println(run(answer));"));
}

// ---------------------------------------------------------------------
// Captures
// ---------------------------------------------------------------------

EMBER_TEST(closures_capture_a_local_by_value) {
    accept(in_main("let offset = 100;\n"
                   "    let shift = |x: int| -> int { return x + offset; };\n"
                   "    println(shift(5));"));
}

EMBER_TEST(closures_record_only_what_the_body_mentions) {
    const SourceFile source = make_source(in_main(
        "let used = 1;\n"
        "    let unused = 2;\n"
        "    let f = |x: int| -> int { return x + used; };\n"
        "    println(f(0) + unused);"));
    const CheckResult result = check(source);
    EMBER_CHECK(result.ok());

    // Exactly one capture, and it is the one the body used.
    EMBER_CHECK_EQ(result.closure_captures.size(), std::size_t{1});
    EMBER_CHECK_EQ(result.closure_captures.begin()->second.size(), std::size_t{1});
}

EMBER_TEST(closures_do_not_capture_their_own_parameters) {
    const SourceFile source =
        make_source(in_main("let f = |x: int, y: int| -> int { return x + y; };\n"
                            "    println(f(1, 2));"));
    const CheckResult result = check(source);
    EMBER_CHECK(result.ok());
    EMBER_CHECK_EQ(result.closure_captures.begin()->second.size(), std::size_t{0});
}

EMBER_TEST(closures_do_not_capture_their_own_locals) {
    const SourceFile source = make_source(
        in_main("let f = |x: int| -> int {\n"
                "        let scratch = 10;\n"
                "        return x + scratch;\n"
                "    };\n"
                "    println(f(1));"));
    const CheckResult result = check(source);
    EMBER_CHECK(result.ok());
    EMBER_CHECK_EQ(result.closure_captures.begin()->second.size(), std::size_t{0});
}

EMBER_TEST(closures_capture_through_nesting) {
    // A name from two scopes out is captured by both closures, so the
    // inner one can still see it.
    accept(in_main("let base = 10;\n"
                   "    let outer = |x: int| -> int {\n"
                   "        let inner = |y: int| -> int { return y + base; };\n"
                   "        return inner(x);\n"
                   "    };\n"
                   "    println(outer(5));"));
}

EMBER_TEST(closures_reject_capturing_an_owned_value) {
    // A closure frees its environment as one block and has no per-
    // closure code to drop what is inside it, so an owned capture would
    // leak. Refused rather than lost.
    const std::vector<ember::ast::Diagnostic> errors =
        reject(in_main("let mut v: Vec<int> = new_vec();\n"
                       "    let f = || -> int { return len(v); };\n"
                       "    println(f());"));
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"cannot capture `v` in a closure"});
    EMBER_CHECK_MSG(errors.at(0).notes.at(0).find("as one block") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
}

// ---------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------

EMBER_TEST(closures_report_a_wrong_argument_type) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject(in_main("let f = |x: int| -> int { return x; };\n    println(f(true));"));
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `bool`"});
}

EMBER_TEST(closures_report_a_wrong_argument_count) {
    EMBER_CHECK_EQ(first_error(in_main("let f = |x: int| -> int { return x; };\n"
                                       "    println(f(1, 2));")),
                   std::string{"this function takes 1 argument but 2 were supplied"});
}

EMBER_TEST(closures_report_a_missing_return) {
    EMBER_CHECK_EQ(first_error(in_main("let f = |x: int| -> int { println(x); };")),
                   std::string{"missing return"});
}

EMBER_TEST(closures_report_a_body_returning_the_wrong_type) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject(in_main("let f = |x: int| -> int { return true; };"));
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `bool`"});
}

EMBER_TEST(closures_reject_calling_a_non_function_value) {
    EMBER_CHECK_EQ(first_error(in_main("let x = 1;\n    println(x(2));")),
                   std::string{"`x` is not callable"});
}

EMBER_TEST(closures_reject_a_type_mismatch_against_a_function_type) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject("pub fn take(f: &fn(int) -> int) { }\n" +
               in_main("let f = |x: bool| -> bool { return x; };\n    take(f);"));
    EMBER_CHECK_EQ(errors.at(0).label,
                   std::string{"expected `&fn(int) -> int`, found `fn(bool) -> bool`"});
}

// ---------------------------------------------------------------------
// Closures are owned values
// ---------------------------------------------------------------------

EMBER_TEST(closures_move_when_passed_by_value) {
    EMBER_CHECK_EQ(first_error("pub fn consume(f: fn(int) -> int) { }\n" +
                               in_main("let f = |x: int| -> int { return x; };\n"
                                       "    consume(f);\n"
                                       "    println(f(1));")),
                   std::string{"use of moved value `f`"});
}

EMBER_TEST(closures_do_not_move_when_borrowed) {
    accept("pub fn look(f: &fn(int) -> int) -> int { return f(1); }\n" +
           in_main("let f = |x: int| -> int { return x; };\n"
                   "    println(look(f));\n"
                   "    println(f(2));"));
}

EMBER_TEST(closures_move_on_assignment) {
    EMBER_CHECK_EQ(first_error(in_main("let f = |x: int| -> int { return x; };\n"
                                       "    let g = f;\n"
                                       "    println(f(1));")),
                   std::string{"use of moved value `f`"});
}

// ---------------------------------------------------------------------
// What codegen emits
// ---------------------------------------------------------------------

EMBER_TEST(closures_lift_to_real_functions) {
    if (!ember::codegen::is_available()) {
        return;
    }
    const std::string ir = compile_ir(in_main("let f = |x: int| -> int { return x * 2; };\n"
                                              "    println(f(21));"));
    EMBER_CHECK_MSG(ir.find("ember_closure_") != std::string::npos,
                    "no lifted closure function in:\n" + ir);
}

EMBER_TEST(closures_with_no_captures_allocate_nothing) {
    if (!ember::codegen::is_available()) {
        return;
    }
    // A capture-free closure has a null environment, so building one
    // touches the heap not at all.
    const std::string ir = compile_ir(in_main("let f = |x: int| -> int { return x; };\n"
                                              "    println(f(1));"));
    EMBER_CHECK_MSG(ir.find("ember_alloc") == std::string::npos,
                    "a capture-free closure should not allocate:\n" + ir);
}

EMBER_TEST(closures_with_captures_allocate_and_free_an_environment) {
    if (!ember::codegen::is_available()) {
        return;
    }
    const std::string ir = compile_ir(in_main("let n = 5;\n"
                                              "    let f = |x: int| -> int { return x + n; };\n"
                                              "    println(f(1));"));
    EMBER_CHECK_MSG(ir.find("ember_alloc") != std::string::npos,
                    "a capturing closure should allocate its environment:\n" + ir);
    EMBER_CHECK_MSG(ir.find("ember_free") != std::string::npos,
                    "a capturing closure should free its environment:\n" + ir);
}
