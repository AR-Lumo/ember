// Phase 2 unit tests: the parser.
//
// The golden `.ast` snapshots in snapshot_tests.cpp cover whole files;
// these cover the parts a snapshot reads past - the §3 precedence table,
// associativity, the struct-literal ambiguity, and the wording and
// placement of every syntax error.

#include "test_harness.hpp"

#include "soliton/ast/diagnostic.hpp"
#include "soliton/ast/nodes.hpp"
#include "soliton/ast/printer.hpp"
#include "soliton/ast/span.hpp"
#include "soliton/parser/parser.hpp"

#include <string>
#include <vector>

namespace {

using soliton::ast::Position;
using soliton::ast::SourceFile;
using soliton::parser::ParseResult;

SourceFile make_source(std::string contents) {
    return SourceFile{"test.sn", std::move(contents)};
}

/// Parse a whole program expected to be clean.
ParseResult parse_ok(const SourceFile& source) {
    ParseResult result = soliton::parser::parse_source(source);
    if (!result.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "unexpected parse errors:\n" +
                                soliton::ast::render_all(result.diagnostics, source));
    }
    return result;
}

/// Parse a program expected to fail, and return its diagnostics.
std::vector<soliton::ast::Diagnostic> parse_errors(const SourceFile& source) {
    ParseResult result = soliton::parser::parse_source(source);
    if (result.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__, "expected parse errors, but parsing succeeded");
    }
    return result.diagnostics;
}

/// Parse a bare expression by wrapping it in a function, and render the
/// resulting tree as a single line so precedence is easy to assert on.
std::string expr_tree(const std::string& expression) {
    const SourceFile source = make_source("fn f() { let x = " + expression + "; }\n");
    const ParseResult result = parse_ok(source);

    const auto* function =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* let = soliton::ast::node_cast<soliton::ast::LetStmt>(
        function->body.statements.at(0).get());

    // Collapse the indented s-expression onto one line.
    std::string tree = soliton::ast::to_sexpr(*let->value, source);
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

SOLITON_TEST(parser_binds_multiplication_tighter_than_addition) {
    SOLITON_CHECK_EQ(shape_of("1 + 2 * 3"),
                   std::string{"(binary add(int-lit 1)(binary mul(int-lit 2)(int-lit 3)))"});
    SOLITON_CHECK_EQ(shape_of("1 * 2 + 3"),
                   std::string{"(binary add(binary mul(int-lit 1)(int-lit 2))(int-lit 3))"});
}

SOLITON_TEST(parser_binds_remainder_like_multiplication) {
    SOLITON_CHECK_EQ(shape_of("1 + 2 % 3"),
                   std::string{"(binary add(int-lit 1)(binary rem(int-lit 2)(int-lit 3)))"});
}

SOLITON_TEST(parser_follows_the_full_precedence_ladder) {
    // ||  <  &&  <  == !=  <  < > <= >=  <  + -  <  * / %
    SOLITON_CHECK_EQ(
        shape_of("a || b && c == d < e + f * g"),
        std::string{"(binary or(name a)(binary and(name b)(binary eq(name c)(binary lt(name d)"
                    "(binary add(name e)(binary mul(name f)(name g)))))))"});
}

SOLITON_TEST(parser_makes_binary_operators_left_associative) {
    SOLITON_CHECK_EQ(shape_of("1 - 2 - 3"),
                   std::string{"(binary sub(binary sub(int-lit 1)(int-lit 2))(int-lit 3))"});
    SOLITON_CHECK_EQ(shape_of("1 / 2 / 3"),
                   std::string{"(binary div(binary div(int-lit 1)(int-lit 2))(int-lit 3))"});
}

SOLITON_TEST(parser_binds_unary_tighter_than_binary) {
    SOLITON_CHECK_EQ(shape_of("-1 + 2"),
                   std::string{"(binary add(unary neg(int-lit 1))(int-lit 2))"});
    SOLITON_CHECK_EQ(shape_of("!a && b"),
                   std::string{"(binary and(unary not(name a))(name b))"});
}

SOLITON_TEST(parser_stacks_unary_operators) {
    SOLITON_CHECK_EQ(shape_of("--1"), std::string{"(unary neg(unary neg(int-lit 1)))"});
    SOLITON_CHECK_EQ(shape_of("!!a"), std::string{"(unary not(unary not(name a)))"});
}

SOLITON_TEST(parser_binds_postfix_tighter_than_unary) {
    // -a.b is -(a.b), not (-a).b
    SOLITON_CHECK_EQ(shape_of("-a.b"), std::string{"(unary neg(field-get b(name a)))"});
    SOLITON_CHECK_EQ(shape_of("-a[0]"),
                   std::string{"(unary neg(index(name a)(int-lit 0)))"});
}

SOLITON_TEST(parser_binds_as_tighter_than_binary_operators) {
    // `a as float * b` is `(a as float) * b`, not `a as (float * b)`.
    SOLITON_CHECK_EQ(shape_of("a as float * b"),
                   std::string{"(binary mul(cast(float)(name a))(name b))"});
    SOLITON_CHECK_EQ(shape_of("1 + 2 as float"),
                   std::string{"(binary add(int-lit 1)(cast(float)(int-lit 2)))"});
}

SOLITON_TEST(parser_binds_as_looser_than_unary) {
    // `-x as float` is `(-x) as float`, following Rust.
    SOLITON_CHECK_EQ(shape_of("-x as float"),
                   std::string{"(cast(float)(unary neg(name x)))"});
}

SOLITON_TEST(parser_chains_casts_left_to_right) {
    SOLITON_CHECK_EQ(shape_of("x as float as int"),
                   std::string{"(cast(int)(cast(float)(name x)))"});
}

SOLITON_TEST(parser_reads_a_cast_to_every_type_form) {
    SOLITON_CHECK_EQ(shape_of("x as int"), std::string{"(cast(int)(name x))"});
    SOLITON_CHECK_EQ(shape_of("x as Point"), std::string{"(cast(named Point)(name x))"});
    SOLITON_CHECK_EQ(shape_of("x as &int"), std::string{"(cast(ref(int))(name x))"});
}

SOLITON_TEST(parser_lets_parentheses_override_precedence) {
    SOLITON_CHECK_EQ(shape_of("(1 + 2) * 3"),
                   std::string{"(binary mul(binary add(int-lit 1)(int-lit 2))(int-lit 3))"});
}

SOLITON_TEST(parser_keeps_no_node_for_parentheses) {
    // Grouping is recorded by the shape of the tree, so `(1)` and `1`
    // parse identically.
    SOLITON_CHECK_EQ(shape_of("(((1)))"), std::string{"(int-lit 1)"});
}

// ---------------------------------------------------------------------
// Postfix chains
// ---------------------------------------------------------------------

SOLITON_TEST(parser_chains_field_access_index_and_method_calls) {
    SOLITON_CHECK_EQ(shape_of("a.b[0].c(1)"),
                   std::string{"(method-call c(index(field-get b(name a))(int-lit 0))"
                               "(int-lit 1))"});
}

SOLITON_TEST(parser_reads_a_call_with_no_arguments) {
    SOLITON_CHECK_EQ(shape_of("main()"), std::string{"(call main)"});
}

SOLITON_TEST(parser_reads_method_calls_on_self) {
    SOLITON_CHECK_EQ(shape_of("self.distance_sq(other)"),
                   std::string{"(method-call distance_sq(name self)(name other))"});
}

SOLITON_TEST(parser_distinguishes_field_access_from_a_method_call) {
    SOLITON_CHECK_EQ(shape_of("p.x"), std::string{"(field-get x(name p))"});
    SOLITON_CHECK_EQ(shape_of("p.x()"), std::string{"(method-call x(name p))"});
}

// ---------------------------------------------------------------------
// Literals
// ---------------------------------------------------------------------

SOLITON_TEST(parser_reads_every_literal_form) {
    SOLITON_CHECK_EQ(shape_of("1"), std::string{"(int-lit 1)"});
    SOLITON_CHECK_EQ(shape_of("1.5"), std::string{"(float-lit 1.5)"});
    SOLITON_CHECK_EQ(shape_of("true"), std::string{"(bool-lit true)"});
    SOLITON_CHECK_EQ(shape_of("\"hi\""), std::string{"(string-lit \"hi\")"});
    SOLITON_CHECK_EQ(shape_of("[1, 2, 3]"),
                   std::string{"(array-lit(int-lit 1)(int-lit 2)(int-lit 3))"});
    SOLITON_CHECK_EQ(shape_of("[]"), std::string{"(array-lit)"});
}

SOLITON_TEST(parser_allows_a_trailing_comma_in_an_array_literal) {
    SOLITON_CHECK_EQ(shape_of("[1, 2,]"), std::string{"(array-lit(int-lit 1)(int-lit 2))"});
}

SOLITON_TEST(parser_reads_struct_literals) {
    SOLITON_CHECK_EQ(shape_of("Point { x: 1, y: 2 }"),
                   std::string{"(struct-lit Point(init x(int-lit 1))(init y(int-lit 2)))"});
    SOLITON_CHECK_EQ(shape_of("Empty {}"), std::string{"(struct-lit Empty)"});
}

// ---------------------------------------------------------------------
// The struct-literal / block ambiguity
// ---------------------------------------------------------------------

SOLITON_TEST(parser_does_not_read_a_struct_literal_in_an_if_condition) {
    // Without the restriction, `if flag { }` would parse `flag { }` as a
    // struct literal and then demand a block that is not there.
    const SourceFile source = make_source("fn f() { if flag { let x = 1; } }\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* function =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* branch =
        soliton::ast::node_cast<soliton::ast::IfStmt>(function->body.statements.at(0).get());
    SOLITON_CHECK(branch != nullptr);
    SOLITON_CHECK(soliton::ast::node_cast<soliton::ast::NameExpr>(branch->condition.get()) != nullptr);
    SOLITON_CHECK_EQ(branch->then_block.statements.size(), std::size_t{1});
}

SOLITON_TEST(parser_does_not_read_a_struct_literal_in_a_while_condition) {
    const SourceFile source = make_source("fn f() { while running { let x = 1; } }\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* function =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    SOLITON_CHECK(soliton::ast::node_cast<soliton::ast::WhileStmt>(
                    function->body.statements.at(0).get()) != nullptr);
}

SOLITON_TEST(parser_allows_a_struct_literal_in_a_condition_inside_parentheses) {
    // The documented escape hatch, same as Rust's.
    const SourceFile source = make_source("fn f() { if (Flag { on: true }).on { } }\n");
    const soliton::parser::ParseResult result = parse_ok(source);
    SOLITON_CHECK_EQ(result.program->items.size(), std::size_t{1});
}

SOLITON_TEST(parser_still_reads_struct_literals_in_ordinary_positions) {
    SOLITON_CHECK_EQ(shape_of("Point { x: 1 }"),
                   std::string{"(struct-lit Point(init x(int-lit 1)))"});
}

// ---------------------------------------------------------------------
// Items and statements
// ---------------------------------------------------------------------

SOLITON_TEST(parser_records_visibility_on_items) {
    const SourceFile source = make_source(
        "pub fn a() { }\n"
        "fn b() { }\n"
        "pub struct S { x: int, }\n"
        "pub const C: int = 1;\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    SOLITON_CHECK_EQ(result.program->items.at(0)->is_public, true);
    SOLITON_CHECK_EQ(result.program->items.at(1)->is_public, false);
    SOLITON_CHECK_EQ(result.program->items.at(2)->is_public, true);
    SOLITON_CHECK_EQ(result.program->items.at(3)->is_public, true);
}

SOLITON_TEST(parser_reads_both_self_receiver_forms) {
    const SourceFile source = make_source(
        "impl Point {\n"
        "    fn by_value(self) { }\n"
        "    fn by_reference(&self) { }\n"
        "    fn free() { }\n"
        "}\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* block =
        soliton::ast::node_cast<soliton::ast::ImplBlock>(result.program->items.at(0).get());
    SOLITON_CHECK_EQ(block->methods.size(), std::size_t{3});
    SOLITON_CHECK(block->methods[0]->self_param()->self_kind == soliton::ast::SelfKind::Value);
    SOLITON_CHECK(block->methods[1]->self_param()->self_kind == soliton::ast::SelfKind::Reference);
    SOLITON_CHECK(block->methods[2]->self_param() == nullptr);
}

SOLITON_TEST(parser_records_the_owning_type_on_methods) {
    // Methods are scoped to their impl type, not the global function
    // namespace; Phase 3 resolves `p.f()` through this.
    const SourceFile source = make_source("impl Point { fn area(&self) -> int { return 1; } }\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* block =
        soliton::ast::node_cast<soliton::ast::ImplBlock>(result.program->items.at(0).get());
    SOLITON_CHECK_EQ(block->methods.at(0)->owner_type, std::string{"Point"});
}

SOLITON_TEST(parser_reads_every_type_form) {
    const SourceFile source = make_source(
        "fn f(a: int, b: float, c: bool, d: string, e: Point, g: &Point, h: [int; 4]) { }\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* function =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    std::string types;
    for (const soliton::ast::Param& param : function->params) {
        if (!types.empty()) {
            types += ' ';
        }
        types += soliton::ast::type_to_string(*param.type);
    }
    SOLITON_CHECK_EQ(types, std::string{"int float bool string Point &Point [int; 4]"});
}

SOLITON_TEST(parser_reads_nested_reference_and_array_types) {
    const SourceFile source = make_source("fn f(a: &[&int; 2]) { }\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* function =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    SOLITON_CHECK_EQ(soliton::ast::type_to_string(*function->params.at(0).type),
                   std::string{"&[&int; 2]"});
}

SOLITON_TEST(parser_distinguishes_annotated_and_inferred_let) {
    const SourceFile source = make_source("fn f() { let a: int = 1; let b = 2; let mut c = 3; }\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* function =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* annotated =
        soliton::ast::node_cast<soliton::ast::LetStmt>(function->body.statements.at(0).get());
    const auto* inferred =
        soliton::ast::node_cast<soliton::ast::LetStmt>(function->body.statements.at(1).get());
    const auto* mutable_binding =
        soliton::ast::node_cast<soliton::ast::LetStmt>(function->body.statements.at(2).get());

    SOLITON_CHECK(annotated->declared_type != nullptr);
    SOLITON_CHECK(inferred->declared_type == nullptr);
    SOLITON_CHECK_EQ(annotated->is_mutable, false);
    SOLITON_CHECK_EQ(mutable_binding->is_mutable, true);
}

SOLITON_TEST(parser_reads_bare_and_valued_returns) {
    const SourceFile source = make_source("fn f() { return; }\nfn g() -> int { return 1; }\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* f =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* g =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(1).get());

    SOLITON_CHECK(soliton::ast::node_cast<soliton::ast::ReturnStmt>(f->body.statements.at(0).get())
                    ->value == nullptr);
    SOLITON_CHECK(soliton::ast::node_cast<soliton::ast::ReturnStmt>(g->body.statements.at(0).get())
                    ->value != nullptr);
    SOLITON_CHECK(f->return_type == nullptr);
    SOLITON_CHECK(g->return_type != nullptr);
}

SOLITON_TEST(parser_chains_else_if) {
    const SourceFile source = make_source(
        "fn f() {\n"
        "    if a { } else if b { } else { }\n"
        "}\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* function =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* first =
        soliton::ast::node_cast<soliton::ast::IfStmt>(function->body.statements.at(0).get());
    const auto* second = soliton::ast::node_cast<soliton::ast::IfStmt>(first->else_branch.get());

    SOLITON_CHECK(second != nullptr);
    SOLITON_CHECK(soliton::ast::node_cast<soliton::ast::BlockStmt>(second->else_branch.get()) !=
                nullptr);
}

SOLITON_TEST(parser_separates_assignment_from_an_expression_statement) {
    const SourceFile source = make_source("fn f() { total = 1; step(); p.x = 2; a[0] = 3; }\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* function =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    SOLITON_CHECK(function->body.statements.at(0)->kind == soliton::ast::StmtKind::Assign);
    SOLITON_CHECK(function->body.statements.at(1)->kind == soliton::ast::StmtKind::Expr);
    SOLITON_CHECK(function->body.statements.at(2)->kind == soliton::ast::StmtKind::Assign);
    SOLITON_CHECK(function->body.statements.at(3)->kind == soliton::ast::StmtKind::Assign);
}

// ---------------------------------------------------------------------
// Spans
// ---------------------------------------------------------------------

SOLITON_TEST(parser_gives_a_binary_expression_a_span_covering_both_operands) {
    const SourceFile source = make_source("fn f() { let x = 10 + 20; }\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* function =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* let =
        soliton::ast::node_cast<soliton::ast::LetStmt>(function->body.statements.at(0).get());

    SOLITON_CHECK_EQ(std::string{source.text_of(let->value->span)}, std::string{"10 + 20"});
}

SOLITON_TEST(parser_gives_each_item_a_span_covering_the_whole_declaration) {
    const SourceFile source = make_source("pub fn f() -> int {\n    return 1;\n}\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    SOLITON_CHECK_EQ(std::string{source.text_of(result.program->items.at(0)->span)},
                   std::string{"pub fn f() -> int {\n    return 1;\n}"});
}

SOLITON_TEST(parser_spans_a_method_call_from_receiver_to_closing_paren) {
    const SourceFile source = make_source("fn f() { let d = p.distance_sq(q); }\n");
    const soliton::parser::ParseResult result = parse_ok(source);

    const auto* function =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    const auto* let =
        soliton::ast::node_cast<soliton::ast::LetStmt>(function->body.statements.at(0).get());

    SOLITON_CHECK_EQ(std::string{source.text_of(let->value->span)},
                   std::string{"p.distance_sq(q)"});
}

// ---------------------------------------------------------------------
// Syntax errors (§7)
// ---------------------------------------------------------------------

SOLITON_TEST(parser_reports_a_missing_semicolon) {
    const SourceFile source = make_source("fn f() { let x = 1 }\n");
    const std::vector<soliton::ast::Diagnostic> errors = parse_errors(source);
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"expected `;`, found `}`"});
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"expected `;`"});
}

SOLITON_TEST(parser_reports_a_missing_expression) {
    const SourceFile source = make_source("fn f() { let x = ; }\n");
    const std::vector<soliton::ast::Diagnostic> errors = parse_errors(source);
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"expected an expression, found `;`"});
}

SOLITON_TEST(parser_reports_a_bad_item_keyword) {
    const SourceFile source = make_source("let x = 1;\n");
    const std::vector<soliton::ast::Diagnostic> errors = parse_errors(source);
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"expected an item, found `let`"});
}

SOLITON_TEST(parser_reports_a_missing_type) {
    const SourceFile source = make_source("fn f(a: 1) { }\n");
    const std::vector<soliton::ast::Diagnostic> errors = parse_errors(source);
    SOLITON_CHECK_EQ(errors.at(0).message,
                   std::string{"expected a type, found an integer literal"});
}

SOLITON_TEST(parser_reports_a_struct_field_without_a_trailing_comma) {
    // §3 puts the comma inside `field`, so it is required on the last
    // one too.
    const SourceFile source = make_source("struct S { x: int }\n");
    const std::vector<soliton::ast::Diagnostic> errors = parse_errors(source);
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"expected `,`, found `}`"});
}

SOLITON_TEST(parser_reports_self_in_a_later_parameter_position) {
    const SourceFile source = make_source("impl P { fn f(a: int, self) { } }\n");
    const std::vector<soliton::ast::Diagnostic> errors = parse_errors(source);
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"`self` must be the first parameter"});
    SOLITON_CHECK_EQ(errors.at(0).label, std::string{"only the receiver can be `self`"});
}

SOLITON_TEST(parser_rejects_calling_an_arbitrary_expression) {
    // v1 has no function values (§4), so this gets a real explanation
    // rather than a confusing "expected `;`".
    const SourceFile source = make_source("fn f() { let y = (a)(1); }\n");
    const std::vector<soliton::ast::Diagnostic> errors = parse_errors(source);
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"this expression cannot be called"});
    SOLITON_CHECK_MSG(errors.at(0).label.find("first-class") != std::string::npos,
                    "label was: " + errors.at(0).label);
}

SOLITON_TEST(parser_rejects_pub_on_an_impl_block) {
    const SourceFile source = make_source("pub impl P { }\n");
    const std::vector<soliton::ast::Diagnostic> errors = parse_errors(source);
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"`impl` blocks cannot be `pub`"});
}

SOLITON_TEST(parser_reports_an_unclosed_block_at_end_of_file) {
    const SourceFile source = make_source("fn f() {\n");
    const std::vector<soliton::ast::Diagnostic> errors = parse_errors(source);
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"expected `}`, found end of file"});
}

SOLITON_TEST(parser_points_the_caret_at_the_offending_token) {
    const SourceFile source = SourceFile{"bad.sn", "fn f() { let x = 1 }\n"};
    const soliton::parser::ParseResult result = soliton::parser::parse_source(source);

    SOLITON_CHECK_EQ(soliton::ast::render(result.diagnostics.at(0), source),
                   std::string{"error: expected `;`, found `}`\n"
                               " --> bad.sn:1:20\n"
                               "  |\n"
                               "1 | fn f() { let x = 1 }\n"
                               "  |                    ^ expected `;`\n"});
}

// ---------------------------------------------------------------------
// Error recovery
// ---------------------------------------------------------------------

SOLITON_TEST(parser_recovers_at_the_next_item) {
    // The broken function must not swallow the good ones after it.
    const SourceFile source = make_source(
        "fn broken( { }\n"
        "fn good_one() { }\n"
        "fn good_two() { }\n");
    const soliton::parser::ParseResult result = soliton::parser::parse_source(source);

    SOLITON_CHECK(!result.ok());
    std::string names;
    for (const soliton::ast::ItemPtr& item : result.program->items) {
        if (const auto* function = soliton::ast::node_cast<soliton::ast::FunctionDecl>(item.get())) {
            names += function->name + " ";
        }
    }
    SOLITON_CHECK_EQ(names, std::string{"good_one good_two "});
}

SOLITON_TEST(parser_recovers_at_the_next_statement) {
    const SourceFile source = make_source(
        "fn f() {\n"
        "    let a = ;\n"
        "    let b = 2;\n"
        "    let c = 3;\n"
        "}\n");
    const soliton::parser::ParseResult result = soliton::parser::parse_source(source);

    SOLITON_CHECK_EQ(result.diagnostics.size(), std::size_t{1});
    const auto* function =
        soliton::ast::node_cast<soliton::ast::FunctionDecl>(result.program->items.at(0).get());
    SOLITON_CHECK_EQ(function->body.statements.size(), std::size_t{2});
}

SOLITON_TEST(parser_reports_several_syntax_errors_in_one_run) {
    const SourceFile source = make_source(
        "fn a() { let x = ; }\n"
        "fn b() { let y = ; }\n");
    const std::vector<soliton::ast::Diagnostic> errors = parse_errors(source);
    SOLITON_CHECK_EQ(errors.size(), std::size_t{2});
}

SOLITON_TEST(parser_is_not_run_when_lexing_fails) {
    // A bad token stream produces cascading parse errors that bury the
    // real one, so parse_source stops after the lexer.
    const SourceFile source = make_source("fn f() { let x = @; }\n");
    const std::vector<soliton::ast::Diagnostic> errors = parse_errors(source);
    SOLITON_CHECK_EQ(errors.size(), std::size_t{1});
    SOLITON_CHECK_EQ(errors.at(0).message, std::string{"unexpected character"});
}

SOLITON_TEST(parser_accepts_an_empty_file) {
    const SourceFile source = make_source("");
    const soliton::parser::ParseResult result = parse_ok(source);
    SOLITON_CHECK_EQ(result.program->items.size(), std::size_t{0});
}

SOLITON_TEST(parser_accepts_a_file_of_only_comments) {
    const SourceFile source = make_source("// nothing here\n/// not even a doc target\n");
    const soliton::parser::ParseResult result = parse_ok(source);
    SOLITON_CHECK_EQ(result.program->items.size(), std::size_t{0});
}
