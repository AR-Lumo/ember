/// A program that depends on a package.
///
/// `textkit` is not a file beside this one and is not vendored into
/// `ember_modules` either. It is declared in `ember.toml`, and the
/// compiler resolves it before it starts looking for modules.
///
///     ember run examples/managed/src/main.em

import textkit;

pub fn main() {
    let mut words: Vec<String> = new_vec();
    push(words, textkit::owned("declared"));
    push(words, textkit::owned("resolved"));
    push(words, textkit::owned("built"));

    print("joined:  ");
    println(textkit::join(words, " -> "));

    print("shouted: ");
    println(textkit::shout("ember"));
}
