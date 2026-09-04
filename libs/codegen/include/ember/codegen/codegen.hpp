// ember::codegen - typed AST -> LLVM IR -> native object code.
//
// Lowering follows §4's claim that methods are sugar: every method
// becomes a plain function named `Type_method` whose first parameter is
// the receiver. Nothing here is virtual, and no vtable is emitted.
//
// Representation choices, all of them "what would C do" (§9):
//
//   int      i64
//   float    double
//   bool     i1 in registers, i8 in memory
//   string   { i8*, i64 } - pointer and length, passed as two arguments
//   &T       T* (a plain non-owning pointer, no lifetime tracking in v1)
//   [T; N]   [N x T], passed by pointer
//   struct   an LLVM struct in declaration order, passed by pointer
//
// Locals live in `alloca` slots in the entry block and are loaded and
// stored on each use. That is what clang -O0 emits; mem2reg promotes the
// ones that can be registers, so writing SSA by hand would only make the
// lowering harder to read for no benefit.
//
// The whole library still compiles when LLVM is absent, so Phases 0-3
// can be built and tested on a machine without it: `is_available()`
// reports false and `compile` fails with an explanation.

#ifndef EMBER_CODEGEN_CODEGEN_HPP
#define EMBER_CODEGEN_CODEGEN_HPP

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/nodes.hpp"
#include "ember/ast/span.hpp"
#include "ember/typeck/typeck.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ember::codegen {

/// Which build phase has implemented this stage so far.
inline constexpr int kImplementedPhase = 4;

/// Name of this pipeline stage, used in driver messages.
std::string_view stage_name() noexcept;

/// True when the compiler was built against LLVM.
bool is_available() noexcept;

/// The LLVM version this compiler was built against, or "none".
std::string_view llvm_version() noexcept;

/// What `compile` should leave behind.
enum class OutputKind {
    /// Human-readable LLVM IR (`.ll`). Used by the golden tests.
    Assembly,
    /// A native object file (`.o`), ready for the system linker.
    Object,
};

struct CompileOptions {
    OutputKind output = OutputKind::Object;
    /// Name recorded in the module, for readable IR.
    std::string module_name = "ember";
    /// LLVM optimization level, 0-3.
    unsigned optimization_level = 0;
};

struct CompileResult {
    /// Only populated for OutputKind::Assembly.
    std::string assembly;
    std::vector<ast::Diagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

/// One module to lower, paired with the name its items were qualified
/// under. Mirrors typeck::ModuleInput.
struct ModuleInput {
    std::string name;
    const ast::Program* program = nullptr;
};

/// Lower a checked program and write the result to `output_path`.
///
/// `checked` must have come from a successful `typeck::check` of these
/// same modules: codegen reads the recorded expression types rather than
/// re-deriving them, and assumes every one of them is present.
///
/// Every module becomes one LLVM module. They are compiled together
/// rather than separately, so a call across an `import` is a direct call
/// and the whole program optimizes as a unit; separate compilation would
/// need a real object-file interface, which v2 does not have.
CompileResult compile(const std::vector<ModuleInput>& modules,
                      const typeck::CheckResult& checked, const std::filesystem::path& output_path,
                      const CompileOptions& options = {});

/// Lower a checked program to LLVM IR text without touching the disk.
CompileResult compile_to_string(const std::vector<ModuleInput>& modules,
                                const typeck::CheckResult& checked,
                                const CompileOptions& options = {});

/// Single-module convenience wrappers.
CompileResult compile(const ast::Program& program, const typeck::CheckResult& checked,
                      const ast::SourceFile& source, const std::filesystem::path& output_path,
                      const CompileOptions& options = {});
CompileResult compile_to_string(const ast::Program& program,
                                const typeck::CheckResult& checked,
                                const ast::SourceFile& source,
                                const CompileOptions& options = {});

/// Check that the program has an entry point `main` with the shape the
/// linker needs: no parameters, and no return value.
///
/// This is separate from `typeck::check` because it is a requirement of
/// producing an executable, not of the type system: `ember check` on a
/// file with no `main` is answering a different question.
std::vector<ast::Diagnostic> verify_entry_point(const typeck::CheckResult& checked);

}  // namespace ember::codegen

#endif  // EMBER_CODEGEN_CODEGEN_HPP
