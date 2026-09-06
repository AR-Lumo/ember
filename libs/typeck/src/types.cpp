#include "ember/typeck/types.hpp"

#include <algorithm>
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

Dimension canonical_dimension(Dimension dimension) {
    std::sort(dimension.begin(), dimension.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Fold repeats together, then drop anything that cancelled out.
    Dimension folded;
    for (const auto& term : dimension) {
        if (!folded.empty() && folded.back().first == term.first) {
            folded.back().second += term.second;
        } else {
            folded.push_back(term);
        }
    }
    folded.erase(std::remove_if(folded.begin(), folded.end(),
                                [](const auto& term) { return term.second == 0; }),
                 folded.end());
    return folded;
}

Dimension combine_dimensions(const Dimension& a, const Dimension& b, int sign) {
    Dimension merged = a;
    for (const auto& term : b) {
        merged.emplace_back(term.first, term.second * sign);
    }
    return canonical_dimension(std::move(merged));
}

std::string dimension_string(const Dimension& dimension) {
    const auto write = [](const std::pair<std::string, int>& term, int power) {
        return power == 1 ? term.first : term.first + "^" + std::to_string(power);
    };

    std::string over;
    std::string under;
    for (const auto& term : dimension) {
        std::string& side = term.second > 0 ? over : under;
        if (!side.empty()) {
            side += "*";
        }
        side += write(term, term.second > 0 ? term.second : -term.second);
    }

    if (under.empty()) {
        return over;
    }
    // `1/seconds` rather than `/seconds`, so the expression reads as
    // arithmetic wherever it is printed.
    return (over.empty() ? std::string{"1"} : over) + "/" + under;
}

TypePtr TypeContext::numeric_type(TypeKind kind, const Dimension& dimension) {
    const Dimension key_dimension = canonical_dimension(dimension);
    if (key_dimension.empty()) {
        return kind == TypeKind::Float ? float_ : int_;
    }
    const auto key = std::make_pair(kind == TypeKind::Float, key_dimension);
    const auto found = numbers_.find(key);
    if (found != numbers_.end()) {
        return found->second;
    }
    Type type{kind, {}, nullptr, 0, {}, nullptr, false};
    type.dimension = key_dimension;
    const TypePtr interned = intern(std::move(type));
    numbers_.emplace(key, interned);
    return interned;
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

TypePtr TypeContext::function_of(const std::vector<TypePtr>& params, TypePtr result,
                                 bool effects_bounded, EffectMask effects) {
    const auto key = std::make_tuple(params, result, effects_bounded, effects);
    const auto found = functions_.find(key);
    if (found != functions_.end()) {
        return found->second;
    }
    Type type;
    type.kind = TypeKind::Function;
    type.args = params;
    type.result = result;
    type.effects_bounded = effects_bounded;
    type.effects = effects;
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

/// `io`, `io, mut`, or `nothing` for the empty bound.
///
/// The bit assignment is fixed in typeck.cpp; this only has to agree
/// with it, which is why both live behind one name each.
std::string effects_string(EffectMask effects) {
    static const char* kNames[] = {"io", "mut"};
    std::string out;
    for (unsigned bit = 0; bit < 2; ++bit) {
        if ((effects & (EffectMask{1} << bit)) != 0) {
            if (!out.empty()) {
                out += ", ";
            }
            out += kNames[bit];
        }
    }
    return out.empty() ? "nothing" : out;
}

std::string to_string(TypePtr type) {
    if (type == nullptr) {
        return "?";
    }
    switch (type->kind) {
        case TypeKind::Int:
            return type->dimension.empty() ? "int"
                                           : "int<" + dimension_string(type->dimension) + ">";
        case TypeKind::Float:
            return type->dimension.empty() ? "float"
                                           : "float<" + dimension_string(type->dimension) + ">";
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
            // The bound is part of the type, so it has to be part of
            // how the type is written - otherwise two different types
            // print identically and a mismatch reads as nonsense.
            if (type->effects_bounded) {
                out += " uses " + effects_string(type->effects);
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
