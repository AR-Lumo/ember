/// A generic container with methods: `impl<T> Stack<T>`.
///
/// The `impl` block declares the type parameters and applies them to the
/// type, and every method inside is generic over them. Which types they
/// stand for is never inferred — the receiver says. A `Stack<int>` makes
/// `T` into `int`, and there is nothing left to work out.
///
/// Each instantiation is a separate struct layout and a separate set of
/// functions, generated where they are first used. No boxing, no vtable,
/// no dispatch: `s.push(4)` on a `Stack<int>` compiles to a direct call
/// to a function that only ever handles `int`.

struct Stack<T> {
    pub items: Vec<T>,
}

impl<T> Stack<T> {
    /// Takes the stack by value and gives back a taller one. `self`
    /// rather than `&self`, because pushing into the vector needs to own
    /// it — a field cannot be moved out of a struct that survives.
    pub fn with(self, value: T) -> Stack<T> {
        let mut grown = self;
        push(grown.items, value);
        return grown;
    }

    pub fn height(&self) -> int {
        return len(self.items);
    }

    pub fn is_empty(&self) -> bool {
        return len(self.items) == 0;
    }

    /// A method with a type parameter of its own, on top of the block's.
    /// This one is inferred from the argument, the way a free function's
    /// parameters are.
    pub fn described_by<L>(&self, label: L) -> L {
        return label;
    }
}

pub fn new_stack<T>() -> Stack<T> {
    let items: Vec<T> = new_vec();
    return Stack { items: items };
}

pub fn main() {
    // The same code, at two types.
    let mut numbers: Stack<int> = new_stack();
    numbers = numbers.with(1);
    numbers = numbers.with(2);
    numbers = numbers.with(3);

    print("numbers:   ");
    println(numbers.height());
    print("empty:     ");
    println(numbers.is_empty());

    let mut names: Stack<String> = new_stack();
    let mut first: String = new_string();
    push_str(first, "ada");
    names = names.with(first);

    print("names:     ");
    println(names.height());
    print("label:     ");
    println(names.described_by("a stack of strings"));

    // Popping gives back the element type, whatever it was.
    print("popped:    ");
    println(pop(numbers.items));
    print("remaining: ");
    println(numbers.height());
}
