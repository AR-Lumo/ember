// The Ember AST.
//
// Node layout follows the §3 grammar closely enough that each production
// maps to one struct, so a reader can check the parser against the EBNF
// without a translation step.
//
// Ownership is by std::unique_ptr throughout (§2): the tree is a strict
// hierarchy with no sharing, so unique ownership is both sufficient and
// the cheapest thing that cannot leak.
//
// Dispatch is on the `kind` tag rather than virtual functions, in the
// style LLVM itself uses: `node_cast<BinaryExpr>(expr)` yields a typed
// pointer or nullptr. That keeps the nodes plain data, which matters
// once the type checker starts annotating them in Phase 3.

#ifndef EMBER_AST_NODES_HPP
#define EMBER_AST_NODES_HPP

#include "ember/ast/span.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ember::ast {

// ---------------------------------------------------------------------
// Types (as written in source; the type checker resolves them later)
// ---------------------------------------------------------------------

enum class TypeKind {
    Int,
    Float,
    Bool,
    String,
    /// A struct name, resolved in Phase 3.
    Named,
    /// `&T`
    Reference,
    /// `[T; N]`
    Array,
};

struct TypeRef;
using TypeRefPtr = std::unique_ptr<TypeRef>;

struct TypeRef {
    TypeKind kind = TypeKind::Int;
    Span span;
    /// Set for Named.
    std::string name;
    /// Element type for Reference and Array.
    TypeRefPtr element;
    /// Element count for Array.
    std::int64_t length = 0;
};

/// How a type is written in source, for diagnostics: `int`, `&Point`,
/// `[float; 8]`.
std::string type_to_string(const TypeRef& type);

// ---------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------

enum class UnaryOp {
    Negate,  // -x
    Not,     // !x
};

enum class BinaryOp {
    Or,        // ||
    And,       // &&
    Equal,     // ==
    NotEqual,  // !=
    Less,      // <
    Greater,   // >
    LessEq,    // <=
    GreaterEq, // >=
    Add,       // +
    Subtract,  // -
    Multiply,  // *
    Divide,    // /
    Remainder, // %
};

/// The operator as written in source, for diagnostics.
std::string_view unary_op_symbol(UnaryOp op) noexcept;
std::string_view binary_op_symbol(BinaryOp op) noexcept;
/// Lower-case tag used by the AST snapshots.
std::string_view unary_op_name(UnaryOp op) noexcept;
std::string_view binary_op_name(BinaryOp op) noexcept;

enum class ExprKind {
    IntLit,
    FloatLit,
    BoolLit,
    StringLit,
    ArrayLit,
    Name,
    Unary,
    Binary,
    Call,
    MethodCall,
    FieldAccess,
    Index,
    StructLit,
    Cast,
};

struct Expr {
    ExprKind kind;
    Span span;

    virtual ~Expr() = default;

    Expr(const Expr&) = delete;
    Expr& operator=(const Expr&) = delete;

protected:
    Expr(ExprKind node_kind, Span node_span) : kind(node_kind), span(node_span) {}
};

using ExprPtr = std::unique_ptr<Expr>;

struct IntLitExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::IntLit;
    std::int64_t value = 0;

    IntLitExpr(Span span, std::int64_t v) : Expr(kKind, span), value(v) {}
};

struct FloatLitExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::FloatLit;
    double value = 0.0;

    FloatLitExpr(Span span, double v) : Expr(kKind, span), value(v) {}
};

struct BoolLitExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::BoolLit;
    bool value = false;

    BoolLitExpr(Span span, bool v) : Expr(kKind, span), value(v) {}
};

struct StringLitExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::StringLit;
    /// Escape-resolved contents, not the raw source text.
    std::string value;

    StringLitExpr(Span span, std::string v) : Expr(kKind, span), value(std::move(v)) {}
};

/// `[a, b, c]`. Not in the §3 grammar as published - see the note in
/// parser.hpp - but required for `[T; N]` types to be constructible.
struct ArrayLitExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::ArrayLit;
    std::vector<ExprPtr> elements;

    explicit ArrayLitExpr(Span span) : Expr(kKind, span) {}
};

/// A bare identifier, including `self`.
struct NameExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Name;
    std::string name;

    NameExpr(Span span, std::string n) : Expr(kKind, span), name(std::move(n)) {}
};

struct UnaryExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Unary;
    UnaryOp op = UnaryOp::Negate;
    ExprPtr operand;

    UnaryExpr(Span span, UnaryOp o, ExprPtr e)
        : Expr(kKind, span), op(o), operand(std::move(e)) {}
};

struct BinaryExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Binary;
    BinaryOp op = BinaryOp::Add;
    ExprPtr left;
    ExprPtr right;

    BinaryExpr(Span span, BinaryOp o, ExprPtr l, ExprPtr r)
        : Expr(kKind, span), op(o), left(std::move(l)), right(std::move(r)) {}
};

/// `name(args)`. Functions are not first-class in v1 (§4), so the callee
/// is a name rather than an arbitrary expression.
struct CallExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Call;
    std::string callee;
    Span callee_span;
    std::vector<ExprPtr> args;

    CallExpr(Span span, std::string name, Span name_span)
        : Expr(kKind, span), callee(std::move(name)), callee_span(name_span) {}
};

/// `receiver.method(args)`, resolved against the receiver's type in
/// Phase 3 rather than the global function namespace.
struct MethodCallExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::MethodCall;
    ExprPtr receiver;
    std::string method;
    Span method_span;
    std::vector<ExprPtr> args;

    MethodCallExpr(Span span, ExprPtr recv, std::string name, Span name_span)
        : Expr(kKind, span),
          receiver(std::move(recv)),
          method(std::move(name)),
          method_span(name_span) {}
};

struct FieldAccessExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::FieldAccess;
    ExprPtr object;
    std::string field;
    Span field_span;

    FieldAccessExpr(Span span, ExprPtr obj, std::string name, Span name_span)
        : Expr(kKind, span),
          object(std::move(obj)),
          field(std::move(name)),
          field_span(name_span) {}
};

struct IndexExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Index;
    ExprPtr object;
    ExprPtr index;

    IndexExpr(Span span, ExprPtr obj, ExprPtr idx)
        : Expr(kKind, span), object(std::move(obj)), index(std::move(idx)) {}
};

/// One `name: value` pair inside a struct literal.
struct FieldInit {
    Span span;
    std::string name;
    Span name_span;
    ExprPtr value;
};

/// `value as int`. §4 requires numeric conversion to be written out
/// rather than inferred; this is how it is written.
struct CastExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Cast;
    ExprPtr operand;
    TypeRefPtr target;

    CastExpr(Span span, ExprPtr value, TypeRefPtr type)
        : Expr(kKind, span), operand(std::move(value)), target(std::move(type)) {}
};

struct StructLitExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::StructLit;
    std::string type_name;
    Span type_name_span;
    std::vector<FieldInit> fields;

    StructLitExpr(Span span, std::string name, Span name_span)
        : Expr(kKind, span), type_name(std::move(name)), type_name_span(name_span) {}
};

// ---------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------

enum class StmtKind {
    Let,
    Return,
    If,
    While,
    Assign,
    Expr,
    /// Only produced as the `else` arm of an if; `statement` in the
    /// grammar has no bare-block production.
    Block,
};

struct Stmt {
    StmtKind kind;
    Span span;

    virtual ~Stmt() = default;

    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

protected:
    Stmt(StmtKind node_kind, Span node_span) : kind(node_kind), span(node_span) {}
};

using StmtPtr = std::unique_ptr<Stmt>;

/// A braced sequence of statements. Held by value where the grammar says
/// a block is mandatory, which keeps "a function always has a body"
/// unrepresentable-otherwise rather than a runtime check.
struct Block {
    Span span;
    std::vector<StmtPtr> statements;
};

struct LetStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::Let;
    bool is_mutable = false;
    std::string name;
    Span name_span;
    /// Null when the type was inferred from the initializer.
    TypeRefPtr declared_type;
    ExprPtr value;

    LetStmt(Span span, bool mutable_binding, std::string binding, Span binding_span)
        : Stmt(kKind, span),
          is_mutable(mutable_binding),
          name(std::move(binding)),
          name_span(binding_span) {}
};

struct ReturnStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::Return;
    /// Null for a bare `return;`.
    ExprPtr value;

    explicit ReturnStmt(Span span) : Stmt(kKind, span) {}
};

struct BlockStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::Block;
    Block block;

    BlockStmt(Span span, Block body) : Stmt(kKind, span), block(std::move(body)) {}
};

struct IfStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::If;
    ExprPtr condition;
    Block then_block;
    /// Null, a BlockStmt for `else { }`, or an IfStmt for `else if`.
    StmtPtr else_branch;

    explicit IfStmt(Span span) : Stmt(kKind, span) {}
};

struct WhileStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::While;
    ExprPtr condition;
    Block body;

    explicit WhileStmt(Span span) : Stmt(kKind, span) {}
};

/// `lvalue = value;`. Which expressions are valid lvalues is a Phase 3
/// question; the parser accepts any expression on the left so it can
/// report a precise error instead of a syntax error.
struct AssignStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::Assign;
    ExprPtr target;
    ExprPtr value;

    AssignStmt(Span span, ExprPtr lhs, ExprPtr rhs)
        : Stmt(kKind, span), target(std::move(lhs)), value(std::move(rhs)) {}
};

struct ExprStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::Expr;
    ExprPtr expr;

    ExprStmt(Span span, ExprPtr e) : Stmt(kKind, span), expr(std::move(e)) {}
};

// ---------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------

enum class ItemKind {
    Function,
    Struct,
    Impl,
    Const,
};

struct Item {
    ItemKind kind;
    Span span;
    /// Parsed and checked, but not yet enforced: `pub` only starts
    /// mattering when v2 adds modules (§4). Kept so v2 needs no syntax
    /// change.
    bool is_public = false;

    virtual ~Item() = default;

    Item(const Item&) = delete;
    Item& operator=(const Item&) = delete;

protected:
    Item(ItemKind node_kind, Span node_span, bool public_item)
        : kind(node_kind), span(node_span), is_public(public_item) {}
};

using ItemPtr = std::unique_ptr<Item>;

/// How a parameter binds the receiver, if at all.
enum class SelfKind {
    /// An ordinary `name: type` parameter.
    None,
    /// `self`, taken by value.
    Value,
    /// `&self`, taken by reference.
    Reference,
};

struct Param {
    Span span;
    SelfKind self_kind = SelfKind::None;
    /// Empty for a self parameter.
    std::string name;
    Span name_span;
    /// Null for a self parameter; its type is the enclosing impl type.
    TypeRefPtr type;

    bool is_self() const noexcept { return self_kind != SelfKind::None; }
};

struct FunctionDecl : Item {
    static constexpr ItemKind kKind = ItemKind::Function;
    std::string name;
    Span name_span;
    std::vector<Param> params;
    /// Null when the function returns nothing.
    TypeRefPtr return_type;
    Block body;
    /// Set for methods: the impl type they were declared in. Empty for
    /// free functions.
    std::string owner_type;

    FunctionDecl(Span span, bool public_item, std::string fn_name, Span fn_name_span)
        : Item(kKind, span, public_item), name(std::move(fn_name)), name_span(fn_name_span) {}

    /// The first parameter, when it is `self` or `&self`.
    const Param* self_param() const noexcept {
        return (!params.empty() && params.front().is_self()) ? &params.front() : nullptr;
    }
};

struct FieldDecl {
    Span span;
    bool is_public = false;
    std::string name;
    Span name_span;
    TypeRefPtr type;
};

struct StructDecl : Item {
    static constexpr ItemKind kKind = ItemKind::Struct;
    std::string name;
    Span name_span;
    std::vector<FieldDecl> fields;

    StructDecl(Span span, bool public_item, std::string struct_name, Span struct_name_span)
        : Item(kKind, span, public_item),
          name(std::move(struct_name)),
          name_span(struct_name_span) {}
};

struct ImplBlock : Item {
    static constexpr ItemKind kKind = ItemKind::Impl;
    std::string type_name;
    Span type_name_span;
    std::vector<std::unique_ptr<FunctionDecl>> methods;

    ImplBlock(Span span, std::string name, Span name_span)
        : Item(kKind, span, false), type_name(std::move(name)), type_name_span(name_span) {}
};

struct ConstDecl : Item {
    static constexpr ItemKind kKind = ItemKind::Const;
    std::string name;
    Span name_span;
    TypeRefPtr type;
    ExprPtr value;

    ConstDecl(Span span, bool public_item, std::string const_name, Span const_name_span)
        : Item(kKind, span, public_item),
          name(std::move(const_name)),
          name_span(const_name_span) {}
};

/// A whole source file.
struct Program {
    std::vector<ItemPtr> items;
};

// ---------------------------------------------------------------------
// Casting
// ---------------------------------------------------------------------

/// Typed access to a node, or nullptr if it is a different kind:
/// `if (const auto* binary = node_cast<BinaryExpr>(expr)) { ... }`.
template <typename Node, typename Base>
const Node* node_cast(const Base* node) noexcept {
    return (node != nullptr && node->kind == Node::kKind) ? static_cast<const Node*>(node)
                                                          : nullptr;
}

template <typename Node, typename Base>
Node* node_cast(Base* node) noexcept {
    return (node != nullptr && node->kind == Node::kKind) ? static_cast<Node*>(node) : nullptr;
}

}  // namespace ember::ast

#endif  // EMBER_AST_NODES_HPP
