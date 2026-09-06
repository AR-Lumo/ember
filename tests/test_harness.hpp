// A very small test registry, so the project stays dependency-free.
//
// §2 asks for golden-file snapshot tests driven by CTest, not for a
// particular framework. This is the minimum that gives named tests,
// readable failure output and a non-zero exit code - roughly the subset
// of Catch2 the compiler actually needs.

#ifndef CINDER_TEST_HARNESS_HPP
#define CINDER_TEST_HARNESS_HPP

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace cinder::test {

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

}  // namespace cinder::test

#define CINDER_TEST_CONCAT_INNER(a, b) a##b
#define CINDER_TEST_CONCAT(a, b) CINDER_TEST_CONCAT_INNER(a, b)

/// Define a test: CINDER_TEST(golden_directory_has_cases) { ... }
#define CINDER_TEST(name)                                                            \
    static void name();                                                             \
    static const ::cinder::test::Registrar CINDER_TEST_CONCAT(name, _registrar){#name, \
                                                                              &name}; \
    static void name()

#define CINDER_CHECK(condition)                                                      \
    do {                                                                            \
        if (!(condition)) {                                                         \
            ::cinder::test::fail(__FILE__, __LINE__, "expected: " #condition);       \
        }                                                                           \
    } while (false)

#define CINDER_CHECK_MSG(condition, message)                                         \
    do {                                                                            \
        if (!(condition)) {                                                         \
            ::cinder::test::fail(__FILE__, __LINE__,                                 \
                                std::string("expected: " #condition "\n     ") +    \
                                    (message));                                     \
        }                                                                           \
    } while (false)

/// Compares two values, reporting both when they differ.
///
/// The operands are taken **by value**, not by reference. Binding a
/// reference here is a trap that has been sprung three times: an
/// expression like `f().things.at(0)` reaches into a temporary that dies
/// at the end of the declaration, and the comparison then reads freed
/// memory - which shows up as a garbled failure message rather than as
/// anything that points at the mistake. A copy costs nothing a test will
/// notice.
#define CINDER_CHECK_EQ(actual, expected)                                            \
    do {                                                                            \
        const auto cinder_actual = (actual);                                         \
        const auto cinder_expected = (expected);                                     \
        if (!(cinder_actual == cinder_expected)) {                                    \
            ::cinder::test::fail(__FILE__, __LINE__,                                 \
                                "expected `" #actual "` == `" #expected "`\n" \
                                "       actual: " +                                 \
                                    ::cinder::test::describe(cinder_actual) +         \
                                    "\n     expected: " +                           \
                                    ::cinder::test::describe(cinder_expected));       \
        }                                                                           \
    } while (false)

#endif  // CINDER_TEST_HARNESS_HPP
