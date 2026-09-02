#include "ember/std/std.hpp"

#include <charconv>
#include <system_error>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {

const char* ember_runtime_version(void) { return "0.1.0"; }

// --- print ----------------------------------------------------------

void ember_print_int(int64_t value) { printf("%" PRId64, value); }

/// Floats print as the shortest string that reads back as the same
/// double, preferring plain notation over exponential.
///
/// std::to_chars does exactly that and is locale-independent, which
/// matters: it picks between fixed and scientific by whichever is
/// shorter, breaking ties toward fixed. Searching `%g` precisions
/// instead gets this wrong — the shortest round-tripping `%g` for 10.0
/// is `%.1g`, which prints `1e+01`.
void ember_print_float(double value) {
    char buffer[64];

    const std::to_chars_result result =
        std::to_chars(buffer, buffer + sizeof(buffer) - 1, value);
    if (result.ec != std::errc{}) {
        // Only reachable if the buffer were too small; fall back rather
        // than print nothing.
        snprintf(buffer, sizeof(buffer), "%.17g", value);
    } else {
        *result.ptr = '\0';
    }

    fputs(buffer, stdout);

    // A float whose shortest form has no decimal point keeps a `.0`, so
    // the output still says which type it was. Skipped for exponential
    // forms and for inf/nan, which are unambiguous already.
    if (strpbrk(buffer, ".eEnN") == nullptr) {
        fputs(".0", stdout);
    }
}

void ember_print_bool(int8_t value) { fputs(value != 0 ? "true" : "false", stdout); }

void ember_print_string(const char* bytes, int64_t length) {
    if (bytes != nullptr && length > 0) {
        fwrite(bytes, 1, static_cast<size_t>(length), stdout);
    }
}

// --- println --------------------------------------------------------

void ember_println_int(int64_t value) {
    ember_print_int(value);
    fputc('\n', stdout);
}

void ember_println_float(double value) {
    ember_print_float(value);
    fputc('\n', stdout);
}

void ember_println_bool(int8_t value) {
    ember_print_bool(value);
    fputc('\n', stdout);
}

void ember_println_string(const char* bytes, int64_t length) {
    ember_print_string(bytes, length);
    fputc('\n', stdout);
}

// --- support --------------------------------------------------------

int8_t ember_string_eq(const char* left, int64_t left_length, const char* right,
                       int64_t right_length) {
    if (left_length != right_length) {
        return 0;
    }
    if (left_length == 0) {
        return 1;
    }
    return memcmp(left, right, static_cast<size_t>(left_length)) == 0 ? 1 : 0;
}

void ember_panic_index_out_of_bounds(int64_t index, int64_t length) {
    fflush(stdout);
    fprintf(stderr, "ember: index out of bounds: the length is %" PRId64 " but the index is %" PRId64 "\n",
            length, index);
    exit(101);
}

void ember_panic_divide_by_zero(void) {
    fflush(stdout);
    fputs("ember: attempt to divide by zero\n", stderr);
    exit(101);
}

}  // extern "C"
