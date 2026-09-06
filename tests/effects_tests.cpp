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
//     Cinder program would stop compiling.

#include "test_harness.hpp"

#include "cinder/ast/diagnostic.hpp"
#include "cinder/ast/nodes.hpp"
#include "cinder/parser/parser.hpp"
#include "cinder/typeck/typeck.hpp"

#include <string>
#include <vector>

namespace {

using cinder::ast::SourceFile;
using cinder::typeck::CheckResult;

SourceFile effect_source(std::string contents) {
    return SourceFile{"effects.ci", std::move(contents)};
}

CheckResult check_effects(const SourceFile& source) {
    cinder::parser::ParseResult parsed = cinder::parser::parse_source(source);
    if (!parsed.ok()) {
        return CheckResult{};  // a parse error; the caller asserts on it
    }
    return cinder::typeck::check(*parsed.program, source);
}

void accepts(const std::string& contents) {
    const SourceFile source = effect_source(contents);
    cinder::parser::ParseResult parsed = cinder::parser::parse_source(source);
    if (!parsed.ok()) {
        ::cinder::test::fail(__FILE__, __LINE__,
                            "expected this to parse, but:\n" +
                                cinder::ast::render_all(parsed.diagnostics, source));
    }
    const CheckResult result = cinder::typeck::check(*parsed.program, source);
    if (!result.ok()) {
        ::cinder::test::fail(__FILE__, __LINE__,
                            "expected this to type-check, but:\n" +
                                cinder::ast::render_all(result.diagnostics, source));
    }
}

/// The first diagnostic, from either stage.
std::string rejects(const std::string& contents) {
    const SourceFile source = effect_source(contents);
    cinder::parser::ParseResult parsed = cinder::parser::parse_source(source);
    if (!parsed.ok()) {
        return parsed.diagnostics.at(0).message;
    }
    CheckResult result = cinder::typeck::check(*parsed.program, source);
    if (result.ok()) {
        ::cinder::test::fail(__FILE__, __LINE__,
                            "expected this to be rejected, but it was accepted");
    }
    return result.diagnostics.at(0).message;
}

const cinder::ast::FunctionDecl& first_function(const cinder::ast::Program& program) {
    for (const cinder::ast::ItemPtr& item : program.items) {
        if (const auto* function = cinder::ast::node_cast<cinder::ast::FunctionDecl>(item.get())) {
            return *function;
        }
    }
    ::cinder::test::fail(__FILE__, __LINE__, "no function in the program");
    throw 0;  // unreachable
}

}  // namespace

// ---------------------------------------------------------------------
// The clause itself
// ---------------------------------------------------------------------

CINDER_TEST(a_clause_is_absent_empty_or_a_list) {
    // Absence and emptiness are different, and the whole design turns
    // on that: no clause is no bound, `uses nothing` is the empty one.
    const SourceFile plain = effect_source("pub fn f() {}\n");
    const cinder::parser::ParseResult a = cinder::parser::parse_source(plain);
    CINDER_CHECK(a.ok());
    CINDER_CHECK(!first_function(*a.program).effects.present);

    const SourceFile pure = effect_source("pub fn f() uses nothing {}\n");
    const cinder::parser::ParseResult b = cinder::parser::parse_source(pure);
    CINDER_CHECK(b.ok());
    CINDER_CHECK(first_function(*b.program).effects.present);
    CINDER_CHECK(first_function(*b.program).effects.effects.empty());

    const SourceFile listed = effect_source("pub fn f() uses io, mut {}\n");
    const cinder::parser::ParseResult c = cinder::parser::parse_source(listed);
    CINDER_CHECK(c.ok());
    CINDER_CHECK_EQ(first_function(*c.program).effects.effects.size(), std::size_t{2});
    CINDER_CHECK(first_function(*c.program).effects.permits(cinder::ast::Effect::Io));
    CINDER_CHECK(first_function(*c.program).effects.permits(cinder::ast::Effect::Mut));
}

CINDER_TEST(an_unknown_effect_is_rejected) {
    CINDER_CHECK_EQ(rejects("pub fn f() uses filesystem {}\n"),
                   std::string{"unknown effect `filesystem`"});
}

CINDER_TEST(an_effect_is_named_once) {
    CINDER_CHECK_EQ(rejects("pub fn f() uses io, io {}\n"),
                   std::string{"`io` is listed twice"});
}

CINDER_TEST(nothing_does_not_combine_in_either_order) {
    // Both orders, because the first version of this caught only one -
    // `uses nothing, io` came out quietly meaning `uses io`, the
    // opposite of what was written.
    CINDER_CHECK_EQ(rejects("pub fn f() uses io, nothing {}\n"),
                   std::string{"`nothing` cannot be combined with an effect"});
    CINDER_CHECK_EQ(rejects("pub fn f() uses nothing, io {}\n"),
                   std::string{"`nothing` cannot be combined with an effect"});
}

CINDER_TEST(effect_names_are_not_keywords) {
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

CINDER_TEST(no_clause_means_no_bound) {
    // The property that let this ship. Every existing Cinder program
    // looks like this one.
    accepts("pub fn main() { println(1); }\n");
}

CINDER_TEST(a_pure_function_may_be_declared_pure) {
    accepts("pub fn area(w: int, h: int) -> int uses nothing { return w * h; }\n");
}

CINDER_TEST(printing_from_a_pure_function_is_an_error) {
    CINDER_CHECK_EQ(rejects("pub fn quiet(x: int) uses nothing { println(x); }\n"),
                   std::string{"`io` is not permitted here"});
}

CINDER_TEST(printing_within_the_bound_is_fine) {
    accepts("pub fn loud(x: int) uses io { println(x); }\n");
}

CINDER_TEST(a_bound_is_a_ceiling_not_a_quota) {
    // Declaring `io` and never printing is allowed, the way an unused
    // `throws` is in Java.
    accepts("pub fn silent(x: int) -> int uses io { return x; }\n");
}

CINDER_TEST(an_effect_travels_through_a_call) {
    CINDER_CHECK_EQ(rejects("pub fn bottom(x: int) { println(x); }\n"
                           "pub fn top(x: int) uses nothing { bottom(x); }\n"),
                   std::string{"`io` is not permitted here"});
}

CINDER_TEST(an_effect_travels_any_distance) {
    // Three hops. A one-level check would let this through.
    CINDER_CHECK_EQ(rejects("pub fn bottom(x: int) { println(x); }\n"
                           "pub fn middle(x: int) { bottom(x); }\n"
                           "pub fn top(x: int) uses nothing { middle(x); }\n"),
                   std::string{"`io` is not permitted here"});
}

CINDER_TEST(a_pure_call_chain_stays_pure) {
    accepts(
        "pub fn area(w: int, h: int) -> int uses nothing { return w * h; }\n"
        "pub fn volume(w: int, h: int, d: int) -> int uses nothing {\n"
        "    return area(w, h) * d;\n"
        "}\n");
}

CINDER_TEST(mutual_recursion_terminates) {
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

CINDER_TEST(mutual_recursion_still_reports_a_real_effect) {
    // And terminating must not mean giving up: an effect inside the
    // cycle still has to come out.
    CINDER_CHECK_EQ(rejects("pub fn ping(n: int) uses nothing {\n"
                           "    if n > 0 { pong(n - 1); }\n"
                           "}\n"
                           "pub fn pong(n: int) uses nothing {\n"
                           "    println(n);\n"
                           "    if n > 0 { ping(n - 1); }\n"
                           "}\n"),
                   std::string{"`io` is not permitted here"});
}

CINDER_TEST(a_contract_counts_as_part_of_the_function) {
    // A `requires` that prints is IO performed by this function, even
    // though it is written in the signature.
    CINDER_CHECK_EQ(rejects("pub fn noisy(x: int) -> bool uses nothing\n"
                           "    requires check(x)\n"
                           "{\n"
                           "    return true;\n"
                           "}\n"
                           "pub fn check(x: int) -> bool { println(x); return true; }\n"),
                   std::string{"`io` is not permitted here"});
}

CINDER_TEST(a_method_may_carry_a_bound) {
    accepts(
        "struct Room { pub w: int, pub h: int, }\n"
        "impl Room {\n"
        "    pub fn area(&self) -> int uses nothing { return self.w * self.h; }\n"
        "}\n");

    CINDER_CHECK_EQ(rejects("struct Room { pub w: int, }\n"
                           "impl Room {\n"
                           "    pub fn show(&self) uses nothing { println(self.w); }\n"
                           "}\n"),
                   std::string{"`io` is not permitted here"});
}

CINDER_TEST(a_generic_function_may_carry_a_bound) {
    accepts("pub fn identity<T>(v: T) -> T uses nothing { return v; }\n"
            "pub fn main() { println(identity(1)); }\n");
}

CINDER_TEST(calling_an_unbounded_function_value_counts_as_effectful) {
    // An unbounded `fn(int) -> int` carries no effect information, so a
    // bound that ignored it would be quietly untrue. This is every
    // function type written before effects entered them.
    CINDER_CHECK_EQ(rejects("pub fn apply(f: fn(int) -> int, x: int) -> int uses nothing {\n"
                           "    return f(x);\n"
                           "}\n"),
                   std::string{"`io` is not permitted here"});

    // And `uses io` still permits it, so closures remain usable.
    accepts(
        "pub fn apply(f: fn(int) -> int, x: int) -> int uses io {\n"
        "    return f(x);\n"
        "}\n");
}

// ---------------------------------------------------------------------
// Effects in function types
// ---------------------------------------------------------------------

CINDER_TEST(a_bounded_function_type_makes_a_pure_higher_order_function_possible) {
    // The hole that putting effects in the type closes. Before it, no
    // function taking a callback could be `uses nothing`, because the
    // call through it counted as anything.
    accepts(
        "pub fn apply(f: fn(int) -> int uses nothing, x: int) -> int uses nothing {\n"
        "    return f(x);\n"
        "}\n");
}

CINDER_TEST(a_bound_on_a_type_permits_exactly_what_it_says) {
    // `uses io` on the type means the call performs `io` - no more, so
    // an `io` caller is fine, and no less, so a pure one is not.
    accepts(
        "pub fn apply(f: fn(int) -> int uses io, x: int) -> int uses io {\n"
        "    return f(x);\n"
        "}\n");

    CINDER_CHECK_EQ(rejects("pub fn apply(f: fn(int) -> int uses io, x: int) -> int uses nothing {\n"
                           "    return f(x);\n"
                           "}\n"),
                   std::string{"`io` is not permitted here"});
}

CINDER_TEST(an_impure_closure_does_not_fit_a_pure_bound) {
    const std::string message = rejects(
        "pub fn apply(f: fn(int) -> int uses nothing, x: int) -> int uses nothing {\n"
        "    return f(x);\n"
        "}\n"
        "pub fn main() {\n"
        "    println(apply(|x: int| { println(x); return x; }, 3));\n"
        "}\n");
    CINDER_CHECK_EQ(message, std::string{"this closure performs `io`"});
}

CINDER_TEST(a_pure_closure_fits_a_pure_bound) {
    accepts(
        "pub fn apply(f: fn(int) -> int uses nothing, x: int) -> int uses nothing {\n"
        "    return f(x);\n"
        "}\n"
        "pub fn main() {\n"
        "    println(apply(|x: int| { return x * 2; }, 21));\n"
        "}\n");
}

CINDER_TEST(a_function_that_does_less_fits_where_more_is_allowed) {
    // Width subtyping on the effect set: a pure closure satisfies a
    // parameter that merely permits `io`. Without this a bound would be
    // a straitjacket rather than a ceiling.
    accepts(
        "pub fn apply(f: fn(int) -> int uses io, x: int) -> int uses io {\n"
        "    return f(x);\n"
        "}\n"
        "pub fn pure_one(f: fn(int) -> int uses nothing, x: int) -> int uses io {\n"
        "    return apply(f, x);\n"
        "}\n");
}

CINDER_TEST(defining_a_closure_is_not_calling_it) {
    // A factory may be pure even though what it hands back is not.
    // Before effects were in types, the closure's `println` was charged
    // to the function that merely wrote it down.
    accepts(
        "pub fn make(limit: int) -> fn(int) -> int uses nothing {\n"
        "    return |x: int| { println(x); return x + limit; };\n"
        "}\n");
}

CINDER_TEST(a_bound_is_part_of_how_a_function_type_is_written) {
    // Two types that differ only in their bound must not print the
    // same, or a mismatch between them reads as nonsense.
    const std::string message = rejects(
        "pub fn apply(f: fn(int) -> int uses nothing) -> int { return f(1); }\n"
        "pub fn main() {\n"
        "    let g: fn(int) -> int uses io = |x: int| { println(x); return x; };\n"
        "    println(apply(g));\n"
        "}\n");
    CINDER_CHECK_EQ(message, std::string{"type mismatch"});
}

CINDER_TEST(a_return_type_bounds_the_function_not_the_type_it_returns) {
    // `fn f() -> fn(int) -> int uses nothing` binds the clause to `f`.
    // Binding it to the returned type instead would leave `f` unbounded
    // while looking like it had been constrained.
    const SourceFile source = effect_source(
        "pub fn make() -> fn(int) -> int uses nothing {\n"
        "    return |x: int| { return x; };\n"
        "}\n");
    const cinder::parser::ParseResult parsed = cinder::parser::parse_source(source);
    CINDER_CHECK(parsed.ok());
    CINDER_CHECK(first_function(*parsed.program).effects.present);
    CINDER_CHECK(first_function(*parsed.program).effects.effects.empty());
}

CINDER_TEST(a_bounded_type_in_a_parameter_list_does_not_eat_the_comma) {
    // `f: fn(int) -> int uses nothing, x: int` - a greedy clause reads
    // the separator as another effect and then `x` as its name.
    accepts(
        "pub fn apply(f: fn(int) -> int uses nothing, x: int) -> int uses nothing {\n"
        "    return f(x);\n"
        "}\n");

    // And with two effects listed, where the comma really does
    // continue. The caller has to permit both, since calling a type
    // bounded `io, mut` performs both.
    accepts(
        "pub fn apply(f: fn(int) -> int uses io, mut, x: int) -> int uses io, mut {\n"
        "    return f(x);\n"
        "}\n");
}

CINDER_TEST(mut_is_declarable_and_nothing_produces_it) {
    // Cinder has no `&mut`, so `mut` is inert for now - it is accepted
    // so that programs written today do not change when it gains
    // meaning. A function declaring only `mut` may not print.
    accepts("pub fn f(x: int) -> int uses mut { return x; }\n");
    CINDER_CHECK_EQ(rejects("pub fn f(x: int) uses mut { println(x); }\n"),
                   std::string{"`io` is not permitted here"});
}
