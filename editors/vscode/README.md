# Soliton for VS Code

Syntax highlighting, bracket handling and snippets for
[Soliton](https://ar-lumo.github.io/soliton/) — a small statically-typed
language that compiles to native code through LLVM.

Applies to `.sn` source files and `.sni` interface files.

## What it colours

The ordinary things you would expect, plus the three constructs that are
particular to Soliton:

**Units of measure.** `float<meters/seconds^2>` and `5.0<meters>` are
highlighted as units rather than as a comparison against a variable —
the unit name, the `*` and `/` that combine units, and the `^` power all
get their own scopes.

The rule is the compiler's rule: a `<` is only a unit when it *touches*
what precedes it. `5.0<meters>` is a quantity; `5.0 < meters` is a
comparison and is coloured as one.

**Contracts.** `requires` and `ensures` are control keywords, and
`result` — which the checker binds only inside an `ensures` — is
highlighted as a language variable rather than as an ordinary name.

**Effects.** After `uses`, the effect names `io`, `mut` and `nothing`
are highlighted. They are ordinary identifiers everywhere else, because
that is what they are to the compiler: a program with a variable called
`io` still compiles, and still looks right here.

The whole standard library — `println`, `push`, `slice`, `len` and the
rest — is highlighted as built in, since the compiler recognises those
names directly rather than finding them in a library.

## Snippets

`main`, `fn`, `fnreq` (a function with a precondition and a
postcondition), `fnpure` (`uses nothing`), `struct`, `impl`, `unit`,
`while`, `vec`, `string`.

## Installing

Not on the Marketplace yet. Grab the `.vsix` from the
[latest release](https://github.com/AR-Lumo/soliton/releases/latest):

```bash
code --install-extension soliton-lang-0.1.0.vsix
```

Or drag the file onto the Extensions panel. Then open a `.sn` file and
check the status bar says **Soliton**.

To work on the extension itself, symlink it in instead, so edits show up
on reload:

```bash
git clone https://github.com/AR-Lumo/soliton.git
cp -r soliton/editors/vscode ~/.vscode/extensions/soliton-lang
```

On Windows the extensions folder is `%USERPROFILE%\.vscode\extensions`.
`sample.sn` in this directory exercises every construct the grammar
claims to handle, and compiles — it is meant to be opened and looked at.

## What it does not do

This is a grammar, not a language server. There is no completion, no
go-to-definition, no inline diagnostics — run `soliton check file.sn`
for those, which is fast because it stops before code generation.

One deliberate omission: `<` and `>` are **not** configured as auto-
closing brackets. They are comparison operators far more often than they
are unit delimiters, and auto-closing them makes ordinary arithmetic
unpleasant to type. Rust's extension makes the same call.

## Keeping it honest

The grammar is checked against the compiler rather than maintained by
hand:

```bash
python editors/vscode/check-grammar.py
```

Every keyword in the lexer's table and every name in the intrinsics list
has to appear in the grammar; the extensions the manifest claims have to
match `kFileExtension` and `kInterfaceExtension` in the compiler's
headers; every file the manifest points at has to exist; and every
regex has to compile. A keyword nobody remembered to add is the
realistic failure here, and it shows up as one word in the wrong colour
that no-one ever notices.

MIT, along with the rest of Soliton.
