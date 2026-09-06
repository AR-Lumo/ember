/// Fibonacci numbers, computed two ways.
///
/// Shows `while` loops, recursion, and the difference an algorithm makes:
/// both functions agree, but only one of them finishes quickly.

/// The nth Fibonacci number, computed with a loop.
pub fn fib_iterative(n: int) -> int {
    let mut previous = 0;
    let mut current = 1;
    let mut i = 0;
    while i < n {
        let next = previous + current;
        previous = current;
        current = next;
        i = i + 1;
    }
    return previous;
}

/// The same sequence, written the way the mathematics defines it.
pub fn fib_recursive(n: int) -> int {
    if n < 2 {
        return n;
    }
    return fib_recursive(n - 1) + fib_recursive(n - 2);
}

pub fn main() {
    print("first ten:");
    let mut i = 0;
    while i < 10 {
        print(" ");
        print(fib_iterative(i));
        i = i + 1;
    }
    println("");

    print("fib_iterative(30) = ");
    println(fib_iterative(30));

    print("fib_recursive(20) = ");
    println(fib_recursive(20));
}
