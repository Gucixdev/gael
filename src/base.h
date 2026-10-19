/* base.h - fixed-size types (the names of holyc's base/src/core/base.h), a C89 static assert, and flat arenas.
 *
 * No malloc anywhere: every arena is one static block with a bump pointer, emptied all at once. Running out is a
 * fatal error that names the arena, so a limit shows up as a clear message instead of corrupted memory.
 * The integer types come from the compiler's __INT*_TYPE__ macros (gcc and clang define them in -std=c89 too). */

#ifndef GAEL_BASE_H_
#define GAEL_BASE_H_

#define _POSIX_C_SOURCE 200809L /* popen and pclose for sh; the language stays C89 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void o;
typedef __INT8_TYPE__ i8;
typedef __INT16_TYPE__ i16;
typedef __INT32_TYPE__ i32;
typedef __INT64_TYPE__ i64;
typedef __UINT8_TYPE__ u8;
typedef __UINT16_TYPE__ u16;
typedef __UINT32_TYPE__ u32;
typedef __UINT64_TYPE__ u64;
typedef float f32;
typedef double f64;

#define GAEL_CAT2_(a, b) a##b
#define GAEL_CAT_(a, b) GAEL_CAT2_(a, b)
#define STATIC_ASSERT(cond, msg) typedef char GAEL_CAT_(static_assert_##msg##_, __LINE__)[(cond) ? 1 : -1]

STATIC_ASSERT(sizeof(i32) == 4, i32_is_4_bytes);
STATIC_ASSERT(sizeof(i64) == 8, i64_is_8_bytes);
STATIC_ASSERT(sizeof(f64) == 8, f64_is_8_bytes);
STATIC_ASSERT(sizeof(long) == 8, long_holds_i64_for_printf); /* LP64: i64 prints with %ld */

#define GAEL_MIN(a, b) ((a) < (b) ? (a) : (b))
#define GAEL_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

typedef struct {
	u8 *base;
	size_t cap;
	size_t used;
	const char *name;
} Arena;

static o die(const char *what)
{
	fprintf(stderr, "gael: %s\n", what);
	exit(70);
}

static o *arena_alloc(Arena *a, size_t n)
{
	o *p;
	n = (n + 7) & ~(size_t)7;
	if (n > a->cap - a->used) {
		fprintf(stderr, "gael: out of memory in the %s arena (%lu bytes)\n", a->name, (unsigned long)a->cap);
		exit(70);
	}
	p = a->base + a->used;
	a->used += n;
	return p;
}

static char *arena_strndup(Arena *a, const char *s, size_t n)
{
	char *p = (char *)arena_alloc(a, n + 1);
	memcpy(p, s, n);
	p[n] = 0;
	return p;
}

#endif
