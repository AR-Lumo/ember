// Dynamic arrays, growable strings, and the ownership model.
//
// The model in one line: a value whose type owns heap memory moves
// rather than copies, cannot be used after it moves, and is freed when
// its owner goes out of scope. There is no borrow checker — `&T` still
// borrows without moving and is still unchecked — so none of this needs
// lifetimes.

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
// Vec: the operations
// ---------------------------------------------------------------------

SOLITON_TEST(vec_supports_push_len_index_and_pop) {
    accept(in_main("let mut v: Vec<int> = new_vec();\n"
                   "    push(v, 1);\n"
                   "    push(v, 2);\n"
                   "    println(len(v));\n"
                   "    println(v[0]);\n"
                   "    println(pop(v));"));
}

SOLITON_TEST(vec_elements_are_assignable) {
    accept(in_main("let mut v: Vec<int> = new_vec();\n"
                   "    push(v, 1);\n"
                   "    v[0] = 5;\n"
                   "    println(v[0]);"));
}

SOLITON_TEST(vec_takes_its_element_type_from_the_annotation) {
    // There is no turbofish, and `new_vec()` has no argument to infer
    // from, so the binding's annotation is the only thing that can say.
    SOLITON_CHECK_EQ(first_error(in_main("let mut v = new_vec();")),
                   std::string{"cannot infer the element type of this `Vec`"});
}

SOLITON_TEST(vec_rejects_pushing_the_wrong_element_type) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let mut v: Vec<int> = new_vec();\n    push(v, \"no\");"));
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `string`"});
}

SOLITON_TEST(vec_rejects_pushing_onto_a_fixed_array) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let mut xs: [int; 2] = [1, 2];\n    push(xs, 3);"));
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"cannot push on `[int; 2]`"});
    SOLITON_CHECK_MSG(errors.at(0).notes.at(0).find("fixed length") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
}

SOLITON_TEST(vec_rejects_pushing_to_an_immutable_binding) {
    SOLITON_CHECK_EQ(first_error(in_main("let v: Vec<int> = new_vec();\n    push(v, 1);")),
                   std::string{"cannot modify immutable binding `v`"});
}

SOLITON_TEST(vec_allows_pushing_through_a_reference_parameter) {
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

SOLITON_TEST(vec_composes_with_generics) {
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

// ---------------------------------------------------------------------
// Vectors whose elements own memory
//
// Dropping one of these is not a single `free`: it walks the live
// elements, drops each, and only then releases the buffer under them.
// What keeps that sound is that an element can never be moved out of the
// middle - the vector would go on counting something it no longer holds.
// ---------------------------------------------------------------------

SOLITON_TEST(vec_accepts_an_element_type_that_owns_memory) {
    accept(in_main("let mut words: Vec<String> = new_vec();\n"
                   "    let mut w: String = new_string();\n"
                   "    push_str(w, \"soliton\");\n"
                   "    push(words, w);\n"
                   "    println(len(words));"));
}

SOLITON_TEST(vec_accepts_a_vector_of_vectors) {
    // The drop is recursive, so the nesting can go as deep as it likes.
    accept(in_main("let mut grid: Vec<Vec<int>> = new_vec();\n"
                   "    let mut row: Vec<int> = new_vec();\n"
                   "    push(row, 1);\n"
                   "    push(grid, row);\n"
                   "    println(len(grid));"));
}

SOLITON_TEST(vec_moves_a_pushed_value_into_the_container) {
    SOLITON_CHECK_EQ(first_error(in_main("let mut words: Vec<String> = new_vec();\n"
                                       "    let mut w: String = new_string();\n"
                                       "    push(words, w);\n"
                                       "    println(w);")),
                   std::string{"use of moved value `w`"});
}

SOLITON_TEST(vec_refuses_to_move_an_owned_element_out) {
    // The rule that makes the drop loop safe.
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let mut words: Vec<String> = new_vec();\n"
                       "    let taken = words[0];\n"
                       "    println(taken);"));
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"cannot move out of `String` here"});
}

SOLITON_TEST(vec_points_at_pop_when_an_element_cannot_be_moved_out) {
    // There is a way to do what they meant, so the diagnostic says so.
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let mut words: Vec<String> = new_vec();\n"
                       "    let taken = words[0];\n"
                       "    println(taken);"));
    bool mentions_pop = false;
    for (const std::string& note : errors.at(0).notes) {
        mentions_pop = mentions_pop || note.find("`pop`") != std::string::npos;
    }
    SOLITON_CHECK_MSG(mentions_pop, "no note pointing at `pop`");
}

SOLITON_TEST(vec_hands_ownership_back_through_pop) {
    // `pop` shortens the vector, so nothing is left half-owned and the
    // binding really does own what it got.
    accept(in_main("let mut words: Vec<String> = new_vec();\n"
                   "    let mut w: String = new_string();\n"
                   "    push(words, w);\n"
                   "    let taken = pop(words);\n"
                   "    println(taken);"));
}

SOLITON_TEST(vec_lends_an_owned_element_without_moving_it) {
    accept("pub fn width(word: &String) -> int { return len(word); }\n" +
           in_main("let mut words: Vec<String> = new_vec();\n"
                   "    let mut w: String = new_string();\n"
                   "    push(words, w);\n"
                   "    println(width(words[0]));\n"
                   "    println(words[0]);"));
}

SOLITON_TEST(vec_drops_its_elements_before_its_buffer) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // A `Vec<int>` frees one block; a `Vec<String>` has to walk what is
    // in it first, which is the loop this looks for.
    const std::string flat = compile_ir(in_main("let mut v: Vec<int> = new_vec();\n"
                                                "    push(v, 1);"));
    SOLITON_CHECK_MSG(flat.find("drop.each") == std::string::npos,
                    "a flat vector should need no element loop:\n" + flat);

    const std::string owned = compile_ir(in_main("let mut v: Vec<String> = new_vec();\n"
                                                 "    let mut w: String = new_string();\n"
                                                 "    push(v, w);"));
    SOLITON_CHECK_MSG(owned.find("drop.each") != std::string::npos,
                    "no element drop loop for a `Vec<String>`:\n" + owned);
}

SOLITON_TEST(vec_frees_an_owned_value_a_statement_throws_away) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // `pop(v);` takes an element out and discards it. The vector has
    // already given it up, so this statement is its last owner and has
    // to free it or it leaks.
    const std::string ir = compile_ir(in_main("let mut v: Vec<String> = new_vec();\n"
                                              "    let mut w: String = new_string();\n"
                                              "    push(v, w);\n"
                                              "    pop(v);"));
    SOLITON_CHECK_MSG(ir.find("discarded") != std::string::npos,
                    "a discarded owned value was left unfreed:\n" + ir);
}

SOLITON_TEST(vec_accepts_an_array_of_vectors) {
    // A `[Vec<int>; 2]` is fine: an array is laid out inline, so drop
    // walks its elements and frees each one.
    accept(in_main("let mut a: Vec<int> = new_vec();\n"
                   "    let mut b: Vec<int> = new_vec();\n"
                   "    push(a, 1);\n"
                   "    push(b, 2);\n"
                   "    let pair: [Vec<int>; 2] = [a, b];\n"
                   "    println(len(pair[0]));"));
}

SOLITON_TEST(vec_rejects_wrong_type_argument_count) {
    SOLITON_CHECK_EQ(first_error("pub fn f(v: Vec<int, float>) { }\n"),
                   std::string{"`Vec` takes 1 type argument but 2 were given"});
}

// ---------------------------------------------------------------------
// String
// ---------------------------------------------------------------------

SOLITON_TEST(string_supports_append_len_and_printing) {
    accept(in_main("let mut s: String = new_string();\n"
                   "    push_str(s, \"hello\");\n"
                   "    push_str(s, \" world\");\n"
                   "    println(s);\n"
                   "    println(len(s));"));
}

SOLITON_TEST(string_accepts_another_string_buffer) {
    accept(in_main("let mut a: String = new_string();\n"
                   "    let mut b: String = new_string();\n"
                   "    push_str(b, \"x\");\n"
                   "    push_str(a, b);\n"
                   "    println(a);"));
}

SOLITON_TEST(string_lends_a_view_of_itself_where_one_is_wanted) {
    // `String` owns a buffer; `string` is a borrowed fixed-length view
    // of one. Handing a `String` to something that only wants to read it
    // takes a view - the caller keeps the buffer, so this is a borrow
    // and not a move.
    accept("pub fn take(s: string) -> int { return len(s); }\n" +
           in_main("let mut s: String = new_string();\n"
                   "    push_str(s, \"text\");\n"
                   "    println(take(s));\n"
                   "    println(take(s));\n"
                   "    println(len(s));"));
}

SOLITON_TEST(string_cannot_be_conjured_from_a_view) {
    // The other direction needs a copy, and nothing here copies
    // silently: a `string` borrows bytes it does not own, and no
    // coercion can turn that into ownership.
    const std::vector<soliton::ast::Diagnostic> errors =
        reject("pub fn take(s: String) { }\n" +
               in_main("let view = \"fixed\";\n    take(view);"));
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `String`, found `string`"});
}

SOLITON_TEST(string_rejects_appending_to_a_view) {
    SOLITON_CHECK_EQ(first_error(in_main("let s = \"fixed\";\n    push_str(s, \"more\");")),
                   std::string{"cannot push text onto `string`"});
}

// ---------------------------------------------------------------------
// Ownership: moves
// ---------------------------------------------------------------------

SOLITON_TEST(ownership_rejects_using_a_value_after_it_moves) {
    const std::vector<soliton::ast::Diagnostic> errors =
        reject(in_main("let mut a: Vec<int> = new_vec();\n"
                       "    let b = a;\n"
                       "    println(len(a));"));
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"use of moved value `a`"});
    SOLITON_CHECK_MSG(errors.at(0).notes.at(0).find("moved at") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(0));
    SOLITON_CHECK_MSG(errors.at(0).notes.at(1).find("owns heap memory") != std::string::npos,
                    "note was: " + errors.at(0).notes.at(1));
}

SOLITON_TEST(ownership_moves_a_value_passed_by_value) {
    SOLITON_CHECK_EQ(first_error("pub fn consume(v: Vec<int>) { }\n" +
                               in_main("let mut v: Vec<int> = new_vec();\n"
                                       "    consume(v);\n"
                                       "    println(len(v));")),
                   std::string{"use of moved value `v`"});
}

SOLITON_TEST(ownership_does_not_move_a_value_passed_by_reference) {
    // A borrow is the whole reason `&T` still exists.
    accept("pub fn look(v: &Vec<int>) -> int { return len(v); }\n" +
           in_main("let mut v: Vec<int> = new_vec();\n"
                   "    println(look(v));\n"
                   "    println(len(v));"));
}

SOLITON_TEST(ownership_moves_a_returned_value_out_of_its_function) {
    accept("pub fn build() -> Vec<int> {\n"
           "    let mut v: Vec<int> = new_vec();\n"
           "    push(v, 1);\n"
           "    return v;\n"
           "}\n" +
           in_main("let made = build();\n    println(len(made));"));
}

SOLITON_TEST(ownership_moves_a_value_into_a_struct) {
    SOLITON_CHECK_EQ(first_error("struct Bag { pub items: Vec<int>, }\n" +
                               in_main("let mut v: Vec<int> = new_vec();\n"
                                       "    let b = Bag { items: v };\n"
                                       "    println(len(v));")),
                   std::string{"use of moved value `v`"});
}

SOLITON_TEST(ownership_makes_a_struct_holding_an_owned_field_owned_too) {
    SOLITON_CHECK_EQ(first_error("struct Bag { pub items: Vec<int>, }\n"
                               "pub fn take(b: Bag) { }\n" +
                               in_main("let mut v: Vec<int> = new_vec();\n"
                                       "    let b = Bag { items: v };\n"
                                       "    take(b);\n"
                                       "    println(len(b.items));")),
                   std::string{"use of moved value `b`"});
}

SOLITON_TEST(ownership_allows_reassigning_a_moved_binding) {
    // Assigning back gives the variable a value again, so it is usable.
    accept(in_main("let mut a: Vec<int> = new_vec();\n"
                   "    let b = a;\n"
                   "    a = new_vec();\n"
                   "    push(a, 1);\n"
                   "    println(len(a));"));
}

SOLITON_TEST(ownership_rejects_moving_out_of_a_field_or_element) {
    // Moving a field out would leave the struct half-owned, with no way
    // for the drop code to know which parts are still live.
    SOLITON_CHECK_EQ(first_error("struct Bag { pub items: Vec<int>, }\n"
                               "pub fn take(v: Vec<int>) { }\n" +
                               in_main("let mut v: Vec<int> = new_vec();\n"
                                       "    let b = Bag { items: v };\n"
                                       "    take(b.items);")),
                   std::string{"cannot move out of `Vec<int>` here"});
}

SOLITON_TEST(ownership_leaves_copyable_types_alone) {
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

SOLITON_TEST(ownership_emits_a_free_for_a_scoped_vec) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const std::string ir = compile_ir(in_main("let mut v: Vec<int> = new_vec();\n"
                                              "    push(v, 1);\n"
                                              "    println(len(v));"));
    SOLITON_CHECK_MSG(ir.find("soliton_free") != std::string::npos,
                    "no free emitted for a scoped Vec:\n" + ir);
}

SOLITON_TEST(ownership_emits_a_drop_flag_for_a_conditional_move) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // Moved on one path only, so whether to free is a runtime question.
    const std::string ir = compile_ir("pub fn consume(v: Vec<int>) { }\n" +
                                      in_main("let mut v: Vec<int> = new_vec();\n"
                                              "    if len(v) > 0 {\n"
                                              "        consume(v);\n"
                                              "    }"));
    SOLITON_CHECK_MSG(ir.find(".live") != std::string::npos,
                    "no drop flag emitted for a conditional move:\n" + ir);
    SOLITON_CHECK_MSG(ir.find("soliton_free") != std::string::npos, "no free emitted");
}

SOLITON_TEST(ownership_emits_no_free_for_a_program_with_no_owned_values) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    const std::string ir = compile_ir(in_main("let xs: [int; 2] = [1, 2];\n    println(xs[0]);"));
    SOLITON_CHECK_MSG(ir.find("soliton_free") == std::string::npos,
                    "a program with no heap values should emit no frees:\n" + ir);
}

SOLITON_TEST(ownership_grows_geometrically_rather_than_per_push) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    // The growth decision lives in the runtime, so a push is one call
    // rather than an inline realloc every time.
    const std::string ir = compile_ir(in_main("let mut v: Vec<int> = new_vec();\n"
                                              "    push(v, 1);"));
    SOLITON_CHECK_MSG(ir.find("soliton_grow") != std::string::npos,
                    "push should go through soliton_grow:\n" + ir);
}
