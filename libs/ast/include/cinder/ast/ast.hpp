// cinder::ast - shared frontend types for the Cinder compiler.
//
// Despite the name this library holds everything the pipeline stages must
// agree on: the language's identity constants, source spans, diagnostics
// (§7) and - from Phase 2 - the AST nodes themselves. Keeping spans and
// diagnostics here lets the lexer, parser and type checker all report
// errors in the same format without a dependency cycle.
//
// Phase 0: scaffolding only.

#ifndef CINDER_AST_AST_HPP
#define CINDER_AST_AST_HPP

#include <string_view>

namespace cinder::ast {

/// Human-readable name of the language, used in diagnostics and CLI output.
inline constexpr std::string_view kLanguageName = "Cinder";

/// Canonical source file extension for Cinder programs, without the dot.
/// Renaming the language means changing this and the CLI follows.
inline constexpr std::string_view kFileExtension = "ci";

/// Version of the compiler, kept in sync with the CMake project version.
std::string_view version() noexcept;

}  // namespace cinder::ast

#endif  // CINDER_AST_AST_HPP
