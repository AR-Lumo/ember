/// Fixed-size arrays: the `[T; N]` type from the grammar, together with
/// the `[a, b, c]` literal this implementation adds so that values of
/// that type can actually be built.
pub fn sum_three(values: [int; 3]) -> int {
    return values[0] + values[1] + values[2];
}

/// Arrays of each primitive type, to pin down how they parse.
pub fn main() {
    let numbers: [int; 3] = [1, 2, 3];
    let ratios: [float; 2] = [0.5, 1.5];
    let flags: [bool; 2] = [true, false];
    let mut total = sum_three(numbers);
    total = total + 1;
    println(total);
    println(ratios[0]);
    println(flags[1]);
}
