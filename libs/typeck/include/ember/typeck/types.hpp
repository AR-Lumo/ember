// Resolved types.
//
// `ast::TypeRef` is what the programmer wrote; `typeck::Type` is what it
// means. The two are deliberately separate: `Point` in source is a name
// until the checker has seen the struct, and the same written type in two
// places must resolve to one shared object.
//
// Types are interned by a TypeContext, so equality is pointer equality.
// That gives nominal struct typing (§4) for free: two structs with
// identical fields are different types because they are different
// objects.

#ifndef EMBER_TYPECK_TYPES_HPP
#define EMBER_TYPECK_TYPES_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ember::typeck {

enum class TypeKind {
    Int,
    Float,
    Bool,
    String,
    /// A named struct.
    Struct,
    /// `&T`
    Reference,
    /// `[T; N]`
    Array,
    /// The type of a function that returns nothing.
    Void,
    /// Poison. Produced wherever an error was already reported, and
    /// silently compatible with everything so one mistake yields one
    /// diagnostic instead of a cascade.
    Error,
};

struct Type {
    TypeKind kind = TypeKind::Error;
    /// Set for Struct.
    std::string name;
    /// Element type for Reference and Array.
    const Type* element = nullptr;
    /// Element count for Array.
    std::int64_t length = 0;
};

using TypePtr = const Type*;

/// How a type is written in source: `int`, `&Point`, `[float; 8]`.
std::string to_string(TypePtr type);

/// Owns every interned type. Must outlive everything holding a TypePtr.
class TypeContext {
public:
    TypeContext();

    TypePtr int_type() const noexcept { return int_; }
    TypePtr float_type() const noexcept { return float_; }
    TypePtr bool_type() const noexcept { return bool_; }
    TypePtr string_type() const noexcept { return string_; }
    TypePtr void_type() const noexcept { return void_; }
    TypePtr error_type() const noexcept { return error_; }

    TypePtr struct_type(const std::string& name);
    TypePtr reference_to(TypePtr element);
    TypePtr array_of(TypePtr element, std::int64_t length);

private:
    TypePtr intern(Type type);

    std::vector<std::unique_ptr<Type>> owned_;
    std::map<std::string, TypePtr> structs_;
    std::map<TypePtr, TypePtr> references_;
    std::map<std::pair<TypePtr, std::int64_t>, TypePtr> arrays_;

    TypePtr int_ = nullptr;
    TypePtr float_ = nullptr;
    TypePtr bool_ = nullptr;
    TypePtr string_ = nullptr;
    TypePtr void_ = nullptr;
    TypePtr error_ = nullptr;
};

/// True for the poison type, which suppresses follow-on diagnostics.
inline bool is_error(TypePtr type) noexcept {
    return type == nullptr || type->kind == TypeKind::Error;
}

/// `&T` -> `T`, anything else unchanged. References are transparent for
/// field access, indexing and method calls, which is the only way to use
/// a `&T` given that v1 has no dereference operator.
inline TypePtr strip_reference(TypePtr type) noexcept {
    return (type != nullptr && type->kind == TypeKind::Reference) ? type->element : type;
}

/// Numeric types, which are the ones arithmetic accepts.
inline bool is_numeric(TypePtr type) noexcept {
    return type != nullptr && (type->kind == TypeKind::Int || type->kind == TypeKind::Float);
}

}  // namespace ember::typeck

#endif  // EMBER_TYPECK_TYPES_HPP
