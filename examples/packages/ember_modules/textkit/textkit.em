/// The package's public face. Everything a program using `textkit` can
/// reach has to be `pub` and has to be in this file, since a module
/// name is what an `import` names.

import casing;

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
    return casing::emphatic(text);
}
