#include "soliton/std/std.hpp"

#include <charconv>
#include <system_error>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {

const char* soliton_runtime_version(void) { return "0.1.0"; }

// --- print ----------------------------------------------------------

void soliton_print_int(int64_t value) { printf("%" PRId64, value); }

/// Floats print as the shortest string that reads back as the same
/// double, preferring plain notation over exponential.
///
/// std::to_chars does exactly that and is locale-independent, which
/// matters: it picks between fixed and scientific by whichever is
/// shorter, breaking ties toward fixed. Searching `%g` precisions
/// instead gets this wrong — the shortest round-tripping `%g` for 10.0
/// is `%.1g`, which prints `1e+01`.
void soliton_print_float(double value) {
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

void soliton_print_bool(int8_t value) { fputs(value != 0 ? "true" : "false", stdout); }

void soliton_print_string(const char* bytes, int64_t length) {
    if (bytes != nullptr && length > 0) {
        fwrite(bytes, 1, static_cast<size_t>(length), stdout);
    }
}

// --- println --------------------------------------------------------

void soliton_println_int(int64_t value) {
    soliton_print_int(value);
    fputc('\n', stdout);
}

void soliton_println_float(double value) {
    soliton_print_float(value);
    fputc('\n', stdout);
}

void soliton_println_bool(int8_t value) {
    soliton_print_bool(value);
    fputc('\n', stdout);
}

void soliton_println_string(const char* bytes, int64_t length) {
    soliton_print_string(bytes, length);
    fputc('\n', stdout);
}

// --- support --------------------------------------------------------

int64_t soliton_string_cmp(const char* left, int64_t left_length, const char* right,
                         int64_t right_length) {
    const int64_t shared = left_length < right_length ? left_length : right_length;
    if (shared > 0) {
        const int order = memcmp(left, right, static_cast<size_t>(shared));
        if (order != 0) {
            return order < 0 ? -1 : 1;
        }
    }
    // One is a prefix of the other, so the shorter comes first.
    if (left_length == right_length) {
        return 0;
    }
    return left_length < right_length ? -1 : 1;
}

int64_t soliton_string_find(const char* haystack, int64_t haystack_length, const char* needle,
                          int64_t needle_length) {
    if (needle_length == 0) {
        return 0;  // the empty string is at the start of everything
    }
    if (needle_length > haystack_length) {
        return -1;
    }
    // Naive search. A million-character haystack would want something
    // better; nothing in Soliton has one yet, and this is the version
    // whose correctness is obvious.
    for (int64_t at = 0; at + needle_length <= haystack_length; ++at) {
        if (memcmp(haystack + at, needle, static_cast<size_t>(needle_length)) == 0) {
            return at;
        }
    }
    return -1;
}

int8_t soliton_string_eq(const char* left, int64_t left_length, const char* right,
                       int64_t right_length) {
    if (left_length != right_length) {
        return 0;
    }
    if (left_length == 0) {
        return 1;
    }
    return memcmp(left, right, static_cast<size_t>(left_length)) == 0 ? 1 : 0;
}

void soliton_panic_bad_slice(int64_t start, int64_t end, int64_t length) {
    fflush(stdout);
    fprintf(stderr,
            "soliton: slice out of bounds: the length is %" PRId64 " but the range is %" PRId64
            "..%" PRId64 "\n",
            length, start, end);
    exit(101);
}

void soliton_panic_index_out_of_bounds(int64_t index, int64_t length) {
    fflush(stdout);
    fprintf(stderr, "soliton: index out of bounds: the length is %" PRId64 " but the index is %" PRId64 "\n",
            length, index);
    exit(101);
}

/// A `requires` or `ensures` that did not hold (10.1).
///
/// `kind` is the keyword, `condition` the clause as written, `location`
/// where it was written and `function` the function it guards. All four
/// are string constants baked in by codegen - nothing is formatted at
/// run time, so a violated contract costs nothing until it fires.
///
/// This names the contract, not the call site. Reporting the caller
/// would mean passing its position into every call to a contracted
/// function, which changes the ABI of those functions and would have to
/// survive separate compilation and interface files.
void soliton_panic_contract(const char* kind, const char* condition, const char* location,
                          const char* function) {
    fflush(stdout);
    fprintf(stderr, "soliton: %s contract violated in `%s`: %s\n", kind, function,
            condition);
    fprintf(stderr, "  --> %s\n", location);
    exit(101);
}

void soliton_panic_divide_by_zero(void) {
    fflush(stdout);
    fputs("soliton: attempt to divide by zero\n", stderr);
    exit(101);
}


// --- heap -------------------------------------------------------------

void* soliton_alloc(int64_t bytes) {
    if (bytes <= 0) {
        // A zero-length buffer needs no allocation; null is a valid
        // empty buffer everywhere below, and free(null) is a no-op.
        return nullptr;
    }
    void* buffer = malloc(static_cast<size_t>(bytes));
    if (buffer == nullptr) {
        fflush(stdout);
        fprintf(stderr, "soliton: out of memory allocating %" PRId64 " bytes\n", bytes);
        exit(101);
    }
    return buffer;
}

void* soliton_realloc(void* buffer, int64_t bytes) {
    if (bytes <= 0) {
        free(buffer);
        return nullptr;
    }
    void* grown = realloc(buffer, static_cast<size_t>(bytes));
    if (grown == nullptr) {
        fflush(stdout);
        fprintf(stderr, "soliton: out of memory growing to %" PRId64 " bytes\n", bytes);
        exit(101);
    }
    return grown;
}

void soliton_free(void* buffer) { free(buffer); }

void* soliton_reserve(void* buffer, int64_t element_size, int64_t* capacity, int64_t wanted) {
    if (wanted <= *capacity) {
        return buffer;  // never shrinks: what is already there is paid for
    }
    void* grown = soliton_realloc(buffer, wanted * element_size);
    *capacity = wanted;
    return grown;
}

void* soliton_grow(void* buffer, int64_t element_size, int64_t length, int64_t* capacity) {
    if (length < *capacity) {
        return buffer;
    }
    // Double, starting at four. Growing by a constant instead would make
    // a run of pushes quadratic.
    int64_t next = (*capacity == 0) ? 4 : *capacity * 2;
    void* grown = soliton_realloc(buffer, next * element_size);
    *capacity = next;
    return grown;
}

void soliton_panic_empty(const char* what, int64_t what_length) {
    fflush(stdout);
    fputs("soliton: cannot pop from an empty ", stderr);
    if (what != nullptr && what_length > 0) {
        fwrite(what, 1, static_cast<size_t>(what_length), stderr);
    }
    fputc('\n', stderr);
    exit(101);
}

void* soliton_string_append(void* buffer, int64_t* length, int64_t* capacity, const char* bytes,
                          int64_t count) {
    if (count <= 0) {
        return buffer;
    }
    int64_t needed = *length + count;
    if (needed > *capacity) {
        int64_t next = (*capacity == 0) ? 16 : *capacity;
        while (next < needed) {
            next *= 2;
        }
        buffer = soliton_realloc(buffer, next);
        *capacity = next;
    }
    memcpy(static_cast<char*>(buffer) + *length, bytes, static_cast<size_t>(count));
    *length = needed;
    return buffer;
}

}  // extern "C"
