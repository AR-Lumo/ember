/// Nested module paths: `shapes::geometry` is the file
/// `shapes/geometry.em`, and a path means the same thing wherever it
/// is written - it is relative to the root the module was found
/// under, not to the file doing the importing.
///
/// The two `math` modules here are the point. `shapes::detail::math`
/// and the top-level `math` share a leaf name and coexist, because
/// what identifies a module is its whole path.

import shapes::geometry;
import math;

pub fn main() {
    let a = shapes::geometry::Point { x: 0, y: 0 };
    let b = shapes::geometry::Point { x: 3, y: 4 };
    print("distance_sq: ");
    println(shapes::geometry::distance_sq(a, b));
    print("top-level math::square: ");
    println(math::square(5));
}
