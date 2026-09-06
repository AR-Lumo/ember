// Source positions and spans.
//
// Every token (Phase 1) and every AST node (Phase 2) carries a Span, so
// the §7 error format is available to every stage without retrofitting.
//
// A Span stores byte offsets rather than line/column pairs: offsets are
// cheap to produce and merge while lexing, and SourceFile converts them
// to human-facing line/column only when a diagnostic is actually
// rendered. This is the same trade-off rustc and clang make.

#ifndef SOLITON_AST_SPAN_HPP
#define SOLITON_AST_SPAN_HPP

#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace soliton::ast {

/// Which file a span points into.
///
/// Before modules a span was just a byte range: there was only ever one
/// file, and it was passed alongside. With `import` a diagnostic can
/// point at any file in the program, so the span has to carry that
/// itself.
using FileId = std::uint32_t;

/// The file every single-file program uses, and the default for a span
/// that was never stamped.
inline constexpr FileId kMainFile = 0;

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
    FileId file = kMainFile;

    /// A span covering `length` bytes starting at `offset`.
    static constexpr Span at(std::uint32_t offset, std::uint32_t length = 1,
                             FileId file = kMainFile) {
        return Span{offset, offset + length, file};
    }

    constexpr std::uint32_t length() const noexcept { return end - start; }
    constexpr bool empty() const noexcept { return start == end; }

    /// The smallest span covering both `*this` and `other`. Used to give a
    /// binary expression a span running from its left operand to its right.
    ///
    /// Merging across files is meaningless, so the left span's file
    /// wins; in practice the two are always from the same file, since a
    /// single expression cannot straddle an `import`.
    constexpr Span merge(const Span& other) const noexcept {
        return Span{start < other.start ? start : other.start,
                    end > other.end ? end : other.end, file};
    }

    friend bool operator==(const Span&, const Span&) = default;
};

/// One source file, plus the line index needed to resolve spans.
class SourceFile {
public:
    SourceFile(std::string path, std::string contents, FileId id = kMainFile);

    /// Read a file from disk. Returns nullopt if it cannot be opened.
    static std::optional<SourceFile> load(const std::filesystem::path& path);

    /// This file's identity, as stamped onto every span it produces.
    FileId id() const noexcept { return id_; }

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
    FileId id_ = kMainFile;
    /// Byte offset where each line begins; always starts with 0.
    std::vector<std::uint32_t> line_starts_;
};

/// Every file in one compilation, so a span can be resolved back to the
/// text it came from no matter which module produced it.
class SourceMap {
public:
    /// Adds a file and returns its id. The map owns it from here.
    FileId add(std::string path, std::string contents);

    /// The file `id` refers to. Ids are only ever handed out by `add`,
    /// so an unknown one is a bug rather than user error; it resolves to
    /// the first file rather than crashing a diagnostic.
    const SourceFile& file(FileId id) const;

    /// The file previously added under `path`, if any. Used to keep an
    /// import cycle from loading the same file twice.
    const SourceFile* find(std::string_view path) const;

    std::size_t size() const noexcept { return files_.size(); }
    bool empty() const noexcept { return files_.empty(); }

private:
    /// A deque, not a vector: references handed out stay valid as more
    /// modules are loaded during resolution.
    std::deque<SourceFile> files_;
};

}  // namespace soliton::ast

#endif  // SOLITON_AST_SPAN_HPP
