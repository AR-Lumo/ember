// Golden-file snapshot tests for the Soliton compiler (Â§2, "Testing").
//
// Every case is an `.sn` file in tests/golden/. Alongside it live the
// expected outputs of each pipeline stage, one file per stage:
//
//   <case>.tokens   Phase 1   one token per line, with spans
//   <case>.ast      Phase 2   s-expression form of the AST
//   <case>.check    Phase 3   type checker diagnostics (Â§7 format)
//   <case>.out      Phase 4   stdout of the compiled program
//
// A stage is only checked for a case when that case has the matching
// snapshot file, so cases can be added ahead of the stage that consumes
// them. Re-record every snapshot by setting SOLITON_UPDATE_GOLDEN=1 in the
// environment and running the tests again.

#include "test_harness.hpp"

#include "cli.hpp"
#include "soliton/ast/ast.hpp"
#include "soliton/ast/diagnostic.hpp"
#include "soliton/ast/span.hpp"
#include "soliton/ast/printer.hpp"
#include "soliton/codegen/codegen.hpp"
#include "soliton/lexer/lexer.hpp"
#include "soliton/parser/parser.hpp"
#include "soliton/typeck/typeck.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// Directory holding the golden cases, injected by CMake so the test
/// binary does not depend on the working directory it is run from.
fs::path golden_dir() { return fs::path{SOLITON_GOLDEN_DIR}; }

bool update_requested() { return std::getenv("SOLITON_UPDATE_GOLDEN") != nullptr; }

/// Line endings and trailing whitespace are not part of what a snapshot
/// asserts; git on Windows rewrites the former behind our back.
std::string normalize(std::string_view text) {
    std::vector<std::string> lines;
    std::string line;
    for (const char ch : text) {
        if (ch == '\n') {
            lines.push_back(line);
            line.clear();
        } else if (ch != '\r') {
            line.push_back(ch);
        }
    }
    lines.push_back(line);

    for (std::string& entry : lines) {
        const std::size_t end = entry.find_last_not_of(" \t");
        entry = (end == std::string::npos) ? std::string{} : entry.substr(0, end + 1);
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }

    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    return out;
}

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void write_file(const fs::path& path, std::string_view contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        ::soliton::test::fail(__FILE__, __LINE__, "cannot write " + path.string());
    }
    stream << contents;
}

std::string source_extension() { return "." + std::string{soliton::ast::kFileExtension}; }

/// One golden test case: an `.sn` source file and its snapshots.
struct GoldenCase {
    std::string name;
    fs::path path;
    std::string source;

    fs::path snapshot_path(std::string_view stage) const {
        fs::path result = path;
        result.replace_extension(std::string{stage});
        return result;
    }

    std::optional<std::string> snapshot(std::string_view stage) const {
        const std::optional<std::string> raw = read_file(snapshot_path(stage));
        if (!raw.has_value()) {
            return std::nullopt;
        }
        return normalize(*raw);
    }
};

/// Every `.sn` file in tests/golden/, sorted by name for stable output.
std::vector<GoldenCase> cases() {
    std::vector<GoldenCase> found;
    const fs::path dir = golden_dir();

    if (!fs::is_directory(dir)) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "golden directory does not exist: " + dir.string());
    }

    for (const fs::directory_entry& entry : fs::directory_iterator{dir}) {
        if (!entry.is_regular_file() || entry.path().extension() != source_extension()) {
            continue;
        }
        const std::optional<std::string> source = read_file(entry.path());
        if (!source.has_value()) {
            ::soliton::test::fail(__FILE__, __LINE__, "cannot read " + entry.path().string());
        }
        found.push_back(GoldenCase{entry.path().stem().string(), entry.path(),
                                   normalize(*source)});
    }

    std::sort(found.begin(), found.end(),
              [](const GoldenCase& a, const GoldenCase& b) { return a.name < b.name; });
    return found;
}

/// Compare `actual` against the recorded snapshot for `stage`, or record
/// it when SOLITON_UPDATE_GOLDEN is set.
///
/// Returns false when this case has no snapshot for this stage, so
/// callers can count how many cases a stage actually covered.
bool check_snapshot(const GoldenCase& test_case, std::string_view stage,
                    std::string_view actual_raw) {
    const std::string actual = normalize(actual_raw);
    const fs::path path = test_case.snapshot_path(stage);

    if (update_requested()) {
        write_file(path, actual + "\n");
        return true;
    }

    const std::optional<std::string> expected = test_case.snapshot(stage);
    if (!expected.has_value()) {
        return false;
    }

    if (*expected != actual) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "snapshot mismatch for `" + test_case.name + "` [" +
                                std::string{stage} + "]\n" + "--- expected (" + path.string() +
                                ") ---\n" + *expected + "\n--- actual ---\n" + actual +
                                "\n---\nre-record with SOLITON_UPDATE_GOLDEN=1");
    }
    return true;
}

/// A multi-file golden case: a subdirectory of tests/golden/ holding a
/// `main.sn` plus the modules it imports.
///
/// These get only an `.out` snapshot. A token stream or a syntax tree
/// for "a program in four files" is not one artifact, and the thing
/// worth pinning about a multi-module program is that it builds and
/// prints what it should.
struct ModuleCase {
    std::string name;
    fs::path entry;
};

std::vector<ModuleCase> module_cases() {
    std::vector<ModuleCase> found;
    for (const fs::directory_entry& entry : fs::directory_iterator{golden_dir()}) {
        if (!entry.is_directory()) {
            continue;
        }
        const fs::path main_file = entry.path() / ("main." + std::string{soliton::ast::kFileExtension});
        if (fs::exists(main_file)) {
            found.push_back(ModuleCase{entry.path().filename().string(), main_file});
        }
    }
    std::sort(found.begin(), found.end(),
              [](const ModuleCase& a, const ModuleCase& b) { return a.name < b.name; });
    return found;
}

/// Every `.sn` file under tests/golden/, including the ones inside
/// module cases. Used by the coverage guards, which care about what the
/// lexer and parser see rather than about whole programs.
std::vector<fs::path> all_sources() {
    std::vector<fs::path> found;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator{golden_dir()}) {
        if (entry.is_regular_file() && entry.path().extension() == source_extension()) {
            found.push_back(entry.path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

/// A SourceFile for a golden case, named by file name only so that
/// rendered diagnostics in `.check` snapshots never contain an absolute
/// path from the machine that recorded them.
soliton::ast::SourceFile source_of(const GoldenCase& test_case) {
    return soliton::ast::SourceFile{test_case.path.filename().string(), test_case.source};
}

soliton::cli::ParseResult parse(std::initializer_list<std::string_view> args) {
    const std::vector<std::string_view> owned{args};
    return soliton::cli::parse_args(owned);
}

std::string usage_message(const soliton::cli::ParseResult& result) {
    const auto* error = std::get_if<soliton::cli::UsageError>(&result);
    if (error == nullptr) {
        ::soliton::test::fail(__FILE__, __LINE__, "expected a usage error, got a valid command");
    }
    return error->message;
}

const soliton::cli::Command& command_of(const soliton::cli::ParseResult& result) {
    const auto* command = std::get_if<soliton::cli::Command>(&result);
    if (command == nullptr) {
        ::soliton::test::fail(__FILE__, __LINE__,
                            "expected a valid command, got usage error: " +
                                std::get<soliton::cli::UsageError>(result).message);
    }
    return *command;
}

}  // namespace

// ---------------------------------------------------------------------
// Phase 0 - the harness itself.
//
// Later phases add tests here that walk cases() and call check_snapshot
// for their stage.
// ---------------------------------------------------------------------

SOLITON_TEST(golden_directory_has_cases) {
    const std::vector<GoldenCase> found = cases();
    SOLITON_CHECK_MSG(!found.empty(), "no cases found in " + golden_dir().string());
}

SOLITON_TEST(every_case_is_readable_and_non_empty) {
    for (const GoldenCase& test_case : cases()) {
        SOLITON_CHECK_MSG(!test_case.source.empty(),
                        "golden case is empty: " + test_case.path.string());
    }
}

SOLITON_TEST(every_snapshot_belongs_to_a_case) {
    std::vector<std::string> known;
    for (const GoldenCase& test_case : cases()) {
        known.push_back(test_case.name);
    }

    constexpr std::array<std::string_view, 4> kStages{".tokens", ".ast", ".check", ".out"};

    for (const fs::directory_entry& entry : fs::directory_iterator{golden_dir()}) {
        if (entry.is_directory()) {
            continue;  // module cases keep their snapshots inside
        }
        const std::string extension = entry.path().extension().string();
        if (std::find(kStages.begin(), kStages.end(), extension) == kStages.end()) {
            continue;
        }
        const std::string stem = entry.path().stem().string();
        SOLITON_CHECK_MSG(std::find(known.begin(), known.end(), stem) != known.end(),
                        "orphaned snapshot with no matching source: " + entry.path().string());
    }
}

SOLITON_TEST(snapshot_normalization_ignores_line_endings_and_trailing_space) {
    SOLITON_CHECK_EQ(normalize("a\r\nb  \n\n"), std::string{"a\nb"});
}

SOLITON_TEST(pipeline_stages_are_linked_in) {
    SOLITON_CHECK_EQ(std::string{soliton::ast::kLanguageName}, std::string{"Soliton"});
    SOLITON_CHECK_EQ(std::string{soliton::ast::kFileExtension}, std::string{"sn"});
    // Phase 0 has no backend; this only asserts the symbol resolves.
    SOLITON_CHECK(soliton::codegen::is_available() || !soliton::codegen::is_available());
}

// ---------------------------------------------------------------------
// Phase 1 - the lexer.
// ---------------------------------------------------------------------

SOLITON_TEST(phase1_every_golden_case_lexes_without_errors) {
    for (const GoldenCase& test_case : cases()) {
        const soliton::ast::SourceFile source = source_of(test_case);
        const soliton::lexer::LexResult result = soliton::lexer::tokenize(source);
        SOLITON_CHECK_MSG(result.ok(), "lexer errors in " + test_case.name + ":\n" +
                                         soliton::ast::render_all(result.diagnostics, source));
    }
}

SOLITON_TEST(phase1_token_streams_match_their_snapshots) {
    std::size_t covered = 0;
    const std::vector<GoldenCase> found = cases();

    for (const GoldenCase& test_case : found) {
        const soliton::ast::SourceFile source = source_of(test_case);
        const soliton::lexer::LexResult result = soliton::lexer::tokenize(source);
        if (check_snapshot(test_case, "tokens",
                           soliton::lexer::dump_tokens(result.tokens, source))) {
            ++covered;
        }
    }

    SOLITON_CHECK_MSG(covered == found.size(),
                    "some golden cases have no recorded `.tokens` snapshot; "
                    "re-record with SOLITON_UPDATE_GOLDEN=1");
}

SOLITON_TEST(phase1_golden_cases_exercise_every_token_kind) {
    // operators.sn exists to keep this honest: if a token kind is added
    // to the grammar without a case that produces it, this fails.
    std::vector<bool> seen(static_cast<std::size_t>(soliton::lexer::TokenKind::Eof) + 1, false);

    for (const fs::path& path : all_sources()) {
        const std::optional<std::string> contents = read_file(path);
        SOLITON_CHECK_MSG(contents.has_value(), "cannot read " + path.string());
        const soliton::ast::SourceFile source{path.filename().string(), normalize(*contents)};
        for (const soliton::lexer::Token& token : soliton::lexer::tokenize(source).tokens) {
            seen[static_cast<std::size_t>(token.kind)] = true;
        }
    }

    std::string missing;
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
            missing += ' ';
            missing += soliton::lexer::token_kind_name(static_cast<soliton::lexer::TokenKind>(i));
        }
    }
    SOLITON_CHECK_MSG(missing.empty(), "token kinds never produced by any golden case:" + missing);
}

// ---------------------------------------------------------------------
// Phase 2 - the parser.
// ---------------------------------------------------------------------

SOLITON_TEST(phase2_every_golden_case_parses_without_errors) {
    for (const GoldenCase& test_case : cases()) {
        const soliton::ast::SourceFile source = source_of(test_case);
        const soliton::parser::ParseResult result = soliton::parser::parse_source(source);
        SOLITON_CHECK_MSG(result.ok(), "parse errors in " + test_case.name + ":\n" +
                                         soliton::ast::render_all(result.diagnostics, source));
    }
}

SOLITON_TEST(phase2_syntax_trees_match_their_snapshots) {
    std::size_t covered = 0;
    const std::vector<GoldenCase> found = cases();

    for (const GoldenCase& test_case : found) {
        const soliton::ast::SourceFile source = source_of(test_case);
        const soliton::parser::ParseResult result = soliton::parser::parse_source(source);
        if (check_snapshot(test_case, "ast", soliton::ast::to_sexpr(*result.program, source))) {
            ++covered;
        }
    }

    SOLITON_CHECK_MSG(covered == found.size(),
                    "some golden cases have no recorded `.ast` snapshot; "
                    "re-record with SOLITON_UPDATE_GOLDEN=1");
}

SOLITON_TEST(phase2_golden_cases_exercise_every_node_kind) {
    // The mirror of the Phase 1 token-coverage test: a grammar
    // production with no golden case producing it is a blind spot.
    // Sized by the last enumerator of each enum. Adding a kind after it
    // without updating these indexes out of range, which is a loud
    // failure on purpose: a silently-too-small array would make this
    // guard stop guarding. All three of these have caught exactly that -
    // most recently `ItemKind::Unit`, added after `Import`.
    std::vector<bool> items(static_cast<std::size_t>(soliton::ast::ItemKind::Unit) + 1, false);
    std::vector<bool> stmts(static_cast<std::size_t>(soliton::ast::StmtKind::Block) + 1, false);
    std::vector<bool> exprs(static_cast<std::size_t>(soliton::ast::ExprKind::Closure) + 1, false);

    // Walk every tree, recording which kinds were produced.
    struct Walker {
        std::vector<bool>& stmts;
        std::vector<bool>& exprs;

        void expr(const soliton::ast::Expr& node) {
            exprs[static_cast<std::size_t>(node.kind)] = true;
            using namespace soliton::ast;
            switch (node.kind) {
                case ExprKind::Unary:
                    expr(*static_cast<const UnaryExpr&>(node).operand);
                    break;
                case ExprKind::Binary: {
                    const auto& binary = static_cast<const BinaryExpr&>(node);
                    expr(*binary.left);
                    expr(*binary.right);
                    break;
                }
                case ExprKind::Call:
                    for (const ExprPtr& arg : static_cast<const CallExpr&>(node).args) {
                        expr(*arg);
                    }
                    break;
                case ExprKind::MethodCall: {
                    const auto& call = static_cast<const MethodCallExpr&>(node);
                    expr(*call.receiver);
                    for (const ExprPtr& arg : call.args) {
                        expr(*arg);
                    }
                    break;
                }
                case ExprKind::FieldAccess:
                    expr(*static_cast<const FieldAccessExpr&>(node).object);
                    break;
                case ExprKind::Cast:
                    expr(*static_cast<const CastExpr&>(node).operand);
                    break;
                case ExprKind::Closure:
                    block(static_cast<const ClosureExpr&>(node).body);
                    break;
                case ExprKind::Index: {
                    const auto& index = static_cast<const IndexExpr&>(node);
                    expr(*index.object);
                    expr(*index.index);
                    break;
                }
                case ExprKind::StructLit:
                    for (const FieldInit& field : static_cast<const StructLitExpr&>(node).fields) {
                        expr(*field.value);
                    }
                    break;
                case ExprKind::ArrayLit:
                    for (const ExprPtr& element :
                         static_cast<const ArrayLitExpr&>(node).elements) {
                        expr(*element);
                    }
                    break;
                default:
                    break;
            }
        }

        void block(const soliton::ast::Block& body) {
            for (const soliton::ast::StmtPtr& statement : body.statements) {
                stmt(*statement);
            }
        }

        void stmt(const soliton::ast::Stmt& node) {
            stmts[static_cast<std::size_t>(node.kind)] = true;
            using namespace soliton::ast;
            switch (node.kind) {
                case StmtKind::Let:
                    expr(*static_cast<const LetStmt&>(node).value);
                    break;
                case StmtKind::Return:
                    if (const auto& value = static_cast<const ReturnStmt&>(node).value) {
                        expr(*value);
                    }
                    break;
                case StmtKind::If: {
                    const auto& branch = static_cast<const IfStmt&>(node);
                    expr(*branch.condition);
                    block(branch.then_block);
                    if (branch.else_branch) {
                        stmt(*branch.else_branch);
                    }
                    break;
                }
                case StmtKind::While: {
                    const auto& loop = static_cast<const WhileStmt&>(node);
                    expr(*loop.condition);
                    block(loop.body);
                    break;
                }
                case StmtKind::Assign: {
                    const auto& assign = static_cast<const AssignStmt&>(node);
                    expr(*assign.target);
                    expr(*assign.value);
                    break;
                }
                case StmtKind::Expr:
                    expr(*static_cast<const ExprStmt&>(node).expr);
                    break;
                case StmtKind::Block:
                    block(static_cast<const BlockStmt&>(node).block);
                    break;
            }
        }
    };

    Walker walker{stmts, exprs};

    // Every source under tests/golden/, module cases included: `import`
    // only ever appears inside one of those.
    for (const fs::path& path : all_sources()) {
        const std::optional<std::string> contents = read_file(path);
        SOLITON_CHECK_MSG(contents.has_value(), "cannot read " + path.string());
        const soliton::ast::SourceFile source{path.filename().string(), normalize(*contents)};
        const soliton::parser::ParseResult result = soliton::parser::parse_source(source);

        for (const soliton::ast::ItemPtr& item : result.program->items) {
            items[static_cast<std::size_t>(item->kind)] = true;
            using namespace soliton::ast;
            if (const auto* function = node_cast<FunctionDecl>(item.get())) {
                walker.block(function->body);
            } else if (const auto* impl = node_cast<ImplBlock>(item.get())) {
                for (const std::unique_ptr<FunctionDecl>& method : impl->methods) {
                    walker.block(method->body);
                }
            } else if (const auto* constant = node_cast<ConstDecl>(item.get())) {
                walker.expr(*constant->value);
            }
        }
    }

    std::string missing;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!items[i]) {
            missing += " item#" + std::to_string(i);
        }
    }
    for (std::size_t i = 0; i < stmts.size(); ++i) {
        if (!stmts[i]) {
            missing += " stmt#" + std::to_string(i);
        }
    }
    for (std::size_t i = 0; i < exprs.size(); ++i) {
        if (!exprs[i]) {
            missing += " expr#" + std::to_string(i);
        }
    }
    SOLITON_CHECK_MSG(missing.empty(), "AST kinds never produced by any golden case:" + missing);
}

// ---------------------------------------------------------------------
// Phase 3 - the type checker.
// ---------------------------------------------------------------------

/// Cases whose whole point is to fail: their `.check` snapshot is the
/// expected diagnostics rather than an empty file.
bool expects_type_errors(const GoldenCase& test_case) {
    return test_case.name.find("errors") != std::string::npos;
}

SOLITON_TEST(phase3_every_golden_case_type_checks_as_expected) {
    for (const GoldenCase& test_case : cases()) {
        const soliton::ast::SourceFile source = source_of(test_case);
        const soliton::parser::ParseResult parsed = soliton::parser::parse_source(source);
        const soliton::typeck::CheckResult checked =
            soliton::typeck::check(*parsed.program, source);

        if (expects_type_errors(test_case)) {
            SOLITON_CHECK_MSG(!checked.ok(),
                            test_case.name + " is meant to fail type checking, but passed");
        } else {
            SOLITON_CHECK_MSG(checked.ok(),
                            "type errors in " + test_case.name + ":\n" +
                                soliton::ast::render_all(checked.diagnostics, source));
        }
    }
}

SOLITON_TEST(phase3_diagnostics_match_their_snapshots) {
    std::size_t covered = 0;
    const std::vector<GoldenCase> found = cases();

    for (const GoldenCase& test_case : found) {
        const soliton::ast::SourceFile source = source_of(test_case);
        const soliton::parser::ParseResult parsed = soliton::parser::parse_source(source);
        const soliton::typeck::CheckResult checked =
            soliton::typeck::check(*parsed.program, source);

        // A clean program records an empty snapshot, which is itself
        // worth pinning: it fails loudly if a future change starts
        // rejecting a program that used to be legal.
        if (check_snapshot(test_case, "check",
                           soliton::ast::render_all(checked.diagnostics, source))) {
            ++covered;
        }
    }

    SOLITON_CHECK_MSG(covered == found.size(),
                    "some golden cases have no recorded `.check` snapshot; "
                    "re-record with SOLITON_UPDATE_GOLDEN=1");
}

SOLITON_TEST(phase3_records_a_type_for_every_expression_in_the_passing_cases) {
    // Phase 4 reads these back; a missing entry is a codegen crash.
    for (const GoldenCase& test_case : cases()) {
        if (expects_type_errors(test_case)) {
            continue;
        }
        const soliton::ast::SourceFile source = source_of(test_case);
        const soliton::parser::ParseResult parsed = soliton::parser::parse_source(source);
        const soliton::typeck::CheckResult checked =
            soliton::typeck::check(*parsed.program, source);

        for (const auto& [expr, type] : checked.expr_types) {
            SOLITON_CHECK_MSG(type != nullptr && !soliton::typeck::is_error(type),
                            "expression with no usable type in " + test_case.name);
        }
    }
}

// ---------------------------------------------------------------------
// Phase 4 - codegen, end to end.
//
// Â§8 asks for exactly this: compile and execute each example program and
// assert its stdout. These drive the real `soliton` binary, so what is
// under test is the whole pipeline plus the linker, not a simulation of
// it.
// ---------------------------------------------------------------------

namespace {

struct ProcessResult {
    int exit_code = 0;
    std::string output;
};

/// Run a command, capturing stdout. stderr is left attached so a failure
/// still shows its diagnostics in the test log.
ProcessResult run_process(const std::string& command) {
#ifdef _WIN32
    // cmd.exe eats the outer quotes of a command that starts with one.
    const std::string wrapped = "\"" + command + "\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        ::soliton::test::fail(__FILE__, __LINE__, "cannot start: " + command);
    }

    ProcessResult result;
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output += buffer.data();
    }

#ifdef _WIN32
    result.exit_code = _pclose(pipe);
#else
    result.exit_code = pclose(pipe);
#endif
    return result;
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

/// Cases that are meant to compile and run.
bool is_runnable(const GoldenCase& test_case) { return !expects_type_errors(test_case); }

}  // namespace

SOLITON_TEST(phase4_the_compiler_binary_exists) {
    SOLITON_CHECK_MSG(fs::exists(fs::path{SOLITON_BINARY}),
                    "no soliton binary at " + std::string{SOLITON_BINARY});
}

SOLITON_TEST(phase4_every_runnable_case_compiles_and_runs) {
    if (!soliton::codegen::is_available()) {
        return;  // reported by phase4_codegen_is_available
    }

    for (const GoldenCase& test_case : cases()) {
        if (!is_runnable(test_case)) {
            continue;
        }
        const ProcessResult result =
            run_process(quoted(fs::path{SOLITON_BINARY}) + " run " + quoted(test_case.path));
        SOLITON_CHECK_MSG(result.exit_code == 0,
                        test_case.name + " exited with " + std::to_string(result.exit_code) +
                            "; output was:\n" + result.output);
    }
}

SOLITON_TEST(phase4_program_output_matches_its_snapshot) {
    if (!soliton::codegen::is_available()) {
        return;
    }

    std::size_t covered = 0;
    std::size_t expected = 0;

    for (const GoldenCase& test_case : cases()) {
        if (!is_runnable(test_case)) {
            continue;
        }
        ++expected;
        const ProcessResult result =
            run_process(quoted(fs::path{SOLITON_BINARY}) + " run " + quoted(test_case.path));
        if (check_snapshot(test_case, "out", result.output)) {
            ++covered;
        }
    }

    SOLITON_CHECK_MSG(covered == expected,
                    "some runnable golden cases have no recorded `.out` snapshot; "
                    "re-record with SOLITON_UPDATE_GOLDEN=1");
}

SOLITON_TEST(modules_every_module_case_compiles_and_runs) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    for (const ModuleCase& test_case : module_cases()) {
        const ProcessResult result =
            run_process(quoted(fs::path{SOLITON_BINARY}) + " run " + quoted(test_case.entry));
        SOLITON_CHECK_MSG(result.exit_code == 0,
                        test_case.name + " exited with " + std::to_string(result.exit_code) +
                            "; output was:\n" + result.output);
    }
}

SOLITON_TEST(modules_program_output_matches_its_snapshot) {
    if (!soliton::codegen::is_available()) {
        return;
    }
    for (const ModuleCase& test_case : module_cases()) {
        const ProcessResult result =
            run_process(quoted(fs::path{SOLITON_BINARY}) + " run " + quoted(test_case.entry));

        // The snapshot sits beside the entry file, as `main.out`.
        fs::path snapshot = test_case.entry;
        snapshot.replace_extension("out");

        const std::string actual = normalize(result.output);
        if (update_requested()) {
            write_file(snapshot, actual + "\n");
            continue;
        }
        const std::optional<std::string> expected = read_file(snapshot);
        SOLITON_CHECK_MSG(expected.has_value(),
                        "no recorded output for module case `" + test_case.name +
                            "`; re-record with SOLITON_UPDATE_GOLDEN=1");
        if (expected.has_value()) {
            SOLITON_CHECK_EQ(actual, normalize(*expected));
        }
    }
}

SOLITON_TEST(modules_at_least_one_multi_file_case_exists) {
    SOLITON_CHECK_MSG(!module_cases().empty(),
                    "no multi-file golden cases; modules are untested end to end");
}

SOLITON_TEST(phase4_build_produces_a_standalone_executable) {
    if (!soliton::codegen::is_available()) {
        return;
    }

    const fs::path output = fs::temp_directory_path() / "soliton-golden-build.exe";
    fs::remove(output);

    const ProcessResult built =
        run_process(quoted(fs::path{SOLITON_BINARY}) + " build " +
                    quoted(golden_dir() / "hello.sn") + " -o " + quoted(output));
    SOLITON_CHECK_MSG(built.exit_code == 0, "build failed:\n" + built.output);
    SOLITON_CHECK_MSG(fs::exists(output), "no executable at " + output.string());

    // The point of `build` rather than `run`: the artifact outlives the
    // compiler invocation and runs on its own.
    const ProcessResult ran = run_process(quoted(output));
    SOLITON_CHECK_EQ(normalize(ran.output), std::string{"hello, world"});

    fs::remove(output);
}

SOLITON_TEST(phase4_methods_lower_to_plain_mangled_functions) {
    // Â§4 claims methods are sugar over functions taking self explicitly.
    // The check is that codegen emits a call to a symbol with that
    // name - no vtable, no indirect dispatch.
    const soliton::ast::SourceFile source{
        "point.sn",
        "struct Point { pub x: int, }\n"
        "impl Point {\n"
        "    pub fn double_x(&self) -> int { return self.x * 2; }\n"
        "}\n"
        "pub fn main() {\n"
        "    let p = Point { x: 21 };\n"
        "    println(p.double_x());\n"
        "}\n"};

    const soliton::parser::ParseResult parsed = soliton::parser::parse_source(source);
    const soliton::typeck::CheckResult checked = soliton::typeck::check(*parsed.program, source);
    SOLITON_CHECK(checked.ok());

    const soliton::codegen::CompileResult compiled =
        soliton::codegen::compile_to_string(*parsed.program, checked, source);
    SOLITON_CHECK_MSG(compiled.ok(), "codegen failed");

    SOLITON_CHECK_MSG(compiled.assembly.find("define") != std::string::npos,
                    "no function definitions in the IR");
    SOLITON_CHECK_MSG(compiled.assembly.find("@Point_double_x") != std::string::npos,
                    "method was not lowered to `Point_double_x`:\n" + compiled.assembly);
    SOLITON_CHECK_MSG(compiled.assembly.find("call") != std::string::npos,
                    "no direct call emitted");
}

SOLITON_TEST(phase4_entry_point_is_required_for_an_executable) {
    const soliton::ast::SourceFile source{"lib.sn", "pub fn helper() -> int { return 1; }\n"};
    const soliton::parser::ParseResult parsed = soliton::parser::parse_source(source);
    const soliton::typeck::CheckResult checked = soliton::typeck::check(*parsed.program, source);

    // `soliton check` accepts this: a file with no main is not a type
    // error, it just cannot be linked into a program.
    SOLITON_CHECK(checked.ok());

    const std::vector<soliton::ast::Diagnostic> entry =
        soliton::codegen::verify_entry_point(checked);
    SOLITON_CHECK_EQ(entry.size(), std::size_t{1});
    SOLITON_CHECK_EQ(entry.at(0).message, std::string{"no `main` function found"});
}

SOLITON_TEST(phase4_entry_point_must_not_take_arguments_or_return) {
    const soliton::ast::SourceFile source{"bad.sn", "pub fn main(a: int) -> int { return a; }\n"};
    const soliton::parser::ParseResult parsed = soliton::parser::parse_source(source);
    const soliton::typeck::CheckResult checked = soliton::typeck::check(*parsed.program, source);

    const std::vector<soliton::ast::Diagnostic> entry =
        soliton::codegen::verify_entry_point(checked);
    SOLITON_CHECK_EQ(entry.size(), std::size_t{2});
    SOLITON_CHECK_EQ(entry.at(0).message, std::string{"`main` cannot take arguments"});
    SOLITON_CHECK_EQ(entry.at(1).message, std::string{"`main` cannot return a value"});
}

SOLITON_TEST(phase4_codegen_is_available) {
    // A build without LLVM silently skips the tests above, so say so
    // loudly rather than reporting a green run that proved nothing.
    SOLITON_CHECK_MSG(soliton::codegen::is_available(),
                    "built without LLVM: the Phase 4 tests did not run");
}

// ---------------------------------------------------------------------
// Phase 0 - the CLI skeleton (Â§6).
// ---------------------------------------------------------------------

SOLITON_TEST(cli_parses_build_with_output) {
    const auto result = parse({"build", "main.sn", "-o", "main"});
    const soliton::cli::Command& command = command_of(result);
    SOLITON_CHECK(command.kind == soliton::cli::CommandKind::Build);
    SOLITON_CHECK_EQ(command.input.string(), std::string{"main.sn"});
    SOLITON_CHECK(command.output.has_value());
    SOLITON_CHECK_EQ(command.output->string(), std::string{"main"});
}

SOLITON_TEST(cli_parses_build_without_output) {
    const auto result = parse({"build", "main.sn"});
    const soliton::cli::Command& command = command_of(result);
    SOLITON_CHECK(command.kind == soliton::cli::CommandKind::Build);
    SOLITON_CHECK(!command.output.has_value());
}

SOLITON_TEST(cli_output_flag_accepts_long_form_in_any_order) {
    const auto result = parse({"build", "--output", "bin/app", "src/main.sn"});
    const soliton::cli::Command& command = command_of(result);
    SOLITON_CHECK_EQ(command.input.string(), std::string{"src/main.sn"});
    SOLITON_CHECK_EQ(command.output->string(), std::string{"bin/app"});
}

SOLITON_TEST(cli_parses_run_and_check) {
    const auto run = parse({"run", "hello.sn"});
    SOLITON_CHECK(command_of(run).kind == soliton::cli::CommandKind::Run);

    const auto check = parse({"check", "hello.sn"});
    SOLITON_CHECK(command_of(check).kind == soliton::cli::CommandKind::Check);
    SOLITON_CHECK_EQ(command_of(check).input.string(), std::string{"hello.sn"});
}

SOLITON_TEST(cli_parses_the_incremental_build_flags) {
    const auto flagged = parse({"build", "--fresh", "main.sn", "-v"});
    const soliton::cli::Command& build = command_of(flagged);
    SOLITON_CHECK(build.build.fresh);
    SOLITON_CHECK(build.build.verbose);
    SOLITON_CHECK_EQ(build.input.string(), std::string{"main.sn"});

    const auto bare = parse({"build", "main.sn"});
    const soliton::cli::Command& plain = command_of(bare);
    SOLITON_CHECK(!plain.build.fresh);
    SOLITON_CHECK(!plain.build.verbose);
}

SOLITON_TEST(cli_parses_fetch) {
    // The odd one out: it works on the manifest it finds, so it takes no
    // input file.
    const auto result = parse({"fetch"});
    SOLITON_CHECK(command_of(result).kind == soliton::cli::CommandKind::Fetch);
    SOLITON_CHECK(!command_of(result).update);

    const auto updating = parse({"fetch", "--update"});
    SOLITON_CHECK(command_of(updating).update);
}

SOLITON_TEST(cli_fetch_rejects_an_input_file) {
    SOLITON_CHECK_EQ(usage_message(parse({"fetch", "main.sn"})),
                   std::string{"`fetch` takes no input file, only the manifest it finds"});
}

SOLITON_TEST(cli_takes_update_on_everything_that_reads_a_program) {
    // Including `check`: resolving dependencies is part of finding the
    // modules, which `check` has to do.
    for (const std::string_view subcommand : {"build", "run", "check"}) {
        const auto result = parse({subcommand, "--update", "main.sn"});
        SOLITON_CHECK_MSG(command_of(result).update, std::string{subcommand});
    }
}

SOLITON_TEST(cli_parses_a_module_path) {
    const auto result = parse({"build", "-L", "vendor", "--module-path", "more", "main.sn"});
    const soliton::cli::Command& command = command_of(result);
    SOLITON_CHECK_EQ(command.module_path.size(), std::size_t{2});
    SOLITON_CHECK_EQ(command.module_path.at(0).string(), std::string{"vendor"});
    SOLITON_CHECK_EQ(command.module_path.at(1).string(), std::string{"more"});
}

SOLITON_TEST(cli_check_takes_a_module_path_even_though_it_takes_no_build_flags) {
    // Finding a module is the front end's problem, and `check` runs the
    // front end. `--fresh` is a back-end flag and stays refused.
    const auto result = parse({"check", "-L", "vendor", "main.sn"});
    SOLITON_CHECK_EQ(command_of(result).module_path.size(), std::size_t{1});
    SOLITON_CHECK_EQ(usage_message(parse({"check", "--fresh", "main.sn"})),
                   std::string{"unknown option `--fresh`"});
}

SOLITON_TEST(cli_rejects_a_dangling_module_path) {
    SOLITON_CHECK_EQ(usage_message(parse({"build", "main.sn", "-L"})),
                   std::string{"`-L` requires a directory argument"});
    SOLITON_CHECK_EQ(usage_message(parse({"run", "main.sn", "--module-path"})),
                   std::string{"`--module-path` requires a directory argument"});
}

SOLITON_TEST(cli_parses_the_whole_program_flag) {
    const auto result = parse({"build", "--whole-program", "main.sn"});
    SOLITON_CHECK(command_of(result).build.whole_program);

    const auto bare = parse({"build", "main.sn"});
    SOLITON_CHECK(!command_of(bare).build.whole_program);
}

SOLITON_TEST(cli_parses_an_optimization_level) {
    const auto result = parse({"build", "-O2", "main.sn"});
    SOLITON_CHECK_EQ(command_of(result).build.optimization_level, 2u);

    const auto bare = parse({"build", "main.sn"});
    SOLITON_CHECK_EQ(command_of(bare).build.optimization_level, 0u);
}

SOLITON_TEST(cli_rejects_an_optimization_level_it_does_not_have) {
    SOLITON_CHECK_EQ(usage_message(parse({"build", "-O4", "main.sn"})),
                   std::string{"expected an optimization level `-O0` through `-O3`, "
                               "found `-O4`"});
}

SOLITON_TEST(cli_will_not_guess_what_a_bare_dash_o_means) {
    // gcc reads it as -O1 and clang as -O2. Refusing beats picking.
    SOLITON_CHECK_MSG(
        usage_message(parse({"build", "-O", "main.sn"})).find("requires a level") !=
            std::string::npos,
        usage_message(parse({"build", "-O", "main.sn"})));
}

SOLITON_TEST(cli_keeps_the_output_flag_distinct_from_the_optimization_flag) {
    // `-o` and `-O` differ only in case, and one of them takes an
    // argument, so this is worth pinning down.
    const auto result = parse({"build", "-o", "app", "-O1", "main.sn"});
    const soliton::cli::Command& command = command_of(result);
    SOLITON_CHECK_EQ(command.output->string(), std::string{"app"});
    SOLITON_CHECK_EQ(command.build.optimization_level, 1u);
}

SOLITON_TEST(cli_run_takes_the_build_flags_too) {
    // `run` compiles before it runs, so the same decisions apply to it.
    const auto result = parse({"run", "--verbose", "hello.sn"});
    const soliton::cli::Command& command = command_of(result);
    SOLITON_CHECK(command.kind == soliton::cli::CommandKind::Run);
    SOLITON_CHECK(command.build.verbose);
    SOLITON_CHECK_EQ(command.input.string(), std::string{"hello.sn"});
}

SOLITON_TEST(cli_check_rejects_the_build_flags) {
    // `check` never reaches the back end, so there is nothing for these
    // to mean; saying so beats accepting them and doing nothing.
    SOLITON_CHECK_EQ(usage_message(parse({"check", "--fresh", "hello.sn"})),
                   std::string{"unknown option `--fresh`"});
}

SOLITON_TEST(cli_parses_help_and_version) {
    SOLITON_CHECK(command_of(parse({"--help"})).kind == soliton::cli::CommandKind::Help);
    SOLITON_CHECK(command_of(parse({"-h"})).kind == soliton::cli::CommandKind::Help);
    SOLITON_CHECK(command_of(parse({"--version"})).kind == soliton::cli::CommandKind::Version);
    SOLITON_CHECK(command_of(parse({"-V"})).kind == soliton::cli::CommandKind::Version);
}

SOLITON_TEST(cli_rejects_empty_command_line) {
    SOLITON_CHECK_EQ(usage_message(parse({})), std::string{"no subcommand given"});
}

SOLITON_TEST(cli_rejects_unknown_subcommand) {
    SOLITON_CHECK_EQ(usage_message(parse({"frobnicate", "x.sn"})),
                   std::string{"unknown subcommand `frobnicate`"});
}

SOLITON_TEST(cli_rejects_wrong_extension) {
    SOLITON_CHECK_EQ(usage_message(parse({"check", "main.cpp"})),
                   std::string{"expected a `.sn` source file, found `main.cpp`"});
}

SOLITON_TEST(cli_rejects_missing_input) {
    SOLITON_CHECK_EQ(usage_message(parse({"build"})),
                   std::string{"`build` requires an input file"});
    SOLITON_CHECK_EQ(usage_message(parse({"run"})), std::string{"`run` requires an input file"});
}

SOLITON_TEST(cli_rejects_dangling_output_flag) {
    SOLITON_CHECK_EQ(usage_message(parse({"build", "main.sn", "-o"})),
                   std::string{"`-o` requires a path argument"});
}

SOLITON_TEST(cli_rejects_extra_arguments) {
    SOLITON_CHECK_EQ(usage_message(parse({"run", "a.sn", "b.sn"})),
                   std::string{"unexpected extra argument `b.sn`"});
}

SOLITON_TEST(cli_default_output_path_replaces_the_source_extension) {
    const fs::path output = soliton::cli::default_output_path(fs::path{"examples/hello.sn"});
    SOLITON_CHECK(output.extension() != source_extension());
    SOLITON_CHECK_EQ(output.stem().string(), std::string{"hello"});
}

int main(int argc, char** argv) {
    std::string_view filter;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        constexpr std::string_view kFilterPrefix = "--filter=";
        if (arg.starts_with(kFilterPrefix)) {
            filter = arg.substr(kFilterPrefix.size());
        }
    }
    return soliton::test::run_all(filter);
}
