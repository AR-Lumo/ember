/// Points and the arithmetic on them.
///
/// Only `pub` items can be reached from another module; `normalize_axis`
/// below is an implementation detail and stays private.

/// A point in 2D space.
pub struct Point {
    pub x: int,
    pub y: int,
}

/// The origin, shared by anything that imports this module.
pub const ORIGIN: Point = Point { x: 0, y: 0 };

/// Squared distance between two points.
pub fn distance_sq(a: Point, b: Point) -> int {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    return dx * dx + dy * dy;
}

/// Squared distance from the origin.
pub fn magnitude_sq(p: Point) -> int {
    return distance_sq(p, ORIGIN);
}

/// Private: reachable from `geometry` itself and nowhere else.
fn normalize_axis(value: int) -> int {
    if value < 0 {
        return 0 - value;
    }
    return value;
}

/// Uses the private helper, which is fine from inside this module.
pub fn manhattan(p: Point) -> int {
    return normalize_axis(p.x) + normalize_axis(p.y);
}
