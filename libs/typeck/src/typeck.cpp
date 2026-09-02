#include "ember/typeck/typeck.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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

    /// Which monomorphized copy is being checked. Everything outside a
    /// generic body is the root instance.
    InstanceId current_instance_ = kRootInstance;
    /// `T` -> `int` for the instantiation being checked, consulted by
    /// resolve_type before it looks for a struct of that name.
    const std::map<std::string, TypePtr>* current_bindings_ = nullptr;
    /// Type parameters that are merely in scope, while a template's own
    /// signature is being resolved. Their names become Generic types.
    std::vector<std::string> generic_scope_;

    /// Each template's signature resolved once, with Generic
    /// placeholders where its type parameters appear. Instantiating
    /// substitutes into this rather than re-walking the AST.
    std::map<std::string, FunctionInfo> template_signatures_;
    /// Instantiations created but not yet checked.
    std::vector<std::size_t> pending_instances_;

    TypeContext& types() { return *result_.types; }

    // -----------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------

    Diagnostic& report(std::string message, Span span, std::string label) {
        result_.diagnostics.push_back(
            Diagnostic::error(std::move(message), span, std::move(label)));
        Diagnostic& diagnostic = result_.diagnostics.back();

        // An error inside a generic body is only an error for the types
        // it was instantiated with, so say which ones and where they
        // came from. This is the whole reason C++ template errors are
        // readable at all when they are readable.
        if (current_instance_ != kRootInstance &&
            current_instance_ - 1 < result_.instantiations.size()) {
            const Instantiation& instance = result_.instantiations[current_instance_ - 1];
            diagnostic.with_note("in `" + instance.info.name + "` instantiated as `" +
                                 instance.info.display_name + "` at " +
                                 location_of(instance.origin));
        }
        return diagnostic;
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
            if (declaration->is_generic()) {
                register_struct_template(*declaration);
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
            if (declaration->is_generic()) {
                continue;  // laid out per instantiation
            }
            const auto entry = result_.structs.find(declaration->name);
            if (entry == result_.structs.end() || !entry->second.fields.empty()) {
                continue;
            }
            resolve_fields(*declaration, entry->second);
        }

        detect_infinite_types();
    }

    void register_struct_template(const ast::StructDecl& declaration) {
        if (result_.struct_templates.count(declaration.name) != 0 ||
            result_.structs.count(declaration.name) != 0) {
            report("duplicate definition of type `" + declaration.name + "`",
                   declaration.name_span, "`" + declaration.name + "` is already defined");
            return;
        }

        StructTemplate tmpl;
        tmpl.name = declaration.name;
        tmpl.span = declaration.name_span;
        tmpl.decl = &declaration;
        for (const ast::GenericParam& parameter : declaration.generic_params) {
            tmpl.generic_params.push_back(parameter.name);
        }
        check_generic_params(declaration.generic_params);
        result_.struct_templates.emplace(declaration.name, std::move(tmpl));
    }

    /// Type parameters share a namespace with types, so a parameter that
    /// shadows a struct or repeats another is worth reporting.
    void check_generic_params(const std::vector<ast::GenericParam>& params) {
        for (std::size_t i = 0; i < params.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (params[i].name == params[j].name) {
                    Diagnostic& diagnostic =
                        report("duplicate type parameter `" + params[i].name + "`",
                               params[i].span, "already declared on this item");
                    note_previous(diagnostic, params[j].span);
                }
            }
            if (result_.structs.count(params[i].name) != 0 ||
                result_.struct_templates.count(params[i].name) != 0) {
                report("type parameter `" + params[i].name + "` shadows a type",
                       params[i].span, "a type with this name is already declared");
            }
        }
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
        if (function.is_generic()) {
            declare_function_template(function, {});
            return;
        }
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

    /// Registers a generic function and resolves its signature once,
    /// with Generic placeholders standing in for its type parameters.
    /// The body is deliberately not checked here - see the note above
    /// substitute().
    void declare_function_template(const ast::FunctionDecl& function,
                                   const std::string& owner) {
        if (is_intrinsic(function.name)) {
            report("cannot redefine the built-in `" + function.name + "`", function.name_span,
                   "`" + function.name + "` is provided by the compiler (§5)");
            return;
        }
        if (result_.function_templates.count(function.name) != 0 ||
            result_.functions.count(function.name) != 0) {
            Diagnostic& diagnostic =
                report("duplicate definition of function `" + function.name + "`",
                       function.name_span, "`" + function.name + "` is already defined");
            const auto existing = result_.function_templates.find(function.name);
            if (existing != result_.function_templates.end()) {
                note_previous(diagnostic, existing->second.span);
            }
            return;
        }

        check_generic_params(function.generic_params);

        FunctionTemplate tmpl;
        tmpl.name = function.name;
        tmpl.owner_type = owner;
        tmpl.span = function.name_span;
        tmpl.decl = &function;
        for (const ast::GenericParam& parameter : function.generic_params) {
            tmpl.generic_params.push_back(parameter.name);
        }

        const std::vector<std::string> saved = generic_scope_;
        generic_scope_ = tmpl.generic_params;
        FunctionInfo signature = signature_of(function, owner);
        generic_scope_ = saved;

        // A parameter that appears nowhere in the parameter list can
        // never be inferred, and there is no turbofish to supply it.
        for (const std::string& parameter : tmpl.generic_params) {
            bool mentioned = false;
            for (const TypePtr type : signature.param_types) {
                mentioned = mentioned || mentions_parameter(type, parameter);
            }
            if (!mentioned) {
                report("type parameter `" + parameter + "` cannot be inferred",
                       function.name_span,
                       "`" + parameter + "` does not appear in any parameter type")
                    .with_note("type arguments are inferred from the call, so every "
                               "parameter must be used by one");
            }
        }

        template_signatures_.emplace(function.name, std::move(signature));
        result_.function_templates.emplace(function.name, std::move(tmpl));
    }

    static bool mentions_parameter(TypePtr type, const std::string& name) {
        if (type == nullptr) {
            return false;
        }
        if (type->kind == TypeKind::Generic) {
            return type->name == name;
        }
        if (type->kind == TypeKind::Reference || type->kind == TypeKind::Array) {
            return mentions_parameter(type->element, name);
        }
        if (type->kind == TypeKind::Struct) {
            for (const TypePtr arg : type->args) {
                if (mentions_parameter(arg, name)) {
                    return true;
                }
            }
        }
        return false;
    }

    void declare_impl(const ast::ImplBlock& block) {
        if (block.is_generic()) {
            report("generic `impl` blocks are not supported yet", block.type_name_span,
                   "`impl<T>` needs generic methods, which are not implemented")
                .with_note("generic free functions do work: `fn first<T>(pair: Pair<T>) -> T`");
            return;
        }
        if (result_.structs.find(block.type_name) == result_.structs.end()) {
            Diagnostic& diagnostic = report("cannot find type `" + block.type_name + "`",
                                            block.type_name_span, "not found in this scope");
            suggest(diagnostic, block.type_name, struct_names());
            // Methods are still recorded below so their bodies get
            // checked and callers get "unknown method" rather than a
            // second complaint about the type.
        }

        for (const std::unique_ptr<ast::FunctionDecl>& method : block.methods) {
            if (method->is_generic()) {
                report("generic methods are not supported yet", method->name_span,
                       "only free functions may have their own type parameters")
                    .with_note("move the type parameters to the `impl` block, or make this a "
                               "free function");
                continue;
            }
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
    // Generics: substitution, inference, instantiation
    //
    // Ember has no traits, so a type parameter carries no guarantees and
    // a generic body cannot be meaningfully checked in the abstract:
    // `a > b` is valid for some `T` and not others. So a template is
    // stored unchecked and each instantiation is checked as if it had
    // been written out by hand - C++'s model rather than Rust's, which
    // is what §6 asks for ("like Rust/C++ templates").
    //
    // The cost is that an uninstantiated generic function is never
    // checked at all, and errors surface at the call site. The note
    // added by report() is what keeps that navigable.
    // -----------------------------------------------------------------

    /// Replace type parameters with their bindings, throughout.
    TypePtr substitute(TypePtr type, const std::map<std::string, TypePtr>& bindings) {
        if (type == nullptr) {
            return type;
        }
        switch (type->kind) {
            case TypeKind::Generic: {
                const auto found = bindings.find(type->name);
                return found != bindings.end() ? found->second : type;
            }
            case TypeKind::Reference:
                return types().reference_to(substitute(type->element, bindings));
            case TypeKind::Array:
                return types().array_of(substitute(type->element, bindings), type->length);
            case TypeKind::Struct: {
                if (type->args.empty()) {
                    return type;
                }
                std::vector<TypePtr> args;
                for (const TypePtr arg : type->args) {
                    args.push_back(substitute(arg, bindings));
                }
                const TypePtr instantiated = types().struct_type(type->name, args);
                ensure_struct_layout(instantiated);
                return instantiated;
            }
            default:
                return type;
        }
    }

    /// Match a template's parameter type against a concrete argument
    /// type, binding type parameters as it goes.
    ///
    /// Deliberately structural and one-directional: it never invents a
    /// binding from nothing, so a parameter that appears only in the
    /// return type stays uninferred and is reported rather than guessed.
    bool unify(TypePtr parameter, TypePtr argument,
               std::map<std::string, TypePtr>& bindings) {
        if (parameter == nullptr || argument == nullptr) {
            return false;
        }
        if (is_error(argument)) {
            return true;  // already reported; do not pile on
        }

        if (parameter->kind == TypeKind::Generic) {
            const auto existing = bindings.find(parameter->name);
            if (existing == bindings.end()) {
                bindings.emplace(parameter->name, argument);
                return true;
            }
            return existing->second == argument;
        }

        if (parameter->kind == TypeKind::Reference) {
            // A `&T` parameter accepts a `T` argument by implicit borrow
            // (§4), so look through the reference on either side.
            return unify(parameter->element, strip_reference(argument), bindings);
        }
        if (parameter->kind == TypeKind::Array) {
            return argument->kind == TypeKind::Array && parameter->length == argument->length &&
                   unify(parameter->element, argument->element, bindings);
        }
        if (parameter->kind == TypeKind::Struct && !parameter->args.empty()) {
            const TypePtr concrete = strip_reference(argument);
            if (concrete->kind != TypeKind::Struct || concrete->name != parameter->name ||
                concrete->args.size() != parameter->args.size()) {
                return false;
            }
            for (std::size_t i = 0; i < parameter->args.size(); ++i) {
                if (!unify(parameter->args[i], concrete->args[i], bindings)) {
                    return false;
                }
            }
            return true;
        }
        return parameter == argument;
    }

    /// `max<int, float>` for diagnostics.
    static std::string display_name_of(const std::string& name,
                                       const std::vector<TypePtr>& args) {
        std::string out = name + "<";
        for (std::size_t i = 0; i < args.size(); ++i) {
            out += (i > 0 ? ", " : "") + to_string(args[i]);
        }
        return out + ">";
    }

    /// A symbol-safe encoding of a type, for mangled names.
    static std::string mangle_type(TypePtr type) {
        if (type == nullptr) {
            return "err";
        }
        switch (type->kind) {
            case TypeKind::Reference:
                return "ref_" + mangle_type(type->element);
            case TypeKind::Array:
                return "arr" + std::to_string(type->length) + "_" + mangle_type(type->element);
            default: {
                // Struct names may already contain `<`, `>` and `,` from
                // an earlier instantiation; keep only what a linker will
                // accept.
                std::string out;
                for (const char c : to_string(type)) {
                    out += (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') ? c : '_';
                }
                return out;
            }
        }
    }

    static std::string mangle_instance(const std::string& base,
                                       const std::vector<TypePtr>& args) {
        std::string out = base;
        for (const TypePtr arg : args) {
            out += "__" + mangle_type(arg);
        }
        return out;
    }

    /// Find or create the instantiation of `tmpl` for `args`.
    ///
    /// The instance is registered before its body is checked, so a
    /// generic function that calls itself with the same arguments finds
    /// the entry already there instead of recursing forever.
    const FunctionInfo* instantiate(const FunctionTemplate& tmpl,
                                    const std::vector<TypePtr>& args, Span origin) {
        const std::string display = display_name_of(tmpl.name, args);

        for (Instantiation& existing : result_.instantiations) {
            if (existing.info.display_name == display) {
                return &existing.info;
            }
        }

        std::map<std::string, TypePtr> bindings;
        for (std::size_t i = 0; i < tmpl.generic_params.size() && i < args.size(); ++i) {
            bindings.emplace(tmpl.generic_params[i], args[i]);
        }

        Instantiation instance;
        instance.id = result_.instantiations.size() + 1;
        instance.bindings = bindings;
        instance.decl = tmpl.decl;
        instance.origin = origin;

        // Build the concrete signature by substituting into the
        // template's, which was resolved once with Generic placeholders.
        FunctionInfo& info = instance.info;
        info.name = tmpl.name;
        info.owner_type = tmpl.owner_type;
        info.display_name = display;
        info.type_args = args;
        info.mangled_name = mangle_instance(
            tmpl.owner_type.empty() ? tmpl.name : tmpl.owner_type + "_" + tmpl.name, args);
        info.span = tmpl.span;
        info.decl = tmpl.decl;

        const FunctionInfo& generic_signature = template_signatures_.at(tmpl.name);
        info.param_names = generic_signature.param_names;
        info.self_kind = generic_signature.self_kind;
        for (const TypePtr parameter : generic_signature.param_types) {
            info.param_types.push_back(substitute(parameter, bindings));
        }
        info.return_type = substitute(generic_signature.return_type, bindings);

        result_.instantiations.push_back(std::move(instance));
        pending_instances_.push_back(result_.instantiations.size() - 1);
        return &result_.instantiations.back().info;
    }

    /// Check every instantiation demanded so far, including any demanded
    /// while checking those.
    void check_pending_instances() {
        while (!pending_instances_.empty()) {
            const std::size_t index = pending_instances_.front();
            pending_instances_.erase(pending_instances_.begin());

            Instantiation& instance = result_.instantiations[index];
            const InstanceId previous_instance = current_instance_;
            const std::map<std::string, TypePtr>* previous_bindings = current_bindings_;

            current_instance_ = instance.id;
            current_bindings_ = &instance.bindings;
            check_function(*instance.decl, instance.info);
            current_instance_ = previous_instance;
            current_bindings_ = previous_bindings;
        }
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

            case ast::TypeKind::Named:
                return resolve_named_type(type);

            case ast::TypeKind::Reference:
                return types().reference_to(resolve_type(*type.element));

            case ast::TypeKind::Array:
                return types().array_of(resolve_type(*type.element), type.length);
        }
        return types().error_type();
    }

    /// A written name in type position: a bound type parameter, an
    /// in-scope parameter of the template being declared, a generic
    /// struct applied to arguments, or a plain struct.
    TypePtr resolve_named_type(const ast::TypeRef& type) {
        // Inside an instantiation, `T` is whatever it was bound to.
        if (current_bindings_ != nullptr) {
            const auto bound = current_bindings_->find(type.name);
            if (bound != current_bindings_->end()) {
                if (!type.type_args.empty()) {
                    report("type parameter `" + type.name + "` cannot take type arguments",
                           type.span, "`" + type.name + "` is not a generic type");
                }
                return bound->second;
            }
        }

        // While resolving a template's own signature, its parameters
        // stand for themselves.
        if (std::find(generic_scope_.begin(), generic_scope_.end(), type.name) !=
            generic_scope_.end()) {
            return types().generic_type(type.name);
        }

        const auto tmpl = result_.struct_templates.find(type.name);
        if (tmpl != result_.struct_templates.end()) {
            return instantiate_struct(tmpl->second, type);
        }

        if (!type.type_args.empty()) {
            report("`" + type.name + "` is not a generic type", type.span,
                   "it takes no type arguments");
            return types().error_type();
        }

        if (result_.structs.find(type.name) == result_.structs.end()) {
            Diagnostic& diagnostic =
                report("cannot find type `" + type.name + "`", type.span,
                       "not found in this scope");
            suggest(diagnostic, type.name, struct_names());
            return types().error_type();
        }
        return types().struct_type(type.name);
    }

    /// `Pair<int>`: resolve the arguments and build the structural
    /// type, laying the fields out once per distinct argument list.
    TypePtr instantiate_struct(const StructTemplate& tmpl, const ast::TypeRef& type) {
        if (type.type_args.size() != tmpl.generic_params.size()) {
            report("`" + tmpl.name + "` takes " +
                       std::to_string(tmpl.generic_params.size()) + " type argument" +
                       (tmpl.generic_params.size() == 1 ? "" : "s") + " but " +
                       std::to_string(type.type_args.size()) + " " +
                       (type.type_args.size() == 1 ? "was" : "were") + " given",
                   type.span, "wrong number of type arguments");
            return types().error_type();
        }

        std::vector<TypePtr> args;
        for (const ast::TypeRefPtr& argument : type.type_args) {
            args.push_back(resolve_type(*argument));
        }

        const TypePtr instantiated = types().struct_type(tmpl.name, args);
        ensure_struct_layout(instantiated);
        return instantiated;
    }

    /// Lay out a generic struct instantiation, once.
    ///
    /// A type still mentioning a parameter is left alone: it only occurs
    /// inside a template's own signature, which is never laid out.
    void ensure_struct_layout(TypePtr type) {
        if (type == nullptr || type->kind != TypeKind::Struct || type->args.empty() ||
            is_generic(type)) {
            return;
        }

        const std::string display = to_string(type);
        if (result_.structs.count(display) != 0) {
            return;
        }

        const auto tmpl = result_.struct_templates.find(type->name);
        if (tmpl == result_.struct_templates.end()) {
            return;
        }

        std::map<std::string, TypePtr> bindings;
        for (std::size_t i = 0;
             i < tmpl->second.generic_params.size() && i < type->args.size(); ++i) {
            bindings.emplace(tmpl->second.generic_params[i], type->args[i]);
        }

        // Registered before the fields are resolved, so a struct that
        // reaches its own instantiation through a reference terminates.
        StructInfo placeholder;
        placeholder.name = display;
        placeholder.span = tmpl->second.span;
        result_.structs.emplace(display, placeholder);

        const std::vector<std::string> saved_scope = generic_scope_;
        const std::map<std::string, TypePtr>* saved_bindings = current_bindings_;
        generic_scope_.clear();
        current_bindings_ = &bindings;

        StructInfo resolved;
        resolved.name = display;
        resolved.span = tmpl->second.span;
        resolve_fields(*tmpl->second.decl, resolved);

        generic_scope_ = saved_scope;
        current_bindings_ = saved_bindings;

        result_.structs[display] = std::move(resolved);
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
        check_concrete_bodies(program);
        // Instantiations demanded while checking those bodies, and any
        // demanded transitively while checking them.
        check_pending_instances();
    }

    void check_concrete_bodies(const ast::Program& program) {
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

        result_.binding_types[{current_instance_, &statement}] = type;

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

            const TypePtr container_type = recorded_type(*container);
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
        result_.expr_types[{current_instance_, &expr}] = type;
        return type;
    }

    TypePtr recorded_type(const ast::Expr& expr) const {
        return result_.type_of(current_instance_, expr);
    }

    bool already_checked(const ast::Expr& expr) const {
        return result_.expr_types.count({current_instance_, &expr}) != 0;
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

        const auto tmpl = result_.function_templates.find(expr.callee);
        if (tmpl != result_.function_templates.end()) {
            return check_generic_call(expr, tmpl->second);
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
        result_.call_targets[{current_instance_, &expr}] = &info;
        check_arguments(expr.span, expr.args, info, 0);
        return record(expr, info.return_type);
    }

    /// A call to a generic function: infer the type arguments from the
    /// arguments actually passed, then check the call against the
    /// instantiated signature like any other.
    TypePtr check_generic_call(const ast::CallExpr& expr, const FunctionTemplate& tmpl) {
        const FunctionInfo& signature = template_signatures_.at(tmpl.name);

        for (const ast::ExprPtr& argument : expr.args) {
            check_expr(*argument);
        }

        if (expr.args.size() != signature.param_types.size()) {
            Diagnostic& diagnostic =
                report("this function takes " + std::to_string(signature.param_types.size()) +
                           " argument" + (signature.param_types.size() == 1 ? "" : "s") +
                           " but " + std::to_string(expr.args.size()) + " " +
                           (expr.args.size() == 1 ? "was" : "were") + " supplied",
                       expr.span,
                       "expected " + std::to_string(signature.param_types.size()) + ", found " +
                           std::to_string(expr.args.size()));
            note_declared_at(diagnostic, tmpl.span);
            return record(expr, types().error_type());
        }

        std::map<std::string, TypePtr> bindings;
        for (std::size_t i = 0; i < expr.args.size(); ++i) {
            const TypePtr argument = recorded_type(*expr.args[i]);
            if (!unify(signature.param_types[i], argument, bindings)) {
                // Either the shapes disagree or one parameter was asked
                // to be two things at once. Both read better as a
                // mismatch against what was already inferred.
                report_mismatch(expr.args[i]->span,
                                substitute(signature.param_types[i], bindings), argument);
                return record(expr, types().error_type());
            }
        }

        std::vector<TypePtr> args;
        for (const std::string& parameter : tmpl.generic_params) {
            const auto bound = bindings.find(parameter);
            if (bound == bindings.end()) {
                report("cannot infer type parameter `" + parameter + "`", expr.span,
                       "nothing in this call determines `" + parameter + "`");
                return record(expr, types().error_type());
            }
            if (is_generic(bound->second)) {
                // Only reachable from inside another template, which is
                // not checked until it is itself instantiated.
                report("cannot infer type parameter `" + parameter + "`", expr.span,
                       "it would depend on an unsubstituted type parameter");
                return record(expr, types().error_type());
            }
            args.push_back(bound->second);
        }

        const FunctionInfo* info = instantiate(tmpl, args, expr.span);
        result_.call_targets[{current_instance_, &expr}] = info;
        check_arguments(expr.span, expr.args, *info, 0);
        return record(expr, info->return_type);
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

        const auto entry = result_.methods.find(std::make_pair(to_string(base), expr.method));
        if (entry == result_.methods.end()) {
            Diagnostic& diagnostic =
                report("no method `" + expr.method + "` on type `" + to_string(base) + "`",
                       expr.method_span, "unknown method");
            suggest(diagnostic, expr.method, method_names(to_string(base)));
            if (const auto info = result_.structs.find(to_string(base));
                info != result_.structs.end() && info->second.field(expr.method) != nullptr) {
                diagnostic.with_note("`" + expr.method + "` is a field, not a method");
            }
            return record(expr, types().error_type());
        }

        const FunctionInfo& info = entry->second;
        result_.call_targets[{current_instance_, &expr}] = &info;

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
            const TypePtr argument =
                already_checked(*args[i]) ? recorded_type(*args[i]) : check_expr(*args[i]);
            if (!assignable(parameter, argument)) {
                report_mismatch(args[i]->span, parameter, argument);
            }
        }
        for (std::size_t i = count; i < args.size(); ++i) {
            if (!already_checked(*args[i])) {
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
            const TypePtr argument = strip_reference(recorded_type(*expr.args[0]));
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

        const TypePtr argument = recorded_type(*expr.args[0]);
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

        const auto info = result_.structs.find(to_string(base));
        if (info == result_.structs.end()) {
            return record(expr, types().error_type());
        }

        const FieldInfo* field = info->second.field(expr.field);
        if (field == nullptr) {
            Diagnostic& diagnostic =
                report("no field `" + expr.field + "` on type `" + to_string(base) + "`",
                       expr.field_span, "unknown field");
            std::vector<std::string> names;
            for (const FieldInfo& candidate : info->second.fields) {
                names.push_back(candidate.name);
            }
            suggest(diagnostic, expr.field, names);
            if (result_.methods.count(std::make_pair(to_string(base), expr.field)) != 0) {
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
        const auto tmpl = result_.struct_templates.find(expr.type_name);
        if (tmpl != result_.struct_templates.end()) {
            return check_generic_struct_literal(expr, tmpl->second);
        }

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

        check_literal_fields(expr, info->second);
        return record(expr, types().struct_type(info->second.name));
    }

    /// Field-by-field checking of a struct literal against a laid-out
    /// struct. Shared by the generic and non-generic paths so both report
    /// missing, unknown and mistyped fields identically.
    void check_literal_fields(const ast::StructLitExpr& expr, const StructInfo& declared) {
        std::vector<bool> initialized(declared.fields.size(), false);

        for (const ast::FieldInit& field : expr.fields) {
            const TypePtr value = already_checked(*field.value) ? recorded_type(*field.value)
                                                                : check_expr(*field.value);
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
    }

    /// `Pair { first: 1, second: 2 }` with no written type arguments:
    /// infer them from the field values.
    ///
    /// A struct literal never spells its arguments out. In type position
    /// `Pair<int>` is unambiguous, but in an expression `Pair<int> { }`
    /// would be indistinguishable from a chain of comparisons, so the
    /// values decide.
    TypePtr check_generic_struct_literal(const ast::StructLitExpr& expr,
                                         const StructTemplate& tmpl) {
        // Resolve the template's field types once, with its parameters
        // standing for themselves, so they can be matched against the
        // values supplied here.
        const std::vector<std::string> saved_scope = generic_scope_;
        const std::map<std::string, TypePtr>* saved_bindings = current_bindings_;
        generic_scope_ = tmpl.generic_params;
        current_bindings_ = nullptr;

        StructInfo pattern;
        pattern.name = tmpl.name;
        pattern.span = tmpl.span;
        const std::size_t before = result_.diagnostics.size();
        resolve_fields(*tmpl.decl, pattern);
        result_.diagnostics.resize(before);  // reported when laid out

        generic_scope_ = saved_scope;
        current_bindings_ = saved_bindings;

        std::map<std::string, TypePtr> bindings;
        for (const ast::FieldInit& field : expr.fields) {
            const TypePtr value = check_expr(*field.value);
            if (const FieldInfo* target = pattern.field(field.name)) {
                unify(target->type, value, bindings);
            }
        }

        std::vector<TypePtr> args;
        for (const std::string& parameter : tmpl.generic_params) {
            const auto bound = bindings.find(parameter);
            if (bound == bindings.end() || is_generic(bound->second)) {
                report("cannot infer type parameter `" + parameter + "` for `" + tmpl.name +
                           "`",
                       expr.span, "nothing in this literal determines `" + parameter + "`")
                    .with_note("annotate the binding, as in `let p: " + tmpl.name +
                               "<int> = ...`");
                return record(expr, types().error_type());
            }
            args.push_back(bound->second);
        }

        const TypePtr instantiated = types().struct_type(tmpl.name, args);
        ensure_struct_layout(instantiated);

        // Now check the literal against the laid-out instantiation, so
        // missing, unknown and mistyped fields are reported once, by the
        // same code that handles a non-generic literal.
        const auto laid_out = result_.structs.find(to_string(instantiated));
        if (laid_out == result_.structs.end()) {
            return record(expr, instantiated);
        }
        check_literal_fields(expr, laid_out->second);
        return record(expr, instantiated);
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

TypePtr CheckResult::type_of(InstanceId instance, const ast::Expr& expr) const {
    const auto found = expr_types.find({instance, &expr});
    return found != expr_types.end() ? found->second : nullptr;
}

const FunctionInfo* CheckResult::target_of(InstanceId instance, const ast::Expr& expr) const {
    const auto found = call_targets.find({instance, &expr});
    return found != call_targets.end() ? found->second : nullptr;
}

TypePtr CheckResult::binding_type(InstanceId instance, const ast::Stmt& statement) const {
    const auto found = binding_types.find({instance, &statement});
    return found != binding_types.end() ? found->second : nullptr;
}

std::string_view stage_name() noexcept { return "type checker"; }

bool is_intrinsic(std::string_view name) noexcept {
    return std::find(kIntrinsics.begin(), kIntrinsics.end(), name) != kIntrinsics.end();
}

CheckResult check(const ast::Program& program, const ast::SourceFile& source) {
    return Checker{source}.run(program);
}

}  // namespace ember::typeck
