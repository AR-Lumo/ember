// A program that parses cleanly but is wrong in many different ways at
// once. Its `.check` snapshot is the record of exactly what the type
// checker says, in the §7 format, for every error class in Phase 3.

struct Point {
    pub x: int,
    pub y: int,
}

impl Point {
    pub fn distance_sq(&self, other: Point) -> int {
        let dx = self.x - other.x;
        let dy = self.y - other.y;
        return dx * dx + dy * dy;
    }
}

/// Duplicate definition: `Point` is already a type.
struct Point {
    pub z: int,
}

/// Missing return: declared `int`, but control can reach the end.
pub fn no_return(flag: bool) -> int {
    if flag {
        return 1;
    }
}

/// Wrong argument count, and an argument of the wrong type.
pub fn arity() {
    let p = Point { x: 1, y: 2 };
    let a = p.distance_sq();
    let b = p.distance_sq(1);
}

/// Unknown method, unknown field, and a field used as a method.
pub fn unknown_members() {
    let p = Point { x: 1, y: 2 };
    println(p.area());
    println(p.z);
    println(p.x());
}

/// No implicit numeric conversion between `int` and `float`.
pub fn no_coercion() {
    let mixed = 1 + 2.5;
    let annotated: float = 3;
}

/// Undefined identifiers, one of them a near-miss on a real name.
pub fn undefined_names() {
    let count = 0;
    println(cout);
    println(entirely_unknown);
    missing_function();
}

/// Type mismatches, including the §7 worked example.
pub fn mismatches() {
    let x: int = "hello";
    if 1 {
        println(0);
    }
}

/// Assigning to things that cannot be assigned.
pub fn mutability() {
    let fixed = 1;
    fixed = 2;
}

/// Struct literals that do not match the declaration.
pub fn bad_literals() {
    let missing = Point { x: 1 };
    let extra = Point { x: 1, y: 2, w: 3 };
}

pub fn main() {
    arity();
    unknown_members();
    no_coercion();
    undefined_names();
    mismatches();
    mutability();
    bad_literals();
}
