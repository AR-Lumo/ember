#include "ember/typeck/typeck.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace ember::typeck {
namespace {

using ast::Diagnostic;
using ast::Span;

/// §5: recognized by the compiler rather than declared in a library.
constexpr std::array<std::string_view, 3> kIntrinsics{"println", "print", "len"};

/// One binding visible in a scope.
struct Binding {
    TypePtr type = nullptr;
    bool is_mutable = false;
    /// Parameters cannot be made mutable in v1, so the "add `mut`"
    /// suggestion would be bad advice for one.
    bool is_parameter = false;
    Span span;
};

/// A stack of lexical scopes. Inner scopes shadow outer ones, as in Rust.
class Scopes {
public:
    void push() { scopes_.emplace_back(); }
    void pop() { scopes_.pop_back(); }

    /// Declare a binding. Returns the previous one if this name is
    /// already bound *in the same scope*, which is a redefinition.
    const Binding* declare(const std::string& name, Binding binding) {
        std::map<std::string, Binding>& scope = scopes_.back();
        const auto found = scope.find(name);
        if (found != scope.end()) {
            return &found->second;
        }
        scope.emplace(name, binding);
        return nullptr;
    }

    const Binding* lookup(const std::string& name) const {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            const auto found = scope->find(name);
            if (found != scope->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    /// Every name currently in scope, for "did you mean" suggestions.
    std::vector<std::string> names() const {
        std::vector<std::string> all;
        for (const std::map<std::string, Binding>& scope : scopes_) {
            for (const auto& [name, binding] : scope) {
                all.push_back(name);
            }
        }
        return all;
    }

private:
    std::vector<std::map<std::string, Binding>> scopes_;
};

/// Levenshtein distance, capped: only used to decide whether a name is
/// close enough to suggest.
std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> previous(b.size() + 1);
    std::vector<std::size_t> current(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        previous[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        current[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
        }
        previous = current;
    }
    return previous[b.size()];
}

/// The closest candidate to `name`, if one is close enough to be worth
/// suggesting. §8 lists suggestions under Phase 5, but the checker is
/// where the candidate lists already exist.
std::optional<std::string> closest_name(std::string_view name,
                                        const std::vector<std::string>& candidates) {
    // Below three characters, every short name is one edit from every
    // other, and "did you mean `x`?" for a typo'd `z` is worse than
    // saying nothing.
    if (name.size() < 3) {
        return std::nullopt;
    }

    std::optional<std::string> best;
    std::size_t best_distance = 0;

    // A third of the length, at least one edit: enough to catch typos
    // without suggesting unrelated names.
    const std::size_t limit = std::max<std::size_t>(1, name.size() / 3);

    for (const std::string& candidate : candidates) {
        const std::size_t distance = edit_distance(name, candidate);
        if (distance <= limit && (!best.has_value() || distance < best_distance)) {
            best = candidate;
            best_distance = distance;
        }
    }
    return best;
}

class Checker {
public:
    explicit Checker(const ast::SourceFile& source) : source_(source) {
        result_.types = std::make_unique<TypeContext>();
    }

    CheckResult run(const ast::Program& program) {
        collect_types(program);
        collect_signatures(program);
        check_constants(program);
        check_bodies(program);
        return std::move(result_);
    }

private:
    const ast::SourceFile& source_;
    CheckResult result_;
    Scopes scopes_;

    /// Signature of the function being checked, for `return` and `self`.
    const FunctionInfo* current_function_ = nullptr;

    TypeContext& types() { return *result_.types; }

    // -----------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------

    Diagnostic& report(std::string message, Span span, std::string label) {
        result_.diagnostics.push_back(
            Diagnostic::error(std::move(message), span, std::move(label)));
        return result_.diagnostics.back();
    }

    /// The §7 worked example is exactly this shape, so every
    /// expected-versus-found error is reported through here.
    void report_mismatch(Span span, TypePtr expected, TypePtr found) {
        if (is_error(expected) || is_error(found)) {
            return;  // already reported upstream
        }
        report("type mismatch", span,
               "expected `" + to_string(expected) + "`, found `" + to_string(found) + "`");
    }

    /// `file.em:3:5`, for notes that point at another location.
    std::string location_of(Span span) const {
        const ast::Position position = source_.position_of(span.start);
        return source_.path() + ":" + std::to_string(position.line) + ":" +
               std::to_string(position.column);
    }

    /// Adds "previously defined at ..." to a duplicate-definition error.
    void note_previous(Diagnostic& diagnostic, Span previous) {
        diagnostic.with_note("previously defined at " + location_of(previous));
    }

    void note_declared_at(Diagnostic& diagnostic, Span declaration) {
        diagnostic.with_note("declared at " + location_of(declaration));
    }

    void suggest(Diagnostic& diagnostic, std::string_view name,
                 const std::vector<std::string>& candidates) {
        if (const std::optional<std::string> best = closest_name(name, candidates)) {
            diagnostic.with_note("did you mean `" + *best + "`?");
        }
    }

    // -----------------------------------------------------------------
    // Pass 1: struct names and fields
    // -----------------------------------------------------------------

    void collect_types(const ast::Program& program) {
        // Names first, so fields can refer to structs declared later.
        for (const ast::ItemPtr& item : program.items) {
            const auto* declaration = ast::node_cast<ast::StructDecl>(item.get());
            if (declaration == nullptr) {
                continue;
            }
            const auto existing = result_.structs.find(declaration->name);
            if (existing != result_.structs.end()) {
                Diagnostic& diagnostic =
                    report("duplicate definition of type `" + declaration->name + "`",
                           declaration->name_span,
                           "`" + declaration->name + "` is already defined");
                note_previous(diagnostic, existing->second.span);
                continue;
            }
            StructInfo info;
            info.name = declaration->name;
            info.span = declaration->name_span;
            result_.structs.emplace(declaration->name, std::move(info));
        }

        for (const ast::ItemPtr& item : program.items) {
            const auto* declaration = ast::node_cast<ast::StructDecl>(item.get());
            if (declaration == nullptr) {
                continue;
            }
            const auto entry = result_.structs.find(declaration->name);
            if (entry == result_.structs.end() || !entry->second.fields.empty()) {
                continue;
            }
            resolve_fields(*declaration, entry->second);
        }

        detect_infinite_types();
    }

    /// Reports structs whose size would be infinite.
    ///
    /// A field only makes a struct bigger when it is stored by value, so
    /// the edges here are "contains by value": a struct field, or an
    /// array of them at any depth. A `&T` field is a pointer of fixed
    /// size and breaks the cycle, which is exactly the fix the
    /// diagnostic suggests.
    void detect_infinite_types() {
        enum class Mark { Unvisited, OnStack, Done };
        std::map<std::string, Mark> marks;
        std::vector<std::string> path;

        for (const auto& [name, info] : result_.structs) {
            marks[name] = Mark::Unvisited;
        }

        // Recursive lambda: `visit` returns true once a cycle has been
        // reported, so each cycle produces one diagnostic rather than
        // one per struct on it.
        const auto visit = [&](auto&& self, const std::string& name) -> bool {
            marks[name] = Mark::OnStack;
            path.push_back(name);

            const auto entry = result_.structs.find(name);
            if (entry != result_.structs.end()) {
                for (const FieldInfo& field : entry->second.fields) {
                    const std::string contained = contained_struct(field.type);
                    if (contained.empty()) {
                        continue;
                    }
                    const auto mark = marks.find(contained);
                    if (mark == marks.end()) {
                        continue;
                    }

                    if (mark->second == Mark::OnStack) {
                        report_infinite_type(entry->second, field, contained, path);
                        marks[name] = Mark::Done;
                        path.pop_back();
                        return true;
                    }
                    if (mark->second == Mark::Unvisited && self(self, contained)) {
                        marks[name] = Mark::Done;
                        path.pop_back();
                        return true;
                    }
                }
            }

            marks[name] = Mark::Done;
            path.pop_back();
            return false;
        };

        for (const auto& [name, info] : result_.structs) {
            if (marks[name] == Mark::Unvisited) {
                visit(visit, name);
            }
        }
    }

    /// The struct a type stores by value, looking through arrays but not
    /// through references. Empty when the type stores no struct.
    static std::string contained_struct(TypePtr type) {
        while (type != nullptr && type->kind == TypeKind::Array) {
            type = type->element;
        }
        if (type != nullptr && type->kind == TypeKind::Struct) {
            return type->name;
        }
        return {};
    }

    void report_infinite_type(const StructInfo& info, const FieldInfo& field,
                              const std::string& contained,
                              const std::vector<std::string>& path) {
        // Trace the loop from where it closes back round to itself, and
        // report it against that struct rather than whichever one the
        // walk happened to reach last.
        std::vector<std::string> steps;
        bool started = false;
        for (const std::string& step : path) {
            started = started || step == contained;
            if (started) {
                steps.push_back(step);
            }
        }
        steps.push_back(contained);

        std::string chain = "`" + steps.front() + "`";
        for (std::size_t i = 1; i < steps.size(); ++i) {
            chain += (i == 1 ? " contains `" : ", which contains `") + steps[i] + "`";
        }

        const auto cycle_start = result_.structs.find(contained);
        const StructInfo& reported = cycle_start != result_.structs.end() ? cycle_start->second
                                                                         : info;

        Diagnostic& diagnostic =
            report("recursive type `" + reported.name + "` has infinite size", reported.span,
                   chain);
        diagnostic.with_note("make one of the fields a reference, such as `&" + contained +
                             "`, to break the cycle");
        (void)field;
    }

    void resolve_fields(const ast::StructDecl& declaration, StructInfo& info) {
        for (const ast::FieldDecl& field : declaration.fields) {
            if (const FieldInfo* existing = info.field(field.name)) {
                Diagnostic& diagnostic =
                    report("duplicate definition of field `" + field.name + "`", field.name_span,
                           "`" + field.name + "` is already a field of `" + info.name + "`");
                note_previous(diagnostic, existing->span);
                continue;
            }
            FieldInfo resolved;
            resolved.name = field.name;
            resolved.span = field.name_span;
            resolved.type = resolve_type(*field.type);
            resolved.index = info.fields.size();
            // Infinite-size types are found afterwards, by walking the
            // whole containment graph: a struct can reach itself through
            // another struct or through an array, not just directly.
            info.fields.push_back(std::move(resolved));
        }
    }

    // -----------------------------------------------------------------
    // Pass 2: function and method signatures
    // -----------------------------------------------------------------

    void collect_signatures(const ast::Program& program) {
        for (const ast::ItemPtr& item : program.items) {
            if (const auto* function = ast::node_cast<ast::FunctionDecl>(item.get())) {
                declare_free_function(*function);
            } else if (const auto* block = ast::node_cast<ast::ImplBlock>(item.get())) {
                declare_impl(*block);
            } else if (const auto* constant = ast::node_cast<ast::ConstDecl>(item.get())) {
                declare_constant(*constant);
            }
        }
    }

    void declare_free_function(const ast::FunctionDecl& function) {
        if (is_intrinsic(function.name)) {
            report("cannot redefine the built-in `" + function.name + "`", function.name_span,
                   "`" + function.name + "` is provided by the compiler (§5)");
            return;
        }
        const auto existing = result_.functions.find(function.name);
        if (existing != result_.functions.end()) {
            Diagnostic& diagnostic =
                report("duplicate definition of function `" + function.name + "`",
                       function.name_span, "`" + function.name + "` is already defined");
            note_previous(diagnostic, existing->second.span);
            return;
        }

        FunctionInfo info = signature_of(function, {});
        if (const ast::Param* self = function.self_param()) {
            report("`self` is only valid inside an `impl` block", self->span,
                   "free functions have no receiver");
        }
        result_.functions.emplace(function.name, std::move(info));
    }

    void declare_impl(const ast::ImplBlock& block) {
        if (result_.structs.find(block.type_name) == result_.structs.end()) {
            Diagnostic& diagnostic = report("cannot find type `" + block.type_name + "`",
                                            block.type_name_span, "not found in this scope");
            suggest(diagnostic, block.type_name, struct_names());
            // Methods are still recorded below so their bodies get
            // checked and callers get "unknown method" rather than a
            // second complaint about the type.
        }

        for (const std::unique_ptr<ast::FunctionDecl>& method : block.methods) {
            const auto key = std::make_pair(block.type_name, method->name);
            const auto existing = result_.methods.find(key);
            if (existing != result_.methods.end()) {
                Diagnostic& diagnostic =
                    report("duplicate definition of method `" + method->name + "`",
                           method->name_span,
                           "`" + method->name + "` is already defined on `" + block.type_name +
                               "`");
                note_previous(diagnostic, existing->second.span);
                continue;
            }
            result_.methods.emplace(key, signature_of(*method, block.type_name));
        }
    }

    /// Builds the signature. The mangled name is where §4's "methods are
    /// sugar over plain functions" becomes concrete.
    FunctionInfo signature_of(const ast::FunctionDecl& function, const std::string& owner) {
        FunctionInfo info;
        info.name = function.name;
        info.owner_type = owner;
        info.mangled_name = owner.empty() ? function.name : owner + "_" + function.name;
        info.span = function.name_span;
        info.decl = &function;
        info.return_type =
            function.return_type ? resolve_type(*function.return_type) : types().void_type();

        for (const ast::Param& param : function.params) {
            if (param.is_self()) {
                info.self_kind = param.self_kind;
                const TypePtr owner_type =
                    owner.empty() ? types().error_type() : types().struct_type(owner);
                info.param_names.emplace_back("self");
                info.param_types.push_back(param.self_kind == ast::SelfKind::Reference
                                               ? types().reference_to(owner_type)
                                               : owner_type);
                continue;
            }
            info.param_names.push_back(param.name);
            info.param_types.push_back(resolve_type(*param.type));
        }
        return info;
    }

    void declare_constant(const ast::ConstDecl& constant) {
        const auto existing = result_.constants.find(constant.name);
        if (existing != result_.constants.end()) {
            report("duplicate definition of constant `" + constant.name + "`",
                   constant.name_span, "`" + constant.name + "` is already defined");
            return;
        }
        result_.constants.emplace(constant.name, resolve_type(*constant.type));
    }

    // -----------------------------------------------------------------
    // Type resolution
    // -----------------------------------------------------------------

    TypePtr resolve_type(const ast::TypeRef& type) {
        switch (type.kind) {
            case ast::TypeKind::Int:
                return types().int_type();
            case ast::TypeKind::Float:
                return types().float_type();
            case ast::TypeKind::Bool:
                return types().bool_type();
            case ast::TypeKind::String:
                return types().string_type();

            case ast::TypeKind::Named: {
                if (result_.structs.find(type.name) == result_.structs.end()) {
                    Diagnostic& diagnostic = report("cannot find type `" + type.name + "`",
                                                    type.span, "not found in this scope");
                    suggest(diagnostic, type.name, struct_names());
                    return types().error_type();
                }
                return types().struct_type(type.name);
            }

            case ast::TypeKind::Reference:
                return types().reference_to(resolve_type(*type.element));

            case ast::TypeKind::Array:
                return types().array_of(resolve_type(*type.element), type.length);
        }
        return types().error_type();
    }

    std::vector<std::string> struct_names() const {
        std::vector<std::string> names;
        for (const auto& [name, info] : result_.structs) {
            names.push_back(name);
        }
        return names;
    }

    std::vector<std::string> function_names() const {
        std::vector<std::string> names;
        for (const auto& [name, info] : result_.functions) {
            names.push_back(name);
        }
        for (const std::string_view intrinsic : kIntrinsics) {
            names.emplace_back(intrinsic);
        }
        return names;
    }

    // -----------------------------------------------------------------
    // Pass 3: constant initializers
    // -----------------------------------------------------------------

    void check_constants(const ast::Program& program) {
        scopes_.push();
        for (const ast::ItemPtr& item : program.items) {
            const auto* constant = ast::node_cast<ast::ConstDecl>(item.get());
            if (constant == nullptr) {
                continue;
            }
            // The annotation was already resolved when the constant was
            // declared; resolving it again would report an unknown type
            // twice.
            const auto declared = result_.constants.find(constant->name);
            const TypePtr expected =
                declared != result_.constants.end() ? declared->second : types().error_type();
            const TypePtr actual = check_expr(*constant->value);
            if (!assignable(expected, actual)) {
                report_mismatch(constant->value->span, expected, actual);
            }
        }
        scopes_.pop();
    }

    // -----------------------------------------------------------------
    // Pass 4: function bodies
    // -----------------------------------------------------------------

    void check_bodies(const ast::Program& program) {
        for (const ast::ItemPtr& item : program.items) {
            if (const auto* function = ast::node_cast<ast::FunctionDecl>(item.get())) {
                const auto entry = result_.functions.find(function->name);
                if (entry != result_.functions.end() && entry->second.decl == function) {
                    check_function(*function, entry->second);
                }
            } else if (const auto* block = ast::node_cast<ast::ImplBlock>(item.get())) {
                for (const std::unique_ptr<ast::FunctionDecl>& method : block->methods) {
                    const auto entry =
                        result_.methods.find(std::make_pair(block->type_name, method->name));
                    if (entry != result_.methods.end() && entry->second.decl == method.get()) {
                        check_function(*method, entry->second);
                    }
                }
            }
        }
    }

    void check_function(const ast::FunctionDecl& function, const FunctionInfo& info) {
        // TODO(v2): `pub` is parsed and carried on every item, but v1 is
        // single-file so there is no boundary to enforce it across (§4).
        // When the module system lands, this is where a reference to a
        // private item from another module becomes an error - the
        // syntax and the AST already carry everything that needs.
        current_function_ = &info;
        scopes_.push();

        for (std::size_t i = 0; i < function.params.size(); ++i) {
            const ast::Param& param = function.params[i];
            const std::string& name = info.param_names[i];
            // Parameters are immutable bindings, as in Rust without
            // `mut`; assigning to one is a diagnostic, not a silent copy.
            const Binding* existing =
                scopes_.declare(name, Binding{info.param_types[i], false, true, param.span});
            if (existing != nullptr) {
                Diagnostic& diagnostic =
                    report("duplicate definition of parameter `" + name + "`", param.name_span,
                           "`" + name + "` is already a parameter of this function");
                note_previous(diagnostic, existing->span);
            }
        }

        check_block(function.body);

        // §8: missing return. Void functions may fall off the end; any
        // other must return on every path.
        if (info.return_type != nullptr && info.return_type->kind != TypeKind::Void &&
            !is_error(info.return_type) && !always_returns(function.body)) {
            report("missing return", function.name_span,
                   "this function must return `" + to_string(info.return_type) +
                       "` on every path");
        }

        scopes_.pop();
        current_function_ = nullptr;
    }

    /// Whether control can leave a block only by returning.
    static bool always_returns(const ast::Block& block) {
        for (const ast::StmtPtr& statement : block.statements) {
            if (always_returns(*statement)) {
                return true;
            }
        }
        return false;
    }

    static bool always_returns(const ast::Stmt& statement) {
        switch (statement.kind) {
            case ast::StmtKind::Return:
                return true;
            case ast::StmtKind::Block:
                return always_returns(static_cast<const ast::BlockStmt&>(statement).block);
            case ast::StmtKind::If: {
                const auto& branch = static_cast<const ast::IfStmt&>(statement);
                // Both arms must return, and there must be an else.
                return branch.else_branch != nullptr && always_returns(branch.then_block) &&
                       always_returns(*branch.else_branch);
            }
            case ast::StmtKind::While:
                // The condition may be false on the first test, so a
                // loop never guarantees a return. `while true` could be
                // special-cased, but v1 keeps the rule simple.
                return false;
            default:
                return false;
        }
    }

    void check_block(const ast::Block& block) {
        scopes_.push();
        for (const ast::StmtPtr& statement : block.statements) {
            check_stmt(*statement);
        }
        scopes_.pop();
    }

    void check_stmt(const ast::Stmt& statement) {
        switch (statement.kind) {
            case ast::StmtKind::Let:
                return check_let(static_cast<const ast::LetStmt&>(statement));
            case ast::StmtKind::Return:
                return check_return(static_cast<const ast::ReturnStmt&>(statement));
            case ast::StmtKind::If:
                return check_if(static_cast<const ast::IfStmt&>(statement));
            case ast::StmtKind::While:
                return check_while(static_cast<const ast::WhileStmt&>(statement));
            case ast::StmtKind::Assign:
                return check_assign(static_cast<const ast::AssignStmt&>(statement));
            case ast::StmtKind::Expr:
                check_expr(*static_cast<const ast::ExprStmt&>(statement).expr);
                return;
            case ast::StmtKind::Block:
                return check_block(static_cast<const ast::BlockStmt&>(statement).block);
        }
    }

    void check_let(const ast::LetStmt& statement) {
        const TypePtr initializer = check_expr(*statement.value);
        TypePtr type = initializer;

        if (statement.declared_type) {
            // Annotated: the annotation wins, and the initializer is
            // checked against it. This is the §7 worked example.
            type = resolve_type(*statement.declared_type);
            if (!assignable(type, initializer)) {
                report_mismatch(statement.value->span, type, initializer);
            }
        } else if (initializer != nullptr && initializer->kind == TypeKind::Void) {
            report("cannot infer a type", statement.value->span,
                   "this expression has no value to bind");
            type = types().error_type();
        }

        const Binding* existing = scopes_.declare(
            statement.name, Binding{type, statement.is_mutable, false, statement.name_span});
        if (existing != nullptr) {
            // Shadowing in an inner scope is fine; redeclaring in the
            // same one is not.
            Diagnostic& diagnostic =
                report("duplicate definition of `" + statement.name + "`", statement.name_span,
                       "`" + statement.name + "` is already defined in this scope");
            note_previous(diagnostic, existing->span);
        }
    }

    void check_return(const ast::ReturnStmt& statement) {
        const TypePtr expected =
            current_function_ != nullptr ? current_function_->return_type : types().void_type();

        if (statement.value == nullptr) {
            if (expected != nullptr && expected->kind != TypeKind::Void && !is_error(expected)) {
                report("missing return value", statement.span,
                       "this function returns `" + to_string(expected) + "`");
            }
            return;
        }

        const TypePtr actual = check_expr(*statement.value);
        if (expected != nullptr && expected->kind == TypeKind::Void) {
            report("returning a value from a function with no return type",
                   statement.value->span, "this function returns nothing");
            return;
        }
        if (!assignable(expected, actual)) {
            report_mismatch(statement.value->span, expected, actual);
        }
    }

    void check_if(const ast::IfStmt& statement) {
        check_condition(*statement.condition);
        check_block(statement.then_block);
        if (statement.else_branch) {
            check_stmt(*statement.else_branch);
        }
    }

    void check_while(const ast::WhileStmt& statement) {
        check_condition(*statement.condition);
        check_block(statement.body);
    }

    void check_condition(const ast::Expr& condition) {
        const TypePtr type = check_expr(condition);
        if (!is_error(type) && type->kind != TypeKind::Bool) {
            report_mismatch(condition.span, types().bool_type(), type);
        }
    }

    void check_assign(const ast::AssignStmt& statement) {
        const TypePtr target = check_expr(*statement.target);
        const TypePtr value = check_expr(*statement.value);

        if (!is_assignable_place(*statement.target)) {
            report("invalid assignment target", statement.target->span,
                   "only variables, fields and array elements can be assigned to");
            return;
        }
        check_mutable(*statement.target);

        if (!assignable(target, value)) {
            report_mismatch(statement.value->span, target, value);
        }
    }

    /// Only places - names, fields, elements - can be assigned to.
    static bool is_assignable_place(const ast::Expr& expr) {
        switch (expr.kind) {
            case ast::ExprKind::Name:
            case ast::ExprKind::FieldAccess:
            case ast::ExprKind::Index:
                return true;
            default:
                return false;
        }
    }

    /// Walks a place back to the variable it is rooted at and checks
    /// that it was declared `mut`, so `p.x = 1` needs `let mut p`.
    ///
    /// The walk stops at a reference. §4 makes `&T` a non-owning pointer
    /// with no borrow checker, so writing through one is allowed: what
    /// `mut` governs is rebinding the pointer, not the pointee.
    void check_mutable(const ast::Expr& expr) {
        const ast::Expr* root = &expr;
        while (true) {
            const ast::Expr* container = nullptr;
            if (const auto* field = ast::node_cast<ast::FieldAccessExpr>(root)) {
                container = field->object.get();
            } else if (const auto* index = ast::node_cast<ast::IndexExpr>(root)) {
                container = index->object.get();
            } else {
                break;
            }

            const TypePtr container_type = result_.type_of(*container);
            if (container_type != nullptr && container_type->kind == TypeKind::Reference) {
                return;
            }
            root = container;
        }

        const auto* name = ast::node_cast<ast::NameExpr>(root);
        if (name == nullptr) {
            return;
        }
        if (result_.constants.count(name->name) != 0 && scopes_.lookup(name->name) == nullptr) {
            report("cannot assign to constant `" + name->name + "`", expr.span,
                   "constants are immutable");
            return;
        }
        const Binding* binding = scopes_.lookup(name->name);
        if (binding != nullptr && !binding->is_mutable) {
            if (binding->is_parameter) {
                Diagnostic& diagnostic =
                    report("cannot assign to parameter `" + name->name + "`", expr.span,
                           "parameters are immutable in v1");
                diagnostic.with_note("copy it into a `let mut` binding, or take a `&" +
                                     to_string(binding->type) + "` to write through");
                return;
            }
            Diagnostic& diagnostic =
                report("cannot assign to immutable binding `" + name->name + "`", expr.span,
                       "`" + name->name + "` is not declared `mut`");
            diagnostic.with_note("declare it as `let mut " + name->name + "` to allow assignment");
        }
    }

    // -----------------------------------------------------------------
    // Expressions
    // -----------------------------------------------------------------

    TypePtr record(const ast::Expr& expr, TypePtr type) {
        result_.expr_types[&expr] = type;
        return type;
    }

    TypePtr check_expr(const ast::Expr& expr) {
        switch (expr.kind) {
            case ast::ExprKind::IntLit:
                return record(expr, types().int_type());
            case ast::ExprKind::FloatLit:
                return record(expr, types().float_type());
            case ast::ExprKind::BoolLit:
                return record(expr, types().bool_type());
            case ast::ExprKind::StringLit:
                return record(expr, types().string_type());

            case ast::ExprKind::Name:
                return check_name(static_cast<const ast::NameExpr&>(expr));
            case ast::ExprKind::Unary:
                return check_unary(static_cast<const ast::UnaryExpr&>(expr));
            case ast::ExprKind::Binary:
                return check_binary(static_cast<const ast::BinaryExpr&>(expr));
            case ast::ExprKind::Call:
                return check_call(static_cast<const ast::CallExpr&>(expr));
            case ast::ExprKind::MethodCall:
                return check_method_call(static_cast<const ast::MethodCallExpr&>(expr));
            case ast::ExprKind::FieldAccess:
                return check_field_access(static_cast<const ast::FieldAccessExpr&>(expr));
            case ast::ExprKind::Index:
                return check_index(static_cast<const ast::IndexExpr&>(expr));
            case ast::ExprKind::Cast:
                return check_cast(static_cast<const ast::CastExpr&>(expr));
            case ast::ExprKind::StructLit:
                return check_struct_literal(static_cast<const ast::StructLitExpr&>(expr));
            case ast::ExprKind::ArrayLit:
                return check_array_literal(static_cast<const ast::ArrayLitExpr&>(expr));
        }
        return record(expr, types().error_type());
    }

    TypePtr check_name(const ast::NameExpr& expr) {
        if (const Binding* binding = scopes_.lookup(expr.name)) {
            return record(expr, binding->type);
        }
        const auto constant = result_.constants.find(expr.name);
        if (constant != result_.constants.end()) {
            return record(expr, constant->second);
        }

        if (expr.name == "self") {
            report("`self` is not available here", expr.span,
                   "only methods declared with a `self` receiver can use it");
            return record(expr, types().error_type());
        }

        Diagnostic& diagnostic = report("cannot find value `" + expr.name + "`", expr.span,
                                        "not found in this scope");
        std::vector<std::string> candidates = scopes_.names();
        for (const auto& [name, type] : result_.constants) {
            candidates.push_back(name);
        }
        suggest(diagnostic, expr.name, candidates);
        return record(expr, types().error_type());
    }

    TypePtr check_unary(const ast::UnaryExpr& expr) {
        const TypePtr operand = check_expr(*expr.operand);
        if (is_error(operand)) {
            return record(expr, types().error_type());
        }

        if (expr.op == ast::UnaryOp::Negate) {
            if (!is_numeric(operand)) {
                report("cannot apply `-` to `" + to_string(operand) + "`", expr.span,
                       "negation needs an `int` or a `float`");
                return record(expr, types().error_type());
            }
            return record(expr, operand);
        }

        if (operand->kind != TypeKind::Bool) {
            report("cannot apply `!` to `" + to_string(operand) + "`", expr.span,
                   "logical negation needs a `bool`");
            return record(expr, types().error_type());
        }
        return record(expr, types().bool_type());
    }

    TypePtr check_binary(const ast::BinaryExpr& expr) {
        const TypePtr left = check_expr(*expr.left);
        const TypePtr right = check_expr(*expr.right);
        if (is_error(left) || is_error(right)) {
            return record(expr, types().error_type());
        }

        const std::string symbol{ast::binary_op_symbol(expr.op)};

        switch (expr.op) {
            case ast::BinaryOp::And:
            case ast::BinaryOp::Or: {
                if (left->kind != TypeKind::Bool || right->kind != TypeKind::Bool) {
                    report("cannot apply `" + symbol + "` to `" + to_string(left) + "` and `" +
                               to_string(right) + "`",
                           expr.span, "`" + symbol + "` needs two `bool` operands");
                    return record(expr, types().error_type());
                }
                return record(expr, types().bool_type());
            }

            case ast::BinaryOp::Equal:
            case ast::BinaryOp::NotEqual: {
                if (left != right) {
                    return record(expr, mismatched_operands(expr, symbol, left, right));
                }
                // Structs and arrays have no element-wise comparison in
                // v1; that lands with derived traits in v2.
                if (left->kind == TypeKind::Struct || left->kind == TypeKind::Array ||
                    left->kind == TypeKind::Reference) {
                    report("cannot compare values of type `" + to_string(left) + "`", expr.span,
                           "`" + symbol + "` works on `int`, `float`, `bool` and `string`");
                    return record(expr, types().error_type());
                }
                return record(expr, types().bool_type());
            }

            case ast::BinaryOp::Less:
            case ast::BinaryOp::Greater:
            case ast::BinaryOp::LessEq:
            case ast::BinaryOp::GreaterEq: {
                if (left != right) {
                    return record(expr, mismatched_operands(expr, symbol, left, right));
                }
                if (!is_numeric(left)) {
                    report("cannot compare values of type `" + to_string(left) + "`", expr.span,
                           "`" + symbol + "` needs an `int` or a `float`");
                    return record(expr, types().error_type());
                }
                return record(expr, types().bool_type());
            }

            case ast::BinaryOp::Remainder: {
                // C's `%` is integer-only, and §9 says follow C on the
                // low-level questions.
                if (left != right || left->kind != TypeKind::Int) {
                    report("cannot apply `%` to `" + to_string(left) + "` and `" +
                               to_string(right) + "`",
                           expr.span, "`%` needs two `int` operands");
                    return record(expr, types().error_type());
                }
                return record(expr, types().int_type());
            }

            default: {
                // + - * /
                if (left != right) {
                    return record(expr, mismatched_operands(expr, symbol, left, right));
                }
                if (!is_numeric(left)) {
                    report("cannot apply `" + symbol + "` to `" + to_string(left) + "` and `" +
                               to_string(right) + "`",
                           expr.span, "`" + symbol + "` needs an `int` or a `float`");
                    return record(expr, types().error_type());
                }
                return record(expr, left);
            }
        }
    }

    /// §4 forbids implicit numeric conversion, so mixing `int` and
    /// `float` gets an explanation rather than a bare mismatch.
    TypePtr mismatched_operands(const ast::BinaryExpr& expr, const std::string& symbol,
                                TypePtr left, TypePtr right) {
        Diagnostic& diagnostic =
            report("cannot apply `" + symbol + "` to `" + to_string(left) + "` and `" +
                       to_string(right) + "`",
                   expr.span, "the operands have different types");
        if (is_numeric(left) && is_numeric(right)) {
            diagnostic.with_note("`int` and `float` never mix implicitly in Ember (§4)");
        }
        return types().error_type();
    }

    TypePtr check_call(const ast::CallExpr& expr) {
        if (is_intrinsic(expr.callee)) {
            return check_intrinsic(expr);
        }

        const auto entry = result_.functions.find(expr.callee);
        if (entry == result_.functions.end()) {
            for (const auto& [key, method] : result_.methods) {
                if (key.second == expr.callee) {
                    Diagnostic& diagnostic =
                        report("cannot find function `" + expr.callee + "`", expr.callee_span,
                               "not found in this scope");
                    diagnostic.with_note("`" + expr.callee + "` is a method on `" + key.first +
                                         "`; call it as `value." + expr.callee + "(...)`");
                    for (const ast::ExprPtr& arg : expr.args) {
                        check_expr(*arg);
                    }
                    return record(expr, types().error_type());
                }
            }
            Diagnostic& diagnostic = report("cannot find function `" + expr.callee + "`",
                                            expr.callee_span, "not found in this scope");
            suggest(diagnostic, expr.callee, function_names());
            for (const ast::ExprPtr& arg : expr.args) {
                check_expr(*arg);
            }
            return record(expr, types().error_type());
        }

        const FunctionInfo& info = entry->second;
        result_.call_targets[&expr] = &info;
        check_arguments(expr.span, expr.args, info, 0);
        return record(expr, info.return_type);
    }

    TypePtr check_method_call(const ast::MethodCallExpr& expr) {
        const TypePtr receiver = check_expr(*expr.receiver);
        for (const ast::ExprPtr& arg : expr.args) {
            check_expr(*arg);
        }

        if (is_error(receiver)) {
            return record(expr, types().error_type());
        }

        // A method call looks through a reference, since `&self` is the
        // usual receiver and v1 has no dereference operator.
        const TypePtr base = strip_reference(receiver);
        if (base->kind != TypeKind::Struct) {
            report("no method `" + expr.method + "` on type `" + to_string(receiver) + "`",
                   expr.method_span, "methods can only be called on struct types");
            return record(expr, types().error_type());
        }

        const auto entry = result_.methods.find(std::make_pair(base->name, expr.method));
        if (entry == result_.methods.end()) {
            Diagnostic& diagnostic =
                report("no method `" + expr.method + "` on type `" + base->name + "`",
                       expr.method_span, "unknown method");
            suggest(diagnostic, expr.method, method_names(base->name));
            if (const auto info = result_.structs.find(base->name);
                info != result_.structs.end() && info->second.field(expr.method) != nullptr) {
                diagnostic.with_note("`" + expr.method + "` is a field, not a method");
            }
            return record(expr, types().error_type());
        }

        const FunctionInfo& info = entry->second;
        result_.call_targets[&expr] = &info;

        if (info.self_kind == ast::SelfKind::None) {
            Diagnostic& diagnostic =
                report("`" + expr.method + "` is an associated function, not a method",
                       expr.method_span, "it has no `self` receiver");
            diagnostic.with_note("associated functions have no call syntax in v1");
            return record(expr, info.return_type);
        }

        // The receiver fills the self parameter, so user arguments line
        // up from index 1.
        check_arguments(expr.span, expr.args, info, 1);
        return record(expr, info.return_type);
    }

    std::vector<std::string> method_names(const std::string& type_name) const {
        std::vector<std::string> names;
        for (const auto& [key, info] : result_.methods) {
            if (key.first == type_name) {
                names.push_back(key.second);
            }
        }
        return names;
    }

    /// Checks argument count and types. `offset` is how many leading
    /// parameters the call site fills implicitly (1 for a receiver).
    void check_arguments(Span call_span, const std::vector<ast::ExprPtr>& args,
                         const FunctionInfo& info, std::size_t offset) {
        const std::size_t expected = info.param_types.size() - offset;

        if (args.size() != expected) {
            const std::string what = info.is_method() ? "method" : "function";
            Diagnostic& diagnostic =
                report("this " + what + " takes " + std::to_string(expected) + " argument" +
                           (expected == 1 ? "" : "s") + " but " + std::to_string(args.size()) +
                           " " + (args.size() == 1 ? "was" : "were") + " supplied",
                       call_span,
                       "expected " + std::to_string(expected) + ", found " +
                           std::to_string(args.size()));
            note_declared_at(diagnostic, info.span);
            // Still check the arguments that do line up.
        }

        const std::size_t count = std::min(args.size(), expected);
        for (std::size_t i = 0; i < count; ++i) {
            const TypePtr parameter = info.param_types[i + offset];
            const TypePtr argument = result_.expr_types.count(args[i].get()) != 0
                                         ? result_.expr_types[args[i].get()]
                                         : check_expr(*args[i]);
            if (!assignable(parameter, argument)) {
                report_mismatch(args[i]->span, parameter, argument);
            }
        }
        for (std::size_t i = count; i < args.size(); ++i) {
            if (result_.expr_types.count(args[i].get()) == 0) {
                check_expr(*args[i]);
            }
        }
    }

    /// §5's intrinsics. They are variadic in neither arity nor type, but
    /// they do accept several unrelated types, which no user-declared
    /// signature can express in v1.
    TypePtr check_intrinsic(const ast::CallExpr& expr) {
        for (const ast::ExprPtr& arg : expr.args) {
            check_expr(*arg);
        }

        if (expr.callee == "len") {
            if (expr.args.size() != 1) {
                report("this function takes 1 argument but " +
                           std::to_string(expr.args.size()) + " " +
                           (expr.args.size() == 1 ? "was" : "were") + " supplied",
                       expr.span, "expected 1, found " + std::to_string(expr.args.size()));
                return record(expr, types().int_type());
            }
            const TypePtr argument = strip_reference(result_.expr_types[expr.args[0].get()]);
            if (!is_error(argument) && argument->kind != TypeKind::Array) {
                report("cannot take the length of `" + to_string(argument) + "`",
                       expr.args[0]->span, "`len` needs an array");
            }
            return record(expr, types().int_type());
        }

        // println / print
        if (expr.args.size() != 1) {
            report("this function takes 1 argument but " + std::to_string(expr.args.size()) +
                       " " + (expr.args.size() == 1 ? "was" : "were") + " supplied",
                   expr.span, "expected 1, found " + std::to_string(expr.args.size()));
            return record(expr, types().void_type());
        }

        const TypePtr argument = result_.expr_types[expr.args[0].get()];
        if (!is_error(argument) && !is_printable(argument)) {
            Diagnostic& diagnostic =
                report("cannot print a value of type `" + to_string(argument) + "`",
                       expr.args[0]->span, "`" + expr.callee + "` accepts `int`, `float`, "
                                           "`bool` and `string`");
            diagnostic.with_note("printing structs and arrays arrives with v2");
        }
        return record(expr, types().void_type());
    }

    static bool is_printable(TypePtr type) {
        switch (strip_reference(type)->kind) {
            case TypeKind::Int:
            case TypeKind::Float:
            case TypeKind::Bool:
            case TypeKind::String:
                return true;
            default:
                return false;
        }
    }

    TypePtr check_field_access(const ast::FieldAccessExpr& expr) {
        const TypePtr object = check_expr(*expr.object);
        if (is_error(object)) {
            return record(expr, types().error_type());
        }

        const TypePtr base = strip_reference(object);
        if (base->kind != TypeKind::Struct) {
            report("no field `" + expr.field + "` on type `" + to_string(object) + "`",
                   expr.field_span, "only struct types have fields");
            return record(expr, types().error_type());
        }

        const auto info = result_.structs.find(base->name);
        if (info == result_.structs.end()) {
            return record(expr, types().error_type());
        }

        const FieldInfo* field = info->second.field(expr.field);
        if (field == nullptr) {
            Diagnostic& diagnostic =
                report("no field `" + expr.field + "` on type `" + base->name + "`",
                       expr.field_span, "unknown field");
            std::vector<std::string> names;
            for (const FieldInfo& candidate : info->second.fields) {
                names.push_back(candidate.name);
            }
            suggest(diagnostic, expr.field, names);
            if (result_.methods.count(std::make_pair(base->name, expr.field)) != 0) {
                diagnostic.with_note("`" + expr.field + "` is a method; call it as `." +
                                     expr.field + "(...)`");
            }
            return record(expr, types().error_type());
        }
        return record(expr, field->type);
    }

    TypePtr check_index(const ast::IndexExpr& expr) {
        const TypePtr object = check_expr(*expr.object);
        const TypePtr index = check_expr(*expr.index);

        if (!is_error(index) && index->kind != TypeKind::Int) {
            report_mismatch(expr.index->span, types().int_type(), index);
        }
        if (is_error(object)) {
            return record(expr, types().error_type());
        }

        const TypePtr base = strip_reference(object);
        if (base->kind != TypeKind::Array) {
            report("cannot index into `" + to_string(object) + "`", expr.span,
                   "only arrays can be indexed");
            return record(expr, types().error_type());
        }
        return record(expr, base->element);
    }

    /// `value as T`. §4 forbids implicit numeric conversion but requires
    /// an explicit one to exist, so this is the only place `int` and
    /// `float` meet.
    ///
    /// The permitted set is deliberately narrow: between the two numeric
    /// types, plus the identity cast. Nothing else has an obvious
    /// meaning, and a cast that quietly reinterprets bytes is exactly
    /// the kind of thing v1 should not offer.
    TypePtr check_cast(const ast::CastExpr& expr) {
        const TypePtr source = check_expr(*expr.operand);
        const TypePtr target = resolve_type(*expr.target);

        if (is_error(source) || is_error(target)) {
            return record(expr, target);
        }
        if (source == target) {
            return record(expr, target);  // identity, a no-op at runtime
        }
        if (is_numeric(source) && is_numeric(target)) {
            return record(expr, target);
        }

        Diagnostic& diagnostic =
            report("cannot cast `" + to_string(source) + "` to `" + to_string(target) + "`",
                   expr.span, "no conversion exists between these types");
        diagnostic.with_note("`as` converts between `int` and `float` only");
        return record(expr, target);
    }

    TypePtr check_struct_literal(const ast::StructLitExpr& expr) {
        const auto info = result_.structs.find(expr.type_name);
        if (info == result_.structs.end()) {
            Diagnostic& diagnostic = report("cannot find type `" + expr.type_name + "`",
                                            expr.type_name_span, "not found in this scope");
            suggest(diagnostic, expr.type_name, struct_names());
            for (const ast::FieldInit& field : expr.fields) {
                check_expr(*field.value);
            }
            return record(expr, types().error_type());
        }

        const StructInfo& declared = info->second;
        std::vector<bool> initialized(declared.fields.size(), false);

        for (const ast::FieldInit& field : expr.fields) {
            const TypePtr value = check_expr(*field.value);
            const FieldInfo* target = declared.field(field.name);

            if (target == nullptr) {
                Diagnostic& diagnostic =
                    report("`" + declared.name + "` has no field `" + field.name + "`",
                           field.name_span, "unknown field");
                std::vector<std::string> names;
                for (const FieldInfo& candidate : declared.fields) {
                    names.push_back(candidate.name);
                }
                suggest(diagnostic, field.name, names);
                continue;
            }

            if (initialized[target->index]) {
                report("field `" + field.name + "` is initialized more than once",
                       field.name_span, "duplicate initializer");
                continue;
            }
            initialized[target->index] = true;

            if (!assignable(target->type, value)) {
                report_mismatch(field.value->span, target->type, value);
            }
        }

        std::vector<std::string> missing;
        for (const FieldInfo& field : declared.fields) {
            if (!initialized[field.index]) {
                missing.push_back(field.name);
            }
        }
        if (!missing.empty()) {
            std::string list;
            for (std::size_t i = 0; i < missing.size(); ++i) {
                if (i > 0) {
                    list += ", ";
                }
                list += "`" + missing[i] + "`";
            }
            report("missing field" + std::string(missing.size() == 1 ? "" : "s") + " " + list +
                       " in initializer of `" + declared.name + "`",
                   expr.span, "every field must be given a value");
        }

        return record(expr, types().struct_type(declared.name));
    }

    TypePtr check_array_literal(const ast::ArrayLitExpr& expr) {
        if (expr.elements.empty()) {
            // Inference here runs bottom-up only, so there is nothing to
            // take an element type from. An annotation does not help
            // either: it is checked against this type rather than
            // pushed into it. Poison rather than `[{error}; 0]`, so the
            // enclosing `let` does not report a second mismatch for the
            // same mistake.
            Diagnostic& diagnostic =
                report("cannot infer the element type of an empty array", expr.span,
                       "an empty array literal has no element type");
            diagnostic.with_note("give the array at least one element");
            return record(expr, types().error_type());
        }

        const TypePtr element = check_expr(*expr.elements.front());
        for (std::size_t i = 1; i < expr.elements.size(); ++i) {
            const TypePtr other = check_expr(*expr.elements[i]);
            if (!assignable(element, other)) {
                report_mismatch(expr.elements[i]->span, element, other);
            }
        }
        return record(expr,
                      types().array_of(element, static_cast<std::int64_t>(expr.elements.size())));
    }

    // -----------------------------------------------------------------
    // Assignability
    // -----------------------------------------------------------------

    /// Whether a `found` value may be used where `expected` is wanted.
    ///
    /// Types are otherwise invariant - §4 rules out implicit numeric
    /// conversion - with one exception: a `T` is accepted for a `&T`.
    /// v1 has no address-of operator, so without that rule a `&T`
    /// parameter could never be given an argument at all.
    bool assignable(TypePtr expected, TypePtr found) const {
        if (is_error(expected) || is_error(found)) {
            return true;
        }
        if (expected == found) {
            return true;
        }
        if (expected->kind == TypeKind::Reference && expected->element == found) {
            return true;
        }
        // A `&T` also satisfies a plain `T` parameter, since references
        // are transparent everywhere else in v1.
        if (found->kind == TypeKind::Reference && found->element == expected) {
            return true;
        }
        return false;
    }
};

}  // namespace

const FieldInfo* StructInfo::field(std::string_view name) const {
    for (const FieldInfo& candidate : fields) {
        if (candidate.name == name) {
            return &candidate;
        }
    }
    return nullptr;
}

TypePtr CheckResult::type_of(const ast::Expr& expr) const {
    const auto found = expr_types.find(&expr);
    return found != expr_types.end() ? found->second : nullptr;
}

std::string_view stage_name() noexcept { return "type checker"; }

bool is_intrinsic(std::string_view name) noexcept {
    return std::find(kIntrinsics.begin(), kIntrinsics.end(), name) != kIntrinsics.end();
}

CheckResult check(const ast::Program& program, const ast::SourceFile& source) {
    return Checker{source}.run(program);
}

}  // namespace ember::typeck
