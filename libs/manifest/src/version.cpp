#include "cinder/manifest/version.hpp"

#include <cctype>

namespace cinder::manifest {
namespace {

/// Reads digits at `at`, or nothing if there are none. Leading zeros are
/// allowed: `1.0.0` and `1.00.0` are the same version, and refusing the
/// second would be pedantry rather than safety.
std::optional<std::uint32_t> read_number(std::string_view text, std::size_t& at) {
    const std::size_t start = at;
    std::uint64_t value = 0;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0) {
        value = value * 10 + static_cast<std::uint64_t>(text[at] - '0');
        if (value > 0xffffffffULL) {
            return std::nullopt;  // no version needs four billion of anything
        }
        ++at;
    }
    if (at == start) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

}  // namespace

std::optional<Version> Version::parse(std::string_view text) {
    std::size_t at = 0;
    Version version;

    const std::optional<std::uint32_t> major = read_number(text, at);
    if (!major.has_value()) {
        return std::nullopt;
    }
    version.major = *major;

    if (at < text.size() && text[at] == '.') {
        ++at;
        const std::optional<std::uint32_t> minor = read_number(text, at);
        if (!minor.has_value()) {
            return std::nullopt;
        }
        version.minor = *minor;

        if (at < text.size() && text[at] == '.') {
            ++at;
            const std::optional<std::uint32_t> patch = read_number(text, at);
            if (!patch.has_value()) {
                return std::nullopt;
            }
            version.patch = *patch;
        }
    }

    // Anything left over is a syntax this does not have: a pre-release
    // tag, build metadata, a wildcard, a second bound.
    if (at != text.size()) {
        return std::nullopt;
    }
    return version;
}

std::string Version::to_string() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

std::optional<Requirement> Requirement::parse(std::string_view text) {
    Kind kind = Kind::Caret;
    if (!text.empty() && (text.front() == '^' || text.front() == '=')) {
        kind = text.front() == '=' ? Kind::Exact : Kind::Caret;
        text.remove_prefix(1);
    }

    const std::optional<Version> version = Version::parse(text);
    if (!version.has_value()) {
        return std::nullopt;
    }
    return Requirement{kind, *version};
}

bool Requirement::allows(const Version& version) const {
    if (kind_ == Kind::Exact) {
        return version == version_;
    }
    if (version < version_) {
        return false;
    }

    // The upper bound is the next release that may break: the leftmost
    // non-zero component decides which one that is, because before 1.0
    // the minor is where breakage lives.
    if (version_.major != 0) {
        return version.major == version_.major;
    }
    if (version_.minor != 0) {
        return version.major == 0 && version.minor == version_.minor;
    }
    if (version_.patch != 0) {
        return version.major == 0 && version.minor == 0 && version.patch == version_.patch;
    }
    // `^0.0.0` allows anything below 1.0.0, which is the only reading
    // that is not simply "nothing".
    return version.major == 0;
}

std::string Requirement::to_string() const {
    return (kind_ == Kind::Exact ? "=" : "^") + version_.to_string();
}

}  // namespace cinder::manifest
