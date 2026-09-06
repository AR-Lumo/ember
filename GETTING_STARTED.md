# Getting started with Soliton

A walk from nothing to a multi-file program, in about twenty minutes of
reading. Every program here was run before it was written down, and the
output shown is what it actually printed.

If you want the reference instead, the [README](README.md) documents the
language feature by feature and
[the spec](soliton-language-spec.md) documents the design.

---

## 1. Getting a compiler

There are no prebuilt releases yet, so this step is building the
compiler from source. It needs LLVM, and it is the slowest part of this
guide by a wide margin — the compiler links all of LLVM statically and
comes out around 210 MB. Put the kettle on.

**Linux / macOS**

```bash
# Debian/Ubuntu: apt install llvm-dev lld clang cmake ninja-build
# macOS:         brew install llvm lld cmake ninja

git clone https://github.com/AR-Lumo/soliton.git
cd soliton
cmake -S . -B build -G Ninja
cmake --build build
```

**Windows (MSYS2 UCRT64)**

```powershell
C:\msys64\usr\bin\pacman -S --needed mingw-w64-ucrt-x86_64-gcc `
    mingw-w64-ucrt-x86_64-llvm mingw-w64-ucrt-x86_64-lld

$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ `
      -DLLVM_DIR="C:/msys64/ucrt64/lib/cmake/llvm" `
      -DCMAKE_PREFIX_PATH="C:/msys64/ucrt64"
cmake --build build
```

The compiler lands at `build/bin/soliton`. Check it:

```console
$ ./build/bin/soliton --version
soliton 0.1.0
```

Add it to your `PATH`, or write out the path each time — the rest of
this guide just says `soliton`.

**Once it is built, nothing else is needed to use it.** Soliton carries
its own linker and ships the archives it links against, so
`cmake --install build --prefix ~/soliton` produces a directory you can
copy to a machine with no compiler on it at all. LLVM and LLD are
needed to *build* Soliton, not to *use* it.

---

## 2. Hello

Put this in `hello.sn`:

```soliton
pub fn main() {
    println("Hello from Soliton");
}
```

```console
$ soliton run hello.sn
Hello from Soliton
```

`soliton run` compiles to a real native executable and runs it. There is
no interpreter and no virtual machine; the binary it made is an ordinary
program.

Three commands, and you will use all three:

| | |
|---|---|
| `soliton run file.sn` | compile and run, leaving nothing behind |
| `soliton check file.sn` | type-check only — fast, no code generated |
| `soliton build file.sn -o prog` | compile to an executable and stop |

`check` is the one to reach for while writing. It says nothing at all
when a program is fine:

```console
$ soliton check hello.sn
$
```

and tells you where you went wrong when it is not:

```console
$ soliton check oops.sn
error: type mismatch
 --> oops.sn:2:18
  |
2 |     let x: int = "hello";
  |                  ^^^^^^^ expected `int`, found `string`

error: aborting due to 1 previous error
```

---

## 3. Values and functions

```soliton
/// Average speed over a trip, in metres per second.
pub fn speed(distance: float, seconds: float) -> float {
    return distance / seconds;
}

pub fn main() {
    let distance = 1500.0;
    let seconds = 300.0;

    print("speed: ");
    println(speed(distance, seconds));
}
```

```console
$ soliton run speed.sn
speed: 5.0
```

Things worth noticing in eleven lines:

- `let` infers the type. Write `let distance: float = 1500.0;` when you
  want to say it out loud, and the checker will hold you to it.
- Bindings are immutable. `let mut` when you mean to change one — the
  same default Rust picked.
- `///` is a doc comment, `//` an ordinary one.
- **`int` and `float` never mix implicitly.** `1500 / 300.0` is an
  error, not a silent promotion. Write `1500 as float / 300.0`.
- `pub` marks what other modules may use. It does nothing in a
  single-file program, but costs nothing to write and saves editing
  later.

---

## 4. Structs and methods

Methods live in an `impl` block and are called with `.`:

```soliton
/// One leg of a journey.
struct Leg {
    pub distance: float,
    pub seconds: float,
}

impl Leg {
    /// Average speed over this leg, in metres per second.
    pub fn speed(&self) -> float {
        return self.distance / self.seconds;
    }
}

pub fn main() {
    let sprint = Leg { distance: 100.0, seconds: 9.6 };
    let jog = Leg { distance: 1500.0, seconds: 300.0 };

    print("sprint: ");
    println(sprint.speed());
    print("jog:    ");
    println(jog.speed());
}
```

```console
$ soliton run trip.sn
sprint: 10.416666666666668
jog:    5.0
```

That long number is not a bug. Floats print in the shortest form that
reads back as the same value, so you are seeing what the machine
actually holds rather than a rounded-off version of it.

`&self` borrows; `self` would take the whole struct by value. There are
no vtables and no dynamic dispatch — `sprint.speed()` compiles to a
plain function call with `sprint` as the first argument.

Struct fields need a trailing comma. `pub y: float,` — the last one too.

---

## 5. Growable data, and who owns it

`Vec<T>` and `String` own heap memory. Everything else so far has been
copied freely; these are the types where it matters who holds them.

```soliton
pub fn main() {
    let mut legs: Vec<String> = new_vec();

    let mut first: String = new_string();
    push_str(first, "sprint");
    push(legs, first);

    let mut second: String = new_string();
    push_str(second, "jog");
    push(legs, second);

    print("legs: ");
    println(len(legs));

    let mut i = 0;
    while i < len(legs) {
        print("  ");
        println(legs[i]);
        i = i + 1;
    }
}
```

```console
$ soliton run log.sn
legs: 2
  sprint
  jog
```

`push(legs, first)` **moves** `first` into the vector. Using `first`
afterwards is a compile error, not a double free — the vector owns those
bytes now. Both strings are freed when `legs` goes out of scope, with no
garbage collector and nothing to call by hand.

Two string types, and the split is the same one Rust makes:

- `String` owns a growable buffer.
- `string` is a borrowed, fixed-length view — what a literal is.

A `String` goes wherever a `string` is wanted. Not the other way round:
a view cannot become ownership without copying.

---

## 6. Saying what a function expects

Everything so far is a fairly ordinary small language. The next three
sections are the parts that are not.

A speed with zero seconds is not a slow speed, it is nonsense. Say so in
the signature rather than in the first line of the body:

```soliton
pub fn speed(distance: float, seconds: float) -> float
    requires seconds > 0.0
    ensures result >= 0.0
{
    return distance / seconds;
}

pub fn main() {
    print("ok:  ");
    println(speed(1500.0, 300.0));

    print("bad: ");
    println(speed(1500.0, 0.0));
}
```

```console
$ soliton run safe.sn
ok:  5.0
bad: soliton: requires contract violated in `speed`: seconds > 0.0
  --> safe.sn:3:5
```

`requires` is checked before the body runs; `ensures` before every
return, with `result` naming the value on its way out. Both are ordinary
`bool` expressions and can see the parameters — including `self` on a
method.

A violation stops the program and names the clause. It does not return a
wrong answer that travels somewhere else and surfaces an hour later.

These are run-time checks. Nothing is proved at compile time, so a
contract that *can* fail is not a compile error.

---

## 7. Units of measure

`float` will happily let you add a distance to a duration. Give the
numbers units and it will not:

```soliton
unit meters;
unit seconds;

/// The unit of the answer falls out of the division.
pub fn speed(distance: float<meters>, elapsed: float<seconds>) -> float<meters/seconds> {
    return distance / elapsed;
}

pub fn main() {
    let distance = 1500.0<meters>;
    let elapsed = 300.0<seconds>;

    print("speed: ");
    println(speed(distance, elapsed));
}
```

```console
$ soliton run measured.sn
speed: 5.0
```

Nobody declared `meters/seconds`. `*` and `/` combine units
algebraically, so dividing a distance by a time produces a speed on its
own, and multiplying it back by a time gives a distance again. `+` and
`-` require the same unit on both sides — which is the point:

```console
$ soliton check mixup.sn
error: cannot apply `+` to `float<meters>` and `float<seconds>`
 --> mixup.sn:7:20
  |
7 |     let nonsense = distance + elapsed;
  |                    ^^^^^^^^^^^^^^^^^^ the operands have different types
  = note: `+` needs the same unit on both sides (§10.2)
```

**Units cost nothing.** A `float<meters>` is a `double` in the generated
code; the whole thing lives in the type checker and is gone before
anything is emitted.

One rule to know: a unit attaches to a literal only when the `<` touches
it. `5.0<meters>` is a quantity; `5.0 < meters` is a comparison.

---

## 8. Saying what a function is allowed to do

A `uses` clause bounds what a function may do:

```soliton
unit meters;
unit seconds;

/// Pure, and held to it: this may not print, log, or do anything else.
pub fn speed(distance: float<meters>, elapsed: float<seconds>)
        -> float<meters/seconds> uses nothing {
    return distance / elapsed;
}

/// Allowed to print, and does.
pub fn report(distance: float<meters>, elapsed: float<seconds>) uses io {
    print("speed: ");
    println(speed(distance, elapsed));
}

pub fn main() {
    report(1500.0<meters>, 300.0<seconds>);
}
```

```console
$ soliton run pure.sn
speed: 5.0
```

Put a `println` inside `speed` and the compiler refuses it, naming the
line and the clause it broke. The check is transitive: calling something
that prints counts, however many calls away it is, and the blame lands
on the call you would have to change.

**A function with no clause has no bound.** That is deliberate, and it
is why `main` above needs no annotation and why every Soliton program
written before effects existed still compiles. Add `uses` where you want
a guarantee; leave it off everywhere else.

---

## 9. More than one file

A module is a file. `import` brings one into scope, and `::` reaches
into it.

```
trip/
├── units.sn
├── pace.sn
└── main.sn
```

`units.sn` — units are `pub` so other files can name them:

```soliton
pub unit meters;
pub unit seconds;
```

`pace.sn`:

```soliton
import units;

/// Average speed. Pure, so a caller knows it only computes.
pub fn speed(distance: float<meters>, elapsed: float<seconds>)
        -> float<meters/seconds> uses nothing
    requires elapsed > 0.0<seconds>
{
    return distance / elapsed;
}
```

`main.sn`:

```soliton
import units;
import pace;

pub fn main() {
    let distance = 1500.0<meters>;
    let elapsed = 300.0<seconds>;

    print("speed: ");
    println(pace::speed(distance, elapsed));
}
```

```console
$ soliton run trip/main.sn
speed: 5.0
```

Point the compiler at the file with `main` in it and it finds the rest
by following the imports. `pub` starts mattering here: drop it from
`speed` and `main.sn` can no longer see it.

To ship the program rather than run it:

```console
$ soliton build trip/main.sn -o trip
```

That produces a 591 KB executable that runs on its own — no runtime to
install, and on Windows it imports nothing that Windows does not
already ship.

---

## 10. Where to go next

- **[The examples](examples/)** — every one compiles, runs, and is a
  regression test in the suite. [`ownership.sn`](examples/ownership.sn)
  and [`closures.sn`](examples/closures.sn) are the two most worth
  reading next.
- **[The README](README.md)** — the reference. Generics, closures,
  references, the module search path, and the package manager, none of
  which this guide touched.
- **[The spec](soliton-language-spec.md)** — why the language is shaped
  this way, including the reasoning behind contracts, units and effects
  in §10.
- **[Known limitations](README.md#known-limitations)** — worth reading
  before you build anything real on this. The one that matters most:
  `&T` references are unchecked, so a reference outliving what it points
  at is a use-after-free that nothing diagnoses. That is a deliberate
  choice, not an oversight, but you should know about it going in.
