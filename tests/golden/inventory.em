/// A small struct-heavy program: an inventory of parts.
///
/// Shows structs, `impl` blocks, methods calling other methods on
/// `self`, module-level constants, and an array of structs.

/// One line in the inventory.
struct Item {
    pub name: string,
    pub quantity: int,
    /// Price per unit, in whole cents, so the arithmetic stays exact.
    pub unit_price: int,
}

impl Item {
    /// What this line is worth in total, in cents.
    pub fn total_value(&self) -> int {
        return self.quantity * self.unit_price;
    }

    /// Whether this line has fallen below the reorder point.
    pub fn is_low(&self, threshold: int) -> bool {
        return self.quantity < threshold;
    }

    /// Prints one line of the report. Calling `total_value` from here is
    /// what makes this a method chain rather than a bag of functions.
    pub fn describe(&self) {
        print(self.name);
        print(": ");
        print(self.quantity);
        print(" @ ");
        print(self.unit_price);
        print("c = ");
        print(self.total_value());
        println("c");
    }
}

/// Anything at or below this quantity gets flagged for reorder.
const LOW_STOCK_THRESHOLD: int = 5;

pub fn main() {
    let stock: [Item; 3] = [
        Item { name: "bolts", quantity: 120, unit_price: 3 },
        Item { name: "widgets", quantity: 2, unit_price: 250 },
        Item { name: "gaskets", quantity: 4, unit_price: 45 },
    ];

    let mut total = 0;
    let mut low_lines = 0;
    let mut i = 0;

    while i < len(stock) {
        stock[i].describe();
        total = total + stock[i].total_value();

        if stock[i].is_low(LOW_STOCK_THRESHOLD) {
            print("  reorder: ");
            println(stock[i].name);
            low_lines = low_lines + 1;
        }

        i = i + 1;
    }

    print("total value: ");
    print(total);
    println("c");

    print("lines needing reorder: ");
    println(low_lines);
}
