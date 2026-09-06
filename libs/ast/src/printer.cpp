#include "cinder/ast/printer.hpp"

#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <system_error>

namespace cinder::ast {
namespace {

std::string format_double(double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        return "<unrepresentable>";
    }
    return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char c : text) {
        switch (c) {
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\0':
                out += "\\0";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

class Printer {
public:
    explicit Printer(const SourceFile& source) : source_(source) {}

    std::string take() { return out_; }

    void program(const Program& node) {
        open("program", std::nullopt);
        for (const ItemPtr& item : node.items) {
            print_item(*item);
        }
        close();
    }

    /// A written unit, as it was written: `meters/seconds^2`.
    ///
    /// Left to right, so the printed form and the source agree about
    /// what is on top and what is underneath.
    static std::string unit_to_string(const std::vector<UnitFactor>& unit) {
        std::string out;
        for (const UnitFactor& factor : unit) {
            if (!out.empty()) {
                out += factor.sign < 0 ? "/" : "*";
            } else if (factor.sign < 0) {
                out += "1/";
            }
            out += factor.name;
            if (factor.power != 1) {
                out += "^" + std::to_string(factor.power);
            }
        }
        return out;
    }

    void print_expr(const Expr& node) {
        switch (node.kind) {
            case ExprKind::IntLit: {
                const auto& literal = static_cast<const IntLitExpr&>(node);
                std::string text = std::to_string(literal.value);
                if (!literal.unit.empty()) {
                    text += "<" + unit_to_string(literal.unit) + ">";
                }
                leaf("int-lit", node.span, text);
                return;
            }
            case ExprKind::FloatLit: {
                const auto& literal = static_cast<const FloatLitExpr&>(node);
                std::string text = format_double(literal.value);
                if (!literal.unit.empty()) {
                    text += "<" + unit_to_string(literal.unit) + ">";
                }
                leaf("float-lit", node.span, text);
                return;
            }
            case ExprKind::BoolLit:
                leaf("bool-lit", node.span,
                     static_cast<const BoolLitExpr&>(node).value ? "true" : "false");
                return;
            case ExprKind::StringLit:
                leaf("string-lit", node.span,
                     "\"" + escape(static_cast<const StringLitExpr&>(node).value) + "\"");
                return;
            case ExprKind::Name:
                leaf("name", node.span, static_cast<const NameExpr&>(node).name);
                return;

            case ExprKind::ArrayLit: {
                const auto& array = static_cast<const ArrayLitExpr&>(node);
                if (array.elements.empty()) {
                    leaf("array-lit", node.span, {});
                    return;
                }
                open("array-lit", node.span);
                for (const ExprPtr& element : array.elements) {
                    print_expr(*element);
                }
                close();
                return;
            }

            case ExprKind::Unary: {
                const auto& unary = static_cast<const UnaryExpr&>(node);
                open("unary", node.span, std::string{unary_op_name(unary.op)});
                print_expr(*unary.operand);
                close();
                return;
            }

            case ExprKind::Binary: {
                const auto& binary = static_cast<const BinaryExpr&>(node);
                open("binary", node.span, std::string{binary_op_name(binary.op)});
                print_expr(*binary.left);
                print_expr(*binary.right);
                close();
                return;
            }

            case ExprKind::Call: {
                const auto& call = static_cast<const CallExpr&>(node);
                if (call.args.empty()) {
                    leaf("call", node.span, call.callee);
                    return;
                }
                open("call", node.span, call.callee);
                for (const ExprPtr& arg : call.args) {
                    print_expr(*arg);
                }
                close();
                return;
            }

            case ExprKind::MethodCall: {
                const auto& call = static_cast<const MethodCallExpr&>(node);
                open("method-call", node.span, call.method);
                print_expr(*call.receiver);
                for (const ExprPtr& arg : call.args) {
                    print_expr(*arg);
                }
                close();
                return;
            }

            case ExprKind::FieldAccess: {
                const auto& access = static_cast<const FieldAccessExpr&>(node);
                open("field-get", node.span, access.field);
                print_expr(*access.object);
                close();
                return;
            }

            case ExprKind::Index: {
                const auto& index = static_cast<const IndexExpr&>(node);
                open("index", node.span);
                print_expr(*index.object);
                print_expr(*index.index);
                close();
                return;
            }

            case ExprKind::Closure: {
                const auto& closure = static_cast<const ClosureExpr&>(node);
                open("closure", node.span);
                if (closure.params.empty()) {
                    empty_group("params");
                } else {
                    open("params", std::nullopt);
                    for (const Param& param : closure.params) {
                        open("param", param.span, param.name);
                        // A closure parameter may have no written type,
                        // and take it from what the closure is passed
                        // to. The tree shows what was written.
                        if (param.type) {
                            print_type(*param.type);
                        } else {
                            empty_group("inferred");
                        }
                        close();
                    }
                    close();
                }
                if (closure.return_type) {
                    open("returns", std::nullopt);
                    print_type(*closure.return_type);
                    close();
                }
                print_block(closure.body);
                close();
                return;
            }

            case ExprKind::Cast: {
                const auto& cast = static_cast<const CastExpr&>(node);
                open("cast", node.span);
                print_type(*cast.target);
                print_expr(*cast.operand);
                close();
                return;
            }

            case ExprKind::StructLit: {
                const auto& literal = static_cast<const StructLitExpr&>(node);
                open("struct-lit", node.span, literal.type_name);
                for (const FieldInit& field : literal.fields) {
                    open("init", field.span, field.name);
                    print_expr(*field.value);
                    close();
                }
                close();
                return;
            }
        }
    }

private:
    const SourceFile& source_;
    std::string out_;
    int depth_ = 0;

    std::string at(Span span) const {
        const Position position = source_.position_of(span.start);
        return "@" + std::to_string(position.line) + ":" + std::to_string(position.column);
    }

    void indent() { out_.append(static_cast<std::size_t>(depth_) * 2, ' '); }

    /// Open a node that has children; `close()` writes the paren.
    void open(std::string_view label, std::optional<Span> span, std::string extra = {}) {
        indent();
        out_ += '(';
        out_ += label;
        if (span.has_value()) {
            out_ += ' ';
            out_ += at(*span);
        }
        if (!extra.empty()) {
            out_ += ' ';
            out_ += extra;
        }
        out_ += '\n';
        ++depth_;
    }

    void close() {
        --depth_;
        // The closing paren rides on the previous line, so the snapshots
        // do not end in a column of lone brackets.
        while (!out_.empty() && out_.back() == '\n') {
            out_.pop_back();
        }
        out_ += ")\n";
    }

    /// A grouping label such as `params`, which is a shape in the
    /// printout rather than a node, so it carries no position.
    void empty_group(std::string_view label) {
        indent();
        out_ += '(';
        out_ += label;
        out_ += ")\n";
    }

    void leaf(std::string_view label, Span span, std::string extra) {
        indent();
        out_ += '(';
        out_ += label;
        out_ += ' ';
        out_ += at(span);
        if (!extra.empty()) {
            out_ += ' ';
            out_ += extra;
        }
        out_ += ")\n";
    }

    void print_type(const TypeRef& type) {
        switch (type.kind) {
            case TypeKind::Int:
            case TypeKind::Float:
                leaf(type_to_string(type), type.span,
                     type.unit.empty() ? std::string{} : unit_to_string(type.unit));
                return;
            case TypeKind::Bool:
            case TypeKind::String:
                leaf(type_to_string(type), type.span, {});
                return;
            case TypeKind::Named:
                leaf("named", type.span, type.name);
                return;
            case TypeKind::Reference:
                open("ref", type.span);
                print_type(*type.element);
                close();
                return;
            case TypeKind::Array:
                open("array", type.span, std::to_string(type.length));
                print_type(*type.element);
                close();
                return;
            case TypeKind::Function:
                open("fn-type", type.span);
                for (const TypeRefPtr& param : type.params) {
                    print_type(*param);
                }
                if (type.result) {
                    open("returns", std::nullopt);
                    print_type(*type.result);
                    close();
                }
                close();
                return;
        }
    }

    void print_block(const Block& block) {
        if (block.statements.empty()) {
            leaf("block", block.span, {});
            return;
        }
        open("block", block.span);
        for (const StmtPtr& statement : block.statements) {
            print_stmt(*statement);
        }
        close();
    }

    void print_stmt(const Stmt& node) {
        switch (node.kind) {
            case StmtKind::Let: {
                const auto& let = static_cast<const LetStmt&>(node);
                std::string header = let.name;
                if (let.is_mutable) {
                    header = "mut " + header;
                }
                open("let", node.span, header);
                if (let.declared_type) {
                    print_type(*let.declared_type);
                }
                print_expr(*let.value);
                close();
                return;
            }

            case StmtKind::Return: {
                const auto& statement = static_cast<const ReturnStmt&>(node);
                if (!statement.value) {
                    leaf("return", node.span, {});
                    return;
                }
                open("return", node.span);
                print_expr(*statement.value);
                close();
                return;
            }

            case StmtKind::If: {
                const auto& statement = static_cast<const IfStmt&>(node);
                open("if", node.span);
                print_expr(*statement.condition);
                print_block(statement.then_block);
                if (statement.else_branch) {
                    print_stmt(*statement.else_branch);
                }
                close();
                return;
            }

            case StmtKind::While: {
                const auto& statement = static_cast<const WhileStmt&>(node);
                open("while", node.span);
                print_expr(*statement.condition);
                print_block(statement.body);
                close();
                return;
            }

            case StmtKind::Assign: {
                const auto& statement = static_cast<const AssignStmt&>(node);
                open("assign", node.span);
                print_expr(*statement.target);
                print_expr(*statement.value);
                close();
                return;
            }

            case StmtKind::Expr: {
                const auto& statement = static_cast<const ExprStmt&>(node);
                open("expr-stmt", node.span);
                print_expr(*statement.expr);
                close();
                return;
            }

            case StmtKind::Block: {
                const auto& statement = static_cast<const BlockStmt&>(node);
                open("else", node.span);
                print_block(statement.block);
                close();
                return;
            }
        }
    }

    void print_function(const FunctionDecl& function) {
        std::string header = function.is_public ? "pub " : "";
        header += function.name;
        open("fn", function.span, header);

        if (function.params.empty()) {
            empty_group("params");
        } else {
            open("params", std::nullopt);
            for (const Param& param : function.params) {
                switch (param.self_kind) {
                    case SelfKind::Value:
                        leaf("self", param.span, {});
                        break;
                    case SelfKind::Reference:
                        leaf("self-ref", param.span, {});
                        break;
                    case SelfKind::None:
                        open("param", param.span, param.name);
                        print_type(*param.type);
                        close();
                        break;
                }
            }
            close();
        }

        if (function.return_type) {
            open("returns", std::nullopt);
            print_type(*function.return_type);
            close();
        }

        if (function.effects.present) {
            std::string listed;
            for (const Effect effect : function.effects.effects) {
                if (!listed.empty()) {
                    listed += ", ";
                }
                listed += effect_name(effect);
            }
            leaf("uses", function.effects.span, listed.empty() ? "nothing" : listed);
        }

        for (const Contract& contract : function.contracts) {
            open(contract.is_ensures() ? "ensures" : "requires", contract.span);
            print_expr(*contract.condition);
            close();
        }

        print_block(function.body);
        close();
    }

    void print_item(const Item& node) {
        switch (node.kind) {
            case ItemKind::Function:
                print_function(static_cast<const FunctionDecl&>(node));
                return;

            case ItemKind::Struct: {
                const auto& declaration = static_cast<const StructDecl&>(node);
                std::string header = declaration.is_public ? "pub " : "";
                header += declaration.name;
                open("struct", node.span, header);
                for (const FieldDecl& field : declaration.fields) {
                    open("field", field.span,
                         (field.is_public ? "pub " : "") + field.name);
                    print_type(*field.type);
                    close();
                }
                close();
                return;
            }

            case ItemKind::Impl: {
                const auto& block = static_cast<const ImplBlock&>(node);
                open("impl", node.span, block.type_name);
                for (const std::unique_ptr<FunctionDecl>& method : block.methods) {
                    print_function(*method);
                }
                close();
                return;
            }

            case ItemKind::Import: {
                const auto& declaration = static_cast<const ImportDecl&>(node);
                leaf("import", node.span, declaration.module);
                return;
            }

            case ItemKind::Unit: {
                const auto& declaration = static_cast<const UnitDecl&>(node);
                std::string header = declaration.is_public ? "pub " : "";
                header += declaration.name;
                leaf("unit", node.span, header);
                return;
            }

            case ItemKind::Const: {
                const auto& declaration = static_cast<const ConstDecl&>(node);
                std::string header = declaration.is_public ? "pub " : "";
                header += declaration.name;
                open("const", node.span, header);
                print_type(*declaration.type);
                print_expr(*declaration.value);
                close();
                return;
            }
        }
    }
};

}  // namespace

std::string to_sexpr(const Program& program, const SourceFile& source) {
    Printer printer{source};
    printer.program(program);
    return printer.take();
}

std::string to_sexpr(const Expr& expr, const SourceFile& source) {
    Printer printer{source};
    printer.print_expr(expr);
    return printer.take();
}

}  // namespace cinder::ast
