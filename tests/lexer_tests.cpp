// Phase 1 unit tests: the lexer, and the §7 diagnostic renderer it is
// the first stage to use.
//
// The golden `.tokens` snapshots in snapshot_tests.cpp cover whole
// files; these cover the edges - spans, literal decoding, maximal munch,
// and every way lexing can fail.

#include "test_harness.hpp"

#include "soliton/ast/diagnostic.hpp"
#include "soliton/ast/span.hpp"
#include "soliton/lexer/lexer.hpp"
#include "soliton/lexer/token.hpp"

#include <string>
#include <vector>

namespace {

using soliton::ast::Position;
using soliton::ast::SourceFile;
using soliton::lexer::Token;
using soliton::lexer::TokenKind;

SourceFile make_source(std::string contents) {
    return SourceFile{"test.sn", std::move(contents)};
}

/// Token kinds of a snippet, with the trailing Eof dropped.
std::vector<TokenKind> kinds_of(const SourceFile& source) {
    const soliton::lexer::LexResult result = soliton::lexer::tokenize(source);
    std::vector<TokenKind> kinds;
    for (const Token& token : result.tokens) {
        if (token.kind != TokenKind::Eof) {
            kinds.push_back(token.kind);
        }
    }
    return kinds;
}

std::string names_of(const std::vector<TokenKind>& kinds) {
    std::string out;
    for (const TokenKind kind : kinds) {
        if (!out.empty()) {
            out += ' ';
        }
        out += soliton::lexer::token_kind_name(kind);
    }
    return out;
}

/// Lex a snippet expected to be clean, and return its tokens.
std::vector<Token> lex_ok(const SourceFile& source) {
    soliton::lexer::LexResult result = soliton::lexer::tokenize(source);
    if (!result.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "unexpected lexer errors:\n" +
                                soliton::ast::render_all(result.diagnostics, source));
    }
    return result.tokens;
}

/// Lex a snippet expected to fail, and return its diagnostics.
std::vector<soliton::ast::Diagnostic> lex_errors(const SourceFile& source) {
    soliton::lexer::LexResult result = soliton::lexer::tokenize(source);
    if (result.ok()) {
        ::soliton::test::fail(__FILE__, __LINE__, "expected lexer errors, but lexing succeeded");
    }
    return result.diagnostics;
}

}  // namespace

// ---------------------------------------------------------------------
// Spans
// ---------------------------------------------------------------------

SOLITON_TEST(lexer_tracks_line_and_column_across_lines) {
    const SourceFile source = make_source("let x\n  = 1;\n");
    const std::vector<Token> tokens = lex_ok(source);

    // let(1:1) x(1:5) =(2:3) 1(2:5) ;(2:6)
    SOLITON_CHECK_EQ(source.position_of(tokens[0].span.start), (Position{1, 1}));
    SOLITON_CHECK_EQ(source.position_of(tokens[1].span.start), (Position{1, 5}));
    SOLITON_CHECK_EQ(source.position_of(tokens[2].span.start), (Position{2, 3}));
    SOLITON_CHECK_EQ(source.position_of(tokens[3].span.start), (Position{2, 5}));
    SOLITON_CHECK_EQ(source.position_of(tokens[4].span.start), (Position{2, 6}));
}

SOLITON_TEST(lexer_spans_cover_exactly_the_token_text) {
    const SourceFile source = make_source("fn distance_sq() -> int {}");
    for (const Token& token : lex_ok(source)) {
        if (token.kind == TokenKind::Eof) {
            continue;
        }
        SOLITON_CHECK_EQ(std::string{source.text_of(token.span)}, std::string{token.text});
    }
}

SOLITON_TEST(lexer_always_ends_with_eof_at_the_end_of_input) {
    const SourceFile source = make_source("let x = 1;");
    const std::vector<Token> tokens = lex_ok(source);
    SOLITON_CHECK(tokens.back().kind == TokenKind::Eof);
    SOLITON_CHECK_EQ(tokens.back().span.start, source.size());
}

SOLITON_TEST(lexer_handles_an_empty_file) {
    const SourceFile source = make_source("");
    const std::vector<Token> tokens = lex_ok(source);
    SOLITON_CHECK_EQ(tokens.size(), std::size_t{1});
    SOLITON_CHECK(tokens[0].kind == TokenKind::Eof);
}

// ---------------------------------------------------------------------
// Identifiers, keywords and comments
// ---------------------------------------------------------------------

SOLITON_TEST(lexer_separates_keywords_from_identifiers) {
    const SourceFile source = make_source("fn function funny _fn fn_");
    SOLITON_CHECK_EQ(names_of(kinds_of(source)),
                   std::string{"kw_fn ident ident ident ident"});
}

SOLITON_TEST(lexer_treats_primitive_type_names_as_keywords) {
    const SourceFile source = make_source("int float bool string");
    SOLITON_CHECK_EQ(names_of(kinds_of(source)),
                   std::string{"kw_int kw_float kw_bool kw_string"});
}

SOLITON_TEST(lexer_reads_true_and_false_as_bool_literals) {
    const SourceFile source = make_source("true false");
    const std::vector<Token> tokens = lex_ok(source);
    SOLITON_CHECK(tokens[0].kind == TokenKind::BoolLit);
    SOLITON_CHECK_EQ(tokens[0].bool_value(), true);
    SOLITON_CHECK_EQ(tokens[1].bool_value(), false);
}

SOLITON_TEST(lexer_skips_line_and_doc_comments) {
    const SourceFile source = make_source(
        "// a comment\n"
        "/// a doc comment\n"
        "let x = 1; // trailing\n");
    SOLITON_CHECK_EQ(names_of(kinds_of(source)), std::string{"kw_let ident eq int_lit semi"});
}

SOLITON_TEST(lexer_handles_a_comment_that_ends_the_file_without_a_newline) {
    const SourceFile source = make_source("let x = 1; // no newline after this");
    SOLITON_CHECK_EQ(names_of(kinds_of(source)), std::string{"kw_let ident eq int_lit semi"});
}

// ---------------------------------------------------------------------
// Numeric literals
// ---------------------------------------------------------------------

SOLITON_TEST(lexer_reads_integers_with_underscore_separators) {
    const SourceFile source = make_source("1 1_000_000 0");
    const std::vector<Token> tokens = lex_ok(source);
    SOLITON_CHECK_EQ(tokens[0].int_value(), std::int64_t{1});
    SOLITON_CHECK_EQ(tokens[1].int_value(), std::int64_t{1000000});
    SOLITON_CHECK_EQ(tokens[2].int_value(), std::int64_t{0});
}

SOLITON_TEST(lexer_reads_floats_with_and_without_exponents) {
    const SourceFile source = make_source("1.618 6.02e23 1.5e-3 2E+2");
    const std::vector<Token> tokens = lex_ok(source);
    SOLITON_CHECK(tokens[0].kind == TokenKind::FloatLit);
    SOLITON_CHECK_EQ(tokens[0].float_value(), 1.618);
    SOLITON_CHECK_EQ(tokens[1].float_value(), 6.02e23);
    SOLITON_CHECK_EQ(tokens[2].float_value(), 1.5e-3);
    SOLITON_CHECK_EQ(tokens[3].float_value(), 2E+2);
}

SOLITON_TEST(lexer_requires_a_digit_after_the_dot_so_method_calls_still_work) {
    // `1.max(2)` must not lex `1.` as a float, or method calls on integer
    // literals would be ambiguous. This is why a float needs digits on
    // both sides of the dot.
    const SourceFile source = make_source("1.max(2)");
    SOLITON_CHECK_EQ(names_of(kinds_of(source)),
                   std::string{"int_lit dot ident l_paren int_lit r_paren"});
}

SOLITON_TEST(lexer_rejects_an_integer_that_does_not_fit_in_64_bits) {
    const SourceFile source = make_source("let big = 9223372036854775808;");
    const std::vector<soliton::ast::Diagnostic> errors = lex_errors(source);
    SOLITON_CHECK_EQ(errors.size(), std::size_t{1});
    SOLITON_CHECK_EQ(errors[0].message, std::string{"integer literal out of range"});
    SOLITON_CHECK_EQ(errors[0].label,
                   std::string{"`int` values must fit in a signed 64-bit integer"});
}

SOLITON_TEST(lexer_accepts_the_largest_representable_integer) {
    const SourceFile source = make_source("9223372036854775807");
    const std::vector<Token> tokens = lex_ok(source);
    SOLITON_CHECK_EQ(tokens[0].int_value(), std::int64_t{9223372036854775807});
}

SOLITON_TEST(lexer_rejects_a_suffix_glued_to_a_number) {
    const SourceFile source = make_source("let x = 1abc;");
    const std::vector<soliton::ast::Diagnostic> errors = lex_errors(source);
    SOLITON_CHECK_EQ(errors[0].message, std::string{"invalid suffix on numeric literal"});
    SOLITON_CHECK_EQ(errors[0].label, std::string{"`abc` is not a valid literal suffix"});
}

SOLITON_TEST(lexer_rejects_an_exponent_with_no_digits) {
    const SourceFile source = make_source("1e");
    const std::vector<soliton::ast::Diagnostic> errors = lex_errors(source);
    SOLITON_CHECK_EQ(errors[0].message, std::string{"invalid suffix on numeric literal"});
}

// ---------------------------------------------------------------------
// String literals
// ---------------------------------------------------------------------

SOLITON_TEST(lexer_decodes_string_escapes) {
    const SourceFile source = make_source(R"("a\tb\nc\\d\"e\0f")");
    const std::vector<Token> tokens = lex_ok(source);
    const Token& token = tokens[0];
    SOLITON_CHECK(token.kind == TokenKind::StringLit);
    SOLITON_CHECK_EQ(token.string_value(), (std::string{"a\tb\nc\\d\"e\0f", 11}));
}

SOLITON_TEST(lexer_keeps_the_raw_text_alongside_the_decoded_value) {
    const SourceFile source = make_source(R"("hi\n")");
    const std::vector<Token> tokens = lex_ok(source);
    const Token& token = tokens[0];
    SOLITON_CHECK_EQ(std::string{token.text}, std::string{R"("hi\n")"});
    SOLITON_CHECK_EQ(token.string_value(), std::string{"hi\n"});
}

SOLITON_TEST(lexer_reads_an_empty_string) {
    const SourceFile source = make_source(R"("")");
    const std::vector<Token> tokens = lex_ok(source);
    SOLITON_CHECK_EQ(tokens[0].string_value(), std::string{});
}

SOLITON_TEST(lexer_rejects_an_unknown_escape) {
    const SourceFile source = make_source(R"("a\qb")");
    const std::vector<soliton::ast::Diagnostic> errors = lex_errors(source);
    SOLITON_CHECK_EQ(errors[0].message, std::string{"unknown escape sequence"});
    SOLITON_CHECK_EQ(errors[0].label,
                   std::string{"`\\q` is not a recognized escape sequence"});
}

SOLITON_TEST(lexer_rejects_a_string_that_runs_to_the_end_of_the_line) {
    // The error points at the opening quote, not at the newline, so the
    // caret lands on the literal the programmer actually has to fix.
    const SourceFile source = make_source("let s = \"oops;\nlet t = 1;\n");
    const std::vector<soliton::ast::Diagnostic> errors = lex_errors(source);
    SOLITON_CHECK_EQ(errors.size(), std::size_t{1});
    SOLITON_CHECK_EQ(errors[0].message, std::string{"unterminated string literal"});
    SOLITON_CHECK_EQ(source.position_of(errors[0].span.start), (Position{1, 9}));
}

// ---------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------

SOLITON_TEST(lexer_prefers_the_longest_operator) {
    const SourceFile source = make_source("== = != ! <= < >= > && & || -> -");
    SOLITON_CHECK_EQ(
        names_of(kinds_of(source)),
        std::string{"eq_eq eq bang_eq bang lt_eq lt gt_eq gt amp_amp amp pipe_pipe arrow minus"});
}

SOLITON_TEST(lexer_reads_every_delimiter_and_punctuation_mark) {
    const SourceFile source = make_source("( ) { } [ ] , ; : . + - * / %");
    SOLITON_CHECK_EQ(names_of(kinds_of(source)),
                   std::string{"l_paren r_paren l_brace r_brace l_bracket r_bracket comma "
                               "semi colon dot plus minus star slash percent"});
}

SOLITON_TEST(lexer_reads_a_lone_pipe_as_its_own_token) {
    // A single `|` opens a closure's parameter list, so it is a token
    // rather than the error it used to be before closures existed.
    const SourceFile source = make_source("| || |");
    SOLITON_CHECK_EQ(names_of(kinds_of(source)), std::string{"pipe pipe_pipe pipe"});
}

SOLITON_TEST(lexer_rejects_characters_outside_the_grammar) {
    const SourceFile source = make_source("let x = a @ b;");
    const std::vector<soliton::ast::Diagnostic> errors = lex_errors(source);
    SOLITON_CHECK_EQ(errors[0].message, std::string{"unexpected character"});
    SOLITON_CHECK_EQ(errors[0].label, std::string{"`@` is not valid in Soliton source"});
}

// ---------------------------------------------------------------------
// Error recovery
// ---------------------------------------------------------------------

SOLITON_TEST(lexer_reports_every_error_in_one_run) {
    const SourceFile source = make_source("let a = @;\nlet b = #;\nlet c = $;\n");
    const std::vector<soliton::ast::Diagnostic> errors = lex_errors(source);
    SOLITON_CHECK_EQ(errors.size(), std::size_t{3});
    SOLITON_CHECK_EQ(source.position_of(errors[0].span.start), (Position{1, 9}));
    SOLITON_CHECK_EQ(source.position_of(errors[1].span.start), (Position{2, 9}));
    SOLITON_CHECK_EQ(source.position_of(errors[2].span.start), (Position{3, 9}));
}

SOLITON_TEST(lexer_keeps_lexing_valid_tokens_after_a_bad_character) {
    const SourceFile source = make_source("let @ x = 1;");
    const soliton::lexer::LexResult result = soliton::lexer::tokenize(source);
    SOLITON_CHECK_EQ(result.diagnostics.size(), std::size_t{1});

    std::string kinds;
    for (const Token& token : result.tokens) {
        if (!kinds.empty()) {
            kinds += ' ';
        }
        kinds += soliton::lexer::token_kind_name(token.kind);
    }
    SOLITON_CHECK_EQ(kinds, std::string{"kw_let ident eq int_lit semi eof"});
}

// ---------------------------------------------------------------------
// The §7 diagnostic format
// ---------------------------------------------------------------------

SOLITON_TEST(diagnostics_render_in_the_spec_format) {
    // This is the worked example from §7 of the spec, rebuilt exactly.
    std::string contents;
    for (int i = 0; i < 11; ++i) {
        contents += '\n';
    }
    contents += "    let x: int = \"hello\";\n";
    const SourceFile source{"file.sn", contents};

    const auto offset = static_cast<std::uint32_t>(contents.find("\"hello\""));
    const soliton::ast::Diagnostic diagnostic = soliton::ast::Diagnostic::error(
        "type mismatch", soliton::ast::Span::at(offset, 7), "expected `int`, found `string`");

    SOLITON_CHECK_EQ(soliton::ast::render(diagnostic, source),
                   std::string{"error: type mismatch\n"
                               "  --> file.sn:12:18\n"
                               "   |\n"
                               "12 |     let x: int = \"hello\";\n"
                               "   |                  ^^^^^^^ expected `int`, found `string`\n"});
}

SOLITON_TEST(diagnostic_gutter_widens_with_the_line_number) {
    std::string contents;
    for (int i = 0; i < 99; ++i) {
        contents += '\n';
    }
    contents += "let x = 1;\n";
    const SourceFile source{"wide.sn", contents};

    const auto offset = static_cast<std::uint32_t>(contents.find("1;"));
    const std::string rendered = soliton::ast::render(
        soliton::ast::Diagnostic::error("bad", soliton::ast::Span::at(offset), "here"), source);

    SOLITON_CHECK_EQ(rendered, std::string{"error: bad\n"
                                         "   --> wide.sn:100:9\n"
                                         "    |\n"
                                         "100 | let x = 1;\n"
                                         "    |         ^ here\n"});
}

SOLITON_TEST(diagnostic_carets_align_under_tab_indented_source) {
    // The source line is echoed verbatim, so the caret indent reuses the
    // line's own tabs rather than expanding them to a guessed width.
    const SourceFile source{"tabs.sn", "\t\tlet x = 1;\n"};
    const std::string rendered = soliton::ast::render(
        soliton::ast::Diagnostic::error("bad", soliton::ast::Span::at(6, 1), "here"), source);

    SOLITON_CHECK_EQ(rendered, std::string{"error: bad\n"
                                         " --> tabs.sn:1:7\n"
                                         "  |\n"
                                         "1 | \t\tlet x = 1;\n"
                                         "  | \t\t    ^ here\n"});
}

SOLITON_TEST(diagnostic_notes_render_after_the_snippet) {
    const SourceFile source{"note.sn", "let x = y;\n"};
    soliton::ast::Diagnostic diagnostic =
        soliton::ast::Diagnostic::error("cannot find value", soliton::ast::Span::at(8), "not found");
    diagnostic.with_note("did you mean `x`?");

    SOLITON_CHECK_EQ(soliton::ast::render(diagnostic, source),
                   std::string{"error: cannot find value\n"
                               " --> note.sn:1:9\n"
                               "  |\n"
                               "1 | let x = y;\n"
                               "  |         ^ not found\n"
                               "  = note: did you mean `x`?\n"});
}

SOLITON_TEST(rendering_several_diagnostics_ends_with_a_summary) {
    const SourceFile source{"many.sn", "let a = @;\nlet b = @;\n"};
    const soliton::lexer::LexResult result = soliton::lexer::tokenize(source);
    const std::string rendered = soliton::ast::render_all(result.diagnostics, source);

    SOLITON_CHECK_MSG(rendered.find("error: aborting due to 2 previous errors") != std::string::npos,
                    "missing summary line in:\n" + rendered);
}

SOLITON_TEST(lexer_errors_render_with_a_caret_on_the_offending_text) {
    const SourceFile source{"bad.sn", "let x = 1abc;\n"};
    const soliton::lexer::LexResult result = soliton::lexer::tokenize(source);

    SOLITON_CHECK_EQ(soliton::ast::render(result.diagnostics.at(0), source),
                   std::string{"error: invalid suffix on numeric literal\n"
                               " --> bad.sn:1:10\n"
                               "  |\n"
                               "1 | let x = 1abc;\n"
                               "  |          ^^^ `abc` is not a valid literal suffix\n"});
}


SOLITON_TEST(lexer_skips_a_utf8_byte_order_mark) {
    // Notepad and PowerShell's `Out-File -Encoding utf8` both write a
    // BOM, so this is the first file many people on Windows save. It
    // used to produce three "not valid in Soliton source" errors before
    // the lexer reached a single token.
    const SourceFile source = make_source("\xEF\xBB\xBFlet x = 1;");
    const std::vector<Token> tokens = lex_ok(source);
    SOLITON_CHECK(tokens.front().kind == TokenKind::KwLet);
}

SOLITON_TEST(a_byte_order_mark_does_not_shift_columns) {
    // Why the mark is dropped when the file is read rather than skipped
    // by the lexer. Were it left in the text, every column on the first
    // line would be reported three too high and the caret in a
    // diagnostic would point past the thing it is about.
    const SourceFile plain = make_source("let x = 1;");
    const SourceFile marked = make_source("\xEF\xBB\xBFlet x = 1;");

    const std::vector<Token> from_plain = lex_ok(plain);
    const std::vector<Token> from_marked = lex_ok(marked);

    SOLITON_CHECK_EQ(marked.position_of(from_marked[0].span.start), (Position{1, 1}));
    SOLITON_CHECK_EQ(plain.position_of(from_plain[0].span.start),
                   marked.position_of(from_marked[0].span.start));

    // And the one after it, so this is about the whole line rather than
    // just the token that happened to sit against the mark.
    SOLITON_CHECK_EQ(plain.position_of(from_plain[1].span.start),
                   marked.position_of(from_marked[1].span.start));
}

SOLITON_TEST(a_byte_order_mark_only_counts_at_the_start) {
    // Those bytes anywhere else are still junk, and saying so is the
    // whole point of the diagnostic this skips.
    const SourceFile source = make_source("let x = 1;\xEF\xBB\xBF");
    const soliton::lexer::LexResult result = soliton::lexer::tokenize(source);
    SOLITON_CHECK(!result.diagnostics.empty());
}
