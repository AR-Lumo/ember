// Text: comparing it, slicing it, searching it, and asking a container
// what it has room for.
//
// Ember has two text types and they are not interchangeable in every
// way: `string` is a borrowed view and `String` owns a buffer. But
// everything that only *reads* text should take either, because the
// difference is who owns the bytes and not what they say. That is the
// rule these tests pin down.
//
// `slice` returns a view rather than a copy, so a substring costs a
// bounds check and two fields. The cost of that choice is the same one
// `&T` already carries: the view dangles if what it points into is
// dropped or grown. Copying instead would make every substring an
// allocation, which is a worse default for a language with no garbage
// collector.

#include "test_harness.hpp"

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/nodes.hpp"
#include "ember/codegen/codegen.hpp"
#include "ember/parser/parser.hpp"
#include "ember/typeck/typeck.hpp"

#include <string>
#include <vector>

namespace {

using ember::ast::SourceFile;
using ember::typeck::CheckResult;

CheckResult check(const SourceFile& source) {
    const ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    if (!parsed.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "test fixture does not parse:\n" +
                                ember::ast::render_all(parsed.diagnostics, source));
    }
    return ember::typeck::check(*parsed.program, source);
}

void accept(const std::string& contents) {
    const SourceFile source{"test.em", contents};
    const CheckResult result = check(source);
    if (!result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this program to type-check, but:\n" +
                                ember::ast::render_all(result.diagnostics, source));
    }
}

std::vector<ember::ast::Diagnostic> reject(const std::string& contents) {
    const SourceFile source{"test.em", contents};
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

/// A `String` holding "hello, world", ready for a test body to use.
const char* const kOwned =
    "let mut owned: String = new_string();\n"
    "    push_str(owned, \"hello, world\");\n    ";

}  // namespace

// ---------------------------------------------------------------------
// Comparing
// ---------------------------------------------------------------------

EMBER_TEST(text_compares_across_the_two_representations) {
    // The papercut this fixes: the two types hold the same bytes and
    // differ only in who owns them.
    accept(in_main(std::string{kOwned} + "println(owned == \"hello, world\");\n"
                                         "    println(\"hello, world\" == owned);\n"
                                         "    println(owned != \"other\");"));
}

EMBER_TEST(text_orders_lexicographically) {
    accept(in_main("println(\"apple\" < \"banana\");\n"
                   "    println(\"a\" <= \"a\");\n"
                   "    println(\"b\" > \"a\");"));
}

EMBER_TEST(text_orders_across_the_two_representations) {
    accept(in_main(std::string{kOwned} + "println(owned < \"z\");"));
}

EMBER_TEST(text_ordering_still_refuses_what_has_no_order) {
    EMBER_CHECK_EQ(first_error(in_main("let a = true;\n    println(a < false);")),
                   std::string{"cannot compare values of type `bool`"});
}

// ---------------------------------------------------------------------
// Slicing
// ---------------------------------------------------------------------

EMBER_TEST(slice_takes_a_view_of_either_kind_of_text) {
    accept(in_main(std::string{kOwned} + "println(slice(\"hello\", 0, 2));\n"
                                         "    println(slice(owned, 7, 12));"));
}

EMBER_TEST(slice_yields_a_borrowed_view_not_an_owned_buffer) {
    // The type it produces is the whole point: a `string`, which owns
    // nothing and costs nothing.
    const SourceFile source{"test.em", in_main("let part = slice(\"hello\", 0, 2);\n"
                                               "    println(part);")};
    const CheckResult result = check(source);
    EMBER_CHECK(result.ok());

    // A `String` would move on assignment; a `string` copies, so this
    // second use is legal exactly because the slice is a view.
    accept(in_main("let part = slice(\"hello\", 0, 2);\n"
                   "    let again = part;\n"
                   "    println(part);\n"
                   "    println(again);"));
}

EMBER_TEST(slice_refuses_what_is_not_text) {
    EMBER_CHECK_EQ(first_error(in_main("println(slice(42, 0, 1));")),
                   std::string{"cannot slice `int`"});

    const std::vector<ember::ast::Diagnostic> errors =
        reject(in_main("let v: Vec<int> = new_vec();\n    println(slice(v, 0, 1));"));
    EMBER_CHECK_MSG(errors.at(0).notes.at(0).find("index it instead") != std::string::npos,
                    errors.at(0).notes.at(0));
}

EMBER_TEST(slice_requires_integer_bounds) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject(in_main("println(slice(\"hello\", \"a\", 2));"));
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `string`"});
}

EMBER_TEST(slice_wants_three_arguments) {
    EMBER_CHECK_EQ(first_error(in_main("println(slice(\"hello\", 1));")),
                   std::string{"this function takes 3 arguments but 2 were supplied"});
}

// ---------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------

EMBER_TEST(find_and_contains_take_either_kind_of_text) {
    accept(in_main(std::string{kOwned} + "println(find(owned, \"world\"));\n"
                                         "    println(contains(\"abc\", owned));\n"
                                         "    println(find(\"abc\", \"b\"));"));
}

EMBER_TEST(find_reports_an_int_and_contains_a_bool) {
    accept("pub fn takes_int(n: int) { }\n"
           "pub fn takes_bool(b: bool) { }\n" +
           in_main("takes_int(find(\"abc\", \"b\"));\n"
                   "    takes_bool(contains(\"abc\", \"b\"));"));
}

EMBER_TEST(find_refuses_what_is_not_text) {
    EMBER_CHECK_EQ(first_error(in_main("println(find(42, \"a\"));")),
                   std::string{"cannot search `int`"});
}

// ---------------------------------------------------------------------
// Capacity
// ---------------------------------------------------------------------

EMBER_TEST(capacity_and_reserve_work_on_growable_containers) {
    accept(in_main("let mut v: Vec<int> = new_vec();\n"
                   "    reserve(v, 100);\n"
                   "    println(capacity(v));\n"
                   "    let mut s: String = new_string();\n"
                   "    reserve(s, 64);\n"
                   "    println(capacity(s));"));
}

EMBER_TEST(capacity_refuses_a_container_that_cannot_grow) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject(in_main("let a: [int; 3] = [1, 2, 3];\n    println(capacity(a));"));
    EMBER_CHECK_EQ(errors.at(0).message,
                   std::string{"cannot ask `[int; 3]` about capacity"});
    EMBER_CHECK_MSG(errors.at(0).notes.at(0).find("fixed") != std::string::npos,
                    errors.at(0).notes.at(0));
}

EMBER_TEST(reserve_needs_a_mutable_container) {
    // The same rule `push` already applies.
    EMBER_CHECK_EQ(first_error(in_main("let v: Vec<int> = new_vec();\n    reserve(v, 10);")),
                   std::string{"cannot modify immutable binding `v`"});
}

EMBER_TEST(reserve_needs_an_integer) {
    const std::vector<ember::ast::Diagnostic> errors =
        reject(in_main("let mut v: Vec<int> = new_vec();\n    reserve(v, \"lots\");"));
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"expected `int`, found `string`"});
}
