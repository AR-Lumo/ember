// Phase 2 unit tests: the parser.
//
// The golden `.ast` snapshots in snapshot_tests.cpp cover whole files;
// these cover the parts a snapshot reads past - the §3 precedence table,
// associativity, the struct-literal ambiguity, and the wording and
// placement of every syntax error.

#include "test_harness.hpp"

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/nodes.hpp"
#include "ember/ast/printer.hpp"
#include "ember/ast/span.hpp"
#include "ember/parser/parser.hpp"

#include <string>
#include <vector>

namespace {

using ember::ast::Position;
using ember::ast::SourceFile;
using ember::parser::ParseResult;

SourceFile make_source(std::string contents) {
    return SourceFile{"test.em", std::move(contents)};
}

/// Parse a whole program expected to be clean.
ParseResult parse_ok(const SourceFile& source) {
    ParseResult result = ember::parser::parse_source(source);
    if (!result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "unexpected parse errors:\n" +
                                ember::ast::render_all(result.diagnostics, source));
    }
    return result;
}

/// Parse a program expected to fail, and return its diagnostics.
std::vector<ember::ast::Diagnostic> parse_errors(const SourceFile& source) {
    ParseResult result = ember::parser::parse_source(source);
    if (result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__, "expected parse errors, but parsing succeeded");
    }
    return result.diagnostics;
}

/// Parse a bare expression by wrapping it in a function, and render the
/// resulting tree as a single line so precedence is easy to assert on.
std::string expr_tree(const std::string& expression) {
    const SourceFile source = make_source("fn f() { let x = " + expression + "; }\n");
    const ParseResult result = parse_ok(source);

    const auto* function =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* let = ember::ast::node_cast<ember::ast::LetStmt>(
        function->body.statements.at(0).get());

    // Collapse the indented s-expression onto one line.
    std::string tree = ember::ast::to_sexpr(*let->value, source);
    std::string flat;
    bool pending_space = false;
    for (const char c : tree) {
        if (c == '\n' || c == ' ') {
            pending_space = !flat.empty();
            continue;
        }
        // Space only between a label and its inline value, never before
        // a bracket, which keeps these assertions readable as one line.
        if (pending_space && c != ')' && c != '(') {
            flat += ' ';
        }
        pending_space = false;
        flat += c;
    }
    return flat;
}

/// The same, with `@line:col` markers stripped, for tests about shape
/// rather than position.
std::string shape_of(const std::string& expression) {
    const std::string tree = expr_tree(expression);
    std::string out;
    for (std::size_t i = 0; i < tree.size();) {
        if (tree[i] == '@') {
            // A span marker runs to the next space or bracket. Stopping
            // at `(` matters for nodes with no inline label, where the
            // first child follows the marker directly.
            while (i < tree.size() && tree[i] != ' ' && tree[i] != ')' && tree[i] != '(') {
                ++i;
            }
            if (i < tree.size() && tree[i] == ' ') {
                // A label follows: the marker and one space go away.
                ++i;
            } else if (!out.empty() && out.back() == ' ') {
                // A bracket follows: drop the space that preceded the
                // marker too, so no gap is left behind.
                out.pop_back();
            }
            continue;
        }
        out += tree[i];
        ++i;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------
// Operator precedence and associativity (§3)
// ---------------------------------------------------------------------

EMBER_TEST(parser_binds_multiplication_tighter_than_addition) {
    EMBER_CHECK_EQ(shape_of("1 + 2 * 3"),
                   std::string{"(binary add(int-lit 1)(binary mul(int-lit 2)(int-lit 3)))"});
    EMBER_CHECK_EQ(shape_of("1 * 2 + 3"),
                   std::string{"(binary add(binary mul(int-lit 1)(int-lit 2))(int-lit 3))"});
}

EMBER_TEST(parser_binds_remainder_like_multiplication) {
    EMBER_CHECK_EQ(shape_of("1 + 2 % 3"),
                   std::string{"(binary add(int-lit 1)(binary rem(int-lit 2)(int-lit 3)))"});
}

EMBER_TEST(parser_follows_the_full_precedence_ladder) {
    // ||  <  &&  <  == !=  <  < > <= >=  <  + -  <  * / %
    EMBER_CHECK_EQ(
        shape_of("a || b && c == d < e + f * g"),
        std::string{"(binary or(name a)(binary and(name b)(binary eq(name c)(binary lt(name d)"
                    "(binary add(name e)(binary mul(name f)(name g)))))))"});
}

EMBER_TEST(parser_makes_binary_operators_left_associative) {
    EMBER_CHECK_EQ(shape_of("1 - 2 - 3"),
                   std::string{"(binary sub(binary sub(int-lit 1)(int-lit 2))(int-lit 3))"});
    EMBER_CHECK_EQ(shape_of("1 / 2 / 3"),
                   std::string{"(binary div(binary div(int-lit 1)(int-lit 2))(int-lit 3))"});
}

EMBER_TEST(parser_binds_unary_tighter_than_binary) {
    EMBER_CHECK_EQ(shape_of("-1 + 2"),
                   std::string{"(binary add(unary neg(int-lit 1))(int-lit 2))"});
    EMBER_CHECK_EQ(shape_of("!a && b"),
                   std::string{"(binary and(unary not(name a))(name b))"});
}

EMBER_TEST(parser_stacks_unary_operators) {
    EMBER_CHECK_EQ(shape_of("--1"), std::string{"(unary neg(unary neg(int-lit 1)))"});
    EMBER_CHECK_EQ(shape_of("!!a"), std::string{"(unary not(unary not(name a)))"});
}

EMBER_TEST(parser_binds_postfix_tighter_than_unary) {
    // -a.b is -(a.b), not (-a).b
    EMBER_CHECK_EQ(shape_of("-a.b"), std::string{"(unary neg(field-get b(name a)))"});
    EMBER_CHECK_EQ(shape_of("-a[0]"),
                   std::string{"(unary neg(index(name a)(int-lit 0)))"});
}

EMBER_TEST(parser_binds_as_tighter_than_binary_operators) {
    // `a as float * b` is `(a as float) * b`, not `a as (float * b)`.
    EMBER_CHECK_EQ(shape_of("a as float * b"),
                   std::string{"(binary mul(cast(float)(name a))(name b))"});
    EMBER_CHECK_EQ(shape_of("1 + 2 as float"),
                   std::string{"(binary add(int-lit 1)(cast(float)(int-lit 2)))"});
}

EMBER_TEST(parser_binds_as_looser_than_unary) {
    // `-x as float` is `(-x) as float`, following Rust.
    EMBER_CHECK_EQ(shape_of("-x as float"),
                   std::string{"(cast(float)(unary neg(name x)))"});
}

EMBER_TEST(parser_chains_casts_left_to_right) {
    EMBER_CHECK_EQ(shape_of("x as float as int"),
                   std::string{"(cast(int)(cast(float)(name x)))"});
}

EMBER_TEST(parser_reads_a_cast_to_every_type_form) {
    EMBER_CHECK_EQ(shape_of("x as int"), std::string{"(cast(int)(name x))"});
    EMBER_CHECK_EQ(shape_of("x as Point"), std::string{"(cast(named Point)(name x))"});
    EMBER_CHECK_EQ(shape_of("x as &int"), std::string{"(cast(ref(int))(name x))"});
}

EMBER_TEST(parser_lets_parentheses_override_precedence) {
    EMBER_CHECK_EQ(shape_of("(1 + 2) * 3"),
                   std::string{"(binary mul(binary add(int-lit 1)(int-lit 2))(int-lit 3))"});
}

EMBER_TEST(parser_keeps_no_node_for_parentheses) {
    // Grouping is recorded by the shape of the tree, so `(1)` and `1`
    // parse identically.
    EMBER_CHECK_EQ(shape_of("(((1)))"), std::string{"(int-lit 1)"});
}

// ---------------------------------------------------------------------
// Postfix chains
// ---------------------------------------------------------------------

EMBER_TEST(parser_chains_field_access_index_and_method_calls) {
    EMBER_CHECK_EQ(shape_of("a.b[0].c(1)"),
                   std::string{"(method-call c(index(field-get b(name a))(int-lit 0))"
                               "(int-lit 1))"});
}

EMBER_TEST(parser_reads_a_call_with_no_arguments) {
    EMBER_CHECK_EQ(shape_of("main()"), std::string{"(call main)"});
}

EMBER_TEST(parser_reads_method_calls_on_self) {
    EMBER_CHECK_EQ(shape_of("self.distance_sq(other)"),
                   std::string{"(method-call distance_sq(name self)(name other))"});
}

EMBER_TEST(parser_distinguishes_field_access_from_a_method_call) {
    EMBER_CHECK_EQ(shape_of("p.x"), std::string{"(field-get x(name p))"});
    EMBER_CHECK_EQ(shape_of("p.x()"), std::string{"(method-call x(name p))"});
}

// ---------------------------------------------------------------------
// Literals
// ---------------------------------------------------------------------

EMBER_TEST(parser_reads_every_literal_form) {
    EMBER_CHECK_EQ(shape_of("1"), std::string{"(int-lit 1)"});
    EMBER_CHECK_EQ(shape_of("1.5"), std::string{"(float-lit 1.5)"});
    EMBER_CHECK_EQ(shape_of("true"), std::string{"(bool-lit true)"});
    EMBER_CHECK_EQ(shape_of("\"hi\""), std::string{"(string-lit \"hi\")"});
    EMBER_CHECK_EQ(shape_of("[1, 2, 3]"),
                   std::string{"(array-lit(int-lit 1)(int-lit 2)(int-lit 3))"});
    EMBER_CHECK_EQ(shape_of("[]"), std::string{"(array-lit)"});
}

EMBER_TEST(parser_allows_a_trailing_comma_in_an_array_literal) {
    EMBER_CHECK_EQ(shape_of("[1, 2,]"), std::string{"(array-lit(int-lit 1)(int-lit 2))"});
}

EMBER_TEST(parser_reads_struct_literals) {
    EMBER_CHECK_EQ(shape_of("Point { x: 1, y: 2 }"),
                   std::string{"(struct-lit Point(init x(int-lit 1))(init y(int-lit 2)))"});
    EMBER_CHECK_EQ(shape_of("Empty {}"), std::string{"(struct-lit Empty)"});
}

// ---------------------------------------------------------------------
// The struct-literal / block ambiguity
// ---------------------------------------------------------------------

EMBER_TEST(parser_does_not_read_a_struct_literal_in_an_if_condition) {
    // Without the restriction, `if flag { }` would parse `flag { }` as a
    // struct literal and then demand a block that is not there.
    const SourceFile source = make_source("fn f() { if flag { let x = 1; } }\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* function =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* branch =
        ember::ast::node_cast<ember::ast::IfStmt>(function->body.statements.at(0).get());
    EMBER_CHECK(branch != nullptr);
    EMBER_CHECK(ember::ast::node_cast<ember::ast::NameExpr>(branch->condition.get()) != nullptr);
    EMBER_CHECK_EQ(branch->then_block.statements.size(), std::size_t{1});
}

EMBER_TEST(parser_does_not_read_a_struct_literal_in_a_while_condition) {
    const SourceFile source = make_source("fn f() { while running { let x = 1; } }\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* function =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    EMBER_CHECK(ember::ast::node_cast<ember::ast::WhileStmt>(
                    function->body.statements.at(0).get()) != nullptr);
}

EMBER_TEST(parser_allows_a_struct_literal_in_a_condition_inside_parentheses) {
    // The documented escape hatch, same as Rust's.
    const SourceFile source = make_source("fn f() { if (Flag { on: true }).on { } }\n");
    const ember::parser::ParseResult result = parse_ok(source);
    EMBER_CHECK_EQ(result.program->items.size(), std::size_t{1});
}

EMBER_TEST(parser_still_reads_struct_literals_in_ordinary_positions) {
    EMBER_CHECK_EQ(shape_of("Point { x: 1 }"),
                   std::string{"(struct-lit Point(init x(int-lit 1)))"});
}

// ---------------------------------------------------------------------
// Items and statements
// ---------------------------------------------------------------------

EMBER_TEST(parser_records_visibility_on_items) {
    const SourceFile source = make_source(
        "pub fn a() { }\n"
        "fn b() { }\n"
        "pub struct S { x: int, }\n"
        "pub const C: int = 1;\n");
    const ember::parser::ParseResult result = parse_ok(source);

    EMBER_CHECK_EQ(result.program->items.at(0)->is_public, true);
    EMBER_CHECK_EQ(result.program->items.at(1)->is_public, false);
    EMBER_CHECK_EQ(result.program->items.at(2)->is_public, true);
    EMBER_CHECK_EQ(result.program->items.at(3)->is_public, true);
}

EMBER_TEST(parser_reads_both_self_receiver_forms) {
    const SourceFile source = make_source(
        "impl Point {\n"
        "    fn by_value(self) { }\n"
        "    fn by_reference(&self) { }\n"
        "    fn free() { }\n"
        "}\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* block =
        ember::ast::node_cast<ember::ast::ImplBlock>(result.program->items.at(0).get());
    EMBER_CHECK_EQ(block->methods.size(), std::size_t{3});
    EMBER_CHECK(block->methods[0]->self_param()->self_kind == ember::ast::SelfKind::Value);
    EMBER_CHECK(block->methods[1]->self_param()->self_kind == ember::ast::SelfKind::Reference);
    EMBER_CHECK(block->methods[2]->self_param() == nullptr);
}

EMBER_TEST(parser_records_the_owning_type_on_methods) {
    // Methods are scoped to their impl type, not the global function
    // namespace; Phase 3 resolves `p.f()` through this.
    const SourceFile source = make_source("impl Point { fn area(&self) -> int { return 1; } }\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* block =
        ember::ast::node_cast<ember::ast::ImplBlock>(result.program->items.at(0).get());
    EMBER_CHECK_EQ(block->methods.at(0)->owner_type, std::string{"Point"});
}

EMBER_TEST(parser_reads_every_type_form) {
    const SourceFile source = make_source(
        "fn f(a: int, b: float, c: bool, d: string, e: Point, g: &Point, h: [int; 4]) { }\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* function =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    std::string types;
    for (const ember::ast::Param& param : function->params) {
        if (!types.empty()) {
            types += ' ';
        }
        types += ember::ast::type_to_string(*param.type);
    }
    EMBER_CHECK_EQ(types, std::string{"int float bool string Point &Point [int; 4]"});
}

EMBER_TEST(parser_reads_nested_reference_and_array_types) {
    const SourceFile source = make_source("fn f(a: &[&int; 2]) { }\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* function =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    EMBER_CHECK_EQ(ember::ast::type_to_string(*function->params.at(0).type),
                   std::string{"&[&int; 2]"});
}

EMBER_TEST(parser_distinguishes_annotated_and_inferred_let) {
    const SourceFile source = make_source("fn f() { let a: int = 1; let b = 2; let mut c = 3; }\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* function =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* annotated =
        ember::ast::node_cast<ember::ast::LetStmt>(function->body.statements.at(0).get());
    const auto* inferred =
        ember::ast::node_cast<ember::ast::LetStmt>(function->body.statements.at(1).get());
    const auto* mutable_binding =
        ember::ast::node_cast<ember::ast::LetStmt>(function->body.statements.at(2).get());

    EMBER_CHECK(annotated->declared_type != nullptr);
    EMBER_CHECK(inferred->declared_type == nullptr);
    EMBER_CHECK_EQ(annotated->is_mutable, false);
    EMBER_CHECK_EQ(mutable_binding->is_mutable, true);
}

EMBER_TEST(parser_reads_bare_and_valued_returns) {
    const SourceFile source = make_source("fn f() { return; }\nfn g() -> int { return 1; }\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* f =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* g =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(1).get());

    EMBER_CHECK(ember::ast::node_cast<ember::ast::ReturnStmt>(f->body.statements.at(0).get())
                    ->value == nullptr);
    EMBER_CHECK(ember::ast::node_cast<ember::ast::ReturnStmt>(g->body.statements.at(0).get())
                    ->value != nullptr);
    EMBER_CHECK(f->return_type == nullptr);
    EMBER_CHECK(g->return_type != nullptr);
}

EMBER_TEST(parser_chains_else_if) {
    const SourceFile source = make_source(
        "fn f() {\n"
        "    if a { } else if b { } else { }\n"
        "}\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* function =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* first =
        ember::ast::node_cast<ember::ast::IfStmt>(function->body.statements.at(0).get());
    const auto* second = ember::ast::node_cast<ember::ast::IfStmt>(first->else_branch.get());

    EMBER_CHECK(second != nullptr);
    EMBER_CHECK(ember::ast::node_cast<ember::ast::BlockStmt>(second->else_branch.get()) !=
                nullptr);
}

EMBER_TEST(parser_separates_assignment_from_an_expression_statement) {
    const SourceFile source = make_source("fn f() { total = 1; step(); p.x = 2; a[0] = 3; }\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* function =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    EMBER_CHECK(function->body.statements.at(0)->kind == ember::ast::StmtKind::Assign);
    EMBER_CHECK(function->body.statements.at(1)->kind == ember::ast::StmtKind::Expr);
    EMBER_CHECK(function->body.statements.at(2)->kind == ember::ast::StmtKind::Assign);
    EMBER_CHECK(function->body.statements.at(3)->kind == ember::ast::StmtKind::Assign);
}

// ---------------------------------------------------------------------
// Spans
// ---------------------------------------------------------------------

EMBER_TEST(parser_gives_a_binary_expression_a_span_covering_both_operands) {
    const SourceFile source = make_source("fn f() { let x = 10 + 20; }\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* function =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* let =
        ember::ast::node_cast<ember::ast::LetStmt>(function->body.statements.at(0).get());

    EMBER_CHECK_EQ(std::string{source.text_of(let->value->span)}, std::string{"10 + 20"});
}

EMBER_TEST(parser_gives_each_item_a_span_covering_the_whole_declaration) {
    const SourceFile source = make_source("pub fn f() -> int {\n    return 1;\n}\n");
    const ember::parser::ParseResult result = parse_ok(source);

    EMBER_CHECK_EQ(std::string{source.text_of(result.program->items.at(0)->span)},
                   std::string{"pub fn f() -> int {\n    return 1;\n}"});
}

EMBER_TEST(parser_spans_a_method_call_from_receiver_to_closing_paren) {
    const SourceFile source = make_source("fn f() { let d = p.distance_sq(q); }\n");
    const ember::parser::ParseResult result = parse_ok(source);

    const auto* function =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* let =
        ember::ast::node_cast<ember::ast::LetStmt>(function->body.statements.at(0).get());

    EMBER_CHECK_EQ(std::string{source.text_of(let->value->span)},
                   std::string{"p.distance_sq(q)"});
}

// ---------------------------------------------------------------------
// Syntax errors (§7)
// ---------------------------------------------------------------------

EMBER_TEST(parser_reports_a_missing_semicolon) {
    const SourceFile source = make_source("fn f() { let x = 1 }\n");
    const std::vector<ember::ast::Diagnostic> errors = parse_errors(source);
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"expected `;`, found `}`"});
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"expected `;`"});
}

EMBER_TEST(parser_reports_a_missing_expression) {
    const SourceFile source = make_source("fn f() { let x = ; }\n");
    const std::vector<ember::ast::Diagnostic> errors = parse_errors(source);
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"expected an expression, found `;`"});
}

EMBER_TEST(parser_reports_a_bad_item_keyword) {
    const SourceFile source = make_source("let x = 1;\n");
    const std::vector<ember::ast::Diagnostic> errors = parse_errors(source);
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"expected an item, found `let`"});
}

EMBER_TEST(parser_reports_a_missing_type) {
    const SourceFile source = make_source("fn f(a: 1) { }\n");
    const std::vector<ember::ast::Diagnostic> errors = parse_errors(source);
    EMBER_CHECK_EQ(errors.at(0).message,
                   std::string{"expected a type, found an integer literal"});
}

EMBER_TEST(parser_reports_a_struct_field_without_a_trailing_comma) {
    // §3 puts the comma inside `field`, so it is required on the last
    // one too.
    const SourceFile source = make_source("struct S { x: int }\n");
    const std::vector<ember::ast::Diagnostic> errors = parse_errors(source);
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"expected `,`, found `}`"});
}

EMBER_TEST(parser_reports_self_in_a_later_parameter_position) {
    const SourceFile source = make_source("impl P { fn f(a: int, self) { } }\n");
    const std::vector<ember::ast::Diagnostic> errors = parse_errors(source);
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"`self` must be the first parameter"});
    EMBER_CHECK_EQ(errors.at(0).label, std::string{"only the receiver can be `self`"});
}

EMBER_TEST(parser_rejects_calling_an_arbitrary_expression) {
    // v1 has no function values (§4), so this gets a real explanation
    // rather than a confusing "expected `;`".
    const SourceFile source = make_source("fn f() { let y = (a)(1); }\n");
    const std::vector<ember::ast::Diagnostic> errors = parse_errors(source);
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"this expression cannot be called"});
    EMBER_CHECK_MSG(errors.at(0).label.find("first-class") != std::string::npos,
                    "label was: " + errors.at(0).label);
}

EMBER_TEST(parser_rejects_pub_on_an_impl_block) {
    const SourceFile source = make_source("pub impl P { }\n");
    const std::vector<ember::ast::Diagnostic> errors = parse_errors(source);
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"`impl` blocks cannot be `pub`"});
}

EMBER_TEST(parser_reports_an_unclosed_block_at_end_of_file) {
    const SourceFile source = make_source("fn f() {\n");
    const std::vector<ember::ast::Diagnostic> errors = parse_errors(source);
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"expected `}`, found end of file"});
}

EMBER_TEST(parser_points_the_caret_at_the_offending_token) {
    const SourceFile source = SourceFile{"bad.em", "fn f() { let x = 1 }\n"};
    const ember::parser::ParseResult result = ember::parser::parse_source(source);

    EMBER_CHECK_EQ(ember::ast::render(result.diagnostics.at(0), source),
                   std::string{"error: expected `;`, found `}`\n"
                               " --> bad.em:1:20\n"
                               "  |\n"
                               "1 | fn f() { let x = 1 }\n"
                               "  |                    ^ expected `;`\n"});
}

// ---------------------------------------------------------------------
// Error recovery
// ---------------------------------------------------------------------

EMBER_TEST(parser_recovers_at_the_next_item) {
    // The broken function must not swallow the good ones after it.
    const SourceFile source = make_source(
        "fn broken( { }\n"
        "fn good_one() { }\n"
        "fn good_two() { }\n");
    const ember::parser::ParseResult result = ember::parser::parse_source(source);

    EMBER_CHECK(!result.ok());
    std::string names;
    for (const ember::ast::ItemPtr& item : result.program->items) {
        if (const auto* function = ember::ast::node_cast<ember::ast::FunctionDecl>(item.get())) {
            names += function->name + " ";
        }
    }
    EMBER_CHECK_EQ(names, std::string{"good_one good_two "});
}

EMBER_TEST(parser_recovers_at_the_next_statement) {
    const SourceFile source = make_source(
        "fn f() {\n"
        "    let a = ;\n"
        "    let b = 2;\n"
        "    let c = 3;\n"
        "}\n");
    const ember::parser::ParseResult result = ember::parser::parse_source(source);

    EMBER_CHECK_EQ(result.diagnostics.size(), std::size_t{1});
    const auto* function =
        ember::ast::node_cast<ember::ast::FunctionDecl>(result.program->items.at(0).get());
    EMBER_CHECK_EQ(function->body.statements.size(), std::size_t{2});
}

EMBER_TEST(parser_reports_several_syntax_errors_in_one_run) {
    const SourceFile source = make_source(
        "fn a() { let x = ; }\n"
        "fn b() { let y = ; }\n");
    const std::vector<ember::ast::Diagnostic> errors = parse_errors(source);
    EMBER_CHECK_EQ(errors.size(), std::size_t{2});
}

EMBER_TEST(parser_is_not_run_when_lexing_fails) {
    // A bad token stream produces cascading parse errors that bury the
    // real one, so parse_source stops after the lexer.
    const SourceFile source = make_source("fn f() { let x = @; }\n");
    const std::vector<ember::ast::Diagnostic> errors = parse_errors(source);
    EMBER_CHECK_EQ(errors.size(), std::size_t{1});
    EMBER_CHECK_EQ(errors.at(0).message, std::string{"unexpected character"});
}

EMBER_TEST(parser_accepts_an_empty_file) {
    const SourceFile source = make_source("");
    const ember::parser::ParseResult result = parse_ok(source);
    EMBER_CHECK_EQ(result.program->items.size(), std::size_t{0});
}

EMBER_TEST(parser_accepts_a_file_of_only_comments) {
    const SourceFile source = make_source("// nothing here\n/// not even a doc target\n");
    const ember::parser::ParseResult result = parse_ok(source);
    EMBER_CHECK_EQ(result.program->items.size(), std::size_t{0});
}
