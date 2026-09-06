<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/cinder-wordmark-dark.svg">
  <img src="assets/cinder-wordmark.svg" alt="Cinder" width="230">
</picture>


A small statically-typed, compiled language that produces real native
executables via LLVM. Structs, methods in `impl` blocks, fixed-size
arrays, references, and error messages that tell you what went wrong.

```cinder
/// A point in 2D space.
struct Point {
    pub x: int,
    pub y: int,
}

impl Point {
    /// Squared distance to another point (avoids a sqrt for comparisons).
    pub fn distance_sq(&self, other: Point) -> int {
        let dx = self.x - other.x;
        let dy = self.y - other.y;
        return dx * dx + dy * dy;
    }
}

const ORIGIN: Point = Point { x: 0, y: 0 };

pub fn main() {
    let p = Point { x: 3, y: 4 };
    println(p.distance_sq(ORIGIN));
}
```

```console
$ cinder run examples/point.ci
25
```

---

> **New here?** [Getting started](GETTING_STARTED.md) walks from an
> empty file to a multi-file program in about twenty minutes. This
> README is the reference; that is the tutorial.

## Installing

Cinder is a C++20 project built with CMake. You need:

| Requirement | Notes |
|---|---|
| A C++20 compiler | GCC 13+, Clang 16+, or MSVC 19.3+ |
| CMake 3.20+ | Plus any generator — Ninja or Make |
| LLVM 17+ development files | Headers, static libraries, and `LLVMConfig.cmake` |
| LLD | Same version as LLVM. Linked into the compiler — see below |

These are needed to **build** Cinder. They are not needed to **use** it:
an installed Cinder carries its own linker and its own copy of everything
it links against, and runs on a machine with no compiler on it at all.

LLVM is only needed by the code generator. **Without it the project still
builds**, and the lexer, parser and type checker — everything behind
`cinder check` — work normally; only `cinder build` and `cinder run` are
unavailable.

### Linux / macOS

```bash
# Debian/Ubuntu: apt install llvm-dev lld clang cmake ninja-build
# macOS:         brew install llvm lld cmake ninja

cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

If CMake cannot find LLVM, point it at the install:

```bash
cmake -S . -B build -G Ninja -DLLVM_DIR=$(llvm-config --cmakedir)
```

### Windows (MSYS2 UCRT64)

The toolchain used to develop Cinder. From an ordinary PowerShell prompt:

```powershell
# One-time: install the compiler and LLVM
C:\msys64\usr\bin\pacman -S --needed mingw-w64-ucrt-x86_64-gcc `
    mingw-w64-ucrt-x86_64-llvm mingw-w64-ucrt-x86_64-lld

$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ `
      -DLLVM_DIR="C:/msys64/ucrt64/lib/cmake/llvm" `
      -DCMAKE_PREFIX_PATH="C:/msys64/ucrt64"
cmake --build build
ctest --test-dir build --output-on-failure
```

`CMAKE_PREFIX_PATH` is not optional here. LLVM's exported CMake targets
reference `ZLIB::ZLIB` and friends, and without the MSYS2 prefix on the
search path `find_package(LLVM)` fails with *"the link interface of
target LLVMSupport contains ZLIB::ZLIB but the target was not found"*.

The compiler lands at `build/bin/cinder`. Add it to your `PATH`, or call
it by path as the examples below do.

### Installing it somewhere

```bash
cmake --install build --prefix /where/you/want/it
```

That produces a tree with nothing outside it:

```
<prefix>/bin/cinder                  the compiler, with LLD inside it
<prefix>/lib/cinder/libcinder_std.a   the Cinder runtime
<prefix>/lib/cinder/crt2.o, ...      startup objects
<prefix>/lib/cinder/libmsvcrt.a, ... system archives
<prefix>/share/cinder/README.md
```

Copy that directory to a machine that has never had a compiler on it and
`cinder run` works. The compiler finds `lib/cinder` relative to its own
executable — not the working directory, and not a path baked in at
build time — so the tree can live anywhere and be moved after the fact.

**Why this took work.** Cinder used to link by running
`${CMAKE_CXX_COMPILER}`, an absolute path recorded when the compiler was
built. That works on precisely one machine. Three separate things had to
change:

- `cinder` itself needed five DLLs from the MSYS2 prefix
  (`libstdc++-6`, `libgcc_s_seh-1`, `libwinpthread-1`, `zlib1`,
  `libzstd`). It now links them statically and imports nothing Windows
  does not ship.
- Linking now happens **in process**. LLD is compiled into the binary,
  so no external linker is invoked and none needs to exist.
- The programs Cinder produces used to import `libstdc++-6.dll`
  themselves. They now link their runtime in and import only Windows'
  own DLLs.

The last one is the quiet failure. A GNU-style `-l` prefers an import
library over an archive when both are present, so one wrong flag gives a
program that builds cleanly, runs on the machine that built it, and dies
with `0xC0000135` anywhere else. `tests/link_tests.cpp` reads the PE
import table of both the compiler and a program it produced, and fails
on any DLL Windows does not ship — because that is the only place the
answer actually lives.

The cost is size: `cinder` is around 210 MB, because lld's COFF driver
initialises every LLVM target unconditionally, so they all have to be
linked in. The install adds about 18 MB of archives on top.

### Build options

| Option | Default | Meaning |
|---|---|---|
| `CINDER_REQUIRE_LLVM` | `OFF` | Fail configuration instead of warning when LLVM is missing |
| `CINDER_STATIC_DRIVER` | `ON` | Link the compiler against static runtimes so it needs no toolchain DLLs |
| `LLVM_DIR` | — | Path to the directory holding `LLVMConfig.cmake` |

---

## Hello, world

Put this in `hello.ci`:

```cinder
/// The smallest Cinder program that produces output.
pub fn main() {
    println("hello, world");
}
```

Run it straight away:

```console
$ cinder run hello.ci
hello, world
```

Or compile it to a standalone executable that no longer needs the
compiler:

```console
$ cinder build hello.ci -o hello
$ ./hello
hello, world
```

Now break it deliberately — change the line to `println(missing);` and
type-check without generating code:

```console
$ cinder check hello.ci
error: cannot find value `missing`
 --> hello.ci:3:13
  |
3 |     println(missing);
  |             ^^^^^^^ not found in this scope

error: aborting due to 1 previous error
```

Every stage reports as many problems as it can in one run, rather than
stopping at the first.

---

## The CLI

```
cinder build <file.ci> [-o <output>]   compile to a native executable
cinder run <file.ci>                   compile and run in one step
cinder check <file.ci>                 type-check only, no codegen
cinder fetch                           resolve and download dependencies
cinder publish                         record this version in the registry index
cinder interface <file.ci>             write the module's public interface

  -L, --module-path <dir>
                   also look here for imported modules (repeatable)
  -O0 .. -O3       optimization level (default: -O0)
      --update     re-resolve git dependencies, ignoring `cinder.lock`
      --dry-run    for `publish`: say what it would record, record nothing
      --lib        compile to an object file, with no `main` required
      --whole-program
                   compile as one unit, so `-O` can inline across modules
      --link <path> an object or library to link in as well
  -v, --verbose    say which modules were compiled and which were cached
      --fresh      recompile every module, ignoring cached object files
```

`--module-path` works on `check` too — finding a module is a question
the front end asks, and `check` runs the front end. The build flags do
not, since `check` never reaches the back end.

Exit codes are `0` on success, `1` when the program failed to compile,
and `2` when the command line itself was wrong. A compiled program that
hits a runtime error — an out-of-bounds index, a division by zero —
exits `101`.

### Optimization

`-O0` through `-O3`, spelled the way C spells them, running LLVM's
standard per-module pipeline. A bare `-O` is refused rather than guessed
at: gcc reads it as `-O1` and clang as `-O2`, and there is no reason to
pick a side silently.

On a Collatz search over 300,000 starting points:

| | time |
|---|---|
| `-O0` | 0.43s |
| `-O1` | 0.27s |
| `-O2` | 0.27s |
| `-O3` | 0.24s |

The default is `-O0`, as it is for every C compiler: it compiles faster,
and the IR it produces still reads like the source it came from.

`--whole-program` folds every module into one before optimizing, so a
cross-module call is a direct call that `-O` can inline. It gives up
incremental builds to do it — there is nothing smaller than the whole
thing to cache. The win is real but modest: on the Collatz search with
its inner call moved into another module, best-of-7 goes from 0.054s to
0.051s. Worth reaching for on a release build, not on every build.

### Packages

A package is a directory with an `cinder.toml` and its modules in `src/`.
Depending on one puts that `src/` on the module search path — which is
all a dependency has ever been here.

```toml
[package]
name = "myapp"
version = "0.1.0"

[registry]
index = "https://example.invalid/cinder-index"

[dependencies]
serde = "1.0.0"                                              # from the registry
textkit = { path = "../textkit" }                            # a directory
httpkit = { git = "https://example.invalid/h", rev = "v1.2" } # a repository
```

`build`, `run` and `check` find the manifest by walking up from the
source file, resolve it, and fetch anything missing. `cinder fetch` does
that and stops. A program with no manifest needs none: most of them are
one file and depend on nothing.

A `git` dependency is cloned once into `.cinder/packages` and the commit
it resolved to is written to `cinder.lock`:

```toml
[httpkit]
source = "git"
location = "https://example.invalid/httpkit"
rev = "9f2c1ab4e83d0715c6a2f4b8e1d093a75c6e4021"
version = "1.2.0"
```

So a manifest that asks for a branch keeps building the same code until
`--update` says otherwise. Check the lockfile in. After the first build
nothing touches the network: a commit hash cannot move, so having it
already is proof enough — which is why `rev` is required, and why a tag
or a branch is re-checked every time.

#### The registry

A bare version string is a registry dependency. The registry is a
directory of index files, one per package, saying where each published
version lives:

```toml
# textkit.toml
[1.0.0]
git = "https://example.invalid/textkit"
rev = "v1.0.0"

[1.2.0]
git = "https://example.invalid/textkit"
rev = "v1.2.0"
```

That directory can be a path or a git repository — which is how
crates.io's index works, and means publishing a registry needs a git
host rather than a server. Point `[registry] index` at it.

A version resolves to a git dependency, so everything past that point is
machinery that already existed; the index only decides *which* commit.

**Requirements.** `"1.2.3"` is a caret: this version or anything
compatible, where compatible stops at the next release that may break —
`^1.2.3` allows `1.9.0` but not `2.0.0`, and `^0.2.3` allows `0.2.9` but
not `0.3.0`, because before 1.0 the minor is where breakage lives.
`"^1.2.3"` says the same thing out loud and `"=1.2.3"` pins exactly.
There are no ranges, wildcards or pre-release tags; each is refused by
name rather than misread.

**How a version is chosen.** Each package gets one: the highest the
index has that satisfies every requirement written against it. Finding
that takes a fixpoint, because a package's own requirements are inside
its manifest and reading that means picking a version first — so the
graph is walked, the choices reconciled against everything the walk
found, and walked again until nothing moves.

**It does not backtrack.** It will not try a lower version of one
package to make room for another. A graph that would need that is
reported rather than solved:

```console
error: no version of `textkit` satisfies every requirement
 --> left/cinder.toml:6:1
  |
6 | textkit = "1.0.0"
  | ^^^^^^^ `left` wants ^1.0.0
  = note: `right` wants ^2.0.0
  = note: the registry has 1.0.0, 1.2.0, 1.3.0, 2.0.0
  = note: cinder picks the highest version satisfying every requirement and does not backtrack, so the requirements have to agree
```

Every requirement is named, along with who wrote it and what has
actually been published. In an ecosystem this size that is more useful
than a solver that takes a minute to reach the same place.

The lockfile pins the version as well as the commit, so publishing
`1.3.0` does not move a build that settled on `1.2.0` until `--update`
says so. And an index that claims a commit is `1.3.0` when the package
there says `1.2.0` is refused — an index that can be wrong about that
can serve anything for anything.

#### Publishing

`cinder publish` adds this package's current version to the index. It
checks first — the package has to type-check, have a root module, sit in
a clean git tree with an `origin` remote, and be tagged `v<version>`:

```console
$ cinder publish
 checking textkit 1.0.0
 packaged textkit 1.0.0 (d3743a17)
   staged /home/me/.../index/textkit.toml
committed to the index, and not pushed

To publish textkit 1.0.0, send it:
    git -C "/home/me/.../index" push
```

**It stops before pushing.** Everything up to that point can be undone
by deleting a directory; sending it cannot, because a version once
published has to go on meaning what it meant. So cinder writes the entry,
commits it in its own checkout of the index, and hands you the command.
`--dry-run` prints what it would record and writes nothing.

What gets recorded is the **commit**, not the tag — a tag can be moved
and a commit cannot. Republishing a version is refused outright:

```console
error: version 1.0.0 of `textkit` is already published
  = note: it is `d3743a17...`; publish a new version instead
  = note: anyone who locked this version did so expecting it to stay put
```

A package that depends on a **path** cannot be published at all: nobody
else has that directory, so it would not build for them. Publish that
dependency too and depend on its version, or use a git dependency.

There is no server, no account and no ownership. Whoever can push to the
index can publish, which is a property of the git repository rather than
of cinder.

[`examples/managed`](examples/managed) is a small project end to end.

### Interfaces, and shipping a compiled library

`cinder interface` writes a module's public surface with the
implementations taken out:

```console
$ cinder interface geometry.ci
// Interface for `geometry`, written by cinder.
//
// The public surface of the module, with the implementations taken
// out. A generic keeps its body, because monomorphizing one needs it.

pub struct Point {
    pub x: int,
    pub y: int,
}

pub const ORIGIN: Point = Point { x: 0, y: 0 };

pub fn distance_sq(a: Point, b: Point) -> int;
```

`fn f() -> int;` — a signature with no body — is now part of the
language. It is checked as a signature and lowered to a declaration, and
separate compilation already knew what to do with a function whose body
is somewhere else.

An interface plus an object is a library. `--lib` writes the object:

```console
$ cinder interface lib/textkit.ci -o dist/textkit.cii
$ cinder build --lib lib/textkit.ci -o dist/textkit.o
$ rm -r lib                                    # the consumer never sees it
$ cinder run app/main.ci -L dist --link dist/textkit.o
cinder!
```

`import textkit;` finds `textkit.cii` on the search path when there is
no `textkit.ci`; **source always wins**, so an interface can never
quietly stand in for something you could have compiled. A library's
symbols carry its module prefix, taken from its file name — the same
rule `import` uses to find it.

**A generic keeps its body.** Cinder monomorphizes, so a copy of
`twice<int>` is generated wherever it is first used, and generating it
needs the body. That is the bargain C++ strikes by putting templates in
headers, and it has the same consequence: a generic's implementation is
part of its interface, and changing it changes what everyone compiles.

**The interface is cut out of the source, not printed from the tree.**
Every item knows the span it came from, so a signature is the text up to
the body and a generic is the text of the whole thing. Nothing is
re-rendered, so nothing can be rendered wrong — the output is your own
Cinder, and it parses because it already did.

A public signature that names a private type is refused, because a
caller could not use it:

```console
error: `Point::distance_sq` cannot be part of an interface
 --> point.ci:9:38
  |
9 |     pub fn distance_sq(&self, other: Point) -> int {
  |                                      ^^^^^ `Point` is not `pub`
  = note: a caller outside this module cannot name `Point`, so it could not call `Point::distance_sq` even with the declaration in front of it
  = note: make `Point` public, or keep `Point::distance_sq` private
```

### Incremental builds

Each module compiles to its own object file, kept in a `.cinder`
directory beside the entry source and reused when nothing it depends on
has changed:

```console
$ cinder build main.ci --verbose
compiling main.ci
compiling shapes.ci
compiling counter.ci
 linking  main.exe

$ cinder build main.ci --verbose        # nothing edited
  cached  main.ci
  cached  shapes.ci
  cached  counter.ci
 linking  main.exe
```

An object is valid as long as its module's source *and* the source of
everything that module imports are unchanged — a struct that changes
shape changes the code generated in every module that uses it. So
editing `shapes.ci` rebuilds `shapes` and `main`, and leaves `counter`
alone:

```console
$ cinder build main.ci --verbose
compiling main.ci
compiling shapes.ci
  cached  counter.ci
 linking  main.exe
```

The cache key is the fingerprint in the object's file name, so a hit is
just a file existing — there is no manifest that can disagree with what
is on disk. `--fresh` ignores it. Deleting `.cinder` is always safe.

Each optimization level keeps its own objects, so working at `-O0` and
dropping to `-O2` to check something does not recompile the program each
way round.

---

## The language

A tour by way of the pieces. The full grammar is in
[`cinder-language-spec.md`](cinder-language-spec.md) §3.

### Types

| Type | Meaning |
|---|---|
| `int` | 64-bit signed integer |
| `float` | 64-bit IEEE 754 double |
| `bool` | `true` or `false` |
| `string` | Immutable, fixed-length view over bytes |
| `[T; N]` | Fixed-size array of `N` elements |
| `Vec<T>` | Growable array. Owns a heap buffer |
| `String` | Growable string buffer. Owns a heap buffer |
| `fn(A) -> R` | A callable value: a closure or a function |
| `&T` | Non-owning reference |
| `struct` | Nominal record type, no inheritance |

`int` and `float` never mix implicitly. Where you need both, say so
with `as`:

```cinder
pub fn mean(values: &[int; 5]) -> float {
    let mut total = 0;
    let mut i = 0;
    while i < len(values) {
        total = total + values[i];
        i = i + 1;
    }
    return total as float / len(values) as float;
}
```

`as` binds tighter than arithmetic and looser than unary minus, so
`a as float * b` is `(a as float) * b` and `-x as float` is
`(-x) as float`.

### Bindings

```cinder
let count = 1;            // inferred
let total: int = 0;       // annotated
let mut running = true;   // reassignable
```

A binding is immutable unless declared `mut`. Inner scopes may shadow
outer ones.

### Functions and methods

```cinder
pub fn add(a: int, b: int) -> int {
    return a + b;
}

impl Point {
    pub fn shift(&self, by: int) -> Point {
        return Point { x: self.x + by, y: self.y + by };
    }
}
```

Methods live in `impl` blocks and are scoped to their type: `shift` is
reachable as `p.shift(2)` and never as a bare `shift(...)`. They compile
to plain functions with the receiver as an explicit first argument —
`Point_shift` is a real symbol in the output binary, and there are no
vtables or dynamic dispatch anywhere in v1.

### Arrays and references

```cinder
pub fn total(values: &[int; 4]) -> int {
    let mut sum = 0;
    let mut i = 0;
    while i < len(values) {
        sum = sum + values[i];
        i = i + 1;
    }
    return sum;
}
```

Passing `&[int; 4]` hands over a pointer to the caller's array instead of
a copy, so writes through it are visible to the caller. `mut` governs
rebinding a reference, not writing through one.

Array accesses are bounds-checked at runtime. Strict C would not check,
but Cinder has no borrow checker either, and a silent out-of-bounds write
is a worse trade than a branch the optimizer usually removes.

### Generics

Functions and structs may take type parameters. Each combination of
argument types is compiled to its own function, so there is no boxing
and nothing is decided at run time:

```cinder
pub fn max<T>(a: T, b: T) -> T {
    if a > b {
        return a;
    }
    return b;
}

struct Pair<T> {
    pub left: T,
    pub right: T,
}
```

`max(3, 7)` and `max(2.5, 1.5)` become `max__int` and `max__float` in the
binary. Type arguments are inferred from the call, so there is no
turbofish; a parameter that appears in no argument type is rejected at
the declaration, because nothing could ever determine it.

Cinder has no traits, so a type parameter carries no guarantees and a
generic body is **checked once per instantiation**, as C++ templates are
rather than Rust generics. `a > b` above is legal for `int` and not for a
struct, and that is only knowable once `T` is chosen — so the error is
reported against the body, with a note naming the call that caused it:

```console
error: cannot compare values of type `Point`
 --> sort.ci:4:8
  |
4 |     if a > b {
  |        ^^^^^ `>` needs an `int` or a `float`
  = note: in `max` instantiated as `max<Point>` at sort.ci:13:13
```

The trade-off is that a generic function nobody calls is never checked.

Struct literals infer their type arguments from the field values —
`Pair { left: 1, right: 2 }` is a `Pair<int>`. They are never written
out in an expression, where `Pair<int> { }` would be ambiguous with a
chain of comparisons; in type position they are explicit.

#### Generic types with methods

`impl<T> Stack<T>` declares the parameters and applies them to the type,
and every method inside is generic over them:

```cinder
struct Stack<T> {
    pub items: Vec<T>,
}

impl<T> Stack<T> {
    pub fn with(self, value: T) -> Stack<T> {
        let mut grown = self;
        push(grown.items, value);
        return grown;
    }

    pub fn height(&self) -> int { return len(self.items); }

    /// A parameter of the method's own, inferred from the argument.
    pub fn described_by<L>(&self, label: L) -> L { return label; }
}
```

**Nothing is inferred for the block's parameters** — the receiver says.
A `Stack<int>` makes `T` into `int`, and there is nothing left to work
out. A method's own parameters are a different matter, and come from the
arguments the way a free function's do.

A type parameter used only in the **return type** is settled by what the
result is bound to, which is the only way to write a constructor for a
generic type:

```cinder
pub fn new_stack<T>() -> Stack<T> {
    let items: Vec<T> = new_vec();
    return Stack { items: items };
}

let s: Stack<int> = new_stack();   // the annotation says what T is
```

Arguments are unified first, so the binding only fills in what the call
left open — it can never override what was actually passed. Without an
annotation there is nothing to go on, and the error says so.
[`examples/stack.ci`](examples/stack.ci) is the whole thing.

### Modules

A program may span several files. `import` names a module, and a module
called `geometry` lives in `geometry.ci` beside the file that imports it:

In `geometry.ci`:

```cinder
pub struct Point {
    pub x: int,
    pub y: int,
}

pub fn magnitude_sq(p: Point) -> int {
    return p.x * p.x + p.y * p.y;
}

fn private_helper() -> int { return 1; }
```

In `main.ci` beside it:

```cinder
import geometry;

pub fn main() {
    let p = geometry::Point { x: 3, y: 4 };
    println(geometry::magnitude_sq(p));
}
```

```console
$ cinder run main.ci
25
```

Items are reached through `module::item`. Each module keeps its own
namespace, so two modules may both declare a `helper` or a `Value`
without colliding — the symbols they emit are prefixed (`a__helper`,
`b__helper`), and the entry module's stay unprefixed so the linker still
finds `main`.

**This is where `pub` starts doing something.** Until modules there was
no boundary to enforce it across; now anything not marked `pub` is
private to the file that declared it.

Reach for `private_helper` from `main.ci` and the note points into the
other file:

```console
error: function `geometry::private_helper` is private
 --> main.ci:4:23
  |
4 |     println(geometry::private_helper());
  |                       ^^^^^^^^^^^^^^ `geometry::private_helper` is not declared `pub`
  = note: declared at geometry.ci:10:4
```

That applies to types, constants and individual struct fields too. A
struct with any non-`pub` field cannot be built from outside its module
at all — every field needs a value and a private one cannot be given
one — so the module has to offer a constructor instead.

Two rules worth knowing:

- **Imports are not transitive.** If `mid` imports `geometry`, a file
  that imports `mid` still cannot name `geometry`.
- **Unqualified names resolve within one module only.** There are no
  implicit imports, so `magnitude_sq(...)` never silently finds
  another module's function.

Modules may import each other in a cycle. Every module is collected
before any body is checked, so neither has to come first.

#### Nested paths

A module path is a file path. `shapes::geometry` is `shapes/geometry.ci`,
as deep as you care to go:

```cinder
import shapes::geometry;
import shapes::detail::math;

pub fn main() {
    let p = shapes::geometry::Point { x: 3, y: 4 };
    println(shapes::detail::math::square(p.x));
}
```

The last segment names the item; everything before it names the module.
The path is resolved against the root the *importing module* was found
under, not against the directory it happens to sit in — so
`shapes::detail::math` means the same file written in `main.ci` and in
`shapes/geometry.ci`, and a package keeps resolving its own modules
against its own directory.

**Nesting is a naming device and nothing more.** `shapes::geometry` has
no relationship to `shapes`, which need not exist and gets no special
access if it does. What it buys is that a leaf name can repeat: a
package's internal `casing` can live at `textkit::casing` and stop
colliding with yours. It does not make anything private — `pub` still
decides that, and a caller who knows the name can import it.

Paths become `__` in symbols, so `shapes::detail::math::square` links as
`shapes__detail__math__square`.

#### Where modules come from

`import geometry;` looks for `geometry.ci` under the root the importing
module was found under — for the entry file, the directory it sits in.
If it is not there, each directory on the module search path is tried
twice — as `geometry.ci`, and as `geometry/geometry.ci`:

| | |
|---|---|
| `--module-path <dir>`, or `-L <dir>` | repeatable, tried in order |
| `CINDER_MODULE_PATH` | `PATH`-style list, `;` on Windows and `:` elsewhere |
| `cinder_modules/` beside the entry file | used automatically if it exists |

Explicit beats ambient beats conventional. **The importing module's own
root always wins**, so adding a dependency can never quietly take over a
name a program was already using for a module of its own.

The `geometry/geometry.ci` form is what lets a package be more than one
file: that directory becomes the package's root, so everything it
imports resolves inside it.
[`examples/packages`](examples/packages) is a whole one, and needs no
flags — it just puts `textkit` in `cinder_modules/`.

When nothing turns up, the error is a list of where it looked:

```console
error: cannot find module `textkit`
 --> main.ci:1:8
  |
1 | import textkit;
  |        ^^^^^^^ no file for this module
  = note: looked at `textkit.ci`
  = note: looked at `vendor\textkit.ci`
  = note: looked at `vendor\textkit\textkit.ci`
```

Module names are global — a path is what makes one unique, not the
directory it sits in — so two files claiming one path is an error rather
than a coin toss, and the compiler names both.

### Dynamic arrays and strings

`[T; N]` has its length in its type. `Vec<T>` grows:

```cinder
let mut v: Vec<int> = new_vec();
push(v, 10);
push(v, 20);
println(len(v));     // 2
println(v[0]);       // 10
println(pop(v));     // 20
```

`String` is the growable counterpart to `string`, which stays a borrowed
fixed-length view — the same split Rust makes between `String` and
`&str`:

```cinder
let mut message: String = new_string();
push_str(message, "built ");
push_str(message, "a piece at a time");
println(message);
```

`new_vec()` takes its element type from the binding, since there is no
turbofish and nothing in the call to infer from. That same rule now also
lets `let xs: [int; 0] = [];` work.

`capacity(c)` says what a container has room for and `reserve(c, n)`
asks for more, on either growable container. Neither ever shrinks.

#### Text reads the same whichever type holds it

`string` borrows and `String` owns, but the bytes say the same thing, so
everything that only *reads* takes either:

```cinder
let sentence = "the quick brown fox";
println(slice(sentence, 4, 9));      // quick
println(find(sentence, "fox"));      // 16
println(contains(sentence, "cat"));  // false

let mut owned: String = new_string();
push_str(owned, "the quick brown fox");
println(owned == sentence);          // true, across the two types
println("apple" < "banana");         // text orders lexicographically
```

`slice` returns a **view**, so a substring costs a bounds check and two
fields rather than an allocation — and carries the same warning `&T`
does: it points into something else, and dangles if that something is
dropped or grown while the view is alive.

Passing a `String` where a `string` is wanted **lends a view of it**;
the caller keeps the buffer, so this borrows rather than moves. The
other direction is refused, because turning a borrow into ownership
needs a copy and nothing here copies silently.

[`examples/words.ci`](examples/words.ci) splits a sentence, sorts the
pieces and searches them.

An element may own memory of its own — `Vec<String>`, `Vec<Vec<int>>`,
as deep as you like. Dropping such a vector is not one `free`: it walks
its live elements, drops each, and only then releases the buffer they
sat in.

```cinder
let mut words: Vec<String> = new_vec();
push(words, make("cinder"));       // `make` returns a String; it moves in

println(words[0]);                // reads an element without taking it
let last = pop(words);            // takes one back out, shortening the vector
```

Reading `words[i]` borrows. Moving an element *out* of the middle is
refused, because the vector would go on counting something it no longer
holds — `pop` is how ownership comes back, and the error says so:

```console
error: cannot move out of `String` here
  --> main.ci:11:17
   |
11 |     let taken = words[0];
   |                 ^^^^^^^^ only a whole variable can be moved
   = note: a field or element cannot be moved out on its own, because what remains would be half-owned
   = note: `pop` takes the last element out of a `Vec` and shortens it, which leaves nothing half-owned
```

[`examples/word_list.ci`](examples/word_list.ci) is the whole thing end
to end.

### Ownership

`Vec` and `String` own heap memory, and so does any struct holding one.
Owned values **move** rather than copy, and are **freed automatically**
when their owner goes out of scope:

```cinder
let mut a: Vec<int> = new_vec();
let b = a;           // the buffer moves to b
println(len(a));     // error: use of moved value `a`
```

```console
error: use of moved value `a`
 --> main.ci:5:17
  |
5 |     println(len(a));
  |                 ^ `a` was moved and no longer holds a value
  = note: moved at main.ci:4:13
  = note: `Vec<int>` owns heap memory, so assigning or passing it moves it rather than copying
```

Nothing is reference-counted and nothing is collected. The compiler works
out where each buffer dies and frees it there, so a million vectors
built and dropped in a loop hold flat memory.

Passing by value moves; passing `&T` borrows and does not:

```cinder
pub fn sum(v: &Vec<int>) -> int { ... }    // caller keeps it
pub fn consume(v: Vec<int>) -> int { ... } // caller gives it up
```

**There is still no borrow checker.** `&T` remains an unchecked
non-owning pointer exactly as it always was, so none of this needs
lifetimes — returning a reference to a local will still compile and then
dangle. What ownership buys is that heap memory is freed exactly once,
automatically, with no runtime bookkeeping.

Assigning into a moved-from variable gives it a value again. A value
moved on only one path of an `if` is tracked with a one-bit flag the
optimizer folds away wherever the answer is obvious.

Types with no heap behind them — `int`, `bool`, `string`, `[T; N]`,
`&T`, and structs built only from those — are unaffected and still copy
freely.

### Closures

Functions are values. A closure is written `|params| -> Result { ... }`
and has the type `fn(Params) -> Result`:

```cinder
let double = |x: int| -> int { return x * 2; };
println(double(21));                 // 42

let offset = 100;
let shift = |x: int| -> int { return x + offset; };
println(shift(5));                   // 105
```

`||` with nothing between the bars is an empty parameter list.

**Parameter types may be left out** when there is something to take them
from — which for a closure there usually is, since it is being passed to
something:

```cinder
map_in_place(values, |x| { return x * 2; });
```

The `&fn(int) -> int` the function expects says what `x` is and what the
body must return. A *function's* parameters are never inferred this way,
because a function has no context to take them from. Where a closure has
none either, it says so and asks for the type.

**Captures are by value.** A closure takes what it mentions from the
surrounding scope into its own storage, which is why one can be returned
and still work:

```cinder
pub fn scaler(factor: int) -> fn(int) -> int {
    return |x: int| -> int { return x * factor; };
}
```

A closure is an owned value, like a `Vec`, because a capturing one holds
a heap block. Calling it is a *use* rather than a move, so it can be
called as often as you like; passing it by value moves it, and
`&fn(...)` borrows it — which is what a higher-order function usually
wants:

```cinder
pub fn map_in_place(values: &Vec<int>, f: &fn(int) -> int) { ... }
```

A closure that captures nothing has a null environment and allocates
nothing at all.

**A capture may own memory.** Taking one by value means taking it: the
closure owns it from then on, and the scope that had it does not.

```cinder
let mut greeting: String = new_string();
push_str(greeting, "hello");

let speak = |name: string| { print(greeting); println(name); };
speak("ada");
speak("grace");                      // still there; calling is a use
println(greeting);                   // error: use of moved value
```

Such a closure carries **its own drop function**, because what is inside
an environment cannot be worked out from the closure's type — two
closures of the same `fn() -> int` may have captured quite different
things. A closure with nothing owned in it carries none, and a million
closures each holding a `String` and a `Vec` hold flat memory.

### Contracts

A function can say what it expects and what it promises, in the
signature rather than the first few lines of the body:

```cinder
pub fn divide(a: int, b: int) -> int
    requires b != 0
    ensures result != 0 || a == 0
{
    return a / b;
}
```

`requires` is checked before the body runs. `ensures` is checked before
**every** return, with `result` naming the value about to be returned.
Both are ordinary `bool` expressions, type-checked like the condition of
an `if`, and both can see the parameters — including `self` on a method.

A violation is a panic that names the clause, not a wrong answer that
travels:

```console
$ cinder run divide.ci
cinder: requires contract violated in `divide`: b != 0
  --> divide.ci:2:5
```

`result` is **not** a keyword. Reserving it would break every program
that has a variable by that name, for a word that means something in
exactly one place — so it is bound only while an `ensures` is in scope,
and asking for it anywhere else is a specific error rather than a
puzzling one:

```
error: `result` is not in scope in a `requires`
 --> lib.ci:2:14
  |
2 |     requires result > 0
  |              ^^^^^^ a `requires` is checked before the function runs, so there is no result yet
```

A contract sees what the signature sees, so it cannot mention a local —
a local is not part of the promise. Contracts survive into an interface
file, since they are part of the signature a caller is reading.

**What this is not.** The checks happen at run time; nothing is proved
at compile time, so a contract that can fail is not a compile error.
And the message names the contract rather than the call site: reporting
the caller means threading its position into every call to a contracted
function, which changes those functions' ABI and would have to survive
separate compilation.

### Units of measure

A number can carry a unit, and the compiler refuses to mix them up:

```cinder
unit meters;
unit seconds;

pub fn main() {
    let d: float<meters> = 100.0<meters>;
    let t: float<seconds> = 4.0<seconds>;

    let speed = d / t;        // float<meters/seconds>, inferred
    println(speed);           // 25.0
}
```

`+` and `-` need the same unit on both sides. `*` and `/` combine them
algebraically, so a distance over a time is a speed and nobody has to
declare one:

```cinder
let rate: float<meters/seconds^2> = d / t / t;   // an acceleration
let back: float<meters>           = speed * t;   // and back again
let ratio: float                  = d / d;       // cancels to a plain number
```

Adding what you should not is a compile error, not a wrong answer:

```console
$ cinder check units.ci
error: cannot apply `+` to `float<meters>` and `float<seconds>`
 --> units.ci:7:15
  |
7 |     let bad = d + t;
  |               ^^^^^ the operands have different types
  = note: `+` needs the same unit on both sides (§10.2)
```

A plain number is **not** a unitless quantity that fits anywhere —
`d + 2.0` is an error too. Scaling is what a plain number is for, and
`d * 2.0` keeps the metres.

**Units cost nothing.** A `float<meters>` is a `double`; the whole thing
lives in the type checker and is gone by codegen. The test suite compiles
a program with units and the same program without them and asserts the
generated IR is identical, so this stays true rather than merely having
been true once.

**One rule worth knowing.** A unit binds to a literal only when the `<`
touches it. `5.0<meters>` is a quantity; `5.0 < meters` is a comparison.
That keeps `f(5<x, 3)` — a comparison written without spaces — parsing as
it always did, and stops `5 < meters` from changing meaning the day
somebody declares a unit called `meters`.

### Effects

A `uses` clause bounds what a function is allowed to do:

```cinder
pub fn area(w: int, h: int) -> int uses nothing {
    return w * h;                  // pure, and held to it
}

pub fn report(w: int, h: int) uses io {
    println(area(w, h));           // allowed to print, and does
}
```

The compiler infers what each function actually does — `println`
performs `io`, a call performs whatever the callee performs — and
reports anything the clause does not permit:

```console
$ cinder check effects.ci
error: `io` is not permitted here
 --> effects.ci:6:5
  |
6 |     println(w);
  |     ^^^^^^^^^^ this performs `io`
  = note: `quiet` is declared `uses nothing`
```

It works at any distance. An effect three calls away is still caught,
and the blame lands on the call you would have to change rather than on
the distant `println`. Mutual recursion is fine — inference is a least
fixed point, not a walk.

**A function with no clause has no bound.** That is the whole reason
this could be added to a language that already had programs: every one
of the 31 in `examples/` and `tests/golden/` prints, and none of them
needed changing. Effects go on one function at a time, and nothing is
checked until you ask.

`uses nothing` is how you ask for the strongest answer. A bound is a
ceiling and not a quota, so `uses io` on something that never prints is
fine, like an unused `throws` in Java.

**Effects are part of a function type**, which is what makes a
higher-order function bounded at all:

```cinder
pub fn apply(f: fn(int) -> int uses nothing, x: int) -> int uses nothing {
    return f(x);            // the type says the call is pure
}
```

A function that does less goes where one that may do more is wanted, so
a pure closure satisfies a `uses io` parameter. An *unbounded*
`fn(int) -> int` still counts as performing anything — it says nothing
about itself — which is why every such type written before this still
means what it did.

And defining a closure is not calling it, so a factory can be pure even
though what it hands back is not:

```cinder
pub fn make(limit: int) -> fn(int) -> int uses nothing {
    return |x: int| { println(x); return x + limit; };
}
```

**One honest limit.** A function declared without a body contributes
nothing, so an unannotated library is assumed pure — the same bargain as
"no clause is no claim". A `uses` clause does survive into an interface
file, so a library that annotates is believed.

`mut` is declarable and inert: Cinder has no `&mut` for it to be about
yet, and it is accepted now so programs need not change when it gains
meaning.

### Standard library

The whole of it, recognized directly by the compiler:

- `println(x)` and `print(x)` for `int`, `float`, `bool`, `string` and `String`
- `len(x)` for arrays, `Vec`s and `String`s
- `new_vec()`, `push(v, x)`, `pop(v)`
- `new_string()`, `push_str(s, text)`

---

## Examples

Everything in [`examples/`](examples) compiles and runs, and each one is
also a regression test in the suite.

| Program | Shows |
|---|---|
| [`hello.ci`](examples/hello.ci) | The smallest program |
| [`point.ci`](examples/point.ci) | Structs, methods, constants |
| [`fibonacci.ci`](examples/fibonacci.ci) | Loops and recursion |
| [`bubble_sort.ci`](examples/bubble_sort.ci) | Arrays, indexing, in-place mutation through `&T` |
| [`inventory.ci`](examples/inventory.ci) | An array of structs, methods calling methods |
| [`averages.ci`](examples/averages.ci) | Explicit `int`/`float` conversion with `as` |
| [`generics.ci`](examples/generics.ci) | Generic functions and structs, monomorphized |
| [`modules/`](examples/modules) | A program in three files, with `import` and `pub` |
| [`ownership.ci`](examples/ownership.ci) | `Vec`, `String`, moves and automatic drops |
| [`closures.ci`](examples/closures.ci) | Function values, captures, higher-order functions |
| [`contracts.ci`](examples/contracts.ci) | `requires` and `ensures` on functions and methods |
| [`units.ci`](examples/units.ci) | Units of measure, combined by `*` and `/` |
| [`effects.ci`](examples/effects.ci) | `uses` clauses bounding what a function may do |

```console
$ cinder run examples/bubble_sort.ci
before: 5 2 9 1 7 3 8 4
after:  1 2 3 4 5 7 8 9
```

---

## How the compiler is put together

One static library per pipeline stage, under [`libs/`](libs):

```
source text
  -> cinder::lexer     tokens with line/column spans
  -> cinder::parser    AST (recursive descent + Pratt for expressions)
  -> cinder::typeck    resolved types, symbol tables, diagnostics
  -> cinder::codegen   LLVM IR -> one native object file per module
  -> system linker    + cinder::std runtime -> executable
```

`cinder::ast` holds what the stages share: AST nodes, source spans, and
the diagnostic renderer. `cinder::std` is the runtime linked into every
compiled program — it backs `println` and reports runtime errors.

### Separate compilation

Codegen lowers one Cinder module at a time. Functions from other modules
become `declare` lines for the linker to resolve, so a module can be
rebuilt without re-lowering the rest of the program.

Monomorphized generics are the awkward case, because a copy of
`max<int>` belongs to no single module: the template is written in one
and demanded from others. Cinder does what C++ does — emits each copy
into every object that needs it under `linkonce_odr` linkage and lets
the linker keep one. The module that *wrote* the template emits nothing
for it; a generic function is not code until someone picks its types.

Demand is transitive. If `max3<int>` calls `max<int>`, every object
carrying the first also carries the second, so the checker propagates
demand to a fixpoint before codegen runs — a fixpoint rather than a walk
because generic functions may call each other in a cycle.

### Tests

```console
$ ctest --test-dir build --output-on-failure
```

Most of the suite is golden-file snapshots. Each `.ci` file in
`tests/golden/` has a recorded expectation per stage:

| File | Stage | Contents |
|---|---|---|
| `<case>.tokens` | Lexer | One token per line, with spans |
| `<case>.ast` | Parser | The syntax tree as an s-expression |
| `<case>.check` | Type checker | Diagnostics, or empty for a clean program |
| `<case>.out` | Codegen | stdout of the compiled program |

A clean program records an *empty* `.check` file, which pins the other
direction too: a change that starts rejecting valid code fails loudly.

After an intentional change, re-record and read the diff before
committing it:

```console
$ CINDER_UPDATE_GOLDEN=1 ./build/bin/cinder_snapshot_tests
```

---

## Known limitations

v1 is deliberately small. These are the sharp edges worth knowing about.

- **`as` converts between `int`, `float` and `bool` only.** There is no
  cast to or from `string` or a struct, and no reinterpreting cast.
  `float as int` truncates toward zero and a value too large for an
  `int` is undefined, both as in C; `bool as int` is 0 or 1 and
  `int as bool` is whether it is not zero, also as in C. `float` has no
  such convention with `bool`, so that pair is refused.
- **References are unchecked.** `&T` is a raw non-owning pointer with no
  borrow checker and no lifetimes. Returning a reference to a local, or
  holding one past the drop of what it points at, will compile and then
  dangle. Ownership governs who frees a buffer, not who may look at it.
- **A field or element cannot be moved out of what holds it.** Moving
  one out would leave the container half-owned, with no way for the drop
  code to know which parts are still live — so it is rejected. Move the
  whole struct, or use `pop` to shorten a `Vec` and take its last
  element back.
- **No `+` on text, and no slicing a `Vec`.** `push_str` appends and
  `slice` takes a sub-range of text, but `+` on strings is still not a
  thing — allocation stays visible, as it is in Rust — and only text can
  be sliced. Index a `Vec` instead.
- **The front end is still whole-program.** Codegen is incremental, but
  every build re-reads, re-parses and re-type-checks every module whose
  source it has. Interfaces make it *possible* to check a module against
  a description of its imports instead; nothing does that yet. On a
  13-module program a no-change rebuild is 0.29s against 0.93s from
  scratch, and of that 0.29s the front end is 0.08s and the link is most
  of the rest — so the reason the numbers stop improving is the linker,
  not the compiler.
- **The self-contained toolchain is Windows-only so far.** `cinder` on
  Windows carries LLD inside it and ships the archives it links
  against, so it needs no compiler on the target machine. On Linux and
  macOS it still shells out to an external linker at the path recorded
  when it was built, exactly as before — the link line here is
  MinGW-specific (`-m i386pep`, `crt2.o`, `libmsvcrt`), and the ELF and
  Mach-O equivalents are each their own job. Nothing is broken there;
  it is just not yet independent.

- **An install records no target triple.** It links for whatever
  machine built it. There is no cross-compilation and no way to ask for
  one, so a single install serves a single platform.

- **A shipped library is an object file and nothing else.** No archive,
  no target triple recorded, no ABI version. Handing someone an object
  built for a different platform fails at the link, or worse, and
  nothing checks. The package manager still distributes source; wiring
  interfaces into it would need all of that first.
- **A module's private functions are still symbols.** They carry the
  module prefix, so nothing collides and nothing links against them by
  accident, but they are not hidden. Making them internal would break a
  public generic that calls one, since that generic's body is
  monomorphized in the consumer's object.
- **Version selection does not backtrack.** It takes the highest
  version satisfying every requirement, iterated to a fixpoint. A graph
  that could only be satisfied by choosing a *lower* version of one
  package to make room for another is reported as a conflict rather than
  solved. Requirements are carets and exact versions only — no ranges,
  wildcards or pre-release tags.
- **A registry has no owners and no checksums.** Whoever can push to
  the index repository can publish anything under any name, and nothing
  verifies that a commit still contains what it did when it was
  published. `cinder publish` stops before pushing, so the git host's own
  access control is what stands in for all of this. There is no hosted
  index to point at.
- **A package's manifest is a subset of TOML.** Comments, `[section]`
  headers, string values, and one level of `{ ... }`. No numbers, no
  booleans, no arrays. Anything else is refused by name rather than
  ignored, so nothing silently fails to take effect.
- **Optimization stops at the module boundary by default.** A
  cross-module call crosses an object-file boundary that `-O3` cannot
  see across. `--whole-program` compiles everything as one unit and gets
  that back, at the cost of incremental builds; there is still no LTO,
  which would give both.
- **Nesting names modules; it does not scope them.** `shapes::geometry`
  gets no access to `shapes` and gives none, and there is no `super` or
  `self` to shorten a path with — every path is written in full from the
  root. Two files claiming one path is still an error rather than
  something resolved.
- **A package cannot seal anything off.** `pub` controls what another
  module may reach, not which modules may be imported, so nothing stops
  a program importing a package's internals directly if it knows the
  path. Nesting makes that unlikely by accident rather than impossible.

The spec's §6 sketches where these go next.

---

## Renaming the language

This section used to claim the name lived in four places. It was
written early and never tested, and when the language was actually
renamed — it was Ember before this — the real count was closer to 3,400
occurrences across 98 files, plus 8 directories and 52 source files that
had to move. What follows is what the rename actually took, so the next
one is a morning's work rather than a discovery exercise.

**In text.** Apply these in order, and anchor every lowercase rule with
`\b` on the **left only**:

| Pattern | Becomes |
|---|---|
| `libcinder_` | the runtime archive; it has no left boundary, so name it outright |
| `\bCINDER` | macro prefixes — `CINDER_TEST`, `CINDER_BINARY` |
| `\bCinder` | prose and `kLanguageName` |
| `\bcinder` | namespaces, `cinder_` symbols, `cinder/` include paths, `.cinder` |
| `\.cii` | interface files — before `.ci`, or it eats the stem |
| `\.ci` | source files |

The left anchor is not optional. "ember" is a substring of **member**,
**remember** and **December**, all of which occur in this source; an
unanchored replace turns them into `mcinder`, `recinder`, `Decinder`.
There is no boundary between the `m` and the `ember` of `member`, so
anchoring left protects them while still matching `ember_std`,
`ember::ast` and `ember/ast/ast.hpp`.

**In the filesystem.** `libs/*/include/<name>/` (8 directories), every
source and interface file, and four paths the text rules cannot see
because the name is in the filename: the manifest, the lockfile, the
vendored-modules directory, and the spec document.

**Constants that spell the extension without a dot**, and so match none
of the above: `kFileExtension`, `kInterfaceExtension`, `kLanguageName`
in [`libs/ast/include/cinder/ast/ast.hpp`](libs/ast/include/cinder/ast/ast.hpp)
and [`interface.hpp`](libs/ast/include/cinder/ast/interface.hpp).
`kManifestName` and `kLockName` in
[`manifest.hpp`](libs/manifest/include/cinder/manifest/manifest.hpp) do
contain the name and are handled by the text rules.

**Also `.gitattributes`**, which pins the line endings of `*.ci` and the
golden snapshots and is not a source file, so a suffix-driven pass
misses it.

The golden test harness reads `kFileExtension`, so the 31 example
programs need no separate handling once that constant moves.

---

## License

MIT — see [LICENSE](LICENSE). Use it for anything, including commercially;
just keep the copyright notice.

One thing to read first, if you are thinking of depending on this:
references are unchecked. Cinder has ownership and moves, but no borrow
checker, so a reference outliving what it points at is a use-after-free
that nothing diagnoses. That is a deliberate design choice, not a bug
queue — see [Known limitations](#known-limitations) for the rest of them.
