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
    /// The symbol codegen emits: `distance_sq`, or `Point_distance_sq`.
    std::string mangled_name;
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

    /// The type of every expression the checker visited.
    std::unordered_map<const ast::Expr*, TypePtr> expr_types;
    /// Which function each call resolved to. Null for intrinsics.
    std::unordered_map<const ast::Expr*, const FunctionInfo*> call_targets;

    bool ok() const noexcept { return diagnostics.empty(); }

    /// Type recorded for `expr`, or nullptr if it was never visited.
    TypePtr type_of(const ast::Expr& expr) const;
};

/// Check a parsed program.
CheckResult check(const ast::Program& program, const ast::SourceFile& source);

/// The names §5 reserves for compiler intrinsics.
bool is_intrinsic(std::string_view name) noexcept;

}  // namespace ember::typeck

#endif  // EMBER_TYPECK_TYPECK_HPP
