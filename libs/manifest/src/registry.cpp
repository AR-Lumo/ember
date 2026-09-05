#include "ember/manifest/registry.hpp"

#include "ember/manifest/toml.hpp"

#include <algorithm>

namespace ember::manifest {
namespace {

using ast::Diagnostic;

Diagnostic& push(std::vector<Diagnostic>& into, std::string message, ast::Span span,
                 std::string label) {
    into.push_back(Diagnostic::error(std::move(message), span, std::move(label)));
    return into.back();
}

}  // namespace

const Release* IndexEntry::best(const std::function<bool(const Version&)>& allow) const {
    const Release* found = nullptr;
    for (const Release& release : releases) {
        if (!allow(release.version)) {
            continue;
        }
        if (found == nullptr || found->version < release.version) {
            found = &release;
        }
    }
    return found;
}

IndexResult parse_index_entry(const ast::SourceFile& source, const std::string& name) {
    IndexResult result;

    toml::ParseResult parsed = toml::parse(source);
    result.diagnostics = std::move(parsed.diagnostics);

    IndexEntry entry;
    entry.name = name;

    for (const toml::Table& table : parsed.document.tables) {
        if (table.name.empty()) {
            // Keys outside any version section: the file is not shaped
            // like an index, and guessing what was meant would be worse
            // than saying so.
            if (!table.entries.empty()) {
                push(result.diagnostics, "index entry for `" + name + "` has no version section",
                     table.entries.front().key_span,
                     "expected `[1.2.3]` before any key")
                    .with_note("each section of an index file is one published version");
            }
            continue;
        }

        const std::optional<Version> version = Version::parse(table.name);
        if (!version.has_value()) {
            push(result.diagnostics, "`" + table.name + "` is not a version", table.span,
                 "expected `major.minor.patch`");
            continue;
        }

        const toml::Entry* git = table.find("git");
        const toml::Entry* rev = table.find("rev");
        if (git == nullptr || rev == nullptr) {
            push(result.diagnostics,
                 "version " + version->to_string() + " of `" + name + "` has no source",
                 table.span, "expected `git` and `rev`")
                .with_note("a published version says which commit it is");
            continue;
        }

        for (const toml::Entry& field : table.entries) {
            if (field.key != "git" && field.key != "rev") {
                push(result.diagnostics, "unknown key `" + field.key + "` in an index entry",
                     field.key_span, "expected `git` or `rev`");
            }
        }

        if (std::any_of(entry.releases.begin(), entry.releases.end(),
                        [&](const Release& existing) { return existing.version == *version; })) {
            push(result.diagnostics,
                 "version " + version->to_string() + " of `" + name + "` is listed twice",
                 table.span, "each version appears once");
            continue;
        }

        entry.releases.push_back(Release{*version, git->value, rev->value});
    }

    if (entry.releases.empty() && result.diagnostics.empty()) {
        push(result.diagnostics, "index entry for `" + name + "` lists no versions",
             ast::Span{0, 0, source.id()}, "the file is empty of published versions");
    }

    result.entry = std::move(entry);
    return result;
}

// ---------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------

std::string render_index_entry(const std::string& name, std::vector<Release> releases) {
    std::sort(releases.begin(), releases.end(),
              [](const Release& a, const Release& b) { return a.version < b.version; });

    std::string out =
        "# Index entry for `" + name +
        "`, written by ember. Each section is one published\n"
        "# version and the commit it is. A version already here never changes.\n";

    for (const Release& release : releases) {
        out += "\n[" + release.version.to_string() + "]\n";
        out += "git = " + toml::quoted(release.git) + "\n";
        out += "rev = " + toml::quoted(release.rev) + "\n";
    }
    return out;
}

PublishResult add_release(const ast::SourceFile& existing, const std::string& name,
                          const Release& release) {
    PublishResult result;

    std::vector<Release> releases;
    if (!existing.contents().empty()) {
        IndexResult parsed = parse_index_entry(existing, name);
        // An index file that will not parse is not one to append to:
        // rewriting it would throw away whatever is wrong with it along
        // with whatever is right.
        if (!parsed.diagnostics.empty()) {
            result.diagnostics = std::move(parsed.diagnostics);
            return result;
        }
        releases = std::move(parsed.entry->releases);
    }

    for (const Release& published : releases) {
        if (published.version == release.version) {
            push(result.diagnostics,
                 "version " + release.version.to_string() + " of `" + name +
                     "` is already published",
                 ast::Span{0, 0, existing.id()}, "a published version never changes")
                .with_note("it is `" + published.rev + "`; publish a new version instead")
                .with_note("anyone who locked this version did so expecting it to stay put");
            return result;
        }
    }

    releases.push_back(release);
    result.contents = render_index_entry(name, std::move(releases));
    return result;
}

}  // namespace ember::manifest
