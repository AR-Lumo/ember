/// Effect annotations (§10.3).
///
/// A `uses` clause is an upper bound on what a function may do. The
/// checker infers what each function actually does — `println` performs
/// `io`, a call performs whatever the callee performs — and reports any
/// effect the clause does not permit.
///
/// A function with no clause has no bound. That is what lets effects be
/// added to an existing program one function at a time: nothing is
/// checked until somebody asks for it. `uses nothing` is how to ask for
/// the strongest answer, that a function is pure.

/// Pure, and says so. The compiler holds it to that.
pub fn area(width: int, height: int) -> int uses nothing {
    return width * height;
}

/// Pure three levels down: `area` is called, and it promises nothing
/// either, so the promise here still holds.
pub fn volume(width: int, height: int, depth: int) -> int uses nothing {
    return area(width, height) * depth;
}

/// Allowed to print, and does.
pub fn report(width: int, height: int) uses io {
    print("area: ");
    println(area(width, height));
}

/// A bound is a ceiling, not a quota: this is permitted to print and
/// never does.
pub fn silent(width: int) -> int uses io {
    return width;
}

/// Effects are inferred through a method, and a method may carry a
/// clause of its own.
struct Room {
    pub width: int,
    pub height: int,
}

impl Room {
    pub fn area(&self) -> int uses nothing {
        return self.width * self.height;
    }

    pub fn describe(&self) uses io {
        print("room: ");
        println(self.area());
    }
}

/// A generic function can be bounded too.
pub fn identity<T>(value: T) -> T uses nothing {
    return value;
}

/// No clause, so no bound — this prints freely, and every program
/// written before effects existed still compiles for this reason.
pub fn main() {
    println(area(3, 4));
    println(volume(2, 3, 4));
    report(5, 6);
    println(silent(7));

    let room = Room { width: 3, height: 5 };
    room.describe();

    println(identity(42));
}
