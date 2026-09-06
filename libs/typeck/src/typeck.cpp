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
constexpr std::array<std::string_view, 13> kIntrinsics{
    "println", "print",  "len",      "new_vec", "push",     "pop",     "new_string",
    "push_str", "slice", "find",     "contains", "capacity", "reserve"};

/// One binding visible in a scope.
struct Binding {
    TypePtr type = nullptr;
    bool is_mutable = false;
    /// Parameters cannot be made mutable in v1, so the "add `mut`"
    /// suggestion would be bad advice for one.
    bool is_parameter = false;
    Span span;

    /// Set once the value has been moved out, so a later use can be
    /// reported. Only ever true for an owned type.
    bool moved = false;
    /// Where it was moved, for the "moved here" note.
    Span moved_at;
};

/// A stack of lexical scopes. Inner scopes shadow outer ones, as in Rust.
class Scopes {
public:
    void push() { scopes_.emplace_back(); }
    std::size_t size() const noexcept { return scopes_.size(); }
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

    /// Which bindings have been moved out of, and where.
    ///
    /// Taken before a branch and put back after one that cannot fall
    /// through, because a value given up on a path that returns was not
    /// given up on the path that carries on.
    using MoveState = std::vector<std::map<std::string, std::pair<bool, Span>>>;

    MoveState moves() const {
        MoveState state;
        state.reserve(scopes_.size());
        for (const std::map<std::string, Binding>& scope : scopes_) {
            std::map<std::string, std::pair<bool, Span>> level;
            for (const auto& [name, binding] : scope) {
                level.emplace(name, std::pair{binding.moved, binding.moved_at});
            }
            state.push_back(std::move(level));
        }
        return state;
    }

    void restore_moves(const MoveState& state) {
        for (std::size_t i = 0; i < scopes_.size() && i < state.size(); ++i) {
            for (auto& [name, binding] : scopes_[i]) {
                const auto found = state[i].find(name);
                if (found != state[i].end()) {
                    binding.moved = found->second.first;
                    binding.moved_at = found->second.second;
                }
            }
        }
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

    /// The scope index a name was found at, or nullopt. A closure uses
    /// this to decide whether a name belongs to it or to the function
    /// around it.
    std::optional<std::size_t> depth_of(const std::string& name) const {
        for (std::size_t i = scopes_.size(); i > 0; --i) {
            if (scopes_[i - 1].count(name) != 0) {
                return i - 1;
            }
        }
        return std::nullopt;
    }

    Binding* lookup_mutable(const std::string& name) {
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
    explicit Checker(const ast::SourceMap& sources) : sources_(sources) {
        result_.types = std::make_unique<TypeContext>();
    }

    /// Every module is walked at each stage before the next stage
    /// begins, so a name declared in one module is visible to another
    /// regardless of load order - which is what makes an `import` cycle
    /// resolve instead of depending on who came first.
    CheckResult run(const std::vector<ModuleInput>& modules) {
        for (const ModuleInput& module : modules) {
            imports_.emplace(module.name, module.imports);
            declared_modules_.push_back(module.name);
        }

        // Units before types: a struct field or a signature may be
        // written `float<meters>`, so the unit has to be known before
        // anything that could mention one is resolved.
        for (const ModuleInput& module : modules) {
            current_module_ = module.name;
            collect_units(*module.program);
        }
        for (const ModuleInput& module : modules) {
            current_module_ = module.name;
            collect_types(*module.program);
        }
        for (const ModuleInput& module : modules) {
            current_module_ = module.name;
            collect_signatures(*module.program);
        }
        for (const ModuleInput& module : modules) {
            current_module_ = module.name;
            check_constants(*module.program);
        }
        for (const ModuleInput& module : modules) {
            current_module_ = module.name;
            check_concrete_bodies(*module.program);
        }

        // Instantiations are checked last, each restoring the module it
        // was declared in so its body resolves names as that module.
        check_pending_instances();
        resolve_instance_demand();

        // Effects after all of that: the fixpoint needs every body
        // walked, instantiations included, before it can carry a
        // callee's effects up to its callers (§10.3).
        check_effects();

        current_module_.clear();
        return std::move(result_);
    }

private:
    const ast::SourceMap& sources_;

    /// The module whose items are being declared or whose body is being
    /// checked. Empty means the entry module.
    std::string current_module_;
    /// What each module imported, keyed by module name.
    std::map<std::string, std::vector<std::string>> imports_;
    std::vector<std::string> declared_modules_;
    CheckResult result_;
    Scopes scopes_;

    /// Signature of the function being checked, for `return` and `self`.
    const FunctionInfo* current_function_ = nullptr;
    /// The contract being checked, if any. Only used to explain a bare
    /// `result` that did not resolve - which is the whole reason
    /// `result` is not a keyword.
    const ast::Contract* current_contract_ = nullptr;
    /// The bit an effect occupies in a `typeck::EffectMask`.
    ///
    /// types.hpp deliberately does not know about `ast::Effect`, so the
    /// translation lives here and nowhere else.
    static EffectMask bit_of(ast::Effect effect) noexcept {
        switch (effect) {
            case ast::Effect::Io:
                return EffectMask{1} << 0;
            case ast::Effect::Mut:
                return EffectMask{1} << 1;
        }
        return 0;
    }

    static EffectMask mask_of(const ast::EffectClause& clause) noexcept {
        EffectMask mask = 0;
        for (const ast::Effect effect : clause.effects) {
            mask |= bit_of(effect);
        }
        return mask;
    }

    /// Every effect anything can currently perform.
    static EffectMask every_effect() noexcept { return bit_of(ast::Effect::Io); }

    /// What one function does, for the effect pass (§10.3).
    struct EffectFacts {
        /// Effects known to be performed. Grows during the fixpoint as
        /// callees' effects arrive.
        std::set<ast::Effect> performed;
        /// Where each effect came from, for the diagnostic. For one
        /// performed directly this is the `println`; for one that
        /// arrived through a call it is the call.
        std::map<ast::Effect, Span> blame;
        /// An extra line explaining a blame that is not self-evident -
        /// a call through a function value looks innocent, so it says
        /// why it counts.
        std::map<ast::Effect, std::string> because;
        /// Functions called from here, by mangled name.
        std::vector<std::pair<std::string, Span>> calls;
        /// The declaration, for its `uses` clause and its name.
        const ast::FunctionDecl* decl = nullptr;
    };
    /// Every function that has a body, by mangled name. Closures are
    /// in here too, under a synthetic name.
    std::map<std::string, EffectFacts> effects_;

    /// A closure that has to fit an effect bound (§10.3).
    ///
    /// Checked after the fixpoint rather than at the closure, because
    /// what a closure performs includes what the functions it calls
    /// perform, and that is not known until every body has been walked.
    struct ClosureBound {
        std::string key;
        EffectMask permitted = 0;
        Span span;
    };
    std::vector<ClosureBound> closure_bounds_;
    int next_closure_effects_ = 0;
    /// The one being checked, so a call deep in an expression can find
    /// it without threading a parameter through everything.
    EffectFacts* current_effects_ = nullptr;

    /// Declared units, by name, with where each was declared (§10.2).
    ///
    /// Not module-qualified: a unit is a bare tag with no members, and
    /// metres declared in two modules are the same metres. Making them
    /// distinct would mean a conversion that has nothing to convert.
    std::map<std::string, Span> units_;

    /// Which monomorphized copy is being checked. Everything outside a
    /// generic body is the root instance.
    InstanceId current_instance_ = kRootInstance;
    /// `T` -> `int` for the instantiation being checked, consulted by
    /// resolve_type before it looks for a struct of that name.
    const std::map<std::string, TypePtr>* current_bindings_ = nullptr;
    /// Type parameters that are merely in scope, while a template's own
    /// signature is being resolved. Their names become Generic types.
    std::vector<std::string> generic_scope_;

    /// One closure being checked: where its own scopes start, and what
    /// it has captured so far.
    struct ClosureContext {
        std::size_t scope_base = 0;
        ast::ClosureExpr* closure = nullptr;
        /// Names already captured, so each is recorded once.
        std::vector<std::string> captured;
    };
    /// Nested, so a closure inside a closure captures through both.
    std::vector<ClosureContext> closures_;
    /// Numbers the closures in this program, for codegen's lifted names.
    std::size_t next_closure_id_ = 1;

    /// The bare name currently on the left of an `=`. Writing to a
    /// variable is not a use of what it held, so assigning into a moved
    /// binding gives it a value again rather than being an error.
    const ast::Expr* assignment_target_ = nullptr;

    /// Each template's signature resolved once, with Generic
    /// placeholders where its type parameters appear. Instantiating
    /// substitutes into this rather than re-walking the AST.
    std::map<std::string, FunctionInfo> template_signatures_;
    /// Generic methods, by (owning type, method name). Kept apart from
    /// the free-function templates because `Pair::first` and a function
    /// `first` in a module called `Pair` would otherwise be the same
    /// key.
    std::map<std::pair<std::string, std::string>, FunctionTemplate> method_templates_;
    std::map<std::pair<std::string, std::string>, FunctionInfo> method_template_signatures_;

    /// The resolved signature a template was declared with, whichever
    /// kind of template it is.
    const FunctionInfo& signature_for(const FunctionTemplate& tmpl) const {
        if (tmpl.owner_type.empty()) {
            return template_signatures_.at(tmpl.name);
        }
        return method_template_signatures_.at({tmpl.owner_type, tmpl.simple_name});
    }
    /// Instantiations created but not yet checked.
    std::vector<std::size_t> pending_instances_;
    /// Which instantiations each instantiation's body demands. Kept
    /// separate from `demanded_by` because it is only resolvable once
    /// every body has been checked: an instance demanded from inside
    /// another one inherits whatever modules needed the outer copy.
    std::map<InstanceId, std::set<InstanceId>> instance_demands_;

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

    /// `file.em:3:5`, for notes that point at another location. With
    /// modules the location may be in a different file from the error
    /// itself, so it is resolved through the span's own file id.
    std::string location_of(Span span) const {
        const ast::SourceFile& file = sources_.file(span.file);
        const ast::Position position = file.position_of(span.start);
        return file.path() + ":" + std::to_string(position.line) + ":" +
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

    /// Record that the function being checked performs `effect`.
    void note_effect(ast::Effect effect, Span span, const std::string& because = {}) {
        if (current_effects_ == nullptr) {
            return;
        }
        if (current_effects_->performed.insert(effect).second) {
            current_effects_->blame.emplace(effect, span);
            if (!because.empty()) {
                current_effects_->because.emplace(effect, because);
            }
        }
    }

    /// Record a call, so the fixpoint can carry the callee's effects up.
    void note_call(const FunctionInfo* callee, Span span) {
        if (current_effects_ == nullptr || callee == nullptr) {
            return;
        }
        current_effects_->calls.emplace_back(callee->mangled_name, span);
    }

    /// Effects flow from callee to caller until nothing changes, then
    /// every declared bound is checked (§10.3).
    ///
    /// A least fixed point rather than a walk: two functions may call
    /// each other, and a walk would either recurse forever or have to
    /// pick an order that does not exist.
    void check_effects() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& [name, facts] : effects_) {
                for (const auto& [callee, span] : facts.calls) {
                    const auto target = effects_.find(callee);
                    if (target == effects_.end()) {
                        // A function with no body - an interface file's
                        // signature. It claims nothing, so nothing
                        // arrives; see the limitation in the README.
                        continue;
                    }
                    for (const ast::Effect effect : target->second.performed) {
                        if (facts.performed.insert(effect).second) {
                            // Blamed on the call, not on whatever the
                            // callee did: the call is the line the
                            // reader has to change.
                            facts.blame.emplace(effect, span);
                            changed = true;
                        }
                    }
                }
            }
        }

        for (const ClosureBound& bound : closure_bounds_) {
            const auto found = effects_.find(bound.key);
            if (found == effects_.end()) {
                continue;
            }
            for (const ast::Effect effect : found->second.performed) {
                if ((bound.permitted & bit_of(effect)) != 0) {
                    continue;
                }
                const std::string named{ast::effect_name(effect)};
                const auto where = found->second.blame.find(effect);
                Diagnostic& diagnostic =
                    report("this closure performs `" + named + "`",
                           where != found->second.blame.end() ? where->second : bound.span,
                           "`" + named + "` happens here");
                diagnostic.with_note("it is being used where `uses " +
                                     effects_string(bound.permitted) + "` is required");
            }
        }

        for (const auto& [name, facts] : effects_) {
            if (facts.decl == nullptr || !facts.decl->effects.present) {
                continue;  // no clause is no bound
            }
            for (const ast::Effect effect : facts.performed) {
                if (facts.decl->effects.permits(effect)) {
                    continue;
                }
                const std::string named{ast::effect_name(effect)};
                const auto where = facts.blame.find(effect);
                Diagnostic& diagnostic =
                    report("`" + named + "` is not permitted here",
                           where != facts.blame.end() ? where->second : facts.decl->name_span,
                           "this performs `" + named + "`");
                const auto why = facts.because.find(effect);
                if (why != facts.because.end()) {
                    diagnostic.with_note(why->second);
                }
                diagnostic.with_note(
                    "`" + facts.decl->name + "` is declared `uses " +
                    (facts.decl->effects.effects.empty() ? std::string{"nothing"}
                                                         : declared_effects(*facts.decl)) +
                    "`");
            }
        }
    }

    /// The effects a function declares, as written.
    static std::string declared_effects(const ast::FunctionDecl& function) {
        std::string listed;
        for (const ast::Effect effect : function.effects.effects) {
            if (!listed.empty()) {
                listed += ", ";
            }
            listed += ast::effect_name(effect);
        }
        return listed;
    }

    /// `unit meters;` - a name and nothing else (§10.2).
    ///
    /// A unit has no fields, no size and no representation, so there is
    /// nothing to lay out and no second pass: the name is the whole
    /// declaration.
    void collect_units(const ast::Program& program) {
        for (const ast::ItemPtr& item : program.items) {
            const auto* declaration = ast::node_cast<ast::UnitDecl>(item.get());
            if (declaration == nullptr) {
                continue;
            }
            const auto existing = units_.find(declaration->name);
            if (existing != units_.end()) {
                Diagnostic& diagnostic =
                    report("duplicate definition of unit `" + declaration->name + "`",
                           declaration->name_span,
                           "`" + declaration->name + "` is already a unit");
                note_previous(diagnostic, existing->second);
                continue;
            }
            units_.emplace(declaration->name, declaration->name_span);
        }
    }

    /// A written unit turned into a canonical dimension.
    ///
    /// Every factor has to name a declared unit: a typo would otherwise
    /// invent a silent new dimension that nothing could ever match,
    /// which is the failure mode this whole feature exists to prevent.
    Dimension resolve_unit(const std::vector<ast::UnitFactor>& unit) {
        Dimension dimension;
        for (const ast::UnitFactor& factor : unit) {
            if (units_.find(factor.name) == units_.end()) {
                report("unknown unit `" + factor.name + "`", factor.span,
                       "declare it with `unit " + factor.name + ";`");
                continue;
            }
            dimension.emplace_back(factor.name, factor.sign * factor.power);
        }
        return canonical_dimension(std::move(dimension));
    }

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
            const std::string qualified = qualify(declaration->name);
            const auto existing = result_.structs.find(qualified);
            if (existing != result_.structs.end()) {
                Diagnostic& diagnostic =
                    report("duplicate definition of type `" + declaration->name + "`",
                           declaration->name_span,
                           "`" + declaration->name + "` is already defined");
                note_previous(diagnostic, existing->second.span);
                continue;
            }
            StructInfo info;
            info.name = qualified;
            info.module = current_module_;
            info.is_public = declaration->is_public;
            info.span = declaration->name_span;
            result_.structs.emplace(qualified, std::move(info));
        }

        for (const ast::ItemPtr& item : program.items) {
            const auto* declaration = ast::node_cast<ast::StructDecl>(item.get());
            if (declaration == nullptr) {
                continue;
            }
            if (declaration->is_generic()) {
                continue;  // laid out per instantiation
            }
            const auto entry = result_.structs.find(qualify(declaration->name));
            if (entry == result_.structs.end() || !entry->second.fields.empty()) {
                continue;
            }
            resolve_fields(*declaration, entry->second);
        }

        detect_infinite_types();
    }

    void register_struct_template(const ast::StructDecl& declaration) {
        const std::string qualified = qualify(declaration.name);
        if (result_.struct_templates.count(qualified) != 0 ||
            result_.structs.count(qualified) != 0) {
            report("duplicate definition of type `" + declaration.name + "`",
                   declaration.name_span, "`" + declaration.name + "` is already defined");
            return;
        }

        StructTemplate tmpl;
        tmpl.name = qualified;
        tmpl.module = current_module_;
        tmpl.is_public = declaration.is_public;
        tmpl.span = declaration.name_span;
        tmpl.decl = &declaration;
        for (const ast::GenericParam& parameter : declaration.generic_params) {
            tmpl.generic_params.push_back(parameter.name);
        }
        check_generic_params(declaration.generic_params);
        result_.struct_templates.emplace(qualified, std::move(tmpl));
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
            resolved.is_public = field.is_public;
            resolved.span = field.name_span;
            resolved.type = resolve_type(*field.type);
            resolved.index = info.fields.size();
            // Infinite-size types are found afterwards, by walking the
            // whole containment graph: a struct can reach itself through
            // another struct or through an array, not just directly.
            if (is_owned(resolved.type)) {
                // The struct now owns heap memory too, so it moves and
                // drops like the field inside it.
                types().mark_owning(types().struct_type(info.name));
            }
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
        const std::string qualified = qualify(function.name);
        const auto existing = result_.functions.find(qualified);
        if (existing != result_.functions.end()) {
            Diagnostic& diagnostic =
                report("duplicate definition of function `" + function.name + "`",
                       function.name_span, "`" + function.name + "` is already defined");
            note_previous(diagnostic, existing->second.span);
            return;
        }

        FunctionInfo info = signature_of(function, {});
        info.module = current_module_;
        info.is_public = function.is_public;
        if (const ast::Param* self = function.self_param()) {
            report("`self` is only valid inside an `impl` block", self->span,
                   "free functions have no receiver");
        }
        result_.functions.emplace(qualified, std::move(info));
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
        const std::string qualified = qualify(function.name);
        if (result_.function_templates.count(qualified) != 0 ||
            result_.functions.count(qualified) != 0) {
            Diagnostic& diagnostic =
                report("duplicate definition of function `" + function.name + "`",
                       function.name_span, "`" + function.name + "` is already defined");
            const auto existing = result_.function_templates.find(qualified);
            if (existing != result_.function_templates.end()) {
                note_previous(diagnostic, existing->second.span);
            }
            return;
        }

        check_generic_params(function.generic_params);

        FunctionTemplate tmpl;
        tmpl.name = qualified;
        tmpl.simple_name = function.name;
        tmpl.module = current_module_;
        tmpl.is_public = function.is_public;
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

        // A parameter has to be findable somewhere: in an argument, or -
        // for a constructor, which has no arguments to go on - in the
        // return type, where the context supplies it. Appearing in
        // neither means no call could ever determine it, and there is no
        // turbofish to say so by hand.
        for (const std::string& parameter : tmpl.generic_params) {
            bool mentioned = mentions_parameter(signature.return_type, parameter);
            for (const TypePtr type : signature.param_types) {
                mentioned = mentioned || mentions_parameter(type, parameter);
            }
            if (!mentioned) {
                report("type parameter `" + parameter + "` cannot be inferred",
                       function.name_span,
                       "`" + parameter + "` appears in no parameter and no return type")
                    .with_note("type arguments come from the call: from an argument, or from "
                               "what the result is bound to");
            }
        }

        template_signatures_.emplace(qualified, std::move(signature));
        result_.function_templates.emplace(qualified, std::move(tmpl));
    }

    /// `impl<T> Pair<T> { ... }`.
    ///
    /// Every method becomes a template over the block's parameters plus
    /// any of its own. Which types those stand for is settled at the
    /// call: the receiver is a `Pair<int>`, so `T` is `int` - no
    /// inference needed for the block's share of them.
    void declare_generic_impl(const ast::ImplBlock& block, const std::string& owner) {
        const auto declared = result_.struct_templates.find(owner);
        if (declared == result_.struct_templates.end()) {
            Diagnostic& diagnostic =
                report(result_.structs.count(owner) != 0
                           ? "`" + block.type_name + "` is not generic"
                           : "cannot find type `" + block.type_name + "`",
                       block.type_name_span,
                       result_.structs.count(owner) != 0 ? "it takes no type parameters"
                                                         : "not found in this scope");
            if (result_.structs.count(owner) != 0) {
                diagnostic.with_note("write `impl " + block.type_name + " { ... }`");
            } else {
                suggest(diagnostic, block.type_name, struct_names());
            }
            return;
        }

        if (declared->second.generic_params.size() != block.generic_params.size()) {
            report("`" + block.type_name + "` takes " +
                       std::to_string(declared->second.generic_params.size()) +
                       " type parameter" +
                       (declared->second.generic_params.size() == 1 ? "" : "s") + " but this "
                       "`impl` declares " + std::to_string(block.generic_params.size()),
                   block.type_name_span, "the counts have to match")
                .with_note("every parameter of the type has to be named, because the `impl` "
                           "applies to all of them");
            return;
        }

        check_generic_params(block.generic_params);

        // `self` is `Pair<T>` here, with the block's own placeholders
        // standing in for the arguments.
        std::vector<std::string> block_params;
        std::vector<TypePtr> placeholders;
        for (const ast::GenericParam& parameter : block.generic_params) {
            block_params.push_back(parameter.name);
            placeholders.push_back(types().generic_type(parameter.name));
        }
        const TypePtr self_type = types().struct_type(owner, placeholders);

        for (const std::unique_ptr<ast::FunctionDecl>& method : block.methods) {
            declare_method_template(block, *method, owner, self_type, block_params);
        }
    }

    /// Records one method as a template, over `block_params` (from the
    /// `impl`) followed by its own.
    void declare_method_template(const ast::ImplBlock& block, const ast::FunctionDecl& method,
                                 const std::string& owner, TypePtr self_type,
                                 const std::vector<std::string>& block_params) {
        const auto key = std::make_pair(owner, method.name);
        if (method_templates_.count(key) != 0 || result_.methods.count(key) != 0) {
            Diagnostic& diagnostic =
                report("duplicate definition of method `" + method.name + "`",
                       method.name_span,
                       "`" + method.name + "` is already defined on `" + block.type_name + "`");
            const auto existing = method_templates_.find(key);
            if (existing != method_templates_.end()) {
                note_previous(diagnostic, existing->second.span);
            }
            return;
        }

        check_generic_params(method.generic_params);

        FunctionTemplate tmpl;
        tmpl.name = owner + "::" + method.name;
        tmpl.simple_name = method.name;
        tmpl.module = current_module_;
        tmpl.is_public = method.is_public;
        tmpl.owner_type = owner;
        tmpl.span = method.name_span;
        tmpl.decl = &method;
        tmpl.generic_params = block_params;
        for (const ast::GenericParam& parameter : method.generic_params) {
            if (std::find(block_params.begin(), block_params.end(), parameter.name) !=
                block_params.end()) {
                report("type parameter `" + parameter.name + "` shadows the `impl` block's",
                       parameter.span, "the block already declares `" + parameter.name + "`")
                    .with_note("give this one a different name");
                continue;
            }
            tmpl.generic_params.push_back(parameter.name);
        }

        const std::vector<std::string> saved = generic_scope_;
        generic_scope_ = tmpl.generic_params;
        FunctionInfo signature = signature_of(method, owner, self_type);
        generic_scope_ = saved;

        // The block's parameters come from the receiver, so only the
        // method's own have to be findable in the arguments.
        for (std::size_t i = block_params.size(); i < tmpl.generic_params.size(); ++i) {
            bool mentioned = false;
            for (const TypePtr type : signature.param_types) {
                mentioned = mentioned || mentions_parameter(type, tmpl.generic_params[i]);
            }
            if (!mentioned) {
                report("type parameter `" + tmpl.generic_params[i] + "` cannot be inferred",
                       method.name_span,
                       "`" + tmpl.generic_params[i] + "` does not appear in any parameter type")
                    .with_note("type arguments are inferred from the call, so every "
                               "parameter must be used by one");
            }
        }

        method_template_signatures_.emplace(key, std::move(signature));
        method_templates_.emplace(key, std::move(tmpl));
    }

    static bool mentions_parameter(TypePtr type, const std::string& name) {
        if (type == nullptr) {
            return false;
        }
        if (type->kind == TypeKind::Generic) {
            return type->name == name;
        }
        if (type->kind == TypeKind::Reference || type->kind == TypeKind::Array ||
            type->kind == TypeKind::Vec) {
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
        const std::string owner = qualify(block.type_name);

        if (block.is_generic()) {
            declare_generic_impl(block, owner);
            return;
        }
        if (result_.structs.find(owner) == result_.structs.end() &&
            result_.struct_templates.count(owner) != 0) {
            report("`" + block.type_name + "` is generic", block.type_name_span,
                   "an `impl` on it has to name its type parameters")
                .with_note("write `impl<T> " + block.type_name + "<T> { ... }`");
            return;
        }
        if (result_.structs.find(owner) == result_.structs.end()) {
            Diagnostic& diagnostic = report("cannot find type `" + block.type_name + "`",
                                            block.type_name_span, "not found in this scope");
            suggest(diagnostic, block.type_name, struct_names());
            // Methods are still recorded below so their bodies get
            // checked and callers get "unknown method" rather than a
            // second complaint about the type.
        }

        for (const std::unique_ptr<ast::FunctionDecl>& method : block.methods) {
            if (method->is_generic()) {
                // A method with type parameters of its own, on a struct
                // that has none. Its parameters are inferred from the
                // call, exactly as a free function's are.
                declare_method_template(block, *method, owner, /*self_type=*/nullptr, {});
                continue;
            }
            const auto key = std::make_pair(owner, method->name);
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
            FunctionInfo method_info = signature_of(*method, owner);
            method_info.module = current_module_;
            method_info.is_public = method->is_public;
            result_.methods.emplace(key, std::move(method_info));
        }
    }

    /// A module path as a linker-safe prefix: `a::b` becomes `a__b`.
    static std::string symbol_prefix(const std::string& module) {
        std::string out;
        for (std::size_t i = 0; i < module.size(); ++i) {
            if (module[i] == ':' && i + 1 < module.size() && module[i + 1] == ':') {
                out += "__";
                ++i;
                continue;
            }
            out.push_back(module[i]);
        }
        return out;
    }

    /// The symbol a function links as.
    ///
    /// `main` is left alone: the linker has to find it under that exact
    /// name, and only the entry module may define one.
    std::string mangle_symbol(const std::string& name, const std::string& owner) const {
        std::string base = owner.empty() ? name : owner + "_" + name;
        if (current_module_.empty()) {
            return base;
        }
        return symbol_prefix(current_module_) + "__" + base;
    }

    /// Builds the signature. The mangled name is where §4's "methods are
    /// sugar over plain functions" becomes concrete.
    /// `self_type` is what `self` stands for. It is the bare struct for
    /// an ordinary `impl`, and `Pair<T>` - placeholders and all - inside
    /// an `impl<T> Pair<T>`, because that is what the receiver will be.
    FunctionInfo signature_of(const ast::FunctionDecl& function, const std::string& owner,
                              TypePtr self_type = nullptr) {
        FunctionInfo info;
        info.name = function.name;
        info.owner_type = owner;
        info.mangled_name = mangle_symbol(function.name, owner);
        info.span = function.name_span;
        info.decl = &function;
        info.return_type =
            function.return_type ? resolve_type(*function.return_type) : types().void_type();

        for (const ast::Param& param : function.params) {
            if (param.is_self()) {
                info.self_kind = param.self_kind;
                const TypePtr owner_type =
                    self_type != nullptr
                        ? self_type
                        : (owner.empty() ? types().error_type() : types().struct_type(owner));
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
        const std::string qualified = qualify(constant.name);
        const auto existing = result_.constants.find(qualified);
        if (existing != result_.constants.end()) {
            report("duplicate definition of constant `" + constant.name + "`",
                   constant.name_span, "`" + constant.name + "` is already defined");
            return;
        }
        result_.constants.emplace(
            qualified, ConstantInfo{resolve_type(*constant.type), current_module_,
                                    constant.is_public, constant.name_span});
    }

    // -----------------------------------------------------------------
    // Ownership
    //
    // A value whose type owns heap memory - a `Vec`, a `String`, or an
    // aggregate containing one - moves rather than copies. After it
    // moves, the source is dead: using it is an error, and it is not
    // dropped at scope end because something else owns it now.
    //
    // There is no borrow checker. `&T` still borrows without moving and
    // is still unchecked, exactly as §4 describes, so none of this needs
    // lifetimes. What it buys is that heap memory is freed exactly once,
    // automatically, with no runtime bookkeeping.
    // -----------------------------------------------------------------

    /// Records a capture when a name comes from outside the closure
    /// currently being checked.
    ///
    /// Captures are by value: a copyable type is copied, an owned one is
    /// moved into the closure, which then owns it. That is the only rule
    /// consistent with the memory model - capturing a reference would
    /// hand out a pointer with no way to check it outlives the closure.
    void note_capture(const std::string& name, Span span, const Binding& binding) {
        if (closures_.empty()) {
            return;
        }
        const std::optional<std::size_t> depth = scopes_.depth_of(name);
        if (!depth.has_value()) {
            return;
        }

        // Walk outwards: a name from outside an enclosing closure is
        // captured by that one too, so a nested closure sees it.
        for (ClosureContext& context : closures_) {
            if (*depth >= context.scope_base) {
                continue;  // local to this closure
            }
            if (std::find(context.captured.begin(), context.captured.end(), name) !=
                context.captured.end()) {
                continue;
            }
            context.captured.push_back(name);
            context.closure->captures.push_back(ast::Capture{name, span});
        }
        (void)binding;
    }

    /// Whether handing `argument` to a parameter of type `parameter`
    /// lends it rather than gives it away.
    ///
    /// A `&T` parameter borrows, which is the obvious case. So does a
    /// `string` parameter given a `String`: what is passed is a view of
    /// the buffer, and the caller still owns the buffer.
    static bool passes_by_borrow(TypePtr parameter, TypePtr argument) {
        if (parameter == nullptr) {
            return false;
        }
        if (parameter->kind == TypeKind::Reference) {
            return true;
        }
        return parameter->kind == TypeKind::String &&
               strip_reference(argument) != nullptr &&
               strip_reference(argument)->kind == TypeKind::StringBuf;
    }

    /// Records that `expr` gives up ownership of whatever it names.
    ///
    /// Only a bare local can be moved *from*: moving out of a field or
    /// an element would leave a hole the drop code could not reason
    /// about, so those are rejected rather than tracked.
    void move_out_of(const ast::Expr& expr) {
        const TypePtr type = recorded_type(expr);
        if (!is_owned(type)) {
            return;  // copies; nothing to track
        }

        if (const auto* name = ast::node_cast<ast::NameExpr>(&expr)) {
            if (name->module.empty()) {
                if (Binding* binding = scopes_.lookup_mutable(name->name)) {
                    binding->moved = true;
                    binding->moved_at = expr.span;
                    // Recorded for codegen, which has to clear the drop
                    // flag on exactly the expressions the checker
                    // decided were moves.
                    result_.moved_expressions.emplace(current_instance_, &expr);
                    return;
                }
            }
        }

        switch (expr.kind) {
            case ast::ExprKind::FieldAccess:
            case ast::ExprKind::Index: {
                Diagnostic& diagnostic =
                    report("cannot move out of `" + to_string(type) + "` here", expr.span,
                           "only a whole variable can be moved");
                diagnostic.with_note("a field or element cannot be moved out on its own, "
                                     "because what remains would be half-owned");

                // There is a way to do what they meant, for a vector.
                const auto* index = ast::node_cast<ast::IndexExpr>(&expr);
                if (index != nullptr &&
                    strip_reference(recorded_type(*index->object))->kind == TypeKind::Vec) {
                    diagnostic.with_note("`pop` takes the last element out of a `Vec` and "
                                         "shortens it, which leaves nothing half-owned");
                }
                return;
            }
            default:
                // A temporary - a call result, a literal - owns itself
                // and is simply handed on.
                return;
        }
    }

    /// Reports a use of a value that has already been moved away.
    /// Returns true when the binding is still live.
    bool check_not_moved(const ast::NameExpr& expr, const Binding& binding) {
        if (!binding.moved) {
            return true;
        }
        Diagnostic& diagnostic =
            report("use of moved value `" + expr.name + "`", expr.span,
                   "`" + expr.name + "` was moved and no longer holds a value");
        diagnostic.with_note("moved at " + location_of(binding.moved_at));
        diagnostic.with_note("`" + to_string(binding.type) +
                             "` owns heap memory, so assigning or passing it moves it "
                             "rather than copying");
        return false;
    }

    // -----------------------------------------------------------------
    // Modules
    //
    // Every item is stored under a fully qualified name: `Point` in the
    // entry module, `geometry::Point` in a module called `geometry`.
    // Unqualified names resolve within the current module only - there
    // are no implicit imports - and a qualified name has to name a
    // module this one imported, and reach an item marked `pub`.
    //
    // This is where the `pub` that has been parsed and carried since
    // Phase 2 finally does something.
    // -----------------------------------------------------------------

    /// The fully qualified form of a name declared in the current module.
    std::string qualify(const std::string& name) const {
        return current_module_.empty() ? name : current_module_ + "::" + name;
    }

    /// The qualified name to look up for a written path, or nullopt when
    /// the module part is itself wrong (already reported).
    ///
    /// `module` empty means the path was unqualified, which resolves
    /// against whichever module is being checked.
    std::optional<std::string> lookup_name(const std::string& module, const std::string& name,
                                           Span span) {
        if (module.empty()) {
            return qualify(name);
        }
        if (module == current_module_) {
            // `geometry::area` written inside `geometry` itself: legal,
            // and reaches private items because it is the same module.
            return qualify(name);
        }

        if (std::find(declared_modules_.begin(), declared_modules_.end(), module) ==
            declared_modules_.end()) {
            Diagnostic& diagnostic = report("cannot find module `" + module + "`", span,
                                            "no module with this name was loaded");
            suggest(diagnostic, module, declared_modules_);
            return std::nullopt;
        }

        const auto imported = imports_.find(current_module_);
        if (imported == imports_.end() ||
            std::find(imported->second.begin(), imported->second.end(), module) ==
                imported->second.end()) {
            report("module `" + module + "` is not imported here", span,
                   "add `import " + module + ";` at the top of this file")
                .with_note("a module is only in scope for the file that imports it");
            return std::nullopt;
        }

        return module + "::" + name;
    }

    /// Reports when an item exists but the current module may not see it.
    /// Returns true when access is allowed.
    bool check_visible(const std::string& owner, bool is_public, const std::string& display,
                       Span span, std::string_view what, Span declared) {
        if (is_public || owner == current_module_) {
            return true;
        }
        Diagnostic& diagnostic =
            report(std::string{what} + " `" + display + "` is private", span,
                   "`" + display + "` is not declared `pub`");
        diagnostic.with_note("declared at " + location_of(declared));
        return false;
    }

    /// How a path reads back in a diagnostic.
    static std::string path_string(const std::string& module, const std::string& name) {
        return module.empty() ? name : module + "::" + name;
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
            case TypeKind::Vec:
                return types().vec_of(substitute(type->element, bindings));
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
        if (parameter->kind == TypeKind::Vec) {
            const TypePtr concrete = strip_reference(argument);
            return concrete != nullptr && concrete->kind == TypeKind::Vec &&
                   unify(parameter->element, concrete->element, bindings);
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

    /// Notes that whatever is being checked right now needs `instance`.
    ///
    /// Inside a generic body the answer is another instantiation rather
    /// than a module, because the outer copy may itself end up in
    /// several object files; that edge is resolved after the walk.
    void note_demand(Instantiation& instance) {
        if (current_instance_ == kRootInstance) {
            instance.demanded_by.insert(current_module_);
        } else {
            instance_demands_[current_instance_].insert(instance.id);
        }
    }

    /// Pushes each instantiation's demanding modules along the edges
    /// recorded above, until nothing changes.
    ///
    /// A fixpoint rather than a walk because generic functions may call
    /// each other in a cycle, and `max<int>` calling `min<int>` means
    /// every object holding the first also needs the second.
    void resolve_instance_demand() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [from, targets] : instance_demands_) {
                if (from == kRootInstance || from > result_.instantiations.size()) {
                    continue;
                }
                const std::set<std::string> sources =
                    result_.instantiations[from - 1].demanded_by;
                for (const InstanceId to : targets) {
                    if (to == kRootInstance || to > result_.instantiations.size()) {
                        continue;
                    }
                    std::set<std::string>& sink = result_.instantiations[to - 1].demanded_by;
                    const std::size_t before = sink.size();
                    sink.insert(sources.begin(), sources.end());
                    changed = changed || sink.size() != before;
                }
            }
        }
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
                note_demand(existing);
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
        info.name = tmpl.simple_name;
        info.owner_type = tmpl.owner_type;
        info.display_name = display;
        info.type_args = args;
        // `tmpl.name` is already qualified; turn the `::` into a
        // linker-safe prefix rather than mangling it as punctuation.
        std::string base = tmpl.simple_name;
        if (!tmpl.owner_type.empty()) {
            base = tmpl.owner_type + "_" + base;
        }
        if (!tmpl.module.empty()) {
            base = symbol_prefix(tmpl.module) + "__" + base;
        }
        info.mangled_name = mangle_instance(base, args);
        info.span = tmpl.span;
        info.decl = tmpl.decl;

        const FunctionInfo& generic_signature = signature_for(tmpl);
        info.module = tmpl.module;
        info.is_public = tmpl.is_public;
        info.param_names = generic_signature.param_names;
        info.self_kind = generic_signature.self_kind;
        for (const TypePtr parameter : generic_signature.param_types) {
            info.param_types.push_back(substitute(parameter, bindings));
        }
        info.return_type = substitute(generic_signature.return_type, bindings);

        result_.instantiations.push_back(std::move(instance));
        pending_instances_.push_back(result_.instantiations.size() - 1);
        note_demand(result_.instantiations.back());
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
            const std::string previous_module = current_module_;

            current_instance_ = instance.id;
            current_bindings_ = &instance.bindings;
            // A template's body resolves names as the module that wrote
            // it, not the one that happened to instantiate it.
            current_module_ = instance.info.module;
            check_function(*instance.decl, instance.info);
            current_instance_ = previous_instance;
            current_bindings_ = previous_bindings;
            current_module_ = previous_module;
        }
    }

    // -----------------------------------------------------------------
    // Type resolution
    // -----------------------------------------------------------------

    TypePtr resolve_type(const ast::TypeRef& type) {
        switch (type.kind) {
            case ast::TypeKind::Int:
                return type.unit.empty()
                           ? types().int_type()
                           : types().numeric_type(TypeKind::Int, resolve_unit(type.unit));
            case ast::TypeKind::Float:
                return type.unit.empty()
                           ? types().float_type()
                           : types().numeric_type(TypeKind::Float, resolve_unit(type.unit));
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

            case ast::TypeKind::Function: {
                std::vector<TypePtr> params;
                for (const ast::TypeRefPtr& param : type.params) {
                    params.push_back(resolve_type(*param));
                }
                return types().function_of(
                    params, type.result ? resolve_type(*type.result) : types().void_type(),
                    type.effects.present, mask_of(type.effects));
            }
        }
        return types().error_type();
    }

    /// A written name in type position: a bound type parameter, an
    /// in-scope parameter of the template being declared, a generic
    /// struct applied to arguments, or a plain struct.
    TypePtr resolve_named_type(const ast::TypeRef& type) {
        // A type parameter is never module-qualified, so those two
        // lookups only apply to a bare name.
        if (type.module.empty()) {
            if (current_bindings_ != nullptr) {
                const auto bound = current_bindings_->find(type.name);
                if (bound != current_bindings_->end()) {
                    if (!type.type_args.empty()) {
                        report("type parameter `" + type.name +
                                   "` cannot take type arguments",
                               type.span, "`" + type.name + "` is not a generic type");
                    }
                    return bound->second;
                }
            }
            if (std::find(generic_scope_.begin(), generic_scope_.end(), type.name) !=
                generic_scope_.end()) {
                return types().generic_type(type.name);
            }
        }

        // The two owned built-ins. They behave like generic structs in
        // type position but are known to the compiler, since there is no
        // way to write a heap-allocating type in Ember itself.
        if (type.module.empty() && type.name == "Vec") {
            if (type.type_args.size() != 1) {
                report("`Vec` takes 1 type argument but " +
                           std::to_string(type.type_args.size()) + " " +
                           (type.type_args.size() == 1 ? "was" : "were") + " given",
                       type.span, "write it as `Vec<int>`");
                return types().error_type();
            }
            // The element may own memory of its own: dropping the
            // vector walks its live elements first. `move_out_of` still
            // refuses to move one out of the middle, so `pop` is the
            // only way to take ownership of an element back.
            return types().vec_of(resolve_type(*type.type_args.front()));
        }
        if (type.module.empty() && type.name == "String") {
            if (!type.type_args.empty()) {
                report("`String` is not a generic type", type.span,
                       "it takes no type arguments");
                return types().error_type();
            }
            return types().string_buf_type();
        }

        const std::optional<std::string> qualified =
            lookup_name(type.module, type.name, type.span);
        if (!qualified.has_value()) {
            return types().error_type();
        }

        const auto tmpl = result_.struct_templates.find(*qualified);
        if (tmpl != result_.struct_templates.end()) {
            if (!check_visible(tmpl->second.module, tmpl->second.is_public,
                               path_string(type.module, type.name), type.span, "type",
                               tmpl->second.span)) {
                return types().error_type();
            }
            return instantiate_struct(tmpl->second, type);
        }

        if (!type.type_args.empty()) {
            report("`" + path_string(type.module, type.name) + "` is not a generic type",
                   type.span, "it takes no type arguments");
            return types().error_type();
        }

        const auto found = result_.structs.find(*qualified);
        if (found == result_.structs.end()) {
            Diagnostic& diagnostic =
                report("cannot find type `" + path_string(type.module, type.name) + "`",
                       type.span, "not found in this scope");
            suggest(diagnostic, type.name, struct_names());
            return types().error_type();
        }
        if (!check_visible(found->second.module, found->second.is_public,
                           path_string(type.module, type.name), type.span, "type",
                           found->second.span)) {
            return types().error_type();
        }
        return types().struct_type(*qualified);
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

    /// Candidates for a "did you mean" suggestion: only what the
    /// current module can actually name without a qualifier.
    std::vector<std::string> struct_names() const {
        std::vector<std::string> names;
        const std::string prefix = current_module_.empty() ? "" : current_module_ + "::";
        for (const auto& [name, info] : result_.structs) {
            if (info.module == current_module_) {
                names.push_back(name.substr(prefix.size()));
            }
        }
        for (const auto& [name, tmpl] : result_.struct_templates) {
            if (tmpl.module == current_module_) {
                names.push_back(name.substr(prefix.size()));
            }
        }
        return names;
    }

    std::vector<std::string> function_names() const {
        std::vector<std::string> names;
        const std::string prefix = current_module_.empty() ? "" : current_module_ + "::";
        for (const auto& [name, info] : result_.functions) {
            if (info.module == current_module_) {
                names.push_back(name.substr(prefix.size()));
            }
        }
        for (const auto& [name, tmpl] : result_.function_templates) {
            if (tmpl.module == current_module_) {
                names.push_back(name.substr(prefix.size()));
            }
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
            const auto declared = result_.constants.find(qualify(constant->name));
            const TypePtr expected = declared != result_.constants.end()
                                         ? declared->second.type
                                         : types().error_type();
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
                const auto entry = result_.functions.find(qualify(function->name));
                if (entry != result_.functions.end() && entry->second.decl == function) {
                    check_function(*function, entry->second);
                }
            } else if (const auto* block = ast::node_cast<ast::ImplBlock>(item.get())) {
                for (const std::unique_ptr<ast::FunctionDecl>& method : block->methods) {
                    const auto entry =
                        result_.methods.find(std::make_pair(qualify(block->type_name),
                                                            method->name));
                    if (entry != result_.methods.end() && entry->second.decl == method.get()) {
                        check_function(*method, entry->second);
                    }
                }
            }
        }
    }

    /// `requires` and `ensures` clauses (10.1).
    ///
    /// Each condition has to be a `bool`, like the condition of an
    /// `if`. Inside an `ensures`, `result` names the value about to be
    /// returned; inside a `requires` it does not exist, because the
    /// function has not run yet.
    void check_contracts(const ast::FunctionDecl& function, const FunctionInfo& info) {
        if (function.contracts.empty()) {
            return;
        }
        const bool returns_value = info.return_type != nullptr &&
                                   info.return_type->kind != TypeKind::Void &&
                                   !is_error(info.return_type);

        for (const ast::Contract& contract : function.contracts) {
            current_contract_ = &contract;
            scopes_.push();

            if (contract.is_ensures() && returns_value) {
                // Bound here rather than reserved in the lexer: making
                // `result` a keyword would break every existing program
                // that has a variable by that name, for a word that is
                // only meaningful inside one clause.
                scopes_.declare("result", Binding{info.return_type, false, true,
                                                  contract.keyword_span, false, {}});
            }

            const TypePtr type = check_expr(*contract.condition);
            if (!is_error(type) && type->kind != TypeKind::Bool) {
                report("a contract must be a `bool`", contract.condition->span,
                       "expected `bool`, found `" + to_string(type) + "`");
            }

            scopes_.pop();
            current_contract_ = nullptr;
        }
    }

    void check_function(const ast::FunctionDecl& function, const FunctionInfo& info) {
        // A declaration with no body is a promise that one exists
        // elsewhere. There is nothing to walk and nothing to require a
        // return from.
        if (!function.has_body) {
            return;
        }

        // TODO(v2): `pub` is parsed and carried on every item, but v1 is
        // single-file so there is no boundary to enforce it across (§4).
        // When the module system lands, this is where a reference to a
        // private item from another module becomes an error - the
        // syntax and the AST already carry everything that needs.
        current_function_ = &info;
        current_effects_ = &effects_[info.mangled_name];
        current_effects_->decl = &function;
        scopes_.push();

        for (std::size_t i = 0; i < function.params.size(); ++i) {
            const ast::Param& param = function.params[i];
            const std::string& name = info.param_names[i];
            // Parameters are immutable bindings, as in Rust without
            // `mut`; assigning to one is a diagnostic, not a silent copy.
            const Binding* existing =
                scopes_.declare(name, Binding{info.param_types[i], false, true, param.span, false, {}});
            if (existing != nullptr) {
                Diagnostic& diagnostic =
                    report("duplicate definition of parameter `" + name + "`", param.name_span,
                           "`" + name + "` is already a parameter of this function");
                note_previous(diagnostic, existing->span);
            }
        }

        // Before the body, and in a scope holding only the
        // parameters: a contract belongs to the signature, so it can
        // see what the signature can see and nothing a local
        // introduces.
        check_contracts(function, info);

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
        current_effects_ = nullptr;
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
        // An annotation is resolved first and handed down, so an
        // expression with nothing else to go on - `new_vec()`, or an
        // empty array literal - can take its type from it. Everything
        // else ignores the hint and is checked against it as before.
        const TypePtr hint =
            statement.declared_type ? resolve_type(*statement.declared_type) : nullptr;

        const TypePtr initializer = check_expr(*statement.value, hint);
        TypePtr type = initializer;

        if (statement.declared_type) {
            // Annotated: the annotation wins, and the initializer is
            // checked against it. This is the §7 worked example.
            type = hint;
            if (!assignable(type, initializer)) {
                report_mismatch(statement.value->span, type, initializer);
            }
        } else if (initializer != nullptr && initializer->kind == TypeKind::Void) {
            report("cannot infer a type", statement.value->span,
                   "this expression has no value to bind");
            type = types().error_type();
        }

        // `let b = a;` takes ownership from `a`.
        move_out_of(*statement.value);

        result_.binding_types[{current_instance_, &statement}] = type;

        const Binding* existing = scopes_.declare(
            statement.name, Binding{type, statement.is_mutable, false, statement.name_span, false, {}});
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

        // The return type is a hint, exactly as a parameter type is at
        // a call. Without it `return |x| ...;` has nothing to take the
        // closure's parameter and result types from, and now nothing to
        // take its effect bound from either.
        const TypePtr actual = check_expr(*statement.value, expected);
        move_out_of(*statement.value);
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

        // Moves made on a branch that cannot fall through are put back
        // before checking what follows it. Without this, the ordinary
        // shape
        //
        //     if done { return answer; }
        //     use(answer);
        //
        // would report the second line as a use of something moved on a
        // path that never reaches it.
        const Scopes::MoveState before_then = scopes_.moves();
        check_block(statement.then_block);
        if (always_returns(statement.then_block)) {
            scopes_.restore_moves(before_then);
        }

        if (statement.else_branch) {
            const Scopes::MoveState before_else = scopes_.moves();
            check_stmt(*statement.else_branch);
            if (always_returns(*statement.else_branch)) {
                scopes_.restore_moves(before_else);
            }
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
        // A bare name on the left is written, not read. Anything more
        // complex - `p.x`, `xs[0]` - does read the container, so only
        // the simple case is exempt from the moved check.
        assignment_target_ = ast::node_cast<ast::NameExpr>(statement.target.get()) != nullptr
                                 ? statement.target.get()
                                 : nullptr;
        const TypePtr target = check_expr(*statement.target);
        assignment_target_ = nullptr;

        // The target's type is the hint, so `a = new_vec();` knows what
        // it is building.
        const TypePtr value = check_expr(*statement.value, target);

        if (!is_assignable_place(*statement.target)) {
            report("invalid assignment target", statement.target->span,
                   "only variables, fields and array elements can be assigned to");
            return;
        }
        check_mutable(*statement.target);

        if (!assignable(target, value)) {
            report_mismatch(statement.value->span, target, value);
        }

        move_out_of(*statement.value);

        // Assigning back into a moved-out variable makes it live again.
        if (const auto* name = ast::node_cast<ast::NameExpr>(statement.target.get())) {
            if (name->module.empty()) {
                if (Binding* binding = scopes_.lookup_mutable(name->name)) {
                    binding->moved = false;
                }
            }
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
        if (result_.constants.count(qualify(name->name)) != 0 &&
            scopes_.lookup(name->name) == nullptr) {
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

    TypePtr check_expr(const ast::Expr& expr, TypePtr hint = nullptr) {
        switch (expr.kind) {
            case ast::ExprKind::IntLit: {
                const auto& literal = static_cast<const ast::IntLitExpr&>(expr);
                return record(expr, literal.unit.empty()
                                        ? types().int_type()
                                        : types().numeric_type(TypeKind::Int,
                                                               resolve_unit(literal.unit)));
            }
            case ast::ExprKind::FloatLit: {
                const auto& literal = static_cast<const ast::FloatLitExpr&>(expr);
                return record(expr, literal.unit.empty()
                                        ? types().float_type()
                                        : types().numeric_type(TypeKind::Float,
                                                               resolve_unit(literal.unit)));
            }
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
                return check_call(static_cast<const ast::CallExpr&>(expr), hint);
            case ast::ExprKind::MethodCall:
                return check_method_call(static_cast<const ast::MethodCallExpr&>(expr));
            case ast::ExprKind::FieldAccess:
                return check_field_access(static_cast<const ast::FieldAccessExpr&>(expr));
            case ast::ExprKind::Index:
                return check_index(static_cast<const ast::IndexExpr&>(expr));
            case ast::ExprKind::Closure:
                return check_closure(static_cast<const ast::ClosureExpr&>(expr), hint);
            case ast::ExprKind::Cast:
                return check_cast(static_cast<const ast::CastExpr&>(expr));
            case ast::ExprKind::StructLit:
                return check_struct_literal(static_cast<const ast::StructLitExpr&>(expr));
            case ast::ExprKind::ArrayLit:
                return check_array_literal(static_cast<const ast::ArrayLitExpr&>(expr), hint);
        }
        return record(expr, types().error_type());
    }

    TypePtr check_name(const ast::NameExpr& expr) {
        // A local always wins, and can never be module-qualified.
        if (expr.module.empty()) {
            if (const Binding* binding = scopes_.lookup(expr.name)) {
                record(expr, binding->type);
                if (&expr != assignment_target_ && !check_not_moved(expr, *binding)) {
                    return record(expr, types().error_type());
                }
                note_capture(expr.name, expr.span, *binding);
                return binding->type;
            }
        }

        // `result` is an ordinary identifier, so an unresolved one
        // would otherwise be reported as simply undefined. Inside a
        // contract that is never the useful answer: the name is real,
        // it is just not in scope here.
        if (current_contract_ != nullptr && expr.module.empty() && expr.name == "result") {
            if (!current_contract_->is_ensures()) {
                report("`result` is not in scope in a `requires`", expr.span,
                       "a `requires` is checked before the function runs, so there is "
                       "no result yet");
            } else {
                report("this function returns nothing, so it has no `result`", expr.span,
                       "an `ensures` on a function without a return type can still talk "
                       "about its parameters");
            }
            return record(expr, types().error_type());
        }

        const std::optional<std::string> qualified =
            lookup_name(expr.module, expr.name, expr.span);
        if (!qualified.has_value()) {
            return record(expr, types().error_type());
        }

        const auto constant = result_.constants.find(*qualified);
        if (constant != result_.constants.end()) {
            if (!check_visible(constant->second.module, constant->second.is_public,
                               path_string(expr.module, expr.name), expr.span, "constant",
                               constant->second.span)) {
                return record(expr, types().error_type());
            }
            return record(expr, constant->second.type);
        }

        if (expr.name == "self") {
            report("`self` is not available here", expr.span,
                   "only methods declared with a `self` receiver can use it");
            return record(expr, types().error_type());
        }

        Diagnostic& diagnostic =
            report("cannot find value `" + path_string(expr.module, expr.name) + "`", expr.span,
                   "not found in this scope");
        std::vector<std::string> candidates = scopes_.names();
        const std::string constant_prefix =
            current_module_.empty() ? "" : current_module_ + "::";
        for (const auto& [name, info] : result_.constants) {
            if (info.module == current_module_) {
                candidates.push_back(name.substr(constant_prefix.size()));
            }
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
                // A `string` and a `String` hold the same bytes and
                // differ only in who owns them, so comparing one with
                // the other is comparing text with text.
                if (is_text(left) && is_text(right)) {
                    return record(expr, types().bool_type());
                }
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
                // Text orders lexicographically, by bytes. That is the
                // same order as by code point for UTF-8, and it is an
                // order rather than a collation - `Z` before `a`.
                if (is_text(left) && is_text(right)) {
                    return record(expr, types().bool_type());
                }
                if (left != right) {
                    return record(expr, mismatched_operands(expr, symbol, left, right));
                }
                if (!is_numeric(left)) {
                    report("cannot compare values of type `" + to_string(left) + "`", expr.span,
                           "`" + symbol + "` needs an `int`, a `float` or text");
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
                // `left`, not the plain `int`: a remainder of two
                // lengths is a length.
                return record(expr, left);
            }

            case ast::BinaryOp::Multiply:
            case ast::BinaryOp::Divide: {
                // The one place units combine rather than having to
                // match. `*` adds the exponents and `/` subtracts them,
                // so `meters / seconds` is a speed and `meters / meters`
                // is a plain number again (§10.2).
                if (!is_numeric(left) || !is_numeric(right)) {
                    report("cannot apply `" + symbol + "` to `" + to_string(left) + "` and `" +
                               to_string(right) + "`",
                           expr.span, "`" + symbol + "` needs an `int` or a `float`");
                    return record(expr, types().error_type());
                }
                if (left->kind != right->kind) {
                    // int against float: still forbidden, units or not.
                    return record(expr, mismatched_operands(expr, symbol, left, right));
                }
                const Dimension combined =
                    combine_dimensions(dimension_of(left), dimension_of(right),
                                       expr.op == ast::BinaryOp::Divide ? -1 : 1);
                return record(expr, types().numeric_type(left->kind, combined));
            }

            default: {
                // + and -. Units have to match exactly, which the
                // interning already enforces: `float<meters>` and
                // `float<seconds>` are different types, so the ordinary
                // mismatch check catches them and only the message
                // needs to know why.
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
            if (left->kind == right->kind) {
                // Same number, different unit - so the note about `int`
                // and `float` would be beside the point.
                diagnostic.with_note("`" + symbol + "` needs the same unit on both sides (§10.2)");
                if (dimension_of(left).empty() || dimension_of(right).empty()) {
                    diagnostic.with_note(
                        "a plain number has no unit, and no unit is not the same as any unit");
                }
            } else {
                diagnostic.with_note("`int` and `float` never mix implicitly in Ember (§4)");
            }
        }
        return types().error_type();
    }

    TypePtr check_call(const ast::CallExpr& expr, TypePtr hint = nullptr) {
        // A local holding a function value shadows everything else: this
        // is what makes `f(1)` work when `f` is a closure.
        if (expr.module.empty()) {
            if (const Binding* binding = scopes_.lookup(expr.callee)) {
                return check_indirect_call(expr, *binding);
            }
        }

        // Intrinsics belong to no module and are never qualified.
        if (expr.module.empty() && is_intrinsic(expr.callee)) {
            return check_intrinsic(expr, hint);
        }

        const std::optional<std::string> qualified =
            lookup_name(expr.module, expr.callee, expr.callee_span);
        if (!qualified.has_value()) {
            for (const ast::ExprPtr& arg : expr.args) {
                check_expr(*arg);
            }
            return record(expr, types().error_type());
        }

        const auto tmpl = result_.function_templates.find(*qualified);
        if (tmpl != result_.function_templates.end()) {
            if (!check_visible(tmpl->second.module, tmpl->second.is_public,
                               path_string(expr.module, expr.callee), expr.callee_span,
                               "function", tmpl->second.span)) {
                for (const ast::ExprPtr& arg : expr.args) {
                    check_expr(*arg);
                }
                return record(expr, types().error_type());
            }
            return check_generic_call(expr, tmpl->second, hint);
        }

        const auto entry = result_.functions.find(*qualified);
        if (entry == result_.functions.end()) {
            for (const auto& [key, method] : result_.methods) {
                if (key.second == expr.callee) {
                    Diagnostic& diagnostic =
                        report("cannot find function `" +
                                   path_string(expr.module, expr.callee) + "`",
                               expr.callee_span, "not found in this scope");
                    diagnostic.with_note("`" + expr.callee + "` is a method on `" + key.first +
                                         "`; call it as `value." + expr.callee + "(...)`");
                    for (const ast::ExprPtr& arg : expr.args) {
                        check_expr(*arg);
                    }
                    return record(expr, types().error_type());
                }
            }
            Diagnostic& diagnostic =
                report("cannot find function `" + path_string(expr.module, expr.callee) + "`",
                       expr.callee_span, "not found in this scope");
            suggest(diagnostic, expr.callee, function_names());
            for (const ast::ExprPtr& arg : expr.args) {
                check_expr(*arg);
            }
            return record(expr, types().error_type());
        }

        const FunctionInfo& info = entry->second;
        if (!check_visible(info.module, info.is_public, path_string(expr.module, expr.callee),
                           expr.callee_span, "function", info.span)) {
            for (const ast::ExprPtr& arg : expr.args) {
                check_expr(*arg);
            }
            return record(expr, types().error_type());
        }
        result_.call_targets[{current_instance_, &expr}] = &info;
        note_call(&info, expr.span);
        check_arguments(expr.span, expr.args, info, 0);
        return record(expr, info.return_type);
    }

    /// `f(args)` where `f` is a variable holding a function value.
    ///
    /// Calling reads the closure rather than consuming it, so `f` is
    /// still usable afterwards - only assigning or passing it moves it.
    TypePtr check_indirect_call(const ast::CallExpr& expr, const Binding& binding) {
        // A `fn(int) -> int` says nothing about what it does, so a call
        // through one could do anything. Counting it as the worst case
        // keeps a declared bound honest rather than quietly wrong:
        // `uses io` still permits this, `uses nothing` does not.
        // What this call performs is whatever the function type
        // permits. An unbounded one - `fn(int) -> int`, as every such
        // type was written before §10.3 - could do anything, so it
        // counts as everything; a bounded one counts as exactly its
        // bound, which is the point of putting effects in the type.
        const TypePtr called = strip_reference(binding.type);
        if (called != nullptr && called->kind == TypeKind::Function &&
            called->effects_bounded) {
            for (const ast::Effect effect : {ast::Effect::Io, ast::Effect::Mut}) {
                if ((called->effects & bit_of(effect)) != 0) {
                    note_effect(effect, expr.span);
                }
            }
        } else {
            note_effect(ast::Effect::Io, expr.span,
                        "this calls a function value whose type carries no `uses` clause, "
                        "so it counts as performing any effect");
        }

        for (const ast::ExprPtr& arg : expr.args) {
            check_expr(*arg);
        }

        const TypePtr callee = strip_reference(binding.type);
        if (is_error(callee)) {
            return record(expr, types().error_type());
        }
        if (callee->kind != TypeKind::Function) {
            report("`" + expr.callee + "` is not callable", expr.callee_span,
                   "it holds a `" + to_string(callee) + "`, which is not a function");
            return record(expr, types().error_type());
        }

        if (binding.moved) {
            Diagnostic& diagnostic =
                report("use of moved value `" + expr.callee + "`", expr.callee_span,
                       "`" + expr.callee + "` was moved and no longer holds a value");
            diagnostic.with_note("moved at " + location_of(binding.moved_at));
            return record(expr, types().error_type());
        }

        // Capturing the closure name here keeps it alive for codegen and
        // records the capture if this call is inside another closure.
        note_capture(expr.callee, expr.callee_span, binding);

        if (expr.args.size() != callee->args.size()) {
            report("this function takes " + std::to_string(callee->args.size()) +
                       " argument" + (callee->args.size() == 1 ? "" : "s") + " but " +
                       std::to_string(expr.args.size()) + " " +
                       (expr.args.size() == 1 ? "was" : "were") + " supplied",
                   expr.span,
                   "expected " + std::to_string(callee->args.size()) + ", found " +
                       std::to_string(expr.args.size()));
            return record(expr, callee->result);
        }

        for (std::size_t i = 0; i < expr.args.size(); ++i) {
            const TypePtr argument = recorded_type(*expr.args[i]);
            if (!assignable(callee->args[i], argument)) {
                report_mismatch(expr.args[i]->span, callee->args[i], argument);
            }
            if (!passes_by_borrow(callee->args[i], argument)) {
                move_out_of(*expr.args[i]);
            }
        }
        return record(expr, callee->result);
    }

    /// A call to a method of a generic `impl`, or a generic method.
    ///
    /// The receiver settles the block's type parameters outright - a
    /// `Pair<int>` makes `T` `int`, with nothing to infer - and any
    /// parameters the method declared for itself are then inferred from
    /// the arguments, the same way a free function's are.
    TypePtr check_generic_method_call(const ast::MethodCallExpr& expr, TypePtr receiver,
                                      const FunctionTemplate& tmpl) {
        const FunctionInfo& signature = signature_for(tmpl);

        if (!check_visible(tmpl.module, tmpl.is_public, expr.method, expr.method_span, "method",
                           tmpl.span)) {
            return record(expr, types().error_type());
        }

        const auto declared = result_.struct_templates.find(tmpl.owner_type);
        const std::size_t from_receiver =
            declared == result_.struct_templates.end() ? 0
                                                       : declared->second.generic_params.size();

        std::map<std::string, TypePtr> bindings;
        for (std::size_t i = 0; i < from_receiver && i < tmpl.generic_params.size(); ++i) {
            if (i >= receiver->args.size()) {
                report("cannot work out what `" + tmpl.generic_params[i] + "` is here",
                       expr.method_span,
                       "the receiver does not say, and nothing else can");
                return record(expr, types().error_type());
            }
            bindings.emplace(tmpl.generic_params[i], receiver->args[i]);
        }

        // Self is the first parameter, so the written arguments start at
        // index 1 of the signature.
        const std::size_t expected =
            signature.param_types.empty() ? 0 : signature.param_types.size() - 1;
        if (signature.self_kind == ast::SelfKind::None) {
            report("`" + expr.method + "` is an associated function, not a method",
                   expr.method_span, "it has no `self` receiver")
                .with_note("associated functions have no call syntax in v1");
            return record(expr, types().error_type());
        }
        if (expr.args.size() != expected) {
            Diagnostic& diagnostic =
                report("this method takes " + std::to_string(expected) + " argument" +
                           (expected == 1 ? "" : "s") + " but " +
                           std::to_string(expr.args.size()) + " " +
                           (expr.args.size() == 1 ? "was" : "were") + " supplied",
                       expr.span,
                       "expected " + std::to_string(expected) + ", found " +
                           std::to_string(expr.args.size()));
            note_declared_at(diagnostic, tmpl.span);
            return record(expr, types().error_type());
        }

        for (std::size_t i = 0; i < expr.args.size(); ++i) {
            const TypePtr argument = recorded_type(*expr.args[i]);
            if (!unify(signature.param_types[i + 1], argument, bindings)) {
                report_mismatch(expr.args[i]->span,
                                substitute(signature.param_types[i + 1], bindings), argument);
                return record(expr, types().error_type());
            }
        }

        std::vector<TypePtr> args;
        for (const std::string& parameter : tmpl.generic_params) {
            const auto bound = bindings.find(parameter);
            if (bound == bindings.end() || is_generic(bound->second)) {
                report("cannot infer type parameter `" + parameter + "`", expr.span,
                       bound == bindings.end()
                           ? "nothing in this call determines `" + parameter + "`"
                           : "it would depend on an unsubstituted type parameter");
                return record(expr, types().error_type());
            }
            args.push_back(bound->second);
        }

        const FunctionInfo* instance = instantiate(tmpl, args, expr.method_span);
        if (instance == nullptr) {
            return record(expr, types().error_type());
        }
        result_.call_targets[{current_instance_, &expr}] = instance;
        note_call(instance, expr.span);

        check_arguments(expr.span, expr.args, *instance, 1);
        return record(expr, instance->return_type);
    }

    /// A call to a generic function: infer the type arguments from the
    /// arguments actually passed, then check the call against the
    /// instantiated signature like any other.
    TypePtr check_generic_call(const ast::CallExpr& expr, const FunctionTemplate& tmpl,
                               TypePtr hint = nullptr) {
        const FunctionInfo& signature = signature_for(tmpl);

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

        // A constructor - `fn new_stack<T>() -> Stack<T>` - has nothing
        // in its arguments to go on, so what the context expects is the
        // only thing that can say. `let s: Stack<int> = new_stack();`
        // reads the same way `let v: Vec<int> = new_vec();` already did.
        if (hint != nullptr && !is_error(hint)) {
            unify(signature.return_type, hint, bindings);
        }

        std::vector<TypePtr> args;
        for (const std::string& parameter : tmpl.generic_params) {
            const auto bound = bindings.find(parameter);
            if (bound == bindings.end()) {
                Diagnostic& diagnostic =
                    report("cannot infer type parameter `" + parameter + "`", expr.span,
                           "nothing in this call determines `" + parameter + "`");
                if (mentions_parameter(signature.return_type, parameter)) {
                    diagnostic.with_note(
                        "`" + parameter + "` only appears in the return type, so it has to "
                        "come from the context: annotate what this is being bound to");
                }
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
        note_call(info, expr.span);
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
            // A generic struct's methods are keyed by the bare name -
            // `Pair`, not `Pair<int>` - because one template serves
            // every instantiation.
            const auto tmpl = method_templates_.find(std::make_pair(base->name, expr.method));
            if (tmpl != method_templates_.end()) {
                return check_generic_method_call(expr, base, tmpl->second);
            }

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
        if (!check_visible(info.module, info.is_public, expr.method, expr.method_span, "method",
                           info.span)) {
            return record(expr, types().error_type());
        }
        result_.call_targets[{current_instance_, &expr}] = &info;
        note_call(&info, expr.span);

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
            // The parameter type is handed down as a hint, which is what
            // lets `map(xs, |x| x * 2)` know what `x` is.
            const TypePtr argument = already_checked(*args[i])
                                         ? recorded_type(*args[i])
                                         : check_expr(*args[i], parameter);
            if (!assignable(parameter, argument)) {
                report_mismatch(args[i]->span, parameter, argument);
            }
            // A `&T` parameter borrows; a `T` parameter takes ownership.
            if (!passes_by_borrow(parameter, recorded_type(*args[i]))) {
                move_out_of(*args[i]);
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
    TypePtr check_intrinsic(const ast::CallExpr& expr, TypePtr hint = nullptr) {
        // The only sources of an effect in the language. Everything
        // else an intrinsic does is arithmetic or memory, which no
        // effect is about (§10.3).
        if (expr.callee == "println" || expr.callee == "print") {
            note_effect(ast::Effect::Io, expr.span);
        }

        if (expr.callee == "new_vec" || expr.callee == "new_string") {
            return check_constructor(expr, hint);
        }
        if (expr.callee == "push" || expr.callee == "pop" || expr.callee == "push_str") {
            return check_container_op(expr);
        }
        if (expr.callee == "slice") {
            return check_slice(expr);
        }
        if (expr.callee == "find" || expr.callee == "contains") {
            return check_search(expr);
        }
        if (expr.callee == "capacity" || expr.callee == "reserve") {
            return check_capacity_op(expr);
        }

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
            if (!is_error(argument) && argument->kind != TypeKind::Array &&
                argument->kind != TypeKind::Vec && !is_text(argument)) {
                report("cannot take the length of `" + to_string(argument) + "`",
                       expr.args[0]->span,
                       "`len` needs an array, a `Vec`, a `string` or a `String`");
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
                                           "`bool`, `string` and `String`");
            diagnostic.with_note("printing structs and arrays arrives with v2");
        }
        return record(expr, types().void_type());
    }

    /// `new_vec()` and `new_string()` have no arguments to infer from,
    /// and there is no turbofish, so they take their type from where the
    /// result is going.
    TypePtr check_constructor(const ast::CallExpr& expr, TypePtr hint) {
        for (const ast::ExprPtr& arg : expr.args) {
            check_expr(*arg);
        }
        if (!expr.args.empty()) {
            report("this function takes 0 arguments but " +
                       std::to_string(expr.args.size()) + " " +
                       (expr.args.size() == 1 ? "was" : "were") + " supplied",
                   expr.span, "expected 0, found " + std::to_string(expr.args.size()));
        }

        if (expr.callee == "new_string") {
            return record(expr, types().string_buf_type());
        }

        if (hint != nullptr && hint->kind == TypeKind::Vec) {
            return record(expr, hint);
        }
        if (hint != nullptr && is_error(hint)) {
            // The annotation was written but already rejected - one
            // error for it is enough. A *missing* hint is a different
            // situation and still needs reporting below, which is why
            // this tests for null separately: is_error(nullptr) is true.
            return record(expr, types().error_type());
        }
        report("cannot infer the element type of this `Vec`", expr.span,
               "nothing here says what it will hold")
            .with_note("annotate the binding, as in `let v: Vec<int> = new_vec();`");
        return record(expr, types().error_type());
    }

    /// `push(v, x)`, `pop(v)` and `push_str(s, text)`.
    ///
    /// Each mutates its container through a reference, so the container
    /// is borrowed rather than moved - otherwise a push would consume
    /// the very thing it is pushing onto.
    TypePtr check_container_op(const ast::CallExpr& expr) {
        for (const ast::ExprPtr& arg : expr.args) {
            check_expr(*arg);
        }

        const std::size_t expected = expr.callee == "pop" ? 1 : 2;
        if (expr.args.size() != expected) {
            report("this function takes " + std::to_string(expected) + " argument" +
                       (expected == 1 ? "" : "s") + " but " +
                       std::to_string(expr.args.size()) + " " +
                       (expr.args.size() == 1 ? "was" : "were") + " supplied",
                   expr.span,
                   "expected " + std::to_string(expected) + ", found " +
                       std::to_string(expr.args.size()));
            return record(expr, types().error_type());
        }

        const TypePtr container = strip_reference(recorded_type(*expr.args[0]));
        if (is_error(container)) {
            return record(expr, types().error_type());
        }

        if (expr.callee == "push_str") {
            if (container->kind != TypeKind::StringBuf) {
                report("cannot push text onto `" + to_string(container) + "`",
                       expr.args[0]->span, "`push_str` needs a `String`");
                return record(expr, types().error_type());
            }
            check_container_is_mutable(*expr.args[0]);
            const TypePtr text = recorded_type(*expr.args[1]);
            if (!is_error(text) && text->kind != TypeKind::String &&
                text->kind != TypeKind::StringBuf) {
                report_mismatch(expr.args[1]->span, types().string_type(), text);
            }
            return record(expr, types().void_type());
        }

        if (container->kind != TypeKind::Vec) {
            report("cannot " + expr.callee + " on `" + to_string(container) + "`",
                   expr.args[0]->span, "`" + expr.callee + "` needs a `Vec`")
                .with_note("a `[T; N]` has a fixed length; use a `Vec<T>` to grow one");
            return record(expr, types().error_type());
        }
        check_container_is_mutable(*expr.args[0]);

        if (expr.callee == "pop") {
            return record(expr, container->element);
        }

        const TypePtr value = recorded_type(*expr.args[1]);
        if (!assignable(container->element, value)) {
            report_mismatch(expr.args[1]->span, container->element, value);
        }
        // The pushed value is stored in the vector, so ownership passes.
        move_out_of(*expr.args[1]);
        return record(expr, types().void_type());
    }

    /// `slice(text, start, end)`: the bytes from `start` up to but not
    /// including `end`, as a borrowed view.
    ///
    /// A `string` rather than a `String`, so it costs nothing - and
    /// dangles if what it points into is dropped or grown, exactly as
    /// `&T` does. That is the same bargain the language already made,
    /// and copying instead would make every substring an allocation.
    TypePtr check_slice(const ast::CallExpr& expr) {
        for (const ast::ExprPtr& arg : expr.args) {
            check_expr(*arg);
        }
        if (!check_arity(expr, 3)) {
            return record(expr, types().string_type());
        }

        const TypePtr text = strip_reference(recorded_type(*expr.args[0]));
        if (!is_error(text) && !is_text(text)) {
            report("cannot slice `" + to_string(text) + "`", expr.args[0]->span,
                   "`slice` needs a `string` or a `String`")
                .with_note("a `Vec` has no slicing yet; index it instead");
        }
        for (std::size_t i = 1; i < 3 && i < expr.args.size(); ++i) {
            const TypePtr bound = recorded_type(*expr.args[i]);
            if (!is_error(bound) && bound->kind != TypeKind::Int) {
                report_mismatch(expr.args[i]->span, types().int_type(), bound);
            }
        }
        return record(expr, types().string_type());
    }

    /// `find(haystack, needle)` and `contains(haystack, needle)`.
    TypePtr check_search(const ast::CallExpr& expr) {
        for (const ast::ExprPtr& arg : expr.args) {
            check_expr(*arg);
        }
        const TypePtr result = expr.callee == "find" ? types().int_type()
                                                     : types().bool_type();
        if (!check_arity(expr, 2)) {
            return record(expr, result);
        }

        for (const ast::ExprPtr& arg : expr.args) {
            const TypePtr text = strip_reference(recorded_type(*arg));
            if (!is_error(text) && !is_text(text)) {
                report("cannot search `" + to_string(text) + "`", arg->span,
                       "`" + expr.callee + "` needs a `string` or a `String`");
            }
        }
        return record(expr, result);
    }

    /// `capacity(c)` and `reserve(c, n)`: what a container has room for,
    /// and asking it for more.
    TypePtr check_capacity_op(const ast::CallExpr& expr) {
        for (const ast::ExprPtr& arg : expr.args) {
            check_expr(*arg);
        }
        const bool reserving = expr.callee == "reserve";
        const TypePtr result = reserving ? types().void_type() : types().int_type();
        if (!check_arity(expr, reserving ? 2 : 1)) {
            return record(expr, result);
        }

        const TypePtr container = strip_reference(recorded_type(*expr.args[0]));
        if (!is_error(container) && container->kind != TypeKind::Vec &&
            container->kind != TypeKind::StringBuf) {
            report("cannot ask `" + to_string(container) + "` about capacity",
                   expr.args[0]->span, "`" + expr.callee + "` needs a `Vec` or a `String`")
                .with_note("a `[T; N]` and a `string` are fixed; only a growable container "
                           "has capacity to speak of");
            return record(expr, result);
        }

        if (reserving) {
            check_container_is_mutable(*expr.args[0]);
            const TypePtr wanted = recorded_type(*expr.args[1]);
            if (!is_error(wanted) && wanted->kind != TypeKind::Int) {
                report_mismatch(expr.args[1]->span, types().int_type(), wanted);
            }
        }
        return record(expr, result);
    }

    /// Reports a call with the wrong number of arguments. Returns false
    /// when it did.
    bool check_arity(const ast::CallExpr& expr, std::size_t expected) {
        if (expr.args.size() == expected) {
            return true;
        }
        report("this function takes " + std::to_string(expected) + " argument" +
                   (expected == 1 ? "" : "s") + " but " + std::to_string(expr.args.size()) +
                   " " + (expr.args.size() == 1 ? "was" : "were") + " supplied",
               expr.span,
               "expected " + std::to_string(expected) + ", found " +
                   std::to_string(expr.args.size()));
        return false;
    }

    /// A container being pushed to has to be a mutable binding, the same
    /// rule assignment already applies.
    void check_container_is_mutable(const ast::Expr& expr) {
        const auto* name = ast::node_cast<ast::NameExpr>(&expr);
        if (name == nullptr || !name->module.empty()) {
            return;
        }
        const Binding* binding = scopes_.lookup(name->name);
        if (binding == nullptr || binding->is_mutable) {
            return;
        }
        // A `&Vec<T>` parameter is a borrow of someone else's mutable
        // container, so writing through it is allowed - the same rule as
        // assigning through a reference.
        if (binding->type != nullptr && binding->type->kind == TypeKind::Reference) {
            return;
        }
        if (binding->is_parameter) {
            report("cannot modify parameter `" + name->name + "`", expr.span,
                   "parameters are immutable in v1")
                .with_note("take a `&" + to_string(binding->type) + "` to modify the "
                           "caller's container");
            return;
        }
        report("cannot modify immutable binding `" + name->name + "`", expr.span,
               "`" + name->name + "` is not declared `mut`")
            .with_note("declare it as `let mut " + name->name + "` to allow this");
    }

    static bool is_printable(TypePtr type) {
        switch (strip_reference(type)->kind) {
            case TypeKind::Int:
            case TypeKind::Float:
            case TypeKind::Bool:
            case TypeKind::String:
            case TypeKind::StringBuf:
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
        if (field != nullptr && info->second.module != current_module_ && !field->is_public) {
            report("field `" + expr.field + "` of `" + to_string(base) + "` is private",
                   expr.field_span, "`" + expr.field + "` is not declared `pub`")
                .with_note("declared at " + location_of(field->span));
            return record(expr, types().error_type());
        }
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
        if (base->kind != TypeKind::Array && base->kind != TypeKind::Vec) {
            report("cannot index into `" + to_string(object) + "`", expr.span,
                   "only arrays and `Vec`s can be indexed");
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
        // `bool as int` is 0 or 1, and `int as bool` is "not zero" -
        // which is what C means by both, and §9 says follow C on the
        // low-level questions. `float` has no such convention, so it is
        // left out rather than guessed at.
        if ((source->kind == TypeKind::Bool && target->kind == TypeKind::Int) ||
            (source->kind == TypeKind::Int && target->kind == TypeKind::Bool)) {
            return record(expr, target);
        }

        Diagnostic& diagnostic =
            report("cannot cast `" + to_string(source) + "` to `" + to_string(target) + "`",
                   expr.span, "no conversion exists between these types");
        diagnostic.with_note("`as` converts between `int` and `float`, and between `int` and "
                             "`bool`");
        if (source->kind == TypeKind::Bool || target->kind == TypeKind::Bool) {
            diagnostic.with_note("a `bool` becomes 0 or 1, and an `int` becomes whether it is "
                                 "not zero; there is no such convention for `float`");
        }
        return record(expr, target);
    }

    /// `|a: int| -> int { ... }`
    ///
    /// The body is checked in its own scope with the parameters bound.
    /// Anything it mentions from further out becomes a capture, which is
    /// what turns a plain function into a closure.
    TypePtr check_closure(const ast::ClosureExpr& expr, TypePtr hint = nullptr) {
        // The node is mutated to record what was captured, which is only
        // knowable once the body has been resolved.
        auto& closure = const_cast<ast::ClosureExpr&>(expr);
        closure.id = next_closure_id_++;

        // What the closure is being handed to, when that is a function
        // type. A `&fn(int) -> int` parameter says as much about the
        // closure as writing the types out would.
        const TypePtr expected =
            hint != nullptr && strip_reference(hint)->kind == TypeKind::Function
                ? strip_reference(hint)
                : nullptr;

        std::vector<TypePtr> params;
        for (std::size_t i = 0; i < closure.params.size(); ++i) {
            const ast::Param& param = closure.params[i];
            if (param.type) {
                params.push_back(resolve_type(*param.type));
                continue;
            }
            if (expected != nullptr && i < expected->args.size()) {
                params.push_back(expected->args[i]);
                continue;
            }
            report("cannot work out the type of `" + param.name + "`", param.span,
                   "nothing here says what it is")
                .with_note("a closure's parameter types come from what it is passed to; "
                           "write `" + param.name + ": <type>` when there is nothing to "
                           "take them from");
            params.push_back(types().error_type());
        }

        // The return type follows the same rule, and falls back to
        // nothing rather than to an error: a closure that returns
        // nothing is ordinary.
        TypePtr result = types().void_type();
        if (closure.return_type) {
            result = resolve_type(*closure.return_type);
        } else if (expected != nullptr && expected->result != nullptr) {
            result = expected->result;
        }

        scopes_.push();
        closures_.push_back(ClosureContext{scopes_.size() - 1, &closure, {}});

        for (std::size_t i = 0; i < closure.params.size(); ++i) {
            const ast::Param& param = closure.params[i];
            const Binding* existing = scopes_.declare(
                param.name, Binding{params[i], false, true, param.span, false, {}});
            if (existing != nullptr) {
                report("duplicate definition of parameter `" + param.name + "`",
                       param.name_span, "`" + param.name + "` is already a parameter here");
            }
        }

        // `return` inside the body answers to the closure, not to the
        // function containing it.
        const FunctionInfo* enclosing = current_function_;
        FunctionInfo signature;
        signature.name = "closure";
        signature.return_type = result;
        current_function_ = &signature;

        // And so do its effects. Defining a closure is not calling it,
        // so a `println` in the body belongs to the closure rather than
        // to the function that wrote it down - which is what makes
        // `uses nothing` on a factory that returns an io closure
        // truthful rather than absurd.
        const std::string effect_key = "closure#" + std::to_string(next_closure_effects_++);
        EffectFacts* outer_effects = current_effects_;
        current_effects_ = &effects_[effect_key];

        check_block(closure.body);

        if (result != nullptr && result->kind != TypeKind::Void && !is_error(result) &&
            !always_returns(closure.body)) {
            report("missing return", expr.span,
                   "this closure must return `" + to_string(result) + "` on every path");
        }

        current_effects_ = outer_effects;

        // If it is going somewhere with a bound, it has to fit.
        if (expected != nullptr && expected->effects_bounded) {
            closure_bounds_.push_back(ClosureBound{effect_key, expected->effects, expr.span});
        }

        current_function_ = enclosing;
        closures_.pop_back();
        scopes_.pop();

        // A capture that owns memory moves into the closure, which
        // owns it from then on. Codegen gives such a closure a drop
        // function of its own, since what is in an environment cannot be
        // worked out from the closure's type - two closures of the same
        // `fn(int) -> int` may capture quite different things.
        std::vector<TypePtr> capture_types;
        for (const ast::Capture& capture : closure.captures) {
            Binding* binding = scopes_.lookup_mutable(capture.name);
            const TypePtr captured = binding != nullptr ? binding->type : types().error_type();
            capture_types.push_back(captured);

            // Taking it by value means taking it: the enclosing scope
            // gives it up, exactly as if it had been passed by value.
            if (binding != nullptr && is_owned(captured)) {
                binding->moved = true;
                binding->moved_at = capture.span;
            }
        }

        // The closure takes the bound it is being handed to, the same
        // way it takes its parameter types from there. What it actually
        // performs is verified separately, after the fixpoint - a
        // closure cannot be given its own inferred bound here, because
        // what it calls may not have been walked yet.
        //
        // With no expectation to take one from it stays unbounded,
        // which is what every closure written before §10.3 was.
        const TypePtr type =
            expected != nullptr && expected->effects_bounded
                ? types().function_of(params, result, true, expected->effects)
                : types().function_of(params, result);
        result_.closure_captures[{current_instance_, &expr}] = std::move(capture_types);
        return record(expr, type);
    }

    TypePtr check_struct_literal(const ast::StructLitExpr& expr) {
        const std::optional<std::string> qualified =
            lookup_name(expr.module, expr.type_name, expr.type_name_span);
        if (!qualified.has_value()) {
            for (const ast::FieldInit& field : expr.fields) {
                check_expr(*field.value);
            }
            return record(expr, types().error_type());
        }

        const auto tmpl = result_.struct_templates.find(*qualified);
        if (tmpl != result_.struct_templates.end()) {
            if (!check_visible(tmpl->second.module, tmpl->second.is_public,
                               path_string(expr.module, expr.type_name), expr.type_name_span,
                               "type", tmpl->second.span)) {
                for (const ast::FieldInit& field : expr.fields) {
                    check_expr(*field.value);
                }
                return record(expr, types().error_type());
            }
            return check_generic_struct_literal(expr, tmpl->second);
        }

        const auto info = result_.structs.find(*qualified);
        if (info == result_.structs.end()) {
            Diagnostic& diagnostic =
                report("cannot find type `" + path_string(expr.module, expr.type_name) + "`",
                       expr.type_name_span, "not found in this scope");
            suggest(diagnostic, expr.type_name, struct_names());
            for (const ast::FieldInit& field : expr.fields) {
                check_expr(*field.value);
            }
            return record(expr, types().error_type());
        }

        if (!check_visible(info->second.module, info->second.is_public,
                           path_string(expr.module, expr.type_name), expr.type_name_span,
                           "type", info->second.span)) {
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

            // Reading a private field is rejected in check_field_access;
            // writing one in a literal has to be rejected here, or a
            // module could construct another module's invariants away.
            if (declared.module != current_module_ && !target->is_public) {
                report("field `" + field.name + "` of `" + declared.name + "` is private",
                       field.name_span, "`" + field.name + "` is not declared `pub`")
                    .with_note("declared at " + location_of(target->span));
                continue;
            }

            if (!assignable(target->type, value)) {
                report_mismatch(field.value->span, target->type, value);
            }
            move_out_of(*field.value);
        }

        std::vector<std::string> missing;
        for (const FieldInfo& field : declared.fields) {
            if (!initialized[field.index]) {
                missing.push_back(field.name);
            }
        }
        if (!missing.empty() && declared.module != current_module_) {
            // Every field must be given a value, but a private one
            // cannot be - so the type simply is not constructible here,
            // and saying that is more useful than listing the fields.
            report("`" + declared.name + "` cannot be constructed from outside its module",
                   expr.span, "it has fields that are not `pub`")
                .with_note("add a `pub fn` to `" + declared.module +
                           "` that builds one instead");
            return;
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

    TypePtr check_array_literal(const ast::ArrayLitExpr& expr, TypePtr hint = nullptr) {
        if (expr.elements.empty() && hint != nullptr && hint->kind == TypeKind::Array &&
            hint->length == 0) {
            // `let xs: [int; 0] = [];` now works: the annotation says
            // what the element type is, which bottom-up inference alone
            // could never determine.
            return record(expr, hint);
        }
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
        move_out_of(*expr.elements.front());
        for (std::size_t i = 1; i < expr.elements.size(); ++i) {
            const TypePtr other = check_expr(*expr.elements[i]);
            if (!assignable(element, other)) {
                report_mismatch(expr.elements[i]->span, element, other);
            }
            move_out_of(*expr.elements[i]);
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
        // A `String` goes where a `string` is wanted: the view is the
        // buffer's own bytes, and handing one over lends rather than
        // gives. It carries the warning every borrow here carries - the
        // view dangles if the buffer is dropped or grown - which is the
        // same bargain `&T` already makes.
        //
        // Not the other way round: a `string` borrows bytes it does not
        // own, and nothing can turn that into ownership without copying.
        if (expected->kind == TypeKind::String &&
            strip_reference(found)->kind == TypeKind::StringBuf) {
            return true;
        }
        // A function that does less goes where one that may do more is
        // wanted. Everything but the bound has to match exactly - this
        // is width subtyping on the effect set only, not on parameters
        // or results, which stay invariant as they were.
        if (expected->kind == TypeKind::Function && found->kind == TypeKind::Function &&
            expected->args == found->args && expected->result == found->result) {
            return effects_within(expected->effects_bounded, expected->effects,
                                  found->effects_bounded ? found->effects : every_effect());
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

bool CheckResult::is_move(InstanceId instance, const ast::Expr& expr) const {
    return moved_expressions.count({instance, &expr}) != 0;
}

std::string_view stage_name() noexcept { return "type checker"; }

bool is_intrinsic(std::string_view name) noexcept {
    return std::find(kIntrinsics.begin(), kIntrinsics.end(), name) != kIntrinsics.end();
}

CheckResult check(const std::vector<ModuleInput>& modules, const ast::SourceMap& sources) {
    return Checker{sources}.run(modules);
}

CheckResult check(const ast::Program& program, const ast::SourceFile& source) {
    // A single-module program is the one-element case of the general
    // one. The map is built here so the single-file callers - the tests,
    // and anything that has only ever seen one file - keep working.
    ast::SourceMap sources;
    sources.add(source.path(), source.contents());

    const std::vector<ModuleInput> modules{ModuleInput{{}, &program, {}}};
    return Checker{sources}.run(modules);
}

}  // namespace ember::typeck
