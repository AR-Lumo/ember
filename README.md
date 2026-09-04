# Ember

A small statically-typed, compiled language that produces real native
executables via LLVM. Structs, methods in `impl` blocks, fixed-size
arrays, references, and error messages that tell you what went wrong.

```ember
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
$ ember run examples/point.em
25
```

---

## Installing

Ember is a C++20 project built with CMake. You need:

| Requirement | Notes |
|---|---|
| A C++20 compiler | GCC 13+, Clang 16+, or MSVC 19.3+ |
| CMake 3.20+ | Plus any generator — Ninja or Make |
| LLVM 17+ development files | Headers, static libraries, and `LLVMConfig.cmake` |

LLVM is only needed by the code generator. **Without it the project still
builds**, and the lexer, parser and type checker — everything behind
`ember check` — work normally; only `ember build` and `ember run` are
unavailable.

### Linux / macOS

```bash
# Debian/Ubuntu: apt install llvm-dev clang cmake ninja-build
# macOS:         brew install llvm cmake ninja

cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

If CMake cannot find LLVM, point it at the install:

```bash
cmake -S . -B build -G Ninja -DLLVM_DIR=$(llvm-config --cmakedir)
```

### Windows (MSYS2 UCRT64)

The toolchain used to develop Ember. From an ordinary PowerShell prompt:

```powershell
# One-time: install the compiler and LLVM
C:\msys64\usr\bin\pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-llvm

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

The compiler lands at `build/bin/ember`. Add it to your `PATH`, or call
it by path as the examples below do.

### Build options

| Option | Default | Meaning |
|---|---|---|
| `EMBER_REQUIRE_LLVM` | `OFF` | Fail configuration instead of warning when LLVM is missing |
| `LLVM_DIR` | — | Path to the directory holding `LLVMConfig.cmake` |

---

## Hello, world

Put this in `hello.em`:

```ember
/// The smallest Ember program that produces output.
pub fn main() {
    println("hello, world");
}
```

Run it straight away:

```console
$ ember run hello.em
hello, world
```

Or compile it to a standalone executable that no longer needs the
compiler:

```console
$ ember build hello.em -o hello
$ ./hello
hello, world
```

Now break it deliberately — change the line to `println(missing);` and
type-check without generating code:

```console
$ ember check hello.em
error: cannot find value `missing`
 --> hello.em:3:13
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
ember build <file.em> [-o <output>]   compile to a native executable
ember run <file.em>                   compile and run in one step
ember check <file.em>                 type-check only, no codegen

  -O0 .. -O3       optimization level (default: -O0)
  -v, --verbose    say which modules were compiled and which were cached
      --fresh      recompile every module, ignoring cached object files
```

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

### Incremental builds

Each module compiles to its own object file, kept in a `.ember`
directory beside the entry source and reused when nothing it depends on
has changed:

```console
$ ember build main.em --verbose
compiling main.em
compiling shapes.em
compiling counter.em
 linking  main.exe

$ ember build main.em --verbose        # nothing edited
  cached  main.em
  cached  shapes.em
  cached  counter.em
 linking  main.exe
```

An object is valid as long as its module's source *and* the source of
everything that module imports are unchanged — a struct that changes
shape changes the code generated in every module that uses it. So
editing `shapes.em` rebuilds `shapes` and `main`, and leaves `counter`
alone:

```console
$ ember build main.em --verbose
compiling main.em
compiling shapes.em
  cached  counter.em
 linking  main.exe
```

The cache key is the fingerprint in the object's file name, so a hit is
just a file existing — there is no manifest that can disagree with what
is on disk. `--fresh` ignores it. Deleting `.ember` is always safe.

Each optimization level keeps its own objects, so working at `-O0` and
dropping to `-O2` to check something does not recompile the program each
way round.

---

## The language

A tour by way of the pieces. The full grammar is in
[`ember-language-spec.md`](ember-language-spec.md) §3.

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

```ember
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

```ember
let count = 1;            // inferred
let total: int = 0;       // annotated
let mut running = true;   // reassignable
```

A binding is immutable unless declared `mut`. Inner scopes may shadow
outer ones.

### Functions and methods

```ember
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

```ember
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
but Ember has no borrow checker either, and a silent out-of-bounds write
is a worse trade than a branch the optimizer usually removes.

### Generics

Functions and structs may take type parameters. Each combination of
argument types is compiled to its own function, so there is no boxing
and nothing is decided at run time:

```ember
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

Ember has no traits, so a type parameter carries no guarantees and a
generic body is **checked once per instantiation**, as C++ templates are
rather than Rust generics. `a > b` above is legal for `int` and not for a
struct, and that is only knowable once `T` is chosen — so the error is
reported against the body, with a note naming the call that caused it:

```console
error: cannot compare values of type `Point`
 --> sort.em:4:8
  |
4 |     if a > b {
  |        ^^^^^ `>` needs an `int` or a `float`
  = note: in `max` instantiated as `max<Point>` at sort.em:13:13
```

The trade-off is that a generic function nobody calls is never checked.

Struct literals infer their type arguments from the field values —
`Pair { left: 1, right: 2 }` is a `Pair<int>`. They are never written
out in an expression, where `Pair<int> { }` would be ambiguous with a
chain of comparisons; in type position they are explicit.

### Modules

A program may span several files. `import` names a module, and a module
called `geometry` lives in `geometry.em` beside the file that imports it:

In `geometry.em`:

```ember
pub struct Point {
    pub x: int,
    pub y: int,
}

pub fn magnitude_sq(p: Point) -> int {
    return p.x * p.x + p.y * p.y;
}

fn private_helper() -> int { return 1; }
```

In `main.em` beside it:

```ember
import geometry;

pub fn main() {
    let p = geometry::Point { x: 3, y: 4 };
    println(geometry::magnitude_sq(p));
}
```

```console
$ ember run main.em
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

Reach for `private_helper` from `main.em` and the note points into the
other file:

```console
error: function `geometry::private_helper` is private
 --> main.em:4:23
  |
4 |     println(geometry::private_helper());
  |                       ^^^^^^^^^^^^^^ `geometry::private_helper` is not declared `pub`
  = note: declared at geometry.em:10:4
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

### Dynamic arrays and strings

`[T; N]` has its length in its type. `Vec<T>` grows:

```ember
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

```ember
let mut message: String = new_string();
push_str(message, "built ");
push_str(message, "a piece at a time");
println(message);
```

`new_vec()` takes its element type from the binding, since there is no
turbofish and nothing in the call to infer from. That same rule now also
lets `let xs: [int; 0] = [];` work.

An element may own memory of its own — `Vec<String>`, `Vec<Vec<int>>`,
as deep as you like. Dropping such a vector is not one `free`: it walks
its live elements, drops each, and only then releases the buffer they
sat in.

```ember
let mut words: Vec<String> = new_vec();
push(words, make("ember"));       // `make` returns a String; it moves in

println(words[0]);                // reads an element without taking it
let last = pop(words);            // takes one back out, shortening the vector
```

Reading `words[i]` borrows. Moving an element *out* of the middle is
refused, because the vector would go on counting something it no longer
holds — `pop` is how ownership comes back, and the error says so:

```console
error: cannot move out of `String` here
  --> main.em:11:17
   |
11 |     let taken = words[0];
   |                 ^^^^^^^^ only a whole variable can be moved
   = note: a field or element cannot be moved out on its own, because what remains would be half-owned
   = note: `pop` takes the last element out of a `Vec` and shortens it, which leaves nothing half-owned
```

[`examples/word_list.em`](examples/word_list.em) is the whole thing end
to end.

### Ownership

`Vec` and `String` own heap memory, and so does any struct holding one.
Owned values **move** rather than copy, and are **freed automatically**
when their owner goes out of scope:

```ember
let mut a: Vec<int> = new_vec();
let b = a;           // the buffer moves to b
println(len(a));     // error: use of moved value `a`
```

```console
error: use of moved value `a`
 --> main.em:5:17
  |
5 |     println(len(a));
  |                 ^ `a` was moved and no longer holds a value
  = note: moved at main.em:4:13
  = note: `Vec<int>` owns heap memory, so assigning or passing it moves it rather than copying
```

Nothing is reference-counted and nothing is collected. The compiler works
out where each buffer dies and frees it there, so a million vectors
built and dropped in a loop hold flat memory.

Passing by value moves; passing `&T` borrows and does not:

```ember
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

```ember
let double = |x: int| -> int { return x * 2; };
println(double(21));                 // 42

let offset = 100;
let shift = |x: int| -> int { return x + offset; };
println(shift(5));                   // 105
```

Parameter and return types are written out, as they are everywhere else
in Ember. `||` with nothing between the bars is an empty parameter list.

**Captures are by value.** A closure copies what it mentions from the
surrounding scope into its own storage, which is why one can be returned
and still work:

```ember
pub fn scaler(factor: int) -> fn(int) -> int {
    return |x: int| -> int { return x * factor; };
}
```

A closure is an owned value, like a `Vec`, because a capturing one holds
a heap block. Calling it is a *use* rather than a move, so it can be
called as often as you like; passing it by value moves it, and
`&fn(...)` borrows it — which is what a higher-order function usually
wants:

```ember
pub fn map_in_place(values: &Vec<int>, f: &fn(int) -> int) { ... }
```

A closure that captures nothing has a null environment and allocates
nothing at all.

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
| [`hello.em`](examples/hello.em) | The smallest program |
| [`point.em`](examples/point.em) | Structs, methods, constants |
| [`fibonacci.em`](examples/fibonacci.em) | Loops and recursion |
| [`bubble_sort.em`](examples/bubble_sort.em) | Arrays, indexing, in-place mutation through `&T` |
| [`inventory.em`](examples/inventory.em) | An array of structs, methods calling methods |
| [`averages.em`](examples/averages.em) | Explicit `int`/`float` conversion with `as` |
| [`generics.em`](examples/generics.em) | Generic functions and structs, monomorphized |
| [`modules/`](examples/modules) | A program in three files, with `import` and `pub` |
| [`ownership.em`](examples/ownership.em) | `Vec`, `String`, moves and automatic drops |
| [`closures.em`](examples/closures.em) | Function values, captures, higher-order functions |

```console
$ ember run examples/bubble_sort.em
before: 5 2 9 1 7 3 8 4
after:  1 2 3 4 5 7 8 9
```

---

## How the compiler is put together

One static library per pipeline stage, under [`libs/`](libs):

```
source text
  -> ember::lexer     tokens with line/column spans
  -> ember::parser    AST (recursive descent + Pratt for expressions)
  -> ember::typeck    resolved types, symbol tables, diagnostics
  -> ember::codegen   LLVM IR -> one native object file per module
  -> system linker    + ember::std runtime -> executable
```

`ember::ast` holds what the stages share: AST nodes, source spans, and
the diagnostic renderer. `ember::std` is the runtime linked into every
compiled program — it backs `println` and reports runtime errors.

### Separate compilation

Codegen lowers one Ember module at a time. Functions from other modules
become `declare` lines for the linker to resolve, so a module can be
rebuilt without re-lowering the rest of the program.

Monomorphized generics are the awkward case, because a copy of
`max<int>` belongs to no single module: the template is written in one
and demanded from others. Ember does what C++ does — emits each copy
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

Most of the suite is golden-file snapshots. Each `.em` file in
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
$ EMBER_UPDATE_GOLDEN=1 ./build/bin/ember_snapshot_tests
```

---

## Known limitations

v1 is deliberately small. These are the sharp edges worth knowing about.

- **`as` converts between `int` and `float` only.** There is no cast to
  or from `bool`, `string` or a struct, and no reinterpreting cast.
  `float as int` truncates toward zero, and a value too large for an
  `int` is undefined — both as in C.
- **References are unchecked.** `&T` is a raw non-owning pointer with no
  borrow checker and no lifetimes. Returning a reference to a local, or
  holding one past the drop of what it points at, will compile and then
  dangle. Ownership governs who frees a buffer, not who may look at it.
- **A field or element cannot be moved out of what holds it.** Moving
  one out would leave the container half-owned, with no way for the drop
  code to know which parts are still live — so it is rejected. Move the
  whole struct, or use `pop` to shorten a `Vec` and take its last
  element back.
- **No slicing, no concatenation operator.** `push_str` builds a
  `String`; `+` on strings is still not a thing, and there is no way to
  take a sub-range of either a `Vec` or a `String`.
- **No capacity control.** No `reserve`, no `shrink`, no way to ask what
  a container has allocated.
- **Generic methods and `impl<T>` blocks are not implemented.** Generic
  free functions and generic structs work; a method with its own type
  parameters, or an `impl` block over a generic type, is reported as
  unsupported rather than mis-compiled.
- **A closure cannot capture an owned value.** It frees its captures as
  one block and has no per-closure code to drop them individually, so
  capturing a `Vec` or a `String` is refused rather than leaked. Pass it
  as an argument instead.
- **Closure parameter types are never inferred.** `|x| x + 1` is not
  valid; write `|x: int| -> int { return x + 1; }`.
- **The front end is still whole-program.** Codegen is incremental, but
  every build re-reads, re-parses and re-type-checks every module: on a
  13-module program a no-change rebuild is 0.29s against 0.93s from
  scratch, and of that 0.29s the front end is 0.08s and the link is most
  of the rest. Cheap enough to leave alone at this size, and the reason
  the numbers stop improving is the linker, not the compiler.
- **There is no compiled-library format.** Building a module still needs
  the *source* of everything it imports, because there is no interface
  file recording another module's types and signatures. That is what a
  package manager distributing binaries would need; one distributing
  source, the way Cargo does, would not.
- **Optimization stops at the module boundary.** A cross-module call
  used to be a direct call in one LLVM module and could be inlined; now
  it crosses an object-file boundary that `-O3` cannot see across.
  There is no LTO to win that back.
- **Module paths are one level deep.** `geometry::Point` works;
  `shapes::geometry::Point` does not. There are no nested modules and no
  search path — an imported module is a file beside the importer.

The spec's §6 sketches where these go next.

---

## Renaming the language

"Ember" and `.em` appear in four places: the `ember::*` library
namespaces, the CLI binary name, `kLanguageName` / `kFileExtension` in
[`libs/ast/include/ember/ast/ast.hpp`](libs/ast/include/ember/ast/ast.hpp),
and the spec document. The extension check in the CLI reads
`kFileExtension`, so changing that constant is enough to move the file
extension.
