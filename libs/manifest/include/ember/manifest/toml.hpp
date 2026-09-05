// ember::manifest::toml - the small strict subset of TOML that
// `ember.toml` and `ember.lock` are written in.
//
// This is deliberately *not* a TOML implementation. A real one is a
// large amount of code for a file that will only ever hold a name, a
// version and a list of dependencies, and every feature it has is one
// more thing a manifest can mean. What is here is the subset a manifest
// needs, parsed strictly, with everything else rejected by name rather
// than ignored:
//
//     # a comment
//     [package]
//     name = "myapp"
//     version = "0.1.0"
//
//     [dependencies]
//     textkit = { path = "../textkit" }
//     httpkit = { git = "https://example.invalid/httpkit", rev = "v1.2.0" }
//
// So: comments, `[section]` headers (including dotted ones), string
// values, and one level of inline table whose values are strings. No
// numbers, no booleans, no arrays, no multi-line strings, no nesting
// beyond that one level.
//
// Every value carries the span it came from, so a bad dependency is
// reported in the §7 format against the manifest itself, with carets
// under the part that is wrong.

#ifndef EMBER_MANIFEST_TOML_HPP
#define EMBER_MANIFEST_TOML_HPP

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/span.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ember::manifest::toml {

/// One `key = "value"` pair, or one `key = { ... }` inline table.
struct Entry {
    std::string key;
    ast::Span key_span;

    /// The string on the right, for a plain entry.
    std::string value;
    ast::Span value_span;

    /// The entries of `{ ... }`, when the value was an inline table.
    /// Empty and `is_table` false for a plain string value.
    bool is_table = false;
    std::vector<Entry> entries;

    /// The entry under `key` in this inline table, if there is one.
    const Entry* find(std::string_view name) const;
};

/// One `[section]`, with the entries written under it.
struct Table {
    /// As written, so `[dependencies]` is "dependencies" and
    /// `[a.b]` is "a.b".
    std::string name;
    ast::Span span;
    std::vector<Entry> entries;

    const Entry* find(std::string_view key) const;
};

/// A whole file. Entries before any `[section]` land in a table whose
/// name is empty.
struct Document {
    std::vector<Table> tables;

    const Table* find(std::string_view name) const;
};

struct ParseResult {
    Document document;
    std::vector<ast::Diagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

/// Parse `source` as the subset above. Parsing continues past an error
/// so a manifest with two mistakes reports both.
ParseResult parse(const ast::SourceFile& source);

}  // namespace ember::manifest::toml

#endif  // EMBER_MANIFEST_TOML_HPP
