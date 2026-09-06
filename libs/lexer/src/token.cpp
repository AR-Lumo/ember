#include "soliton/lexer/token.hpp"

#include <array>
#include <utility>

namespace soliton::lexer {
namespace {

struct KindNames {
    std::string_view snake;       // for snapshots
    std::string_view description;  // for diagnostics
};

KindNames names_for(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::IntLit:
            return {"int_lit", "an integer literal"};
        case TokenKind::FloatLit:
            return {"float_lit", "a float literal"};
        case TokenKind::StringLit:
            return {"string_lit", "a string literal"};
        case TokenKind::BoolLit:
            return {"bool_lit", "a boolean literal"};
        case TokenKind::Identifier:
            return {"ident", "an identifier"};

        case TokenKind::KwPub:
            return {"kw_pub", "`pub`"};
        case TokenKind::KwConst:
            return {"kw_const", "`const`"};
        case TokenKind::KwFn:
            return {"kw_fn", "`fn`"};
        case TokenKind::KwStruct:
            return {"kw_struct", "`struct`"};
        case TokenKind::KwImpl:
            return {"kw_impl", "`impl`"};
        case TokenKind::KwLet:
            return {"kw_let", "`let`"};
        case TokenKind::KwMut:
            return {"kw_mut", "`mut`"};
        case TokenKind::KwReturn:
            return {"kw_return", "`return`"};
        case TokenKind::KwIf:
            return {"kw_if", "`if`"};
        case TokenKind::KwElse:
            return {"kw_else", "`else`"};
        case TokenKind::KwWhile:
            return {"kw_while", "`while`"};
        case TokenKind::KwSelf:
            return {"kw_self", "`self`"};
        case TokenKind::KwAs:
            return {"kw_as", "`as`"};
        case TokenKind::KwImport:
            return {"kw_import", "`import`"};
        case TokenKind::KwRequires:
            return {"kw_requires", "`requires`"};
        case TokenKind::KwEnsures:
            return {"kw_ensures", "`ensures`"};
        case TokenKind::KwUnit:
            return {"kw_unit", "`unit`"};
        case TokenKind::KwUses:
            return {"kw_uses", "`uses`"};

        case TokenKind::KwInt:
            return {"kw_int", "`int`"};
        case TokenKind::KwFloat:
            return {"kw_float", "`float`"};
        case TokenKind::KwBool:
            return {"kw_bool", "`bool`"};
        case TokenKind::KwString:
            return {"kw_string", "`string`"};

        case TokenKind::LParen:
            return {"l_paren", "`(`"};
        case TokenKind::RParen:
            return {"r_paren", "`)`"};
        case TokenKind::LBrace:
            return {"l_brace", "`{`"};
        case TokenKind::RBrace:
            return {"r_brace", "`}`"};
        case TokenKind::LBracket:
            return {"l_bracket", "`[`"};
        case TokenKind::RBracket:
            return {"r_bracket", "`]`"};

        case TokenKind::Comma:
            return {"comma", "`,`"};
        case TokenKind::Semicolon:
            return {"semi", "`;`"};
        case TokenKind::Colon:
            return {"colon", "`:`"};
        case TokenKind::ColonColon:
            return {"colon_colon", "`::`"};
        case TokenKind::Dot:
            return {"dot", "`.`"};
        case TokenKind::Arrow:
            return {"arrow", "`->`"};

        case TokenKind::PipePipe:
            return {"pipe_pipe", "`||`"};
        case TokenKind::Pipe:
            return {"pipe", "`|`"};
        case TokenKind::AmpAmp:
            return {"amp_amp", "`&&`"};
        case TokenKind::EqEq:
            return {"eq_eq", "`==`"};
        case TokenKind::BangEq:
            return {"bang_eq", "`!=`"};
        case TokenKind::Lt:
            return {"lt", "`<`"};
        case TokenKind::Gt:
            return {"gt", "`>`"};
        case TokenKind::LtEq:
            return {"lt_eq", "`<=`"};
        case TokenKind::GtEq:
            return {"gt_eq", "`>=`"};
        case TokenKind::Plus:
            return {"plus", "`+`"};
        case TokenKind::Minus:
            return {"minus", "`-`"};
        case TokenKind::Star:
            return {"star", "`*`"};
        case TokenKind::Slash:
            return {"slash", "`/`"};
        case TokenKind::Caret:
            return {"caret", "`^`"};
        case TokenKind::Percent:
            return {"percent", "`%`"};
        case TokenKind::Bang:
            return {"bang", "`!`"};

        case TokenKind::Eq:
            return {"eq", "`=`"};
        case TokenKind::Amp:
            return {"amp", "`&`"};

        case TokenKind::Eof:
            return {"eof", "end of file"};
    }
    return {"unknown", "an unknown token"};
}

/// Keyword table. `true` and `false` are deliberately absent: the
/// grammar classifies them as bool_lit, so the lexer produces literals.
constexpr std::array<std::pair<std::string_view, TokenKind>, 22> kKeywords{{
    {"pub", TokenKind::KwPub},
    {"const", TokenKind::KwConst},
    {"fn", TokenKind::KwFn},
    {"struct", TokenKind::KwStruct},
    {"impl", TokenKind::KwImpl},
    {"let", TokenKind::KwLet},
    {"mut", TokenKind::KwMut},
    {"return", TokenKind::KwReturn},
    {"if", TokenKind::KwIf},
    {"else", TokenKind::KwElse},
    {"while", TokenKind::KwWhile},
    {"self", TokenKind::KwSelf},
    {"as", TokenKind::KwAs},
    {"import", TokenKind::KwImport},
    {"requires", TokenKind::KwRequires},
    {"ensures", TokenKind::KwEnsures},
    {"unit", TokenKind::KwUnit},
    {"uses", TokenKind::KwUses},
    {"int", TokenKind::KwInt},
    {"float", TokenKind::KwFloat},
    {"bool", TokenKind::KwBool},
    {"string", TokenKind::KwString},
}};

}  // namespace

std::string_view token_kind_name(TokenKind kind) noexcept { return names_for(kind).snake; }

std::string_view token_kind_description(TokenKind kind) noexcept {
    return names_for(kind).description;
}

std::optional<TokenKind> keyword_kind(std::string_view text) noexcept {
    for (const auto& [keyword, kind] : kKeywords) {
        if (keyword == text) {
            return kind;
        }
    }
    return std::nullopt;
}

}  // namespace soliton::lexer
