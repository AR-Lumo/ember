#include "ember/manifest/manifest.hpp"

#include "ember/manifest/toml.hpp"

#include <algorithm>
#include <deque>
#include <system_error>

namespace ember::manifest {
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
    return kind == SourceKind::Git ? "git" : "path";
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
    if (!entry.is_table) {
        push(diagnostics, "dependency `" + entry.key + "` is not a table", entry.value_span,
             "expected `{ ... }`")
            .with_note("write `" + entry.key + " = { path = \"../" + entry.key +
                       "\" }` or `{ git = \"...\", rev = \"...\" }`")
            .with_note("a bare version string would need a registry to look it up in, and "
                       "there is not one yet");
        return std::nullopt;
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
        if (!table.name.empty() && table.name != "package" && table.name != "dependencies") {
            push(result.diagnostics, "unknown section `[" + table.name + "]`", table.span,
                 "expected `[package]` or `[dependencies]`");
        }
    }

    result.manifest = std::move(manifest);
    return result;
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
        } else {
            push(result.diagnostics, "unknown source `" + source_field->value + "`",
                 source_field->value_span, "expected `path` or `git`");
            continue;
        }

        entry.location = location->value;
        if (const toml::Entry* rev = table.find("rev")) {
            entry.rev = rev->value;
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
        "# Generated by ember. Check this in; do not edit it by hand.\n"
        "#\n"
        "# It records the exact commit each git dependency resolved to, so a\n"
        "# manifest that asks for a branch keeps building the same code.\n";

    for (const ResolvedPackage* package : sorted) {
        out += "\n[" + package->name + "]\n";
        out += "source = \"" + std::string{source_kind_name(package->kind)} + "\"\n";
        out += "location = \"" + package->location + "\"\n";
        if (!package->resolved_rev.empty()) {
            out += "rev = \"" + package->resolved_rev + "\"\n";
        }
        if (!package->version.empty()) {
            out += "version = \"" + package->version + "\"\n";
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

std::string describe(const Dependency& dependency) {
    std::string out{source_kind_name(dependency.kind)};
    out += " " + dependency.location;
    if (!dependency.rev.empty()) {
        out += " @ " + dependency.rev;
    }
    return out;
}

std::string describe(const ResolvedPackage& package) {
    std::string out{source_kind_name(package.kind)};
    out += " " + package.location;
    if (!package.requested_rev.empty()) {
        out += " @ " + package.requested_rev;
    }
    return out;
}

}  // namespace

Resolution resolve(const Manifest& root, const Lock& lock, const Fetcher& fetch,
                   ast::SourceMap& sources) {
    Resolution result;

    std::deque<Pending> queue;
    for (const Dependency& dependency : root.dependencies) {
        queue.push_back(Pending{dependency, root.name, root.root});
    }

    while (!queue.empty()) {
        const Pending pending = std::move(queue.front());
        queue.pop_front();
        const Dependency& dependency = pending.dependency;

        // Already resolved? Then it either matches or it is a conflict.
        const auto seen = std::find_if(
            result.packages.begin(), result.packages.end(),
            [&](const ResolvedPackage& package) { return package.name == dependency.name; });
        if (seen != result.packages.end()) {
            const bool same = seen->kind == dependency.kind &&
                              seen->location == dependency.location &&
                              seen->requested_rev == dependency.rev;
            if (!same) {
                // No registry to negotiate in, so this is as far as it
                // goes. Both sides are named so the choice is the
                // author's rather than whichever came first.
                push(result.diagnostics,
                     "two packages want different versions of `" + dependency.name + "`",
                     dependency.span, "`" + pending.requested_by + "` wants " +
                                          describe(dependency))
                    .with_note("already resolved as " + describe(*seen))
                    .with_note("ember has no registry and does not solve versions; make the "
                               "two agree");
            }
            continue;
        }

        // Where does it actually live?
        std::filesystem::path package_root;
        std::string resolved_rev;

        if (dependency.kind == SourceKind::Path) {
            std::error_code code;
            package_root = std::filesystem::weakly_canonical(
                pending.requested_from / std::filesystem::path{dependency.location}, code);
            if (code) {
                package_root = pending.requested_from / std::filesystem::path{dependency.location};
            }
            if (!std::filesystem::is_directory(package_root, code)) {
                push(result.diagnostics, "cannot find package `" + dependency.name + "`",
                     dependency.span, "no directory at `" + package_root.string() + "`")
                    .with_note("a path dependency is relative to the manifest that names it");
                continue;
            }
        } else {
            // A locked revision wins over what the manifest asked for:
            // that is the whole point of locking one.
            std::string wanted = dependency.rev;
            if (const LockEntry* locked = lock.find(dependency.name)) {
                if (locked->kind == SourceKind::Git && locked->location == dependency.location &&
                    !locked->rev.empty()) {
                    wanted = locked->rev;
                }
            }

            const Fetched fetched = fetch(dependency, wanted);
            if (!fetched.ok()) {
                push(result.diagnostics, "cannot fetch package `" + dependency.name + "`",
                     dependency.span, describe(dependency))
                    .with_note(fetched.error);
                continue;
            }
            package_root = fetched.root;
            resolved_rev = fetched.rev;
        }

        // Read its manifest, which is also where its own dependencies
        // come from.
        const std::filesystem::path manifest_path =
            package_root / std::filesystem::path{kManifestName};
        const std::optional<ast::SourceFile> file = ast::SourceFile::load(manifest_path);
        if (!file.has_value()) {
            push(result.diagnostics, "package `" + dependency.name + "` has no manifest",
                 dependency.span, "no `" + std::string{kManifestName} + "` at `" +
                                      package_root.string() + "`")
                .with_note("a package is a directory with a manifest and a `" +
                           std::string{kSourceDirectory} + "` directory");
            continue;
        }

        const ast::FileId id = sources.add(manifest_path.string(), file->contents());
        ManifestResult parsed = parse_manifest(sources.file(id));
        for (ast::Diagnostic& diagnostic : parsed.diagnostics) {
            result.diagnostics.push_back(std::move(diagnostic));
        }
        if (!parsed.manifest.has_value()) {
            continue;
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
            continue;
        }

        std::error_code code;
        if (!std::filesystem::is_directory(parsed.manifest->source_directory(), code)) {
            push(result.diagnostics, "package `" + dependency.name + "` has no source directory",
                 dependency.span,
                 "expected `" + parsed.manifest->source_directory().string() + "`")
                .with_note("a package keeps its modules in `" + std::string{kSourceDirectory} +
                           "`, so `import " + dependency.name + ";` finds `" +
                           std::string{kSourceDirectory} + "/" + dependency.name + ".em`");
            continue;
        }

        ResolvedPackage package;
        package.name = dependency.name;
        package.version = parsed.manifest->version;
        package.root = package_root;
        package.kind = dependency.kind;
        package.location = dependency.location;
        package.resolved_rev = resolved_rev;
        package.requested_rev = dependency.rev;
        result.packages.push_back(std::move(package));

        for (const Dependency& next : parsed.manifest->dependencies) {
            queue.push_back(Pending{next, dependency.name, package_root});
        }
    }

    return result;
}

}  // namespace ember::manifest
