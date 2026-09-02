// ember::typeck - scope resolution, type inference and type errors.
//
// The checker walks the AST, resolves every name, gives every expression
// a type, and reports the error classes §8 names for this phase: type
// mismatches, undefined identifiers, wrong argument count and types,
// missing returns, duplicate definitions, and unknown methods.
//
// It also records what it learned. Phase 4 needs to know the type of
// every expression and which function each call resolved to, and doing
// that lookup twice is how the two stages drift apart, so CheckResult
// carries it forward rather than leaving codegen to re-derive it.
//
// Errors do not stop the walk: a failing expression yields the poison
// type, which is compatible with everything, so one mistake produces one
// diagnostic instead of a cascade.

#ifndef EMBER_TYPECK_TYPECK_HPP
#define EMBER_TYPECK_TYPECK_HPP

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/nodes.hpp"
#include "ember/ast/span.hpp"
#include "ember/typeck/types.hpp"

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ember::typeck {

/// Which build phase has implemented this stage so far.
inline constexpr int kImplementedPhase = 3;

/// Name of this pipeline stage, used in driver messages.
std::string_view stage_name() noexcept;

/// One field of a struct, in declaration order.
struct FieldInfo {
    std::string name;
    TypePtr type = nullptr;
    ast::Span span;
    /// Position in the struct, which is also its LLVM struct index.
    std::size_t index = 0;
};

struct StructInfo {
    std::string name;
    ast::Span span;
    std::vector<FieldInfo> fields;

    const FieldInfo* field(std::string_view name) const;
};

/// A free function or a method. Methods are stored with the mangled name
/// codegen will emit, confirming the §4 claim that methods are sugar.
struct FunctionInfo {
    /// As written in source, e.g. `distance_sq`.
    std::string name;
    /// The symbol codegen emits: `distance_sq`, `Point_distance_sq`, or
    /// `max__int` for a monomorphized copy.
    std::string mangled_name;
    /// How it reads in a diagnostic: `max<int>`. Same as `name` for a
    /// function with no type parameters.
    std::string display_name;
    /// The type arguments this copy was instantiated with, in order.
    std::vector<TypePtr> type_args;
    /// Empty for a free function, the struct name for a method.
    std::string owner_type;
    ast::Span span;
    std::vector<std::string> param_names;
    std::vector<TypePtr> param_types;
    TypePtr return_type = nullptr;
    /// How the receiver is taken, for methods.
    ast::SelfKind self_kind = ast::SelfKind::None;
    const ast::FunctionDecl* decl = nullptr;

    bool is_method() const noexcept { return !owner_type.empty(); }
};

/// Identifies which monomorphized copy an expression's type belongs to.
///
/// This is the one thing generics forced on the design. Before them, an
/// expression had exactly one type and a plain `Expr* -> Type` map was
/// enough. A generic body is checked once per instantiation, so the same
/// AST node has an `int` type in `max<int>` and a `float` type in
/// `max<float>`; the instance has to be part of the key.
using InstanceId = std::size_t;

/// Everything outside a generic body lives here.
inline constexpr InstanceId kRootInstance = 0;

/// A generic function as written, before substitution.
struct FunctionTemplate {
    std::string name;
    /// Empty for a free function; the struct name for a method.
    std::string owner_type;
    std::vector<std::string> generic_params;
    ast::Span span;
    const ast::FunctionDecl* decl = nullptr;
};

/// A generic struct as written, before substitution.
struct StructTemplate {
    std::string name;
    std::vector<std::string> generic_params;
    ast::Span span;
    const ast::StructDecl* decl = nullptr;
};

/// One monomorphized copy of a generic function: a concrete signature,
/// the bindings that produced it, and the body to check and emit.
struct Instantiation {
    InstanceId id = kRootInstance;
    FunctionInfo info;
    /// `T` -> `int`, for resolving type parameters inside the body.
    std::map<std::string, TypePtr> bindings;
    const ast::FunctionDecl* decl = nullptr;
    /// Where this instantiation was first demanded, for diagnostics.
    ast::Span origin;
};

struct CheckResult {
    std::vector<ast::Diagnostic> diagnostics;
    /// Owns every type the maps below point into.
    std::unique_ptr<TypeContext> types;

    std::map<std::string, StructInfo> structs;
    /// Free functions, by name.
    std::map<std::string, FunctionInfo> functions;
    /// Methods, keyed by (owning type, method name) - §8 is explicit
    /// that these are scoped to their type rather than global.
    std::map<std::pair<std::string, std::string>, FunctionInfo> methods;
    /// Module-level constants, by name.
    std::map<std::string, TypePtr> constants;

    /// Generic declarations, keyed by name. These are never checked or
    /// emitted directly - only their instantiations are.
    std::map<std::string, FunctionTemplate> function_templates;
    std::map<std::string, StructTemplate> struct_templates;

    /// Every monomorphized function, in the order they were demanded.
    /// A deque rather than a vector because `call_targets` holds
    /// pointers into it and it grows while being walked.
    std::deque<Instantiation> instantiations;

    /// The resolved type of every `let` binding, per instantiation.
    /// Codegen reads this rather than re-resolving the written
    /// annotation, which it could not do inside a generic body where the
    /// annotation may name a type parameter.
    std::map<std::pair<InstanceId, const ast::Stmt*>, TypePtr> binding_types;

    /// The type of every expression, per instantiation it was checked in.
    std::map<std::pair<InstanceId, const ast::Expr*>, TypePtr> expr_types;
    /// Which function each call resolved to. Absent for intrinsics.
    std::map<std::pair<InstanceId, const ast::Expr*>, const FunctionInfo*> call_targets;

    bool ok() const noexcept { return diagnostics.empty(); }

    /// Type recorded for `expr` in `instance`, or nullptr if it was
    /// never visited there.
    TypePtr type_of(InstanceId instance, const ast::Expr& expr) const;

    /// The function a call resolved to, or nullptr for an intrinsic.
    const FunctionInfo* target_of(InstanceId instance, const ast::Expr& expr) const;

    /// The type given to a `let` binding in `instance`.
    TypePtr binding_type(InstanceId instance, const ast::Stmt& statement) const;
};

/// Check a parsed program.
CheckResult check(const ast::Program& program, const ast::SourceFile& source);

/// The names §5 reserves for compiler intrinsics.
bool is_intrinsic(std::string_view name) noexcept;

}  // namespace ember::typeck

#endif  // EMBER_TYPECK_TYPECK_HPP
