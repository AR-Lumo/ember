#include "ember/ast/nodes.hpp"

namespace ember::ast {

std::string type_to_string(const TypeRef& type) {
    switch (type.kind) {
        case TypeKind::Int:
            return "int";
        case TypeKind::Float:
            return "float";
        case TypeKind::Bool:
            return "bool";
        case TypeKind::String:
            return "string";
        case TypeKind::Named: {
            if (type.type_args.empty()) {
                return type.name;
            }
            std::string out = type.name + "<";
            for (std::size_t i = 0; i < type.type_args.size(); ++i) {
                out += (i > 0 ? ", " : "") + type_to_string(*type.type_args[i]);
            }
            return out + ">";
        }
        case TypeKind::Reference:
            return "&" + (type.element ? type_to_string(*type.element) : std::string{"?"});
        case TypeKind::Array:
            return "[" + (type.element ? type_to_string(*type.element) : std::string{"?"}) +
                   "; " + std::to_string(type.length) + "]";
    }
    return "?";
}

std::string_view unary_op_symbol(UnaryOp op) noexcept {
    switch (op) {
        case UnaryOp::Negate:
            return "-";
        case UnaryOp::Not:
            return "!";
    }
    return "?";
}

std::string_view unary_op_name(UnaryOp op) noexcept {
    switch (op) {
        case UnaryOp::Negate:
            return "neg";
        case UnaryOp::Not:
            return "not";
    }
    return "?";
}

std::string_view binary_op_symbol(BinaryOp op) noexcept {
    switch (op) {
        case BinaryOp::Or:
            return "||";
        case BinaryOp::And:
            return "&&";
        case BinaryOp::Equal:
            return "==";
        case BinaryOp::NotEqual:
            return "!=";
        case BinaryOp::Less:
            return "<";
        case BinaryOp::Greater:
            return ">";
        case BinaryOp::LessEq:
            return "<=";
        case BinaryOp::GreaterEq:
            return ">=";
        case BinaryOp::Add:
            return "+";
        case BinaryOp::Subtract:
            return "-";
        case BinaryOp::Multiply:
            return "*";
        case BinaryOp::Divide:
            return "/";
        case BinaryOp::Remainder:
            return "%";
    }
    return "?";
}

std::string_view binary_op_name(BinaryOp op) noexcept {
    switch (op) {
        case BinaryOp::Or:
            return "or";
        case BinaryOp::And:
            return "and";
        case BinaryOp::Equal:
            return "eq";
        case BinaryOp::NotEqual:
            return "ne";
        case BinaryOp::Less:
            return "lt";
        case BinaryOp::Greater:
            return "gt";
        case BinaryOp::LessEq:
            return "le";
        case BinaryOp::GreaterEq:
            return "ge";
        case BinaryOp::Add:
            return "add";
        case BinaryOp::Subtract:
            return "sub";
        case BinaryOp::Multiply:
            return "mul";
        case BinaryOp::Divide:
            return "div";
        case BinaryOp::Remainder:
            return "rem";
    }
    return "?";
}

}  // namespace ember::ast
