// ember::manifest - versions and the requirements written against them.
//
// Semantic versions, `major.minor.patch`, and two ways to ask for one:
//
//     textkit = "1.2.3"     the default, a caret requirement
//     textkit = "^1.2.3"    the same thing, said out loud
//     textkit = "=1.2.3"    that version and no other
//
// A caret allows anything up to the next incompatible release, where
// "incompatible" is decided by the leftmost non-zero component — so
// `^1.2.3` allows `1.9.0` but not `2.0.0`, and `^0.2.3` allows `0.2.9`
// but not `0.3.0`, because before 1.0 the minor is where breakage
// lives. That is Cargo's rule, and it is the one people expect.
//
// There are no ranges, no unions, no wildcards and no pre-release tags.
// Each is a real feature with real semantics to get wrong, and none of
// them is needed to say "this version or a compatible one", which is
// what a requirement is for. Anything else is refused by name.

#ifndef EMBER_MANIFEST_VERSION_HPP
#define EMBER_MANIFEST_VERSION_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ember::manifest {

struct Version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;

    /// Parses `1`, `1.2` or `1.2.3`; missing components are zero.
    /// Returns nothing if it is not a version at all.
    static std::optional<Version> parse(std::string_view text);

    std::string to_string() const;

    friend auto operator<=>(const Version&, const Version&) = default;
    friend bool operator==(const Version&, const Version&) = default;
};

/// What a dependency will accept.
class Requirement {
public:
    enum class Kind {
        /// This version or anything compatible with it.
        Caret,
        /// Exactly this version.
        Exact,
    };

    Requirement() = default;
    Requirement(Kind kind, Version version) : kind_(kind), version_(version) {}

    /// Parses `1.2.3`, `^1.2.3` or `=1.2.3`. Returns nothing for
    /// anything else, including the range syntaxes Ember does not have.
    static std::optional<Requirement> parse(std::string_view text);

    bool allows(const Version& version) const;

    /// As it would be written in a manifest.
    std::string to_string() const;

    Kind kind() const noexcept { return kind_; }
    const Version& version() const noexcept { return version_; }

    friend bool operator==(const Requirement&, const Requirement&) = default;

private:
    Kind kind_ = Kind::Caret;
    Version version_;
};

}  // namespace ember::manifest

#endif  // EMBER_MANIFEST_VERSION_HPP
