/// Closures: functions as values.
///
/// A closure is written `|params| -> Result { ... }` and has the type
/// `fn(Params) -> Result`. Parameter and return types are written out,
/// as they are everywhere else in Ember.
///
/// Captures are **by value**: a closure copies what it mentions from the
/// scope around it into its own storage. That is the only rule that fits
/// the memory model — capturing by reference would hand out a pointer
/// with nothing to guarantee it outlives the closure.
///
/// A closure is itself an owned value, like a `Vec`, because a capturing
/// one holds a heap block. Calling it is a *use*, not a move, so it can
/// be called as often as you like; passing it by value moves it, and
/// `&fn(...)` borrows it.
///
/// A capture may own memory of its own. Taking one by value means taking
/// it: the closure owns it from then on, and the scope that had it does
/// not. A closure holding anything owned carries its own drop function,
/// because what is inside an environment cannot be worked out from the
/// closure's type — two closures of the same `fn() -> int` may have
/// captured quite different things.

/// Takes a closure by reference, so the caller keeps it.
pub fn apply(f: &fn(int) -> int, value: int) -> int {
    return f(value);
}

/// Applies a closure to every element of a vector, in place.
pub fn map_in_place(values: &Vec<int>, f: &fn(int) -> int) {
    let mut i = 0;
    while i < len(values) {
        values[i] = f(values[i]);
        i = i + 1;
    }
}

/// Counts the elements a predicate accepts.
pub fn count_where(values: &Vec<int>, keep: &fn(int) -> bool) -> int {
    let mut found = 0;
    let mut i = 0;
    while i < len(values) {
        if keep(values[i]) {
            found = found + 1;
        }
        i = i + 1;
    }
    return found;
}

/// Returns a closure. The captured `factor` is copied into it, so the
/// closure stays valid after this function returns.
pub fn scaler(factor: int) -> fn(int) -> int {
    return |x: int| -> int { return x * factor; };
}

pub fn main() {
    // A closure with no captures.
    let double = |x: int| -> int { return x * 2; };
    print("double(21): ");
    println(double(21));

    // Calling does not consume it, so it works as often as you like.
    print("again:      ");
    println(double(50));

    // Capturing a local by value.
    let offset = 1000;
    let shift = |x: int| -> int { return x + offset; };
    print("shift(5):   ");
    println(shift(5));

    // Passed to a higher-order function.
    print("apply:      ");
    println(apply(double, 7));

    // Built and returned by another function.
    let triple = scaler(3);
    print("triple(14): ");
    println(triple(14));

    // Driving a vector with closures.
    let mut values: Vec<int> = new_vec();
    let mut i = 1;
    while i <= 6 {
        push(values, i);
        i = i + 1;
    }

    map_in_place(values, double);
    print("doubled:   ");
    let mut j = 0;
    while j < len(values) {
        print(" ");
        print(values[j]);
        j = j + 1;
    }
    println("");

    let big = |x: int| -> bool { return x > 6; };
    print("over six:   ");
    println(count_where(values, big));

    // No parameters, and one that returns nothing.
    let answer = || -> int { return 42; };
    print("answer:     ");
    println(answer());

    let announce = |text: string| { println(text); };
    announce("closures are values");

    // An owned capture. `greeting` moves into the closure, which frees
    // it when the closure itself is dropped. Naming `greeting` again
    // after this line would be a compile error, not a use-after-free.
    let mut greeting: String = new_string();
    push_str(greeting, "owned capture");

    let speak = |name: string| {
        print(greeting);
        print(": ");
        println(name);
    };
    speak("held by the closure");
    speak("and still held");
}
