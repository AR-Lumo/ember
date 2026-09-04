/// A second module, to show that a program may import more than one and
/// that each keeps its own namespace.

pub struct Counter {
    pub value: int,
}

pub fn start() -> Counter {
    return Counter { value: 0 };
}

pub fn bumped(c: Counter, by: int) -> Counter {
    return Counter { value: c.value + by };
}
