#include "cinder/manifest/toml.hpp"

#include <cctype>

namespace cinder::manifest::toml {
namespace {

using ast::Diagnostic;
using ast::Span;

bool is_key_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/// A cursor over one line, carrying the file offset of its start so
/// every span points at the right place in the whole file.
class Line {
public:
    Line(std::string_view text, std::uint32_t offset, ast::FileId file)
        : text_(text), offset_(offset), file_(file) {}

    bool done() const { return at_ >= text_.size(); }
    char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }
    char take() { return at_ < text_.size() ? text_[at_++] : '\0'; }

    void skip_spaces() {
        while (!done() && is_space(peek())) {
            ++at_;
        }
    }

    std::uint32_t here() const { return offset_ + static_cast<std::uint32_t>(at_); }

    Span from(std::uint32_t start) const { return Span{start, here(), file_}; }

    /// The rest of the line, for pointing at whatever went wrong.
    Span rest() const {
        return Span{here(), offset_ + static_cast<std::uint32_t>(text_.size()), file_};
    }

private:
    std::string_view text_;
    std::uint32_t offset_ = 0;
    ast::FileId file_ = ast::kMainFile;
    std::size_t at_ = 0;
};

}  // namespace

const Entry* Entry::find(std::string_view name) const {
    for (const Entry& entry : entries) {
        if (entry.key == name) {
            return &entry;
        }
    }
    return nullptr;
}

const Entry* Table::find(std::string_view key) const {
    for (const Entry& entry : entries) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

const Table* Document::find(std::string_view name) const {
    for (const Table& table : tables) {
        if (table.name == name) {
            return &table;
        }
    }
    return nullptr;
}

namespace {

/// Parses one line into whatever it turns out to be, appending to the
/// document and to `diagnostics`.
class Parser {
public:
    Parser(const ast::SourceFile& source, ParseResult& result)
        : source_(source), result_(result) {
        // Entries written before any `[section]` belong to the file
        // itself, which is how a lockfile's header comment and a
        // manifest's stray key both land somewhere addressable.
        result_.document.tables.push_back(Table{{}, Span{0, 0, source.id()}, {}});
    }

    void run() {
        for (std::uint32_t number = 1; number <= source_.line_count(); ++number) {
            parse_line(source_.line_text(number), line_offset(number));
        }
    }

private:
    const ast::SourceFile& source_;
    ParseResult& result_;

    /// Byte offset where a 1-based line begins.
    std::uint32_t line_offset(std::uint32_t number) const {
        const std::string_view text = source_.line_text(number);
        // `line_text` returns a view into the file's own storage, so the
        // offset is the distance from the start of the contents.
        return static_cast<std::uint32_t>(text.data() - source_.contents().data());
    }

    Diagnostic& report(std::string message, Span span, std::string label) {
        result_.diagnostics.push_back(
            Diagnostic::error(std::move(message), span, std::move(label)));
        return result_.diagnostics.back();
    }

    Table& current() { return result_.document.tables.back(); }

    void parse_line(std::string_view text, std::uint32_t offset) {
        Line line{text, offset, source_.id()};
        line.skip_spaces();
        if (line.done() || line.peek() == '#') {
            return;
        }
        if (line.peek() == '[') {
            parse_table_header(line);
            return;
        }
        parse_entry(line, current().entries, /*inline_table=*/false);
    }

    void parse_table_header(Line& line) {
        const std::uint32_t start = line.here();
        line.take();  // '['

        std::string name;
        while (!line.done() && is_key_char(line.peek())) {
            name.push_back(line.take());
        }

        if (line.done() || line.peek() != ']') {
            report("unclosed section header", line.from(start),
                   "expected `]` to close this section name");
            return;
        }
        line.take();  // ']'
        const Span span = line.from(start);

        line.skip_spaces();
        if (!line.done() && line.peek() != '#') {
            report("unexpected text after a section header", line.rest(),
                   "a section header is the whole line");
            return;
        }
        if (name.empty()) {
            report("empty section name", span, "a section needs a name, as in `[package]`");
            return;
        }
        if (result_.document.find(name) != nullptr) {
            report("duplicate section `" + name + "`", span, "this section is already open")
                .with_note("each section may appear once");
            return;
        }
        result_.document.tables.push_back(Table{name, span, {}});
    }

    /// `key = "value"` or `key = { ... }`. Returns false on an error the
    /// caller should not try to continue past.
    bool parse_entry(Line& line, std::vector<Entry>& into, bool inline_table) {
        const std::uint32_t key_start = line.here();

        std::string key;
        while (!line.done() && is_key_char(line.peek())) {
            key.push_back(line.take());
        }
        if (key.empty()) {
            report("expected a key", line.rest(),
                   "a line is either a `[section]`, a `key = \"value\"`, or a comment");
            return false;
        }
        const Span key_span = line.from(key_start);

        line.skip_spaces();
        if (line.done() || line.take() != '=') {
            report("expected `=` after `" + key + "`", line.rest(),
                   "every key needs a value");
            return false;
        }
        line.skip_spaces();

        Entry entry;
        entry.key = std::move(key);
        entry.key_span = key_span;

        if (line.peek() == '{') {
            if (inline_table) {
                report("a table cannot be nested inside a table", line.rest(),
                       "only one level of `{ ... }` is supported");
                return false;
            }
            if (!parse_inline_table(line, entry)) {
                return false;
            }
        } else if (!parse_string(line, entry.value, entry.value_span)) {
            return false;
        }

        if (!inline_table) {
            line.skip_spaces();
            if (!line.done() && line.peek() != '#') {
                report("unexpected text after a value", line.rest(),
                       "one key and one value to a line");
                return false;
            }
        }

        for (const Entry& existing : into) {
            if (existing.key == entry.key) {
                report("duplicate key `" + entry.key + "`", entry.key_span,
                       "this key already has a value");
                return false;
            }
        }
        into.push_back(std::move(entry));
        return true;
    }

    bool parse_inline_table(Line& line, Entry& entry) {
        const std::uint32_t start = line.here();
        line.take();  // '{'
        entry.is_table = true;

        line.skip_spaces();
        if (line.peek() == '}') {
            line.take();
            entry.value_span = line.from(start);
            return true;
        }

        while (true) {
            line.skip_spaces();
            if (!parse_entry(line, entry.entries, /*inline_table=*/true)) {
                return false;
            }
            line.skip_spaces();

            const char next = line.take();
            if (next == '}') {
                break;
            }
            if (next != ',') {
                report("expected `,` or `}`", line.from(line.here()),
                       "inline table entries are separated by commas");
                return false;
            }
        }
        entry.value_span = line.from(start);
        return true;
    }

    bool parse_string(Line& line, std::string& out, Span& span) {
        const std::uint32_t start = line.here();
        if (line.peek() != '"') {
            report("expected a quoted string", line.rest(),
                   "values are written in double quotes")
                .with_note("numbers, booleans and arrays are not supported here");
            return false;
        }
        line.take();

        while (!line.done() && line.peek() != '"') {
            const char c = line.take();
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            switch (const char escape = line.take()) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(escape);
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    report("unknown escape `\\" + std::string{escape} + "`",
                           line.from(line.here() - 2), "only `\\\\`, `\\\"`, `\\n` and `\\t`");
                    return false;
            }
        }

        if (line.done()) {
            report("unterminated string", line.from(start), "no closing `\"` on this line");
            return false;
        }
        line.take();  // closing quote
        span = line.from(start);
        return true;
    }
};

}  // namespace

ParseResult parse(const ast::SourceFile& source) {
    ParseResult result;
    Parser{source, result}.run();
    return result;
}

}  // namespace cinder::manifest::toml

namespace cinder::manifest::toml {

std::string quoted(std::string_view text) {
    std::string out{'"'};
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace cinder::manifest::toml
