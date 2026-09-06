// soliton::lexer - source text -> token stream with line/column spans.
//
// The lexer never stops at the first problem: it records a diagnostic,
// skips the offending text, and carries on, so one run reports every
// lexical error in a file. Phase 5 asks for exactly this behaviour from
// the whole compiler; it costs nothing to have it here already.

#ifndef SOLITON_LEXER_LEXER_HPP
#define SOLITON_LEXER_LEXER_HPP

#include "soliton/ast/diagnostic.hpp"
#include "soliton/ast/span.hpp"
#include "soliton/lexer/token.hpp"

#include <string_view>
#include <vector>

namespace soliton::lexer {

/// Which build phase has implemented this stage so far.
inline constexpr int kImplementedPhase = 1;

/// Name of this pipeline stage, used in driver messages.
std::string_view stage_name() noexcept;

struct LexResult {
    /// Always ends with an Eof token, even when errors were reported.
    std::vector<Token> tokens;
    std::vector<ast::Diagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

/// Tokenize a whole file. Comments and whitespace are trivia and do not
/// appear in the token stream; `///` doc comments are recognized as
/// comments and skipped too, since v1 has no doc tooling to consume them.
LexResult tokenize(const ast::SourceFile& source);

/// Render a token stream one token per line, for the `.tokens` golden
/// snapshots:
///
///     1:1-1:4    kw_pub      pub
///     3:18-3:25  string_lit  "hi\n"  = hi<newline>
std::string dump_tokens(const std::vector<Token>& tokens, const ast::SourceFile& source);

}  // namespace soliton::lexer

#endif  // SOLITON_LEXER_LEXER_HPP
