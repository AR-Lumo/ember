// Diagnostics, rendered in the §7 format.
//
//     error: type mismatch
//       --> file.sn:12:9
//        |
//     12 |     let x: int = "hello";
//        |                  ^^^^^^^ expected `int`, found `string`
//
// The spec calls this non-negotiable, so it lives here in soliton::ast
// where every stage from the lexer onward can reach it, rather than
// being bolted on when the type checker lands.

#ifndef SOLITON_AST_DIAGNOSTIC_HPP
#define SOLITON_AST_DIAGNOSTIC_HPP

#include "soliton/ast/span.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace soliton::ast {

enum class Severity {
    Error,
    Warning,
    Note,
};

/// The word that opens the first line of a rendered diagnostic.
std::string_view severity_name(Severity severity) noexcept;

/// One diagnostic: a headline message, the span it points at, an inline
/// label under the carets, and any trailing notes.
struct Diagnostic {
    Severity severity = Severity::Error;
    /// Headline, e.g. "type mismatch". Lower case, no trailing period.
    std::string message;
    /// The source range the carets underline.
    Span span;
    /// Text printed after the carets, e.g. "expected `int`, found `string`".
    std::string label;
    /// Extra lines rendered as `= note: ...`, used from Phase 5 for
    /// suggestions like "did you mean `x`?".
    std::vector<std::string> notes;

    static Diagnostic error(std::string message, Span span, std::string label = {});
    static Diagnostic warning(std::string message, Span span, std::string label = {});

    /// Attach a note; returns `*this` so notes can be chained on.
    Diagnostic& with_note(std::string note);
};

/// Render one diagnostic in the §7 format, newline-terminated.
///
/// The SourceMap overload is the one a multi-file program needs: with
/// `import`, consecutive diagnostics can point into different files, so
/// each one is resolved through its span's file id. The single-file
/// overload stays for callers that have only ever seen one file.
std::string render(const Diagnostic& diagnostic, const SourceFile& source);
std::string render(const Diagnostic& diagnostic, const SourceMap& sources);

/// Render several diagnostics, blank-line separated, followed by a
/// summary line such as "error: aborting due to 2 previous errors".
std::string render_all(const std::vector<Diagnostic>& diagnostics, const SourceFile& source);
std::string render_all(const std::vector<Diagnostic>& diagnostics, const SourceMap& sources);

}  // namespace soliton::ast

#endif  // SOLITON_AST_DIAGNOSTIC_HPP
