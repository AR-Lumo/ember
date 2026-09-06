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

#ifndef SOLITON_TYPECK_TYPES_HPP
#define SOLITON_TYPECK_TYPES_HPP

#include <cstdint>
#include <map>
#include <tuple>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace soliton::typeck {

/// The effects a function type permits, as a bitmask (§10.3).
///
/// A bitmask rather than `std::vector<ast::Effect>` so that this header
/// does not have to know about the AST. The bits are assigned in
/// typeck.cpp, which is the only place that translates between the two.
using EffectMask = std::uint32_t;

/// A unit and the power it appears to, as a canonical list.
///
/// `meters/seconds` is `{{"meters", 1}, {"seconds", -1}}` and
/// `meters*meters` is `{{"meters", 2}}`. Canonical means sorted by name
/// with no zero exponents, which is what makes two dimensions that mean
/// the same thing compare equal however they were written: `m/s` and
/// `m*s^-1` and `(m*m)/(m*s)` are one dimension, not three.
///
/// Empty is unitless - an ordinary `int` or `float`, exactly as before.
using Dimension = std::vector<std::pair<std::string, int>>;

/// Sorted by name, with zero exponents dropped.
Dimension canonical_dimension(Dimension dimension);

/// `a` multiplied by `b` raised to `sign` (+1 to multiply, -1 to
/// divide). This is the whole of unit algebra: `*` adds exponents and
/// `/` subtracts them, and a term that cancels to zero disappears - so
/// `meters/meters` is not a unit called that, it is unitless.
Dimension combine_dimensions(const Dimension& a, const Dimension& b, int sign);

/// `meters/seconds`, as it would be written.
std::string dimension_string(const Dimension& dimension);

enum class TypeKind {
    Int,
    Float,
    Bool,
    String,
    /// A named struct. `name` is the base name and `args` holds the
    /// type arguments, so `Pair<T>` keeps its `T` reachable: matching
    /// and substitution have to see inside it, which a flattened
    /// `"Pair<T>"` string would hide.
    Struct,
    /// An unsubstituted type parameter, the `T` of `fn max<T>`. Only
    /// ever appears in a template's signature; every type reachable
    /// from a checked instantiation is concrete.
    Generic,
    /// `&T`
    Reference,
    /// `[T; N]`
    Array,
    /// `Vec<T>`: a growable array. Owns a heap buffer, so it moves on
    /// assignment and is dropped when its owner goes out of scope.
    Vec,
    /// `fn(A, B) -> R`: a callable value.
    ///
    /// Owned, because a closure that captured anything holds a heap
    /// environment it must free. One that captured nothing has a null
    /// environment and drops for free, but shares the type so that
    /// `fn(int) -> int` means one thing wherever it appears.
    Function,
    /// `String`: a growable, owned string buffer. Distinct from
    /// `string`, which is a borrowed fixed-length view - the same split
    /// Rust makes between `String` and `&str`.
    StringBuf,
    /// The type of a function that returns nothing.
    Void,
    /// Poison. Produced wherever an error was already reported, and
    /// silently compatible with everything so one mistake yields one
    /// diagnostic instead of a cascade.
    Error,
};

struct Type {
    TypeKind kind = TypeKind::Error;
    /// Set for Struct and Generic.
    std::string name;
    /// Element type for Reference and Array.
    const Type* element = nullptr;
    /// Element count for Array.
    std::int64_t length = 0;
    /// Type arguments for a generic struct; for a Function, its
    /// parameter types.
    std::vector<const Type*> args;
    /// Return type, for a Function.
    const Type* result = nullptr;
    /// Set for a Struct whose fields transitively own heap memory, so
    /// that the struct moves and drops like the values inside it.
    /// Filled in when the struct is laid out, since a Type alone does
    /// not know its own fields.
    bool owns_heap = false;
    /// For Function: whether a `uses` clause was written on the type,
    /// and what it permits (§10.3).
    ///
    /// Unbounded - no clause - permits everything, which is what keeps
    /// every `fn(int) -> int` written before effects existed meaning
    /// what it did. Part of the interning key, so a bounded function
    /// type is a different type from an unbounded one.
    bool effects_bounded = false;
    EffectMask effects = 0;
    /// For Int and Float: the unit, canonical. Empty for an ordinary
    /// number.
    ///
    /// Part of the interning key, so `float<meters>` and `float` are
    /// different types and the existing pointer equality does the whole
    /// job of keeping them apart. It has no runtime meaning at all: a
    /// `float<meters>` is a `double` in the generated IR, and codegen
    /// never reads this field.
    Dimension dimension;
};

using TypePtr = const Type*;

/// An effect bound as it is written: `io`, `io, mut`, or `nothing`.
std::string effects_string(EffectMask effects);

/// How a type is written in source: `int`, `&Point`, `[float; 8]`.
std::string to_string(TypePtr type);

/// Owns every interned type. Must outlive everything holding a TypePtr.
class TypeContext {
public:
    TypeContext();

    TypePtr int_type() const noexcept { return int_; }
    TypePtr float_type() const noexcept { return float_; }

    /// `int` or `float` carrying a unit. An empty dimension gives back
    /// the plain singleton, so nothing that does not use units ever
    /// sees a new type.
    TypePtr numeric_type(TypeKind kind, const Dimension& dimension);
    TypePtr bool_type() const noexcept { return bool_; }
    TypePtr string_type() const noexcept { return string_; }
    TypePtr void_type() const noexcept { return void_; }
    TypePtr error_type() const noexcept { return error_; }

    /// `Point`, or `Pair<int>` when `args` is given.
    TypePtr struct_type(const std::string& name, const std::vector<TypePtr>& args = {});
    /// The placeholder for a type parameter named `name`.
    TypePtr generic_type(const std::string& name);
    TypePtr reference_to(TypePtr element);
    TypePtr array_of(TypePtr element, std::int64_t length);
    TypePtr vec_of(TypePtr element);
    /// `fn(A, B) -> R`, optionally carrying an effect bound.
    TypePtr function_of(const std::vector<TypePtr>& params, TypePtr result,
                        bool effects_bounded = false, EffectMask effects = 0);
    /// Record that a struct type owns heap memory through its fields.
    void mark_owning(TypePtr type);
    TypePtr string_buf_type() const noexcept { return string_buf_; }

private:
    TypePtr intern(Type type);

    std::vector<std::unique_ptr<Type>> owned_;
    std::map<std::pair<std::string, std::vector<TypePtr>>, TypePtr> structs_;
    std::map<std::string, TypePtr> generics_;
    std::map<TypePtr, TypePtr> references_;
    std::map<TypePtr, TypePtr> vecs_;
    std::map<std::tuple<std::vector<TypePtr>, TypePtr, bool, EffectMask>, TypePtr> functions_;
    std::map<std::pair<TypePtr, std::int64_t>, TypePtr> arrays_;
    std::map<std::pair<bool, Dimension>, TypePtr> numbers_;

    TypePtr int_ = nullptr;
    TypePtr float_ = nullptr;
    TypePtr bool_ = nullptr;
    TypePtr string_ = nullptr;
    TypePtr string_buf_ = nullptr;
    TypePtr void_ = nullptr;
    TypePtr error_ = nullptr;
};

/// The unit on a number, or empty for anything else. `is_numeric` is
/// declared further down, so this asks about the kind directly.
inline const Dimension& dimension_of(TypePtr type) noexcept {
    static const Dimension none;
    if (type == nullptr || (type->kind != TypeKind::Int && type->kind != TypeKind::Float)) {
        return none;
    }
    return type->dimension;
}

/// Does a function type of bound `permitted` accept one that performs
/// `performed`?
///
/// An unbounded type accepts anything, which is what an absent clause
/// has meant since §10.3: no clause is no claim.
inline bool effects_within(bool bounded, EffectMask permitted, EffectMask performed) noexcept {
    return !bounded || (performed & ~permitted) == 0;
}

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

/// True when `type` still mentions an unsubstituted type parameter, and
/// so cannot be laid out or lowered.
bool is_generic(TypePtr type) noexcept;

/// True when a value of this type owns heap memory.
///
/// This is the whole of the ownership model in one predicate. An owned
/// value moves rather than copies, cannot be used after it moves, and is
/// dropped when its owner goes out of scope. Everything else - the
/// primitives, `&T`, and aggregates built only from those - is freely
/// copyable and needs no cleanup, exactly as before.
///
/// A `&T` is never owned however owned its pointee: a reference is a
/// borrow, which is why it can be passed without moving anything.
bool is_owned(TypePtr type) noexcept;

/// Numeric types, which are the ones arithmetic accepts.
/// True for either kind of text: the borrowed `string` view and the
/// owned `String` buffer. They hold the same bytes and differ only in
/// who owns them, so anything that only *reads* text takes both.
inline bool is_text(TypePtr type) noexcept {
    return type != nullptr &&
           (type->kind == TypeKind::String || type->kind == TypeKind::StringBuf);
}

inline bool is_numeric(TypePtr type) noexcept {
    return type != nullptr && (type->kind == TypeKind::Int || type->kind == TypeKind::Float);
}

}  // namespace soliton::typeck

#endif  // SOLITON_TYPECK_TYPES_HPP
