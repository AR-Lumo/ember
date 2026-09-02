/// Exercises every token kind in the v1 grammar.
// A regular comment, skipped by the lexer along with the doc comment above.

pub const MAX_RETRIES: int = 3;
const LARGE: int = 1_000_000;
const GOLDEN_RATIO: float = 1.618;
const AVOGADRO: float = 6.02e23;
const GREETING: string = "hi\tthere\n";
const ENABLED: bool = true;
const DISABLED: bool = false;

struct Config {
    pub retries: int,
    pub ratio: float,
    pub verbose: bool,
    pub label: string,
}

impl Config {
    /// True when another attempt should be made.
    pub fn should_retry(&self, attempt: int) -> bool {
        return attempt < self.retries && !self.verbose || attempt != 0;
    }
}

/// Every arithmetic operator, plus unary minus and the comparisons.
pub fn arithmetic(a: int, b: int) -> int {
    let mut total = a + b - a * b / 2 % 3;
    total = -total;
    while total > 0 {
        if total >= 100 && total <= 200 {
            total = total - 1;
        } else {
            if a == b {
                total = 0;
            } else {
                total = total - 2;
            }
        }
    }
    return total;
}

/// Indexing and fixed-size array types.
pub fn first(items: [int; 3]) -> int {
    return items[0];
}

pub fn main() {
    let config = Config {
        retries: MAX_RETRIES,
        ratio: GOLDEN_RATIO,
        verbose: ENABLED,
        label: GREETING,
    };
    println(config.should_retry(1));
    println(arithmetic(10, 4));
}
