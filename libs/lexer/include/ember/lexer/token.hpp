// Tokens produced by the Ember lexer.
//
// The kind list covers every terminal in the §3 grammar and nothing
// more: v1 has no block comments, no path separator `::`, and no
// bitwise operators, so those characters are lexical errors rather than
// tokens waiting for a parser that will never accept them.

#ifndef EMBER_LEXER_TOKEN_HPP
#define EMBER_LEXER_TOKEN_HPP

#include "ember/ast/span.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace ember::lexer {

enum class TokenKind {
    // Literals and names.
    IntLit,
    FloatLit,
    StringLit,
    BoolLit,
    Identifier,

    // Keywords, in the order they appear in the grammar.
    KwPub,
    KwConst,
    KwFn,
    KwStruct,
    KwImpl,
    KwLet,
    KwMut,
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwSelf,
    /// `as`, the explicit conversion operator required by section 4.
    KwAs,

    // Primitive type names. The grammar spells these as terminals, so
    // they are reserved words rather than ordinary identifiers.
    KwInt,
    KwFloat,
    KwBool,
    KwString,

    // Delimiters.
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,

    // Punctuation.
    Comma,
    Semicolon,
    Colon,
    Dot,
    Arrow,

    // Operators, low to high precedence per §3.
    PipePipe,
    AmpAmp,
    EqEq,
    BangEq,
    Lt,
    Gt,
    LtEq,
    GtEq,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Bang,

    // Assignment and references.
    Eq,
    Amp,

    /// Synthetic end-of-file token; always the last token in a stream.
    Eof,
};

/// Stable snake_case name, used by the token-stream snapshots.
std::string_view token_kind_name(TokenKind kind) noexcept;

/// How a token is referred to inside a diagnostic, e.g. "`fn`" or
/// "an identifier". Used by the parser from Phase 2.
std::string_view token_kind_description(TokenKind kind) noexcept;

/// The keyword kind for `text`, or nullopt if it is an ordinary
/// identifier. `true` and `false` are literals, not keywords, so they
/// are not reported here.
std::optional<TokenKind> keyword_kind(std::string_view text) noexcept;

/// One token: what it is, where it came from, and its decoded value.
///
/// `text` is a view into the SourceFile that produced the token, so the
/// file must outlive the token stream. `value` holds the decoded literal:
/// the integer for IntLit, the double for FloatLit, the bool for
/// BoolLit, and the escape-resolved contents for StringLit.
struct Token {
    TokenKind kind = TokenKind::Eof;
    ast::Span span;
    std::string_view text;
    std::variant<std::monostate, std::int64_t, double, bool, std::string> value;

    bool is(TokenKind other) const noexcept { return kind == other; }

    std::int64_t int_value() const { return std::get<std::int64_t>(value); }
    double float_value() const { return std::get<double>(value); }
    bool bool_value() const { return std::get<bool>(value); }
    const std::string& string_value() const { return std::get<std::string>(value); }
};

}  // namespace ember::lexer

#endif  // EMBER_LEXER_TOKEN_HPP
