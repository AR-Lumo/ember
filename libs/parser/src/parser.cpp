#include "ember/parser/parser.hpp"

#include "ember/ast/ast.hpp"
#include "ember/lexer/lexer.hpp"

#include <algorithm>
#include <deque>
#include <optional>
#include <fstream>
#include <sstream>


#include <string>
#include <utility>

namespace ember::parser {
namespace {

using ast::Span;
using lexer::Token;
using lexer::TokenKind;

/// Left binding power of a binary operator, following the §3 table.
/// Zero means "not a binary operator". Higher binds tighter.
int binary_precedence(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::PipePipe:
            return 1;
        case TokenKind::AmpAmp:
            return 2;
        case TokenKind::EqEq:
        case TokenKind::BangEq:
            return 3;
        case TokenKind::Lt:
        case TokenKind::Gt:
        case TokenKind::LtEq:
        case TokenKind::GtEq:
            return 4;
        case TokenKind::Plus:
        case TokenKind::Minus:
            return 5;
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
            return 6;
        default:
            return 0;
    }
}

ast::BinaryOp binary_op_of(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::PipePipe:
            return ast::BinaryOp::Or;
        case TokenKind::AmpAmp:
            return ast::BinaryOp::And;
        case TokenKind::EqEq:
            return ast::BinaryOp::Equal;
        case TokenKind::BangEq:
            return ast::BinaryOp::NotEqual;
        case TokenKind::Lt:
            return ast::BinaryOp::Less;
        case TokenKind::Gt:
            return ast::BinaryOp::Greater;
        case TokenKind::LtEq:
            return ast::BinaryOp::LessEq;
        case TokenKind::GtEq:
            return ast::BinaryOp::GreaterEq;
        case TokenKind::Plus:
            return ast::BinaryOp::Add;
        case TokenKind::Minus:
            return ast::BinaryOp::Subtract;
        case TokenKind::Star:
            return ast::BinaryOp::Multiply;
        case TokenKind::Slash:
            return ast::BinaryOp::Divide;
        default:
            return ast::BinaryOp::Remainder;
    }
}

/// Thrown to unwind to the nearest recovery point. The diagnostic has
/// already been recorded by the time this is thrown; it carries no
/// payload of its own.
struct ParseError {};

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    ParseResult run() {
        auto program = std::make_unique<ast::Program>();

        while (!at_end()) {
            try {
                program->items.push_back(parse_item());
            } catch (const ParseError&) {
                recover_to_item();
            }
        }

        return ParseResult{std::move(program), std::move(diagnostics_)};
    }

private:
    const std::vector<Token>& tokens_;
    std::size_t index_ = 0;
    std::vector<ast::Diagnostic> diagnostics_;

    // -----------------------------------------------------------------
    // Token access
    // -----------------------------------------------------------------

    const Token& peek(std::size_t ahead = 0) const {
        const std::size_t i = index_ + ahead;
        return tokens_[i < tokens_.size() ? i : tokens_.size() - 1];
    }

    bool at_end() const { return peek().kind == TokenKind::Eof; }

    bool check(TokenKind kind) const { return peek().kind == kind; }

    const Token& advance() {
        const std::size_t current = index_;
        if (!at_end()) {
            ++index_;
        }
        return tokens_[current];
    }

    bool match(TokenKind kind) {
        if (!check(kind)) {
            return false;
        }
        advance();
        return true;
    }

    /// Consume a token of `kind`, or report and unwind.
    const Token& expect(TokenKind kind) {
        if (check(kind)) {
            return advance();
        }
        throw error_here("expected " + std::string{lexer::token_kind_description(kind)});
    }

    // -----------------------------------------------------------------
    // Diagnostics and recovery
    // -----------------------------------------------------------------

    /// Record a diagnostic whose headline names what was expected and
    /// what turned up instead, with the shorter form under the caret.
    ParseError error_here(const std::string& expectation) {
        const Token& token = peek();
        const std::string found{lexer::token_kind_description(token.kind)};
        diagnostics_.push_back(ast::Diagnostic::error(
            expectation + ", found " + found, caret_span(token), expectation));
        return ParseError{};
    }

    ParseError error_at(Span span, std::string message, std::string label) {
        diagnostics_.push_back(
            ast::Diagnostic::error(std::move(message), span, std::move(label)));
        return ParseError{};
    }

    /// An Eof token is empty, which would render as a caret over nothing.
    /// Give it one column so the diagnostic still points somewhere.
    static Span caret_span(const Token& token) {
        return token.span.empty() ? Span::at(token.span.start) : token.span;
    }

    /// Panic-mode recovery: skip to something that plausibly starts the
    /// next item, so one broken function does not cascade into the rest
    /// of the file.
    void recover_to_item() {
        while (!at_end()) {
            switch (peek().kind) {
                case TokenKind::KwPub:
                case TokenKind::KwFn:
                case TokenKind::KwStruct:
                case TokenKind::KwImpl:
                case TokenKind::KwConst:
                case TokenKind::KwImport:
                    return;
                default:
                    advance();
                    break;
            }
        }
    }

    /// Recovery inside a block: stop after a `;`, or before a `}` or
    /// anything that starts a fresh statement.
    void recover_to_statement() {
        while (!at_end()) {
            if (peek().kind == TokenKind::Semicolon) {
                advance();
                return;
            }
            switch (peek().kind) {
                case TokenKind::RBrace:
                case TokenKind::KwLet:
                case TokenKind::KwReturn:
                case TokenKind::KwIf:
                case TokenKind::KwWhile:
                    return;
                default:
                    advance();
                    break;
            }
        }
    }

    // -----------------------------------------------------------------
    // Items
    // -----------------------------------------------------------------

    ast::ItemPtr parse_item() {
        const Span start = peek().span;
        const bool is_public = match(TokenKind::KwPub);

        if (check(TokenKind::KwFn)) {
            return parse_function(start, is_public, {});
        }
        if (check(TokenKind::KwStruct)) {
            return parse_struct(start, is_public);
        }
        if (check(TokenKind::KwConst)) {
            return parse_const(start, is_public);
        }
        if (check(TokenKind::KwImport)) {
            if (is_public) {
                // An import is not a declaration others can reach; it
                // names what *this* file uses.
                throw error_at(start, "`import` cannot be `pub`",
                               "an import is private to the file that writes it");
            }
            return parse_import(start);
        }
        if (check(TokenKind::KwImpl)) {
            if (is_public) {
                // `impl` has no visibility in the grammar. Saying so
                // beats a bare "expected an item" pointing at `impl`.
                throw error_at(start, "`impl` blocks cannot be `pub`",
                               "visibility is declared on the methods inside");
            }
            return parse_impl(start);
        }

        throw error_here("expected an item");
    }

    /// `<T, U>` after a declaration's name. Empty when there is no `<`.
    ///
    /// Only ever parsed straight after the name of a `fn`, `struct` or
    /// `impl`, where a `<` cannot be a comparison, so there is no
    /// ambiguity to resolve here.
    std::vector<ast::GenericParam> parse_generic_params() {
        std::vector<ast::GenericParam> params;
        if (!match(TokenKind::Lt)) {
            return params;
        }

        if (check(TokenKind::Gt)) {
            throw error_at(peek().span, "empty type parameter list",
                           "write the parameters, as in `<T>`, or drop the `<>`");
        }

        do {
            const Token& name = expect(TokenKind::Identifier);
            params.push_back(ast::GenericParam{std::string{name.text}, name.span});
        } while (match(TokenKind::Comma));

        expect(TokenKind::Gt);
        return params;
    }

    std::unique_ptr<ast::FunctionDecl> parse_function(Span start, bool is_public,
                                                      std::string owner_type) {
        expect(TokenKind::KwFn);
        const Token& name = expect(TokenKind::Identifier);

        auto function = std::make_unique<ast::FunctionDecl>(start, is_public,
                                                            std::string{name.text}, name.span);
        function->owner_type = std::move(owner_type);
        function->generic_params = parse_generic_params();

        expect(TokenKind::LParen);
        if (!check(TokenKind::RParen)) {
            do {
                function->params.push_back(parse_param(function->params.empty()));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen);

        if (match(TokenKind::Arrow)) {
            function->return_type = parse_type();
        }

        function->body = parse_block();
        function->span = start.merge(function->body.span);
        return function;
    }

    /// `param = ( "self" | "&self" ) | identifier ":" type`. A self
    /// parameter is only legal first; anywhere else it is reported as
    /// such rather than as a generic syntax error.
    ast::Param parse_param(bool is_first) {
        const Span start = peek().span;

        if (check(TokenKind::KwSelf) ||
            (check(TokenKind::Amp) && peek(1).kind == TokenKind::KwSelf)) {
            const bool by_reference = match(TokenKind::Amp);
            const Token& self = expect(TokenKind::KwSelf);

            if (!is_first) {
                throw error_at(start.merge(self.span),
                               "`self` must be the first parameter",
                               "only the receiver can be `self`");
            }

            ast::Param param;
            param.span = start.merge(self.span);
            param.self_kind = by_reference ? ast::SelfKind::Reference : ast::SelfKind::Value;
            param.name_span = self.span;
            return param;
        }

        const Token& name = expect(TokenKind::Identifier);
        expect(TokenKind::Colon);

        ast::Param param;
        param.name = std::string{name.text};
        param.name_span = name.span;
        param.type = parse_type();
        param.span = start.merge(param.type->span);
        return param;
    }

    ast::ItemPtr parse_struct(Span start, bool is_public) {
        expect(TokenKind::KwStruct);
        const Token& name = expect(TokenKind::Identifier);

        auto declaration = std::make_unique<ast::StructDecl>(start, is_public,
                                                             std::string{name.text}, name.span);
        declaration->generic_params = parse_generic_params();
        expect(TokenKind::LBrace);

        while (!check(TokenKind::RBrace) && !at_end()) {
            ast::FieldDecl field;
            const Span field_start = peek().span;
            field.is_public = match(TokenKind::KwPub);

            const Token& field_name = expect(TokenKind::Identifier);
            field.name = std::string{field_name.text};
            field.name_span = field_name.span;

            expect(TokenKind::Colon);
            field.type = parse_type();
            field.span = field_start.merge(field.type->span);

            // §3 makes the trailing comma part of `field`, so it is
            // required after every field including the last.
            expect(TokenKind::Comma);
            declaration->fields.push_back(std::move(field));
        }

        const Token& close = expect(TokenKind::RBrace);
        declaration->span = start.merge(close.span);
        return declaration;
    }

    ast::ItemPtr parse_impl(Span start) {
        expect(TokenKind::KwImpl);
        // `impl<T> Pair<T>`: the parameters are bound before the type,
        // because the type refers to them.
        std::vector<ast::GenericParam> generic_params = parse_generic_params();

        const Token& name = expect(TokenKind::Identifier);
        auto block = std::make_unique<ast::ImplBlock>(start, std::string{name.text}, name.span);
        block->generic_params = std::move(generic_params);

        // `impl<T> Pair<T>` repeats the parameters as arguments. They
        // have to match what was just bound, so they are parsed and
        // checked rather than stored.
        if (check(TokenKind::Lt)) {
            const std::vector<ast::GenericParam> applied = parse_generic_params();
            if (applied.size() != block->generic_params.size()) {
                throw error_at(name.span,
                               "`impl` type arguments do not match its parameters",
                               "`" + block->type_name + "` is applied to " +
                                   std::to_string(applied.size()) + " argument" +
                                   (applied.size() == 1 ? "" : "s") + " but " +
                                   std::to_string(block->generic_params.size()) +
                                   " were declared");
            }
            for (std::size_t i = 0; i < applied.size(); ++i) {
                if (applied[i].name != block->generic_params[i].name) {
                    throw error_at(applied[i].span, "unknown type parameter `" +
                                                        applied[i].name + "`",
                                   "declare it in the `impl` parameter list");
                }
            }
        }

        expect(TokenKind::LBrace);

        while (!check(TokenKind::RBrace) && !at_end()) {
            const Span method_start = peek().span;
            const bool is_public = match(TokenKind::KwPub);
            block->methods.push_back(
                parse_function(method_start, is_public, block->type_name));
        }

        const Token& close = expect(TokenKind::RBrace);
        block->span = start.merge(close.span);
        return block;
    }

    /// `import geometry;`
    ast::ItemPtr parse_import(Span start) {
        expect(TokenKind::KwImport);
        const Token& name = expect(TokenKind::Identifier);
        const Token& semi = expect(TokenKind::Semicolon);
        return std::make_unique<ast::ImportDecl>(start.merge(semi.span),
                                                 std::string{name.text}, name.span);
    }

    ast::ItemPtr parse_const(Span start, bool is_public) {
        expect(TokenKind::KwConst);
        const Token& name = expect(TokenKind::Identifier);

        auto declaration = std::make_unique<ast::ConstDecl>(start, is_public,
                                                            std::string{name.text}, name.span);
        expect(TokenKind::Colon);
        declaration->type = parse_type();
        expect(TokenKind::Eq);
        declaration->value = parse_expr();

        const Token& semi = expect(TokenKind::Semicolon);
        declaration->span = start.merge(semi.span);
        return declaration;
    }

    // -----------------------------------------------------------------
    // Types
    // -----------------------------------------------------------------

    ast::TypeRefPtr parse_type() {
        const Span start = peek().span;
        auto type = std::make_unique<ast::TypeRef>();
        type->span = start;

        switch (peek().kind) {
            case TokenKind::KwInt:
                advance();
                type->kind = ast::TypeKind::Int;
                return type;
            case TokenKind::KwFloat:
                advance();
                type->kind = ast::TypeKind::Float;
                return type;
            case TokenKind::KwBool:
                advance();
                type->kind = ast::TypeKind::Bool;
                return type;
            case TokenKind::KwString:
                advance();
                type->kind = ast::TypeKind::String;
                return type;

            case TokenKind::Identifier: {
                const Token* name = &advance();
                type->kind = ast::TypeKind::Named;

                // `geometry::Point`: the first identifier is the module.
                if (match(TokenKind::ColonColon)) {
                    type->module = std::string{name->text};
                    name = &expect(TokenKind::Identifier);
                }
                type->name = std::string{name->text};

                // `Pair<int, float>`. Type arguments only appear in type
                // position; in an expression a struct literal infers
                // them from its fields, which keeps `<` unambiguous.
                if (match(TokenKind::Lt)) {
                    do {
                        type->type_args.push_back(parse_type());
                    } while (match(TokenKind::Comma));
                    const Token& close = expect(TokenKind::Gt);
                    type->span = start.merge(close.span);
                } else {
                    type->span = start.merge(name->span);
                }
                return type;
            }

            case TokenKind::Amp: {
                advance();
                type->kind = ast::TypeKind::Reference;
                type->element = parse_type();
                type->span = start.merge(type->element->span);
                return type;
            }

            case TokenKind::LBracket: {
                advance();
                type->kind = ast::TypeKind::Array;
                type->element = parse_type();
                expect(TokenKind::Semicolon);

                if (!check(TokenKind::IntLit)) {
                    throw error_here("expected an array length");
                }
                type->length = advance().int_value();

                const Token& close = expect(TokenKind::RBracket);
                type->span = start.merge(close.span);
                return type;
            }

            default:
                throw error_here("expected a type");
        }
    }

    // -----------------------------------------------------------------
    // Statements
    // -----------------------------------------------------------------

    ast::Block parse_block() {
        const Token& open = expect(TokenKind::LBrace);
        ast::Block block;

        while (!check(TokenKind::RBrace) && !at_end()) {
            try {
                block.statements.push_back(parse_statement());
            } catch (const ParseError&) {
                recover_to_statement();
            }
        }

        const Token& close = expect(TokenKind::RBrace);
        block.span = open.span.merge(close.span);
        return block;
    }

    ast::StmtPtr parse_statement() {
        switch (peek().kind) {
            case TokenKind::KwLet:
                return parse_let();
            case TokenKind::KwReturn:
                return parse_return();
            case TokenKind::KwIf:
                return parse_if();
            case TokenKind::KwWhile:
                return parse_while();
            default:
                return parse_expr_or_assign();
        }
    }

    ast::StmtPtr parse_let() {
        const Span start = expect(TokenKind::KwLet).span;
        const bool is_mutable = match(TokenKind::KwMut);
        const Token& name = expect(TokenKind::Identifier);

        auto statement = std::make_unique<ast::LetStmt>(start, is_mutable,
                                                        std::string{name.text}, name.span);
        if (match(TokenKind::Colon)) {
            statement->declared_type = parse_type();
        }

        expect(TokenKind::Eq);
        statement->value = parse_expr();

        const Token& semi = expect(TokenKind::Semicolon);
        statement->span = start.merge(semi.span);
        return statement;
    }

    ast::StmtPtr parse_return() {
        const Span start = expect(TokenKind::KwReturn).span;
        auto statement = std::make_unique<ast::ReturnStmt>(start);

        if (!check(TokenKind::Semicolon)) {
            statement->value = parse_expr();
        }

        const Token& semi = expect(TokenKind::Semicolon);
        statement->span = start.merge(semi.span);
        return statement;
    }

    ast::StmtPtr parse_if() {
        const Span start = expect(TokenKind::KwIf).span;
        auto statement = std::make_unique<ast::IfStmt>(start);

        statement->condition = parse_condition();
        statement->then_block = parse_block();
        Span end = statement->then_block.span;

        if (match(TokenKind::KwElse)) {
            if (check(TokenKind::KwIf)) {
                statement->else_branch = parse_if();
                end = statement->else_branch->span;
            } else {
                ast::Block block = parse_block();
                end = block.span;
                statement->else_branch = std::make_unique<ast::BlockStmt>(end, std::move(block));
            }
        }

        statement->span = start.merge(end);
        return statement;
    }

    ast::StmtPtr parse_while() {
        const Span start = expect(TokenKind::KwWhile).span;
        auto statement = std::make_unique<ast::WhileStmt>(start);

        statement->condition = parse_condition();
        statement->body = parse_block();
        statement->span = start.merge(statement->body.span);
        return statement;
    }

    /// The condition of an `if` or `while`, where a `{` must start the
    /// body rather than a struct literal.
    ast::ExprPtr parse_condition() { return parse_expr(0, false); }

    /// Either `expr;` or `lvalue = expr;`. Both start with an
    /// expression, so the two are told apart by what follows it.
    ast::StmtPtr parse_expr_or_assign() {
        const Span start = peek().span;
        ast::ExprPtr expr = parse_expr();

        if (match(TokenKind::Eq)) {
            ast::ExprPtr value = parse_expr();
            const Token& semi = expect(TokenKind::Semicolon);
            return std::make_unique<ast::AssignStmt>(start.merge(semi.span), std::move(expr),
                                                     std::move(value));
        }

        const Token& semi = expect(TokenKind::Semicolon);
        return std::make_unique<ast::ExprStmt>(start.merge(semi.span), std::move(expr));
    }

    // -----------------------------------------------------------------
    // Expressions (Pratt)
    // -----------------------------------------------------------------

    ast::ExprPtr parse_expr(int min_precedence = 0, bool allow_struct_literal = true) {
        ast::ExprPtr left = parse_cast(allow_struct_literal);

        while (true) {
            const int precedence = binary_precedence(peek().kind);
            // Left-associative: an operator of equal precedence belongs
            // to the loop, not to the recursive call.
            if (precedence == 0 || precedence <= min_precedence) {
                break;
            }

            const Token& op = advance();
            ast::ExprPtr right = parse_expr(precedence, allow_struct_literal);
            // (the right operand is itself a cast-level expression)
            const Span span = left->span.merge(right->span);
            left = std::make_unique<ast::BinaryExpr>(span, binary_op_of(op.kind),
                                                     std::move(left), std::move(right));
        }

        return left;
    }

    /// `unary_expr { "as" type }`.
    ///
    /// `as` sits between unary and multiplicative, following Rust: it
    /// binds tighter than every binary operator, so `a as float * b` is
    /// `(a as float) * b`, and looser than unary, so `-x as float` is
    /// `(-x) as float`. Chaining left-associates: `x as float as int`.
    ast::ExprPtr parse_cast(bool allow_struct_literal) {
        ast::ExprPtr expr = parse_unary(allow_struct_literal);

        while (check(TokenKind::KwAs)) {
            advance();
            ast::TypeRefPtr target = parse_type();
            const Span span = expr->span.merge(target->span);
            expr = std::make_unique<ast::CastExpr>(span, std::move(expr), std::move(target));
        }
        return expr;
    }

    ast::ExprPtr parse_unary(bool allow_struct_literal) {
        if (check(TokenKind::Minus) || check(TokenKind::Bang)) {
            const Token& op = advance();
            const ast::UnaryOp kind =
                op.kind == TokenKind::Minus ? ast::UnaryOp::Negate : ast::UnaryOp::Not;
            // The operand is parsed at unary level, not cast level, so
            // `-x as float` casts the negation rather than negating a
            // cast. Same as Rust.
            ast::ExprPtr operand = parse_unary(allow_struct_literal);
            const Span span = op.span.merge(operand->span);
            return std::make_unique<ast::UnaryExpr>(span, kind, std::move(operand));
        }
        return parse_postfix(parse_primary(allow_struct_literal));
    }

    ast::ExprPtr parse_postfix(ast::ExprPtr expr) {
        while (true) {
            if (check(TokenKind::Dot)) {
                advance();
                const Token& name = expect(TokenKind::Identifier);

                if (check(TokenKind::LParen)) {
                    auto call = std::make_unique<ast::MethodCallExpr>(
                        expr->span, std::move(expr), std::string{name.text}, name.span);
                    const Span end = parse_arguments(call->args);
                    call->span = call->receiver->span.merge(end);
                    expr = std::move(call);
                } else {
                    const Span span = expr->span.merge(name.span);
                    expr = std::make_unique<ast::FieldAccessExpr>(
                        span, std::move(expr), std::string{name.text}, name.span);
                }
                continue;
            }

            if (check(TokenKind::LBracket)) {
                advance();
                ast::ExprPtr index = parse_expr();
                const Token& close = expect(TokenKind::RBracket);
                const Span span = expr->span.merge(close.span);
                expr = std::make_unique<ast::IndexExpr>(span, std::move(expr), std::move(index));
                continue;
            }

            if (check(TokenKind::LParen)) {
                // `call_expr = identifier "(" ... ")"`: only a bare name
                // can be called, since v1 has no function values (§4).
                throw error_at(expr->span, "this expression cannot be called",
                               "functions are not first-class values in v1, so only a "
                               "named function or method can be called");
            }

            // Struct literals are handled in parse_primary, where the
            // name is still in hand; by here the `{` can only belong to
            // an enclosing block.
            return expr;
        }
    }

    /// Parses `( a, b, c )` into `args`, returning the span of the `)`.
    Span parse_arguments(std::vector<ast::ExprPtr>& args) {
        expect(TokenKind::LParen);
        if (!check(TokenKind::RParen)) {
            do {
                args.push_back(parse_expr());
            } while (match(TokenKind::Comma));
        }
        return expect(TokenKind::RParen).span;
    }

    ast::ExprPtr parse_struct_literal(const std::string& type_name, Span name_span,
                                      std::string module = {}) {
        auto literal = std::make_unique<ast::StructLitExpr>(name_span, type_name, name_span);
        literal->module = std::move(module);
        expect(TokenKind::LBrace);

        if (!check(TokenKind::RBrace)) {
            do {
                // A trailing comma before `}` is allowed, following Rust.
                if (check(TokenKind::RBrace)) {
                    break;
                }
                ast::FieldInit field;
                const Token& name = expect(TokenKind::Identifier);
                field.name = std::string{name.text};
                field.name_span = name.span;
                expect(TokenKind::Colon);
                field.value = parse_expr();
                field.span = name.span.merge(field.value->span);
                literal->fields.push_back(std::move(field));
            } while (match(TokenKind::Comma));
        }

        const Token& close = expect(TokenKind::RBrace);
        literal->span = name_span.merge(close.span);
        return literal;
    }

    ast::ExprPtr parse_primary(bool allow_struct_literal) {
        const Token& token = peek();

        switch (token.kind) {
            case TokenKind::IntLit:
                advance();
                return std::make_unique<ast::IntLitExpr>(token.span, token.int_value());

            case TokenKind::FloatLit:
                advance();
                return std::make_unique<ast::FloatLitExpr>(token.span, token.float_value());

            case TokenKind::BoolLit:
                advance();
                return std::make_unique<ast::BoolLitExpr>(token.span, token.bool_value());

            case TokenKind::StringLit:
                advance();
                return std::make_unique<ast::StringLitExpr>(token.span, token.string_value());

            case TokenKind::KwSelf:
                advance();
                return std::make_unique<ast::NameExpr>(token.span, "self");

            case TokenKind::Identifier: {
                advance();

                // `module::item`. Only one level deep: there are no
                // nested modules in v2, so a second `::` is an error
                // rather than a path to somewhere.
                std::string module;
                const Token* name = &token;
                if (match(TokenKind::ColonColon)) {
                    module = std::string{token.text};
                    name = &expect(TokenKind::Identifier);
                    if (check(TokenKind::ColonColon)) {
                        throw error_at(peek().span, "nested module paths are not supported",
                                       "a path is `module::item`, one level deep");
                    }
                }

                const Span name_span = name->span;
                const Span whole = token.span.merge(name_span);

                if (check(TokenKind::LParen)) {
                    auto call = std::make_unique<ast::CallExpr>(
                        whole, std::string{name->text}, name_span);
                    call->module = std::move(module);
                    const Span end = parse_arguments(call->args);
                    call->span = whole.merge(end);
                    return call;
                }
                if (allow_struct_literal && check(TokenKind::LBrace)) {
                    return parse_struct_literal(std::string{name->text}, whole,
                                                std::move(module));
                }
                return std::make_unique<ast::NameExpr>(whole, std::string{name->text},
                                                       std::move(module));
            }

            case TokenKind::LParen: {
                advance();
                // Parentheses only group; the tree shape already records
                // precedence, so no node is kept for them. Struct
                // literals are legal again inside them, which is the
                // documented escape hatch for `if (P { }) { }`.
                ast::ExprPtr inner = parse_expr(0, true);
                expect(TokenKind::RParen);
                return inner;
            }

            case TokenKind::LBracket: {
                const Span start = advance().span;
                auto array = std::make_unique<ast::ArrayLitExpr>(start);
                if (!check(TokenKind::RBracket)) {
                    do {
                        if (check(TokenKind::RBracket)) {
                            break;
                        }
                        array->elements.push_back(parse_expr());
                    } while (match(TokenKind::Comma));
                }
                const Token& close = expect(TokenKind::RBracket);
                array->span = start.merge(close.span);
                return array;
            }

            default:
                throw error_here("expected an expression");
        }
    }
};

}  // namespace

std::string_view stage_name() noexcept { return "parser"; }

ParseResult parse(const std::vector<Token>& tokens,
                  [[maybe_unused]] const ast::SourceFile& source) {
    // Diagnostics carry spans rather than rendered text, so the parser
    // itself never needs the source. It stays in the signature because
    // callers pass it anyway and Phase 3 will want it here.
    return Parser{tokens}.run();
}

ParseResult parse_source(const ast::SourceFile& source) {
    lexer::LexResult lexed = lexer::tokenize(source);
    if (!lexed.ok()) {
        return ParseResult{std::make_unique<ast::Program>(), std::move(lexed.diagnostics)};
    }
    return parse(lexed.tokens, source);
}


namespace {

/// The imports a parsed file declares, in source order.
std::vector<std::pair<std::string, ast::Span>> imports_of(const ast::Program& program) {
    std::vector<std::pair<std::string, ast::Span>> imports;
    for (const ast::ItemPtr& item : program.items) {
        if (const auto* declaration = ast::node_cast<ast::ImportDecl>(item.get())) {
            imports.emplace_back(declaration->module, declaration->module_span);
        }
    }
    return imports;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

}  // namespace

LoadResult load_program(const std::filesystem::path& entry, ast::SourceMap& sources) {
    LoadResult result;

    // Breadth-first from the entry file. Each module is loaded once, so
    // a cycle terminates: `a` imports `b` imports `a` leaves `a` already
    // in `loaded` the second time round.
    struct Pending {
        std::string name;
        std::filesystem::path path;
        /// Where the `import` was written, so a missing file can be
        /// reported against it rather than against nothing.
        std::optional<ast::Span> requested_at;
    };

    std::deque<Pending> queue;
    queue.push_back(Pending{{}, entry, std::nullopt});
    std::vector<std::string> loaded;

    while (!queue.empty()) {
        const Pending pending = std::move(queue.front());
        queue.pop_front();

        if (std::find(loaded.begin(), loaded.end(), pending.name) != loaded.end()) {
            continue;
        }

        const std::optional<std::string> contents = read_file(pending.path);
        if (!contents.has_value()) {
            if (pending.requested_at.has_value()) {
                result.diagnostics.push_back(ast::Diagnostic::error(
                    "cannot find module `" + pending.name + "`", *pending.requested_at,
                    "no file at `" + pending.path.string() + "`"));
            } else {
                result.diagnostics.push_back(ast::Diagnostic::error(
                    "cannot read `" + pending.path.string() + "`", ast::Span::at(0),
                    "the file could not be opened"));
            }
            continue;
        }

        const ast::FileId file = sources.add(pending.path.string(), *contents);
        ParseResult parsed = parse_source(sources.file(file));

        for (ast::Diagnostic& diagnostic : parsed.diagnostics) {
            result.diagnostics.push_back(std::move(diagnostic));
        }

        Module module;
        module.name = pending.name;
        module.path = pending.path;
        module.file = file;
        module.program = std::move(parsed.program);
        module.program->module = pending.name;

        const std::filesystem::path directory = pending.path.parent_path();
        for (const auto& [name, span] : imports_of(*module.program)) {
            if (name == pending.name) {
                result.diagnostics.push_back(ast::Diagnostic::error(
                    "a module cannot import itself", span, "`" + name + "` is this module"));
                continue;
            }
            module.imports.push_back(name);
            queue.push_back(Pending{
                name, directory / (name + "." + std::string{ast::kFileExtension}), span});
        }

        loaded.push_back(pending.name);
        result.modules.push_back(std::move(module));
    }

    return result;
}

}  // namespace ember::parser
