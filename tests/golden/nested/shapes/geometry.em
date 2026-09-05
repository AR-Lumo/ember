import shapes::detail::math;

pub struct Point {
    pub x: int,
    pub y: int,
}

pub fn distance_sq(a: Point, b: Point) -> int {
    return shapes::detail::math::square(a.x - b.x) + shapes::detail::math::square(a.y - b.y);
}
