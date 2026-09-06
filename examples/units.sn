/// Units of measure (§10.2).
///
/// A number can carry a unit, and the checker refuses to add metres to
/// seconds. Units are compile-time only: a `float<meters>` is an
/// ordinary `double` in the generated code, so none of this costs
/// anything at run time.
///
/// `+` and `-` need the same unit on both sides. `*` and `/` combine
/// units algebraically, so dividing a distance by a time gives a speed
/// without anyone having to declare one.

unit meters;
unit seconds;
unit kilograms;

/// The derived unit is written out here, but it would be inferred.
pub fn speed(distance: float<meters>, elapsed: float<seconds>) -> float<meters/seconds> {
    return distance / elapsed;
}

/// Dividing by a time twice gives `meters/seconds^2`, and `^` is how a
/// repeated unit is written.
pub fn acceleration(distance: float<meters>, elapsed: float<seconds>)
        -> float<meters/seconds^2> {
    return distance / elapsed / elapsed;
}

/// Force is mass times acceleration, and its unit falls out of the
/// multiplication rather than being declared.
pub fn force(mass: float<kilograms>, rate: float<meters/seconds^2>)
        -> float<kilograms*meters/seconds^2> {
    return mass * rate;
}

pub fn main() {
    let distance: float<meters> = 100.0<meters>;
    let elapsed: float<seconds> = 4.0<seconds>;

    println(speed(distance, elapsed));
    println(acceleration(distance, elapsed));
    println(force(2.0<kilograms>, acceleration(distance, elapsed)));

    // Scaling by a plain number keeps the unit.
    let twice: float<meters> = distance * 2.0;
    println(twice);

    // A unit that cancels is gone: this is a plain `float` again, and
    // annotating it as one is how you can tell.
    let ratio: float = distance / twice;
    println(ratio);

    // Multiplying a speed back by a time returns a distance.
    let travelled: float<meters> = speed(distance, elapsed) * elapsed;
    println(travelled);

    // `int` carries units too.
    let steps: int<meters> = 7<meters>;
    println(steps);

    // Comparison needs matching units, and gives a plain `bool`.
    if distance > 50.0<meters> {
        println("further than fifty metres");
    }

    // A unit binds to a literal only when the `<` touches it, so an
    // ordinary comparison written without spaces still means what it
    // always did.
    let limit = 9;
    if 5<limit {
        println("five is less than nine");
    }
}
