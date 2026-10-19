/* vm.c - the bytecode machine: one operand stack of untyped cells and a stack of call frames.
 *
 * The compiler already proved every type, so a cell carries no tag: ADD_I reads two ints, ADD_F two floats. A struct
 * or an option is a run of cells. Integer arithmetic wraps (done in u64, no undefined behaviour); dividing by zero
 * and running out of stack are runtime errors with the line they happened on. A frame needs its slots plus the
 * deepest operand stack the compiler counted for it, and the VM checks that all of it fits before it enters the
 * frame, so no instruction inside can write past the stack.
 *
 * Memory: strings made at run time, vecs and maps are objects on the heap with a reference count. The compiler emits
 * RETAIN when it copies a value out of a variable, DROP when it throws one away and RELEASE when a variable's value
 * dies; an instruction consumes the values it takes, once it is done with them, unless it only borrows a variable's.
 * An object whose count reaches 0 goes on a chain through its own count cell and is freed at once; the objects a
 * freed one held join the chain and the same loop frees them, so freeing never recurses. The heap is a buddy
 * allocator: blocks of 2^k cells, split to fit and merged with their free neighbour (buddy) when freed, so memory a
 * growing vec gave back can make one big block again. The program's own strings live in the text arena, marked by
 * bit 63, and are never counted. */

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
static Cell g_vstack[VM_STACK];
static VFrame g_vframes[VM_FRAMES];

/* The heap: blocks of 2^k cells (k from 2 to HEAP_ORDER), each starting at a multiple of its size. A block's buddy
 * is the other half of the block twice its size. Free blocks of each size are a doubly linked list through their
 * first two cells, -(next + 2) and -(prev + 2), so a freed object's count cell reads as freed (below 1). Cells 0 to
 * 3 are a block of their own that is never freed: an empty string, so a cell of 0 (a zeroed slot) reads as "" and as
 * no object */
#define HEAP_ORDER 22
#define HEAP_CELLS (1 << HEAP_ORDER)
static Cell g_heap[HEAP_CELLS];
static i64 g_free_head[HEAP_ORDER + 1];  /* the first free block of each size, -1 when none */
static u8 g_free_order[HEAP_CELLS >> 2]; /* k + 1 when a free block of 2^k cells starts at this cell * 4, else 0 */
static i64 g_live;                       /* cells handed out and not freed yet, for the leak check */
static int g_check;                      /* GAEL_CHECK=1: freed blocks get a pattern, leaks are reported */

static o free_push(i64 at, i32 k)
{
	i64 head = g_free_head[k];
	g_heap[at].i = -(head + 2);
	g_heap[at + 1].i = -1; /* no previous one */
	if (head >= 0) g_heap[head + 1].i = -(at + 2);
	g_free_head[k] = at;
	g_free_order[at >> 2] = (u8)(k + 1);
}

static o free_unlink(i64 at, i32 k)
{
	i64 next = -g_heap[at].i - 2, prev = -g_heap[at + 1].i - 2;
	if (prev >= 0) g_heap[prev].i = -(next + 2);
	else g_free_head[k] = next;
	if (next >= 0) g_heap[next + 1].i = -(prev + 2);
	g_free_order[at >> 2] = 0;
}

static i32 order_of(i64 n)
{
	i32 k = 2;
	while (((i64)1 << k) < n) k++;
	return k;
}

/* heap_alloc N: a block of at least N cells, the smallest free one split down to size; 0 for N = 0, -1 when the heap
 * has no free block that big */
static i64 heap_alloc(i64 n)
{
	i32 k, j;
	i64 at;
	if (n <= 0) return 0;
	k = order_of(n);
	for (j = k; j <= HEAP_ORDER && g_free_head[j] < 0; j++) {}
	if (j > HEAP_ORDER) return -1;
	at = g_free_head[j];
	free_unlink(at, j);
	while (j > k) { /* the upper half of each split stays free */
		j--;
		free_push(at + ((i64)1 << j), j);
	}
	g_live += (i64)1 << k;
	return at;
}

/* heap_free AT N: give back the block heap_alloc N returned, merged with its buddy while that one is free too */
static o heap_free(i64 at, i64 n)
{
	i32 k;
	if (at <= 0 || n <= 0) return;
	k = order_of(n);
	g_live -= (i64)1 << k;
	if (g_check) memset(g_heap + at, 0xa5, ((size_t)1 << k) * sizeof(Cell)); /* a stale read shows, a stale count dies */
	while (k < HEAP_ORDER) {
		i64 buddy = at ^ ((i64)1 << k);
		if (g_free_order[buddy >> 2] != k + 1) break;
		free_unlink(buddy, k);
		at &= ~((i64)1 << k);
		k++;
	}
	free_push(at, k);
}

/* heap_grow AT N: make the block heap_alloc N returned twice as big where it is, when it is the lower half of its
 * pair and the upper half (its buddy) is free; 1 when it did. Afterwards it is the block heap_alloc 2N would return */
static int heap_grow(i64 at, i64 n)
{
	i32 k;
	i64 buddy;
	if (at <= 0 || n <= 0) return 0;
	k = order_of(n);
	buddy = at + ((i64)1 << k);
	if (k >= HEAP_ORDER || (at & ((i64)1 << k)) || g_free_order[buddy >> 2] != k + 1) return 0;
	free_unlink(buddy, k);
	g_live += (i64)1 << k;
	return 1;
}

/* vm_init CHECK: the whole heap one free block, then cells 0 to 3 taken for the empty string */
static o vm_init(int check)
{
	i32 k;
	g_check = check;
	for (k = 0; k <= HEAP_ORDER; k++) g_free_head[k] = -1;
	free_push(0, HEAP_ORDER);
	(void)heap_alloc(4);
	memset(g_heap, 0, 4 * sizeof(Cell));
	g_live = 0;
}

/* every object starts with its count and its kind; the kind cell holds the type above the low 8 bits (the vec or
 * map type) or, for a string, its length */
enum { OB_RC, OB_KIND };
enum { KIND_STR = 1, KIND_VEC, KIND_MAP };

static Ty obj_type(i64 h) { return (Ty)((u64)g_heap[h + OB_KIND].i >> 8); }

/* a string value: bit 63 set - the program's own text, offset and length in the text arena; else the heap index of
 * a string object [count][length][bytes and a 0] */
#define STR_LEN(v) ((v) < 0 ? (u32)((u64)(v) & 0xffffffffu) : (u32)((u64)g_heap[(v) + OB_KIND].i >> 8))
#define STR_P(v) ((v) < 0 ? (char *)g_text.base + (((u64)(v) >> 32) & 0x7fffffffu) : (char *)(g_heap + (v) + 2))
#define STR_OFF(v) ((size_t)(((u64)(v) >> 32) & 0x7fffffffu))
static i64 str_cells(u64 len) { return 2 + (i64)((len + 8) / 8); }

static i64 g_pending;    /* objects whose count reached 0, chained through their count cells */
static int g_draining;   /* the chain is being freed: a release inside only adds to it */
static i64 g_objects[4]; /* the strings, vecs and maps alive, by kind */

#define USED_FREED "internal error: an object was used after it was freed"

static o drain(void);

static o obj_retain(i64 h)
{
	if (h <= 0) return; /* no object, or the program's own string */
	if (g_heap[h + OB_RC].i <= 0) die(USED_FREED);
	g_heap[h + OB_RC].i++;
}

static o obj_release(i64 h)
{
	if (h <= 0) return;
	if (g_heap[h + OB_RC].i <= 0) die(USED_FREED);
	if (--g_heap[h + OB_RC].i) return;
	g_heap[h + OB_RC].i = -(g_pending + 1);
	g_pending = h;
	if (!g_draining) drain();
}

/* refs V STRIDE TYPE RETAIN: every object inside a value of TYPE, whose cell C is at V + C * STRIDE, gets one owner
 * more (RETAIN) or one less. An object inside a none is not there */
static o refs(const Cell *v, i64 stride, Ty t, int retain)
{
	i32 k, j, n = g_tys[t].nleaf, first = g_tys[t].leaf_first;
	for (k = 0; k < n; k++) {
		const Leaf *lf = &g_leaves[first + k];
		for (j = 0; j < lf->nguards && v[g_guards[lf->guard_first + j] * stride].i; j++) {}
		if (j < lf->nguards) continue;
		if (retain) obj_retain(v[lf->off * stride].i);
		else obj_release(v[lf->off * stride].i);
	}
}

/* A vec: [count][kind][len][cap][data]; its elements are columns (SoA): cell C of element I sits at data + C * cap + I,
 * so one field of every element is one run of memory. With one-cell elements that is a plain array */
enum { VH_LEN = 2, VH_CAP, VH_DATA, VH_CELLS };

static Ty vec_elem(i64 v) { return g_tys[obj_type(v)].elem; }
static Cell *vec_cell(const Cell *hd, i64 i, i64 c) { return g_heap + hd[VH_DATA].i + c * hd[VH_CAP].i + i; }

static o vec_load(const Cell *hd, i64 i, i64 size, Cell *to)
{
	i64 c;
	for (c = 0; c < size; c++) to[c] = *vec_cell(hd, i, c);
}

static o vec_store(const Cell *hd, i64 i, i64 size, const Cell *from)
{
	i64 c;
	for (c = 0; c < size; c++) *vec_cell(hd, i, c) = from[c];
}

/* A map: [count][kind][entries][used][cap][ent][slots][idx]; its entries in the order they went in, each an alive
 * flag, the key and the value's cells; an index of SLOTS cells holding an entry's number + 1 (0: empty), probed
 * linearly from the key's hash. A removed entry stays in place, dead, until the map grows and packs the live ones */
enum { MH_COUNT = 2, MH_USED, MH_CAP, MH_ENT, MH_SLOTS, MH_IDX, MH_CELLS };

static i64 map_vsize(i64 m) { return cells(g_tys[obj_type(m)].elem); }
static int map_strkeys(i64 m) { return g_tys[obj_type(m)].key == T_STR; }

/* obj_free H: give back an object whose count reached 0; what it held loses an owner */
static o obj_free(i64 h)
{
	Cell *hd = g_heap + h;
	g_objects[hd[OB_KIND].i & 3]--;
	switch (hd[OB_KIND].i & 0xff) {
	case KIND_STR:
		heap_free(h, str_cells((u64)hd[OB_KIND].i >> 8));
		break;
	case KIND_VEC: {
		Ty et = vec_elem(h);
		i64 i, size = cells(et);
		for (i = 0; g_tys[et].nleaf && i < hd[VH_LEN].i; i++) refs(vec_cell(hd, i, 0), hd[VH_CAP].i, et, 0);
		heap_free(hd[VH_DATA].i, hd[VH_CAP].i * size);
		heap_free(h, VH_CELLS);
		break;
	}
	case KIND_MAP: {
		Ty vt = g_tys[obj_type(h)].elem;
		i64 e, esize = 2 + cells(vt);
		int strkeys = map_strkeys(h);
		for (e = 0; e < hd[MH_USED].i; e++) {
			const Cell *ent = g_heap + hd[MH_ENT].i + e * esize;
			if (!ent[0].i) continue;
			if (strkeys) obj_release(ent[1].i);
			refs(ent + 2, 1, vt, 0);
		}
		heap_free(hd[MH_ENT].i, hd[MH_CAP].i * esize);
		heap_free(hd[MH_IDX].i, hd[MH_SLOTS].i);
		heap_free(h, MH_CELLS);
		break;
	}
	default:
		die(USED_FREED);
	}
}

static o drain(void)
{
	g_draining = 1;
	while (g_pending) {
		i64 h = g_pending;
		g_pending = -g_heap[h + OB_RC].i - 1;
		obj_free(h);
	}
	g_draining = 0;
}

static int vm_error(i32 line, const char *msg)
{
	fflush(stdout);
	fprintf(stderr, "%s:%ld: runtime error: %s\n", g_unit.file, (long)line, msg);
	return 1;
}

#define HEAP_FULL "out of memory (the heap is full)"
#define NOT_MADE "this vec or map was never made: a global read before its def ran"
#define MADE(h) if ((h) <= 0) return vm_error(in->line, NOT_MADE)
#define TEXT_FULL "out of memory for strings (the text arena is full)"

/* Strings made at run time are first written to the free end of the text arena, then copied into a string object
 * (text_take); the arena goes back to where it was */
static int text_put(const char *p, size_t n)
{
	if (g_text.cap - g_text.used < n + 1) return 0; /* + 1: room for a 0 after it */
	memcpy(g_text.base + g_text.used, p, n);
	g_text.used += n;
	return 1;
}

static int text_room(void) { return g_text.used < g_text.cap; }

/* str_new P N: a string object with these bytes (0, the empty string kept at the start of the heap, when there are
 * none); -1 when the heap is full */
static i64 str_new(const char *p, size_t n)
{
	i64 h;
	if (!n) return 0;
	if ((h = heap_alloc(str_cells(n))) < 0) return -1;
	g_heap[h + OB_RC].i = 1;
	g_heap[h + OB_KIND].i = KIND_STR | (i64)((u64)n << 8);
	g_objects[KIND_STR]++;
	memcpy(g_heap + h + 2, p, n);
	((char *)(g_heap + h + 2))[n] = 0;
	return h;
}

/* text_take START: what was written since START as a new string; -1 when the heap is full */
static i64 text_take(size_t start)
{
	i64 h = str_new((const char *)g_text.base + start, g_text.used - start);
	g_text.used = start;
	return h;
}

/* text_const START: what was written since START, kept in the arena for good as a string of the program's own */
static i64 text_const(size_t start)
{
	size_t len = g_text.used - start;
	g_text.base[g_text.used++] = 0;
	return str_cell(start, len);
}

/* c_str STR: the string with a 0 after it, for the C library, written to the free end of the text arena; valid until
 * the next string is made, 0 when there is no room */
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

/* str_piece S FROM LEN: bytes of S as a string: a view when S is the program's own text, a copy otherwise (an object
 * of its own, so it outlives S); -1 when the heap is full */
static i64 str_piece(i64 v, i64 from, i64 len)
{
	if (v < 0) return str_cell(STR_OFF(v) + (size_t)from, (size_t)len);
	return str_new(STR_P(v) + from, (size_t)len);
}

static int str_eq(i64 a, i64 b) { return STR_LEN(a) == STR_LEN(b) && !memcmp(STR_P(a), STR_P(b), STR_LEN(a)); }

/* Printing goes to g_sink, or, when it is 0, onto the free end of the text arena: the string being built */
static FILE *g_sink;
static int g_sink_full; /* the text arena had no room for the whole string */

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
#define PRINT_SCRATCH (1 << 16)
static Cell g_print_scratch[PRINT_SCRATCH]; /* a vec's element, gathered from its columns to be printed */

/* print_value TYPE CELLS: one value, as the REPL shows it: (Vec2 :x 1.0 :y 2.0), (some 3), none, north, (vec 1 2) */
static o print_value(Ty type, const Cell *c)
{
	struct {
		Ty t;
		i32 i;
		const Cell *c;
		i32 mark; /* the scratch in use before this value was gathered */
	} st[PRINT_DEPTH];
	i32 n = 1, top = 0;
	st[0].t = type;
	st[0].i = -1;
	st[0].c = c;
	st[0].mark = 0;
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
			if (*i >= hd[VH_LEN].i) { out_s(")"); top = st[--n].mark; continue; }
			out_s(" ");
			if (n == PRINT_DEPTH || top + cells(ti->elem) > PRINT_SCRATCH) { out_s("..."); (*i)++; continue; }
			st[n].t = ti->elem;
			st[n].i = -1;
			st[n].mark = top;
			vec_load(hd, *i, cells(ti->elem), g_print_scratch + top);
			st[n].c = g_print_scratch + top;
			top += cells(ti->elem);
			(*i)++;
			n++;
			continue;
		}
		if (ti->kind == K_MAP) { /* (map (k v) ...): *i counts three steps per entry - "(", key, value */
			const Cell *hd = g_heap + v->i, *ent;
			i64 esize = 2 + cells(ti->elem), e;
			if (*i < 0) {
				out_s("(map");
				*i = 0;
			}
			e = *i / 3;
			while (*i % 3 == 0 && e < hd[MH_USED].i && !g_heap[hd[MH_ENT].i + e * esize].i) *i = (i32)(++e * 3);
			if (e >= hd[MH_USED].i) { out_s(")"); top = st[--n].mark; continue; }
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
			st[n].mark = top;
			st[n].c = *i % 3 == 1 ? ent + 1 : ent + 2;
			n++;
			continue;
		}
		if (ti->kind == K_STRUCT || ti->kind == K_OPTION) {
			const Field *fd = 0;
			if (*i < 0) {
				if (ti->kind == K_OPTION && !v->i) { out_s("none"); top = st[--n].mark; continue; }
				out_s("(");
				out_s(ti->kind == K_OPTION ? "some" : ti->name);
				*i = 0;
			}
			if (ti->kind == K_STRUCT && *i < ti->n) fd = &g_fields[ti->first + *i];
			if (ti->kind == K_OPTION && *i < 1) out_s(" ");
			else if (!fd) { out_s(")"); top = st[--n].mark; continue; }
			else { out_s(" :"); out_s(fd->name); out_s(" "); }
			(*i)++;
			if (n == PRINT_DEPTH) { out_s("..."); continue; }
			st[n].t = fd ? fd->type : ti->elem;
			st[n].i = -1;
			st[n].mark = top;
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
		top = st[--n].mark;
	}
}

/* print_fmt FORMAT ARGS: a print, println or format; the compiler checked every % code against its value */
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

/* release_args FORMAT ARGS: the printed values are consumed */
static o release_args(const Fmt *fm, const Cell *args)
{
	i32 k;
	for (k = 0; k < fm->n; k++) {
		refs(args, 1, g_fmt_tys[fm->first + k], 0);
		args += cells(g_fmt_tys[fm->first + k]);
	}
}

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

static const char *g_fail; /* why the last helper below failed */

/* run_cmd CMD RES: run a command (0-terminated) and return its standard output as a new string, the newlines at its
 * end left out; 0 when there was no room (the rest of the output is still read, so the command can finish). CMD may
 * sit at the free end of the text arena: popen has passed it to the child before the output lands on it */
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
	g_fail = TEXT_FULL;
	if (full) {
		g_text.used = start;
		return 0;
	}
	while (g_text.used > start && g_text.base[g_text.used - 1] == '\n') g_text.used--;
	g_fail = HEAP_FULL;
	return (*res = text_take(start)) >= 0;
}

/* run_pipe CMDS N INPUT RES: the commands joined into one pipeline, each in its own subshell; with INPUT the first
 * string goes to the first command's stdin through a temporary file */
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
		g_fail = "<<<: no temporary file for the text";
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
		g_fail = TEXT_FULL;
	}
	if (input) unlink(tmp);
	return ok;
}

/* read_into FILE RES: everything left in FILE as a new string */
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
	g_fail = TEXT_FULL;
	if (!ok) {
		g_text.used = start;
		return 0;
	}
	g_fail = HEAP_FULL;
	return (*res = text_take(start)) >= 0;
}

/* vec_new TYPE N FROM: a vec of type TYPE holding the N elements at FROM (moved in), or N zeros without FROM; -1 when
 * the heap is full */
static i64 vec_new(Ty vt, i64 n, const Cell *from)
{
	i64 size = cells(g_tys[vt].elem), cap = n < 4 ? 4 : n, h = heap_alloc(VH_CELLS), data = heap_alloc(cap * size), i;
	if (h < 0 || data < 0) return -1;
	g_heap[h + OB_RC].i = 1;
	g_heap[h + OB_KIND].i = KIND_VEC | (i64)((u64)vt << 8);
	g_objects[KIND_VEC]++;
	g_heap[h + VH_LEN].i = n;
	g_heap[h + VH_CAP].i = cap;
	g_heap[h + VH_DATA].i = data;
	if (data) memset(g_heap + data, 0, (size_t)(cap * size) * sizeof(Cell));
	for (i = 0; from && i < n; i++) vec_store(g_heap + h, i, size, from + i * size); /* the stack has them in a row */
	return h;
}

/* vec_push V FROM: move one element in at the end; 0 when the heap is full */
static int vec_push(i64 v, const Cell *from)
{
	Cell *hd = g_heap + v;
	i64 size = cells(vec_elem(v));
	if (hd[VH_LEN].i == hd[VH_CAP].i) { /* twice the room; each column moves to its place in it */
		i64 old = hd[VH_CAP].i, cap = old * 2, data = hd[VH_DATA].i, c;
		if (heap_grow(data, old * size)) { /* in place: the last column first, each moves up */
			for (c = size - 1; c > 0; c--)
				memmove(g_heap + data + c * cap, g_heap + data + c * old, (size_t)hd[VH_LEN].i * sizeof(Cell));
		} else {
			if ((data = heap_alloc(cap * size)) < 0) return 0;
			for (c = 0; c < size; c++)
				memcpy(g_heap + data + c * cap, vec_cell(hd, 0, c), (size_t)hd[VH_LEN].i * sizeof(Cell));
			heap_free(hd[VH_DATA].i, old * size);
		}
		hd[VH_DATA].i = data;
		hd[VH_CAP].i = cap;
	}
	vec_store(hd, hd[VH_LEN].i, size, from);
	hd[VH_LEN].i++;
	return 1;
}

static u64 hash_key(i64 key, int is_str)
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

/* map_new TYPE: an empty map of this map type; -1 when the heap is full */
static i64 map_new(Ty mt)
{
	i64 esize = 2 + cells(g_tys[mt].elem), h = heap_alloc(MH_CELLS), ent = heap_alloc(8 * esize), idx = heap_alloc(16);
	if (h < 0 || ent < 0 || idx < 0) return -1;
	g_heap[h + OB_RC].i = 1;
	g_heap[h + OB_KIND].i = KIND_MAP | (i64)((u64)mt << 8);
	g_objects[KIND_MAP]++;
	g_heap[h + MH_COUNT].i = 0;
	g_heap[h + MH_USED].i = 0;
	g_heap[h + MH_CAP].i = 8;
	g_heap[h + MH_ENT].i = ent;
	g_heap[h + MH_SLOTS].i = 16;
	g_heap[h + MH_IDX].i = idx;
	memset(g_heap + idx, 0, 16 * sizeof(Cell));
	return h;
}

/* map_find M KEY SLOT: the entry number of KEY, or -1; *SLOT gets the index slot where it is or would go */
static i64 map_find(i64 m, i64 key, i64 *slot)
{
	const Cell *hd = g_heap + m;
	int strkeys = map_strkeys(m);
	i64 mask = hd[MH_SLOTS].i - 1, esize = 2 + map_vsize(m), at = (i64)(hash_key(key, strkeys) & (u64)mask);
	for (;; at = (at + 1) & mask) {
		i64 e = g_heap[hd[MH_IDX].i + at].i;
		const Cell *ent;
		if (!e) {
			*slot = at;
			return -1;
		}
		ent = g_heap + hd[MH_ENT].i + (e - 1) * esize;
		if (ent[0].i && (strkeys ? str_eq(ent[1].i, key) : ent[1].i == key)) {
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
	i64 esize = 2 + map_vsize(m), cap = hd[MH_CAP].i * 2, slots = hd[MH_SLOTS].i * 2, ent, idx, e, k = 0, slot;
	int idx_here = heap_grow(hd[MH_IDX].i, hd[MH_SLOTS].i), ent_here; /* each grows in place when it can */
	idx = idx_here ? hd[MH_IDX].i : heap_alloc(slots);
	if (idx < 0) return 0;
	ent_here = heap_grow(hd[MH_ENT].i, hd[MH_CAP].i * esize);
	ent = ent_here ? hd[MH_ENT].i : heap_alloc(cap * esize);
	if (ent < 0) { /* the map stays as it was */
		if (idx_here) heap_free(idx + hd[MH_SLOTS].i, hd[MH_SLOTS].i);
		else heap_free(idx, slots);
		return 0;
	}
	for (e = 0; e < hd[MH_USED].i; e++) { /* the live entries, packed; in place they only move down */
		const Cell *from = g_heap + hd[MH_ENT].i + e * esize;
		if (from[0].i) memmove(g_heap + ent + k++ * esize, from, (size_t)esize * sizeof(Cell));
	}
	memset(g_heap + idx, 0, (size_t)slots * sizeof(Cell));
	if (!ent_here) heap_free(hd[MH_ENT].i, hd[MH_CAP].i * esize);
	if (!idx_here) heap_free(hd[MH_IDX].i, hd[MH_SLOTS].i);
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

/* map_insert M KEY VALUE: move the entry in, or give an existing key the new value (the old value and the key just
 * passed lose an owner); 0 when the heap is full */
static int map_insert(i64 m, i64 key, const Cell *val)
{
	Cell *hd = g_heap + m, *ent;
	Ty vt = g_tys[obj_type(m)].elem;
	i64 vsize = cells(vt), esize = 2 + vsize, slot, e = map_find(m, key, &slot);
	if (e >= 0) {
		ent = g_heap + hd[MH_ENT].i + e * esize;
		refs(ent + 2, 1, vt, 0);
		memcpy(ent + 2, val, (size_t)vsize * sizeof(Cell));
		if (map_strkeys(m)) obj_release(key);
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

/* range_error LINE FROM TO LEN: substr's bytes FROM up to TO do not fit a string of LEN, or TO is before FROM */
static int range_error(i32 line, i64 from, i64 to, i64 len)
{
	char msg[160];
	sprintf(msg, "substr %ld %ld does not fit a string of %ld bytes", (long)from, (long)to, (long)len);
	return vm_error(line, msg);
}

/* the command line after the file name, kept in the text arena as the program's own strings */
#define MAX_ARGS 4096
static i64 g_arg_cells[MAX_ARGS];
static i64 g_nargs;

static o args_init(int argc, char **argv)
{
	int i;
	for (i = 0; i < argc && i < MAX_ARGS; i++) {
		size_t start = g_text.used;
		if (!text_put(argv[i], strlen(argv[i]))) die("arguments too long for the text arena");
		g_arg_cells[g_nargs++] = text_const(start);
	}
}

/* vm_run PC: run from PC until HALT; 0 when it finished, 1 after a runtime error */
static int vm_run(i32 pc)
{
	i32 sp = 0, fp = 0, nf = 0;
	Cell *s = g_vstack;
	const Ins *in = 0;
	for (;;) {
		in = &g_code[pc++];
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
		case OP_RETAIN: refs(s + sp - cells((Ty)in->a), 1, (Ty)in->a, 1); break;
		case OP_DROP: sp -= cells((Ty)in->a); refs(s + sp, 1, (Ty)in->a, 0); break;
		case OP_RELEASE_L: refs(s + fp + in->a, 1, (Ty)in->b, 0); break;
		case OP_RELEASE_G: refs(g_global_cells + in->a, 1, (Ty)in->b, 0); break;

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
		case OP_EQ_S:
		case OP_NE_S: { /* a: bit 0 the first string is borrowed, bit 1 the second */
			i64 a = s[sp - 2].i, b = s[sp - 1].i;
			int eq = str_eq(a, b);
			if (!(in->a & 1)) obj_release(a);
			if (!(in->a & 2)) obj_release(b);
			sp--;
			s[sp - 1].i = in->op == OP_EQ_S ? eq : !eq;
			break;
		}
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
		case OP_LEN_S: {
			i64 v = s[sp - 1].i;
			s[sp - 1].i = (i64)STR_LEN(v);
			if (!in->a) obj_release(v);
			break;
		}

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
			release_args(&g_fmts[in->a], s + sp);
			break;
		case OP_FORMAT: {
			size_t start = g_text.used;
			i64 h;
			sp -= g_fmts[in->a].cells;
			g_sink = 0;
			g_sink_full = !text_room();
			print_fmt(&g_fmts[in->a], s + sp);
			if (g_sink_full) return vm_error(in->line, TEXT_FULL);
			release_args(&g_fmts[in->a], s + sp);
			if ((h = text_take(start)) < 0) return vm_error(in->line, HEAP_FULL);
			s[sp++].i = h;
			break;
		}
		case OP_SH: {
			i64 cmd = s[sp - 1].i, res;
			if (!run_cmd(c_str(cmd), &res)) return vm_error(in->line, g_fail);
			obj_release(cmd);
			s[sp - 1].i = res;
			break;
		}
		case OP_SHPIPE: {
			i64 res, k;
			sp -= in->a;
			if (!run_pipe(s + sp, in->a, in->b, &res)) return vm_error(in->line, g_fail);
			for (k = 0; k < in->a; k++) obj_release(s[sp + k].i);
			s[sp++].i = res;
			break;
		}
		case OP_PID: s[sp++].i = (i64)getpid(); break;
		case OP_ENV: {
			i64 name = s[sp - 1].i, h;
			const char *c = c_str(name), *val;
			if (!c) return vm_error(in->line, TEXT_FULL);
			val = getenv(c);
			if (!val) val = "";
			if ((h = str_new(val, strlen(val))) < 0) return vm_error(in->line, HEAP_FULL);
			obj_release(name);
			s[sp - 1].i = h;
			break;
		}
		case OP_SYSTEM: {
			i64 cmd = s[sp - 1].i;
			const char *c = c_str(cmd);
			int st;
			if (!c) return vm_error(in->line, TEXT_FULL);
			fflush(stdout);
			st = system(c);
			obj_release(cmd);
			s[sp - 1].i = st == -1 ? -1 : (st >> 8) & 0xff; /* the exit status as the shell reports it (Linux) */
			break;
		}
		case OP_PARSE_I: {
			i64 v = 0, str = s[sp - 1].i;
			int ok = parse_int(str, &v);
			obj_release(str);
			s[sp - 1].i = ok;
			s[sp++].i = v;
			break;
		}
		case OP_PARSE_F: {
			i64 str = s[sp - 1].i;
			f64 v = 0;
			int ok = parse_float(str, &v);
			obj_release(str);
			s[sp - 1].i = ok;
			s[sp++].f = ok ? v : 0;
			break;
		}
		case OP_SUBSTR: {
			i64 v = s[sp - 3].i, from = s[sp - 2].i, to = s[sp - 1].i, h;
			if (from < 0 || from > to || to > STR_LEN(v)) return range_error(in->line, from, to, STR_LEN(v));
			if ((h = str_piece(v, from, to - from)) == -1) return vm_error(in->line, HEAP_FULL);
			obj_release(v);
			sp -= 2;
			s[sp - 1].i = h;
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
			if (!in->a) obj_release(v);
			break;
		}
		case OP_FIND: { /* the two strings become the option's two cells */
			i64 a = s[sp - 2].i, b = s[sp - 1].i, at = str_find(a, b, 0);
			obj_release(a);
			obj_release(b);
			s[sp - 2].i = at >= 0;
			s[sp - 1].i = at >= 0 ? at : 0;
			break;
		}
		case OP_SPLIT: {
			i64 v = s[sp - 2].i, sep = s[sp - 1].i, n = 1, at, start = 0, h, piece;
			Cell *data;
			if (!STR_LEN(sep)) return vm_error(in->line, "split: the separator is empty");
			for (at = str_find(v, sep, 0); at >= 0; at = str_find(v, sep, at + STR_LEN(sep))) n++;
			if ((h = vec_new(g_vec_str, n, 0)) < 0) return vm_error(in->line, HEAP_FULL);
			data = g_heap + g_heap[h + VH_DATA].i;
			for (n = 0;; start = at + STR_LEN(sep)) {
				at = str_find(v, sep, start);
				if ((piece = str_piece(v, start, (at >= 0 ? at : STR_LEN(v)) - start)) == -1) return vm_error(in->line, HEAP_FULL);
				data[n++].i = piece;
				if (at < 0) break;
			}
			obj_release(v);
			obj_release(sep);
			sp--;
			s[sp - 1].i = h;
			break;
		}
		case OP_JOIN: {
			i64 v = s[sp - 2].i, sep = s[sp - 1].i, k, h;
			const Cell *hd = g_heap + v;
			size_t start = g_text.used;
			int ok = text_room();
			MADE(v);
			for (k = 0; ok && k < hd[VH_LEN].i; k++) {
				i64 e = vec_cell(hd, k, 0)->i;
				if (k) ok = text_put(STR_P(sep), STR_LEN(sep));
				if (ok) ok = text_put(STR_P(e), STR_LEN(e));
			}
			if (!ok) {
				g_text.used = start;
				return vm_error(in->line, TEXT_FULL);
			}
			if ((h = text_take(start)) < 0) return vm_error(in->line, HEAP_FULL);
			obj_release(v);
			obj_release(sep);
			sp--;
			s[sp - 1].i = h;
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
			i64 v = s[sp - 1].i, h;
			const char *p;
			u32 n;
			trimmed(v, &p, &n);
			if ((h = str_piece(v, p - STR_P(v), n)) == -1) return vm_error(in->line, HEAP_FULL);
			obj_release(v);
			s[sp - 1].i = h;
			break;
		}
		case OP_READ_FILE: { /* the path's cell becomes the option's two: tag, string */
			i64 pathv = s[sp - 1].i, v = 0;
			const char *path = c_str(pathv);
			FILE *file = path ? fopen(path, "rb") : 0;
			if (!path) return vm_error(in->line, TEXT_FULL);
			if (file) {
				int ok = read_into(file, &v);
				fclose(file);
				if (!ok) return vm_error(in->line, g_fail);
			}
			obj_release(pathv);
			s[sp - 1].i = file != 0;
			s[sp++].i = v;
			break;
		}
		case OP_WRITE_FILE: { /* a: append, b: the text is below the path */
			i64 text = s[sp - (in->b ? 2 : 1)].i, pathv = s[sp - (in->b ? 1 : 2)].i;
			const char *path = c_str(pathv);
			FILE *file = path ? fopen(path, in->a ? "ab" : "wb") : 0;
			int ok = file && fwrite(STR_P(text), 1, STR_LEN(text), file) == STR_LEN(text);
			if (file && fclose(file)) ok = 0;
			if (!path) return vm_error(in->line, TEXT_FULL);
			obj_release(text);
			obj_release(pathv);
			sp--;
			s[sp - 1].i = ok;
			break;
		}
		case OP_READ_LINE: {
			size_t start = g_text.used;
			int c = EOF, any = 0;
			i64 h;
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
			if (!any) {
				g_text.used = start;
				s[sp++].i = 0;
				s[sp++].i = 0;
				break;
			}
			if ((h = text_take(start)) < 0) return vm_error(in->line, HEAP_FULL);
			s[sp++].i = 1;
			s[sp++].i = h;
			break;
		}
		case OP_GETENV: {
			i64 name = s[sp - 1].i, h = 0;
			const char *c = c_str(name), *val;
			if (!c) return vm_error(in->line, TEXT_FULL);
			val = getenv(c);
			if (val && (h = str_new(val, strlen(val))) < 0) return vm_error(in->line, HEAP_FULL);
			obj_release(name);
			s[sp - 1].i = val != 0;
			s[sp++].i = h;
			break;
		}
		case OP_ARGS: {
			i64 h = vec_new(g_vec_str, g_nargs, 0), k;
			if (h < 0) return vm_error(in->line, HEAP_FULL);
			for (k = 0; k < g_nargs; k++) vec_cell(g_heap + h, k, 0)->i = g_arg_cells[k];
			s[sp++].i = h;
			break;
		}
		case OP_EXIT:
			fflush(stdout);
			exit((int)(s[sp - 1].i & 0xff));
		case OP_INC_L: s[fp + in->a].i = (i64)((u64)s[fp + in->a].i + 1); break;
		case OP_VEC_NEW: { /* a: elements on the stack, b: the vec type */
			i64 h;
			sp -= in->a * cells(g_tys[in->b].elem);
			if ((h = vec_new((Ty)in->b, in->a, s + sp)) < 0) return vm_error(in->line, HEAP_FULL);
			s[sp++].i = h;
			break;
		}
		case OP_VEC_LEN: {
			i64 v = s[sp - 1].i;
			MADE(v);
			s[sp - 1].i = g_heap[v + VH_LEN].i;
			if (!in->a) obj_release(v);
			break;
		}
		case OP_VEC_PUSH:
			sp -= in->a + 1;
			MADE(s[sp].i);
			if (!vec_push(s[sp].i, s + sp + 1)) return vm_error(in->line, HEAP_FULL);
			if (!in->b) obj_release(s[sp].i);
			break;
		case OP_VEC_GET: { /* a copy of the element: the vec keeps its own */
			i64 v = s[sp - 2].i, i = s[sp - 1].i;
			const Cell *hd = g_heap + v;
			MADE(v);
			if (i < 0 || i >= hd[VH_LEN].i) return index_error(in->line, i, hd[VH_LEN].i);
			sp -= 2;
			vec_load(hd, i, in->a, s + sp);
			refs(s + sp, 1, vec_elem(v), 1);
			sp += in->a;
			if (!in->b) obj_release(v);
			break;
		}
		case OP_VEC_PUT: {
			const Cell *hd;
			i64 i, v;
			sp -= in->a + 2;
			v = s[sp].i;
			hd = g_heap + v;
			i = s[sp + 1].i;
			MADE(v);
			if (i < 0 || i >= hd[VH_LEN].i) return index_error(in->line, i, hd[VH_LEN].i);
			refs(vec_cell(hd, i, 0), hd[VH_CAP].i, vec_elem(v), 0); /* the element it replaces */
			vec_store(hd, i, in->a, s + sp + 2);
			if (!in->b) obj_release(v);
			break;
		}
		case OP_VEC_POP: { /* the last element moves out */
			i64 v = s[sp - 1].i;
			Cell *hd = g_heap + v;
			MADE(v);
			if (!hd[VH_LEN].i) { /* none */
				s[sp - 1].i = 0;
				memset(s + sp, 0, (size_t)in->a * sizeof(Cell));
			} else {
				hd[VH_LEN].i--;
				s[sp - 1].i = 1;
				vec_load(hd, hd[VH_LEN].i, in->a, s + sp);
			}
			sp += in->a;
			if (!in->b) obj_release(v);
			break;
		}
		case OP_MAP_NEW: { /* a: entries on the stack, b: the map type */
			i64 vsize = cells(g_tys[in->b].elem), k, m;
			sp -= in->a * (1 + vsize);
			if ((m = map_new((Ty)in->b)) < 0) return vm_error(in->line, HEAP_FULL);
			for (k = 0; k < in->a; k++) {
				const Cell *e = s + sp + k * (1 + vsize);
				if (!map_insert(m, e[0].i, e + 1)) return vm_error(in->line, HEAP_FULL);
			}
			s[sp++].i = m;
			break;
		}
		case OP_MAP_INSERT:
			sp -= in->a + 2;
			MADE(s[sp].i);
			if (!map_insert(s[sp].i, s[sp + 1].i, s + sp + 2)) return vm_error(in->line, HEAP_FULL);
			if (!in->b) obj_release(s[sp].i);
			break;
		case OP_MAP_LOOKUP:
		case OP_MAP_REMOVE: { /* the map and the key become the option: tag, value */
			i64 m = s[sp - 2].i, key = s[sp - 1].i, slot, e;
			int strkeys = map_strkeys(m);
			MADE(m);
			e = map_find(m, key, &slot);
			sp -= 2;
			s[sp].i = e >= 0;
			if (e >= 0) {
				Cell *ent = g_heap + g_heap[m + MH_ENT].i + e * (2 + in->a);
				memcpy(s + sp + 1, ent + 2, (size_t)in->a * sizeof(Cell));
				if (in->op == OP_MAP_LOOKUP) {
					refs(s + sp + 1, 1, g_tys[obj_type(m)].elem, 1); /* a copy: the map keeps its own */
				} else { /* the value moves out, the entry dies with its key */
					ent[0].i = 0;
					g_heap[m + MH_COUNT].i--;
					if (strkeys) obj_release(ent[1].i);
				}
			} else {
				memset(s + sp + 1, 0, (size_t)in->a * sizeof(Cell));
			}
			if (strkeys && !(in->b & 2)) obj_release(key);
			if (!(in->b & 1)) obj_release(m);
			sp += 1 + in->a;
			break;
		}
		case OP_MAP_HAS: {
			i64 m = s[sp - 2].i, key = s[sp - 1].i, slot, found;
			MADE(m);
			found = map_find(m, key, &slot) >= 0;
			if (map_strkeys(m) && !(in->a & 2)) obj_release(key);
			if (!(in->a & 1)) obj_release(m);
			sp--;
			s[sp - 1].i = found;
			break;
		}
		case OP_MAP_LEN: {
			i64 m = s[sp - 1].i;
			MADE(m);
			s[sp - 1].i = g_heap[m + MH_COUNT].i;
			if (!in->a) obj_release(m);
			break;
		}
		case OP_MAP_KEYS:
		case OP_MAP_VALUES: { /* a: the vec type made; its elements are copies */
			i64 m = s[sp - 1].i, e, k = 0, v, size = cells(g_tys[in->a].elem), esize = 2 + map_vsize(m);
			MADE(m);
			if ((v = vec_new((Ty)in->a, g_heap[m + MH_COUNT].i, 0)) < 0) return vm_error(in->line, HEAP_FULL);
			for (e = 0; e < g_heap[m + MH_USED].i; e++) {
				const Cell *ent = g_heap + g_heap[m + MH_ENT].i + e * esize;
				if (!ent[0].i) continue;
				refs(in->op == OP_MAP_KEYS ? ent + 1 : ent + 2, 1, g_tys[in->a].elem, 1);
				vec_store(g_heap + v, k++, size, in->op == OP_MAP_KEYS ? ent + 1 : ent + 2);
			}
			if (!in->b) obj_release(m);
			s[sp - 1].i = v;
			break;
		}
		case OP_SHOW:
			sp -= cells((Ty)in->a);
			g_sink = stdout;
			fputs("=> ", stdout);
			print_value((Ty)in->a, s + sp);
			printf(" : %s\n", tn((Ty)in->a));
			refs(s + sp, 1, (Ty)in->a, 0);
			break;
		default:
			return vm_error(in->line, "internal: unknown instruction");
		}
	}
}

/* leak_check: free what the globals hold; 0 when that leaves nothing on the heap */
static i64 leak_check(void)
{
	i32 i;
	for (i = 0; i < g_nglobals; i++) refs(g_global_cells + g_globals[i].cell, 1, g_globals[i].type, 0);
	drain();
	return g_live;
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
		if (in->op == OP_SHOW || in->op == OP_RETAIN || in->op == OP_DROP || in->op == OP_MAP_KEYS || in->op == OP_MAP_VALUES)
			printf("\t(%.40s)", tn((Ty)in->a));
		if (in->op == OP_RELEASE_L || in->op == OP_RELEASE_G || in->op == OP_VEC_NEW || in->op == OP_MAP_NEW)
			printf("\t(%.40s)", tn((Ty)in->b));
		if (in->line) printf("\t; line %ld", (long)in->line);
		putchar('\n');
	}
}
