/* vm.c - the bytecode machine: one operand stack of untyped cells and a stack of call frames.
 *
 * The compiler already proved every type, so a cell carries no tag: ADD_I reads two ints, ADD_F two floats. A string
 * is its place in the text arena. A struct or an option is a run of cells; a vec is one cell, the heap index of its
 * header (length, capacity, where its elements are). Integer arithmetic wraps (done in u64,
 * no undefined behaviour); dividing by zero and running out of stack are runtime errors with the line they happened
 * on. A frame needs its slots plus the deepest operand stack the compiler counted for it, and the VM checks that all
 * of it fits before it enters the frame, so no instruction inside can write past the stack. */

typedef union {
	i64 i;
	f64 f;
} Cell;

#define VM_STACK (1 << 16)
#define VM_FRAMES (1 << 12)

typedef struct {
	i32 ret_pc, fp;
} VFrame;

static Cell g_global_cells[MAX_GLOBAL_CELLS];

/* the heap: vecs and maps, laid out flat. A vec that outgrows its room moves to a new block twice the size; nothing is
 * freed */
#define HEAP_CELLS (1 << 22)
static Cell g_heap[HEAP_CELLS];
static i64 g_nheap;
enum { VH_LEN, VH_CAP, VH_DATA, VH_CELLS };

/* A map: this header; its entries in the order they went in, each an alive flag, the key and the value's cells; and
 * an index of SLOTS cells holding an entry's number + 1 (0: empty), probed linearly from the key's hash. A removed
 * entry stays in place, dead, until the map grows and packs the live ones together */
enum { MH_COUNT, MH_USED, MH_CAP, MH_ENT, MH_SLOTS, MH_IDX, MH_VSIZE, MH_STR, MH_CELLS };
static Cell g_vstack[VM_STACK];
static VFrame g_vframes[VM_FRAMES];

static int vm_error(i32 line, const char *msg)
{
	fflush(stdout);
	fprintf(stderr, "%s:%ld: runtime error: %s\n", g_unit.file, (long)line, msg);
	return 1;
}

/* Printing goes to g_sink, or, when it is 0, onto the end of the text arena: the string being built */
static FILE *g_sink;
static int g_sink_full; /* the text arena had no room for the whole string */

static int text_put(const char *p, size_t n)
{
	if (g_text.cap - g_text.used < n + 1) return 0; /* + 1: the 0 after the string */
	memcpy(g_text.base + g_text.used, p, n);
	g_text.used += n;
	return 1;
}

/* text_end START: close the string that began at START, return its value; the caller checked text_room first */
static i64 text_end(size_t start)
{
	g_text.base[g_text.used] = 0; /* text_put and text_room keep room for it */
	g_text.used++;
	return str_cell(start, g_text.used - 1 - start);
}

static int text_room(void) { return g_text.used < g_text.cap; }

/* c_str STR: the string with a 0 after it, for the C library. It is written to the free end of the text arena and
 * stays valid until the next string is made; 0 when there is no room. A string can be a piece of another, so it
 * has no 0 of its own */
static const char *c_str(i64 v)
{
	char *d = (char *)g_text.base + g_text.used;
	u32 n = STR_LEN(v);
	if (g_text.cap - g_text.used < (size_t)n + 1) return 0;
	memcpy(d, STR_P(v), n);
	d[n] = 0;
	return d;
}

/* str_find S PART FROM: where PART first starts in S at or after FROM, or -1 */
static i64 str_find(i64 hay, i64 part, i64 from)
{
	const char *h = STR_P(hay), *p = STR_P(part);
	i64 n = STR_LEN(hay), m = STR_LEN(part), i;
	for (i = from; i + m <= n; i++) {
		if (!memcmp(h + i, p, (size_t)m)) return i;
	}
	return -1;
}

static o out(const char *p, size_t n)
{
	if (g_sink) fwrite(p, 1, n, g_sink);
	else if (!g_sink_full && !text_put(p, n)) g_sink_full = 1;
}

static o out_s(const char *s) { out(s, strlen(s)); }

static o print_float(f64 v)
{
	char buf[64];
	sprintf(buf, "%.15g", v);
	out_s(buf);
	if (!strpbrk(buf, ".eni")) out_s(".0"); /* 2.0 stays visibly a float; inf and nan stay as they are */
}

/* print_quoted STR: a string inside a struct or an option, written the way the source would write it */
static o print_quoted(i64 v)
{
	const char *p = STR_P(v);
	u32 i, n = STR_LEN(v);
	out_s("\"");
	for (i = 0; i < n; i++) {
		if (p[i] == '"' || p[i] == '\\') { out("\\", 1); out(p + i, 1); }
		else if (p[i] == '\n') out_s("\\n");
		else if (p[i] == '\t') out_s("\\t");
		else out(p + i, 1);
	}
	out_s("\"");
}

#define PRINT_DEPTH 64

/* print_value TYPE CELLS: one value, as the REPL shows it: (Vec2 :x 1.0 :y 2.0), (some 3), none, north, (vec 1 2) */
static o print_value(Ty type, const Cell *c)
{
	struct {
		Ty t;
		i32 i;
		const Cell *c;
	} st[PRINT_DEPTH];
	i32 n = 1;
	st[0].t = type;
	st[0].i = -1;
	st[0].c = c;
	while (n > 0) {
		const TyInfo *ti = &g_tys[st[n - 1].t];
		const Cell *v = st[n - 1].c;
		i32 *i = &st[n - 1].i;
		if (ti->kind == K_VEC) {
			const Cell *hd = g_heap + v->i;
			if (*i < 0) {
				out_s("(vec");
				*i = 0;
			}
			if (*i >= hd[VH_LEN].i) { out_s(")"); n--; continue; }
			out_s(" ");
			if (n == PRINT_DEPTH) { out_s("..."); (*i)++; continue; }
			st[n].t = ti->elem;
			st[n].i = -1;
			st[n].c = g_heap + hd[VH_DATA].i + (i64)*i * cells(ti->elem);
			(*i)++;
			n++;
			continue;
		}
		if (ti->kind == K_MAP) { /* (map (k v) ...): *i counts three steps per entry - "(", key, value */
			const Cell *hd = g_heap + v->i, *ent;
			i64 esize = 2 + hd[MH_VSIZE].i, e;
			if (*i < 0) {
				out_s("(map");
				*i = 0;
			}
			e = *i / 3;
			while (*i % 3 == 0 && e < hd[MH_USED].i && !g_heap[hd[MH_ENT].i + e * esize].i) *i = (i32)(++e * 3);
			if (e >= hd[MH_USED].i) { out_s(")"); n--; continue; }
			ent = g_heap + hd[MH_ENT].i + e * esize;
			if (*i % 3 == 2) { /* the value was printed */
				out_s(")");
				(*i)++;
				continue;
			}
			out_s(*i % 3 ? " " : " (");
			(*i)++;
			if (n == PRINT_DEPTH) { out_s("..."); continue; }
			st[n].t = *i % 3 == 1 ? ti->key : ti->elem;
			st[n].i = -1;
			st[n].c = *i % 3 == 1 ? ent + 1 : ent + 2;
			n++;
			continue;
		}
		if (ti->kind == K_STRUCT || ti->kind == K_OPTION) {
			const Field *fd = 0;
			if (*i < 0) {
				if (ti->kind == K_OPTION && !v->i) { out_s("none"); n--; continue; }
				out_s("(");
				out_s(ti->kind == K_OPTION ? "some" : ti->name);
				*i = 0;
			}
			if (ti->kind == K_STRUCT && *i < ti->n) fd = &g_fields[ti->first + *i];
			if (ti->kind == K_OPTION && *i < 1) out_s(" ");
			else if (!fd) { out_s(")"); n--; continue; }
			else { out_s(" :"); out_s(fd->name); out_s(" "); }
			(*i)++;
			if (n == PRINT_DEPTH) { out_s("..."); continue; }
			st[n].t = fd ? fd->type : ti->elem;
			st[n].i = -1;
			st[n].c = fd ? v + fd->offset : v + 1;
			n++;
			continue;
		}
		if (ti->kind == K_ENUM) {
			out_s(g_variants[ti->first + v->i]);
		} else if (st[n - 1].t == T_INT) {
			char buf[32];
			sprintf(buf, "%ld", (long)v->i);
			out_s(buf);
		} else if (st[n - 1].t == T_FLOAT) {
			print_float(v->f);
		} else if (st[n - 1].t == T_BOOL) {
			out_s(v->i ? "true" : "false");
		} else if (st[n - 1].t == T_STR && n > 1) {
			print_quoted(v->i);
		} else if (st[n - 1].t == T_STR) {
			out(STR_P(v->i), STR_LEN(v->i));
		} else {
			out_s("()");
		}
		n--;
	}
}

/* print_fmt FORMAT ARGS: a print or println; the compiler checked every % code against its value */
static o print_fmt(const Fmt *fm, const Cell *args)
{
	const Str *fmt = &g_strs[fm->str];
	u32 i;
	i32 k = 0;
	for (i = 0; i < fmt->len; i++) {
		if (fmt->p[i] != '%') { out(fmt->p + i, 1); continue; }
		if (fmt->p[++i] == '%') { out_s("%"); continue; }
		print_value(g_fmt_tys[fm->first + k], args);
		args += cells(g_fmt_tys[fm->first + k]);
		k++;
	}
}

static int str_eq(i64 a, i64 b) { return STR_LEN(a) == STR_LEN(b) && !memcmp(STR_P(a), STR_P(b), STR_LEN(a)); }

/* trimmed STR START LEN: the string without spaces, tabs and newlines around it */
static o trimmed(i64 v, const char **p, u32 *n)
{
	*p = STR_P(v);
	*n = STR_LEN(v);
	while (*n && strchr(" \t\r\n", (*p)[0])) (*p)++, (*n)--;
	while (*n && strchr(" \t\r\n", (*p)[*n - 1])) (*n)--;
}

/* parse_int STR OUT: 1 when the whole string (spaces around it aside) is an int that fits in 64 bits */
static int parse_int(i64 v, i64 *out_v)
{
	const char *p;
	u32 n, i = 0;
	u64 acc = 0, limit;
	int neg;
	trimmed(v, &p, &n);
	neg = n && p[0] == '-';
	if (n && (p[0] == '-' || p[0] == '+')) i = 1;
	if (i == n) return 0;
	limit = neg ? (u64)1 << 63 : ((u64)1 << 63) - 1;
	for (; i < n; i++) {
		u64 d = (u64)(p[i] - '0');
		if (p[i] < '0' || p[i] > '9' || acc > (limit - d) / 10) return 0;
		acc = acc * 10 + d;
	}
	*out_v = neg ? (i64)(0 - acc) : (i64)acc;
	return 1;
}

/* parse_float STR OUT: 1 when the whole string (spaces around it aside) is a number strtod reads */
static int parse_float(i64 v, f64 *out_v)
{
	char buf[128], *end;
	const char *p;
	u32 n;
	trimmed(v, &p, &n);
	if (!n || n >= sizeof(buf)) return 0;
	memcpy(buf, p, n);
	buf[n] = 0;
	*out_v = strtod(buf, &end);
	return *end == 0;
}

/* run_cmd CMD RES: run a command (0-terminated) and return its standard output as a new string, the newlines at its
 * end left out; 0 when the text arena filled up (the rest of the output is still read, so the command can finish).
 * CMD may sit at the free end of the text arena: popen has passed it to the child before the output lands on it */
static int run_cmd(const char *c, i64 *res)
{
	size_t start = g_text.used;
	int full = !c;
	FILE *pipe = 0;
	fflush(stdout);
	if (c) pipe = popen(c, "r");
	if (pipe) {
		for (;;) {
			char buf[4096];
			size_t got = fread(buf, 1, sizeof(buf), pipe);
			if (!got) break;
			if (!full && !text_put(buf, got)) full = 1;
		}
		pclose(pipe);
	}
	if (full) {
		g_text.used = start;
		return 0;
	}
	while (g_text.used > start && g_text.base[g_text.used - 1] == '\n') g_text.used--;
	*res = text_end(start);
	return 1;
}

static int run_sh(i64 cmd, i64 *res) { return run_cmd(c_str(cmd), res); }

/* run_pipe CMDS N INPUT RES: the commands joined into one pipeline, each in its own subshell; with INPUT the first
 * string goes to its stdin through a temporary file. 0 when the text arena filled up or no file could be made */
static int run_pipe(const Cell *cmds, i64 n, int input, i64 *res)
{
	size_t start = g_text.used;
	char tmp[600];
	int ok = 1, fd;
	i64 k;
	if (input) {
		const char *dir = getenv("TMPDIR");
		if (!dir || strlen(dir) > 500) dir = "/tmp";
		sprintf(tmp, "%s/gael-stdin.XXXXXX", dir);
		if ((fd = mkstemp(tmp)) < 0) return 0;
		ok = write(fd, STR_P(cmds[0].i), STR_LEN(cmds[0].i)) == (long)STR_LEN(cmds[0].i);
		close(fd);
	}
	for (k = input; ok && k < n; k++) { /* "(a) < 'tmp' | (b)", built at the free end of the text arena */
		if (k > input) ok = text_put(" | ", 3);
		ok = ok && text_put("(", 1) && text_put(STR_P(cmds[k].i), STR_LEN(cmds[k].i)) && text_put(")", 1);
		if (ok && input && k == input) ok = text_put(" < '", 4) && text_put(tmp, strlen(tmp)) && text_put("'", 1);
	}
	if (ok) ok = text_room();
	if (ok) {
		g_text.base[g_text.used] = 0;
		g_text.used = start; /* the command was scratch; the output goes where it was */
		ok = run_cmd((const char *)g_text.base + start, res);
	} else {
		g_text.used = start;
	}
	if (input) unlink(tmp);
	return ok;
}

/* heap_alloc N: N cells off the heap; -1 when it is full */
static i64 heap_alloc(i64 n)
{
	i64 at = g_nheap;
	if (n > HEAP_CELLS - g_nheap) return -1;
	g_nheap += n;
	return at;
}

/* vec_new N SIZE FROM: a vec of the N elements (SIZE cells each) at FROM, or of N zeros without FROM; -1 when the heap
 * is full */
static i64 vec_new(i64 n, i64 size, const Cell *from)
{
	i64 cap = n < 4 ? 4 : n, h = heap_alloc(VH_CELLS + cap * size);
	if (h < 0) return -1;
	g_heap[h + VH_LEN].i = n;
	g_heap[h + VH_CAP].i = cap;
	g_heap[h + VH_DATA].i = h + VH_CELLS;
	if (from) memcpy(g_heap + h + VH_CELLS, from, (size_t)(n * size) * sizeof(Cell));
	else memset(g_heap + h + VH_CELLS, 0, (size_t)(n * size) * sizeof(Cell));
	return h;
}

/* vec_push V SIZE FROM: append one element; 0 when the heap is full */
static int vec_push(i64 v, i64 size, const Cell *from)
{
	Cell *hd = g_heap + v;
	if (hd[VH_LEN].i == hd[VH_CAP].i) {
		i64 cap = hd[VH_CAP].i * 2, data = heap_alloc(cap * size);
		if (data < 0) return 0;
		memcpy(g_heap + data, g_heap + hd[VH_DATA].i, (size_t)(hd[VH_LEN].i * size) * sizeof(Cell));
		hd[VH_DATA].i = data;
		hd[VH_CAP].i = cap;
	}
	memcpy(g_heap + hd[VH_DATA].i + hd[VH_LEN].i * size, from, (size_t)size * sizeof(Cell));
	hd[VH_LEN].i++;
	return 1;
}

static u64 hash_key(i64 key, i64 is_str)
{
	u64 h;
	if (is_str) { /* FNV-1a */
		const char *p = STR_P(key);
		u32 i, n = STR_LEN(key);
		h = 14695981039346656037UL;
		for (i = 0; i < n; i++) h = (h ^ (u8)p[i]) * 1099511628211UL;
		return h;
	}
	h = (u64)key; /* the splitmix64 finish: an int, a bool or an enum value spread over all the bits */
	h ^= h >> 30;
	h *= 0xbf58476d1ce4e5b9UL;
	h ^= h >> 27;
	h *= 0x94d049bb133111ebUL;
	return h ^ (h >> 31);
}

/* map_new VSIZE STR: an empty map whose values take VSIZE cells, keyed by strings when STR; -1 when the heap is full */
static i64 map_new(i64 vsize, i64 is_str)
{
	i64 h = heap_alloc(MH_CELLS), ent = heap_alloc(8 * (2 + vsize)), idx = heap_alloc(16);
	if (h < 0 || ent < 0 || idx < 0) return -1;
	g_heap[h + MH_COUNT].i = 0;
	g_heap[h + MH_USED].i = 0;
	g_heap[h + MH_CAP].i = 8;
	g_heap[h + MH_ENT].i = ent;
	g_heap[h + MH_SLOTS].i = 16;
	g_heap[h + MH_IDX].i = idx;
	g_heap[h + MH_VSIZE].i = vsize;
	g_heap[h + MH_STR].i = is_str;
	memset(g_heap + idx, 0, 16 * sizeof(Cell));
	return h;
}

/* map_find M KEY SLOT: the entry number of KEY, or -1; *SLOT gets the index slot where it is or would go */
static i64 map_find(i64 m, i64 key, i64 *slot)
{
	const Cell *hd = g_heap + m;
	i64 mask = hd[MH_SLOTS].i - 1, esize = 2 + hd[MH_VSIZE].i, at = (i64)(hash_key(key, hd[MH_STR].i) & (u64)mask);
	for (;; at = (at + 1) & mask) {
		i64 e = g_heap[hd[MH_IDX].i + at].i;
		const Cell *ent;
		if (!e) {
			*slot = at;
			return -1;
		}
		ent = g_heap + hd[MH_ENT].i + (e - 1) * esize;
		if (ent[0].i && (hd[MH_STR].i ? str_eq(ent[1].i, key) : ent[1].i == key)) {
			*slot = at;
			return e - 1;
		}
	}
}

/* map_grow M: room for twice the entries, the dead ones packed out, the index made again (it stays at most half
 * full, so a probe always ends); 0 when the heap is full */
static int map_grow(i64 m)
{
	Cell *hd = g_heap + m;
	i64 esize = 2 + hd[MH_VSIZE].i, cap = hd[MH_CAP].i * 2, slots = hd[MH_SLOTS].i * 2;
	i64 ent = heap_alloc(cap * esize), idx = heap_alloc(slots), e, k = 0, slot;
	if (ent < 0 || idx < 0) return 0;
	for (e = 0; e < hd[MH_USED].i; e++) {
		const Cell *from = g_heap + hd[MH_ENT].i + e * esize;
		if (from[0].i) memcpy(g_heap + ent + k++ * esize, from, (size_t)esize * sizeof(Cell));
	}
	memset(g_heap + idx, 0, (size_t)slots * sizeof(Cell));
	hd[MH_ENT].i = ent;
	hd[MH_IDX].i = idx;
	hd[MH_CAP].i = cap;
	hd[MH_SLOTS].i = slots;
	hd[MH_USED].i = k;
	for (e = 0; e < k; e++) {
		(void)map_find(m, g_heap[ent + e * esize + 1].i, &slot);
		g_heap[idx + slot].i = e + 1;
	}
	return 1;
}

/* map_insert M KEY VALUE: add the entry, or give an existing key the new value; 0 when the heap is full */
static int map_insert(i64 m, i64 key, const Cell *val)
{
	Cell *hd = g_heap + m, *ent;
	i64 vsize = hd[MH_VSIZE].i, esize = 2 + vsize, slot, e = map_find(m, key, &slot);
	if (e >= 0) {
		memcpy(g_heap + hd[MH_ENT].i + e * esize + 2, val, (size_t)vsize * sizeof(Cell));
		return 1;
	}
	if (hd[MH_USED].i == hd[MH_CAP].i) {
		if (!map_grow(m)) return 0;
		(void)map_find(m, key, &slot);
	}
	e = hd[MH_USED].i++;
	ent = g_heap + hd[MH_ENT].i + e * esize;
	ent[0].i = 1;
	ent[1].i = key;
	memcpy(ent + 2, val, (size_t)vsize * sizeof(Cell));
	g_heap[hd[MH_IDX].i + slot].i = e + 1;
	hd[MH_COUNT].i++;
	return 1;
}

/* index_error LINE I LEN: the runtime error for an index outside a vec */
static int index_error(i32 line, i64 i, i64 len)
{
	char msg[96];
	sprintf(msg, "index %ld is outside a vec of %ld", (long)i, (long)len);
	return vm_error(line, msg);
}

#define HEAP_FULL "out of memory for vecs and maps (the heap is full)"

/* the command line after the file name, copied into the text arena once */
#define MAX_ARGS 4096
static i64 g_arg_cells[MAX_ARGS];
static i64 g_nargs;

static o args_init(int argc, char **argv)
{
	int i;
	for (i = 0; i < argc && i < MAX_ARGS; i++) {
		size_t start = g_text.used;
		if (!text_put(argv[i], strlen(argv[i]))) die("arguments too long for the text arena");
		g_arg_cells[g_nargs++] = text_end(start);
	}
}

/* read_into FILE: everything left in FILE as a new string; 0 when the text arena filled up */
static int read_into(FILE *in, i64 *res)
{
	size_t start = g_text.used;
	int ok = text_room();
	for (;;) {
		char buf[4096];
		size_t got = fread(buf, 1, sizeof(buf), in);
		if (!got) break;
		if (ok && !text_put(buf, got)) ok = 0;
	}
	if (!ok) {
		g_text.used = start;
		return 0;
	}
	*res = text_end(start);
	return 1;
}
#define TEXT_FULL "out of memory for strings (the text arena is full)"

/* range_error LINE FROM TO LEN: substr's bytes FROM up to TO do not fit a string of LEN, or TO is before FROM */
static int range_error(i32 line, i64 from, i64 to, i64 len)
{
	char msg[160];
	sprintf(msg, "substr %ld %ld does not fit a string of %ld bytes", (long)from, (long)to, (long)len);
	return vm_error(line, msg);
}

/* vm_run PC: run from PC until HALT; 0 when it finished, 1 after a runtime error */
static int vm_run(i32 pc)
{
	i32 sp = 0, fp = 0, nf = 0;
	Cell *s = g_vstack;
	for (;;) {
		const Ins *in = &g_code[pc++];
		switch ((Op)in->op) {
		case OP_HALT: return 0;
		case OP_ENTER:
			if (fp + in->a + in->b > VM_STACK) return vm_error(in->line, "stack overflow");
			while (sp < fp + in->a) s[sp++].i = 0;
			break;
		case OP_PUSH_I: s[sp++].i = g_ints[in->a]; break;
		case OP_PUSH_F: s[sp++].f = g_floats[in->a]; break;
		case OP_PUSH_S: s[sp++].i = g_strs[in->a].cell; break;
		case OP_PUSH_K: s[sp++].i = in->a; break;
		case OP_ZERO: memset(s + sp, 0, (size_t)in->a * sizeof(Cell)); sp += in->a; break;
		case OP_POP: sp--; break;
		case OP_POPN: sp -= in->a; break;
		case OP_LOAD_L: s[sp++] = s[fp + in->a]; break;
		case OP_STORE_L: s[fp + in->a] = s[--sp]; break;
		case OP_LOAD_G: s[sp++] = g_global_cells[in->a]; break;
		case OP_STORE_G: g_global_cells[in->a] = s[--sp]; break;
		case OP_LOADN_L: memcpy(s + sp, s + fp + in->a, (size_t)in->b * sizeof(Cell)); sp += in->b; break;
		case OP_STOREN_L: sp -= in->b; memcpy(s + fp + in->a, s + sp, (size_t)in->b * sizeof(Cell)); break;
		case OP_LOADN_G: memcpy(s + sp, g_global_cells + in->a, (size_t)in->b * sizeof(Cell)); sp += in->b; break;
		case OP_STOREN_G: sp -= in->b; memcpy(g_global_cells + in->a, s + sp, (size_t)in->b * sizeof(Cell)); break;

		case OP_ADD_I: sp--; s[sp - 1].i = (i64)((u64)s[sp - 1].i + (u64)s[sp].i); break;
		case OP_SUB_I: sp--; s[sp - 1].i = (i64)((u64)s[sp - 1].i - (u64)s[sp].i); break;
		case OP_MUL_I: sp--; s[sp - 1].i = (i64)((u64)s[sp - 1].i * (u64)s[sp].i); break;
		case OP_DIV_I:
		case OP_MOD_I:
			sp--;
			if (s[sp].i == 0) return vm_error(in->line, "division by zero");
			if (s[sp].i == -1) { /* INT_MIN / -1 overflows; -x and 0 are what wrapping gives */
				s[sp - 1].i = in->op == OP_DIV_I ? (i64)(0 - (u64)s[sp - 1].i) : 0;
				break;
			}
			s[sp - 1].i = in->op == OP_DIV_I ? s[sp - 1].i / s[sp].i : s[sp - 1].i % s[sp].i;
			break;
		case OP_NEG_I: s[sp - 1].i = (i64)(0 - (u64)s[sp - 1].i); break;
		case OP_ADD_F: sp--; s[sp - 1].f += s[sp].f; break;
		case OP_SUB_F: sp--; s[sp - 1].f -= s[sp].f; break;
		case OP_MUL_F: sp--; s[sp - 1].f *= s[sp].f; break;
		case OP_DIV_F: sp--; s[sp - 1].f /= s[sp].f; break;
		case OP_NEG_F: s[sp - 1].f = -s[sp - 1].f; break;

		case OP_LT_I: sp--; s[sp - 1].i = s[sp - 1].i < s[sp].i; break;
		case OP_LE_I: sp--; s[sp - 1].i = s[sp - 1].i <= s[sp].i; break;
		case OP_GT_I: sp--; s[sp - 1].i = s[sp - 1].i > s[sp].i; break;
		case OP_GE_I: sp--; s[sp - 1].i = s[sp - 1].i >= s[sp].i; break;
		case OP_EQ_I: sp--; s[sp - 1].i = s[sp - 1].i == s[sp].i; break;
		case OP_NE_I: sp--; s[sp - 1].i = s[sp - 1].i != s[sp].i; break;
		case OP_LT_F: sp--; s[sp - 1].i = s[sp - 1].f < s[sp].f; break;
		case OP_LE_F: sp--; s[sp - 1].i = s[sp - 1].f <= s[sp].f; break;
		case OP_GT_F: sp--; s[sp - 1].i = s[sp - 1].f > s[sp].f; break;
		case OP_GE_F: sp--; s[sp - 1].i = s[sp - 1].f >= s[sp].f; break;
		case OP_EQ_F: sp--; s[sp - 1].i = s[sp - 1].f == s[sp].f; break;
		case OP_NE_F: sp--; s[sp - 1].i = s[sp - 1].f != s[sp].f; break;
		case OP_EQ_S: sp--; s[sp - 1].i = str_eq(s[sp - 1].i, s[sp].i); break;
		case OP_NE_S: sp--; s[sp - 1].i = !str_eq(s[sp - 1].i, s[sp].i); break;
		case OP_NOT: s[sp - 1].i = !s[sp - 1].i; break;

		case OP_JMP: pc = in->a; break;
		case OP_JF: if (!s[--sp].i) pc = in->a; break;
		case OP_JT: if (s[--sp].i) pc = in->a; break;

		case OP_I2F: s[sp - 1].f = (f64)s[sp - 1].i; break;
		case OP_F2I: {
			f64 v = s[sp - 1].f;
			if (!(v > -9223372036854775808.0 && v < 9223372036854775808.0)) /* also false for nan */
				return vm_error(in->line, "int of a float that does not fit in 64 bits");
			s[sp - 1].i = (i64)v;
			break;
		}
		case OP_LEN_S: s[sp - 1].i = (i64)STR_LEN(s[sp - 1].i); break;

		case OP_CALL: {
			const Fn *fn = &g_fns[in->a];
			i32 nfp = sp - fn->pcells;
			if (nf == VM_FRAMES || nfp + fn->nslots + fn->maxdepth > VM_STACK)
				return vm_error(in->line, "stack overflow (recursion too deep?)");
			g_vframes[nf].ret_pc = pc;
			g_vframes[nf].fp = fp;
			nf++;
			fp = nfp;
			while (sp < fp + fn->nslots) s[sp++].i = 0;
			pc = fn->entry;
			break;
		}
		case OP_RET_V: {
			Cell r = s[sp - 1];
			sp = fp;
			nf--;
			fp = g_vframes[nf].fp;
			pc = g_vframes[nf].ret_pc;
			s[sp++] = r;
			break;
		}
		case OP_RET_N:
			memmove(s + fp, s + sp - in->a, (size_t)in->a * sizeof(Cell));
			sp = fp + in->a;
			nf--;
			fp = g_vframes[nf].fp;
			pc = g_vframes[nf].ret_pc;
			break;
		case OP_RET_U:
			sp = fp;
			nf--;
			fp = g_vframes[nf].fp;
			pc = g_vframes[nf].ret_pc;
			break;

		case OP_PRINT:
		case OP_PRINTLN:
			sp -= g_fmts[in->a].cells;
			if (in->b) fflush(stdout); /* stdout's text first when both go to one terminal */
			g_sink = in->b ? stderr : stdout;
			print_fmt(&g_fmts[in->a], s + sp);
			if (in->op == OP_PRINTLN) putc('\n', g_sink);
			break;
		case OP_FORMAT: {
			size_t start = g_text.used;
			sp -= g_fmts[in->a].cells;
			g_sink = 0;
			g_sink_full = !text_room();
			print_fmt(&g_fmts[in->a], s + sp);
			if (g_sink_full) return vm_error(in->line, TEXT_FULL);
			s[sp++].i = text_end(start);
			break;
		}
		case OP_SH:
			if (!run_sh(s[sp - 1].i, &s[sp - 1].i)) return vm_error(in->line, TEXT_FULL);
			break;
		case OP_SHPIPE: {
			i64 res;
			sp -= in->a;
			if (!run_pipe(s + sp, in->a, in->b, &res))
				return vm_error(in->line, in->b ? "<<<: no room for the text, or no temporary file for it" : TEXT_FULL);
			s[sp++].i = res;
			break;
		}
		case OP_PID: s[sp++].i = (i64)getpid(); break;
		case OP_ENV: {
			const char *name = c_str(s[sp - 1].i), *val;
			size_t start = g_text.used;
			if (!name) return vm_error(in->line, TEXT_FULL);
			val = getenv(name);
			if (!val) val = "";
			if (!text_put(val, strlen(val)) || !text_room()) return vm_error(in->line, TEXT_FULL);
			s[sp - 1].i = text_end(start);
			break;
		}
		case OP_SYSTEM: {
			const char *c = c_str(s[sp - 1].i);
			int st;
			if (!c) return vm_error(in->line, TEXT_FULL);
			fflush(stdout);
			st = system(c);
			s[sp - 1].i = st == -1 ? -1 : (st >> 8) & 0xff; /* the exit status as the shell reports it (Linux) */
			break;
		}
		case OP_PARSE_I: {
			i64 v = 0;
			int ok = parse_int(s[sp - 1].i, &v);
			s[sp - 1].i = ok;
			s[sp++].i = v;
			break;
		}
		case OP_PARSE_F: {
			f64 v = 0;
			int ok = parse_float(s[sp - 1].i, &v);
			s[sp - 1].i = ok;
			s[sp++].f = ok ? v : 0;
			break;
		}
		case OP_SUBSTR: {
			i64 v = s[sp - 3].i, from = s[sp - 2].i, to = s[sp - 1].i;
			if (from < 0 || from > to || to > STR_LEN(v)) return range_error(in->line, from, to, STR_LEN(v));
			sp -= 2;
			s[sp - 1].i = str_cell(STR_OFF(v) + (size_t)from, (size_t)(to - from));
			break;
		}
		case OP_BYTE: {
			i64 v = s[sp - 2].i, i = s[sp - 1].i;
			if (i < 0 || i >= STR_LEN(v)) {
				char msg[96];
				sprintf(msg, "byte %ld is outside a string of %ld bytes", (long)i, (long)STR_LEN(v));
				return vm_error(in->line, msg);
			}
			sp--;
			s[sp - 1].i = (u8)STR_P(v)[i];
			break;
		}
		case OP_FIND: { /* the two strings become the option's two cells */
			i64 at = str_find(s[sp - 2].i, s[sp - 1].i, 0);
			s[sp - 2].i = at >= 0;
			s[sp - 1].i = at >= 0 ? at : 0;
			break;
		}
		case OP_SPLIT: {
			i64 v = s[sp - 2].i, sep = s[sp - 1].i, n = 1, at, start = 0, h;
			Cell *data;
			if (!STR_LEN(sep)) return vm_error(in->line, "split: the separator is empty");
			for (at = str_find(v, sep, 0); at >= 0; at = str_find(v, sep, at + STR_LEN(sep))) n++;
			if ((h = vec_new(n, 1, 0)) < 0) return vm_error(in->line, HEAP_FULL);
			data = g_heap + g_heap[h + VH_DATA].i;
			for (n = 0; (at = str_find(v, sep, start)) >= 0; start = at + STR_LEN(sep))
				data[n++].i = str_cell(STR_OFF(v) + (size_t)start, (size_t)(at - start));
			data[n].i = str_cell(STR_OFF(v) + (size_t)start, (size_t)(STR_LEN(v) - start));
			sp--;
			s[sp - 1].i = h;
			break;
		}
		case OP_JOIN: {
			const Cell *hd = g_heap + s[sp - 2].i;
			i64 sep = s[sp - 1].i, k;
			size_t start = g_text.used;
			int ok = text_room();
			for (k = 0; ok && k < hd[VH_LEN].i; k++) {
				i64 e = g_heap[hd[VH_DATA].i + k].i;
				if (k) ok = text_put(STR_P(sep), STR_LEN(sep));
				if (ok) ok = text_put(STR_P(e), STR_LEN(e));
			}
			if (!ok) {
				g_text.used = start;
				return vm_error(in->line, TEXT_FULL);
			}
			sp--;
			s[sp - 1].i = text_end(start);
			break;
		}
		case OP_CHAR:
			if (s[sp - 1].i < 0 || s[sp - 1].i > 255) {
				char msg[64];
				sprintf(msg, "char takes 0 to 255, this is %ld", (long)s[sp - 1].i);
				return vm_error(in->line, msg);
			}
			s[sp - 1].i = str_cell((size_t)(2 * s[sp - 1].i), 1);
			break;
		case OP_TRIM: {
			const char *p;
			u32 n;
			trimmed(s[sp - 1].i, &p, &n);
			s[sp - 1].i = str_cell((size_t)(p - (const char *)g_text.base), n);
			break;
		}
		case OP_READ_FILE: { /* the path's cell becomes the option's two: tag, string */
			const char *path = c_str(s[sp - 1].i);
			FILE *file = path ? fopen(path, "rb") : 0;
			i64 v = 0;
			int ok = 1;
			if (!path) return vm_error(in->line, TEXT_FULL);
			if (file) {
				ok = read_into(file, &v);
				fclose(file);
				if (!ok) return vm_error(in->line, TEXT_FULL);
			}
			s[sp - 1].i = file != 0;
			s[sp++].i = v;
			break;
		}
		case OP_WRITE_FILE: { /* a: append, b: the text is below the path */
			i64 text = s[sp - (in->b ? 2 : 1)].i;
			const char *path = c_str(s[sp - (in->b ? 1 : 2)].i);
			FILE *file = path ? fopen(path, in->a ? "ab" : "wb") : 0;
			int ok = file && fwrite(STR_P(text), 1, STR_LEN(text), file) == STR_LEN(text);
			if (file && fclose(file)) ok = 0;
			if (!path) return vm_error(in->line, TEXT_FULL);
			sp--;
			s[sp - 1].i = ok;
			break;
		}
		case OP_READ_LINE: {
			size_t start = g_text.used;
			int c = EOF, any = 0;
			fflush(stdout); /* a prompt printed before it shows up first */
			while ((c = getchar()) != EOF) {
				char ch = (char)c;
				any = 1;
				if (ch == '\n') break;
				if (!text_put(&ch, 1)) {
					g_text.used = start;
					return vm_error(in->line, TEXT_FULL);
				}
			}
			if (g_text.used > start && g_text.base[g_text.used - 1] == '\r') g_text.used--;
			if (!any || !text_room()) {
				g_text.used = start;
				s[sp++].i = 0;
				s[sp++].i = 0;
				if (any) return vm_error(in->line, TEXT_FULL);
				break;
			}
			s[sp++].i = 1;
			s[sp++].i = text_end(start);
			break;
		}
		case OP_GETENV: {
			const char *name = c_str(s[sp - 1].i), *val;
			size_t start = g_text.used;
			if (!name) return vm_error(in->line, TEXT_FULL);
			val = getenv(name);
			s[sp - 1].i = val != 0;
			s[sp].i = 0;
			if (val) {
				if (!text_put(val, strlen(val))) return vm_error(in->line, TEXT_FULL);
				s[sp].i = text_end(start);
			}
			sp++;
			break;
		}
		case OP_ARGS: {
			i64 h = vec_new(g_nargs, 1, (const Cell *)0);
			i64 k;
			if (h < 0) return vm_error(in->line, HEAP_FULL);
			for (k = 0; k < g_nargs; k++) g_heap[g_heap[h + VH_DATA].i + k].i = g_arg_cells[k];
			s[sp++].i = h;
			break;
		}
		case OP_EXIT:
			fflush(stdout);
			exit((int)(s[sp - 1].i & 0xff));
		case OP_INC_L: s[fp + in->a].i = (i64)((u64)s[fp + in->a].i + 1); break;
		case OP_VEC_NEW: {
			i64 h;
			sp -= in->a * in->b;
			if ((h = vec_new(in->a, in->b, s + sp)) < 0) return vm_error(in->line, HEAP_FULL);
			s[sp++].i = h;
			break;
		}
		case OP_VEC_LEN: s[sp - 1].i = g_heap[s[sp - 1].i + VH_LEN].i; break;
		case OP_VEC_PUSH:
			sp -= in->a + 1;
			if (!vec_push(s[sp].i, in->a, s + sp + 1)) return vm_error(in->line, HEAP_FULL);
			break;
		case OP_VEC_GET: {
			const Cell *hd = g_heap + s[sp - 2].i;
			i64 i = s[sp - 1].i;
			if (i < 0 || i >= hd[VH_LEN].i) return index_error(in->line, i, hd[VH_LEN].i);
			sp -= 2;
			memcpy(s + sp, g_heap + hd[VH_DATA].i + i * in->a, (size_t)in->a * sizeof(Cell));
			sp += in->a;
			break;
		}
		case OP_VEC_PUT: {
			const Cell *hd;
			i64 i;
			sp -= in->a + 2;
			hd = g_heap + s[sp].i;
			i = s[sp + 1].i;
			if (i < 0 || i >= hd[VH_LEN].i) return index_error(in->line, i, hd[VH_LEN].i);
			memcpy(g_heap + hd[VH_DATA].i + i * in->a, s + sp + 2, (size_t)in->a * sizeof(Cell));
			break;
		}
		case OP_VEC_POP: {
			Cell *hd = g_heap + s[sp - 1].i;
			if (!hd[VH_LEN].i) { /* none */
				s[sp - 1].i = 0;
				memset(s + sp, 0, (size_t)in->a * sizeof(Cell));
			} else {
				hd[VH_LEN].i--;
				s[sp - 1].i = 1;
				memcpy(s + sp, g_heap + hd[VH_DATA].i + hd[VH_LEN].i * in->a, (size_t)in->a * sizeof(Cell));
			}
			sp += in->a;
			break;
		}
		case OP_MAP_NEW: { /* a: entries on the stack, b: value cells * 2 + (keys are strings) */
			i64 vsize = in->b >> 1, k, m;
			sp -= in->a * (1 + vsize);
			if ((m = map_new(vsize, in->b & 1)) < 0) return vm_error(in->line, HEAP_FULL);
			for (k = 0; k < in->a; k++) {
				const Cell *e = s + sp + k * (1 + vsize);
				if (!map_insert(m, e[0].i, e + 1)) return vm_error(in->line, HEAP_FULL);
			}
			s[sp++].i = m;
			break;
		}
		case OP_MAP_INSERT:
			sp -= in->a + 2;
			if (!map_insert(s[sp].i, s[sp + 1].i, s + sp + 2)) return vm_error(in->line, HEAP_FULL);
			break;
		case OP_MAP_LOOKUP:
		case OP_MAP_REMOVE: { /* the map and the key become the option: tag, value */
			i64 m = s[sp - 2].i, slot, e = map_find(m, s[sp - 1].i, &slot);
			sp -= 2;
			s[sp].i = e >= 0;
			if (e >= 0) {
				Cell *ent = g_heap + g_heap[m + MH_ENT].i + e * (2 + in->a);
				memcpy(s + sp + 1, ent + 2, (size_t)in->a * sizeof(Cell));
				if (in->op == OP_MAP_REMOVE) {
					ent[0].i = 0;
					g_heap[m + MH_COUNT].i--;
				}
			} else {
				memset(s + sp + 1, 0, (size_t)in->a * sizeof(Cell));
			}
			sp += 1 + in->a;
			break;
		}
		case OP_MAP_HAS: {
			i64 slot;
			sp--;
			s[sp - 1].i = map_find(s[sp - 1].i, s[sp].i, &slot) >= 0;
			break;
		}
		case OP_MAP_LEN: s[sp - 1].i = g_heap[s[sp - 1].i + MH_COUNT].i; break;
		case OP_MAP_KEYS:
		case OP_MAP_VALUES: {
			i64 m = s[sp - 1].i, esize = 2 + g_heap[m + MH_VSIZE].i, size = in->op == OP_MAP_KEYS ? 1 : g_heap[m + MH_VSIZE].i;
			i64 e, k = 0, v = vec_new(g_heap[m + MH_COUNT].i, size, (const Cell *)0);
			Cell *data;
			if (v < 0) return vm_error(in->line, HEAP_FULL);
			data = g_heap + g_heap[v + VH_DATA].i;
			for (e = 0; e < g_heap[m + MH_USED].i; e++) {
				const Cell *ent = g_heap + g_heap[m + MH_ENT].i + e * esize;
				if (ent[0].i) memcpy(data + k++ * size, in->op == OP_MAP_KEYS ? ent + 1 : ent + 2, (size_t)size * sizeof(Cell));
			}
			s[sp - 1].i = v;
			break;
		}
		case OP_SHOW:
			sp -= cells((Ty)in->a);
			g_sink = stdout;
			fputs("=> ", stdout);
			print_value((Ty)in->a, s + sp);
			printf(" : %s\n", tn((Ty)in->a));
			break;
		default:
			return vm_error(in->line, "internal: unknown instruction");
		}
	}
}

#define GAEL_OP_NAME_(name, n) #name,
static const char *const g_op_names[] = { GAEL_OPS(GAEL_OP_NAME_) "?" };
#define GAEL_OP_ARGS_(name, n) n,
static const int g_op_args[] = { GAEL_OPS(GAEL_OP_ARGS_) 0 };

/* disassemble FROM TO: the code of one unit, one instruction per line */
static o disassemble(i32 from, i32 to)
{
	i32 pc;
	for (pc = from; pc < to; pc++) {
		const Ins *in = &g_code[pc];
		printf("%5ld  %-8s", (long)pc, g_op_names[in->op]);
		if (g_op_args[in->op] >= 1) printf(" %ld", (long)in->a);
		if (g_op_args[in->op] >= 2) printf(" %ld", (long)in->b);
		if (in->op == OP_PUSH_I) printf("\t(%ld)", (long)g_ints[in->a]);
		if (in->op == OP_PUSH_F) { g_sink = stdout; fputs("\t(", stdout); print_float(g_floats[in->a]); putchar(')'); }
		if (in->op == OP_PUSH_S) printf("\t(\"%.40s\")", g_strs[in->a].p);
		if (in->op == OP_PRINT || in->op == OP_PRINTLN || in->op == OP_FORMAT) printf("\t(\"%.40s\")", g_strs[g_fmts[in->a].str].p);
		if (in->op == OP_CALL) printf("\t(%.40s)", g_fns[in->a].name);
		if (in->op == OP_SHOW) printf("\t(%.40s)", tn((Ty)in->a));
		if (in->line) printf("\t; line %ld", (long)in->line);
		putchar('\n');
	}
}
