// Source positions and spans.
//
// Every token (Phase 1) and every AST node (Phase 2) carries a Span, so
// the §7 error format is available to every stage without retrofitting.
//
// A Span stores byte offsets rather than line/column pairs: offsets are
// cheap to produce and merge while lexing, and SourceFile converts them
// to human-facing line/column only when a diagnostic is actually
// rendered. This is the same trade-off rustc and clang make.

#ifndef EMBER_AST_SPAN_HPP
#define EMBER_AST_SPAN_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ember::ast {

/// A 1-based line/column pair, as a human would cite it.
///
/// `column` counts bytes, not Unicode code points: v1 is ASCII-oriented,
/// and counting bytes is what C would do. Non-ASCII source will still
/// compile, but carets under multi-byte characters may sit slightly off.
struct Position {
    std::uint32_t line = 1;
    std::uint32_t column = 1;

    friend bool operator==(const Position&, const Position&) = default;
};

/// A half-open byte range `[start, end)` within one source file.
struct Span {
    std::uint32_t start = 0;
    std::uint32_t end = 0;

    /// A span covering `length` bytes starting at `offset`.
    static constexpr Span at(std::uint32_t offset, std::uint32_t length = 1) {
        return Span{offset, offset + length};
    }

    constexpr std::uint32_t length() const noexcept { return end - start; }
    constexpr bool empty() const noexcept { return start == end; }

    /// The smallest span covering both `*this` and `other`. Used to give a
    /// binary expression a span running from its left operand to its right.
    constexpr Span merge(const Span& other) const noexcept {
        return Span{start < other.start ? start : other.start,
                    end > other.end ? end : other.end};
    }

    friend bool operator==(const Span&, const Span&) = default;
};

/// One source file, plus the line index needed to resolve spans.
class SourceFile {
public:
    SourceFile(std::string path, std::string contents);

    /// Read a file from disk. Returns nullopt if it cannot be opened.
    static std::optional<SourceFile> load(const std::filesystem::path& path);

    const std::string& path() const noexcept { return path_; }
    const std::string& contents() const noexcept { return contents_; }
    std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(contents_.size()); }

    /// Number of lines. A file always has at least one line, even if empty.
    std::uint32_t line_count() const noexcept {
        return static_cast<std::uint32_t>(line_starts_.size());
    }

    /// Line and column of a byte offset. Offsets past the end clamp to the
    /// end of the file so an error at EOF still renders somewhere sane.
    Position position_of(std::uint32_t offset) const;

    /// Text of a 1-based line, without its trailing newline. Out-of-range
    /// lines yield an empty view.
    std::string_view line_text(std::uint32_t line) const;

    /// The source text a span covers.
    std::string_view text_of(Span span) const;

private:
    std::string path_;
    std::string contents_;
    /// Byte offset where each line begins; always starts with 0.
    std::vector<std::uint32_t> line_starts_;
};

}  // namespace ember::ast

#endif  // EMBER_AST_SPAN_HPP
