#include "ember/codegen/codegen.hpp"

#ifndef EMBER_HAVE_LLVM
#define EMBER_HAVE_LLVM 0
#endif

#include <map>
#include <optional>
#include <string>
#include <vector>

#if EMBER_HAVE_LLVM
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#endif

namespace ember::codegen {

std::string_view stage_name() noexcept { return "codegen"; }

bool is_available() noexcept { return EMBER_HAVE_LLVM != 0; }

std::string_view llvm_version() noexcept {
#if EMBER_HAVE_LLVM
    return LLVM_VERSION_STRING;
#else
    return "none";
#endif
}

std::vector<ast::Diagnostic> verify_entry_point(const typeck::CheckResult& checked) {
    std::vector<ast::Diagnostic> diagnostics;

    const auto main = checked.functions.find("main");
    if (main == checked.functions.end()) {
        diagnostics.push_back(ast::Diagnostic::error(
            "no `main` function found", ast::Span::at(0),
            "an executable needs an entry point declared as `pub fn main()`"));
        return diagnostics;
    }

    if (!main->second.param_types.empty()) {
        diagnostics.push_back(ast::Diagnostic::error("`main` cannot take arguments",
                                                     main->second.span,
                                                     "expected `pub fn main()`"));
    }
    if (main->second.return_type != nullptr &&
        main->second.return_type->kind != typeck::TypeKind::Void) {
        diagnostics.push_back(ast::Diagnostic::error(
            "`main` cannot return a value", main->second.span,
            "expected `pub fn main()`, found a return type of `" +
                typeck::to_string(main->second.return_type) + "`"));
    }
    return diagnostics;
}

#if EMBER_HAVE_LLVM
namespace {

using typeck::TypeKind;
using typeck::TypePtr;

/// Where a named value lives: the address of its stack slot (or of its
/// global), plus the Ember type stored there.
struct Slot {
    llvm::Value* address = nullptr;
    TypePtr type = nullptr;
    /// For an owned type: an i1 that is true while this slot still holds
    /// a value it must free. Null for types that need no cleanup.
    llvm::Value* drop_flag = nullptr;
};

class Emitter {
public:
    Emitter(const std::vector<ModuleInput>& modules, const typeck::CheckResult& checked,
            const CompileOptions& options, const llvm::DataLayout* layout)
        : modules_(modules),
          checked_(checked),
          target_(options.target_module),
          context_(std::make_unique<llvm::LLVMContext>()),
          module_(std::make_unique<llvm::Module>(options.module_name, *context_)),
          builder_(*context_) {
        // { i8*, i64 }: §4's "immutable, fixed-length view". Carrying the
        // length keeps len() O(1) and lets a string hold a NUL byte.
        string_type_ = llvm::StructType::create(*context_, {ptr_type(), int_type()},
                                                "ember.string");
        // A growable container is { buffer, length, capacity }. One
        // layout serves every `Vec<T>` and `String`, because the element
        // size is known at each use site rather than carried at runtime.
        buffer_type_ = llvm::StructType::create(
            *context_, {ptr_type(), int_type(), int_type()}, "ember.buffer");
        // A closure is a pair: the lifted function, and the heap block
        // holding what it captured. A closure that captured nothing has
        // a null environment, so the pair is uniform either way.
        closure_type_ =
            llvm::StructType::create(*context_, {ptr_type(), ptr_type()}, "ember.closure");

        // Set before anything is emitted: a `Vec<T>` push needs the size
        // of T, and asking a module with no layout gives nonsense.
        if (layout != nullptr) {
            module_->setDataLayout(*layout);
        }
    }

    bool run() {
        declare_structs();
        declare_functions();
        emit_constants();
        emit_function_bodies();
        strip_unused_declarations();

        if (!diagnostics_.empty()) {
            return false;
        }

        std::string error;
        llvm::raw_string_ostream stream(error);
        if (llvm::verifyModule(*module_, &stream)) {
            // A verifier failure is a bug in this file, not in the user's
            // program, so it says so rather than blaming the source.
            report(ast::Span::at(0), "internal error: generated invalid LLVM IR",
                   "this is a bug in the Ember compiler")
                .with_note(error);
            return false;
        }
        return true;
    }

    llvm::Module& module() { return *module_; }
    std::vector<ast::Diagnostic>& diagnostics() { return diagnostics_; }

private:
    const std::vector<ModuleInput>& modules_;
    const typeck::CheckResult& checked_;
    /// The one module whose bodies belong in this object file, or unset
    /// for a whole-program build.
    std::optional<std::string> target_;
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
    std::vector<ast::Diagnostic> diagnostics_;

    llvm::StructType* string_type_ = nullptr;
    llvm::StructType* buffer_type_ = nullptr;
    llvm::StructType* closure_type_ = nullptr;
    std::map<std::string, llvm::StructType*> struct_types_;
    std::map<std::string, llvm::Function*> functions_;
    std::map<std::string, llvm::GlobalVariable*> constants_;

    std::vector<std::map<std::string, Slot>> scopes_;
    llvm::Function* current_function_ = nullptr;
    const typeck::FunctionInfo* current_info_ = nullptr;
    bool current_is_entry_point_ = false;
    /// Which monomorphized copy is being emitted. The checker recorded a
    /// separate type for every expression per instance, so every lookup
    /// has to say which one it means.
    typeck::InstanceId current_instance_ = typeck::kRootInstance;
    /// The module whose items are being emitted, so a name can be
    /// qualified the same way the checker qualified it.
    std::string current_module_;

    std::string qualify(const std::string& name) const {
        return current_module_.empty() ? name : current_module_ + "::" + name;
    }

    /// Whether definitions from `module` belong in this object file.
    /// Always true for a whole-program build.
    bool is_target(const std::string& module) const {
        return !target_.has_value() || *target_ == module;
    }

    typeck::TypePtr type_of(const ast::Expr& expr) const {
        return checked_.type_of(current_instance_, expr);
    }

    ast::Diagnostic& report(ast::Span span, std::string message, std::string label) {
        diagnostics_.push_back(
            ast::Diagnostic::error(std::move(message), span, std::move(label)));
        return diagnostics_.back();
    }

    // -----------------------------------------------------------------
    // Types
    // -----------------------------------------------------------------

    llvm::IntegerType* int_type() { return llvm::Type::getInt64Ty(*context_); }
    llvm::Type* float_type() { return llvm::Type::getDoubleTy(*context_); }
    llvm::IntegerType* bool_type() { return llvm::Type::getInt1Ty(*context_); }
    llvm::IntegerType* byte_type() { return llvm::Type::getInt8Ty(*context_); }
    llvm::PointerType* ptr_type() { return llvm::PointerType::get(*context_, 0); }

    llvm::Type* lower(TypePtr type) {
        switch (type->kind) {
            case TypeKind::Int:
                return int_type();
            case TypeKind::Float:
                return float_type();
            case TypeKind::Bool:
                return bool_type();
            case TypeKind::String:
                return string_type_;
            case TypeKind::Struct:
                // Keyed by the spelled-out name, so `Pair<int>` and
                // `Pair<float>` are two distinct LLVM struct types.
                return struct_types_.at(typeck::to_string(type));
            case TypeKind::Reference:
                // Opaque pointers: a `&T` is just `ptr`, and the pointee
                // type comes from the Ember type, not the LLVM one.
                return ptr_type();
            case TypeKind::Array:
                return llvm::ArrayType::get(lower(type->element),
                                            static_cast<std::uint64_t>(type->length));
            case TypeKind::Vec:
            case TypeKind::StringBuf:
                return buffer_type_;
            case TypeKind::Function:
                return closure_type_;
            case TypeKind::Generic:
                // Unreachable: only instantiated bodies are emitted, and
                // every type in one has been substituted.
                report(ast::Span::at(0), "internal error: unsubstituted type parameter",
                       "this is a bug in the Ember compiler");
                return llvm::Type::getVoidTy(*context_);
            case TypeKind::Void:
            case TypeKind::Error:
                return llvm::Type::getVoidTy(*context_);
        }
        return llvm::Type::getVoidTy(*context_);
    }

    /// Creates every struct type before any body is set, so fields can
    /// refer to structs declared later in the file.
    void declare_structs() {
        for (const auto& [name, info] : checked_.structs) {
            struct_types_[name] = llvm::StructType::create(*context_, "struct." + name);
        }
        for (const auto& [name, info] : checked_.structs) {
            std::vector<llvm::Type*> fields;
            fields.reserve(info.fields.size());
            for (const typeck::FieldInfo& field : info.fields) {
                fields.push_back(lower(field.type));
            }
            struct_types_[name]->setBody(fields);
        }
    }

    // -----------------------------------------------------------------
    // Functions
    // -----------------------------------------------------------------

    /// `main` is emitted as the C entry point `i32 main()`, so the system
    /// linker finds it and the process exits 0.
    static bool is_entry_point(const typeck::FunctionInfo& info) {
        return !info.is_method() && info.module.empty() && info.name == "main";
    }

    llvm::FunctionType* signature_of(const typeck::FunctionInfo& info) {
        std::vector<llvm::Type*> params;
        params.reserve(info.param_types.size());
        for (const TypePtr param : info.param_types) {
            params.push_back(lower(param));
        }
        if (is_entry_point(info)) {
            return llvm::FunctionType::get(llvm::Type::getInt32Ty(*context_), params, false);
        }
        return llvm::FunctionType::get(lower(info.return_type), params, false);
    }

    void declare_functions() {
        for (const auto& [name, info] : checked_.functions) {
            declare_function(info);
        }
        for (const auto& [key, info] : checked_.methods) {
            declare_function(info);
        }
        // One function per instantiation, each with its own mangled
        // name: this is monomorphization made concrete.
        for (const typeck::Instantiation& instance : checked_.instantiations) {
            declare_function(instance.info);
        }
    }

    void declare_function(const typeck::FunctionInfo& info) {
        llvm::Function* function =
            llvm::Function::Create(signature_of(info), llvm::Function::ExternalLinkage,
                                   info.mangled_name, module_.get());

        for (std::size_t i = 0; i < info.param_names.size(); ++i) {
            function->getArg(static_cast<unsigned>(i))->setName(info.param_names[i]);
        }
        functions_[info.mangled_name] = function;
    }

    void emit_function_bodies() {
        for (const ModuleInput& module : modules_) {
            if (!is_target(module.name)) {
                continue;  // a declaration is all this object needs
            }
            current_module_ = module.name;
            emit_concrete_bodies(*module.program);
        }

        for (const typeck::Instantiation& instance : checked_.instantiations) {
            if (!needs_instance(instance)) {
                continue;
            }
            // Emitted wherever it is used rather than where it was
            // written, so `linkonce_odr`: several objects may carry the
            // same copy, and the linker is entitled to keep any one.
            functions_.at(instance.info.mangled_name)
                ->setLinkage(llvm::Function::LinkOnceODRLinkage);
            current_instance_ = instance.id;
            current_module_ = instance.info.module;
            emit_function(*instance.decl, instance.info);
        }
        current_instance_ = typeck::kRootInstance;
        current_module_.clear();
    }

    /// Removes the `declare` lines nothing ended up referring to.
    ///
    /// Every function in the program is declared before any body is
    /// emitted, so a call can be lowered before its definition is
    /// reached. In a whole-program build they all get bodies and this
    /// finds nothing. In a per-module one most of them are another
    /// object's business, and leaving them behind would write the whole
    /// program's symbol table into every object file - which is the
    /// coupling separate compilation exists to remove.
    void strip_unused_declarations() {
        std::vector<llvm::Function*> dead;
        for (llvm::Function& function : *module_) {
            if (function.isDeclaration() && function.use_empty()) {
                dead.push_back(&function);
            }
        }
        for (llvm::Function* function : dead) {
            functions_.erase(function->getName().str());
            function->eraseFromParent();
        }
    }

    /// Whether this object file has to carry a copy of `instance`.
    bool needs_instance(const typeck::Instantiation& instance) const {
        return !target_.has_value() || instance.demanded_by.count(*target_) != 0;
    }

    void emit_concrete_bodies(const ast::Program& program) {
        for (const ast::ItemPtr& item : program.items) {
            if (const auto* declaration = ast::node_cast<ast::FunctionDecl>(item.get())) {
                const auto entry = checked_.functions.find(qualify(declaration->name));
                if (entry != checked_.functions.end() && entry->second.decl == declaration) {
                    emit_function(*declaration, entry->second);
                }
            } else if (const auto* block = ast::node_cast<ast::ImplBlock>(item.get())) {
                for (const std::unique_ptr<ast::FunctionDecl>& method : block->methods) {
                    const auto entry = checked_.methods.find(
                        std::make_pair(qualify(block->type_name), method->name));
                    if (entry != checked_.methods.end() && entry->second.decl == method.get()) {
                        emit_function(*method, entry->second);
                    }
                }
            }
        }
    }

    void emit_function(const ast::FunctionDecl& declaration, const typeck::FunctionInfo& info) {
        llvm::Function* function = functions_.at(info.mangled_name);
        current_function_ = function;
        current_info_ = &info;
        current_is_entry_point_ = is_entry_point(info);

        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", function);
        builder_.SetInsertPoint(entry);

        scopes_.clear();
        push_scope();

        // Parameters get stack slots like any other local. That is what
        // clang -O0 does; mem2reg promotes the ones that can live in
        // registers, so there is nothing to gain by being clever here.
        for (std::size_t i = 0; i < info.param_names.size(); ++i) {
            const TypePtr type = info.param_types[i];
            llvm::Value* slot = create_entry_alloca(lower(type), info.param_names[i]);
            builder_.CreateStore(function->getArg(static_cast<unsigned>(i)), slot);
            scopes_.back()[info.param_names[i]] = Slot{slot, type};
        }

        emit_block(declaration.body);

        // Fall off the end: the checker has already proved this is only
        // reachable for functions that return nothing.
        if (!builder_.GetInsertBlock()->getTerminator()) {
            emit_all_drops();
            emit_default_return();
        }

        // The parameter scope was already covered by emit_all_drops.
        scopes_.pop_back();
        current_function_ = nullptr;
        current_info_ = nullptr;
    }

    void emit_default_return() {
        if (current_is_entry_point_) {
            builder_.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0));
        } else {
            builder_.CreateRetVoid();
        }
    }

    /// Allocas belong in the entry block: putting them anywhere else
    /// defeats mem2reg and can allocate inside a loop.
    llvm::AllocaInst* create_entry_alloca(llvm::Type* type, const std::string& name) {
        llvm::BasicBlock& entry = current_function_->getEntryBlock();
        llvm::IRBuilder<> entry_builder(&entry, entry.begin());
        return entry_builder.CreateAlloca(type, nullptr, name);
    }

    // -----------------------------------------------------------------
    // Scopes and drops
    //
    // Every owned local gets a drop flag beside it: an i1 set when the
    // value is stored and cleared when it moves away. At the end of the
    // scope each live local is freed, newest first.
    //
    // A flag rather than a static decision, because a value can be moved
    // on one path and not another - `if c { consume(v); }` - and only the
    // running program knows which happened. The flag is a stack slot the
    // optimizer folds away wherever the answer is obvious.
    // -----------------------------------------------------------------

    void push_scope() { scopes_.emplace_back(); }

    /// Drops everything the scope still owns, then discards it.
    void pop_scope() {
        emit_scope_drops(scopes_.back());
        scopes_.pop_back();
    }

    void emit_scope_drops(const std::map<std::string, Slot>& scope) {
        if (block_terminated()) {
            return;  // a return already dropped everything on its way out
        }
        // Reverse declaration order, so a value is dropped before
        // anything it was built from.
        for (auto slot = scope.rbegin(); slot != scope.rend(); ++slot) {
            emit_conditional_drop(slot->second);
        }
    }

    /// `if (flag) drop(value)`, for one slot.
    void emit_conditional_drop(const Slot& slot) {
        if (slot.drop_flag == nullptr || !typeck::is_owned(slot.type)) {
            return;
        }

        llvm::BasicBlock* drop_block = llvm::BasicBlock::Create(*context_, "drop");
        llvm::BasicBlock* after = llvm::BasicBlock::Create(*context_, "drop.end");

        llvm::Value* live = builder_.CreateLoad(bool_type(), slot.drop_flag, "live");
        builder_.CreateCondBr(live, drop_block, after);

        drop_block->insertInto(current_function_);
        builder_.SetInsertPoint(drop_block);
        emit_drop(slot.type, slot.address);
        builder_.CreateBr(after);

        after->insertInto(current_function_);
        builder_.SetInsertPoint(after);
    }

    /// Frees whatever `address` owns, recursing into aggregates.
    void emit_drop(TypePtr type, llvm::Value* address) {
        if (!typeck::is_owned(type)) {
            return;
        }

        switch (type->kind) {
            case TypeKind::Function: {
                // Only the environment block. Captures are required to
                // be copyable, so there is nothing inside it to drop.
                llvm::Value* env = builder_.CreateLoad(
                    ptr_type(), builder_.CreateStructGEP(closure_type_, address, 1), "env");
                builder_.CreateCall(runtime("ember_free", void_type(), {ptr_type()}), {env});
                builder_.CreateStore(llvm::ConstantPointerNull::get(ptr_type()),
                                     builder_.CreateStructGEP(closure_type_, address, 1));
                return;
            }

            case TypeKind::Vec:
            case TypeKind::StringBuf: {
                // The elements of a `Vec<T>` where T itself owns memory
                // would need a loop here; v2 only allows scalar and
                // borrowed element types, which the checker enforces.
                llvm::Value* buffer = builder_.CreateLoad(
                    ptr_type(), builder_.CreateStructGEP(buffer_type_, address, 0), "buf");
                builder_.CreateCall(runtime("ember_free", void_type(), {ptr_type()}), {buffer});
                // Null the pointer so a double drop cannot free twice,
                // whatever the flags say.
                builder_.CreateStore(llvm::ConstantPointerNull::get(ptr_type()),
                                     builder_.CreateStructGEP(buffer_type_, address, 0));
                return;
            }

            case TypeKind::Struct: {
                const typeck::StructInfo& info = checked_.structs.at(typeck::to_string(type));
                llvm::StructType* layout = struct_types_.at(typeck::to_string(type));
                for (const typeck::FieldInfo& field : info.fields) {
                    if (typeck::is_owned(field.type)) {
                        emit_drop(field.type,
                                  builder_.CreateStructGEP(
                                      layout, address, static_cast<unsigned>(field.index)));
                    }
                }
                return;
            }

            case TypeKind::Array: {
                llvm::Type* layout = lower(type);
                for (std::int64_t i = 0; i < type->length; ++i) {
                    emit_drop(type->element,
                              builder_.CreateInBoundsGEP(
                                  layout, address,
                                  {builder_.getInt64(0), builder_.getInt64(
                                                             static_cast<std::uint64_t>(i))}));
                }
                return;
            }

            default:
                return;
        }
    }

    /// Drops every live local in every open scope. Used on the way out
    /// of a function, where the scopes are not popped one at a time.
    void emit_all_drops() {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            for (auto slot = scope->rbegin(); slot != scope->rend(); ++slot) {
                emit_conditional_drop(slot->second);
            }
        }
    }

    /// A stack slot for an owned local, plus the flag that says whether
    /// it still holds something.
    llvm::Value* create_drop_flag(const std::string& name) {
        llvm::Value* flag = create_entry_alloca(bool_type(), name + ".live");
        // Cleared in the entry block: a value declared inside a loop is
        // dead again on each new iteration until it is stored.
        llvm::IRBuilder<> entry_builder(&current_function_->getEntryBlock(),
                                        std::next(current_function_->getEntryBlock().begin(),
                                                  0));
        builder_.CreateStore(builder_.getInt1(false), flag);
        return flag;
    }

    const Slot* lookup(const std::string& name) const {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            const auto found = scope->find(name);
            if (found != scope->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    // -----------------------------------------------------------------
    // Runtime
    // -----------------------------------------------------------------

    llvm::FunctionCallee runtime(const char* name, llvm::Type* result,
                                 llvm::ArrayRef<llvm::Type*> params) {
        return module_->getOrInsertFunction(name,
                                            llvm::FunctionType::get(result, params, false));
    }

    llvm::Type* void_type() { return llvm::Type::getVoidTy(*context_); }

    // -----------------------------------------------------------------
    // Statements
    // -----------------------------------------------------------------

    /// True once the current block ends in a branch or return, after
    /// which nothing more may be appended to it.
    bool block_terminated() const {
        llvm::BasicBlock* block = builder_.GetInsertBlock();
        return block == nullptr || block->getTerminator() != nullptr;
    }

    void emit_block(const ast::Block& block) {
        push_scope();
        for (const ast::StmtPtr& statement : block.statements) {
            if (block_terminated()) {
                break;  // unreachable code after a return
            }
            emit_stmt(*statement);
        }
        pop_scope();
    }

    void emit_stmt(const ast::Stmt& statement) {
        switch (statement.kind) {
            case ast::StmtKind::Let:
                return emit_let(static_cast<const ast::LetStmt&>(statement));
            case ast::StmtKind::Return:
                return emit_return(static_cast<const ast::ReturnStmt&>(statement));
            case ast::StmtKind::If:
                return emit_if(static_cast<const ast::IfStmt&>(statement));
            case ast::StmtKind::While:
                return emit_while(static_cast<const ast::WhileStmt&>(statement));
            case ast::StmtKind::Assign:
                return emit_assign(static_cast<const ast::AssignStmt&>(statement));
            case ast::StmtKind::Expr:
                emit_value(*static_cast<const ast::ExprStmt&>(statement).expr);
                return;
            case ast::StmtKind::Block:
                return emit_block(static_cast<const ast::BlockStmt&>(statement).block);
        }
    }

    void emit_let(const ast::LetStmt& statement) {
        const TypePtr type = binding_type(statement);
        llvm::Value* slot = create_entry_alloca(lower(type), statement.name);

        // The flag is created before the initializer runs, so that a
        // `let` inside a loop starts each iteration owning nothing.
        llvm::Value* flag =
            typeck::is_owned(type) ? create_drop_flag(statement.name) : nullptr;

        builder_.CreateStore(emit_as(*statement.value, type), slot);
        if (flag != nullptr) {
            builder_.CreateStore(builder_.getInt1(true), flag);
        }
        scopes_.back()[statement.name] = Slot{slot, type, flag};
    }

    /// The type the checker gave this binding. Re-deriving it here
    /// would mean re-resolving the written annotation, which cannot be
    /// done inside a generic body where it may name a type parameter.
    TypePtr binding_type(const ast::LetStmt& statement) {
        if (const TypePtr recorded = checked_.binding_type(current_instance_, statement)) {
            return recorded;
        }
        return type_of(*statement.value);
    }

    void emit_return(const ast::ReturnStmt& statement) {
        if (statement.value == nullptr) {
            emit_all_drops();
            emit_default_return();
            return;
        }
        llvm::Value* value = emit_as(*statement.value, current_info_->return_type);
        // After the value is in hand: the returned local has had its flag
        // cleared by the move above, so this frees everything else.
        emit_all_drops();
        if (current_is_entry_point_) {
            builder_.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0));
        } else {
            builder_.CreateRet(value);
        }
    }

    void emit_if(const ast::IfStmt& statement) {
        llvm::Value* condition = emit_value(*statement.condition);

        llvm::BasicBlock* then_block =
            llvm::BasicBlock::Create(*context_, "if.then", current_function_);
        llvm::BasicBlock* else_block =
            statement.else_branch ? llvm::BasicBlock::Create(*context_, "if.else") : nullptr;
        llvm::BasicBlock* merge_block = llvm::BasicBlock::Create(*context_, "if.end");

        builder_.CreateCondBr(condition, then_block, else_block ? else_block : merge_block);

        builder_.SetInsertPoint(then_block);
        emit_block(statement.then_block);
        if (!block_terminated()) {
            builder_.CreateBr(merge_block);
        }

        if (else_block != nullptr) {
            else_block->insertInto(current_function_);
            builder_.SetInsertPoint(else_block);
            emit_stmt(*statement.else_branch);
            if (!block_terminated()) {
                builder_.CreateBr(merge_block);
            }
        }

        // If both arms returned, the merge block has no predecessors and
        // would be dead; drop it rather than emit an unreachable block.
        if (merge_block->hasNPredecessorsOrMore(1)) {
            merge_block->insertInto(current_function_);
            builder_.SetInsertPoint(merge_block);
        } else {
            delete merge_block;
            builder_.ClearInsertionPoint();
        }
    }

    void emit_while(const ast::WhileStmt& statement) {
        llvm::BasicBlock* condition_block =
            llvm::BasicBlock::Create(*context_, "while.cond", current_function_);
        llvm::BasicBlock* body_block = llvm::BasicBlock::Create(*context_, "while.body");
        llvm::BasicBlock* end_block = llvm::BasicBlock::Create(*context_, "while.end");

        builder_.CreateBr(condition_block);

        builder_.SetInsertPoint(condition_block);
        builder_.CreateCondBr(emit_value(*statement.condition), body_block, end_block);

        body_block->insertInto(current_function_);
        builder_.SetInsertPoint(body_block);
        emit_block(statement.body);
        if (!block_terminated()) {
            builder_.CreateBr(condition_block);
        }

        end_block->insertInto(current_function_);
        builder_.SetInsertPoint(end_block);
    }

    void emit_assign(const ast::AssignStmt& statement) {
        const TypePtr target = type_of(*statement.target);
        llvm::Value* address = emit_address(*statement.target);

        // Overwriting an owned value frees what was there, or the old
        // buffer would be lost with nothing left pointing at it.
        const Slot* slot = slot_of(*statement.target);
        if (typeck::is_owned(target)) {
            if (slot != nullptr) {
                emit_conditional_drop(*slot);
            } else {
                emit_drop(target, address);
            }
        }

        builder_.CreateStore(emit_as(*statement.value, target), address);

        if (slot != nullptr && slot->drop_flag != nullptr) {
            builder_.CreateStore(builder_.getInt1(true), slot->drop_flag);
        }
    }

    /// The slot a place expression names, when it names one directly.
    const Slot* slot_of(const ast::Expr& expr) const {
        const auto* name = ast::node_cast<ast::NameExpr>(&expr);
        if (name == nullptr || !name->module.empty()) {
            return nullptr;
        }
        return lookup(name->name);
    }

    // -----------------------------------------------------------------
    // Expressions: addresses
    // -----------------------------------------------------------------

    /// The address of a place expression. Non-places are materialized
    /// into a temporary slot, which is what makes `f().x` work.
    llvm::Value* emit_address(const ast::Expr& expr) {
        switch (expr.kind) {
            case ast::ExprKind::Name: {
                const auto& name = static_cast<const ast::NameExpr&>(expr);
                if (const Slot* slot = lookup(name.name)) {
                    return slot->address;
                }
                const auto constant =
                    constants_.find(name.module.empty() ? qualify(name.name)
                                                        : name.module + "::" + name.name);
                if (constant != constants_.end()) {
                    return constant->second;
                }
                break;
            }

            case ast::ExprKind::FieldAccess: {
                const auto& access = static_cast<const ast::FieldAccessExpr&>(expr);
                const TypePtr object = type_of(*access.object);
                const TypePtr base = typeck::strip_reference(object);

                // Through a `&T` the address is the pointer itself; on a
                // value it is that value's own slot.
                llvm::Value* base_address = object->kind == TypeKind::Reference
                                                ? emit_value(*access.object)
                                                : emit_address(*access.object);

                const std::string struct_name = typeck::to_string(base);
                const typeck::StructInfo& info = checked_.structs.at(struct_name);
                const typeck::FieldInfo* field = info.field(access.field);
                return builder_.CreateStructGEP(struct_types_.at(struct_name), base_address,
                                                static_cast<unsigned>(field->index),
                                                access.field);
            }

            case ast::ExprKind::Index: {
                const auto& index = static_cast<const ast::IndexExpr&>(expr);
                const TypePtr object = type_of(*index.object);
                const TypePtr base = typeck::strip_reference(object);

                llvm::Value* base_address = object->kind == TypeKind::Reference
                                                ? emit_value(*index.object)
                                                : emit_address(*index.object);
                llvm::Value* offset = emit_value(*index.index);

                if (base->kind == TypeKind::Vec) {
                    // A `Vec` indexes into its heap buffer, and its
                    // length is a runtime value rather than part of the
                    // type, so the bounds check loads it.
                    llvm::Value* length = builder_.CreateLoad(
                        int_type(), builder_.CreateStructGEP(buffer_type_, base_address, 1),
                        "len");
                    emit_dynamic_bounds_check(offset, length);

                    llvm::Value* buffer = builder_.CreateLoad(
                        ptr_type(), builder_.CreateStructGEP(buffer_type_, base_address, 0),
                        "buf");
                    return builder_.CreateInBoundsGEP(lower(base->element), buffer, {offset},
                                                      "elem");
                }

                emit_bounds_check(offset, base->length, index.span);
                llvm::Type* array_type = lower(base);
                return builder_.CreateInBoundsGEP(
                    array_type, base_address, {builder_.getInt64(0), offset}, "elem");
            }

            default:
                break;
        }

        // Not a place: evaluate it and give the value a slot to live in.
        const TypePtr type = type_of(expr);
        llvm::Value* slot = create_entry_alloca(lower(type), "temp");
        builder_.CreateStore(emit_value(expr), slot);
        return slot;
    }

    /// The same check against a length only known at runtime.
    void emit_dynamic_bounds_check(llvm::Value* index, llvm::Value* length) {
        llvm::BasicBlock* ok = llvm::BasicBlock::Create(*context_, "bounds.ok");
        llvm::BasicBlock* fail = llvm::BasicBlock::Create(*context_, "bounds.fail");

        builder_.CreateCondBr(builder_.CreateICmpULT(index, length), ok, fail);

        fail->insertInto(current_function_);
        builder_.SetInsertPoint(fail);
        builder_.CreateCall(
            runtime("ember_panic_index_out_of_bounds", void_type(), {int_type(), int_type()}),
            {index, length});
        builder_.CreateUnreachable();

        ok->insertInto(current_function_);
        builder_.SetInsertPoint(ok);
    }

    /// §9 says follow C for the low-level rules, and C does not bounds
    /// check. This does, because Ember has no borrow checker either: an
    /// unchecked write past the end would silently corrupt the frame,
    /// and the check folds away for constant indices under -O1.
    void emit_bounds_check(llvm::Value* index, std::int64_t length, ast::Span span) {
        llvm::BasicBlock* ok = llvm::BasicBlock::Create(*context_, "bounds.ok");
        llvm::BasicBlock* fail = llvm::BasicBlock::Create(*context_, "bounds.fail");

        // Unsigned comparison catches negative indices too: they wrap to
        // very large values and fail the same test.
        llvm::Value* in_range = builder_.CreateICmpULT(index, builder_.getInt64(length));
        builder_.CreateCondBr(in_range, ok, fail);

        fail->insertInto(current_function_);
        builder_.SetInsertPoint(fail);
        builder_.CreateCall(
            runtime("ember_panic_index_out_of_bounds", void_type(), {int_type(), int_type()}),
            {index, builder_.getInt64(length)});
        builder_.CreateUnreachable();

        ok->insertInto(current_function_);
        builder_.SetInsertPoint(ok);
        (void)span;
    }

    // -----------------------------------------------------------------
    // Expressions: values
    // -----------------------------------------------------------------

    /// Evaluate `expr` and adapt it to `target`, inserting the implicit
    /// borrow or load that §4's reference rules imply.
    llvm::Value* emit_as(const ast::Expr& expr, TypePtr target) {
        const TypePtr actual = type_of(expr);
        if (target == nullptr || actual == nullptr || target == actual) {
            return emit_value(expr);
        }
        if (target->kind == TypeKind::Reference && actual->kind != TypeKind::Reference) {
            return emit_address(expr);  // implicit borrow
        }
        if (target->kind != TypeKind::Reference && actual->kind == TypeKind::Reference) {
            return builder_.CreateLoad(lower(target), emit_value(expr), "deref");
        }
        return emit_value(expr);
    }

    llvm::Value* emit_value(const ast::Expr& expr) {
        switch (expr.kind) {
            case ast::ExprKind::IntLit:
                return builder_.getInt64(
                    static_cast<std::uint64_t>(static_cast<const ast::IntLitExpr&>(expr).value));

            case ast::ExprKind::FloatLit:
                return llvm::ConstantFP::get(float_type(),
                                             static_cast<const ast::FloatLitExpr&>(expr).value);

            case ast::ExprKind::BoolLit:
                return builder_.getInt1(static_cast<const ast::BoolLitExpr&>(expr).value);

            case ast::ExprKind::StringLit:
                return emit_string(static_cast<const ast::StringLitExpr&>(expr).value);

            case ast::ExprKind::Name: {
                const auto& name = static_cast<const ast::NameExpr&>(expr);
                const TypePtr type = type_of(expr);
                llvm::Value* value =
                    builder_.CreateLoad(lower(type), emit_address(expr), name.name);

                // The checker decided this read hands ownership on, so
                // this slot must no longer free the buffer.
                if (checked_.is_move(current_instance_, expr)) {
                    if (const Slot* slot = slot_of(expr)) {
                        if (slot->drop_flag != nullptr) {
                            builder_.CreateStore(builder_.getInt1(false), slot->drop_flag);
                        }
                    }
                }
                return value;
            }

            case ast::ExprKind::FieldAccess:
            case ast::ExprKind::Index:
                return builder_.CreateLoad(lower(type_of(expr)), emit_address(expr));

            case ast::ExprKind::Unary:
                return emit_unary(static_cast<const ast::UnaryExpr&>(expr));
            case ast::ExprKind::Binary:
                return emit_binary(static_cast<const ast::BinaryExpr&>(expr));
            case ast::ExprKind::Call:
                return emit_call(static_cast<const ast::CallExpr&>(expr));
            case ast::ExprKind::MethodCall:
                return emit_method_call(static_cast<const ast::MethodCallExpr&>(expr));
            case ast::ExprKind::Closure:
                return emit_closure(static_cast<const ast::ClosureExpr&>(expr));
            case ast::ExprKind::Cast:
                return emit_cast(static_cast<const ast::CastExpr&>(expr));
            case ast::ExprKind::StructLit:
                return emit_struct_literal(static_cast<const ast::StructLitExpr&>(expr));
            case ast::ExprKind::ArrayLit:
                return emit_array_literal(static_cast<const ast::ArrayLitExpr&>(expr));
        }
        return llvm::UndefValue::get(lower(type_of(expr)));
    }

    /// A string literal becomes a private global plus a { ptr, len }
    /// pair built around it.
    llvm::Value* emit_string(const std::string& text) {
        const auto cached = string_literals_.find(text);
        llvm::GlobalVariable* bytes = nullptr;
        if (cached != string_literals_.end()) {
            bytes = cached->second;
        } else {
            bytes = builder_.CreateGlobalString(text, "str");
            string_literals_[text] = bytes;
        }

        llvm::Value* value = llvm::UndefValue::get(string_type_);
        value = builder_.CreateInsertValue(value, bytes, {0});
        value = builder_.CreateInsertValue(
            value, builder_.getInt64(static_cast<std::uint64_t>(text.size())), {1});
        return value;
    }

    llvm::Value* emit_unary(const ast::UnaryExpr& expr) {
        llvm::Value* operand = emit_value(*expr.operand);
        if (expr.op == ast::UnaryOp::Not) {
            return builder_.CreateNot(operand, "not");
        }
        const TypePtr type = type_of(*expr.operand);
        if (type->kind == TypeKind::Float) {
            return builder_.CreateFNeg(operand, "neg");
        }
        return builder_.CreateNeg(operand, "neg");
    }

    llvm::Value* emit_binary(const ast::BinaryExpr& expr) {
        // `&&` and `||` short-circuit, so they cannot evaluate both
        // sides up front like the others.
        if (expr.op == ast::BinaryOp::And || expr.op == ast::BinaryOp::Or) {
            return emit_short_circuit(expr);
        }

        const TypePtr operand_type = type_of(*expr.left);
        llvm::Value* left = emit_value(*expr.left);
        llvm::Value* right = emit_value(*expr.right);

        if (operand_type->kind == TypeKind::String) {
            return emit_string_comparison(expr, left, right);
        }

        const bool is_float = operand_type->kind == TypeKind::Float;

        switch (expr.op) {
            case ast::BinaryOp::Add:
                return is_float ? builder_.CreateFAdd(left, right, "add")
                                : builder_.CreateAdd(left, right, "add");
            case ast::BinaryOp::Subtract:
                return is_float ? builder_.CreateFSub(left, right, "sub")
                                : builder_.CreateSub(left, right, "sub");
            case ast::BinaryOp::Multiply:
                return is_float ? builder_.CreateFMul(left, right, "mul")
                                : builder_.CreateMul(left, right, "mul");
            case ast::BinaryOp::Divide:
                if (is_float) {
                    return builder_.CreateFDiv(left, right, "div");
                }
                emit_divide_by_zero_check(right);
                return builder_.CreateSDiv(left, right, "div");
            case ast::BinaryOp::Remainder:
                emit_divide_by_zero_check(right);
                return builder_.CreateSRem(left, right, "rem");

            case ast::BinaryOp::Equal:
                return is_float ? builder_.CreateFCmpOEQ(left, right, "eq")
                                : builder_.CreateICmpEQ(left, right, "eq");
            case ast::BinaryOp::NotEqual:
                return is_float ? builder_.CreateFCmpONE(left, right, "ne")
                                : builder_.CreateICmpNE(left, right, "ne");
            case ast::BinaryOp::Less:
                return is_float ? builder_.CreateFCmpOLT(left, right, "lt")
                                : builder_.CreateICmpSLT(left, right, "lt");
            case ast::BinaryOp::Greater:
                return is_float ? builder_.CreateFCmpOGT(left, right, "gt")
                                : builder_.CreateICmpSGT(left, right, "gt");
            case ast::BinaryOp::LessEq:
                return is_float ? builder_.CreateFCmpOLE(left, right, "le")
                                : builder_.CreateICmpSLE(left, right, "le");
            case ast::BinaryOp::GreaterEq:
                return is_float ? builder_.CreateFCmpOGE(left, right, "ge")
                                : builder_.CreateICmpSGE(left, right, "ge");

            default:
                break;
        }
        return llvm::UndefValue::get(lower(type_of(expr)));
    }

    /// Integer division by zero is undefined in LLVM and traps on most
    /// targets. A named runtime error beats a bare SIGFPE.
    void emit_divide_by_zero_check(llvm::Value* divisor) {
        llvm::BasicBlock* ok = llvm::BasicBlock::Create(*context_, "div.ok");
        llvm::BasicBlock* fail = llvm::BasicBlock::Create(*context_, "div.zero");

        builder_.CreateCondBr(builder_.CreateICmpEQ(divisor, builder_.getInt64(0)), fail, ok);

        fail->insertInto(current_function_);
        builder_.SetInsertPoint(fail);
        builder_.CreateCall(runtime("ember_panic_divide_by_zero", void_type(), {}), {});
        builder_.CreateUnreachable();

        ok->insertInto(current_function_);
        builder_.SetInsertPoint(ok);
    }

    llvm::Value* emit_string_comparison(const ast::BinaryExpr& expr, llvm::Value* left,
                                        llvm::Value* right) {
        llvm::Value* equal = builder_.CreateCall(
            runtime("ember_string_eq", byte_type(),
                    {ptr_type(), int_type(), ptr_type(), int_type()}),
            {builder_.CreateExtractValue(left, {0}), builder_.CreateExtractValue(left, {1}),
             builder_.CreateExtractValue(right, {0}), builder_.CreateExtractValue(right, {1})});

        llvm::Value* as_bool = builder_.CreateICmpNE(equal, builder_.getInt8(0), "streq");
        return expr.op == ast::BinaryOp::Equal ? as_bool : builder_.CreateNot(as_bool, "strne");
    }

    /// `a && b` evaluates `b` only when `a` is true, and `a || b` only
    /// when `a` is false. A phi merges the two paths.
    llvm::Value* emit_short_circuit(const ast::BinaryExpr& expr) {
        const bool is_and = expr.op == ast::BinaryOp::And;
        llvm::Value* left = emit_value(*expr.left);
        llvm::BasicBlock* entry = builder_.GetInsertBlock();

        llvm::BasicBlock* rhs_block =
            llvm::BasicBlock::Create(*context_, is_and ? "and.rhs" : "or.rhs");
        llvm::BasicBlock* merge_block =
            llvm::BasicBlock::Create(*context_, is_and ? "and.end" : "or.end");

        if (is_and) {
            builder_.CreateCondBr(left, rhs_block, merge_block);
        } else {
            builder_.CreateCondBr(left, merge_block, rhs_block);
        }

        rhs_block->insertInto(current_function_);
        builder_.SetInsertPoint(rhs_block);
        llvm::Value* right = emit_value(*expr.right);
        llvm::BasicBlock* rhs_end = builder_.GetInsertBlock();
        builder_.CreateBr(merge_block);

        merge_block->insertInto(current_function_);
        builder_.SetInsertPoint(merge_block);
        llvm::PHINode* phi = builder_.CreatePHI(bool_type(), 2, is_and ? "and" : "or");
        phi->addIncoming(builder_.getInt1(!is_and), entry);
        phi->addIncoming(right, rhs_end);
        return phi;
    }

    llvm::Value* emit_call(const ast::CallExpr& expr) {
        if (typeck::is_intrinsic(expr.callee)) {
            return emit_intrinsic(expr);
        }

        // A local holding a closure is called through its pointer.
        if (expr.module.empty()) {
            if (const Slot* slot = lookup(expr.callee)) {
                return emit_indirect_call(expr, *slot);
            }
        }

        // The recorded target is authoritative: for a generic call the
        // callee name is `max`, but the symbol is `max__int`.
        const typeck::FunctionInfo* target = checked_.target_of(current_instance_, expr);
        const typeck::FunctionInfo& info =
            target != nullptr
                ? *target
                : checked_.functions.at(expr.module.empty() ? qualify(expr.callee)
                                                            : expr.module + "::" + expr.callee);
        std::vector<llvm::Value*> args;
        args.reserve(expr.args.size());
        for (std::size_t i = 0; i < expr.args.size(); ++i) {
            args.push_back(emit_as(*expr.args[i], info.param_types[i]));
        }

        llvm::Function* callee = functions_.at(info.mangled_name);
        // A void call has no name: LLVM rejects naming a void value.
        return builder_.CreateCall(callee, args,
                                   callee->getReturnType()->isVoidTy() ? "" : "call");
    }

    /// §4's "methods are sugar" made real: this emits a direct call to
    /// `Type_method` with the receiver as the first argument. No vtable,
    /// no dynamic dispatch.
    llvm::Value* emit_method_call(const ast::MethodCallExpr& expr) {
        const typeck::FunctionInfo* info = checked_.target_of(current_instance_, expr);
        if (info == nullptr) {
            return llvm::UndefValue::get(lower(type_of(expr)));
        }

        std::vector<llvm::Value*> args;
        args.reserve(expr.args.size() + 1);

        // The receiver fills `self`: by pointer for `&self`, by value
        // for `self`.
        const TypePtr receiver_type = type_of(*expr.receiver);
        if (info->self_kind == ast::SelfKind::Reference) {
            args.push_back(receiver_type->kind == TypeKind::Reference
                               ? emit_value(*expr.receiver)
                               : emit_address(*expr.receiver));
        } else {
            args.push_back(emit_as(*expr.receiver, info->param_types.front()));
        }

        for (std::size_t i = 0; i < expr.args.size(); ++i) {
            args.push_back(emit_as(*expr.args[i], info->param_types[i + 1]));
        }

        llvm::Function* callee = functions_.at(info->mangled_name);
        return builder_.CreateCall(callee, args,
                                   callee->getReturnType()->isVoidTy() ? "" : "call");
    }

    /// Size of one element, for the growth arithmetic.
    llvm::Value* size_of(TypePtr element) {
        return builder_.getInt64(
            module_->getDataLayout().getTypeAllocSize(lower(element)).getFixedValue());
    }

    /// The address of a container argument, whether it was passed by
    /// reference or named directly.
    llvm::Value* container_address(const ast::Expr& expr) {
        const TypePtr type = type_of(expr);
        return (type != nullptr && type->kind == TypeKind::Reference) ? emit_value(expr)
                                                                     : emit_address(expr);
    }

    llvm::Value* emit_intrinsic(const ast::CallExpr& expr) {
        if (expr.callee == "new_vec" || expr.callee == "new_string") {
            // An empty container is { null, 0, 0 }. Nothing is allocated
            // until the first push, so an unused one costs no heap.
            llvm::Value* value = llvm::UndefValue::get(buffer_type_);
            value = builder_.CreateInsertValue(
                value, llvm::ConstantPointerNull::get(ptr_type()), {0});
            value = builder_.CreateInsertValue(value, builder_.getInt64(0), {1});
            value = builder_.CreateInsertValue(value, builder_.getInt64(0), {2});
            return value;
        }

        if (expr.callee == "push") {
            return emit_push(expr);
        }
        if (expr.callee == "pop") {
            return emit_pop(expr);
        }
        if (expr.callee == "push_str") {
            return emit_push_str(expr);
        }

        if (expr.callee == "len") {
            const TypePtr argument = typeck::strip_reference(type_of(*expr.args.front()));

            if (argument->kind == TypeKind::Vec || argument->kind == TypeKind::StringBuf) {
                // A growable container carries its length at runtime.
                llvm::Value* address = container_address(*expr.args.front());
                return builder_.CreateLoad(
                    int_type(), builder_.CreateStructGEP(buffer_type_, address, 1), "len");
            }

            // An array's length is part of its type, so `len` folds to a
            // constant. The argument is still evaluated for its effects.
            emit_value(*expr.args.front());
            return builder_.getInt64(static_cast<std::uint64_t>(argument->length));
        }

        const bool newline = expr.callee == "println";
        const ast::Expr& argument = *expr.args.front();
        const TypePtr type = typeck::strip_reference(type_of(argument));

        if (type->kind == TypeKind::StringBuf) {
            // A `String` prints its buffer and length directly, the same
            // way a `string` view does.
            llvm::Value* address = container_address(argument);
            builder_.CreateCall(
                runtime(newline ? "ember_println_string" : "ember_print_string", void_type(),
                        {ptr_type(), int_type()}),
                {builder_.CreateLoad(ptr_type(),
                                     builder_.CreateStructGEP(buffer_type_, address, 0)),
                 builder_.CreateLoad(int_type(),
                                     builder_.CreateStructGEP(buffer_type_, address, 1))});
            return nullptr;
        }

        llvm::Value* value = emit_as(argument, type);

        switch (type->kind) {
            case TypeKind::Int:
                builder_.CreateCall(runtime(newline ? "ember_println_int" : "ember_print_int",
                                            void_type(), {int_type()}),
                                    {value});
                break;
            case TypeKind::Float:
                builder_.CreateCall(runtime(newline ? "ember_println_float" : "ember_print_float",
                                            void_type(), {float_type()}),
                                    {value});
                break;
            case TypeKind::Bool:
                // The runtime takes an int8_t, so widen the i1.
                builder_.CreateCall(runtime(newline ? "ember_println_bool" : "ember_print_bool",
                                            void_type(), {byte_type()}),
                                    {builder_.CreateZExt(value, byte_type(), "bool")});
                break;
            case TypeKind::String:
                builder_.CreateCall(
                    runtime(newline ? "ember_println_string" : "ember_print_string", void_type(),
                            {ptr_type(), int_type()}),
                    {builder_.CreateExtractValue(value, {0}),
                     builder_.CreateExtractValue(value, {1})});
                break;
            default:
                break;
        }
        return nullptr;
    }

    /// `value as T`. The checker has already limited this to the numeric
    /// pair and the identity cast, so there are only three cases.
    ///
    /// `float as int` truncates toward zero, which is what C does. A
    /// value too large for an `int` is undefined, also as in C - LLVM
    /// yields poison for it rather than saturating.
    llvm::Value* emit_cast(const ast::CastExpr& expr) {
        const TypePtr source = type_of(*expr.operand);
        const TypePtr target = type_of(expr);
        llvm::Value* value = emit_value(*expr.operand);

        if (source == target || source == nullptr || target == nullptr) {
            return value;
        }
        if (source->kind == TypeKind::Int && target->kind == TypeKind::Float) {
            return builder_.CreateSIToFP(value, float_type(), "cast");
        }
        if (source->kind == TypeKind::Float && target->kind == TypeKind::Int) {
            return builder_.CreateFPToSI(value, int_type(), "cast");
        }
        return value;
    }

    /// `push(v, x)`: grow if the buffer is full, store, bump the length.
    llvm::Value* emit_push(const ast::CallExpr& expr) {
        const TypePtr container = typeck::strip_reference(type_of(*expr.args[0]));
        llvm::Value* address = container_address(*expr.args[0]);
        llvm::Value* value = emit_as(*expr.args[1], container->element);

        llvm::Value* buffer_field = builder_.CreateStructGEP(buffer_type_, address, 0);
        llvm::Value* length_field = builder_.CreateStructGEP(buffer_type_, address, 1);
        llvm::Value* capacity_field = builder_.CreateStructGEP(buffer_type_, address, 2);

        llvm::Value* length = builder_.CreateLoad(int_type(), length_field, "len");

        // The runtime decides whether to grow and by how much, and hands
        // back the buffer, which realloc may have moved.
        llvm::Value* grown = builder_.CreateCall(
            runtime("ember_grow", ptr_type(),
                    {ptr_type(), int_type(), int_type(), ptr_type()}),
            {builder_.CreateLoad(ptr_type(), buffer_field, "buf"), size_of(container->element),
             length, capacity_field});
        builder_.CreateStore(grown, buffer_field);

        builder_.CreateStore(
            value, builder_.CreateInBoundsGEP(lower(container->element), grown, {length}));
        builder_.CreateStore(builder_.CreateAdd(length, builder_.getInt64(1)), length_field);
        return nullptr;
    }

    /// `pop(v)`: the last element, with the length reduced by one.
    llvm::Value* emit_pop(const ast::CallExpr& expr) {
        const TypePtr container = typeck::strip_reference(type_of(*expr.args[0]));
        llvm::Value* address = container_address(*expr.args[0]);

        llvm::Value* length_field = builder_.CreateStructGEP(buffer_type_, address, 1);
        llvm::Value* length = builder_.CreateLoad(int_type(), length_field, "len");

        llvm::BasicBlock* ok = llvm::BasicBlock::Create(*context_, "pop.ok");
        llvm::BasicBlock* empty = llvm::BasicBlock::Create(*context_, "pop.empty");
        builder_.CreateCondBr(builder_.CreateICmpSGT(length, builder_.getInt64(0)), ok, empty);

        empty->insertInto(current_function_);
        builder_.SetInsertPoint(empty);
        llvm::Value* what = builder_.CreateGlobalString("Vec", "popwhat");
        builder_.CreateCall(runtime("ember_panic_empty", void_type(), {ptr_type(), int_type()}),
                            {what, builder_.getInt64(3)});
        builder_.CreateUnreachable();

        ok->insertInto(current_function_);
        builder_.SetInsertPoint(ok);

        llvm::Value* last = builder_.CreateSub(length, builder_.getInt64(1), "last");
        builder_.CreateStore(last, length_field);

        llvm::Value* buffer = builder_.CreateLoad(
            ptr_type(), builder_.CreateStructGEP(buffer_type_, address, 0), "buf");
        return builder_.CreateLoad(
            lower(container->element),
            builder_.CreateInBoundsGEP(lower(container->element), buffer, {last}), "popped");
    }

    /// `push_str(s, text)`: append bytes, growing the buffer as needed.
    llvm::Value* emit_push_str(const ast::CallExpr& expr) {
        llvm::Value* address = container_address(*expr.args[0]);
        const TypePtr text_type = typeck::strip_reference(type_of(*expr.args[1]));

        llvm::Value* bytes = nullptr;
        llvm::Value* count = nullptr;
        if (text_type->kind == TypeKind::StringBuf) {
            llvm::Value* other = container_address(*expr.args[1]);
            bytes = builder_.CreateLoad(ptr_type(),
                                        builder_.CreateStructGEP(buffer_type_, other, 0));
            count = builder_.CreateLoad(int_type(),
                                        builder_.CreateStructGEP(buffer_type_, other, 1));
        } else {
            llvm::Value* view = emit_as(*expr.args[1], text_type);
            bytes = builder_.CreateExtractValue(view, {0});
            count = builder_.CreateExtractValue(view, {1});
        }

        llvm::Value* buffer_field = builder_.CreateStructGEP(buffer_type_, address, 0);
        llvm::Value* appended = builder_.CreateCall(
            runtime("ember_string_append", ptr_type(),
                    {ptr_type(), ptr_type(), ptr_type(), ptr_type(), int_type()}),
            {builder_.CreateLoad(ptr_type(), buffer_field, "buf"),
             builder_.CreateStructGEP(buffer_type_, address, 1),
             builder_.CreateStructGEP(buffer_type_, address, 2), bytes, count});
        builder_.CreateStore(appended, buffer_field);
        return nullptr;
    }

    /// Builds a closure value: lift the body to a real function, then
    /// pack its captures into a heap block beside a pointer to it.
    ///
    /// This is what makes a closure a *value* rather than a special
    /// form: after this, calling one is an indirect call through a
    /// pointer, and nothing else in the language needs to know.
    llvm::Value* emit_closure(const ast::ClosureExpr& expr) {
        const TypePtr type = type_of(expr);
        const std::vector<TypePtr>& capture_types = capture_types_of(expr);

        // The environment layout is per closure, not per type: two
        // closures of the same `fn(int) -> int` may capture different
        // things.
        std::vector<llvm::Type*> fields;
        for (const TypePtr captured : capture_types) {
            fields.push_back(lower(captured));
        }
        llvm::StructType* env_type =
            llvm::StructType::create(*context_, fields, "closure.env." + std::to_string(expr.id));

        llvm::Function* lifted = emit_lifted_closure(expr, type, capture_types, env_type);

        // Copy each captured value into a fresh heap block. Captures are
        // copyable, so this is a copy rather than a move.
        llvm::Value* env = llvm::ConstantPointerNull::get(ptr_type());
        if (!capture_types.empty()) {
            const std::uint64_t size =
                module_->getDataLayout().getTypeAllocSize(env_type).getFixedValue();
            env = builder_.CreateCall(runtime("ember_alloc", ptr_type(), {int_type()}),
                                      {builder_.getInt64(size)}, "env");

            for (std::size_t i = 0; i < expr.captures.size(); ++i) {
                const Slot* slot = lookup(expr.captures[i].name);
                if (slot == nullptr) {
                    continue;
                }
                llvm::Value* value =
                    builder_.CreateLoad(lower(capture_types[i]), slot->address);
                builder_.CreateStore(
                    value, builder_.CreateStructGEP(env_type, env, static_cast<unsigned>(i)));
            }
        }

        llvm::Value* value = llvm::UndefValue::get(closure_type_);
        value = builder_.CreateInsertValue(value, lifted, {0});
        value = builder_.CreateInsertValue(value, env, {1});
        return value;
    }

    const std::vector<TypePtr>& capture_types_of(const ast::ClosureExpr& expr) {
        static const std::vector<TypePtr> none;
        const auto found = checked_.closure_captures.find({current_instance_, &expr});
        return found != checked_.closure_captures.end() ? found->second : none;
    }

    /// Emits the closure's body as an ordinary function taking the
    /// environment as a hidden first parameter, then restores whatever
    /// was being emitted around it.
    llvm::Function* emit_lifted_closure(const ast::ClosureExpr& expr, TypePtr type,
                                        const std::vector<TypePtr>& capture_types,
                                        llvm::StructType* env_type) {
        std::vector<llvm::Type*> params{ptr_type()};
        for (const TypePtr param : type->args) {
            params.push_back(lower(param));
        }
        llvm::FunctionType* signature = llvm::FunctionType::get(
            lower(type->result), params, false);

        const std::string name = "ember_closure_" + std::to_string(expr.id);
        llvm::Function* function = llvm::Function::Create(
            signature, llvm::Function::InternalLinkage, name, module_.get());

        // Save everything the outer emission is in the middle of.
        llvm::Function* saved_function = current_function_;
        const typeck::FunctionInfo* saved_info = current_info_;
        const bool saved_entry = current_is_entry_point_;
        std::vector<std::map<std::string, Slot>> saved_scopes;
        saved_scopes.swap(scopes_);
        llvm::IRBuilder<>::InsertPoint saved_point = builder_.saveIP();

        current_function_ = function;
        current_is_entry_point_ = false;

        typeck::FunctionInfo info;
        info.name = name;
        info.return_type = type->result;
        current_info_ = &info;

        builder_.SetInsertPoint(llvm::BasicBlock::Create(*context_, "entry", function));
        push_scope();

        // Captures come back out of the environment into ordinary
        // locals, so the body reads them like any other variable.
        llvm::Value* env = function->getArg(0);
        for (std::size_t i = 0; i < expr.captures.size(); ++i) {
            const std::string& captured = expr.captures[i].name;
            llvm::Value* slot = create_entry_alloca(lower(capture_types[i]), captured);
            builder_.CreateStore(
                builder_.CreateLoad(lower(capture_types[i]),
                                    builder_.CreateStructGEP(env_type, env,
                                                             static_cast<unsigned>(i))),
                slot);
            scopes_.back()[captured] = Slot{slot, capture_types[i], nullptr};
        }

        for (std::size_t i = 0; i < expr.params.size(); ++i) {
            const ast::Param& param = expr.params[i];
            llvm::Value* slot = create_entry_alloca(lower(type->args[i]), param.name);
            builder_.CreateStore(function->getArg(static_cast<unsigned>(i + 1)), slot);
            scopes_.back()[param.name] = Slot{slot, type->args[i], nullptr};
        }

        emit_block(expr.body);
        if (!builder_.GetInsertBlock()->getTerminator()) {
            emit_all_drops();
            builder_.CreateRetVoid();
        }

        scopes_.clear();
        scopes_.swap(saved_scopes);
        builder_.restoreIP(saved_point);
        current_function_ = saved_function;
        current_info_ = saved_info;
        current_is_entry_point_ = saved_entry;
        return function;
    }

    /// `f(args)` where `f` holds a closure: load the pair and call
    /// through it, passing the environment first.
    llvm::Value* emit_indirect_call(const ast::CallExpr& expr, const Slot& slot) {
        const TypePtr type = typeck::strip_reference(slot.type);

        llvm::Value* address = slot.address;
        if (slot.type->kind == TypeKind::Reference) {
            address = builder_.CreateLoad(ptr_type(), slot.address, "borrowed");
        }

        llvm::Value* code = builder_.CreateLoad(
            ptr_type(), builder_.CreateStructGEP(closure_type_, address, 0), "code");
        llvm::Value* env = builder_.CreateLoad(
            ptr_type(), builder_.CreateStructGEP(closure_type_, address, 1), "env");

        std::vector<llvm::Type*> param_types{ptr_type()};
        std::vector<llvm::Value*> args{env};
        for (std::size_t i = 0; i < expr.args.size(); ++i) {
            param_types.push_back(lower(type->args[i]));
            args.push_back(emit_as(*expr.args[i], type->args[i]));
        }

        llvm::FunctionType* signature =
            llvm::FunctionType::get(lower(type->result), param_types, false);
        return builder_.CreateCall(signature, code, args,
                                   lower(type->result)->isVoidTy() ? "" : "call");
    }

    llvm::Value* emit_struct_literal(const ast::StructLitExpr& expr) {
        // The literal names the base struct; its instantiation comes
        // from the type the checker gave the expression.
        const std::string struct_name = typeck::to_string(type_of(expr));
        const typeck::StructInfo& info = checked_.structs.at(struct_name);
        llvm::StructType* type = struct_types_.at(struct_name);

        // Built with insertvalue rather than a slot and stores, so a
        // struct literal is an ordinary value like any other.
        llvm::Value* value = llvm::UndefValue::get(type);
        for (const ast::FieldInit& field : expr.fields) {
            const typeck::FieldInfo* target = info.field(field.name);
            value = builder_.CreateInsertValue(
                value, emit_as(*field.value, target->type),
                {static_cast<unsigned>(target->index)}, field.name);
        }
        return value;
    }

    llvm::Value* emit_array_literal(const ast::ArrayLitExpr& expr) {
        const TypePtr type = type_of(expr);
        llvm::Value* value = llvm::UndefValue::get(lower(type));
        for (std::size_t i = 0; i < expr.elements.size(); ++i) {
            value = builder_.CreateInsertValue(value, emit_as(*expr.elements[i], type->element),
                                               {static_cast<unsigned>(i)});
        }
        return value;
    }

    // -----------------------------------------------------------------
    // Constants
    // -----------------------------------------------------------------

    /// A `const` becomes a real LLVM global with a constant initializer,
    /// which means its value has to be foldable at compile time. That is
    /// the same rule C and Rust apply, and the diagnostic says so.
    void emit_constants() {
        for (const ModuleInput& module : modules_) {
            current_module_ = module.name;
            emit_module_constants(*module.program);
        }
        current_module_.clear();
    }

    void emit_module_constants(const ast::Program& program) {
        for (const ast::ItemPtr& item : program.items) {
            const auto* declaration = ast::node_cast<ast::ConstDecl>(item.get());
            if (declaration == nullptr) {
                continue;
            }
            const std::string qualified = qualify(declaration->name);
            if (constants_.count(qualified) != 0) {
                continue;
            }
            const auto entry = checked_.constants.find(qualified);
            if (entry == checked_.constants.end()) {
                continue;
            }

            llvm::Constant* value = fold(*declaration->value, entry->second.type);
            if (value == nullptr) {
                report(declaration->value->span, "constant initializer is not a constant",
                       "a `const` must be computable at compile time")
                    .with_note("only literals and operations on literals are allowed here");
                continue;
            }

            auto* global = new llvm::GlobalVariable(*module_, lower(entry->second.type), true,
                                                    llvm::GlobalValue::PrivateLinkage, value,
                                                    qualified);
            constants_[qualified] = global;
        }
    }

    /// Constant-folds an initializer, or returns null if it needs
    /// anything that only exists at run time.
    llvm::Constant* fold(const ast::Expr& expr, TypePtr expected) {
        switch (expr.kind) {
            case ast::ExprKind::IntLit:
                return builder_.getInt64(
                    static_cast<std::uint64_t>(static_cast<const ast::IntLitExpr&>(expr).value));
            case ast::ExprKind::FloatLit:
                return llvm::ConstantFP::get(float_type(),
                                             static_cast<const ast::FloatLitExpr&>(expr).value);
            case ast::ExprKind::BoolLit:
                return builder_.getInt1(static_cast<const ast::BoolLitExpr&>(expr).value);

            case ast::ExprKind::StringLit: {
                const auto& literal = static_cast<const ast::StringLitExpr&>(expr);
                llvm::Constant* bytes = llvm::ConstantDataArray::getString(
                    *context_, literal.value, true);
                auto* global =
                    new llvm::GlobalVariable(*module_, bytes->getType(), true,
                                             llvm::GlobalValue::PrivateLinkage, bytes, "str");
                return llvm::ConstantStruct::get(
                    string_type_,
                    {global, builder_.getInt64(static_cast<std::uint64_t>(literal.value.size()))});
            }

            case ast::ExprKind::Unary: {
                const auto& unary = static_cast<const ast::UnaryExpr&>(expr);
                llvm::Constant* operand = fold(*unary.operand, expected);
                if (operand == nullptr) {
                    return nullptr;
                }
                if (unary.op == ast::UnaryOp::Not) {
                    return llvm::ConstantExpr::getNot(operand);
                }
                return llvm::ConstantExpr::getNeg(operand);
            }

            case ast::ExprKind::StructLit: {
                const auto& literal = static_cast<const ast::StructLitExpr&>(expr);
                const std::string struct_name = typeck::to_string(type_of(expr));
                const typeck::StructInfo& info = checked_.structs.at(struct_name);
                std::vector<llvm::Constant*> fields(info.fields.size(), nullptr);

                for (const ast::FieldInit& field : literal.fields) {
                    const typeck::FieldInfo* target = info.field(field.name);
                    fields[target->index] = fold(*field.value, target->type);
                    if (fields[target->index] == nullptr) {
                        return nullptr;
                    }
                }
                for (llvm::Constant* field : fields) {
                    if (field == nullptr) {
                        return nullptr;
                    }
                }
                return llvm::ConstantStruct::get(struct_types_.at(struct_name), fields);
            }

            case ast::ExprKind::ArrayLit: {
                const auto& literal = static_cast<const ast::ArrayLitExpr&>(expr);
                const TypePtr type = type_of(expr);
                std::vector<llvm::Constant*> elements;
                for (const ast::ExprPtr& element : literal.elements) {
                    llvm::Constant* value = fold(*element, type->element);
                    if (value == nullptr) {
                        return nullptr;
                    }
                    elements.push_back(value);
                }
                return llvm::ConstantArray::get(
                    llvm::cast<llvm::ArrayType>(lower(type)), elements);
            }

            default:
                // Calls, names and arithmetic on them are all run-time
                // work; v1 has no const evaluator beyond literals.
                return nullptr;
        }
    }

    std::map<std::string, llvm::GlobalVariable*> string_literals_;
};

/// Builds the target machine for the host, initializing LLVM's native
/// target exactly once.
llvm::TargetMachine* host_target_machine(std::string& error) {
    static bool initialized = false;
    if (!initialized) {
        // Only the target and its asm printer: object emission needs
        // both, and nothing here ever parses assembly text.
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        initialized = true;
    }

    const std::string triple = llvm::sys::getDefaultTargetTriple();
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (target == nullptr) {
        return nullptr;
    }

    llvm::TargetOptions options;
    return target->createTargetMachine(llvm::Triple(triple), "generic", "", options,
                                       llvm::Reloc::PIC_);
}

void run_optimization_pipeline(llvm::Module& module, llvm::TargetMachine* machine,
                               unsigned level) {
    if (level == 0) {
        return;
    }

    llvm::LoopAnalysisManager loop_analysis;
    llvm::FunctionAnalysisManager function_analysis;
    llvm::CGSCCAnalysisManager cgscc_analysis;
    llvm::ModuleAnalysisManager module_analysis;

    llvm::PassBuilder builder(machine);
    builder.registerModuleAnalyses(module_analysis);
    builder.registerCGSCCAnalyses(cgscc_analysis);
    builder.registerFunctionAnalyses(function_analysis);
    builder.registerLoopAnalyses(loop_analysis);
    builder.crossRegisterProxies(loop_analysis, function_analysis, cgscc_analysis,
                                 module_analysis);

    llvm::OptimizationLevel optimization = llvm::OptimizationLevel::O1;
    if (level == 2) {
        optimization = llvm::OptimizationLevel::O2;
    } else if (level >= 3) {
        optimization = llvm::OptimizationLevel::O3;
    }

    builder.buildPerModuleDefaultPipeline(optimization).run(module, module_analysis);
}

}  // namespace

CompileResult compile_to_string(const std::vector<ModuleInput>& modules,
                                const typeck::CheckResult& checked,
                                const CompileOptions& options) {
    CompileResult result;

    // The layout has to exist before any IR is built: a `Vec<T>` push
    // needs the size of T.
    std::string error;
    llvm::TargetMachine* machine = host_target_machine(error);
    const std::optional<llvm::DataLayout> layout =
        machine != nullptr ? std::optional<llvm::DataLayout>{machine->createDataLayout()}
                           : std::nullopt;

    Emitter emitter(modules, checked, options, layout ? &*layout : nullptr);
    if (!emitter.run()) {
        result.diagnostics = std::move(emitter.diagnostics());
        return result;
    }

    if (machine != nullptr) {
        emitter.module().setTargetTriple(machine->getTargetTriple());
        run_optimization_pipeline(emitter.module(), machine, options.optimization_level);
    }

    llvm::raw_string_ostream stream(result.assembly);
    emitter.module().print(stream, nullptr);
    stream.flush();
    return result;
}

CompileResult compile(const std::vector<ModuleInput>& modules,
                      const typeck::CheckResult& checked,
                      const std::filesystem::path& output_path,
                      const CompileOptions& options) {
    CompileResult result;

    std::string error;
    llvm::TargetMachine* machine = host_target_machine(error);
    if (machine == nullptr) {
        result.diagnostics.push_back(ast::Diagnostic::error(
            "cannot target this machine", ast::Span::at(0), error));
        return result;
    }

    const llvm::DataLayout layout = machine->createDataLayout();
    Emitter emitter(modules, checked, options, &layout);
    if (!emitter.run()) {
        result.diagnostics = std::move(emitter.diagnostics());
        return result;
    }

    emitter.module().setTargetTriple(machine->getTargetTriple());
    run_optimization_pipeline(emitter.module(), machine, options.optimization_level);

    std::error_code code;
    llvm::raw_fd_ostream out(output_path.string(), code, llvm::sys::fs::OF_None);
    if (code) {
        result.diagnostics.push_back(ast::Diagnostic::error(
            "cannot write `" + output_path.string() + "`", ast::Span::at(0), code.message()));
        return result;
    }

    if (options.output == OutputKind::Assembly) {
        emitter.module().print(out, nullptr);
        out.flush();
        return result;
    }

    llvm::legacy::PassManager pass_manager;
    if (machine->addPassesToEmitFile(pass_manager, out, nullptr,
                                     llvm::CodeGenFileType::ObjectFile)) {
        result.diagnostics.push_back(
            ast::Diagnostic::error("this target cannot emit object files", ast::Span::at(0),
                                   "LLVM refused to build an object-file pipeline"));
        return result;
    }
    pass_manager.run(emitter.module());
    out.flush();
    return result;
}

CompileResult compile_to_string(const ast::Program& program,
                                const typeck::CheckResult& checked,
                                const ast::SourceFile& source, const CompileOptions& options) {
    (void)source;
    return compile_to_string(std::vector<ModuleInput>{ModuleInput{{}, &program}}, checked,
                             options);
}

CompileResult compile(const ast::Program& program, const typeck::CheckResult& checked,
                      const ast::SourceFile& source, const std::filesystem::path& output_path,
                      const CompileOptions& options) {
    (void)source;
    return compile(std::vector<ModuleInput>{ModuleInput{{}, &program}}, checked, output_path,
                   options);
}

#else  // !EMBER_HAVE_LLVM

namespace {

CompileResult unavailable() {
    CompileResult result;
    result.diagnostics.push_back(ast::Diagnostic::error(
        "this build of ember has no code generator", ast::Span::at(0),
        "the compiler was built without LLVM"));
    return result;
}

}  // namespace

CompileResult compile_to_string(const std::vector<ModuleInput>&, const typeck::CheckResult&,
                                const CompileOptions&) {
    return unavailable();
}

CompileResult compile(const std::vector<ModuleInput>&, const typeck::CheckResult&,
                      const std::filesystem::path&, const CompileOptions&) {
    return unavailable();
}

CompileResult compile_to_string(const ast::Program&, const typeck::CheckResult&,
                                const ast::SourceFile&, const CompileOptions&) {
    return unavailable();
}

CompileResult compile(const ast::Program&, const typeck::CheckResult&, const ast::SourceFile&,
                      const std::filesystem::path&, const CompileOptions&) {
    return unavailable();
}

#endif  // EMBER_HAVE_LLVM

}  // namespace ember::codegen
