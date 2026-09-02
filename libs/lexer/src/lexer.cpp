#include "ember/lexer/lexer.hpp"

#include <array>
#include <charconv>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace ember::lexer {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_ident_continue(char c) { return is_ident_start(c) || is_digit(c); }

bool is_whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// A character as it should appear inside a diagnostic: printable ones
/// verbatim, everything else as a hex escape.
std::string describe_char(char c) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x20 && byte < 0x7F) {
        return std::string{c};
    }
    std::ostringstream out;
    out << "\\x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << static_cast<unsigned>(byte);
    return out.str();
}

/// Re-escape a decoded string so it can be shown on one line.
std::string escape_for_display(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char c : text) {
        switch (c) {
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\0':
                out += "\\0";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "\\x";
                    static constexpr char kHex[] = "0123456789ABCDEF";
                    out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
                    out.push_back(kHex[static_cast<unsigned char>(c) & 0xF]);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

/// Shortest round-trippable rendering of a double, so float snapshots
/// are stable across platforms and locales.
std::string format_double(double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        return "<unrepresentable>";
    }
    return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

std::string pad_right(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

class Lexer {
public:
    explicit Lexer(const ast::SourceFile& source)
        : source_(source), text_(source.contents()) {}

    LexResult run() {
        while (true) {
            skip_trivia();
            if (at_end()) {
                break;
            }
            lex_token();
        }
        tokens_.push_back(Token{TokenKind::Eof, ast::Span{position(), position()}, {}, {}});
        return LexResult{std::move(tokens_), std::move(diagnostics_)};
    }

private:
    const ast::SourceFile& source_;
    std::string_view text_;
    std::size_t pos_ = 0;
    std::vector<Token> tokens_;
    std::vector<ast::Diagnostic> diagnostics_;

    bool at_end() const { return pos_ >= text_.size(); }
    std::uint32_t position() const { return static_cast<std::uint32_t>(pos_); }

    char peek(std::size_t ahead = 0) const {
        const std::size_t index = pos_ + ahead;
        return index < text_.size() ? text_[index] : '\0';
    }

    char advance() { return text_[pos_++]; }

    bool match(char expected) {
        if (peek() != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

    ast::Span span_from(std::size_t start) const {
        return ast::Span{static_cast<std::uint32_t>(start), position()};
    }

    std::string_view text_from(std::size_t start) const {
        return text_.substr(start, pos_ - start);
    }

    void error(std::string message, ast::Span span, std::string label) {
        diagnostics_.push_back(
            ast::Diagnostic::error(std::move(message), span, std::move(label)));
    }

    void push(TokenKind kind, std::size_t start,
              std::variant<std::monostate, std::int64_t, double, bool, std::string> value = {}) {
        tokens_.push_back(Token{kind, span_from(start), text_from(start), std::move(value)});
    }

    /// Whitespace and comments. Both `//` and `///` are skipped: v1 has
    /// no doc tooling, and keeping doc comments out of the stream means
    /// no parser rule has to step over them.
    void skip_trivia() {
        while (!at_end()) {
            const char c = peek();
            if (is_whitespace(c)) {
                ++pos_;
            } else if (c == '/' && peek(1) == '/') {
                while (!at_end() && peek() != '\n') {
                    ++pos_;
                }
            } else {
                return;
            }
        }
    }

    void lex_token() {
        const std::size_t start = pos_;
        const char c = peek();

        if (is_ident_start(c)) {
            lex_identifier(start);
            return;
        }
        if (is_digit(c)) {
            lex_number(start);
            return;
        }
        if (c == '"') {
            lex_string(start);
            return;
        }
        lex_operator(start);
    }

    void lex_identifier(std::size_t start) {
        while (!at_end() && is_ident_continue(peek())) {
            ++pos_;
        }
        const std::string_view word = text_from(start);

        if (word == "true" || word == "false") {
            push(TokenKind::BoolLit, start, word == "true");
            return;
        }
        if (const std::optional<TokenKind> keyword = keyword_kind(word)) {
            push(*keyword, start);
            return;
        }
        push(TokenKind::Identifier, start);
    }

    /// Decimal integers and floats, with Rust-style `_` separators
    /// (§9: ergonomics follow Rust). A float needs a digit on both sides
    /// of the dot, so `1.foo()` stays an integer followed by a method
    /// call rather than becoming an ambiguous `1.` literal.
    void lex_number(std::size_t start) {
        bool is_float = false;
        consume_digits();

        if (peek() == '.' && is_digit(peek(1))) {
            is_float = true;
            ++pos_;
            consume_digits();
        }

        if ((peek() == 'e' || peek() == 'E') && has_exponent_digits()) {
            is_float = true;
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            consume_digits();
        }

        const std::string_view raw = text_from(start);

        // A letter or `_` glued to the end of a number is never valid:
        // v1 has no literal suffixes, so `1abc` and `1e` land here.
        if (!at_end() && is_ident_continue(peek())) {
            const std::size_t suffix_start = pos_;
            while (!at_end() && is_ident_continue(peek())) {
                ++pos_;
            }
            error("invalid suffix on numeric literal",
                  ast::Span{static_cast<std::uint32_t>(suffix_start), position()},
                  "`" + std::string{text_.substr(suffix_start, pos_ - suffix_start)} +
                      "` is not a valid literal suffix");
            return;
        }

        std::string cleaned;
        cleaned.reserve(raw.size());
        for (const char c : raw) {
            if (c != '_') {
                cleaned.push_back(c);
            }
        }

        if (is_float) {
            double value = 0.0;
            const auto result =
                std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), value);
            if (result.ec != std::errc{}) {
                error("float literal out of range", span_from(start),
                      "`float` values must fit in a 64-bit IEEE 754 double");
                return;
            }
            push(TokenKind::FloatLit, start, value);
            return;
        }

        std::int64_t value = 0;
        const auto result =
            std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), value);
        if (result.ec == std::errc::result_out_of_range) {
            // Note that `-9223372036854775808` is unary minus applied to a
            // literal that is itself out of range, exactly as in C.
            error("integer literal out of range", span_from(start),
                  "`int` values must fit in a signed 64-bit integer");
            return;
        }
        if (result.ec != std::errc{}) {
            error("malformed integer literal", span_from(start), "cannot parse this as an `int`");
            return;
        }
        push(TokenKind::IntLit, start, value);
    }

    void consume_digits() {
        while (!at_end() && (is_digit(peek()) || peek() == '_')) {
            ++pos_;
        }
    }

    /// True when `e`/`E` at the cursor really begins an exponent, so that
    /// `1e` is reported as a bad suffix rather than silently truncated.
    bool has_exponent_digits() const {
        std::size_t ahead = 1;
        if (peek(ahead) == '+' || peek(ahead) == '-') {
            ++ahead;
        }
        return is_digit(peek(ahead));
    }

    /// A double-quoted string. Newlines may not appear inside one: an
    /// unterminated literal would otherwise swallow the rest of the file
    /// and report its error somewhere useless.
    void lex_string(std::size_t start) {
        ++pos_;  // opening quote
        std::string value;

        while (true) {
            if (at_end() || peek() == '\n') {
                error("unterminated string literal", ast::Span::at(static_cast<std::uint32_t>(start)),
                      "this string literal is never closed");
                push(TokenKind::StringLit, start, std::move(value));
                return;
            }
            const char c = advance();
            if (c == '"') {
                break;
            }
            if (c != '\\') {
                value.push_back(c);
                continue;
            }

            const std::size_t escape_start = pos_ - 1;
            if (at_end() || peek() == '\n') {
                continue;  // reported as unterminated on the next iteration
            }
            switch (const char escaped = advance()) {
                case 'n':
                    value.push_back('\n');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case '0':
                    value.push_back('\0');
                    break;
                case '\\':
                    value.push_back('\\');
                    break;
                case '"':
                    value.push_back('"');
                    break;
                default:
                    error("unknown escape sequence",
                          ast::Span{static_cast<std::uint32_t>(escape_start), position()},
                          "`\\" + describe_char(escaped) +
                              "` is not a recognized escape sequence");
                    value.push_back(escaped);
                    break;
            }
        }

        push(TokenKind::StringLit, start, std::move(value));
    }

    void lex_operator(std::size_t start) {
        const char c = advance();
        switch (c) {
            case '(':
                return push(TokenKind::LParen, start);
            case ')':
                return push(TokenKind::RParen, start);
            case '{':
                return push(TokenKind::LBrace, start);
            case '}':
                return push(TokenKind::RBrace, start);
            case '[':
                return push(TokenKind::LBracket, start);
            case ']':
                return push(TokenKind::RBracket, start);
            case ',':
                return push(TokenKind::Comma, start);
            case ';':
                return push(TokenKind::Semicolon, start);
            case ':':
                return push(TokenKind::Colon, start);
            case '.':
                return push(TokenKind::Dot, start);
            case '+':
                return push(TokenKind::Plus, start);
            case '*':
                return push(TokenKind::Star, start);
            case '/':
                return push(TokenKind::Slash, start);
            case '%':
                return push(TokenKind::Percent, start);
            case '-':
                return push(match('>') ? TokenKind::Arrow : TokenKind::Minus, start);
            case '=':
                return push(match('=') ? TokenKind::EqEq : TokenKind::Eq, start);
            case '!':
                return push(match('=') ? TokenKind::BangEq : TokenKind::Bang, start);
            case '<':
                return push(match('=') ? TokenKind::LtEq : TokenKind::Lt, start);
            case '>':
                return push(match('=') ? TokenKind::GtEq : TokenKind::Gt, start);
            case '&':
                return push(match('&') ? TokenKind::AmpAmp : TokenKind::Amp, start);
            case '|':
                if (match('|')) {
                    return push(TokenKind::PipePipe, start);
                }
                error("unexpected character", span_from(start),
                      "`|` is not an operator in Ember; did you mean `||`?");
                return;
            default:
                error("unexpected character", span_from(start),
                      "`" + describe_char(c) + "` is not valid in Ember source");
                return;
        }
    }
};

}  // namespace

std::string_view stage_name() noexcept { return "lexer"; }

LexResult tokenize(const ast::SourceFile& source) { return Lexer{source}.run(); }

std::string dump_tokens(const std::vector<Token>& tokens, const ast::SourceFile& source) {
    std::ostringstream out;

    for (const Token& token : tokens) {
        const ast::Position start = source.position_of(token.span.start);
        const ast::Position end = source.position_of(token.span.end);

        std::ostringstream location;
        location << start.line << ':' << start.column << '-' << end.line << ':' << end.column;

        out << pad_right(location.str(), 12) << ' '
            << pad_right(std::string{token_kind_name(token.kind)}, 11) << ' ' << token.text;

        switch (token.kind) {
            case TokenKind::IntLit:
                out << "  = " << token.int_value();
                break;
            case TokenKind::FloatLit:
                out << "  = " << format_double(token.float_value());
                break;
            case TokenKind::BoolLit:
                out << "  = " << (token.bool_value() ? "true" : "false");
                break;
            case TokenKind::StringLit:
                out << "  = \"" << escape_for_display(token.string_value()) << "\" ("
                    << token.string_value().size() << " bytes)";
                break;
            default:
                break;
        }
        out << '\n';
    }

    return out.str();
}

}  // namespace ember::lexer
