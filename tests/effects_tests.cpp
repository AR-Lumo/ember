// Effect annotations (section 10.3).
//
// The spec contradicted itself here, and 10.3's "the inference rule,
// written down first" settles it: effects are **inferred**, and a
// `uses` clause is a **checked upper bound**. A function with no clause
// has no bound - which is the only reason this could be added to a
// published language at all, since all 31 programs in the corpus print.
//
// The parts worth testing directly, because each could be wrong in a
// way that still looks like it works:
//
//   - The bound is transitive. An effect three calls away still counts,
//     and the blame lands on the call, not on the distant `println`.
//
//   - The fixpoint terminates. Two functions calling each other must
//     not send the checker round forever.
//
//   - A call through a function value counts as effectful. Its type
//     says nothing about what it does, so anything else would make a
//     bound quietly untrue.
//
//   - No clause really means no bound. If this broke, every existing
//     Ember program would stop compiling.

#include "test_harness.hpp"

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/nodes.hpp"
#include "ember/parser/parser.hpp"
#include "ember/typeck/typeck.hpp"

#include <string>
#include <vector>

namespace {

using ember::ast::SourceFile;
using ember::typeck::CheckResult;

SourceFile effect_source(std::string contents) {
    return SourceFile{"effects.em", std::move(contents)};
}

CheckResult check_effects(const SourceFile& source) {
    ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    if (!parsed.ok()) {
        return CheckResult{};  // a parse error; the caller asserts on it
    }
    return ember::typeck::check(*parsed.program, source);
}

void accepts(const std::string& contents) {
    const SourceFile source = effect_source(contents);
    ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    if (!parsed.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this to parse, but:\n" +
                                ember::ast::render_all(parsed.diagnostics, source));
    }
    const CheckResult result = ember::typeck::check(*parsed.program, source);
    if (!result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this to type-check, but:\n" +
                                ember::ast::render_all(result.diagnostics, source));
    }
}

/// The first diagnostic, from either stage.
std::string rejects(const std::string& contents) {
    const SourceFile source = effect_source(contents);
    ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    if (!parsed.ok()) {
        return parsed.diagnostics.at(0).message;
    }
    CheckResult result = ember::typeck::check(*parsed.program, source);
    if (result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this to be rejected, but it was accepted");
    }
    return result.diagnostics.at(0).message;
}

const ember::ast::FunctionDecl& first_function(const ember::ast::Program& program) {
    for (const ember::ast::ItemPtr& item : program.items) {
        if (const auto* function = ember::ast::node_cast<ember::ast::FunctionDecl>(item.get())) {
            return *function;
        }
    }
    ::ember::test::fail(__FILE__, __LINE__, "no function in the program");
    throw 0;  // unreachable
}

}  // namespace

// ---------------------------------------------------------------------
// The clause itself
// ---------------------------------------------------------------------

EMBER_TEST(a_clause_is_absent_empty_or_a_list) {
    // Absence and emptiness are different, and the whole design turns
    // on that: no clause is no bound, `uses nothing` is the empty one.
    const SourceFile plain = effect_source("pub fn f() {}\n");
    const ember::parser::ParseResult a = ember::parser::parse_source(plain);
    EMBER_CHECK(a.ok());
    EMBER_CHECK(!first_function(*a.program).effects.present);

    const SourceFile pure = effect_source("pub fn f() uses nothing {}\n");
    const ember::parser::ParseResult b = ember::parser::parse_source(pure);
    EMBER_CHECK(b.ok());
    EMBER_CHECK(first_function(*b.program).effects.present);
    EMBER_CHECK(first_function(*b.program).effects.effects.empty());

    const SourceFile listed = effect_source("pub fn f() uses io, mut {}\n");
    const ember::parser::ParseResult c = ember::parser::parse_source(listed);
    EMBER_CHECK(c.ok());
    EMBER_CHECK_EQ(first_function(*c.program).effects.effects.size(), std::size_t{2});
    EMBER_CHECK(first_function(*c.program).effects.permits(ember::ast::Effect::Io));
    EMBER_CHECK(first_function(*c.program).effects.permits(ember::ast::Effect::Mut));
}

EMBER_TEST(an_unknown_effect_is_rejected) {
    EMBER_CHECK_EQ(rejects("pub fn f() uses filesystem {}\n"),
                   std::string{"unknown effect `filesystem`"});
}

EMBER_TEST(an_effect_is_named_once) {
    EMBER_CHECK_EQ(rejects("pub fn f() uses io, io {}\n"),
                   std::string{"`io` is listed twice"});
}

EMBER_TEST(nothing_does_not_combine_in_either_order) {
    // Both orders, because the first version of this caught only one -
    // `uses nothing, io` came out quietly meaning `uses io`, the
    // opposite of what was written.
    EMBER_CHECK_EQ(rejects("pub fn f() uses io, nothing {}\n"),
                   std::string{"`nothing` cannot be combined with an effect"});
    EMBER_CHECK_EQ(rejects("pub fn f() uses nothing, io {}\n"),
                   std::string{"`nothing` cannot be combined with an effect"});
}

EMBER_TEST(effect_names_are_not_keywords) {
    // Only `uses` is reserved. A program with a variable called `io`
    // has to keep working.
    accepts(
        "pub fn main() {\n"
        "    let io = 1;\n"
        "    let mut nothing = 2;\n"
        "    nothing = io;\n"
        "    println(nothing);\n"
        "}\n");
}

// ---------------------------------------------------------------------
// The bound
// ---------------------------------------------------------------------

EMBER_TEST(no_clause_means_no_bound) {
    // The property that let this ship. Every existing Ember program
    // looks like this one.
    accepts("pub fn main() { println(1); }\n");
}

EMBER_TEST(a_pure_function_may_be_declared_pure) {
    accepts("pub fn area(w: int, h: int) -> int uses nothing { return w * h; }\n");
}

EMBER_TEST(printing_from_a_pure_function_is_an_error) {
    EMBER_CHECK_EQ(rejects("pub fn quiet(x: int) uses nothing { println(x); }\n"),
                   std::string{"`io` is not permitted here"});
}

EMBER_TEST(printing_within_the_bound_is_fine) {
    accepts("pub fn loud(x: int) uses io { println(x); }\n");
}

EMBER_TEST(a_bound_is_a_ceiling_not_a_quota) {
    // Declaring `io` and never printing is allowed, the way an unused
    // `throws` is in Java.
    accepts("pub fn silent(x: int) -> int uses io { return x; }\n");
}

EMBER_TEST(an_effect_travels_through_a_call) {
    EMBER_CHECK_EQ(rejects("pub fn bottom(x: int) { println(x); }\n"
                           "pub fn top(x: int) uses nothing { bottom(x); }\n"),
                   std::string{"`io` is not permitted here"});
}

EMBER_TEST(an_effect_travels_any_distance) {
    // Three hops. A one-level check would let this through.
    EMBER_CHECK_EQ(rejects("pub fn bottom(x: int) { println(x); }\n"
                           "pub fn middle(x: int) { bottom(x); }\n"
                           "pub fn top(x: int) uses nothing { middle(x); }\n"),
                   std::string{"`io` is not permitted here"});
}

EMBER_TEST(a_pure_call_chain_stays_pure) {
    accepts(
        "pub fn area(w: int, h: int) -> int uses nothing { return w * h; }\n"
        "pub fn volume(w: int, h: int, d: int) -> int uses nothing {\n"
        "    return area(w, h) * d;\n"
        "}\n");
}

EMBER_TEST(mutual_recursion_terminates) {
    // The reason inference is a least fixed point rather than a walk.
    // If this ever hangs, that is the bug.
    accepts(
        "pub fn ping(n: int) uses nothing {\n"
        "    if n > 0 { pong(n - 1); }\n"
        "}\n"
        "pub fn pong(n: int) uses nothing {\n"
        "    if n > 0 { ping(n - 1); }\n"
        "}\n");
}

EMBER_TEST(mutual_recursion_still_reports_a_real_effect) {
    // And terminating must not mean giving up: an effect inside the
    // cycle still has to come out.
    EMBER_CHECK_EQ(rejects("pub fn ping(n: int) uses nothing {\n"
                           "    if n > 0 { pong(n - 1); }\n"
                           "}\n"
                           "pub fn pong(n: int) uses nothing {\n"
                           "    println(n);\n"
                           "    if n > 0 { ping(n - 1); }\n"
                           "}\n"),
                   std::string{"`io` is not permitted here"});
}

EMBER_TEST(a_contract_counts_as_part_of_the_function) {
    // A `requires` that prints is IO performed by this function, even
    // though it is written in the signature.
    EMBER_CHECK_EQ(rejects("pub fn noisy(x: int) -> bool uses nothing\n"
                           "    requires check(x)\n"
                           "{\n"
                           "    return true;\n"
                           "}\n"
                           "pub fn check(x: int) -> bool { println(x); return true; }\n"),
                   std::string{"`io` is not permitted here"});
}

EMBER_TEST(a_method_may_carry_a_bound) {
    accepts(
        "struct Room { pub w: int, pub h: int, }\n"
        "impl Room {\n"
        "    pub fn area(&self) -> int uses nothing { return self.w * self.h; }\n"
        "}\n");

    EMBER_CHECK_EQ(rejects("struct Room { pub w: int, }\n"
                           "impl Room {\n"
                           "    pub fn show(&self) uses nothing { println(self.w); }\n"
                           "}\n"),
                   std::string{"`io` is not permitted here"});
}

EMBER_TEST(a_generic_function_may_carry_a_bound) {
    accepts("pub fn identity<T>(v: T) -> T uses nothing { return v; }\n"
            "pub fn main() { println(identity(1)); }\n");
}

EMBER_TEST(calling_a_function_value_counts_as_effectful) {
    // Rule 6. A `fn(int) -> int` carries no effect information, so a
    // bound that ignored this would be quietly untrue.
    EMBER_CHECK_EQ(rejects("pub fn apply(f: fn(int) -> int, x: int) -> int uses nothing {\n"
                           "    return f(x);\n"
                           "}\n"),
                   std::string{"`io` is not permitted here"});

    // And `uses io` still permits it, so closures remain usable.
    accepts(
        "pub fn apply(f: fn(int) -> int, x: int) -> int uses io {\n"
        "    return f(x);\n"
        "}\n");
}

EMBER_TEST(mut_is_declarable_and_nothing_produces_it) {
    // Ember has no `&mut`, so `mut` is inert for now - it is accepted
    // so that programs written today do not change when it gains
    // meaning. A function declaring only `mut` may not print.
    accepts("pub fn f(x: int) -> int uses mut { return x; }\n");
    EMBER_CHECK_EQ(rejects("pub fn f(x: int) uses mut { println(x); }\n"),
                   std::string{"`io` is not permitted here"});
}
