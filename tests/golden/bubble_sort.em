/// Bubble sort over a fixed-size array.
///
/// Shows arrays, indexing, and `&T` parameters. The array is sorted in
/// place: passing it as `&[int; 8]` hands the function a pointer to the
/// caller's array rather than a copy of it, so the caller sees the
/// result. A plain `[int; 8]` parameter would be sorted and thrown away.

/// Sorts `values` from smallest to largest, in place.
pub fn bubble_sort(values: &[int; 8]) {
    let mut pass = 0;
    while pass < len(values) {
        let mut i = 0;
        // Each pass settles one more element at the end, so the
        // unsorted region shrinks by one every time round.
        while i + 1 < len(values) - pass {
            if values[i] > values[i + 1] {
                let hold = values[i];
                values[i] = values[i + 1];
                values[i + 1] = hold;
            }
            i = i + 1;
        }
        pass = pass + 1;
    }
}

/// Prints an array on one line, space separated.
pub fn print_all(label: string, values: &[int; 8]) {
    print(label);
    let mut i = 0;
    while i < len(values) {
        print(" ");
        print(values[i]);
        i = i + 1;
    }
    println("");
}

pub fn main() {
    let mut numbers: [int; 8] = [5, 2, 9, 1, 7, 3, 8, 4];

    print_all("before:", numbers);
    bubble_sort(numbers);
    print_all("after: ", numbers);
}
