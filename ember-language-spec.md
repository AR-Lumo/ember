# Ember Language — Design Spec & Build Plan

> Working name: **Ember** (file extension `.em`). Rename freely — it's a
> find-and-replace across the codebase, called out at the end of this doc.

This document is written to be handed directly to an AI coding agent
(e.g. Claude Code) as a build spec. It's organized so the agent can work
phase-by-phase, with each phase producing a compiling, testable artifact.

---

## 1. Goals & Non-Goals

**Goals**
- A statically-typed, compiled, C++-like language that produces real
  native executables via LLVM.
- Small, orthogonal core: functions, structs, primitives, control flow,
  arrays/slices, pointers/references.
- Good error messages from day one (this is what separates a toy from
  something people will actually use).
- Fast to build incrementally — every phase below ends in a working
  compiler, not a half-finished one.

**Explicit non-goals (v1)**
- No garbage collector. Manual memory management first; an
  ownership/borrow model can be layered on later (v2+) once the core
  pipeline is solid.
- No generics in v1 (add in v2).
- No macros/metaprogramming in v1.
- No standard library beyond the minimum needed to write real
  programs (println, basic math, string/array ops).

---

## 2. Tech Stack

| Component        | Choice                                   | Why |
|-------------------|-------------------------------------------|-----|
| Implementation language | **C++ (C++20)**                    | Matches the target language's own feel; LLVM's native API is C++, so no binding layer or FFI risk |
| Backend            | **LLVM**, linked directly via its C++ API | Don't hand-write codegen; LLVM gives optimization + every target architecture for free |
| Parser strategy    | Hand-written recursive descent + Pratt parsing for expressions | Full control over error messages; no parser-generator black box |
| Build tool          | CMake, multiple static libraries        | Clean separation of pipeline stages; standard for LLVM-based C++ projects |
| Testing            | Golden-file snapshot tests (via CTest)   | Confidence at every step |

### Project layout (CMake project)

```
ember/
├── CMakeLists.txt             # top-level, finds LLVM, adds subdirectories
├── libs/
│   ├── lexer/                 # ember::lexer
│   ├── parser/                # ember::parser — depends on lexer, produces AST
│   ├── ast/                   # ember::ast — shared AST node types
│   ├── typeck/                # ember::typeck — type checker, depends on ast
│   ├── codegen/                # ember::codegen — AST/typed-AST -> LLVM IR, via LLVM's C++ API
│   └── std/                   # minimal runtime support (linked into binaries)
├── src/
│   └── main.cpp                # `ember` binary: build/run/check subcommands
├── tests/
│   ├── golden/                 # .em source files + expected output
│   └── snapshot_tests.cpp      # CTest-driven
└── examples/
    └── *.em                    # sample programs, one per language feature
```

**A note on memory management for the compiler itself:** LLVM's C++ API
leans on raw pointers and its own ownership conventions (`Module` owns
`Function`s, etc. via `unique_ptr`-like patterns internally). For your
own AST and symbol tables, prefer `std::unique_ptr`/`std::shared_ptr`
over raw `new`/`delete` — this is standard modern-C++ practice and
avoids the manual-memory-bugs-in-your-own-compiler trap, which is a
different problem from Ember's own (intentionally manual) memory model.

---

## 3. Syntax (EBNF, v1 core)

Naming and structural conventions follow the same lineage as Rust/Swift/Go,
so the language reads as familiar rather than idiosyncratic:

| Convention | Rule | Example |
|---|---|---|
| Functions, variables, fields | `snake_case` | `distance_sq`, `left_edge` |
| Types (struct, later enum) | `PascalCase` | `Point`, `LineSegment` |
| Constants | `SCREAMING_SNAKE_CASE` | `MAX_RETRIES` |
| Doc comments | `///` above the item | `/// Computes squared distance.` |
| Regular comments | `//` | `// TODO: revisit precision` |
| Visibility | explicit `pub`, private by default | `pub fn area(&self) -> float` |
| Methods | declared in `impl` blocks, not loose functions | `impl Point { fn area(&self) ... }` |

```ebnf
program        = { item } ;
item           = function_decl | struct_decl | impl_block | const_decl
               | import_decl | unit_decl ;

unit_decl      = "unit" identifier ";" ;                   (* v1.1 *)

import_decl    = "import" module_path ";" ;                (* v2 *)
module_path    = identifier { "::" identifier } ;          (* v2 *)
qualified      = { identifier "::" } identifier ;          (* an item, with
                                                              the module path
                                                              it lives in *)

visibility     = [ "pub" ] ;

const_decl     = visibility "const" identifier ":" type "=" expression ";" ;

function_decl  = visibility "fn" identifier [ generic_params ]
                 "(" [ param_list ] ")" [ "->" type ]
                 [ effect_clause ] { contract } ( block | ";" ) ;
                                            (* `;` declares without
                                               defining: an interface
                                               file is written this
                                               way, v2 *)

effect_clause  = "uses" effect { "," effect } ;            (* v1.1 *)
effect         = "io" | "mut" | "nothing" ;  (* a closed set for now:
                                                easy to add to, hard to
                                                take away once relied on.
                                                `nothing` is the empty
                                                bound and cannot be
                                                combined with either of
                                                the others *)

contract       = ( "requires" | "ensures" ) expression ;   (* v1.1 *)
                                            (* `requires` is checked on
                                               entry, `ensures` before
                                               return, where `result`
                                               names the return value *)

generic_params = "<" identifier { "," identifier } ">" ;   (* v2 *)
param_list     = param { "," param } ;
param          = ( "self" | "&self" ) | identifier ":" type ;

struct_decl    = visibility "struct" identifier [ generic_params ] "{" { field } "}" ;
field          = visibility identifier ":" type "," ;

impl_block     = "impl" [ generic_params ] identifier [ type_args ]
                 "{" { function_decl } "}" ;
                                            (* `impl<T> Pair<T>`, v2 *)

type           = ( "int" | "float" ) [ "<" unit_expr ">" ]
                                           (* a unit, v1.1: `float<meters>`.
                                              Compile-time only - the
                                              generated IR is the same
                                              double either way *)
               | "bool" | "string"
               | "fn" "(" [ type { "," type } ] ")" [ "->" type ]
                                           (* function value, v2 *)
               | "Vec" "<" type ">"        (* growable array, v2 *)
               | "String"                  (* growable string, v2 *)
               | qualified [ type_args ]   (* struct type, optionally
                                              from another module *)
               | "&" type                  (* reference *)
               | "[" type ";" int_lit "]"  (* fixed-size array *) ;

type_args      = "<" type { "," type } ">" ;               (* v2 *)

unit_expr      = unit_term { ( "*" | "/" ) unit_term } ;   (* v1.1 *)
unit_term      = identifier ;                (* a declared unit name, or
                                                one derived by `*` and
                                                `/` - a derived unit
                                                needs no declaration *)

block          = "{" { statement } "}" ;

statement      = let_stmt | return_stmt | if_stmt | while_stmt
               | expr_stmt | assign_stmt ;

let_stmt       = "let" [ "mut" ] identifier [ ":" type ] "=" expression ";" ;
return_stmt    = "return" [ expression ] ";" ;
if_stmt        = "if" expression block [ "else" ( if_stmt | block ) ] ;
while_stmt     = "while" expression block ;
assign_stmt    = lvalue "=" expression ";" ;
expr_stmt      = expression ";" ;

expression     = literal | identifier | unary_expr | binary_expr
               | call_expr | method_call | field_access | index_expr
               | struct_literal | array_literal | cast_expr | closure
               | "(" expression ")" ;

cast_expr      = expression "as" type ;      (* explicit conversion, see 4 *)
closure        = "|" [ closure_param { "," closure_param } ] "|"
                 [ "->" type ] block ;                      (* v2 *)
closure_param  = identifier [ ":" type ] ;  (* the type may be left out
                                               and taken from what the
                                               closure is passed to *)
array_literal  = "[" [ expression { "," expression } ] "]" ;

call_expr      = qualified "(" [ arg_list ] ")" ;
method_call    = expression "." identifier "(" [ arg_list ] ")" ;
arg_list       = expression { "," expression } ;
field_access   = expression "." identifier ;
index_expr     = expression "[" expression "]" ;
struct_literal = qualified "{" [ field_init_list ] "}" ;
field_init_list = identifier ":" expression { "," identifier ":" expression } ;

literal        = ( int_lit | float_lit ) [ "<" unit_expr ">" ]
                                           (* `5.0<meters>`, v1.1 *)
               | bool_lit | string_lit ;
```

**Operator precedence (low to high):**
`|| → && → == != → < > <= >= → + - → * / % → as → unary - ! → call/method/index/field`

### Example program (target for Phase 4)

Methods live in `impl` blocks and are called with `.` — this is what
gives the language a professional, modern feel rather than a purely
procedural C dialect, while still compiling down to plain function
calls (no vtables, no dynamic dispatch in v1).

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

---

## 4. Type System (v1)

- Primitives: `int` (i64), `float` (f64), `bool`, `string` (immutable,
  fixed-length view — no dynamic resizing in v1).
- `struct` types: nominal, no inheritance.
- `&T` references: non-owning pointer, no borrow checker in v1 (just
  don't let it dangle — document as a known limitation).
- Fixed-size arrays `[T; N]`.
- No implicit numeric coercion (`int` and `float` never silently mix —
  require explicit cast). This is a deliberate simplicity choice.
- Functions are not first-class in v1 (no closures, no function
  pointers) — add in v2.
- Methods (`impl` blocks) are syntactic sugar over plain functions —
  `p.distance_sq(other)` compiles to the same thing as
  `Point_distance_sq(p, other)` would. No vtables or dynamic dispatch
  in v1; `self` is always statically resolved. This keeps codegen in
  Phase 4 simple while the surface syntax looks modern.
- `pub` controls visibility across files once the module system lands
  in v2; in v1 (single-file programs) it's parsed and type-checked but
  has no enforcement effect yet — worth a `// TODO` comment in the
  checker rather than skipping it, so v2 doesn't require a syntax change.

---

## 5. Standard Library (v1, minimal)

Implemented as a small runtime (`ember_std`) linked into every binary,
plus compiler-recognized intrinsics:

- `println(x)` — works for `int`, `float`, `bool`, `string`
- `print(x)` — same, no newline
- Arithmetic/comparison operators (compiler intrinsics, not library calls)
- `len(x)` for arrays, `Vec`s, `string`s and `String`s
- `new_vec()`, `push(v, x)`, `pop(v)` for `Vec<T>` (v2)
- `new_string()`, `push_str(s, text)` for `String` (v2)
- `capacity(c)`, `reserve(c, n)` for either growable container (v2)
- `slice(text, start, end)` — a borrowed view, no copy (v2)
- `find(haystack, needle)` — byte offset, or -1 (v2)
- `contains(haystack, needle)` (v2)

Text reads the same whichever type holds it: everything above that only
*reads* takes a `string` or a `String`, comparison works across the two,
and a `String` passed where a `string` is wanted lends a view of itself
rather than moving. (v2)

Everything else (file I/O, collections) is v2+.

---

## 6. CLI Design

```
ember build file.em -o output      # compile to native executable
ember run file.em                  # compile + run in one step
ember check file.em                # type-check only, no codegen
ember fetch                        # resolve dependencies             (v2)
ember publish                      # add this version to the index     (v2)
ember interface file.em            # write the module's interface      (v2)

  -L, --module-path <dir>          # extra place to find modules       (v2)
      --update                     # re-resolve git dependencies       (v2)
      --dry-run                    # `publish`: report, write nothing  (v2)
      --lib                        # compile to an object, no `main`   (v2)
      --whole-program              # one unit, so -O inlines across    (v2)
      --link <path>                # link this object in as well       (v2)
  -O0 .. -O3                       # optimization level, default -O0   (v2)
  -v, --verbose                    # report per-module compile/cache decisions
      --fresh                      # ignore cached object files       (v2)
```

Builds are incremental from v2 on: each module becomes its own object
file, cached beside the entry source and reused when neither it nor
anything it imports has changed.

---

## 7. Error Messages

Non-negotiable from Phase 2 onward — this is what makes it feel like a
real tool instead of a toy. Format:

```
error: type mismatch
  --> file.em:12:9
   |
12 |     let x: int = "hello";
   |                  ^^^^^^^ expected `int`, found `string`
```

Use source spans (line/col) attached to every AST node from the parser
onward so this is possible without retrofitting later.

---

## 8. Phased Build Plan

Each phase should end with passing tests and a tagged commit before
moving to the next. Don't let phases blend together.

### Phase 0 — Scaffolding
- CMake project with an empty static library per the layout above.
- CLI skeleton that parses subcommands but does nothing yet.
- CI-style test harness reading `.em` files from `tests/golden/`.

### Phase 1 — Lexer
- Tokenize all literals, identifiers, keywords, operators, punctuation.
- Track line/column spans on every token.
- Handle comments (`//` line comments) and whitespace.
- Test: snapshot the token stream for a handful of sample files.

### Phase 2 — Parser & AST
- Recursive descent for statements/items, Pratt parsing for
  expressions (respecting the precedence table in §3).
- Parse `impl` blocks, `pub`, `const`, and struct literals alongside
  the core statement/expression grammar — these are cheap to add now
  and expensive to retrofit once codegen assumes loose functions.
- AST nodes carry spans (needed for error messages later).
- Parser produces useful syntax errors (not just "parse failed").
- Test: snapshot the AST (as debug-printed or a simple s-expr form)
  for each example program.

### Phase 3 — Type Checker
- Symbol table / scope resolution for functions, structs, locals,
  and methods (an `impl Point { fn f(&self) }` registers `f` scoped
  to `Point`, resolved at the `p.f()` call site — not in the global
  function namespace).
- Type inference for `let` without annotation; otherwise checks
  annotation vs. inferred expression type.
- Full error messages per §7 for: type mismatches, undefined
  identifiers, wrong arg count/types, missing return, duplicate
  definitions, unknown method on a type.
- Test: a set of programs that should fail, asserting the exact
  error produced; a set that should pass.

### Phase 4 — Codegen (LLVM, native C++ API)
- Lower typed AST to LLVM IR using `llvm::IRBuilder`: functions,
  structs (as `llvm::StructType`), arithmetic, control flow (if/while
  as basic blocks + branches via `llvm::BasicBlock`), struct field
  access (`CreateStructGEP`), arrays.
- Lower methods to plain functions with a mangled name (e.g.
  `Point_distance_sq`) taking `self` as an explicit first parameter —
  confirms the "methods are sugar" design decision from §4 actually
  holds at codegen time.
- Link against a minimal runtime for `println`/`print`.
- `ember build` produces a real native executable (via LLVM's target
  machine + object emission, then invoking the system linker).
- Test: compile + execute each example program, assert stdout.
  **This is the milestone from the example program in §3.**

### Phase 5 — Polish & Real Usability
- `ember run` convenience command.
- Better diagnostics (multiple errors per run instead of stopping at
  first; suggestions like "did you mean `x`?").
- A handful of real example programs (fibonacci, bubble sort, a small
  struct-heavy program) as both regression tests and demos.
- README with install instructions and a "hello world" walkthrough.

### Phase 6+ (v2, after v1 is solid)
- Arrays with dynamic length (grow/shrink).
- Strings as proper owned, growable buffers.
- Generics (monomorphization, like Rust/C++ templates).
- Closures / function values.
- A real memory model (ownership or reference counting) instead of
  raw unchecked references.
- Module system (`import`) for multi-file programs.
- Separate compilation: one object file per module, so editing one
  module does not re-lower the rest.
- A module search path, so a program can import code that does not sit
  beside it.
- Package manager: `ember.toml`, path, git and registry dependencies,
  semver requirements, and an `ember.lock` pinning the version and
  commit each dependency resolved to. A registry is a directory of index
  files, hosted as a directory or a git repository. `ember publish` adds
  a version to one, stopping short of pushing it. Packages are
  distributed as source. Still to come: ownership and checksums for a
  registry. Interface files (`.emi`) let a program be built against a
  library it does not have the source of; wiring them into the package
  manager additionally needs an archive format and a recorded target
  triple, so that shipping an object for the wrong platform is caught
  rather than discovered at the link.

---

## 9. Notes for the Coding Agent

- Work phase by phase. Don't start Phase 4 codegen before Phase 3's
  type checker has real test coverage — bugs compound badly across
  stages in a compiler.
- Prefer many small, focused commits over large ones — each phase
  above is a natural commit boundary.
- When in doubt about a syntax or semantics question not covered
  here, default to "what would C do" for anything low-level (memory,
  pointers) and "what would Rust do" for anything about ergonomics
  (let bindings, struct literals) — this keeps the language's feel
  internally consistent.
- Rename "Ember" / `.em` throughout via a simple find-and-replace if
  a different name is wanted — it appears in: library/namespace names
  (`ember::*`), the CLI binary name, the file extension check in the
  CLI, and this doc.
- Since the compiler is written in C++, prefer `std::unique_ptr` /
  `std::shared_ptr` for AST and symbol-table ownership rather than raw
  `new`/`delete` (see §2) — this keeps the compiler's own code
  memory-safe by convention even though Ember-the-language itself has
  no such guarantees in v1.

---

## 10. v1.1 Roadmap - Signature Features

**Status: all three are implemented.** §10.3's inference rule was
written down before any of its code, as that section asked, and the
resolution it reached is recorded there.

These three are what would make Ember distinctive rather than
"Rust-flavoured syntax on LLVM". Treat this as its own miniature version
of the phased plan in §8: one feature at a time, each with its own
tests, each ending in a tagged release - not all three in one patch.

### 10.1 Contracts (`requires` / `ensures`) - **implemented**

Preconditions and postconditions live in the signature and are checked
at every call, instead of being the first few lines of the body.

```ember
pub fn divide(a: int, b: int) -> int
    requires b != 0
    ensures result != 0 || a == 0
{
    return a / b;
}
```

- `requires <expr>` - checked on entry; failure is a runtime panic
  naming the violated contract and the call site, in the §7 format.
- `ensures <expr>` - checked just before return. `result` is bound to
  the return value and is in scope only inside `ensures`.
- **Implementation:** in codegen this is `if (!condition) { panic(...) }`
  at entry and exit - no new IR concepts. The checker's job is that the
  expressions are `bool` and that `result` appears only in `ensures`.
- v1.1 scope is runtime-checked only. Proving contracts statically, as
  Ada/SPARK does, is a far larger effort and explicitly out of scope.

**As built.** A violation prints the clause as written, the function it
guards, and where the clause is:

```
ember: requires contract violated in `divide`: b != 0
  --> divide.em:3:5
```

`result` is deliberately **not** a keyword. Reserving it would break
every existing program with a variable by that name, for a word that
means something in exactly one place; the checker binds it while an
`ensures` is in scope instead, and reports a specific error for a
`result` used in a `requires` or in a function that returns nothing.

Two details that are load-bearing rather than incidental:

- The condition is parsed the way an `if` condition is, with struct
  literals disallowed. Otherwise `requires b != 0 { ... }` reads
  `0 { ... }` as a struct literal and swallows the function body with no
  syntax error to show for it.
- An `ensures` is emitted with the return value in hand but **before**
  the function's drops, so a condition may still mention a local. The
  other order would have it read freed memory.

**One deviation from the above.** The message names the contract, not
the call site. Reporting the caller means passing its position into
every call to a contracted function, which changes the ABI of those
functions and has to survive separate compilation and interface files.
The clause's own position is what a reader needs first; the call site
can follow if it earns its cost.

### 10.2 Units of measure - **implemented**

Numeric types may carry a physical unit; mixing incompatible ones is a
compile error.

```ember
unit meters;
unit seconds;

pub fn main() {
    let d: float<meters> = 5.0<meters>;
    let t: float<seconds> = 2.0<seconds>;
    let speed = d / t;              // float<meters/seconds>
    // let bad = d + t;             // error: incompatible units
}
```

- `unit` introduces a name. Untagged `int` and `float` stay valid and
  unitless, so nothing existing has to change.
- `+` and `-` require identical units. `*` and `/` combine them
  algebraically; a derived unit such as `meters/seconds` needs no
  declaration of its own.
- **Implementation:** compile-time only. A `float<meters>` is an
  ordinary `double` in the generated IR, so codegen is untouched. The
  work is in the checker: a unit term on the type representation, and
  every arithmetic path taught the rules above.
- The most invasive of the three, because it touches every arithmetic
  type-check path.

**As built.** A dimension is a canonical list of `(unit, exponent)`
pairs - sorted, with anything that cancelled removed - and it is part of
the type's interning key. That does most of the work for free:
`float<meters>` and `float<seconds>` are simply different types, so the
existing mismatch checks catch `d + t` with no special case, and only the
*message* had to learn about units.

`*` and `/` are the one place units combine rather than having to match:
they add and subtract exponents, so `meters/meters` is a plain number
again and `d / t / t` is `meters/seconds^2`.

`^` is an extension to the grammar above, which had `unit_term =
identifier`. Without it a type the compiler prints could not be typed
back in: `meters/seconds^2` has no spelling as a product, because
`meters/seconds*seconds` cancels. Round-tripping is worth one token.

**Parsing `5.0<meters>`.** A `<` after a number is otherwise a
comparison, and no amount of lookahead settles it - `5.0<meters>` and
`5.0 < meters` are both grammatical. Two conditions have to hold before a
unit is read: the `<` must *touch* the literal, and the tokens after it
must form a complete `<unit_expr>`. Adjacency alone would break
`f(5<x, 3)`, a comparison written without spaces. The shape alone would
silently change what `5 < meters` means the day somebody declares a unit
by that name.

**Units are erased.** A `float<meters>` is a `double`, and codegen never
reads the dimension. `tests/units_tests.cpp` compiles a program with
units and the same program without them and asserts the IR is identical,
so the claim stays true rather than merely having been true once.

**One deviation.** The grammar above gives `unit_decl` no visibility.
`pub unit` is accepted, because a unit that cannot cross a module
boundary is useless the moment a program has two files.

### 10.3 Effect annotations (`uses io`, `uses mut`) - **implemented**

Functions declare the side effects they perform, and the compiler stops
an effectful call from a function that has not declared it.

```ember
pub fn read_config(path: string) -> string uses io {
    return read_file(path);   // read_file is itself `uses io`
}

pub fn distance_sq(a: Point, b: Point) -> int {
    // no `uses` clause: pure. Calling an `io` function here
    // would be a compile error.
}
```

- Two effects to start: `io` and `mut`. Keep the vocabulary small - it
  is easy to add one and impossible to remove one people depend on.
- A function's effect set is the union of what it calls; the checker
  infers it rather than making every function annotate. **The inference
  rule needs writing down here before any code is written**, not
  deciding case by case at the keyboard.
- **Implementation:** almost entirely in `typeck`, with no codegen or
  runtime changes. It is a second, effect-flavoured type system layered
  over the first, which is why it is the hardest to get right while
  touching the least code.
- Worth prototyping on paper first. Effect systems are where "seemed
  simple, turned out to have edge cases" bites hardest.

#### The inference rule, written down first

The two paragraphs above contradict each other, and the contradiction
has to be settled before any code is written - which is what this
section is for.

> "the type checker computes this, it isn't hand-annotated everywhere"

says effects are **inferred**. But the worked example says

> "no `uses` clause: this function is pure - calling an `io`-effect
> function here would be a compile error"

which says an absent clause **declares purity**. Those give opposite
answers for `pub fn main() { println(1); }`: inferred, it quietly has
`io`; declared, it is an error.

Declared-by-absence is not viable. Every one of the 31 programs in
`examples/` and `tests/golden/` prints, across 130 functions, so it
would break every Ember program ever written - and every program anyone
else has written, since the language is published and has a registry.
A v1.1 feature does not get to do that.

So: **effects are inferred, and a `uses` clause is a checked upper
bound.**

1. **Inference.** Every function has an effect set. `println` and
   `print` contribute `io`. A call contributes the callee's effect set.
   Contracts count - a `requires` that prints performs IO. Everything
   else contributes nothing.

2. **Recursion.** The set is the least fixed point: start every function
   at empty and iterate until nothing changes. Mutual recursion
   therefore terminates instead of chasing its own tail.

3. **A clause is a bound.** `uses io` means *at most* `io`. The checker
   compares the inferred set against it and reports the effect together
   with the call that introduced it.

4. **No clause means no bound.** The function still has an inferred set,
   which its callers see. This is what keeps existing code compiling:
   nothing is checked until somebody asks for it.

5. **`uses nothing` is the empty bound** - the way to say "pure" and
   have it enforced. This is the spec's `distance_sq` example, made
   explicit rather than implied by absence.

6. **An indirect call contributes every effect a function can perform**,
   which today is `io`. A `fn(int) -> int` type carries no effect
   information, so calling through a function value could do anything.
   Treating it as unknown-and-therefore-worst keeps a bound honest:
   `uses io` still permits it, `uses nothing` refuses it and says why.

7. **`mut` is accepted and inert.** Nothing produces it, because Ember
   has no `&mut` for it to be about - exactly as the paragraph above
   says. It is declarable so that programs written now do not have to
   change when it grows meaning.

**Worked examples.**

```ember
pub fn area(w: int, h: int) -> int uses nothing {
    return w * h;              // ok: pure, and says so
}

pub fn report(w: int, h: int) uses io {
    println(area(w, h));       // ok: io is within the bound
}

pub fn quiet(w: int) uses nothing {
    println(w);                // error: `io` is not permitted here
}

pub fn shout(w: int) uses nothing {
    report(w, 1);              // error: `io`, through the call to `report`
}

pub fn main() {
    println(area(2, 3));       // no clause, no bound, no error
}
```

**What this does not do.** A bound says what a function may do, not what
it must; `uses io` on something that never prints is allowed, the way an
unused `throws` is in Java. And effects are not part of a function type,
which is why rule 6 has to be so blunt - putting them there is the
obvious next step and a much larger one.

**As built.** Two details the rules above do not fix.

`mut` is already a keyword - `let mut x` - so in effect position it
arrives as one rather than as an identifier and is accepted specially.
`io` and `nothing` stay ordinary identifiers, so a program with a
variable called `io` keeps compiling.

A function declared without a body - what an interface file is made of -
contributes nothing, because there is no body to infer from. An
unannotated library function is therefore assumed pure, which is the
same bargain as rule 4: no clause is no claim. A `uses` clause does
survive into an interface, so a library that annotates is believed.
