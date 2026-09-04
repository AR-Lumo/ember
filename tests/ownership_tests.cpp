// Dynamic arrays, growable strings, and the ownership model.
//
// The model in one line: a value whose type owns heap memory moves
// rather than copies, cannot be used after it moves, and is freed when
// its owner goes out of scope. There is no borrow checker — `&T` still
// borrows without moving and is still unchecked — so none of this needs
// lifetimes.

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
// Vec: the operations
// ---------------------------------------------------------------------

EMBER_TEST(vec_supports_push_len_index_and_pop) {
    accept(in_main("let mut v: Vec<int> = new_vec();\n"
                   "    push(v, 1);\n"
                   "    push(v, 2);\n"
                   "    println(len(v));\n"
                   "    println(v[0]);\n"
                   "    println(pop(v));"));
}

EMBER_TEST(vec_elements_are_assignable) {
    accept(in_main("let mut v: Vec<int> = new_vec();\n"
                   "    push(v, 1);\n"
                   "    v[0] = 5;\n"
                   "    println(v[0]);"));
}

EMBER_TEST(vec_takes_its_element_type_from_the_annotation) {
    // There is no turbofish, and `new_vec()` has no argument to infer
    // from, so the binding's annotation is the only thing that can say.
    EMBER_CHECK_EQ(first_error(in_main("let mut v = new_vec();")),
                   std::string{"cannot infer the element type of this `Vec`"});
}

EMBER_TEST(vec_rejects_pushing_the_wrong_element_type) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject(in_main("let mut v: Vec<int> = new_vec();\n    push(v, \"no\");"));
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `string`"});
}

EMBER_TEST(vec_rejects_pushing_onto_a_fixed_array) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject(in_main("let mut xs: [int; 2] = [1, 2];\n    push(xs, 3);"));
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"cannot push on `[int; 2]`"});
    EMBER_CHECK_MSG(errors.at(0).notes.at(0).find("fixed length") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
}

EMBER_TEST(vec_rejects_pushing_to_an_immutable_binding) {
    EMBER_CHECK_EQ(first_error(in_main("let v: Vec<int> = new_vec();\n    push(v, 1);")),
                   std::string{"cannot modify immutable binding `v`"});
}

EMBER_TEST(vec_allows_pushing_through_a_reference_parameter) {
    // The container belongs to the caller, who declared it `mut`; a
    // borrow of it is writable for the same reason assignment through a
    // reference is.
    accept("pub fn fill(v: &Vec<int>) {\n"
           "    push(v, 1);\n"
           "}\n"
           "pub fn main() {\n"
           "    let mut v: Vec<int> = new_vec();\n"
           "    fill(v);\n"
           "    println(len(v));\n"
           "}\n");
}

EMBER_TEST(vec_composes_with_generics) {
    accept("pub fn first_or<T>(v: &Vec<T>, fallback: T) -> T {\n"
           "    if len(v) > 0 {\n"
           "        return v[0];\n"
           "    }\n"
           "    return fallback;\n"
           "}\n"
           "pub fn main() {\n"
           "    let mut a: Vec<int> = new_vec();\n"
           "    let mut b: Vec<float> = new_vec();\n"
           "    println(first_or(a, 0));\n"
           "    println(first_or(b, 1.5));\n"
           "}\n");
}

EMBER_TEST(vec_rejects_an_element_type_that_owns_memory) {
    // Dropping a `Vec` frees its buffer, not each element. An element
    // owning memory of its own would be leaked, so it is refused rather
    // than quietly lost. Reported once, not twice.
    const std::vector<ember::ast::Diagnostic> errors =
        reject(in_main("let mut v: Vec<Vec<int>> = new_vec();"));
    EMBER_CHECK_EQ(errors.size(), std::size_t{1});
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"`Vec<Vec<int>>` is not supported"});
}

EMBER_TEST(vec_accepts_an_array_of_vectors) {
    // A `[Vec<int>; 2]` is fine: an array is laid out inline, so drop
    // walks its elements and frees each one.
    accept(in_main("let mut a: Vec<int> = new_vec();\n"
                   "    let mut b: Vec<int> = new_vec();\n"
                   "    push(a, 1);\n"
                   "    push(b, 2);\n"
                   "    let pair: [Vec<int>; 2] = [a, b];\n"
                   "    println(len(pair[0]));"));
}

EMBER_TEST(vec_rejects_wrong_type_argument_count) {
    EMBER_CHECK_EQ(first_error("pub fn f(v: Vec<int, float>) { }\n"),
                   std::string{"`Vec` takes 1 type argument but 2 were given"});
}

// ---------------------------------------------------------------------
// String
// ---------------------------------------------------------------------

EMBER_TEST(string_supports_append_len_and_printing) {
    accept(in_main("let mut s: String = new_string();\n"
                   "    push_str(s, \"hello\");\n"
                   "    push_str(s, \" world\");\n"
                   "    println(s);\n"
                   "    println(len(s));"));
}

EMBER_TEST(string_accepts_another_string_buffer) {
    accept(in_main("let mut a: String = new_string();\n"
                   "    let mut b: String = new_string();\n"
                   "    push_str(b, \"x\");\n"
                   "    push_str(a, b);\n"
                   "    println(a);"));
}

EMBER_TEST(string_is_distinct_from_the_string_view) {
    // `String` owns a buffer; `string` is a borrowed fixed-length view.
    // Passing one where the other is wanted is a type error.
    const std::vector<ember::ast::Diagnostic> errors =
        reject("pub fn take(s: string) { }\n" +
               in_main("let mut s: String = new_string();\n    take(s);"));
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"expected `string`, found `String`"});
}

EMBER_TEST(string_rejects_appending_to_a_view) {
    EMBER_CHECK_EQ(first_error(in_main("let s = \"fixed\";\n    push_str(s, \"more\");")),
                   std::string{"cannot push text onto `string`"});
}

// ---------------------------------------------------------------------
// Ownership: moves
// ---------------------------------------------------------------------

EMBER_TEST(ownership_rejects_using_a_value_after_it_moves) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject(in_main("let mut a: Vec<int> = new_vec();\n"
                       "    let b = a;\n"
                       "    println(len(a));"));
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"use of moved value `a`"});
    EMBER_CHECK_MSG(errors.at(0).notes.at(0).find("moved at") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
    EMBER_CHECK_MSG(errors.at(0).notes.at(1).find("owns heap memory") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(1));
}

EMBER_TEST(ownership_moves_a_value_passed_by_value) {
    EMBER_CHECK_EQ(first_error("pub fn consume(v: Vec<int>) { }\n" +
                               in_main("let mut v: Vec<int> = new_vec();\n"
                                       "    consume(v);\n"
                                       "    println(len(v));")),
                   std::string{"use of moved value `v`"});
}

EMBER_TEST(ownership_does_not_move_a_value_passed_by_reference) {
    // A borrow is the whole reason `&T` still exists.
    accept("pub fn look(v: &Vec<int>) -> int { return len(v); }\n" +
           in_main("let mut v: Vec<int> = new_vec();\n"
                   "    println(look(v));\n"
                   "    println(len(v));"));
}

EMBER_TEST(ownership_moves_a_returned_value_out_of_its_function) {
    accept("pub fn build() -> Vec<int> {\n"
           "    let mut v: Vec<int> = new_vec();\n"
           "    push(v, 1);\n"
           "    return v;\n"
           "}\n" +
           in_main("let made = build();\n    println(len(made));"));
}

EMBER_TEST(ownership_moves_a_value_into_a_struct) {
    EMBER_CHECK_EQ(first_error("struct Bag { pub items: Vec<int>, }\n" +
                               in_main("let mut v: Vec<int> = new_vec();\n"
                                       "    let b = Bag { items: v };\n"
                                       "    println(len(v));")),
                   std::string{"use of moved value `v`"});
}

EMBER_TEST(ownership_makes_a_struct_holding_an_owned_field_owned_too) {
    EMBER_CHECK_EQ(first_error("struct Bag { pub items: Vec<int>, }\n"
                               "pub fn take(b: Bag) { }\n" +
                               in_main("let mut v: Vec<int> = new_vec();\n"
                                       "    let b = Bag { items: v };\n"
                                       "    take(b);\n"
                                       "    println(len(b.items));")),
                   std::string{"use of moved value `b`"});
}

EMBER_TEST(ownership_allows_reassigning_a_moved_binding) {
    // Assigning back gives the variable a value again, so it is usable.
    accept(in_main("let mut a: Vec<int> = new_vec();\n"
                   "    let b = a;\n"
                   "    a = new_vec();\n"
                   "    push(a, 1);\n"
                   "    println(len(a));"));
}

EMBER_TEST(ownership_rejects_moving_out_of_a_field_or_element) {
    // Moving a field out would leave the struct half-owned, with no way
    // for the drop code to know which parts are still live.
    EMBER_CHECK_EQ(first_error("struct Bag { pub items: Vec<int>, }\n"
                               "pub fn take(v: Vec<int>) { }\n" +
                               in_main("let mut v: Vec<int> = new_vec();\n"
                                       "    let b = Bag { items: v };\n"
                                       "    take(b.items);")),
                   std::string{"cannot move out of `Vec<int>` here"});
}

EMBER_TEST(ownership_leaves_copyable_types_alone) {
    // Nothing about the ownership model touches types with no heap
    // behind them: they still copy freely.
    accept(in_main("let a = 1;\n"
                   "    let b = a;\n"
                   "    let xs: [int; 2] = [1, 2];\n"
                   "    let ys = xs;\n"
                   "    let s = \"text\";\n"
                   "    let t = s;\n"
                   "    println(a + b + xs[0] + ys[1]);\n"
                   "    println(t);"));
}

// ---------------------------------------------------------------------
// Ownership: what codegen emits
// ---------------------------------------------------------------------

EMBER_TEST(ownership_emits_a_free_for_a_scoped_vec) {
    if (!ember::codegen::is_available()) {
        return;
    }
    const std::string ir = compile_ir(in_main("let mut v: Vec<int> = new_vec();\n"
                                              "    push(v, 1);\n"
                                              "    println(len(v));"));
    EMBER_CHECK_MSG(ir.find("ember_free") != std::string::npos,
                    "no free emitted for a scoped Vec:\n" + ir);
}

EMBER_TEST(ownership_emits_a_drop_flag_for_a_conditional_move) {
    if (!ember::codegen::is_available()) {
        return;
    }
    // Moved on one path only, so whether to free is a runtime question.
    const std::string ir = compile_ir("pub fn consume(v: Vec<int>) { }\n" +
                                      in_main("let mut v: Vec<int> = new_vec();\n"
                                              "    if len(v) > 0 {\n"
                                              "        consume(v);\n"
                                              "    }"));
    EMBER_CHECK_MSG(ir.find(".live") != std::string::npos,
                    "no drop flag emitted for a conditional move:\n" + ir);
    EMBER_CHECK_MSG(ir.find("ember_free") != std::string::npos, "no free emitted");
}

EMBER_TEST(ownership_emits_no_free_for_a_program_with_no_owned_values) {
    if (!ember::codegen::is_available()) {
        return;
    }
    const std::string ir = compile_ir(in_main("let xs: [int; 2] = [1, 2];\n    println(xs[0]);"));
    EMBER_CHECK_MSG(ir.find("ember_free") == std::string::npos,
                    "a program with no heap values should emit no frees:\n" + ir);
}

EMBER_TEST(ownership_grows_geometrically_rather_than_per_push) {
    if (!ember::codegen::is_available()) {
        return;
    }
    // The growth decision lives in the runtime, so a push is one call
    // rather than an inline realloc every time.
    const std::string ir = compile_ir(in_main("let mut v: Vec<int> = new_vec();\n"
                                              "    push(v, 1);"));
    EMBER_CHECK_MSG(ir.find("ember_grow") != std::string::npos,
                    "push should go through ember_grow:\n" + ir);
}
