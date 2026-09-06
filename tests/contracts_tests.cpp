// Contracts: `requires` and `ensures` on a signature (section 10.1).
//
// A precondition is what the caller owes the function, checked before
// the body runs. A postcondition is what the function owes back,
// checked before every return, with `result` naming the value about to
// be returned.
//
// Three things here are easy to get wrong and are tested directly
// rather than assumed:
//
//   - `result` is not a keyword. Reserving it would break every
//     existing program with a variable by that name, for a word that
//     means something in exactly one place. It is an ordinary
//     identifier the checker binds while an `ensures` is in scope, so
//     the tests below cover both that it resolves there and that it
//     does not resolve anywhere else.
//
//   - The condition is parsed the way an `if` condition is, with
//     struct literals disallowed. Otherwise `requires b != 0 { ... }`
//     reads `0 { ... }` as a struct literal and swallows the body.
//
//   - An `ensures` runs before the drops at the end of a function, not
//     after, or a condition mentioning a local would read freed memory.

#include "test_harness.hpp"

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/nodes.hpp"
#include "ember/parser/parser.hpp"
#include "ember/typeck/typeck.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using ember::ast::SourceFile;
using ember::typeck::CheckResult;

SourceFile contract_source(std::string contents) {
    return SourceFile{"contracts.em", std::move(contents)};
}

ember::parser::ParseResult parse_contracts(const SourceFile& source) {
    return ember::parser::parse_source(source);
}

CheckResult check_contracts(const SourceFile& source) {
    ember::parser::ParseResult parsed = parse_contracts(source);
    if (!parsed.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "fixture does not parse:\n" +
                                ember::ast::render_all(parsed.diagnostics, source));
    }
    return ember::typeck::check(*parsed.program, source);
}

void accepts(const std::string& contents) {
    const SourceFile source = contract_source(contents);
    const CheckResult result = check_contracts(source);
    if (!result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this to type-check, but:\n" +
                                ember::ast::render_all(result.diagnostics, source));
    }
}

std::string rejects(const std::string& contents) {
    const SourceFile source = contract_source(contents);
    CheckResult result = check_contracts(source);
    if (result.ok()) {
        ::ember::test::fail(__FILE__, __LINE__,
                            "expected this to be rejected, but it type-checked");
    }
    return result.diagnostics.at(0).message;
}

namespace fs = std::filesystem;

struct RunResult {
    int exit_code = 0;
    std::string output;
};

/// Compile `contents` and run it, capturing stdout and stderr together.
///
/// The panic a violated contract raises is the whole point of the
/// feature, and it only exists in a built program - the checker is
/// happy with a contract that will fail, because whether it fails is a
/// question about values, not types.
RunResult build_and_run(const std::string& contents) {
    const fs::path directory =
        fs::temp_directory_path() / ("ember-contract-" + std::to_string(std::rand()));
    std::error_code failed;
    fs::create_directories(directory, failed);

    const fs::path source = directory / "main.em";
    {
        std::ofstream out{source};
        out << contents;
    }

    const std::string command = "\"" + fs::path{EMBER_BINARY}.string() + "\" run \"" +
                                source.string() + "\" 2>&1";
#ifdef _WIN32
    // cmd.exe eats the outer quotes of a command that starts with one.
    FILE* pipe = _popen(("\"" + command + "\"").c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        ::ember::test::fail(__FILE__, __LINE__, "cannot run: " + command);
    }

    RunResult result;
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output += buffer.data();
    }
#ifdef _WIN32
    result.exit_code = _pclose(pipe);
#else
    result.exit_code = pclose(pipe);
#endif

    fs::remove_all(directory, failed);
    return result;
}

/// The first function declared in a program.
const ember::ast::FunctionDecl& first_function(const ember::ast::Program& program) {
    for (const ember::ast::ItemPtr& item : program.items) {
        if (const auto* function = ember::ast::node_cast<ember::ast::FunctionDecl>(item.get())) {
            return *function;
        }
    }
    ::ember::test::fail(__FILE__, __LINE__, "no function in the program");
    throw 0;  // unreachable; fail() does not return
}

}  // namespace

EMBER_TEST(a_signature_carries_its_contracts_in_order) {
    const SourceFile source = contract_source(
        "pub fn divide(a: int, b: int) -> int\n"
        "    requires b != 0\n"
        "    ensures result != 0 || a == 0\n"
        "{\n"
        "    return a / b;\n"
        "}\n");
    const ember::parser::ParseResult parsed = parse_contracts(source);
    EMBER_CHECK(parsed.ok());

    const ember::ast::FunctionDecl& function = first_function(*parsed.program);
    EMBER_CHECK_EQ(function.contracts.size(), std::size_t{2});
    EMBER_CHECK(function.contracts[0].kind == ember::ast::ContractKind::Requires);
    EMBER_CHECK(function.contracts[1].kind == ember::ast::ContractKind::Ensures);
    EMBER_CHECK(function.has_ensures());

    // The text is captured at parse time because codegen is handed no
    // source, and a violated contract has to print what it said.
    EMBER_CHECK_EQ(function.contracts[0].text, std::string{"b != 0"});
    EMBER_CHECK_EQ(function.contracts[1].text, std::string{"result != 0 || a == 0"});
    EMBER_CHECK_EQ(function.contracts[0].location, std::string{"contracts.em:2:5"});
}

EMBER_TEST(a_contract_does_not_swallow_the_function_body) {
    // The struct-literal trap. If the condition were parsed as an
    // ordinary expression, `0 {` would begin a struct literal and the
    // body would vanish into it - with no syntax error to show for it.
    const SourceFile source = contract_source(
        "pub fn f(b: int) -> int\n"
        "    requires b != 0\n"
        "{\n"
        "    return b;\n"
        "}\n");
    const ember::parser::ParseResult parsed = parse_contracts(source);
    EMBER_CHECK(parsed.ok());

    const ember::ast::FunctionDecl& function = first_function(*parsed.program);
    EMBER_CHECK_EQ(function.contracts.size(), std::size_t{1});
    EMBER_CHECK(function.has_body);
    EMBER_CHECK_EQ(function.body.statements.size(), std::size_t{1});
}

EMBER_TEST(a_function_may_promise_nothing) {
    const SourceFile source = contract_source("pub fn f() -> int { return 1; }\n");
    const ember::parser::ParseResult parsed = parse_contracts(source);
    EMBER_CHECK(parsed.ok());
    EMBER_CHECK(first_function(*parsed.program).contracts.empty());
    EMBER_CHECK(!first_function(*parsed.program).has_ensures());
}

EMBER_TEST(contracts_may_talk_about_parameters_and_the_result) {
    accepts(
        "pub fn divide(a: int, b: int) -> int\n"
        "    requires b != 0\n"
        "    ensures result != 0 || a == 0\n"
        "{\n"
        "    return a / b;\n"
        "}\n"
        "pub fn main() { println(divide(4, 2)); }\n");
}

EMBER_TEST(a_method_may_carry_contracts_and_see_self) {
    accepts(
        "struct Counter { pub n: int, }\n"
        "impl Counter {\n"
        "    pub fn stepped(&self, by: int) -> int\n"
        "        requires by > 0\n"
        "        ensures result > self.n\n"
        "    {\n"
        "        return self.n + by;\n"
        "    }\n"
        "}\n"
        "pub fn main() {\n"
        "    let c = Counter { n: 1 };\n"
        "    println(c.stepped(2));\n"
        "}\n");
}

EMBER_TEST(a_function_returning_nothing_may_still_require) {
    accepts(
        "pub fn shout(times: int)\n"
        "    requires times > 0\n"
        "{\n"
        "    println(times);\n"
        "}\n"
        "pub fn main() { shout(1); }\n");
}

EMBER_TEST(result_is_not_a_keyword) {
    // The reason it is bound by the checker rather than reserved by the
    // lexer: reserving it would break programs like this one, which
    // have every right to exist.
    accepts(
        "pub fn main() {\n"
        "    let result = 7;\n"
        "    println(result);\n"
        "}\n");
}

EMBER_TEST(result_is_out_of_scope_in_a_requires) {
    const std::string message = rejects(
        "pub fn f(x: int) -> int\n"
        "    requires result > 0\n"
        "{\n"
        "    return x;\n"
        "}\n");
    EMBER_CHECK_EQ(message, std::string{"`result` is not in scope in a `requires`"});
}

EMBER_TEST(result_is_out_of_scope_when_nothing_is_returned) {
    const std::string message = rejects(
        "pub fn f(x: int)\n"
        "    ensures result == x\n"
        "{\n"
        "}\n");
    EMBER_CHECK_EQ(message,
                   std::string{"this function returns nothing, so it has no `result`"});
}

EMBER_TEST(result_is_out_of_scope_in_the_body) {
    // It belongs to the clause, not the function. Inside the body it is
    // just an undefined name, which is what it should be.
    const std::string message = rejects(
        "pub fn f(x: int) -> int\n"
        "    ensures result > 0\n"
        "{\n"
        "    return result;\n"
        "}\n");
    EMBER_CHECK(message != std::string{"`result` is not in scope in a `requires`"});
}

EMBER_TEST(a_contract_must_be_a_bool) {
    EMBER_CHECK_EQ(rejects("pub fn f(x: int) -> int\n"
                           "    requires x\n"
                           "{\n"
                           "    return x;\n"
                           "}\n"),
                   std::string{"a contract must be a `bool`"});

    EMBER_CHECK_EQ(rejects("pub fn f(x: int) -> int\n"
                           "    ensures result\n"
                           "{\n"
                           "    return x;\n"
                           "}\n"),
                   std::string{"a contract must be a `bool`"});
}

EMBER_TEST(a_contract_cannot_see_a_local) {
    // A contract belongs to the signature, so it sees what the
    // signature sees. A local is not part of the promise.
    const std::string message = rejects(
        "pub fn f() -> int\n"
        "    ensures result > hidden\n"
        "{\n"
        "    let hidden = 1;\n"
        "    return 2;\n"
        "}\n");
    EMBER_CHECK(!message.empty());
}

EMBER_TEST(a_contract_is_type_checked_like_any_expression) {
    // Not waved through: an unknown name or a bad comparison inside a
    // clause is an error like anywhere else.
    EMBER_CHECK(!rejects("pub fn f(x: int) -> int\n"
                         "    requires nonexistent > 0\n"
                         "{\n"
                         "    return x;\n"
                         "}\n")
                     .empty());

    EMBER_CHECK(!rejects("pub fn f(x: int) -> int\n"
                         "    requires x > \"text\"\n"
                         "{\n"
                         "    return x;\n"
                         "}\n")
                     .empty());
}

EMBER_TEST(a_declaration_without_a_body_may_carry_contracts) {
    // What an interface file is made of. The clauses are part of the
    // signature, so they survive being written out and read back.
    const SourceFile source = contract_source(
        "pub fn divide(a: int, b: int) -> int\n"
        "    requires b != 0\n"
        "    ensures result != 0 || a == 0;\n");
    const ember::parser::ParseResult parsed = parse_contracts(source);
    EMBER_CHECK(parsed.ok());

    const ember::ast::FunctionDecl& function = first_function(*parsed.program);
    EMBER_CHECK(!function.has_body);
    EMBER_CHECK_EQ(function.contracts.size(), std::size_t{2});
}


EMBER_TEST(a_satisfied_contract_does_not_get_in_the_way) {
    const RunResult run = build_and_run(
        "pub fn divide(a: int, b: int) -> int\n"
        "    requires b != 0\n"
        "    ensures result != 0 || a == 0\n"
        "{\n"
        "    return a / b;\n"
        "}\n"
        "pub fn main() {\n"
        "    println(divide(84, 2));\n"
        // The `ensures` permits a zero result when the numerator was zero,
        // which is the case that would fail a naive `result != 0`.
        "    println(divide(0, 7));\n"
        "}\n");

    EMBER_CHECK_MSG(run.exit_code == 0, run.output);
    EMBER_CHECK_MSG(run.output.find("42") != std::string::npos, run.output);
}

EMBER_TEST(a_violated_requires_panics_and_names_the_clause) {
    const RunResult run = build_and_run(
        "pub fn divide(a: int, b: int) -> int\n"
        "    requires b != 0\n"
        "{\n"
        "    return a / b;\n"
        "}\n"
        "pub fn main() { println(divide(1, 0)); }\n");

    EMBER_CHECK_MSG(run.exit_code != 0, "expected a panic, got:\n" + run.output);
    EMBER_CHECK_MSG(run.output.find("requires contract violated") != std::string::npos,
                    run.output);
    // The condition as written, so the message says which clause failed
    // rather than merely that one did.
    EMBER_CHECK_MSG(run.output.find("b != 0") != std::string::npos, run.output);
    EMBER_CHECK_MSG(run.output.find("divide") != std::string::npos, run.output);
}

EMBER_TEST(a_requires_is_checked_before_the_body_runs) {
    // If the check came after, the division would fault first and the
    // contract would be pointless.
    const RunResult run = build_and_run(
        "pub fn divide(a: int, b: int) -> int\n"
        "    requires b != 0\n"
        "{\n"
        "    return a / b;\n"
        "}\n"
        "pub fn main() { println(divide(1, 0)); }\n");

    EMBER_CHECK_MSG(run.output.find("requires contract violated") != std::string::npos,
                    run.output);
    EMBER_CHECK_MSG(run.output.find("divide by zero") == std::string::npos,
                    "the body ran before the precondition:\n" + run.output);
}

EMBER_TEST(a_violated_ensures_panics) {
    const RunResult run = build_and_run(
        "pub fn wrong(x: int) -> int\n"
        "    ensures result > x\n"
        "{\n"
        "    return x - 1;\n"
        "}\n"
        "pub fn main() { println(wrong(5)); }\n");

    EMBER_CHECK_MSG(run.exit_code != 0, "expected a panic, got:\n" + run.output);
    EMBER_CHECK_MSG(run.output.find("ensures contract violated") != std::string::npos,
                    run.output);
    // And nothing was printed: the check happens before the value gets
    // back to the caller, not after.
    EMBER_CHECK_MSG(run.output.find("4") == std::string::npos, run.output);
}

EMBER_TEST(every_return_is_checked_not_only_the_last) {
    const RunResult run = build_and_run(
        "pub fn early(x: int) -> int\n"
        "    ensures result >= 0\n"
        "{\n"
        "    if x < 0 {\n"
        "        return x;\n"
        "    }\n"
        "    return x;\n"
        "}\n"
        "pub fn main() { println(early(0 - 1)); }\n");

    EMBER_CHECK_MSG(run.exit_code != 0,
                    "the early return skipped its postcondition:\n" + run.output);
    EMBER_CHECK_MSG(run.output.find("ensures contract violated") != std::string::npos,
                    run.output);
}

EMBER_TEST(an_ensures_may_read_an_owned_return_value) {
    // Ordering: the check runs with the value in hand but before the
    // drops at the end of the function. Were it the other way round the
    // condition would read freed memory, which is the sort of bug that
    // passes on a good day.
    const RunResult run = build_and_run(
        "pub fn build() -> String\n"
        "    ensures len(result) > 0\n"
        "{\n"
        "    let mut out: String = new_string();\n"
        "    push_str(out, \"ok\");\n"
        "    return out;\n"
        "}\n"
        "pub fn main() { println(build()); }\n");

    EMBER_CHECK_MSG(run.exit_code == 0, run.output);
    EMBER_CHECK_MSG(run.output.find("ok") != std::string::npos, run.output);
}
