/// The package's root module: what `import textkit;` gets you.
///
/// Its internals live under its own name — `textkit::casing` — so they
/// cannot collide with a `casing` belonging to the program or to
/// another package. Nothing seals them off, though: a determined caller
/// can `import textkit::casing;` too. Nesting makes names unambiguous,
/// not private.

import textkit::casing;

/// Copies a borrowed view into an owned buffer.
pub fn owned(text: string) -> String {
    let mut out: String = new_string();
    push_str(out, text);
    return out;
}

/// Joins words with a separator between them.
pub fn join(words: &Vec<String>, separator: string) -> String {
    let mut out: String = new_string();
    let mut i = 0;
    while i < len(words) {
        if i > 0 {
            push_str(out, separator);
        }
        push_str(out, words[i]);
        i = i + 1;
    }
    return out;
}

/// Uses the package's own private module.
pub fn shout(text: string) -> String {
    return textkit::casing::emphatic(text);
}
