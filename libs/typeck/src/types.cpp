#include "ember/typeck/types.hpp"

#include <utility>

namespace ember::typeck {

TypeContext::TypeContext() {
    int_ = intern(Type{TypeKind::Int, {}, nullptr, 0, {}, nullptr, false});
    float_ = intern(Type{TypeKind::Float, {}, nullptr, 0, {}, nullptr, false});
    bool_ = intern(Type{TypeKind::Bool, {}, nullptr, 0, {}, nullptr, false});
    string_ = intern(Type{TypeKind::String, {}, nullptr, 0, {}, nullptr, false});
    string_buf_ = intern(Type{TypeKind::StringBuf, {}, nullptr, 0, {}, nullptr, false});
    void_ = intern(Type{TypeKind::Void, {}, nullptr, 0, {}, nullptr, false});
    error_ = intern(Type{TypeKind::Error, {}, nullptr, 0, {}, nullptr, false});
}

TypePtr TypeContext::intern(Type type) {
    owned_.push_back(std::make_unique<Type>(std::move(type)));
    return owned_.back().get();
}

TypePtr TypeContext::struct_type(const std::string& name, const std::vector<TypePtr>& args) {
    const auto key = std::make_pair(name, args);
    const auto found = structs_.find(key);
    if (found != structs_.end()) {
        return found->second;
    }
    const TypePtr type = intern(Type{TypeKind::Struct, name, nullptr, 0, args, nullptr, false});
    structs_.emplace(key, type);
    return type;
}

TypePtr TypeContext::generic_type(const std::string& name) {
    const auto found = generics_.find(name);
    if (found != generics_.end()) {
        return found->second;
    }
    const TypePtr type = intern(Type{TypeKind::Generic, name, nullptr, 0, {}, nullptr, false});
    generics_.emplace(name, type);
    return type;
}

TypePtr TypeContext::reference_to(TypePtr element) {
    const auto found = references_.find(element);
    if (found != references_.end()) {
        return found->second;
    }
    const TypePtr type = intern(Type{TypeKind::Reference, {}, element, 0, {}, nullptr, false});
    references_.emplace(element, type);
    return type;
}

TypePtr TypeContext::array_of(TypePtr element, std::int64_t length) {
    const auto key = std::make_pair(element, length);
    const auto found = arrays_.find(key);
    if (found != arrays_.end()) {
        return found->second;
    }
    const TypePtr type = intern(Type{TypeKind::Array, {}, element, length, {}, nullptr, false});
    arrays_.emplace(key, type);
    return type;
}

TypePtr TypeContext::vec_of(TypePtr element) {
    const auto found = vecs_.find(element);
    if (found != vecs_.end()) {
        return found->second;
    }
    const TypePtr type = intern(Type{TypeKind::Vec, {}, element, 0, {}, nullptr, false});
    vecs_.emplace(element, type);
    return type;
}

void TypeContext::mark_owning(TypePtr type) {
    if (type != nullptr && type->kind == TypeKind::Struct) {
        // The context owns every interned type, so this is writing to
        // its own storage rather than through a caller's pointer.
        const_cast<Type*>(type)->owns_heap = true;
    }
}

TypePtr TypeContext::function_of(const std::vector<TypePtr>& params, TypePtr result) {
    const auto key = std::make_pair(params, result);
    const auto found = functions_.find(key);
    if (found != functions_.end()) {
        return found->second;
    }
    Type type;
    type.kind = TypeKind::Function;
    type.args = params;
    type.result = result;
    const TypePtr interned = intern(std::move(type));
    functions_.emplace(key, interned);
    return interned;
}

bool is_owned(TypePtr type) noexcept {
    if (type == nullptr) {
        return false;
    }
    switch (type->kind) {
        case TypeKind::Vec:
        case TypeKind::StringBuf:
        case TypeKind::Function:
            return true;
        case TypeKind::Array:
            // An array of owned elements owns them all.
            return is_owned(type->element);
        case TypeKind::Reference:
            // A borrow never owns, however owned its pointee.
            return false;
        case TypeKind::Struct:
            return type->owns_heap;
        default:
            return false;
    }
}

bool is_generic(TypePtr type) noexcept {
    if (type == nullptr) {
        return false;
    }
    switch (type->kind) {
        case TypeKind::Generic:
            return true;
        case TypeKind::Reference:
        case TypeKind::Array:
            return is_generic(type->element);
        case TypeKind::Struct:
        case TypeKind::Function:
            for (const Type* arg : type->args) {
                if (is_generic(arg)) {
                    return true;
                }
            }
            return type->kind == TypeKind::Function && is_generic(type->result);
        default:
            return false;
    }
}

std::string to_string(TypePtr type) {
    if (type == nullptr) {
        return "?";
    }
    switch (type->kind) {
        case TypeKind::Int:
            return "int";
        case TypeKind::Float:
            return "float";
        case TypeKind::Bool:
            return "bool";
        case TypeKind::String:
            return "string";
        case TypeKind::Struct: {
            if (type->args.empty()) {
                return type->name;
            }
            std::string out = type->name + "<";
            for (std::size_t i = 0; i < type->args.size(); ++i) {
                out += (i > 0 ? ", " : "") + to_string(type->args[i]);
            }
            return out + ">";
        }
        case TypeKind::Generic:
            return type->name;
        case TypeKind::Reference:
            return "&" + to_string(type->element);
        case TypeKind::Array:
            return "[" + to_string(type->element) + "; " + std::to_string(type->length) + "]";
        case TypeKind::Vec:
            return "Vec<" + to_string(type->element) + ">";
        case TypeKind::StringBuf:
            return "String";
        case TypeKind::Function: {
            std::string out = "fn(";
            for (std::size_t i = 0; i < type->args.size(); ++i) {
                out += (i > 0 ? ", " : "") + to_string(type->args[i]);
            }
            out += ")";
            if (type->result != nullptr && type->result->kind != TypeKind::Void) {
                out += " -> " + to_string(type->result);
            }
            return out;
        }
        case TypeKind::Void:
            return "()";
        case TypeKind::Error:
            return "{error}";
    }
    return "?";
}

}  // namespace ember::typeck
