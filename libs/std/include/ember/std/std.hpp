// ember::std - the Ember runtime, linked into every compiled binary.
//
// Backs the intrinsics in §5. Everything here has C linkage: generated
// LLVM IR calls these by plain symbol name, with no C++ mangling in the
// way, and the names are what codegen emits directly.
//
// An Ember `string` is a fat pointer - bytes plus length - rather than a
// NUL-terminated char*. §4 calls strings "immutable, fixed-length view",
// and carrying the length means a string can hold a NUL and that length
// is O(1). It is passed as two arguments because that is what a struct of
// {ptr, i64} lowers to anyway.

#ifndef EMBER_STD_STD_HPP
#define EMBER_STD_STD_HPP

#include <stdint.h>

extern "C" {

/// Version marker for the runtime, so the driver can report which
/// runtime a binary was linked against.
const char* ember_runtime_version(void);

// --- print: no trailing newline -------------------------------------

void ember_print_int(int64_t value);
void ember_print_float(double value);
void ember_print_bool(int8_t value);
void ember_print_string(const char* bytes, int64_t length);

// --- println: same, with a trailing newline -------------------------

void ember_println_int(int64_t value);
void ember_println_float(double value);
void ember_println_bool(int8_t value);
void ember_println_string(const char* bytes, int64_t length);

/// Byte-wise equality for `==` and `!=` on strings.
int8_t ember_string_eq(const char* left, int64_t left_length, const char* right,
                       int64_t right_length);

/// Reports an out-of-bounds array index and terminates. Codegen emits a
/// call to this on the failing branch of a bounds check.
void ember_panic_index_out_of_bounds(int64_t index, int64_t length);

/// Reports a division or remainder by zero and terminates.
void ember_panic_divide_by_zero(void);

}  // extern "C"

#endif  // EMBER_STD_STD_HPP
