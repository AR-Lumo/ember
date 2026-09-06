#include "ember/ast/span.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace ember::ast {

SourceFile::SourceFile(std::string path, std::string contents, FileId id)
    : path_(std::move(path)), contents_(std::move(contents)), id_(id) {
    // A UTF-8 byte order mark is not part of the program.
    //
    // Windows editors write one by default - Notepad does, and so does
    // PowerShell's `Out-File -Encoding utf8` - so the first file a lot
    // of people save is one Ember used to reject with three
    // "`\xEF` is not valid in Ember source" errors before reaching a
    // single token. Every other compiler skips it; so does this one.
    //
    // Dropped here rather than in the lexer, and before `line_starts_`
    // is built, so that offsets count from the first real character:
    // skipping it later would leave every column on line 1 reported
    // three too high.
    constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
    if (contents_.rfind(kUtf8Bom, 0) == 0) {
        contents_.erase(0, kUtf8Bom.size());
    }

    line_starts_.push_back(0);
    for (std::uint32_t offset = 0; offset < size(); ++offset) {
        if (contents_[offset] == '\n') {
            line_starts_.push_back(offset + 1);
        }
    }
}

std::optional<SourceFile> SourceFile::load(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return SourceFile{path.string(), buffer.str()};
}

Position SourceFile::position_of(std::uint32_t offset) const {
    offset = std::min(offset, size());

    // The first line start strictly greater than `offset` sits one past
    // the line we want.
    const auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    const auto index = static_cast<std::uint32_t>(std::distance(line_starts_.begin(), it) - 1);

    Position position;
    position.line = index + 1;
    position.column = offset - line_starts_[index] + 1;
    return position;
}

std::string_view SourceFile::line_text(std::uint32_t line) const {
    if (line == 0 || line > line_count()) {
        return {};
    }
    const std::uint32_t start = line_starts_[line - 1];
    const std::uint32_t end = (line < line_count()) ? line_starts_[line] : size();

    std::string_view text{contents_};
    text = text.substr(start, end - start);
    // Trim the line terminator, in either encoding.
    if (!text.empty() && text.back() == '\n') {
        text.remove_suffix(1);
    }
    if (!text.empty() && text.back() == '\r') {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view SourceFile::text_of(Span span) const {
    const std::uint32_t start = std::min(span.start, size());
    const std::uint32_t end = std::clamp(span.end, start, size());
    return std::string_view{contents_}.substr(start, end - start);
}

FileId SourceMap::add(std::string path, std::string contents) {
    const auto id = static_cast<FileId>(files_.size());
    files_.emplace_back(std::move(path), std::move(contents), id);
    return id;
}

const SourceFile& SourceMap::file(FileId id) const {
    return files_[id < files_.size() ? id : 0];
}

const SourceFile* SourceMap::find(std::string_view path) const {
    for (const SourceFile& candidate : files_) {
        if (candidate.path() == path) {
            return &candidate;
        }
    }
    return nullptr;
}

}  // namespace ember::ast
