// A very small test registry, so the project stays dependency-free.
//
// §2 asks for golden-file snapshot tests driven by CTest, not for a
// particular framework. This is the minimum that gives named tests,
// readable failure output and a non-zero exit code - roughly the subset
// of Catch2 the compiler actually needs.

#ifndef EMBER_TEST_HARNESS_HPP
#define EMBER_TEST_HARNESS_HPP

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ember::test {

/// Thrown by a failing assertion and caught by the runner.
class AssertionFailure : public std::exception {
public:
    explicit AssertionFailure(std::string message) : message_(std::move(message)) {}
    const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

struct TestCase {
    std::string_view name;
    std::function<void()> body;
};

/// The registry is a function-local static so registration order across
/// translation units stays well defined.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(std::string_view name, std::function<void()> body) {
        registry().push_back(TestCase{name, std::move(body)});
    }
};

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
    std::ostringstream out;
    out << file << ":" << line << ": " << message;
    throw AssertionFailure(out.str());
}

/// Renders a value for a failure message, falling back to a placeholder
/// for types that have no operator<<.
template <typename T>
std::string describe(const T& value) {
    if constexpr (requires(std::ostream& os) { os << value; }) {
        std::ostringstream out;
        out << value;
        return out.str();
    } else {
        return "<value>";
    }
}

/// Run every registered test whose name contains `filter`.
/// Returns a process exit code.
inline int run_all(std::string_view filter = {}) {
    int passed = 0;
    std::vector<std::string> failures;

    for (const TestCase& test : registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string_view::npos) {
            continue;
        }
        try {
            test.body();
            ++passed;
            std::cout << "ok   " << test.name << '\n';
        } catch (const AssertionFailure& failure) {
            std::cout << "FAIL " << test.name << '\n' << "     " << failure.what() << '\n';
            failures.emplace_back(std::string{test.name});
        } catch (const std::exception& error) {
            std::cout << "FAIL " << test.name << '\n'
                      << "     unexpected exception: " << error.what() << '\n';
            failures.emplace_back(std::string{test.name});
        }
    }

    std::cout << "\n" << passed << " passed, " << failures.size() << " failed\n";
    for (const std::string& name : failures) {
        std::cout << "  failed: " << name << '\n';
    }
    return failures.empty() ? 0 : 1;
}

}  // namespace ember::test

#define EMBER_TEST_CONCAT_INNER(a, b) a##b
#define EMBER_TEST_CONCAT(a, b) EMBER_TEST_CONCAT_INNER(a, b)

/// Define a test: EMBER_TEST(golden_directory_has_cases) { ... }
#define EMBER_TEST(name)                                                            \
    static void name();                                                             \
    static const ::ember::test::Registrar EMBER_TEST_CONCAT(name, _registrar){#name, \
                                                                              &name}; \
    static void name()

#define EMBER_CHECK(condition)                                                      \
    do {                                                                            \
        if (!(condition)) {                                                         \
            ::ember::test::fail(__FILE__, __LINE__, "expected: " #condition);       \
        }                                                                           \
    } while (false)

#define EMBER_CHECK_MSG(condition, message)                                         \
    do {                                                                            \
        if (!(condition)) {                                                         \
            ::ember::test::fail(__FILE__, __LINE__,                                 \
                                std::string("expected: " #condition "\n     ") +    \
                                    (message));                                     \
        }                                                                           \
    } while (false)

#define EMBER_CHECK_EQ(actual, expected)                                            \
    do {                                                                            \
        const auto& ember_actual = (actual);                                        \
        const auto& ember_expected = (expected);                                    \
        if (!(ember_actual == ember_expected)) {                                    \
            ::ember::test::fail(__FILE__, __LINE__,                                 \
                                "expected `" #actual "` == `" #expected "`\n" \
                                "       actual: " +                                 \
                                    ::ember::test::describe(ember_actual) +         \
                                    "\n     expected: " +                           \
                                    ::ember::test::describe(ember_expected));       \
        }                                                                           \
    } while (false)

#endif  // EMBER_TEST_HARNESS_HPP
