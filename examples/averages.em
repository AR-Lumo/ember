/// Explicit numeric conversion with `as`.
///
/// `int` and `float` never mix implicitly (§4), so anything that needs
/// both — an average, a percentage, a ratio — has to say where the
/// conversion happens. That is the whole point: the rounding is visible
/// in the source rather than inferred behind your back.

/// The mean of an array of counts, as a float.
pub fn mean(values: &[int; 5]) -> float {
    let mut total = 0;
    let mut i = 0;
    while i < len(values) {
        total = total + values[i];
        i = i + 1;
    }
    // Both operands must be `float`, so both conversions are written.
    return total as float / len(values) as float;
}

/// What fraction of `total` is `part`, as a percentage.
pub fn percent(part: int, total: int) -> float {
    return part as float * 100.0 / total as float;
}

pub fn main() {
    let counts: [int; 5] = [3, 7, 8, 12, 20];

    print("mean: ");
    println(mean(counts));

    print("12 is this percent of 50: ");
    println(percent(12, 50));

    // Going the other way truncates toward zero, as in C.
    print("3.9 as int: ");
    println(3.9 as int);
    print("-3.9 as int: ");
    println(-3.9 as int);

    // `as` binds tighter than arithmetic, so this is (1 as float) + 0.5
    // rather than 1 as (float + 0.5).
    print("1 as float + 0.5 = ");
    println(1 as float + 0.5);
}
