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
```

Exit codes are `0` on success, `1` when the program failed to compile,
and `2` when the command line itself was wrong. A compiled program that
hits a runtime error — an out-of-bounds index, a division by zero —
exits `101`.

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

### Standard library

The whole of it, recognized directly by the compiler:

- `println(x)` and `print(x)` for `int`, `float`, `bool` and `string`
- `len(arr)` for arrays

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
  -> ember::codegen   LLVM IR -> native object file
  -> system linker    + ember::std runtime -> executable
```

`ember::ast` holds what the stages share: AST nodes, source spans, and
the diagnostic renderer. `ember::std` is the runtime linked into every
compiled program — it backs `println` and reports runtime errors.

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
- **No manual memory management yet.** There is no allocation, so there
  is nothing to free — but also no way to build a data structure whose
  size is not known at compile time.
- **References are unchecked.** `&T` is a raw non-owning pointer with no
  borrow checker and no lifetimes. Returning a reference to a local will
  compile and then dangle.
- **Strings are fixed views.** No concatenation, no slicing, no building
  one at run time. `==` and `!=` work; ordering does not.
- **Arrays are fixed-size.** The length is part of the type, so `len` is
  a compile-time constant and there is no `push`.
- **`pub` is parsed but not enforced.** Visibility starts mattering when
  modules land, so it is checked and carried through the compiler now to
  avoid a syntax change later.
- **No generics, closures, or function values.**
- **One file per program.** No `import`.

The spec's §6 sketches where these go next.

---

## Renaming the language

"Ember" and `.em` appear in four places: the `ember::*` library
namespaces, the CLI binary name, `kLanguageName` / `kFileExtension` in
[`libs/ast/include/ember/ast/ast.hpp`](libs/ast/include/ember/ast/ast.hpp),
and the spec document. The extension check in the CLI reads
`kFileExtension`, so changing that constant is enough to move the file
extension.
