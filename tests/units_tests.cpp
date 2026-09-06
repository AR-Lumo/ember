// Units of measure (section 10.2).
//
// A number can carry a unit, and the checker refuses to add metres to
// seconds. `+` and `-` need identical units; `*` and `/` combine them
// algebraically, so a distance over a time is a speed with nobody
// having to declare one.
//
// Three properties are worth stating outright, because each is easy to
// get wrong in a way that looks fine:
//
//   - Units are compile-time only. A `float<meters>` is a `double` in
//     the generated code, and the IR for a program with units is
//     identical to the same program without them. That is asserted
//     here rather than asserted in prose.
//
//   - Dimensions are canonical. `meters*meters` and `meters^2` are one
//     type, and `meters/seconds*seconds` is just `meters` - otherwise
//     two ways of writing the same quantity would not be assignable to
//     each other.
//
//   - A unit binds to a literal only when the `<` touches it. Without
//     that rule `5<x` - a comparison written without spaces - would
//     become a syntax error or, worse, change meaning the day somebody
//     declares a unit called `x`.

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
using ember::typeck::Dimension;

SourceFile unit_source(std::string contents) {
    return SourceFile{"units.em", std::move(contents)};
}

CheckResult check_units(const SourceFile& source) {
    ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    if (!parsed.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "fixture does not parse:\n" +
                                ember::ast::render_all(parsed.diagnostics, source));
    }
    return ember::typeck::check(*parsed.program, source);
}

void accepts(const std::string& contents) {
    const SourceFile source = unit_source(contents);
    const CheckResult result = check_units(source);
    if (!result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this to type-check, but:\n" +
                                ember::ast::render_all(result.diagnostics, source));
    }
}

std::string rejects(const std::string& contents) {
    const SourceFile source = unit_source(contents);
    CheckResult result = check_units(source);
    if (result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this to be rejected, but it type-checked");
    }
    return result.diagnostics.at(0).message;
}

/// A program body wrapped in the two units the tests use.
std::string with_units(const std::string& body) {
    return "unit meters;\nunit seconds;\n\npub fn main() {\n" + body + "\n}\n";
}

std::string compile_ir(const std::string& contents) {
    const SourceFile source = unit_source(contents);
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
// The dimension algebra, on its own
// ---------------------------------------------------------------------

EMBER_TEST(a_dimension_is_canonical_however_it_was_written) {
    using ember::typeck::canonical_dimension;

    // Sorted by name, so the order of writing does not matter.
    const Dimension written{{"seconds", -1}, {"meters", 1}};
    const Dimension canonical = canonical_dimension(written);
    EMBER_CHECK_EQ(canonical.size(), std::size_t{2});
    EMBER_CHECK_EQ(canonical[0].first, std::string{"meters"});
    EMBER_CHECK_EQ(canonical[1].first, std::string{"seconds"});

    // Repeats fold together.
    EMBER_CHECK_EQ(canonical_dimension({{"meters", 1}, {"meters", 1}}).at(0).second, 2);

    // And anything that cancels disappears entirely, rather than
    // lingering as a unit raised to the power nought.
    EMBER_CHECK(canonical_dimension({{"meters", 1}, {"meters", -1}}).empty());
}

EMBER_TEST(combining_dimensions_adds_and_subtracts_exponents) {
    using ember::typeck::combine_dimensions;

    const Dimension meters{{"meters", 1}};
    const Dimension seconds{{"seconds", 1}};

    const Dimension speed = combine_dimensions(meters, seconds, -1);
    EMBER_CHECK_EQ(speed.size(), std::size_t{2});
    EMBER_CHECK_EQ(speed[1].second, -1);

    // A speed times a time is a distance again.
    const Dimension back = combine_dimensions(speed, seconds, 1);
    EMBER_CHECK_EQ(back.size(), std::size_t{1});
    EMBER_CHECK_EQ(back[0].first, std::string{"meters"});
    EMBER_CHECK_EQ(back[0].second, 1);
}

EMBER_TEST(a_dimension_prints_as_it_would_be_written) {
    using ember::typeck::dimension_string;

    EMBER_CHECK_EQ(dimension_string({{"meters", 1}, {"seconds", -1}}),
                   std::string{"meters/seconds"});
    EMBER_CHECK_EQ(dimension_string({{"meters", 1}, {"seconds", -2}}),
                   std::string{"meters/seconds^2"});
    EMBER_CHECK_EQ(dimension_string({{"meters", 2}}), std::string{"meters^2"});

    // A pure reciprocal needs a numerator to read as arithmetic, and
    // this is also what makes every printed unit something you can type
    // back in.
    EMBER_CHECK_EQ(dimension_string({{"seconds", -1}}), std::string{"1/seconds"});
}

// ---------------------------------------------------------------------
// The checker
// ---------------------------------------------------------------------

EMBER_TEST(matching_units_add) {
    accepts(with_units("    let a: float<meters> = 1.0<meters> + 2.0<meters>;"));
}

EMBER_TEST(mismatched_units_do_not_add) {
    const std::string message =
        rejects(with_units("    let bad = 1.0<meters> + 2.0<seconds>;"));
    EMBER_CHECK_EQ(message,
                   std::string{"cannot apply `+` to `float<meters>` and `float<seconds>`"});
}

EMBER_TEST(a_unit_is_not_the_same_as_no_unit) {
    // The case a looser rule would let through, and the one that makes
    // the feature worth having: a bare number is not a length.
    EMBER_CHECK_EQ(rejects(with_units("    let bad = 1.0<meters> + 2.0;")),
                   std::string{"cannot apply `+` to `float<meters>` and `float`"});
}

EMBER_TEST(dividing_combines_units) {
    accepts(with_units(
        "    let d: float<meters> = 10.0<meters>;\n"
        "    let t: float<seconds> = 2.0<seconds>;\n"
        "    let speed: float<meters/seconds> = d / t;"));
}

EMBER_TEST(multiplying_cancels_what_division_introduced) {
    accepts(with_units(
        "    let d: float<meters> = 10.0<meters>;\n"
        "    let t: float<seconds> = 2.0<seconds>;\n"
        "    let back: float<meters> = d / t * t;"));
}

EMBER_TEST(dividing_a_unit_by_itself_leaves_a_plain_number) {
    accepts(with_units(
        "    let d: float<meters> = 10.0<meters>;\n"
        "    let ratio: float = d / d;"));
}

EMBER_TEST(scaling_by_a_plain_number_keeps_the_unit) {
    accepts(with_units(
        "    let d: float<meters> = 10.0<meters>;\n"
        "    let twice: float<meters> = d * 2.0;"));
}

EMBER_TEST(dividing_twice_gives_a_squared_unit) {
    accepts(with_units(
        "    let d: float<meters> = 10.0<meters>;\n"
        "    let t: float<seconds> = 2.0<seconds>;\n"
        "    let rate: float<meters/seconds^2> = d / t / t;"));
}

EMBER_TEST(a_repeated_unit_and_a_power_are_the_same_type) {
    // `meters*meters` and `meters^2` have to be one type, or the two
    // ways of writing an area would not be assignable to each other.
    accepts(with_units(
        "    let area: float<meters*meters> = 2.0<meters> * 3.0<meters>;\n"
        "    let same: float<meters^2> = area;"));
}

EMBER_TEST(the_wrong_derived_unit_is_caught) {
    const std::string message = rejects(with_units(
        "    let d: float<meters> = 10.0<meters>;\n"
        "    let t: float<seconds> = 2.0<seconds>;\n"
        "    let wrong: float<meters> = d / t;"));
    EMBER_CHECK_EQ(message, std::string{"type mismatch"});
}

EMBER_TEST(an_undeclared_unit_is_an_error) {
    EMBER_CHECK_EQ(rejects("pub fn main() {\n    let d: float<furlongs> = 1.0;\n}\n"),
                   std::string{"unknown unit `furlongs`"});
}

EMBER_TEST(a_unit_cannot_be_declared_twice) {
    EMBER_CHECK_EQ(rejects("unit meters;\nunit meters;\npub fn main() {}\n"),
                   std::string{"duplicate definition of unit `meters`"});
}

EMBER_TEST(comparison_needs_matching_units) {
    accepts(with_units("    let yes = 1.0<meters> < 2.0<meters>;"));
    EMBER_CHECK(!rejects(with_units("    let no = 1.0<meters> < 2.0<seconds>;")).empty());
}

EMBER_TEST(int_carries_units_too) {
    accepts(with_units("    let steps: int<meters> = 7<meters>;"));
    EMBER_CHECK(!rejects(with_units("    let bad: int<meters> = 7;")).empty());
}

EMBER_TEST(units_travel_through_signatures) {
    accepts(
        "unit meters;\n"
        "unit seconds;\n"
        "pub fn speed(d: float<meters>, t: float<seconds>) -> float<meters/seconds> {\n"
        "    return d / t;\n"
        "}\n"
        "pub fn main() {\n"
        "    let s: float<meters/seconds> = speed(1.0<meters>, 1.0<seconds>);\n"
        "}\n");
}

EMBER_TEST(a_united_argument_is_not_a_plain_one) {
    EMBER_CHECK(!rejects("unit meters;\n"
                         "pub fn takes(d: float<meters>) -> float { return d / 1.0<meters>; }\n"
                         "pub fn main() { let x = takes(1.0); }\n")
                     .empty());
}

// ---------------------------------------------------------------------
// Parsing: the `<` that is not a comparison
// ---------------------------------------------------------------------

EMBER_TEST(a_unit_binds_only_when_it_touches_the_number) {
    // With a space it is a comparison, and stays one even though
    // `meters` is a declared unit.
    accepts(
        "unit meters;\n"
        "pub fn main() {\n"
        "    let meters = 3;\n"
        "    let less = 5 < meters;\n"
        "}\n");
}

EMBER_TEST(a_comparison_without_spaces_still_parses) {
    // The shape a bare adjacency rule would break: `5<x,` is not a unit
    // and must not be read as the start of one.
    accepts(
        "unit meters;\n"
        "pub fn pick(a: bool, b: int) -> int {\n"
        "    if a { return b; }\n"
        "    return 0;\n"
        "}\n"
        "pub fn main() {\n"
        "    let x = 9;\n"
        "    let got = pick(5<x, 3);\n"
        "}\n");
}

EMBER_TEST(a_literal_unit_parses_onto_the_literal) {
    const SourceFile source = unit_source("unit meters;\npub fn main() { let d = 5<meters>; }\n");
    const ember::parser::ParseResult parsed = ember::parser::parse_source(source);
    EMBER_CHECK(parsed.ok());
}

// ---------------------------------------------------------------------
// Units cost nothing
// ---------------------------------------------------------------------

EMBER_TEST(units_leave_no_trace_in_the_generated_code) {
    // The claim in section 10.2, checked rather than asserted: a
    // `float<meters>` is a `double`, and a program that uses units
    // compiles to exactly what the same program without them does.
    const std::string united = compile_ir(
        "unit meters;\n"
        "unit seconds;\n"
        "pub fn speed(d: float<meters>, t: float<seconds>) -> float<meters/seconds> {\n"
        "    return d / t;\n"
        "}\n"
        "pub fn main() {\n"
        "    println(speed(10.0<meters>, 4.0<seconds>));\n"
        "}\n");

    const std::string plain = compile_ir(
        "pub fn speed(d: float, t: float) -> float {\n"
        "    return d / t;\n"
        "}\n"
        "pub fn main() {\n"
        "    println(speed(10.0, 4.0));\n"
        "}\n");

    EMBER_CHECK_EQ(united, plain);
}
