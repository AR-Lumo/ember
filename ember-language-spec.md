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
               | import_decl ;

import_decl    = "import" identifier ";" ;                 (* v2 *)

visibility     = [ "pub" ] ;

const_decl     = visibility "const" identifier ":" type "=" expression ";" ;

function_decl  = visibility "fn" identifier [ generic_params ]
                 "(" [ param_list ] ")" [ "->" type ] block ;

generic_params = "<" identifier { "," identifier } ">" ;   (* v2 *)
param_list     = param { "," param } ;
param          = ( "self" | "&self" ) | identifier ":" type ;

struct_decl    = visibility "struct" identifier [ generic_params ] "{" { field } "}" ;
field          = visibility identifier ":" type "," ;

impl_block     = "impl" identifier "{" { function_decl } "}" ;

type           = "int" | "float" | "bool" | "string"
               | "fn" "(" [ type { "," type } ] ")" [ "->" type ]
                                           (* function value, v2 *)
               | "Vec" "<" type ">"        (* growable array, v2 *)
               | "String"                  (* growable string, v2 *)
               | [ identifier "::" ] identifier [ type_args ]
                                           (* struct type, optionally
                                              from another module *)
               | "&" type                  (* reference *)
               | "[" type ";" int_lit "]"  (* fixed-size array *) ;

type_args      = "<" type { "," type } ">" ;               (* v2 *)

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
closure        = "|" [ param { "," param } ] "|" [ "->" type ] block ;   (* v2 *)
array_literal  = "[" [ expression { "," expression } ] "]" ;

call_expr      = [ identifier "::" ] identifier "(" [ arg_list ] ")" ;
method_call    = expression "." identifier "(" [ arg_list ] ")" ;
arg_list       = expression { "," expression } ;
field_access   = expression "." identifier ;
index_expr     = expression "[" expression "]" ;
struct_literal = [ identifier "::" ] identifier "{" [ field_init_list ] "}" ;
field_init_list = identifier ":" expression { "," identifier ":" expression } ;

literal        = int_lit | float_lit | bool_lit | string_lit ;
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
- `len(x)` for arrays, `Vec`s and `String`s
- `new_vec()`, `push(v, x)`, `pop(v)` for `Vec<T>` (v2)
- `new_string()`, `push_str(s, text)` for `String` (v2)

Everything else (file I/O, collections, string manipulation) is v2+.

---

## 6. CLI Design

```
ember build file.em -o output      # compile to native executable
ember run file.em                  # compile + run in one step
ember check file.em                # type-check only, no codegen

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
- Cargo workspace with empty crates per the layout above.
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
- Package manager. Distributing *source*, the way Cargo does, needs
  nothing beyond separate compilation. Distributing compiled libraries
  additionally needs an interface file recording a module's types and
  signatures, so a dependent can be built without the dependency's
  source.

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
