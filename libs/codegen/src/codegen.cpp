#include "ember/codegen/codegen.hpp"

#ifndef EMBER_HAVE_LLVM
#define EMBER_HAVE_LLVM 0
#endif

#include <map>
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
};

class Emitter {
public:
    Emitter(const std::vector<ModuleInput>& modules, const typeck::CheckResult& checked,
            const CompileOptions& options)
        : modules_(modules),
          checked_(checked),
          context_(std::make_unique<llvm::LLVMContext>()),
          module_(std::make_unique<llvm::Module>(options.module_name, *context_)),
          builder_(*context_) {
        // { i8*, i64 }: §4's "immutable, fixed-length view". Carrying the
        // length keeps len() O(1) and lets a string hold a NUL byte.
        string_type_ = llvm::StructType::create(*context_, {ptr_type(), int_type()},
                                                "ember.string");
    }

    bool run() {
        declare_structs();
        declare_functions();
        emit_constants();
        emit_function_bodies();

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
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
    std::vector<ast::Diagnostic> diagnostics_;

    llvm::StructType* string_type_ = nullptr;
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
            current_module_ = module.name;
            emit_concrete_bodies(*module.program);
        }

        for (const typeck::Instantiation& instance : checked_.instantiations) {
            current_instance_ = instance.id;
            current_module_ = instance.info.module;
            emit_function(*instance.decl, instance.info);
        }
        current_instance_ = typeck::kRootInstance;
        current_module_.clear();
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
            emit_default_return();
        }

        pop_scope();
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
    // Scopes
    // -----------------------------------------------------------------

    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() { scopes_.pop_back(); }

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
        builder_.CreateStore(emit_as(*statement.value, type), slot);
        scopes_.back()[statement.name] = Slot{slot, type};
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
            emit_default_return();
            return;
        }
        llvm::Value* value = emit_as(*statement.value, current_info_->return_type);
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
        builder_.CreateStore(emit_as(*statement.value, target), address);
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
                return builder_.CreateLoad(lower(type), emit_address(expr), name.name);
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

    llvm::Value* emit_intrinsic(const ast::CallExpr& expr) {
        if (expr.callee == "len") {
            // An array's length is part of its type, so `len` folds to a
            // constant. The argument is still evaluated for its effects.
            const TypePtr argument =
                typeck::strip_reference(type_of(*expr.args.front()));
            emit_value(*expr.args.front());
            return builder_.getInt64(static_cast<std::uint64_t>(argument->length));
        }

        const bool newline = expr.callee == "println";
        const ast::Expr& argument = *expr.args.front();
        const TypePtr type = typeck::strip_reference(type_of(argument));
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

    Emitter emitter(modules, checked, options);
    if (!emitter.run()) {
        result.diagnostics = std::move(emitter.diagnostics());
        return result;
    }

    std::string error;
    llvm::TargetMachine* machine = host_target_machine(error);
    if (machine != nullptr) {
        emitter.module().setTargetTriple(machine->getTargetTriple());
        emitter.module().setDataLayout(machine->createDataLayout());
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

    Emitter emitter(modules, checked, options);
    if (!emitter.run()) {
        result.diagnostics = std::move(emitter.diagnostics());
        return result;
    }

    std::string error;
    llvm::TargetMachine* machine = host_target_machine(error);
    if (machine == nullptr) {
        result.diagnostics.push_back(ast::Diagnostic::error(
            "cannot target this machine", ast::Span::at(0), error));
        return result;
    }

    emitter.module().setTargetTriple(machine->getTargetTriple());
    emitter.module().setDataLayout(machine->createDataLayout());
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
