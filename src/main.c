/* gael - a small statically typed language with s-expressions, compiled to bytecode and run on a VM.
 *
 *   gael                   the REPL
 *   gael FILE ARGS...      compile FILE; when there are no errors, run it; (args) gives it ARGS
 *   gael -c FILE           only compile: report every type error, run nothing
 *   gael -d FILE           compile and print the bytecode
 *   gael -e CODE ARGS...   compile and run CODE
 *   gael -h                this help
 *
 * A file can use others: (use "lib.gael") at its top level, the path relative to the file. Every file is compiled
 * once, the used ones first, and the program runs only when none of them has an error.
 *
 * Exit status: 0 fine, 1 a compile or runtime error, 2 bad usage; (exit n) ends with n.
 * One translation unit (this file includes the others), C89, no malloc and no recursion. */

#include "base.h"
#include "ops.h"
#include "read.c"
#include "compile.c"
#include "vm.c"

static const char g_usage[] =
	"usage: gael                   the REPL\n"
	"       gael FILE ARGS...      compile FILE; when there are no errors, run it; (args) gives it ARGS\n"
	"       gael -c FILE           only compile: report every type error, run nothing\n"
	"       gael -d FILE           compile and print the bytecode\n"
	"       gael -e CODE ARGS...   compile and run CODE\n"
	"       gael -h                this help\n";
static const char g_usage_env[] =
	"GAEL_HEAP=SIZE  the heap for strings, vecs and maps, and as much for the program's own text (default 1G;\n"
	"                K, M, G; the system gives pages only where they are written)\n"
	"GAEL_CHECK=1    freed memory is overwritten, and a program that ran to its end reports what it never freed\n";

enum { RUN, CHECK, DIS };

/* Every file a program uses, once, by its canonical path */
#define MAX_MODULES 1024
typedef struct {
	const char *real;  /* the canonical path: the same file reached two ways is one module */
	const char *shown; /* the path as messages show it */
	u8 state;          /* 0 not loaded, 1 on the load stack, 2 compiled */
} Module;
static Module g_mods[MAX_MODULES];
static i32 g_nmods;

/* a source on the load stack: a file (src 0, read again from disk each time it comes to the top), or the text of
 * a REPL form or of -e */
typedef struct {
	const char *name, *src;
	size_t len;
	i32 mod;
} Source;
#define MAX_USE_DEPTH 256
static Source g_load[MAX_USE_DEPTH];
static i32 g_entries[MAX_MODULES + 1];

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* module_of PATH: the module for a file, made on first sight; -1 when the file is not there */
static i32 module_of(const char *path)
{
	char real[PATH_MAX];
	i32 i;
	if (!realpath(path, real)) return -1;
	for (i = 0; i < g_nmods; i++) {
		if (!strcmp(g_mods[i].real, real)) return i;
	}
	if (g_nmods == MAX_MODULES) die("too many files (MAX_MODULES)");
	g_mods[g_nmods].real = arena_strndup(&g_perm, real, strlen(real));
	g_mods[g_nmods].shown = arena_strndup(&g_perm, path, strlen(path));
	g_mods[g_nmods].state = 0;
	return g_nmods++;
}

/* read_source SOURCE: its text into g_unit, then into nodes; 0 after an error it reported */
static int read_source(const Source *so)
{
	g_unit.file = so->name;
	g_unit.nerrors = 0;
	g_unit_arena.used = 0;
	g_nnodes = 0;
	if (so->src) {
		g_unit.src = so->src;
		g_unit.len = so->len;
	} else {
		FILE *f = fopen(so->name, "rb");
		long size = -1;
		char *buf;
		if (f && !fseek(f, 0, SEEK_END)) size = ftell(f);
		if (!f || size < 0 || fseek(f, 0, SEEK_SET)) {
			if (f) fclose(f);
			fprintf(stderr, "gael: cannot read %s\n", so->name);
			return 0;
		}
		buf = (char *)arena_alloc(&g_unit_arena, (size_t)size + 1);
		if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
			fclose(f);
			fprintf(stderr, "gael: cannot read %s\n", so->name);
			return 0;
		}
		fclose(f);
		buf[size] = 0;
		g_unit.src = buf;
		g_unit.len = (size_t)size;
	}
	return 1;
}

/* next_use TOP FROM AT: the first (use "path") at the top level whose file is not compiled yet, as a module; -1 when
 * there is none, -2 after an error it reported. AT gets the path's node. The path is relative to FROM's folder */
static i32 next_use(i32 top, const char *from, i32 *at)
{
	const char *slash = strrchr(from, '/');
	char path[PATH_MAX];
	i32 n, m;
	for (n = top; n >= 0; n = next_of(n)) {
		i32 arg = next_of(first_of(n));
		size_t dir = slash ? (size_t)(slash - from) + 1 : 0;
		if (!head_is(n, "use") || arg < 0 || g_nodes[arg].kind != N_STR || next_of(arg) >= 0) continue;
		if (g_nodes[arg].text[0] == '/') dir = 0;
		if (dir + g_nodes[arg].len + 1 > sizeof(path)) {
			error_at(g_nodes[arg].line, g_nodes[arg].col, "use: the path is too long");
			return -2;
		}
		memcpy(path, from, dir);
		memcpy(path + dir, g_nodes[arg].text, g_nodes[arg].len + 1);
		if ((m = module_of(path)) < 0) {
			error_at(g_nodes[arg].line, g_nodes[arg].col, "use: there is no file %s", path);
			return -2;
		}
		if (g_mods[m].state == 2) continue;
		*at = arg;
		return m;
	}
	return -1;
}

/* load_failed: forget the files that were on the load stack, so the REPL can try them again */
static int load_failed(void)
{
	i32 i;
	for (i = 0; i < g_nmods; i++) {
		if (g_mods[i].state == 1) g_mods[i].state = 0;
	}
	return 1;
}

/* load NAME SRC LEN REPL MODE: compile NAME (SRC 0: read the file) and, before it, every file it uses and they use.
 * Then, by MODE, run or disassemble them in that order. A program runs only when no file has an error; in the REPL
 * a file runs as soon as it is compiled, like the forms typed after it. */
static int load(const char *name, const char *src, size_t len, int repl, int mode)
{
	i32 depth = 1, nentries = 0, i, top, entry, m, at = -1;
	int st;
	g_load[0].name = name;
	g_load[0].src = src;
	g_load[0].len = len;
	g_load[0].mod = -1;
	if (!src) {
		if ((m = module_of(name)) < 0) {
			fprintf(stderr, "gael: cannot open %s\n", name);
			return 1;
		}
		g_mods[m].state = 1;
		g_load[0].mod = m;
	}
	while (depth > 0) {
		Source *so = &g_load[depth - 1];
		if (!read_source(so)) return load_failed();
		top = read_unit();
		if (g_unit.nerrors) return load_failed();
		m = next_use(top, so->name, &at);
		if (m == -2) return load_failed();
		if (m >= 0) {
			if (g_mods[m].state == 1) {
				error_at(g_nodes[at].line, g_nodes[at].col, "use: %s uses this file, directly or through others", g_mods[m].shown);
				return load_failed();
			}
			if (depth == MAX_USE_DEPTH) {
				error_at(g_nodes[at].line, g_nodes[at].col, "use: files nested deeper than %d", MAX_USE_DEPTH);
				return load_failed();
			}
			g_mods[m].state = 1;
			g_load[depth].name = g_mods[m].shown;
			g_load[depth].src = 0;
			g_load[depth].len = 0;
			g_load[depth].mod = m;
			depth++;
			continue;
		}
		entry = compile_unit(top, repl && depth == 1);
		if (entry < 0) return load_failed();
		if (so->mod >= 0) g_mods[so->mod].state = 2;
		if (mode == DIS) {
			if (depth > 1) printf("; %s\n", so->name);
			disassemble(entry, g_ncode);
		} else if (mode == RUN && repl) {
			if ((st = vm_run(entry)) != 0) return st;
		} else {
			g_entries[nentries++] = entry;
		}
		depth--;
	}
	for (i = 0; mode == RUN && i < nentries; i++) {
		if ((st = vm_run(g_entries[i])) != 0) return st;
	}
	return 0;
}

/* balance TEXT: how many '(' are still open, outside strings and comments; the REPL reads more lines while > 0 */
static long balance(const char *s)
{
	long depth = 0;
	int in_str = 0;
	for (; *s; s++) {
		if (in_str) {
			if (*s == '\\' && s[1]) s++;
			else if (*s == '"') in_str = 0;
		} else if (*s == '"') {
			in_str = 1;
		} else if (*s == ';') {
			while (s[1] && s[1] != '\n') s++;
		} else if (*s == '(') {
			depth++;
		} else if (*s == ')') {
			depth--;
		}
	}
	return in_str ? 1 : depth;
}

/* leaks: with GAEL_CHECK=1, after a program ran to its end its globals are freed too, and then nothing may be left on
 * the heap. What is left was kept alive by a cycle (a vec or map that holds itself) or lost by a counting bug */
static int leaks(void)
{
	static const char *const kinds[] = { "?", "string", "vec", "map" };
	int k, any = 0;
	if (!g_check || !leak_check()) return 0;
	fflush(stdout);
	fputs("gael: GAEL_CHECK: never freed:", stderr);
	for (k = 1; k < 4; k++) {
		if (!g_objects[k]) continue;
		fprintf(stderr, "%s %ld %s%s", any ? "," : "", (long)g_objects[k], kinds[k], g_objects[k] == 1 ? "" : "s");
		any = 1;
	}
	if (!any) fprintf(stderr, " %ld cells", (long)g_live);
	fputc('\n', stderr);
	return 1;
}

static char g_line_buf[1 << 16];

static int repl(void)
{
	size_t used = 0;
	int interactive = 0;
	FILE *tty = fopen("/dev/tty", "r");
	if (tty) { interactive = 1; fclose(tty); } /* a prompt only where someone types */
	if (interactive) puts("gael 0.3 - :q quits, a form may span lines");
	for (;;) {
		if (interactive) {
			fputs(used ? "  ... " : "gael> ", stdout);
			fflush(stdout);
		}
		if (!fgets(g_line_buf + used, (int)(sizeof(g_line_buf) - used), stdin)) {
			if (interactive) putchar('\n');
			return 0;
		}
		if (!used && (!strcmp(g_line_buf, ":q\n") || !strcmp(g_line_buf, ":q"))) return 0;
		used = strlen(g_line_buf);
		if (used + 1 >= sizeof(g_line_buf)) {
			fputs("gael: input too long\n", stderr);
			used = 0;
			continue;
		}
		if (balance(g_line_buf) > 0) continue;
		(void)load("<repl>", g_line_buf, used, 1, RUN);
		fflush(stdout);
		used = 0;
	}
}

int main(int argc, char **argv)
{
	int st;
	vm_init(getenv("GAEL_CHECK") && !strcmp(getenv("GAEL_CHECK"), "1")); /* the text arena first: compile_init writes to it */
	compile_init();
	if (argc == 1) return repl();
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		fputs(g_usage, stdout);
		fputs(g_usage_env, stdout);
		return 0;
	}
	if (argv[1][0] != '-') {
		args_init(argc - 2, argv + 2);
		st = load(argv[1], 0, 0, 0, RUN);
		if (!st) st = leaks();
	} else if (argc == 3 && (!strcmp(argv[1], "-c") || !strcmp(argv[1], "-d"))) {
		st = load(argv[2], 0, 0, 0, argv[1][1] == 'c' ? CHECK : DIS);
	} else if (argc >= 3 && !strcmp(argv[1], "-e")) {
		args_init(argc - 3, argv + 3);
		st = load("<-e>", argv[2], strlen(argv[2]), 0, RUN);
		if (!st) st = leaks();
	} else {
		fputs(g_usage, stderr);
		fputs(g_usage_env, stderr);
		return 2;
	}
	fflush(stdout);
	return st;
}
