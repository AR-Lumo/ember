/// Dynamic arrays, growable strings, and the ownership model that keeps
/// them from leaking.
///
/// A `Vec<T>` and a `String` own a heap buffer. That makes them *owned*
/// types: assigning or passing one moves it rather than copying, using
/// it after it moves is a compile error, and it is freed automatically
/// when its owner goes out of scope. Nothing is reference-counted and
/// nothing is collected — the compiler works out where each buffer dies
/// and frees it there.
///
/// `&T` still borrows without moving and is still unchecked, exactly as
/// it always was, so none of this needs lifetimes or a borrow checker.

/// Borrows the vector: the caller keeps it.
pub fn sum(values: &Vec<int>) -> int {
    let mut total = 0;
    let mut i = 0;
    while i < len(values) {
        total = total + values[i];
        i = i + 1;
    }
    return total;
}

/// Takes ownership: the caller cannot use it afterwards.
pub fn consume(values: Vec<int>) -> int {
    return len(values);
}

/// Builds a vector and hands ownership back out.
pub fn squares(count: int) -> Vec<int> {
    let mut v: Vec<int> = new_vec();
    let mut i = 0;
    while i < count {
        push(v, i * i);
        i = i + 1;
    }
    return v;
}

/// Works for any element type: `Vec<T>` composes with generics.
pub fn first_or<T>(values: &Vec<T>, fallback: T) -> T {
    if len(values) > 0 {
        return values[0];
    }
    return fallback;
}

/// A struct that owns a vector is itself owned, and drops what it holds.
struct Reading {
    pub samples: Vec<int>,
    pub label: string,
}

pub fn describe(r: &Reading) {
    print(r.label);
    print(": ");
    print(len(r.samples));
    print(" samples summing to ");
    println(sum(r.samples));
}

pub fn main() {
    // Grows as needed — no capacity to declare up front.
    let mut v: Vec<int> = new_vec();
    let mut i = 1;
    while i <= 5 {
        push(v, i * 10);
        i = i + 1;
    }

    print("length: ");
    println(len(v));
    print("sum:    ");
    println(sum(v));      // borrowed, so `v` survives
    print("last:   ");
    println(pop(v));
    v[0] = 99;
    print("first:  ");
    println(v[0]);

    // Ownership moves out of `squares` and into `made`.
    let made = squares(6);
    print("squares: ");
    println(sum(made));
    print("first_or: ");
    println(first_or(made, 0));

    // Moving into a struct: `made` belongs to the reading now, and both
    // are freed together at the end of this function.
    let reading = Reading { samples: made, label: "reading" };
    describe(reading);

    // Growable strings, built a piece at a time.
    let mut message: String = new_string();
    push_str(message, "built ");
    push_str(message, "one ");
    push_str(message, "piece ");
    push_str(message, "at a time");
    println(message);
    print("string length: ");
    println(len(message));

    // Consuming `v` ends its life here; using it after this line would
    // be a compile error rather than a use-after-free.
    print("consumed: ");
    println(consume(v));
}
