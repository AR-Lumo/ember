// ember::manifest - `ember.toml`, dependency resolution, and the
// lockfile that makes a build repeatable.
//
// A package is a directory with an `ember.toml` in it and its modules in
// `src/`. Depending on one puts that `src/` on the module search path,
// which is all a dependency has ever been here — the search path landed
// first precisely so this layer would have somewhere to put its answer.
//
//     [package]
//     name = "myapp"
//     version = "0.1.0"
//
//     [registry]
//     index = "https://example.invalid/ember-index"
//
//     [dependencies]
//     serde = "1.0.0"
//     textkit = { path = "../textkit" }
//     httpkit = { git = "https://example.invalid/httpkit", rev = "v1.2.0" }
//
// Three kinds of dependency:
//
//   * a bare version string is a **registry** dependency, looked up in
//     the configured index and resolved to a published version.
//   * `path` is a directory on this machine, resolved relative to the
//     manifest that named it. Nothing is copied or fetched.
//   * `git` is a repository and a revision — a tag, a branch or a commit.
//     It is cloned once into a cache and reused after that.
//
// **How versions are chosen.** Each package gets one version: the
// highest the index has that satisfies every requirement written against
// it, found by walking the graph to a fixpoint as requirements
// accumulate. It does not backtrack — it will not try a lower version of
// one package to make room for another — so a graph that would need that
// is reported rather than solved, naming every requirement and who wrote
// it. A small ecosystem is better served by being told than by a solver
// that takes a minute to say the same thing.
//
// A path or git dependency has no version to choose: it is pinned by
// where it came from. Wanting one package both ways is a conflict.
//
// `ember.lock` records the version and the exact commit each dependency
// resolved to, so a manifest that asks for a branch — or for a range
// somebody has since published into — keeps building the same code until
// somebody asks for it not to.

#ifndef EMBER_MANIFEST_MANIFEST_HPP
#define EMBER_MANIFEST_MANIFEST_HPP

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/span.hpp"
#include "ember/manifest/registry.hpp"
#include "ember/manifest/version.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ember::manifest {

/// The file a package declares itself in.
inline constexpr std::string_view kManifestName = "ember.toml";

/// The file recording what a build actually used.
inline constexpr std::string_view kLockName = "ember.lock";

/// Where a package's modules live inside it.
inline constexpr std::string_view kSourceDirectory = "src";

/// Where a dependency comes from.
enum class SourceKind {
    /// A directory on this machine.
    Path,
    /// A git repository, cloned into the build cache.
    Git,
    /// A published version, looked up in the registry index. Resolves to
    /// a git repository, so everything past resolution is the same.
    Registry,
};

std::string_view source_kind_name(SourceKind kind) noexcept;

struct Dependency {
    std::string name;
    SourceKind kind = SourceKind::Path;
    /// A directory for Path, a repository URL for Git.
    std::string location;
    /// The tag, branch or commit wanted. Git only, and required: a
    /// dependency with no revision is a dependency on whatever somebody
    /// pushed this morning.
    std::string rev;
    /// What versions are acceptable. Registry only.
    Requirement requirement;
    /// Where it was written, so a conflict can point at both sides.
    ast::Span span;
};

struct Manifest {
    std::string name;
    std::string version;
    /// `[registry] index = "..."`: a directory or a git repository
    /// holding one index file per package. Empty when none is
    /// configured, which is not an error until something needs one.
    std::string registry_index;
    /// The `ember.toml` itself.
    std::filesystem::path path;
    /// Its directory: the package root.
    std::filesystem::path root;
    std::vector<Dependency> dependencies;

    /// Where this package's own modules live.
    std::filesystem::path source_directory() const {
        return root / std::filesystem::path{kSourceDirectory};
    }
};

struct ManifestResult {
    std::optional<Manifest> manifest;
    std::vector<ast::Diagnostic> diagnostics;

    bool ok() const noexcept { return manifest.has_value() && diagnostics.empty(); }
};

/// Parse one `ember.toml`. `source.path()` is used as the manifest's
/// location, so it has to be the real one.
ManifestResult parse_manifest(const ast::SourceFile& source);

/// Walk up from `start` looking for an `ember.toml`, the way every build
/// tool eventually learns to. Returns the first one found.
std::optional<std::filesystem::path> find_manifest(const std::filesystem::path& start);

// ---------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------

/// One dependency, once it is a directory on disk.
struct ResolvedPackage {
    std::string name;
    std::string version;
    /// The package root, holding its `ember.toml`.
    std::filesystem::path root;
    SourceKind kind = SourceKind::Path;
    /// The path or URL it came from, as written.
    std::string location;
    /// The exact commit a git dependency resolved to. Empty for a path
    /// dependency, which is whatever is in the directory right now.
    std::string resolved_rev;
    /// What the manifest asked for, before the lockfile pinned it.
    std::string requested_rev;
    /// The published version chosen, for a registry dependency.
    Version chosen;

    std::filesystem::path source_directory() const {
        return root / std::filesystem::path{kSourceDirectory};
    }
};

/// What a fetcher managed to do with a git dependency.
struct Fetched {
    std::filesystem::path root;
    /// The commit actually checked out, which is what gets locked.
    std::string rev;
    /// Set when the fetch failed; `root` is then meaningless.
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

/// Materializes a git dependency and says where it landed.
///
/// Injected rather than called directly so resolution can be tested
/// without a network, a clock, or a git binary.
using Fetcher = std::function<Fetched(const Dependency& dependency, const std::string& rev)>;

/// What a lockfile pins.
struct LockEntry {
    std::string name;
    SourceKind kind = SourceKind::Path;
    std::string location;
    std::string rev;
    /// The published version, for a registry entry. Pinning this is what
    /// keeps a new release from moving a build that did not ask for it.
    std::string version;
};

struct Lock {
    std::vector<LockEntry> entries;

    const LockEntry* find(std::string_view name) const;
};

/// Read `ember.lock`. A malformed one is an error rather than something
/// to shrug off: silently ignoring it would turn a repeatable build into
/// an unrepeatable one without saying so.
struct LockResult {
    Lock lock;
    std::vector<ast::Diagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

LockResult parse_lock(const ast::SourceFile& source);

/// Render a lockfile for what resolution settled on.
std::string write_lock(const std::vector<ResolvedPackage>& packages);

struct Resolution {
    /// Every dependency, transitively, in the order they were reached.
    /// The root package is not among them.
    std::vector<ResolvedPackage> packages;
    std::vector<ast::Diagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }

    /// The directories to add to the module search path, in order.
    std::vector<std::filesystem::path> search_path() const;
};

/// Resolve `root`'s dependencies, transitively.
///
/// The graph is walked breadth-first, and re-walked whenever a version
/// choice changes, until nothing changes. Each package is visited once
/// per round, so two packages depending on the same third is ordinary
/// and a cycle between packages terminates rather than spinning — the
/// same bargain the module loader already strikes. Requirements only
/// accumulate, so the version chosen for a package never rises and the
/// loop always settles.
///
/// `lock` pins what a previous build settled on: a version for a
/// registry dependency, a commit for a git one. A pin is honoured as
/// long as it still satisfies every requirement, which is what keeps a
/// build that says `rev = "main"` — or `"1.0.0"`, against an index that
/// has since grown a `1.1.0` — building the same code. Pass an empty
/// lock to resolve afresh.
///
/// `index` is only consulted for registry dependencies, so a program
/// that has none never needs a registry configured.
///
/// Manifests read along the way are added to `sources`, so a diagnostic
/// about a dependency's own manifest renders against the right file.
Resolution resolve(const Manifest& root, const Lock& lock, const Fetcher& fetch,
                   const IndexReader& index, ast::SourceMap& sources);

}  // namespace ember::manifest

#endif  // EMBER_MANIFEST_MANIFEST_HPP
