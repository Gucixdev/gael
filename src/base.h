/* base.h - fixed-size types (the names of holyc's base/src/core_base.h), a C89 static assert, and flat arenas.
 *
 * No malloc anywhere: an arena is one block with a bump pointer, emptied all at once - a static array, or for the
 * VM's heap and text one reservation of address space (mmap), whose pages the system gives only where something is
 * written. Running out is an error that names the arena, so a limit shows up as a clear message, not corrupt memory.
 * The integer types come from the compiler's __INT*_TYPE__ macros (gcc and clang define them in -std=c89 too). */

#ifndef GAEL_BASE_H_
#define GAEL_BASE_H_

#define _XOPEN_SOURCE 700 /* popen, pclose and realpath; the language stays C89 */
#define _DEFAULT_SOURCE   /* MAP_ANONYMOUS and MAP_NORESERVE */
#include <math.h> /* HUGE_VAL */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef GAEL_NO_MMAP
#include <sys/mman.h>
#endif
#include <unistd.h> /* getpid, unlink */

/* GAEL_SHRINK=N: every table of the compiler is 2^N times smaller (a build for a small memory: with GAEL_NO_MMAP, a
 * pool of 64 MiB takes -DGAEL_SHRINK=4). A file that needs more says which limit it hit. 0 is the full size. */
#ifndef GAEL_SHRINK
#define GAEL_SHRINK 0
#endif

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

#ifdef GAEL_NO_MMAP
/* -DGAEL_NO_MMAP: no system call for memory at all. Every reservation is cut from one static array (zeroed bss, so
 * the loader gives pages as they are written, and WASM puts it in its linear memory); -DGAEL_POOL_BYTES=N sizes it.
 * The compiler's tables and the VM's heap and text arenas must fit together in it, so a build like this sets the
 * limits it needs (a small default here: 64 MiB). Never freed: reservations live as long as the program. */
#ifndef GAEL_POOL_BYTES
#define GAEL_POOL_BYTES ((size_t)64 << 20)
#endif
static u8 g_pool[GAEL_POOL_BYTES] __attribute__((aligned(4096)));
static size_t g_pool_used;

static o *reserve(size_t bytes)
{
	size_t at = (g_pool_used + 4095) & ~(size_t)4095;
	if (at > GAEL_POOL_BYTES || bytes > GAEL_POOL_BYTES - at) return 0;
	g_pool_used = at + bytes;
	return g_pool + at;
}

/* release P BYTES: give back the last reservation (the pool is a stack); anything else stays where it is */
static o release(o *p, size_t bytes)
{
	if ((u8 *)p + bytes == g_pool + g_pool_used) g_pool_used = (size_t)((u8 *)p - g_pool);
}
#else
/* reserve BYTES: that much address space, given pages only where something is written; 0 when the system refuses */
static o *reserve(size_t bytes)
{
	o *p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	return p == MAP_FAILED ? 0 : p;
}

/* release P BYTES: give the reservation back */
static o release(o *p, size_t bytes)
{
	munmap(p, bytes);
}
#endif

/* table N SIZE: room for N things of SIZE bytes, from reserved address space: pages come only where a thing is
 * written, so a table can have a generous limit and cost nothing until it fills */
static o *table(size_t n, size_t size)
{
	o *p = reserve(n * size);
#ifdef GAEL_NO_MMAP
	if (!p) die("the static pool (GAEL_POOL_BYTES) is too small for the compiler's tables; build with a bigger -DGAEL_POOL_BYTES or a larger -DGAEL_SHRINK");
#else
	if (!p) die("the system gave no address space for the compiler's tables");
#endif
	return p;
}

#endif
