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
/// Lexicographic order: negative, zero or positive, the way `strcmp`
/// answers. Compares bytes, so it orders ASCII correctly and orders
/// anything else by its UTF-8 encoding - which is the same order as by
/// code point, and is not a collation.
int64_t ember_string_cmp(const char* left, int64_t left_length, const char* right,
                         int64_t right_length);

/// Byte offset of the first occurrence of `needle` in `haystack`, or -1.
/// An empty needle is found at 0, as it is everywhere.
int64_t ember_string_find(const char* haystack, int64_t haystack_length, const char* needle,
                          int64_t needle_length);

int8_t ember_string_eq(const char* left, int64_t left_length, const char* right,
                       int64_t right_length);

/// Reports an out-of-bounds array index and terminates. Codegen emits a
/// call to this on the failing branch of a bounds check.
void ember_panic_index_out_of_bounds(int64_t index, int64_t length);

// --- heap -----------------------------------------------------------
//
// A `Vec<T>` and a `String` are both { ptr, len, capacity }: a pointer
// to a heap buffer, how much of it is live, and how much was allocated.
// The compiler knows the element size, so these take it as an argument
// rather than being generic - one runtime function serves every `Vec<T>`.

/// Allocates `bytes`, or terminates if the allocator cannot.
void* ember_alloc(int64_t bytes);

/// Grows `buffer` to `bytes`, preserving its contents.
void* ember_realloc(void* buffer, int64_t bytes);

/// Frees a buffer. Null is allowed and does nothing, so dropping a
/// never-used value is free.
void ember_free(void* buffer);

/// Ensures at least one more element fits, growing geometrically.
/// Returns the (possibly moved) buffer and writes the new capacity.
///
/// Doubling keeps a run of pushes amortized O(1); growing by a constant
/// would make building a vector quadratic.
void* ember_grow(void* buffer, int64_t element_size, int64_t length, int64_t* capacity);

/// Makes room for at least `wanted` elements, and never shrinks. Unlike
/// `ember_grow` this takes exactly what was asked for rather than
/// doubling: the caller has said how much it needs.
void* ember_reserve(void* buffer, int64_t element_size, int64_t* capacity, int64_t wanted);

/// Reports a slice whose bounds are not inside what it is slicing.
void ember_panic_bad_slice(int64_t start, int64_t end, int64_t length);

/// Reports popping from an empty container and terminates.
void ember_panic_empty(const char* what, int64_t what_length);

/// Appends `length` bytes to a string buffer, growing it as needed.
void* ember_string_append(void* buffer, int64_t* length, int64_t* capacity, const char* bytes,
                          int64_t count);

/// Reports a division or remainder by zero and terminates.
void ember_panic_divide_by_zero(void);

}  // extern "C"

#endif  // EMBER_STD_STD_HPP
