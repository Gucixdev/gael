/* gael - a small statically typed language with s-expressions, compiled to bytecode and run on a VM.
 *
 *   gael               the REPL
 *   gael FILE          compile FILE; when there are no errors, run it
 *   gael -c FILE       only compile: report every type error, run nothing
 *   gael -d FILE       compile and print the bytecode
 *   gael -e CODE       compile and run CODE
 *   gael -h            this help
 *
 * Exit status: 0 fine, 1 a compile or runtime error, 2 bad usage.
 * One translation unit (this file includes the others), C89, no malloc and no recursion. */

#include "base.h"
#include "ops.h"
#include "read.c"
#include "compile.c"
#include "vm.c"

static const char g_usage[] =
	"usage: gael               the REPL\n"
	"       gael FILE          compile FILE; when there are no errors, run it\n"
	"       gael -c FILE       only compile: report every type error, run nothing\n"
	"       gael -d FILE       compile and print the bytecode\n"
	"       gael -e CODE       compile and run CODE\n"
	"       gael -h            this help\n";

enum { RUN, CHECK, DIS };

/* run_text FILE SRC LEN REPL MODE: read, compile and (by MODE) run or disassemble one unit */
static int run_text(const char *file, const char *src, size_t len, int repl, int mode)
{
	i32 top, entry;
	g_unit.file = file;
	g_unit.src = src;
	g_unit.len = len;
	g_unit.nerrors = 0;
	g_nnodes = 0;
	top = read_unit();
	if (g_unit.nerrors) return 1;
	entry = compile_unit(top, repl);
	if (entry < 0) return 1;
	if (mode == DIS) { disassemble(entry, g_ncode); return 0; }
	if (mode == CHECK) return 0;
	return vm_run(entry);
}

static int run_file(const char *path, int mode)
{
	FILE *f = fopen(path, "rb");
	long size;
	char *buf;
	if (!f) {
		fprintf(stderr, "gael: cannot open %s\n", path);
		return 1;
	}
	if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		fprintf(stderr, "gael: cannot read %s\n", path);
		return 1;
	}
	buf = (char *)arena_alloc(&g_unit_arena, (size_t)size + 1);
	if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
		fclose(f);
		fprintf(stderr, "gael: cannot read %s\n", path);
		return 1;
	}
	fclose(f);
	buf[size] = 0;
	return run_text(path, buf, (size_t)size, 0, mode);
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

static char g_line_buf[1 << 16];

static int repl(void)
{
	size_t used = 0;
	int interactive = 0;
	FILE *tty = fopen("/dev/tty", "r");
	if (tty) { interactive = 1; fclose(tty); } /* a prompt only where someone types */
	if (interactive) puts("gael 0.2 - :q quits, a form may span lines");
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
		g_unit_arena.used = 0;
		(void)run_text("<repl>", g_line_buf, used, 1, RUN);
		fflush(stdout);
		used = 0;
	}
}

int main(int argc, char **argv)
{
	int st;
	if (argc == 1) return repl();
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		fputs(g_usage, stdout);
		return 0;
	}
	if (argc == 2 && argv[1][0] != '-') {
		st = run_file(argv[1], RUN);
	} else if (argc == 3 && (!strcmp(argv[1], "-c") || !strcmp(argv[1], "-d"))) {
		st = run_file(argv[2], argv[1][1] == 'c' ? CHECK : DIS);
	} else if (argc == 3 && !strcmp(argv[1], "-e")) {
		st = run_text("<-e>", argv[2], strlen(argv[2]), 0, RUN);
	} else {
		fputs(g_usage, stderr);
		return 2;
	}
	fflush(stdout);
	return st;
}
