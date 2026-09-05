// ember::manifest - the registry index.
//
// A registry is a directory of index files, one per package, named after
// it. `textkit.toml` says where each published version lives:
//
//     # textkit.toml
//     [0.1.0]
//     git = "https://example.invalid/textkit"
//     rev = "v0.1.0"
//
//     [0.2.0]
//     git = "https://example.invalid/textkit"
//     rev = "v0.2.0"
//
// That is the whole format. A version resolves to a git dependency, so
// everything downstream — fetching, locking, building — is machinery
// that already exists; the index only decides *which* commit.
//
// The directory can be a path or a git repository, which is how
// crates.io's index works and means a registry needs no server: a git
// host is enough to publish one, and a directory is enough to test one.
//
// Reading is an interface rather than a function so resolution can be
// tested without a filesystem or a network behind it.

#ifndef EMBER_MANIFEST_REGISTRY_HPP
#define EMBER_MANIFEST_REGISTRY_HPP

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/span.hpp"
#include "ember/manifest/version.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ember::manifest {

/// The file extension an index entry is written in.
inline constexpr std::string_view kIndexExtension = ".toml";

/// One published version of one package.
struct Release {
    Version version;
    /// The repository it lives in.
    std::string git;
    /// The tag or commit within it.
    std::string rev;
};

/// Every published version of one package, in the order the index
/// listed them.
struct IndexEntry {
    std::string name;
    std::vector<Release> releases;

    /// The highest version this entry has that `allow` accepts, or
    /// nothing if it has none.
    ///
    /// Highest rather than lowest, because a requirement says what is
    /// *compatible*, and among compatible versions the newest is the one
    /// with the fixes in it.
    const Release* best(const std::function<bool(const Version&)>& allow) const;
};

struct IndexResult {
    std::optional<IndexEntry> entry;
    std::vector<ast::Diagnostic> diagnostics;

    bool ok() const noexcept { return entry.has_value() && diagnostics.empty(); }
};

/// Parse one package's index file. `name` is what the package is called;
/// a file that disagrees with its own name is an error, since an index
/// that can lie about that can serve anything for anything.
IndexResult parse_index_entry(const ast::SourceFile& source, const std::string& name);

/// What an index entry lookup produced.
struct IndexLookup {
    /// Empty when the package is not in the index at all.
    std::optional<IndexEntry> entry;
    /// Set when the lookup itself failed - no registry configured, the
    /// index could not be fetched, the file would not parse.
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

/// Looks a package up in whatever registry is configured.
///
/// Injected rather than called directly, so resolution can be tested
/// against an index that exists only in the test.
using IndexReader = std::function<IndexLookup(const std::string& name)>;

// ---------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------

/// Renders a package's index file: every release, lowest version first.
///
/// The whole file is rewritten rather than appended to, so the ordering
/// is canonical and two people publishing different versions produce a
/// diff of one section rather than a conflict. An index file is
/// machine-written; a comment put in one by hand does not survive.
std::string render_index_entry(const std::string& name, std::vector<Release> releases);

struct PublishResult {
    /// The index file as it should now read.
    std::string contents;
    std::vector<ast::Diagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

/// Adds `release` to `existing`, which may be an empty file for a
/// package nobody has published before.
///
/// Republishing a version is refused. A version that already means one
/// thing must go on meaning it: everyone who locked it did so on the
/// understanding that it would not change underneath them.
PublishResult add_release(const ast::SourceFile& existing, const std::string& name,
                          const Release& release);

}  // namespace ember::manifest

#endif  // EMBER_MANIFEST_REGISTRY_HPP
