#include "soliton/manifest/manifest.hpp"

#include "soliton/manifest/toml.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <system_error>

namespace soliton::manifest {
namespace {

using ast::Diagnostic;

Diagnostic& push(std::vector<Diagnostic>& into, std::string message, ast::Span span,
                 std::string label) {
    into.push_back(Diagnostic::error(std::move(message), span, std::move(label)));
    return into.back();
}

/// A name that can be a module, a directory and a linker symbol at once.
bool is_valid_package_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    if (std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
        return false;
    }
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string_view source_kind_name(SourceKind kind) noexcept {
    switch (kind) {
        case SourceKind::Git:
            return "git";
        case SourceKind::Registry:
            return "registry";
        case SourceKind::Path:
            break;
    }
    return "path";
}

const LockEntry* Lock::find(std::string_view name) const {
    for (const LockEntry& entry : entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<std::filesystem::path> Resolution::search_path() const {
    std::vector<std::filesystem::path> directories;
    directories.reserve(packages.size());
    for (const ResolvedPackage& package : packages) {
        directories.push_back(package.source_directory());
    }
    return directories;
}

// ---------------------------------------------------------------------
// Parsing a manifest
// ---------------------------------------------------------------------

namespace {

/// Reads one `[dependencies]` entry into a Dependency.
std::optional<Dependency> read_dependency(const toml::Entry& entry,
                                          std::vector<Diagnostic>& diagnostics) {
    // A bare string is a version requirement, looked up in the registry.
    if (!entry.is_table) {
        const std::optional<Requirement> requirement = Requirement::parse(entry.value);
        if (!requirement.has_value()) {
            push(diagnostics, "`" + entry.value + "` is not a version requirement",
                 entry.value_span, "expected something like `1.2.3`, `^1.2.3` or `=1.2.3`")
                .with_note("soliton has carets and exact versions; there are no ranges, "
                           "wildcards or pre-release tags")
                .with_note("for a dependency that is not published, write `" + entry.key +
                           " = { path = \"../" + entry.key + "\" }` or `{ git = \"...\", "
                           "rev = \"...\" }`");
            return std::nullopt;
        }

        Dependency dependency;
        dependency.name = entry.key;
        dependency.kind = SourceKind::Registry;
        dependency.requirement = *requirement;
        dependency.span = entry.key_span;
        return dependency;
    }

    const toml::Entry* path = entry.find("path");
    const toml::Entry* git = entry.find("git");

    if (path != nullptr && git != nullptr) {
        push(diagnostics, "dependency `" + entry.key + "` has two sources", git->key_span,
             "`path` and `git` cannot both be given")
            .with_note("a dependency comes from exactly one place");
        return std::nullopt;
    }
    if (path == nullptr && git == nullptr) {
        push(diagnostics, "dependency `" + entry.key + "` has no source", entry.value_span,
             "expected `path` or `git`");
        return std::nullopt;
    }

    Dependency dependency;
    dependency.name = entry.key;
    dependency.span = entry.key_span;

    if (path != nullptr) {
        dependency.kind = SourceKind::Path;
        dependency.location = path->value;
        if (const toml::Entry* rev = entry.find("rev")) {
            push(diagnostics, "a path dependency cannot have a revision", rev->key_span,
                 "`rev` only means something for `git`")
                .with_note("a path dependency is whatever is in the directory right now");
            return std::nullopt;
        }
        if (dependency.location.empty()) {
            push(diagnostics, "dependency `" + entry.key + "` has an empty path",
                 path->value_span, "expected a directory");
            return std::nullopt;
        }
        return dependency;
    }

    dependency.kind = SourceKind::Git;
    dependency.location = git->value;
    if (dependency.location.empty()) {
        push(diagnostics, "dependency `" + entry.key + "` has an empty git url",
             git->value_span, "expected a repository");
        return std::nullopt;
    }

    const toml::Entry* rev = entry.find("rev");
    if (rev == nullptr || rev->value.empty()) {
        // Without one, the build depends on whoever pushed last.
        push(diagnostics, "git dependency `" + entry.key + "` has no revision",
             entry.value_span, "expected `rev = \"...\"`")
            .with_note("a tag, a branch or a commit; without one the build is whatever the "
                       "repository happened to contain");
        return std::nullopt;
    }
    dependency.rev = rev->value;

    for (const toml::Entry& field : entry.entries) {
        if (field.key != "git" && field.key != "rev") {
            push(diagnostics, "unknown key `" + field.key + "` in a git dependency",
                 field.key_span, "expected `git` or `rev`");
            return std::nullopt;
        }
    }
    return dependency;
}

}  // namespace

ManifestResult parse_manifest(const ast::SourceFile& source) {
    ManifestResult result;

    toml::ParseResult parsed = toml::parse(source);
    result.diagnostics = std::move(parsed.diagnostics);

    Manifest manifest;
    manifest.path = std::filesystem::path{source.path()};
    manifest.root = manifest.path.parent_path();

    const toml::Table* package = parsed.document.find("package");
    if (package == nullptr) {
        push(result.diagnostics, "manifest has no `[package]` section",
             ast::Span{0, 0, source.id()}, "every package declares its name and version")
            .with_note("add:\n    [package]\n    name = \"...\"\n    version = \"0.1.0\"");
        return result;
    }

    const toml::Entry* name = package->find("name");
    if (name == nullptr) {
        push(result.diagnostics, "package has no name", package->span,
             "expected `name = \"...\"` in this section");
    } else if (!is_valid_package_name(name->value)) {
        push(result.diagnostics, "`" + name->value + "` is not a usable package name",
             name->value_span, "letters, digits and underscores, not starting with a digit")
            .with_note("a package name is also a module name, so it has to be an identifier");
    } else {
        manifest.name = name->value;
    }

    if (const toml::Entry* version = package->find("version")) {
        manifest.version = version->value;
    } else {
        push(result.diagnostics, "package has no version", package->span,
             "expected `version = \"...\"` in this section")
            .with_note("nothing solves for versions yet, but a package still has to say "
                       "which one it is");
    }

    for (const toml::Entry& entry : package->entries) {
        if (entry.key != "name" && entry.key != "version") {
            push(result.diagnostics, "unknown key `" + entry.key + "` in `[package]`",
                 entry.key_span, "expected `name` or `version`");
        }
    }

    if (const toml::Table* registry = parsed.document.find("registry")) {
        if (const toml::Entry* index = registry->find("index")) {
            manifest.registry_index = index->value;
            if (manifest.registry_index.empty()) {
                push(result.diagnostics, "registry index is empty", index->value_span,
                     "expected a directory or a git url");
            }
        } else {
            push(result.diagnostics, "`[registry]` has no index", registry->span,
                 "expected `index = \"...\"` in this section");
        }
        for (const toml::Entry& entry : registry->entries) {
            if (entry.key != "index") {
                push(result.diagnostics, "unknown key `" + entry.key + "` in `[registry]`",
                     entry.key_span, "expected `index`");
            }
        }
    }

    if (const toml::Table* dependencies = parsed.document.find("dependencies")) {
        for (const toml::Entry& entry : dependencies->entries) {
            if (entry.key == manifest.name) {
                push(result.diagnostics, "a package cannot depend on itself", entry.key_span,
                     "`" + entry.key + "` is this package");
                continue;
            }
            if (!is_valid_package_name(entry.key)) {
                push(result.diagnostics, "`" + entry.key + "` is not a usable package name",
                     entry.key_span,
                     "letters, digits and underscores, not starting with a digit");
                continue;
            }
            if (std::optional<Dependency> dependency =
                    read_dependency(entry, result.diagnostics)) {
                manifest.dependencies.push_back(std::move(*dependency));
            }
        }
    }

    // Anything else is a section this version does not understand, and
    // saying so beats letting somebody believe it took effect.
    for (const toml::Table& table : parsed.document.tables) {
        if (!table.name.empty() && table.name != "package" && table.name != "dependencies" &&
            table.name != "registry") {
            push(result.diagnostics, "unknown section `[" + table.name + "]`", table.span,
                 "expected `[package]`, `[dependencies]` or `[registry]`");
        }
    }

    result.manifest = std::move(manifest);
    return result;
}

std::vector<ast::Diagnostic> check_publishable(const Manifest& manifest) {
    std::vector<Diagnostic> diagnostics;

    if (!Version::parse(manifest.version).has_value()) {
        push(diagnostics, "`" + manifest.version + "` is not a version that can be published",
             ast::Span{}, "expected `major.minor.patch`")
            .with_note("a published version has to be one the registry can order and "
                       "compare");
    }

    for (const Dependency& dependency : manifest.dependencies) {
        if (dependency.kind == SourceKind::Path) {
            push(diagnostics,
                 "`" + manifest.name + "` cannot be published: it depends on a path",
                 dependency.span, "`" + dependency.name + "` is a directory on this machine")
                .with_note("nobody else has `" + dependency.location +
                           "`, so the package would not build for them")
                .with_note("publish `" + dependency.name +
                           "` too and depend on its version, or use a git dependency");
        }
    }
    return diagnostics;
}

std::optional<std::filesystem::path> find_manifest(const std::filesystem::path& start) {
    std::error_code code;
    std::filesystem::path directory = std::filesystem::absolute(start, code);
    if (code) {
        directory = start;
    }
    if (!std::filesystem::is_directory(directory, code)) {
        directory = directory.parent_path();
    }

    while (!directory.empty()) {
        const std::filesystem::path candidate =
            directory / std::filesystem::path{kManifestName};
        if (std::filesystem::is_regular_file(candidate, code)) {
            return candidate;
        }
        const std::filesystem::path parent = directory.parent_path();
        if (parent == directory) {
            break;  // reached the root
        }
        directory = parent;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------
// The lockfile
// ---------------------------------------------------------------------

LockResult parse_lock(const ast::SourceFile& source) {
    LockResult result;

    toml::ParseResult parsed = toml::parse(source);
    result.diagnostics = std::move(parsed.diagnostics);

    for (const toml::Table& table : parsed.document.tables) {
        if (table.name.empty()) {
            continue;
        }
        LockEntry entry;
        entry.name = table.name;

        const toml::Entry* source_field = table.find("source");
        const toml::Entry* location = table.find("location");
        if (source_field == nullptr || location == nullptr) {
            push(result.diagnostics, "incomplete lock entry for `" + table.name + "`",
                 table.span, "expected `source` and `location`")
                .with_note("delete `" + std::string{kLockName} + "` to build a fresh one");
            continue;
        }

        if (source_field->value == "git") {
            entry.kind = SourceKind::Git;
        } else if (source_field->value == "path") {
            entry.kind = SourceKind::Path;
        } else if (source_field->value == "registry") {
            entry.kind = SourceKind::Registry;
        } else {
            push(result.diagnostics, "unknown source `" + source_field->value + "`",
                 source_field->value_span, "expected `path`, `git` or `registry`");
            continue;
        }

        entry.location = location->value;
        if (const toml::Entry* rev = table.find("rev")) {
            entry.rev = rev->value;
        }
        if (const toml::Entry* version = table.find("version")) {
            entry.version = version->value;
        }
        result.lock.entries.push_back(std::move(entry));
    }
    return result;
}

std::string write_lock(const std::vector<ResolvedPackage>& packages) {
    // Sorted by name, so the file does not churn when the resolution
    // order changes but the answer does not.
    std::vector<const ResolvedPackage*> sorted;
    sorted.reserve(packages.size());
    for (const ResolvedPackage& package : packages) {
        sorted.push_back(&package);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const ResolvedPackage* a, const ResolvedPackage* b) {
                  return a->name < b->name;
              });

    std::string out =
        "# Generated by soliton. Check this in; do not edit it by hand.\n"
        "#\n"
        "# It records the exact commit each git dependency resolved to, so a\n"
        "# manifest that asks for a branch keeps building the same code.\n";

    for (const ResolvedPackage* package : sorted) {
        out += "\n[" + package->name + "]\n";
        out += "source = " + toml::quoted(source_kind_name(package->kind)) + "\n";
        out += "location = " + toml::quoted(package->location) + "\n";
        if (!package->resolved_rev.empty()) {
            out += "rev = " + toml::quoted(package->resolved_rev) + "\n";
        }
        if (!package->version.empty()) {
            out += "version = " + toml::quoted(package->version) + "\n";
        }
    }
    return out;
}

// ---------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------

namespace {

/// A dependency edge waiting to be walked, with enough context to blame
/// whoever asked for it.
struct Pending {
    Dependency dependency;
    /// The package that named it, for diagnostics and for resolving a
    /// relative path.
    std::string requested_by;
    std::filesystem::path requested_from;
};

/// One requirement written against a package, and who wrote it.
struct Demand {
    Requirement requirement;
    std::string requested_by;
    ast::Span span;
};

std::string describe(const Dependency& dependency) {
    if (dependency.kind == SourceKind::Registry) {
        return dependency.requirement.to_string();
    }
    std::string out{source_kind_name(dependency.kind)};
    out += " " + dependency.location;
    if (!dependency.rev.empty()) {
        out += " @ " + dependency.rev;
    }
    return out;
}

std::string describe(const ResolvedPackage& package) {
    if (package.kind == SourceKind::Registry) {
        return "version " + package.chosen.to_string();
    }
    std::string out{source_kind_name(package.kind)};
    out += " " + package.location;
    if (!package.requested_rev.empty()) {
        out += " @ " + package.requested_rev;
    }
    return out;
}

/// Everything one round of the walk needs to carry.
class Resolver {
public:
    Resolver(const Manifest& root, const Lock& lock, const Fetcher& fetch,
             const IndexReader& index, ast::SourceMap& sources)
        : root_(root), lock_(lock), fetch_(fetch), index_(index), sources_(sources) {}

    Resolution run() {
        // A round walks the graph with the version choices it has,
        // collecting every requirement it meets; then those choices are
        // reconciled against the full set and the walk runs again if any
        // of them moved. Which version a package needs cannot be known
        // before the walk, because a package's requirements are in its
        // own manifest and reading that means picking a version first.
        //
        // Diagnostics from a round that moved something are thrown away:
        // a version chosen before all its requirements were known will
        // fail in ways the next round fixes, and reporting those would
        // be reporting the search rather than the answer.
        //
        // Requirements only accumulate and a chosen version never rises,
        // so the loop walks down a finite ladder and settles. The cap is
        // there for a bug in that reasoning, not for a graph.
        constexpr int kMaxRounds = 64;
        for (int round = 0; round < kMaxRounds; ++round) {
            Resolution attempt = walk();
            terminal_.clear();
            if (reconcile()) {
                continue;
            }

            // Settled. Walk once more so the result carries only the
            // choices that stuck: the round that found them may have
            // tried a version first and failed to read it, and that is
            // the search talking, not the answer.
            Resolution settled = walk();
            // A requirement nothing satisfies is the headline, not a
            // footnote to whatever else the walk tripped over.
            settled.diagnostics.insert(settled.diagnostics.begin(), terminal_.begin(),
                                       terminal_.end());
            return settled;
        }

        Resolution give_up;
        push(give_up.diagnostics, "dependency resolution did not settle",
             root_.dependencies.empty() ? ast::Span{} : root_.dependencies.front().span,
             "this is a bug in the Soliton package resolver");
        return give_up;
    }

private:
    const Manifest& root_;
    const Lock& lock_;
    const Fetcher& fetch_;
    const IndexReader& index_;
    ast::SourceMap& sources_;

    /// The version settled on for each registry package so far.
    std::map<std::string, Version> pinned_;
    /// Every requirement the last round met, by package.
    std::map<std::string, std::vector<Demand>> demands_;
    /// Errors that no further round could fix, kept across the final
    /// clean walk.
    std::vector<ast::Diagnostic> terminal_;
    /// Packages with no version that satisfies everything asked of them.
    /// Nothing more is attempted for these: picking one of the versions
    /// that was rejected and then complaining about *it* would bury the
    /// answer under a symptom.
    std::set<std::string> unsatisfiable_;
    /// Index entries already looked up, so a package in three manifests
    /// is fetched once.
    std::map<std::string, IndexLookup> index_cache_;

    const IndexLookup& look_up(const std::string& name) {
        const auto found = index_cache_.find(name);
        if (found != index_cache_.end()) {
            return found->second;
        }
        return index_cache_.emplace(name, index_ ? index_(name)
                                                 : IndexLookup{std::nullopt,
                                                               "no registry is configured"})
            .first->second;
    }

    /// One pass over the graph. Returns what it resolved; `pinned_` is
    /// updated as version choices are made.
    Resolution walk() {
        Resolution result;
        demands_.clear();

        std::deque<Pending> queue;
        for (const Dependency& dependency : root_.dependencies) {
            queue.push_back(Pending{dependency, root_.name, root_.root});
        }

        while (!queue.empty()) {
            const Pending pending = std::move(queue.front());
            queue.pop_front();
            const Dependency& dependency = pending.dependency;

            if (dependency.kind == SourceKind::Registry) {
                demands_[dependency.name].push_back(
                    Demand{dependency.requirement, pending.requested_by, dependency.span});
                if (unsatisfiable_.count(dependency.name) != 0) {
                    // The requirement is still recorded, because the
                    // report names every one of them; there is just
                    // nothing to fetch.
                    continue;
                }
            }

            const auto seen = std::find_if(result.packages.begin(), result.packages.end(),
                                           [&](const ResolvedPackage& package) {
                                               return package.name == dependency.name;
                                           });
            if (seen != result.packages.end()) {
                check_agrees(*seen, pending, result);
                continue;
            }

            std::optional<ResolvedPackage> package =
                locate(pending, demands_[dependency.name], result);
            if (!package.has_value()) {
                continue;
            }

            const std::optional<Manifest> manifest = read_manifest(*package, pending, result);
            if (!manifest.has_value()) {
                continue;
            }
            package->version = manifest->version;

            const std::filesystem::path root = package->root;
            result.packages.push_back(std::move(*package));
            for (const Dependency& next : manifest->dependencies) {
                queue.push_back(Pending{next, dependency.name, root});
            }
        }
        return result;
    }

    /// Settles each registry package against every requirement the walk
    /// found, rather than the ones it happened to have met by the time
    /// it needed an answer. Returns whether anything moved.
    ///
    /// A package with no satisfying version is reported here and nowhere
    /// else: this is the only point at which all of its requirements are
    /// known, so it is the only point at which giving up is honest.
    bool reconcile() {
        bool changed = false;

        for (const auto& [name, demands] : demands_) {
            const IndexLookup& lookup = look_up(name);
            if (!lookup.ok() || !lookup.entry.has_value()) {
                continue;  // already reported by the walk
            }

            const Release* release = select(name, demands, *lookup.entry);
            if (release == nullptr) {
                Resolution holder;
                report_unsatisfiable(name, demands, holder);
                terminal_.insert(terminal_.end(), holder.diagnostics.begin(),
                                 holder.diagnostics.end());
                unsatisfiable_.insert(name);
                continue;
            }
            unsatisfiable_.erase(name);

            const auto pin = pinned_.find(name);
            if (pin == pinned_.end() || pin->second != release->version) {
                pinned_[name] = release->version;
                changed = true;
            }
        }
        return changed;
    }

    /// Which version of `name` to use, given what is wanted of it.
    ///
    /// The lockfile first, because pinning is what it is for; then
    /// whatever the last round settled on, so the walk agrees with
    /// itself; then the highest version that fits. Each is only taken if
    /// it satisfies every requirement in `demands` — a pin constrains
    /// which acceptable version is chosen, it never makes an
    /// unacceptable one acceptable.
    const Release* select(const std::string& name, const std::vector<Demand>& demands,
                          const IndexEntry& entry) const {
        const auto allowed = [&](const Version& version) {
            return std::all_of(demands.begin(), demands.end(), [&](const Demand& demand) {
                return demand.requirement.allows(version);
            });
        };

        const auto find = [&](const Version& wanted) -> const Release* {
            if (!allowed(wanted)) {
                return nullptr;
            }
            for (const Release& release : entry.releases) {
                if (release.version == wanted) {
                    return &release;
                }
            }
            return nullptr;
        };

        if (const LockEntry* locked = lock_.find(name)) {
            if (locked->kind == SourceKind::Registry) {
                if (const std::optional<Version> version = Version::parse(locked->version)) {
                    if (const Release* release = find(*version)) {
                        return release;
                    }
                }
            }
        }

        const auto pin = pinned_.find(name);
        if (pin != pinned_.end()) {
            if (const Release* release = find(pin->second)) {
                return release;
            }
        }
        return entry.best(allowed);
    }

    /// A package reached twice has to be the same package both times.
    void check_agrees(const ResolvedPackage& chosen, const Pending& pending,
                      Resolution& result) {
        const Dependency& dependency = pending.dependency;

        if (chosen.kind != dependency.kind) {
            push(result.diagnostics,
                 "`" + dependency.name + "` is wanted two different ways", dependency.span,
                 "`" + pending.requested_by + "` wants it as " + describe(dependency))
                .with_note("already resolved as " + describe(chosen))
                .with_note("a package comes from one place; a version and a path are not "
                           "the same package");
            return;
        }

        if (dependency.kind == SourceKind::Registry) {
            // A requirement this version does not meet is not an error
            // yet: it is one more constraint, and `reconcile` will pick
            // again with it in hand.
            return;
        }

        if (chosen.location != dependency.location || chosen.requested_rev != dependency.rev) {
            push(result.diagnostics,
                 "two packages want different versions of `" + dependency.name + "`",
                 dependency.span,
                 "`" + pending.requested_by + "` wants " + describe(dependency))
                .with_note("already resolved as " + describe(chosen))
                .with_note("neither is a published version, so there is nothing to choose "
                           "between; make the two agree");
        }
    }

    /// Turns a dependency into a directory on disk.
    std::optional<ResolvedPackage> locate(const Pending& pending,
                                          const std::vector<Demand>& demands,
                                          Resolution& result) {
        const Dependency& dependency = pending.dependency;

        ResolvedPackage package;
        package.name = dependency.name;
        package.kind = dependency.kind;
        package.location = dependency.location;
        package.requested_rev = dependency.rev;

        if (dependency.kind == SourceKind::Path) {
            std::error_code code;
            package.root = std::filesystem::weakly_canonical(
                pending.requested_from / std::filesystem::path{dependency.location}, code);
            if (code) {
                package.root =
                    pending.requested_from / std::filesystem::path{dependency.location};
            }
            if (!std::filesystem::is_directory(package.root, code)) {
                push(result.diagnostics, "cannot find package `" + dependency.name + "`",
                     dependency.span, "no directory at `" + package.root.string() + "`")
                    .with_note("a path dependency is relative to the manifest that names it");
                return std::nullopt;
            }
            return package;
        }

        Dependency fetchable = dependency;
        if (dependency.kind == SourceKind::Registry) {
            const std::optional<Release> release =
                choose(dependency.name, demands, dependency.span, result);
            if (!release.has_value()) {
                return std::nullopt;
            }
            package.chosen = release->version;
            package.location = release->git;
            package.requested_rev = release->rev;
            pinned_[dependency.name] = release->version;

            fetchable.kind = SourceKind::Git;
            fetchable.location = release->git;
            fetchable.rev = release->rev;
        }

        // A locked commit wins over what the manifest asked for: that is
        // the whole point of locking one.
        std::string wanted = fetchable.rev;
        if (const LockEntry* locked = lock_.find(dependency.name)) {
            if (locked->kind == dependency.kind && !locked->rev.empty() &&
                (dependency.kind == SourceKind::Registry
                     ? locked->version == package.chosen.to_string()
                     : locked->location == dependency.location)) {
                wanted = locked->rev;
            }
        }

        const Fetched fetched = fetch_(fetchable, wanted);
        if (!fetched.ok()) {
            push(result.diagnostics, "cannot fetch package `" + dependency.name + "`",
                 dependency.span, describe(dependency))
                .with_note(fetched.error);
            return std::nullopt;
        }
        package.root = fetched.root;
        package.resolved_rev = fetched.rev;
        return package;
    }

    /// Picks the version a registry package should be.
    std::optional<Release> choose(const std::string& name, const std::vector<Demand>& demands,
                                  ast::Span span, Resolution& result) {
        const IndexLookup& lookup = look_up(name);
        if (!lookup.ok()) {
            push(result.diagnostics, "cannot look up `" + name + "`", span, lookup.error)
                .with_note("a bare version is a registry dependency; configure one with "
                           "`[registry] index = \"...\"`");
            return std::nullopt;
        }
        if (!lookup.entry.has_value()) {
            push(result.diagnostics, "the registry has no package `" + name + "`", span,
                 "nothing published under this name");
            return std::nullopt;
        }

        if (const Release* release = select(name, demands, *lookup.entry)) {
            return *release;
        }
        // Nothing fits what has been seen so far. Saying so is
        // `reconcile`'s job, once it knows the whole set.
        (void)result;
        return std::nullopt;
    }

    void report_unsatisfiable(const std::string& name, const std::vector<Demand>& demands,
                              Resolution& result) {
        if (demands.empty()) {
            return;
        }
        // Reported once however many rounds reach it.
        for (const ast::Diagnostic& existing : result.diagnostics) {
            if (existing.message.find("`" + name + "`") != std::string::npos) {
                return;
            }
        }

        ast::Diagnostic& diagnostic = push(
            result.diagnostics, "no version of `" + name + "` satisfies every requirement",
            demands.front().span,
            "`" + demands.front().requested_by + "` wants " +
                demands.front().requirement.to_string());

        for (std::size_t i = 1; i < demands.size(); ++i) {
            diagnostic.with_note("`" + demands[i].requested_by + "` wants " +
                                 demands[i].requirement.to_string());
        }

        const IndexLookup& lookup = look_up(name);
        if (lookup.ok() && lookup.entry.has_value()) {
            std::string published;
            for (const Release& release : lookup.entry->releases) {
                published += (published.empty() ? "" : ", ") + release.version.to_string();
            }
            diagnostic.with_note("the registry has " + published);
        }
        diagnostic.with_note(
            "soliton picks the highest version satisfying every requirement and does not "
            "backtrack, so the requirements have to agree");
    }

    /// Reads a resolved package's own manifest, which is also where its
    /// dependencies come from.
    std::optional<Manifest> read_manifest(const ResolvedPackage& package,
                                          const Pending& pending, Resolution& result) {
        const Dependency& dependency = pending.dependency;
        const std::filesystem::path manifest_path =
            package.root / std::filesystem::path{kManifestName};

        const std::optional<ast::SourceFile> file = ast::SourceFile::load(manifest_path);
        if (!file.has_value()) {
            push(result.diagnostics, "package `" + dependency.name + "` has no manifest",
                 dependency.span, "no `" + std::string{kManifestName} + "` at `" +
                                      package.root.string() + "`")
                .with_note("a package is a directory with a manifest and a `" +
                           std::string{kSourceDirectory} + "` directory");
            return std::nullopt;
        }

        const ast::FileId id = sources_.add(manifest_path.string(), file->contents());
        ManifestResult parsed = parse_manifest(sources_.file(id));
        for (ast::Diagnostic& diagnostic : parsed.diagnostics) {
            result.diagnostics.push_back(std::move(diagnostic));
        }
        if (!parsed.manifest.has_value() || !parsed.diagnostics.empty()) {
            return std::nullopt;
        }

        // The directory it lives in has no authority over what it is
        // called; the manifest does, and a mismatch means an `import`
        // would not find what the author meant.
        if (parsed.manifest->name != dependency.name) {
            push(result.diagnostics,
                 "package `" + dependency.name + "` calls itself `" + parsed.manifest->name +
                     "`",
                 dependency.span, "the name here has to match its manifest")
                .with_note("declared at `" + manifest_path.string() + "`");
            return std::nullopt;
        }

        // An index that says 1.2.0 and a package that says 1.3.0 is an
        // index nobody can trust; the version is what was chosen *by*.
        if (dependency.kind == SourceKind::Registry &&
            parsed.manifest->version != package.chosen.to_string()) {
            push(result.diagnostics,
                 "the registry says `" + dependency.name + "` " + package.chosen.to_string() +
                     ", and it says " + parsed.manifest->version,
                 dependency.span, "the index and the package disagree")
                .with_note("declared at `" + manifest_path.string() + "`");
            return std::nullopt;
        }

        std::error_code code;
        if (!std::filesystem::is_directory(parsed.manifest->source_directory(), code)) {
            push(result.diagnostics,
                 "package `" + dependency.name + "` has no source directory", dependency.span,
                 "expected `" + parsed.manifest->source_directory().string() + "`")
                .with_note("a package keeps its modules in `" + std::string{kSourceDirectory} +
                           "`, so `import " + dependency.name + ";` finds `" +
                           std::string{kSourceDirectory} + "/" + dependency.name + ".sn`");
            return std::nullopt;
        }
        return std::move(*parsed.manifest);
    }
};

}  // namespace

Resolution resolve(const Manifest& root, const Lock& lock, const Fetcher& fetch,
                   const IndexReader& index, ast::SourceMap& sources) {
    return Resolver{root, lock, fetch, index, sources}.run();
}

}  // namespace soliton::manifest
