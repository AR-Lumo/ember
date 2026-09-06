/// Preconditions and postconditions on the signature (§10.1).
///
/// A `requires` says what a caller owes the function and is checked
/// before the body runs. An `ensures` says what the function owes back
/// and is checked before every return, with `result` naming the value
/// about to be returned.
///
/// Both live in the signature rather than the first few lines of the
/// body, so the obligation is part of what a reader sees without
/// opening the implementation. Both are checked at run time; a
/// violation is a panic naming the clause, not a silently wrong answer.

/// Integer division, which is only meaningful for a non-zero divisor.
pub fn divide(a: int, b: int) -> int
    requires b != 0
    ensures result != 0 || a == 0
{
    return a / b;
}

/// Every `return` is checked, not just the last one.
pub fn clamped(x: int) -> int
    ensures result >= 0
{
    if x < 0 {
        return 0;
    }
    return x;
}

struct Counter {
    pub n: int,
}

impl Counter {
    /// A method may carry contracts too, and `self` is in scope in them.
    pub fn stepped(&self, by: int) -> int
        requires by > 0
        ensures result > self.n
    {
        return self.n + by;
    }
}

/// A function that returns nothing can still promise something about
/// what it was given.
pub fn repeat(word: string, times: int)
    requires times > 0
{
    let mut i = 0;
    while i < times {
        println(word);
        i = i + 1;
    }
}

pub fn main() {
    println(divide(84, 2));
    // The `ensures` allows a zero result when the numerator was zero.
    println(divide(0, 7));

    println(clamped(0 - 4));
    println(clamped(12));

    let c = Counter { n: 10 };
    println(c.stepped(5));

    repeat("twice", 2);

    // `result` is not a keyword - it names the return value only inside
    // an `ensures`, so an ordinary variable may still be called that.
    let result = 99;
    println(result);
}
