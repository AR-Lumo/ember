/// A program that uses a module it did not write.
///
/// `textkit` is not a file beside this one. It lives in
/// `ember_modules/textkit/`, which `import` searches without being
/// told to — the same convention `node_modules` uses, and the place a
/// package manager would put a downloaded dependency.
///
/// Run it from anywhere with no flags:
///
///     ember run examples/packages/main.em
///
/// Other directories can be added with `--module-path <dir>` (or `-L`),
/// or through `EMBER_MODULE_PATH`.

import textkit;

pub fn main() {
    let mut words: Vec<String> = new_vec();
    push(words, textkit::owned("modules"));
    push(words, textkit::owned("found"));
    push(words, textkit::owned("by convention"));

    print("joined:  ");
    println(textkit::join(words, ", "));

    print("shouted: ");
    println(textkit::shout("ember"));
}
