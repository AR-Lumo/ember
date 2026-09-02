#include "ember/ast/diagnostic.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace ember::ast {
namespace {

/// Whitespace that lines the carets up under the offending text.
///
/// Tabs are copied through rather than turned into spaces: the source
/// line above is printed verbatim, so reusing its own tabs is the only
/// way the carets stay aligned whatever tab width the terminal uses.
std::string caret_indent(std::string_view line, std::uint32_t column) {
    const std::size_t stop = std::min<std::size_t>(column - 1, line.size());
    std::string indent;
    indent.reserve(stop);
    for (std::size_t i = 0; i < stop; ++i) {
        indent.push_back(line[i] == '\t' ? '\t' : ' ');
    }
    return indent;
}

/// Number of carets to draw. A span is clamped to the line it starts on,
/// so a multi-line span underlines its first line and stops there.
std::size_t caret_count(std::string_view line, std::uint32_t column, std::uint32_t length) {
    const std::size_t remaining =
        (column - 1 < line.size()) ? line.size() - (column - 1) : 0;
    const std::size_t wanted = std::max<std::size_t>(length, 1);
    return std::max<std::size_t>(std::min(wanted, remaining), 1);
}

}  // namespace

std::string_view severity_name(Severity severity) noexcept {
    switch (severity) {
        case Severity::Error:
            return "error";
        case Severity::Warning:
            return "warning";
        case Severity::Note:
            return "note";
    }
    return "error";
}

Diagnostic Diagnostic::error(std::string message, Span span, std::string label) {
    return Diagnostic{Severity::Error, std::move(message), span, std::move(label), {}};
}

Diagnostic Diagnostic::warning(std::string message, Span span, std::string label) {
    return Diagnostic{Severity::Warning, std::move(message), span, std::move(label), {}};
}

Diagnostic& Diagnostic::with_note(std::string note) {
    notes.push_back(std::move(note));
    return *this;
}

std::string render(const Diagnostic& diagnostic, const SourceFile& source) {
    const Position start = source.position_of(diagnostic.span.start);
    const std::string line_number = std::to_string(start.line);
    const std::string gutter(line_number.size(), ' ');
    const std::string_view line = source.line_text(start.line);

    std::ostringstream out;
    out << severity_name(diagnostic.severity) << ": " << diagnostic.message << '\n';
    out << gutter << "--> " << source.path() << ':' << start.line << ':' << start.column << '\n';
    out << gutter << " |\n";
    out << line_number << " | " << line << '\n';
    out << gutter << " | " << caret_indent(line, start.column)
        << std::string(caret_count(line, start.column, diagnostic.span.length()), '^');
    if (!diagnostic.label.empty()) {
        out << ' ' << diagnostic.label;
    }
    out << '\n';

    for (const std::string& note : diagnostic.notes) {
        out << gutter << " = note: " << note << '\n';
    }
    return out.str();
}

std::string render_all(const std::vector<Diagnostic>& diagnostics, const SourceFile& source) {
    std::ostringstream out;
    std::size_t errors = 0;

    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        if (i > 0) {
            out << '\n';
        }
        out << render(diagnostics[i], source);
        if (diagnostics[i].severity == Severity::Error) {
            ++errors;
        }
    }

    if (errors > 0) {
        out << "\nerror: aborting due to " << errors << " previous error"
            << (errors == 1 ? "" : "s") << '\n';
    }
    return out.str();
}

}  // namespace ember::ast
