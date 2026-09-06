// soliton::ast - shared frontend types for the Soliton compiler.
//
// Despite the name this library holds everything the pipeline stages must
// agree on: the language's identity constants, source spans, diagnostics
// (§7) and - from Phase 2 - the AST nodes themselves. Keeping spans and
// diagnostics here lets the lexer, parser and type checker all report
// errors in the same format without a dependency cycle.
//
// Phase 0: scaffolding only.

#ifndef SOLITON_AST_AST_HPP
#define SOLITON_AST_AST_HPP

#include <string_view>

namespace soliton::ast {

/// Human-readable name of the language, used in diagnostics and CLI output.
inline constexpr std::string_view kLanguageName = "Soliton";

/// Canonical source file extension for Soliton programs, without the dot.
/// Renaming the language means changing this and the CLI follows.
inline constexpr std::string_view kFileExtension = "sn";

/// Version of the compiler, kept in sync with the CMake project version.
std::string_view version() noexcept;

}  // namespace soliton::ast

#endif  // SOLITON_AST_AST_HPP
