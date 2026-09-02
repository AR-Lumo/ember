// S-expression rendering of the AST, for the `.ast` golden snapshots.
//
// Every node prints its start position as `@line:col`, so the snapshots
// double as the regression test that spans are attached and correct -
// Phase 2 requires spans on every node, and a snapshot that showed only
// structure could not catch a wrong one.

#ifndef EMBER_AST_PRINTER_HPP
#define EMBER_AST_PRINTER_HPP

#include "ember/ast/nodes.hpp"
#include "ember/ast/span.hpp"

#include <string>

namespace ember::ast {

/// Render a whole program as an indented s-expression, newline-terminated.
std::string to_sexpr(const Program& program, const SourceFile& source);

/// Render a single expression. Used by tests that care about one tree,
/// such as the operator-precedence checks.
std::string to_sexpr(const Expr& expr, const SourceFile& source);

}  // namespace ember::ast

#endif  // EMBER_AST_PRINTER_HPP
