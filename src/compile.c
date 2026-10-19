/* compile.c - type checking and bytecode generation in one pass over the tree, without recursion.
 *
 * A work stack of frames says which node is being compiled and how far it got (phase). A finished expression leaves
 * exactly one type on the type stack, and code that leaves its value on the VM stack (no value for unit). An error
 * is reported with its line and column and the expression gets the type ERR, which agrees with everything: one
 * mistake gives one message, compilation goes on, and every error of a unit comes out in one run.
 *
 * A value takes as many VM cells as its type says: one for a number, a bool, a string or an enum variant, the sum of
 * its fields for a struct, and one more than the value inside for an option (the tag comes first). Structs are
 * values, copied like ints; nothing is allocated. A parent tells a child which type it expects (want), so `none`
 * knows which option it is; T_UNIT as the expected type means the value is dropped. The compiler also counts how deep
 * each frame's operand stack gets, so the VM can check that a whole frame fits before entering it. */

typedef u16 Ty;
enum { T_ERR, T_UNIT, T_INT, T_FLOAT, T_BOOL, T_STR, T_NONE, T_ANY, T_USER };
typedef enum { K_PRIM, K_STRUCT, K_ENUM, K_OPTION } TyKind;

typedef struct {
	const char *name;
	u8 kind;
	Ty elem;      /* option: the type inside */
	i32 cells;    /* VM cells of one value */
	i32 first, n; /* struct: its fields g_fields[first...]; enum: its variants g_variants[first...] */
	i32 line;     /* where it was declared; 0 for the built-in and the option types */
	u8 broken;    /* its declaration had an error (already reported): uses of it stay quiet */
} TyInfo;

typedef struct {
	const char *name;
	Ty type;
	i32 offset; /* in cells, from the start of the struct */
} Field;

#define MAX_TYS 4096
#define MAX_FIELDS 16384
#define MAX_VARIANTS 16384
static TyInfo g_tys[MAX_TYS] = {
	{ "<error>", K_PRIM, 0, 0, 0, 0, 0, 0 },
	{ "unit", K_PRIM, 0, 0, 0, 0, 0, 0 },
	{ "int", K_PRIM, 0, 1, 0, 0, 0, 0 },
	{ "float", K_PRIM, 0, 1, 0, 0, 0, 0 },
	{ "bool", K_PRIM, 0, 1, 0, 0, 0, 0 },
	{ "string", K_PRIM, 0, 1, 0, 0, 0, 0 },
	{ "<none>", K_PRIM, 0, 0, 0, 0, 0, 0 },
	{ "any value", K_PRIM, 0, 0, 0, 0, 0, 0 }
};
static i32 g_ntys = T_USER;
static Field g_fields[MAX_FIELDS];
static i32 g_nfields;
static const char *g_variants[MAX_VARIANTS];
static i32 g_nvariants;

typedef struct {
	u8 op;
	i32 a, b;
	i32 line;
} Ins;

typedef struct {
	const char *p;
	u32 len;
} Str;

#define MAX_PARAMS 8
typedef struct {
	const char *name;
	u8 nparams;
	Ty ret;
	Ty params[MAX_PARAMS];
	i32 pcells; /* cells the arguments take on the stack */
	i32 entry, nslots, maxdepth, line;
	i32 decl_unit, decl_node; /* the defn that declared it, so only that one compiles a body into it */
} Fn;

typedef struct {
	const char *name;
	Ty type;
	i32 cell; /* the first of its cells in the global area */
} Global;

typedef struct {
	const char *name;
	Ty type;
	i32 slot; /* the first of its slots in the frame */
} Local;

/* a print's format: the string and the type of each value, so the VM can print values of any size */
typedef struct {
	i32 str, first, n, cells;
} Fmt;

/* what lives as long as the process: code, constants, strings, functions, globals */
#define MAX_CODE (1 << 18)
#define MAX_CONSTS (1 << 16)
#define MAX_FNS 4096
#define MAX_GLOBALS 4096
#define MAX_GLOBAL_CELLS (1 << 16)
#define MAX_FMTS (1 << 14)
static Ins g_code[MAX_CODE];
static i32 g_ncode;
static i64 g_ints[MAX_CONSTS];
static i32 g_nints;
static f64 g_floats[MAX_CONSTS];
static i32 g_nfloats;
static Str g_strs[MAX_CONSTS];
static i32 g_nstrs;
static Fn g_fns[MAX_FNS];
static i32 g_nfns;
static Global g_globals[MAX_GLOBALS];
static i32 g_nglobals, g_nglobal_cells;
static Fmt g_fmts[MAX_FMTS];
static i32 g_nfmts;
static Ty g_fmt_tys[MAX_CONSTS];
static i32 g_nfmt_tys;
static u64 g_perm_mem[(8u << 20) / 8];
static Arena g_perm = { (u8 *)g_perm_mem, sizeof(g_perm_mem), 0, "permanent" };

/* what one compilation needs */
#define MAX_LOCALS 1024
#define MAX_WORK 4096
static Local g_locals[MAX_LOCALS];
static i32 g_nlocals, g_local_floor; /* names below the floor belong to an outer frame and are not visible */
static i32 g_slots, g_maxslots;
static i32 g_depth, g_maxdepth;      /* the operand stack of the frame being compiled, in cells */
static Ty g_types[MAX_WORK];
static i32 g_ntypes;
static i32 g_patches[MAX_WORK];
static i32 g_npatches;
static u8 g_seen[MAX_VARIANTS];      /* match: the variants an arm already covers */

typedef enum {
	F_START, F_CALL, F_IF, F_WHEN, F_WHILE, F_DO, F_LET, F_SET, F_AND, F_OR, F_NOT, F_ARITH, F_CMP, F_PRINT,
	F_CONV, F_LEN, F_SH, F_NEW, F_SOME, F_MATCH
} Form;

typedef struct {
	i32 node;    /* the form being compiled; -1 for a body (a run of sibling forms) */
	u8 form, phase;
	u8 op;       /* the operator, which comparison, which builtin; set: a global; new: named fields; match: covered */
	u8 keep;     /* bodies: 1 keeps the value of the last form */
	Ty rt;       /* running type, a binding's declared type, the matched type */
	Ty want;     /* the type the parent expects; T_NONE when it does not know, T_UNIT when it drops the value */
	Ty res;      /* match: the type of the arms */
	i32 cur;     /* the child being compiled */
	i32 k;       /* argument number */
	i32 a, b, c; /* code positions to patch, a function, type, slot or format, a binding's name */
	i32 mark;    /* scope or patch-stack height to go back to */
	i32 mark2;   /* slot count to go back to */
	i32 depth;   /* operand stack depth to go back to at a branch */
} Frame;

static Frame g_work[MAX_WORK];
static i32 g_nwork;
static i32 g_unit_id; /* counts compiled units; node indices restart in each */

#define NAME_FMT "%.64s"

static o err_node(i32 n, const char *fmt, ...)
{
	char msg[900];
	va_list ap;
	va_start(ap, fmt);
	vsprintf(msg, fmt, ap);
	va_end(ap);
	error_at(g_nodes[n].line, g_nodes[n].col, "%s", msg);
}

static const char *tn(Ty t) { return t < g_ntys ? g_tys[t].name : "<error>"; }
static i32 cells(Ty t) { return t < g_ntys ? g_tys[t].cells : 0; }
static int is_struct(Ty t) { return t < g_ntys && g_tys[t].kind == K_STRUCT; }
static int is_enum(Ty t) { return t < g_ntys && g_tys[t].kind == K_ENUM; }
static int is_option(Ty t) { return t < g_ntys && g_tys[t].kind == K_OPTION; }
static int numeric(Ty t) { return t == T_INT || t == T_FLOAT; }
static int comparable(Ty t) { return numeric(t) || t == T_BOOL || t == T_STR || is_enum(t); }
static int matchable(Ty t) { return t == T_INT || t == T_BOOL || t == T_STR || is_enum(t) || is_option(t); }

static o depth_add(i32 d)
{
	g_depth += d;
	if (g_depth > g_maxdepth) g_maxdepth = g_depth;
}

/* effect OP A B: how many cells the instruction adds to the operand stack (negative: takes away) */
static i32 effect(Op op, i32 a, i32 b)
{
	switch (op) {
	case OP_PUSH_I: case OP_PUSH_F: case OP_PUSH_S: case OP_PUSH_K: case OP_LOAD_L: case OP_LOAD_G: return 1;
	case OP_ZERO: return a;
	case OP_LOADN_L: case OP_LOADN_G: return b;
	case OP_POPN: return -a;
	case OP_STOREN_L: case OP_STOREN_G: return -b;
	case OP_POP: case OP_STORE_L: case OP_STORE_G: case OP_JF: case OP_JT:
	case OP_ADD_I: case OP_SUB_I: case OP_MUL_I: case OP_DIV_I: case OP_MOD_I:
	case OP_ADD_F: case OP_SUB_F: case OP_MUL_F: case OP_DIV_F:
	case OP_LT_I: case OP_LE_I: case OP_GT_I: case OP_GE_I: case OP_EQ_I: case OP_NE_I:
	case OP_LT_F: case OP_LE_F: case OP_GT_F: case OP_GE_F: case OP_EQ_F: case OP_NE_F:
	case OP_EQ_S: case OP_NE_S: return -1;
	case OP_CALL: return cells(g_fns[a].ret) - g_fns[a].pcells;
	case OP_PRINT: case OP_PRINTLN: return -g_fmts[a].cells;
	case OP_SHOW: return -cells((Ty)a);
	default: return 0;
	}
}

static i32 emit(Op op, i32 a, i32 b, i32 line)
{
	if (g_ncode == MAX_CODE) die("program too large (MAX_CODE instructions)");
	g_code[g_ncode].op = (u8)op;
	g_code[g_ncode].a = a;
	g_code[g_ncode].b = b;
	g_code[g_ncode].line = line;
	depth_add(effect(op, a, b));
	return g_ncode++;
}

static o patch(i32 at, i32 target) { g_code[at].a = target; }

/* emit_load GLOBAL AT TYPE LINE: push the value of TYPE kept at slot (or global cell) AT */
static o emit_load(int global, i32 at, Ty t, i32 line)
{
	i32 n = cells(t);
	if (n == 1) emit(global ? OP_LOAD_G : OP_LOAD_L, at, 0, line);
	else if (n > 1) emit(global ? OP_LOADN_G : OP_LOADN_L, at, n, line);
}

static o emit_store(int global, i32 at, Ty t, i32 line)
{
	i32 n = cells(t);
	if (n == 1) emit(global ? OP_STORE_G : OP_STORE_L, at, 0, line);
	else if (n > 1) emit(global ? OP_STOREN_G : OP_STOREN_L, at, n, line);
}

static o emit_pop(Ty t, i32 line)
{
	i32 n = cells(t);
	if (n == 1) emit(OP_POP, 0, 0, line);
	else if (n > 1) emit(OP_POPN, n, 0, line);
}

static o push_type(Ty t)
{
	if (g_ntypes == MAX_WORK) die("expression nested too deep (type stack)");
	g_types[g_ntypes++] = t;
}

static Ty pop_type(void) { return g_ntypes ? g_types[--g_ntypes] : (Ty)T_ERR; }

static i32 next_of(i32 n) { return n < 0 ? -1 : g_nodes[n].next; }
static i32 first_of(i32 n) { return n < 0 ? -1 : g_nodes[n].first; }

static i32 count_from(i32 n)
{
	i32 c = 0;
	for (; n >= 0; n = g_nodes[n].next) c++;
	return c;
}

static int is_sym(i32 n) { return n >= 0 && g_nodes[n].kind == N_SYM; }
static int sym_is(i32 n, const char *s) { return is_sym(n) && strcmp(g_nodes[n].text, s) == 0; }
static int head_is(i32 n, const char *s) { return n >= 0 && g_nodes[n].kind == N_LIST && sym_is(g_nodes[n].first, s); }

static const char *const g_reserved[] = {
	"if", "when", "while", "do", "let", "set", "and", "or", "not", "+", "-", "*", "/", "%", "<", "<=", ">", ">=", "=",
	"!=", "print", "println", "int", "float", "bool", "string", "unit", "len", "sh", "defn", "def", "true", "false", ":",
	"struct", "enum", "match", "some", "none", "option", "_"
};

static int is_reserved(const char *s)
{
	size_t i;
	for (i = 0; i < GAEL_COUNT(g_reserved); i++) {
		if (!strcmp(s, g_reserved[i])) return 1;
	}
	return 0;
}

/* find_named_type NAME: a built-in type, a struct or an enum by name; T_NONE when there is none */
static Ty find_named_type(const char *s)
{
	i32 i;
	for (i = T_UNIT; i < g_ntys; i++) {
		if (i == T_NONE || i == T_ANY || g_tys[i].kind == K_OPTION) continue;
		if (!strcmp(g_tys[i].name, s)) return (Ty)i;
	}
	return T_NONE;
}

/* intern_option TYPE: the type (option TYPE), made once */
static Ty intern_option(Ty elem)
{
	char name[160];
	TyInfo *t;
	i32 i;
	for (i = T_USER; i < g_ntys; i++) {
		if (g_tys[i].kind == K_OPTION && g_tys[i].elem == elem) return (Ty)i;
	}
	if (g_ntys == MAX_TYS) die("too many types");
	sprintf(name, "(option %.140s)", tn(elem));
	t = &g_tys[g_ntys];
	memset(t, 0, sizeof(*t));
	t->name = arena_strndup(&g_perm, name, strlen(name));
	t->kind = K_OPTION;
	t->elem = elem;
	t->cells = 1 + cells(elem);
	return (Ty)g_ntys++;
}

/* parse_type NODE: the type a node names - int, float, bool, string, unit, a struct, an enum or (option type) - or an
 * error and ERR */
static Ty parse_type(i32 n)
{
	i32 depth = 0, k;
	Ty t;
	while (n >= 0 && g_nodes[n].kind == N_LIST && sym_is(first_of(n), "option")) {
		i32 inner = next_of(first_of(n));
		if (inner < 0 || next_of(inner) >= 0) {
			err_node(n, "an option type is (option type), like (option int)");
			return T_ERR;
		}
		depth++;
		n = inner;
	}
	if (n < 0) return T_ERR;
	if (!is_sym(n)) {
		err_node(n, "expected a type here: int, float, bool, string, unit, a struct, an enum or (option type)");
		return T_ERR;
	}
	t = find_named_type(g_nodes[n].text);
	if (t == T_NONE) {
		err_node(n, "unknown type " NAME_FMT, g_nodes[n].text);
		return T_ERR;
	}
	for (k = 0; k < depth; k++) {
		if (t == T_UNIT) {
			err_node(n, "(option unit) holds nothing; use bool");
			return T_ERR;
		}
		t = intern_option(t);
	}
	return t;
}

static i32 add_int(i64 v)
{
	if (g_nints == MAX_CONSTS) die("too many int constants");
	g_ints[g_nints] = v;
	return g_nints++;
}

static i32 add_float(f64 v)
{
	if (g_nfloats == MAX_CONSTS) die("too many float constants");
	g_floats[g_nfloats] = v;
	return g_nfloats++;
}

static i32 add_str(const char *p, u32 len)
{
	if (g_nstrs == MAX_CONSTS) die("too many strings");
	g_strs[g_nstrs].p = arena_strndup(&g_perm, p, len);
	g_strs[g_nstrs].len = len;
	return g_nstrs++;
}

static i32 find_local(const char *name)
{
	i32 i;
	for (i = g_nlocals - 1; i >= g_local_floor; i--) {
		if (!strcmp(g_locals[i].name, name)) return i;
	}
	return -1;
}

static i32 find_global(const char *name)
{
	i32 i;
	for (i = 0; i < g_nglobals; i++) {
		if (!strcmp(g_globals[i].name, name)) return i;
	}
	return -1;
}

static i32 find_fn(const char *name)
{
	i32 i;
	for (i = 0; i < g_nfns; i++) {
		if (!strcmp(g_fns[i].name, name)) return i;
	}
	return -1;
}

static const Field *find_field(Ty st, const char *name, size_t len)
{
	const TyInfo *ti = &g_tys[st];
	i32 i;
	for (i = 0; i < ti->n; i++) {
		const Field *fd = &g_fields[ti->first + i];
		if (strlen(fd->name) == len && !memcmp(fd->name, name, len)) return fd;
	}
	return 0;
}

static i32 find_variant(Ty et, const char *name, size_t len)
{
	const TyInfo *ti = &g_tys[et];
	i32 i;
	for (i = 0; i < ti->n; i++) {
		const char *v = g_variants[ti->first + i];
		if (strlen(v) == len && !memcmp(v, name, len)) return i;
	}
	return -1;
}

/* names_of TYPE BUF SIZE: "x, y" - a struct's fields or an enum's variants, for a message */
static const char *names_of(Ty t, char *buf, size_t size)
{
	const TyInfo *ti = &g_tys[t];
	size_t used = 0;
	i32 i;
	strcpy(buf, ti->n ? "" : "none");
	for (i = 0; i < ti->n; i++) {
		const char *s = ti->kind == K_STRUCT ? g_fields[ti->first + i].name : g_variants[ti->first + i];
		size_t len = strlen(s);
		if (used + len + 8 > size) {
			strcpy(buf + used, i ? ", ..." : "...");
			break;
		}
		if (i) {
			strcpy(buf + used, ", ");
			used += 2;
		}
		memcpy(buf + used, s, len + 1);
		used += len;
	}
	return buf;
}

static i32 new_slots(i32 n)
{
	i32 s = g_slots;
	g_slots += n;
	if (g_slots > g_maxslots) g_maxslots = g_slots;
	return s;
}

static o add_local(const char *name, Ty type, i32 slot)
{
	if (g_nlocals == MAX_LOCALS) die("too many local names in scope (MAX_LOCALS)");
	g_locals[g_nlocals].name = name;
	g_locals[g_nlocals].type = type;
	g_locals[g_nlocals].slot = slot;
	g_nlocals++;
}

static Frame *push_frame(i32 node, Form form)
{
	Frame *f;
	if (g_nwork == MAX_WORK) die("expression nested too deep (work stack)");
	f = &g_work[g_nwork++];
	memset(f, 0, sizeof(*f));
	f->node = node;
	f->form = (u8)form;
	f->cur = -1;
	f->rt = f->want = f->res = T_NONE;
	return f;
}

/* push_child NODE WANT: compile NODE next; WANT is the type the parent expects */
static o push_child(i32 node, Ty want) { push_frame(node, F_START)->want = want; }

/* push_body FIRST KEEP WANT: compile the forms FIRST, FIRST.next, ... as one expression; with KEEP the last one's
 * value is the result (and gets WANT), otherwise every value is dropped and the result is unit */
static o push_body(i32 first, int keep, Ty want)
{
	Frame *f = push_frame(-1, F_DO);
	f->cur = first;
	f->keep = (u8)keep;
	f->want = want;
}

static o finish(Ty t)
{
	push_type(t);
	g_nwork--;
}

static i32 line_of(Frame *f) { return f->node >= 0 ? g_nodes[f->node].line : (f->cur >= 0 ? g_nodes[f->cur].line : 0); }

static Op arith_op(u8 op, Ty t)
{
	int fl = t == T_FLOAT;
	switch (op) {
	case '+': return fl ? OP_ADD_F : OP_ADD_I;
	case '-': return fl ? OP_SUB_F : OP_SUB_I;
	case '*': return fl ? OP_MUL_F : OP_MUL_I;
	case '/': return fl ? OP_DIV_F : OP_DIV_I;
	default: return OP_MOD_I;
	}
}

static const char *const g_cmp_names[] = { "<", "<=", ">", ">=", "=", "!=" };

static Op cmp_op(u8 which, Ty t)
{
	static const Op ints[] = { OP_LT_I, OP_LE_I, OP_GT_I, OP_GE_I, OP_EQ_I, OP_NE_I };
	static const Op floats[] = { OP_LT_F, OP_LE_F, OP_GT_F, OP_GE_F, OP_EQ_F, OP_NE_F };
	if (t == T_STR) return which == 4 ? OP_EQ_S : OP_NE_S;
	return t == T_FLOAT ? floats[which] : ints[which]; /* bools and enum variants compare as ints */
}

/* fmt_type TEXT LEN K: the type the K-th % code of a format wants (T_ANY for %v); T_NONE when it has fewer codes.
 * With K = -1: returns T_ERR when the format has an unknown code, T_NONE otherwise; *count gets the number of codes */
static Ty fmt_type(const char *s, u32 n, i32 want, i32 *count)
{
	u32 i;
	i32 k = 0;
	for (i = 0; i < n; i++) {
		Ty t;
		if (s[i] != '%') continue;
		if (i + 1 == n) return want < 0 ? (Ty)T_ERR : (Ty)T_NONE;
		switch (s[++i]) {
		case '%': continue;
		case 'd': t = T_INT; break;
		case 'f': t = T_FLOAT; break;
		case 's': t = T_STR; break;
		case 'b': t = T_BOOL; break;
		case 'v': t = T_ANY; break;
		default: return want < 0 ? (Ty)T_ERR : (Ty)T_NONE;
		}
		if (k == want) return t;
		k++;
	}
	if (count) *count = k;
	return T_NONE;
}

typedef struct {
	int global;
	i32 at;
	Ty type;
} Place;

/* resolve_place NODE PLACE: where a variable or a field path like v.pos.x lives, and its type. 1 when found, 0 after
 * an error it reported, -1 when the first part is not a variable */
static int resolve_place(i32 n, Place *pl)
{
	const char *s = g_nodes[n].text, *dot = strchr(s, '.');
	size_t blen = dot ? (size_t)(dot - s) : strlen(s);
	char base[256];
	i32 i;
	if (blen >= sizeof(base)) return -1;
	memcpy(base, s, blen);
	base[blen] = 0;
	if ((i = find_local(base)) >= 0) {
		pl->global = 0;
		pl->at = g_locals[i].slot;
		pl->type = g_locals[i].type;
	} else if ((i = find_global(base)) >= 0) {
		pl->global = 1;
		pl->at = g_globals[i].cell;
		pl->type = g_globals[i].type;
	} else {
		return -1;
	}
	while (dot) {
		const char *name = dot + 1, *end = strchr(name, '.');
		size_t len = end ? (size_t)(end - name) : strlen(name);
		const Field *fd;
		char names[256];
		if (pl->type == T_ERR) return 0; /* its declaration was wrong and said so */
		if (!len) {
			err_node(n, NAME_FMT ": a field name is missing after a dot", s);
			return 0;
		}
		if (!is_struct(pl->type)) {
			err_node(n, "%.*s is %s, it has no fields", (int)((size_t)(dot - s) < 64 ? (size_t)(dot - s) : 64), s, tn(pl->type));
			return 0;
		}
		fd = find_field(pl->type, name, len);
		if (!fd && g_tys[pl->type].broken) return 0;
		if (!fd) {
			err_node(n, "%s has no field %.*s; its fields are %s", tn(pl->type), (int)(len < 64 ? len : 64), name,
			         names_of(pl->type, names, sizeof(names)));
			return 0;
		}
		pl->at += fd->offset;
		pl->type = fd->type;
		dot = end;
	}
	return 1;
}

static o compile_atom(i32 n, Ty want)
{
	Node *nd = &g_nodes[n];
	const char *s = nd->text, *dot;
	Place pl;
	Ty t;
	int r;
	switch (nd->kind) {
	case N_INT: emit(OP_PUSH_I, add_int(nd->ival), 0, nd->line); push_type(T_INT); return;
	case N_FLOAT: emit(OP_PUSH_F, add_float(nd->fval), 0, nd->line); push_type(T_FLOAT); return;
	case N_STR: emit(OP_PUSH_S, add_str(nd->text, nd->len), 0, nd->line); push_type(T_STR); return;
	case N_KEY: err_node(n, "a keyword like " NAME_FMT " names a field: (Vec2 :x 1.0 :y 2.0)", s); push_type(T_ERR); return;
	default: break;
	}
	if (!strcmp(s, "true") || !strcmp(s, "false")) {
		emit(OP_PUSH_K, s[0] == 't', 0, nd->line);
		push_type(T_BOOL);
		return;
	}
	if (!strcmp(s, "none")) {
		if (is_option(want)) {
			emit(OP_PUSH_K, 0, 0, nd->line);
			if (cells(g_tys[want].elem)) emit(OP_ZERO, cells(g_tys[want].elem), 0, nd->line);
			push_type(want);
			return;
		}
		if (want == T_NONE || want == T_UNIT)
			err_node(n, "none needs to know its option type here; give it one, like (let ((x : (option int) none)) ...)");
		else if (want != T_ERR)
			err_node(n, "none is an option, but %s is expected here", tn(want));
		push_type(T_ERR);
		return;
	}
	if (!strcmp(s, "_")) {
		err_node(n, "_ means \"anything\" only as a match pattern");
		push_type(T_ERR);
		return;
	}
	pl.global = 0;
	pl.at = 0;
	pl.type = T_ERR;
	r = resolve_place(n, &pl);
	if (r > 0) {
		emit_load(pl.global, pl.at, pl.type, nd->line);
		push_type(pl.type);
		return;
	}
	if (r == 0) {
		push_type(T_ERR);
		return;
	}
	dot = strchr(s, '.');
	if (dot && (size_t)(dot - s) < 256) { /* Dir.north */
		char base[256], names[256];
		i32 v;
		memcpy(base, s, (size_t)(dot - s));
		base[dot - s] = 0;
		t = find_named_type(base);
		if (is_enum(t) && (v = find_variant(t, dot + 1, strlen(dot + 1))) >= 0) {
			emit(OP_PUSH_K, v, 0, nd->line);
			push_type(t);
			return;
		}
		if (is_enum(t) && !g_tys[t].broken)
			err_node(n, "%s has no variant " NAME_FMT "; its variants are %s", tn(t), dot + 1, names_of(t, names, sizeof(names)));
		else if (t != T_NONE && !is_enum(t))
			err_node(n, "%s is a type, not a variable", tn(t));
		if (t != T_NONE) {
			push_type(T_ERR);
			return;
		}
	}
	t = find_named_type(s);
	if (find_fn(s) >= 0) err_node(n, NAME_FMT " is a function; call it with (" NAME_FMT " ...)", s, s);
	else if (is_struct(t)) err_node(n, NAME_FMT " is a type; make one with (" NAME_FMT " ...)", s, s);
	else if (is_enum(t) && g_tys[t].n) err_node(n, NAME_FMT " is a type; a value of it is written " NAME_FMT ".%s", s, s, g_variants[g_tys[t].first]);
	else if (t != T_NONE) err_node(n, NAME_FMT " is a type, not a value", s);
	else err_node(n, "unknown name " NAME_FMT, s);
	push_type(T_ERR);
}

/* start_list FRAME: decide which form a list is, check its shape, and start it */
static o start_list(Frame *f)
{
	i32 head = first_of(f->node), fi;
	const char *s;
	size_t k;
	Ty t;
	if (head < 0) { err_node(f->node, "empty list: a list starts with a form, an operator or a function"); finish(T_ERR); return; }
	if (!is_sym(head)) { err_node(head, "a list must start with a name: a form, an operator or a function"); finish(T_ERR); return; }
	s = g_nodes[head].text;
	f->cur = next_of(head);
	f->k = count_from(f->cur);
	if (!strcmp(s, "if")) f->form = F_IF;
	else if (!strcmp(s, "when")) f->form = F_WHEN;
	else if (!strcmp(s, "while")) f->form = F_WHILE;
	else if (!strcmp(s, "do")) { f->form = F_DO; f->keep = 1; }
	else if (!strcmp(s, "let")) f->form = F_LET;
	else if (!strcmp(s, "set")) f->form = F_SET;
	else if (!strcmp(s, "and")) f->form = F_AND;
	else if (!strcmp(s, "or")) f->form = F_OR;
	else if (!strcmp(s, "not")) f->form = F_NOT;
	else if (s[1] == 0 && strchr("+-*/%", s[0])) { f->form = F_ARITH; f->op = (u8)s[0]; }
	else if (!strcmp(s, "print") || !strcmp(s, "println")) { f->form = F_PRINT; f->op = (u8)(s[5] == 'l'); }
	else if (!strcmp(s, "int") || !strcmp(s, "float")) { f->form = F_CONV; f->op = (u8)(s[0] == 'f' ? T_FLOAT : T_INT); }
	else if (!strcmp(s, "len")) f->form = F_LEN;
	else if (!strcmp(s, "sh")) f->form = F_SH;
	else if (!strcmp(s, "some")) f->form = F_SOME;
	else if (!strcmp(s, "match")) f->form = F_MATCH;
	else if (!strcmp(s, "defn") || !strcmp(s, "def") || !strcmp(s, "struct") || !strcmp(s, "enum")) {
		err_node(head, NAME_FMT " is only allowed at the top level of a file", s);
		finish(T_ERR);
		return;
	} else if (!strcmp(s, "none")) {
		err_node(head, "none takes nothing; write it without parentheses");
		finish(T_ERR);
		return;
	} else {
		for (k = 0; k < GAEL_COUNT(g_cmp_names); k++) {
			if (!strcmp(s, g_cmp_names[k])) { f->form = F_CMP; f->op = (u8)k; break; }
		}
		if (f->form != F_START) return;
		if (is_struct(t = find_named_type(s))) {
			f->form = F_NEW;
			f->a = t;
			return;
		}
		if ((fi = find_fn(s)) < 0) {
			if (t != T_NONE) err_node(head, NAME_FMT " is a type, not a function", s);
			else if (find_local(s) >= 0 || find_global(s) >= 0) err_node(head, NAME_FMT " is a variable, not a function", s);
			else err_node(head, "unknown function " NAME_FMT, s);
			finish(T_ERR);
			return;
		}
		f->form = F_CALL;
		f->a = fi;
	}
}

/* check_named NEW FIRST TYPE: (Vec2 :y 2.0 :x 1.0) - every key a field of TYPE, given once, and no field left out */
static int check_named(i32 node, i32 first, Ty st)
{
	const TyInfo *ty = &g_tys[st];
	char names[256];
	i32 p, q, count = 0, i;
	int ok = 1;
	for (p = first; p >= 0; p = next_of(next_of(p))) {
		if (g_nodes[p].kind != N_KEY) {
			err_node(p, "%s: expected a field name here, like :%s", ty->name, ty->n ? g_fields[ty->first].name : "x");
			return 0;
		}
		if (next_of(p) < 0) {
			err_node(p, "%s: field %s has no value", ty->name, g_nodes[p].text + 1);
			return 0;
		}
		for (q = first; q != p && strcmp(g_nodes[q].text, g_nodes[p].text); q = next_of(next_of(q))) {}
		if (q != p) {
			err_node(p, "%s: field %s is given twice", ty->name, g_nodes[p].text + 1);
			ok = 0;
		} else if (!find_field(st, g_nodes[p].text + 1, g_nodes[p].len - 1)) {
			err_node(p, "%s has no field %s; its fields are %s", ty->name, g_nodes[p].text + 1, names_of(st, names, sizeof(names)));
			ok = 0;
		} else {
			count++;
		}
	}
	if (ok && count < ty->n) {
		size_t used = 0;
		names[0] = 0;
		for (i = 0; i < ty->n; i++) {
			const char *fname = g_fields[ty->first + i].name;
			for (p = first; p >= 0 && strcmp(g_nodes[p].text + 1, fname); p = next_of(next_of(p))) {}
			if (p >= 0) continue;
			if (used + strlen(fname) + 8 > sizeof(names)) { strcpy(names + used, ", ..."); break; }
			sprintf(names + used, "%s%s", used ? ", " : "", fname);
			used = strlen(names);
		}
		err_node(node, "%s: missing field%s %s", ty->name, ty->n - count == 1 ? "" : "s", names);
		ok = 0;
	}
	return ok;
}

static const Field *field_of_key(Ty st, i32 key) { return find_field(st, g_nodes[key].text + 1, g_nodes[key].len - 1); }

/* pattern_variant ENUM NODE: the variant a match pattern names, bare (north) or with its type (Dir.north); -1 if none */
static i32 pattern_variant(Ty t, i32 p)
{
	const char *s, *dot;
	if (!is_sym(p)) return -1;
	s = g_nodes[p].text;
	dot = strchr(s, '.');
	if (dot) {
		if ((size_t)(dot - s) != strlen(g_tys[t].name) || strncmp(s, g_tys[t].name, (size_t)(dot - s))) return -1;
		s = dot + 1;
	}
	return find_variant(t, s, strlen(s));
}

/* check_arms MATCH FIRST TYPE: every pattern fits TYPE, none repeats or comes after a pattern that already covers
 * it, and together they cover every value of TYPE; reports what is wrong. Returns 1 when the patterns list every
 * value one by one, so the last arm needs no test */
static int check_arms(i32 node, i32 first, Ty t)
{
	const TyInfo *ti = &g_tys[t];
	char names[256];
	i32 arm, q, covered = 0, i;
	int mask = 0, full = 0, wild = 0, bad = 0;
	if (is_enum(t)) memset(g_seen, 0, (size_t)ti->n);
	for (arm = first; arm >= 0; arm = next_of(arm)) {
		i32 p = first_of(arm);
		int bit = 0, dup = 0;
		if (g_nodes[arm].kind != N_LIST || p < 0) {
			err_node(arm, "an arm is (pattern body...)");
			bad = 1;
			continue;
		}
		if (wild || full) {
			err_node(arm, wild ? "this arm never runs: the _ arm above matches everything"
			                   : "this arm never runs: the arms above already cover every value");
			break;
		}
		if (sym_is(p, "_")) { wild = 1; continue; }
		if (t == T_BOOL) {
			if (!sym_is(p, "true") && !sym_is(p, "false")) { err_node(p, "match on bool: a pattern is true, false or _"); bad = 1; continue; }
			bit = sym_is(p, "true") ? 1 : 2;
		} else if (t == T_INT || t == T_STR) {
			u8 kind = t == T_INT ? N_INT : N_STR;
			if (g_nodes[p].kind != kind) {
				err_node(p, t == T_INT ? "match on int: a pattern is an int like 0 or 42, or _" : "match on string: a pattern is a string like \"yes\", or _");
				bad = 1;
				continue;
			}
			for (q = first; q != arm; q = next_of(q)) {
				i32 qp = first_of(q);
				if (qp < 0 || g_nodes[qp].kind != kind) continue;
				if (kind == N_INT ? g_nodes[qp].ival == g_nodes[p].ival
				                  : g_nodes[qp].len == g_nodes[p].len && !memcmp(g_nodes[qp].text, g_nodes[p].text, g_nodes[p].len)) dup = 1;
			}
		} else if (is_enum(t)) {
			i = pattern_variant(t, p);
			if (i < 0) {
				err_node(p, "match on %s: a pattern is one of %s, or _", ti->name, names_of(t, names, sizeof(names)));
				bad = 1;
				continue;
			}
			if (g_seen[i]) dup = 1;
			else { g_seen[i] = 1; full = ++covered == ti->n; }
		} else { /* option */
			i32 name = next_of(first_of(p));
			if (sym_is(p, "none")) {
				bit = 2;
			} else if (head_is(p, "some") && is_sym(name) && next_of(name) < 0) {
				const char *s = g_nodes[name].text;
				if (strcmp(s, "_") && (is_reserved(s) || strchr(s, '.'))) err_node(name, NAME_FMT " cannot be a variable name", s);
				bit = 1;
			} else {
				err_node(p, "match on %s: a pattern is (some name), none or _", ti->name);
				bad = 1;
				continue;
			}
		}
		if (bit) {
			if (mask & bit) dup = 1;
			mask |= bit;
			full = mask == 3;
		}
		if (dup) err_node(arm, "this arm never runs: an arm above already matches the same value");
	}
	if (full || wild || bad) return full;
	if (t == T_BOOL) {
		err_node(node, "match on bool does not cover %s; add an arm for it or a _ arm", mask & 1 ? "false" : "true");
	} else if (is_enum(t)) {
		size_t used = 0;
		names[0] = 0;
		for (i = 0; i < ti->n; i++) {
			if (g_seen[i]) continue;
			if (used + strlen(g_variants[ti->first + i]) + 8 > sizeof(names)) { strcpy(names + used, ", ..."); break; }
			sprintf(names + used, "%s%s", used ? ", " : "", g_variants[ti->first + i]);
			used = strlen(names);
		}
		err_node(node, "match on %s does not cover %s; add arms for %s or a _ arm", ti->name, names,
		         ti->n - covered == 1 ? "it" : "them");
	} else if (is_option(t)) {
		err_node(node, "match on %s does not cover %s; add an arm for it or a _ arm", ti->name, mask & 1 ? "none" : "(some x)");
	} else {
		err_node(node, "match on %s needs a _ arm for the values the arms do not list", ti->name);
	}
	return 0;
}

/* start_arm MATCH: test the current arm's pattern against the matched value, bind the x of (some x), compile the body */
static o start_arm(Frame *f)
{
	i32 arm = f->cur, p = first_of(arm), line = arm >= 0 ? g_nodes[arm].line : 0, last = next_of(arm) < 0;
	Ty t = f->rt, want;
	int shaped = arm >= 0 && g_nodes[arm].kind == N_LIST && p >= 0;
	g_nlocals = f->mark;
	f->b = -1;
	if (shaped && !sym_is(p, "_") && !(last && f->op)) { /* no test when the arm takes whatever is left */
		emit(OP_LOAD_L, f->a, 0, line); /* the value, or an option's tag */
		if (is_option(t)) {
			f->b = emit(sym_is(p, "none") ? OP_JT : OP_JF, -1, 0, line);
		} else if (t == T_BOOL) {
			f->b = emit(sym_is(p, "true") ? OP_JF : OP_JT, -1, 0, line);
		} else {
			if (t == T_INT) emit(OP_PUSH_I, add_int(g_nodes[p].ival), 0, line);
			else if (t == T_STR) emit(OP_PUSH_S, add_str(g_nodes[p].text, g_nodes[p].len), 0, line);
			else emit(OP_PUSH_K, pattern_variant(t, p), 0, line);
			emit(t == T_STR ? OP_EQ_S : OP_EQ_I, 0, 0, line);
			f->b = emit(OP_JF, -1, 0, line);
		}
	}
	if (shaped && is_option(t) && head_is(p, "some")) {
		i32 name = next_of(first_of(p));
		if (is_sym(name) && strcmp(g_nodes[name].text, "_")) add_local(g_nodes[name].text, g_tys[t].elem, f->a + 1);
	}
	want = f->want != T_NONE ? f->want : (f->res == T_ERR ? (Ty)T_NONE : f->res);
	f->phase = 3;
	push_body(shaped ? next_of(p) : -1, 1, want);
}

/* run_work BASE: compile until the work stack is back at BASE; leaves one type per finished root */
static o run_work(i32 base)
{
	while (g_nwork > base) {
		Frame *f = &g_work[g_nwork - 1];
		i32 line = line_of(f), i;
		Ty t, t2;

		if (f->form == F_START) {
			if (f->node < 0) { finish(T_ERR); continue; } /* a missing operand; its form reported it */
			if (g_nodes[f->node].kind != N_LIST) {
				compile_atom(f->node, f->want);
				g_nwork--;
				continue;
			}
			start_list(f);
			continue;
		}

		switch (f->form) {
		case F_DO:
			if (f->phase == 0) {
				if (f->cur < 0) { finish(T_UNIT); break; }
				f->phase = 1;
				push_child(f->cur, next_of(f->cur) < 0 && f->keep ? f->want : (Ty)T_UNIT);
				break;
			}
			t = pop_type();
			if (next_of(f->cur) < 0 && f->keep) { finish(t); break; }
			emit_pop(t, g_nodes[f->cur].line);
			if (next_of(f->cur) < 0) { finish(T_UNIT); break; }
			f->cur = next_of(f->cur);
			push_child(f->cur, next_of(f->cur) < 0 && f->keep ? f->want : (Ty)T_UNIT);
			break;

		case F_IF:
			switch (f->phase) {
			case 0:
				if (f->k < 2 || f->k > 3) {
					err_node(f->node, "if takes a condition, a then branch and an optional else branch");
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			case 1:
				t = pop_type();
				if (t != T_BOOL && t != T_ERR) err_node(f->cur, "if: the condition is %s, it must be bool", tn(t));
				f->a = emit(OP_JF, -1, 0, line);
				f->depth = g_depth;
				f->cur = next_of(f->cur);
				f->phase = 2;
				push_child(f->cur, next_of(f->cur) < 0 ? (Ty)T_UNIT : f->want); /* without else the value is dropped */
				break;
			case 2:
				if (next_of(f->cur) < 0) {
					emit_pop(pop_type(), line);
					patch(f->a, g_ncode);
					finish(T_UNIT);
					break;
				}
				if (f->want == T_UNIT) { /* the value is dropped anyway, so the branches need not agree */
					emit_pop(pop_type(), line);
					push_type(T_UNIT);
				}
				f->b = emit(OP_JMP, -1, 0, line);
				patch(f->a, g_ncode);
				g_depth = f->depth;
				f->cur = next_of(f->cur);
				f->phase = 3;
				t = g_types[g_ntypes - 1]; /* the then branch tells an else branch like none what it is */
				push_child(f->cur, f->want != T_NONE ? f->want : (t == T_ERR ? (Ty)T_NONE : t));
				break;
			default:
				t2 = pop_type();
				if (f->want == T_UNIT) {
					emit_pop(t2, line);
					t2 = T_UNIT;
				}
				t = pop_type();
				patch(f->b, g_ncode);
				if (t != t2 && t != T_ERR && t2 != T_ERR) { /* reported here, so the parent does not report it again */
					err_node(f->cur, "if: the then branch is %s and the else branch is %s; they must match", tn(t), tn(t2));
					t = T_ERR;
					t2 = T_ERR;
				}
				finish(t == T_ERR ? t2 : t);
				break;
			}
			break;

		case F_WHEN:
		case F_WHILE:
			switch (f->phase) {
			case 0:
				if (f->k < 1) {
					err_node(f->node, "%s takes a condition and a body", f->form == F_WHEN ? "when" : "while");
					finish(T_ERR);
					break;
				}
				f->a = g_ncode; /* while loops back here */
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			case 1:
				t = pop_type();
				if (t != T_BOOL && t != T_ERR)
					err_node(f->cur, "%s: the condition is %s, it must be bool", f->form == F_WHEN ? "when" : "while", tn(t));
				f->b = emit(OP_JF, -1, 0, line);
				f->phase = 2;
				push_body(next_of(f->cur), 0, T_UNIT);
				break;
			default:
				(void)pop_type();
				if (f->form == F_WHILE) emit(OP_JMP, f->a, 0, line);
				patch(f->b, g_ncode);
				finish(T_UNIT);
				break;
			}
			break;

		case F_LET:
			switch (f->phase) {
			case 0:
				if (f->cur < 0 || g_nodes[f->cur].kind != N_LIST) {
					err_node(f->node, "let needs a list of bindings: (let ((x 1) (y : float 2.0)) body)");
					finish(T_ERR);
					break;
				}
				f->mark = g_nlocals;
				f->mark2 = g_slots;
				f->b = f->cur;              /* the bindings list; the body follows it */
				f->cur = first_of(f->cur);  /* the first binding */
				f->phase = 1;
				break;
			case 1: {
				i32 name, x, init;
				if (f->cur < 0) {
					f->phase = 3;
					push_body(next_of(f->b), 1, f->want);
					break;
				}
				name = first_of(f->cur);
				if (g_nodes[f->cur].kind != N_LIST || !is_sym(name)) {
					err_node(f->cur, "a binding is (name value) or (name : type value)");
					f->cur = next_of(f->cur);
					break;
				}
				if (is_reserved(g_nodes[name].text) || strchr(g_nodes[name].text, '.'))
					err_node(name, NAME_FMT " cannot be a variable name", g_nodes[name].text);
				x = next_of(name);
				f->rt = T_NONE;
				if (sym_is(x, ":")) {
					f->rt = parse_type(next_of(x));
					init = next_of(next_of(x));
				} else {
					init = x;
				}
				if (init < 0) {
					err_node(name, NAME_FMT " has no value", g_nodes[name].text);
					f->cur = next_of(f->cur);
					break;
				}
				if (next_of(init) >= 0) err_node(next_of(init), "a binding has one value");
				f->a = name;
				f->phase = 2;
				push_child(init, f->rt);
				break;
			}
			case 2: {
				const char *name = g_nodes[f->a].text;
				Ty type;
				t = pop_type();
				type = f->rt != T_NONE ? f->rt : t;
				if (f->rt != T_NONE && f->rt != T_ERR && t != T_ERR && t != f->rt)
					err_node(f->a, NAME_FMT " is declared %s, its value is %s", name, tn(f->rt), tn(t));
				if (type == T_UNIT) err_node(f->a, NAME_FMT " would hold no value (unit)", name);
				i = new_slots(cells(type));
				emit_store(0, i, type, g_nodes[f->a].line);
				add_local(name, type, i);
				f->cur = next_of(f->cur);
				f->phase = 1;
				break;
			}
			default:
				t = pop_type();
				g_nlocals = f->mark;
				g_slots = f->mark2;
				finish(t);
				break;
			}
			break;

		case F_SET:
			if (f->phase == 0) {
				Place pl;
				if (f->k != 2 || !is_sym(f->cur)) {
					err_node(f->node, "set takes a name and a value: (set x 5)");
					finish(T_ERR);
					break;
				}
				pl.global = 0;
				pl.at = 0;
				pl.type = T_ERR;
				f->c = resolve_place(f->cur, &pl);
				if (f->c < 0) err_node(f->cur, "set: unknown name " NAME_FMT, g_nodes[f->cur].text);
				f->rt = f->c > 0 ? pl.type : (Ty)T_ERR;
				f->op = (u8)pl.global;
				f->a = pl.at;
				f->phase = 1;
				push_child(next_of(f->cur), f->rt);
				break;
			}
			t = pop_type();
			if (f->c > 0) {
				if (f->rt != T_ERR && t != T_ERR && t != f->rt)
					err_node(next_of(f->cur), "set: " NAME_FMT " is %s, the value is %s", g_nodes[f->cur].text, tn(f->rt), tn(t));
				emit_store(f->op, f->a, f->rt, line);
			}
			finish(T_UNIT);
			break;

		case F_AND:
		case F_OR:
			if (f->phase == 0) {
				if (f->k < 1) {
					err_node(f->node, "%s takes one or more bool values", f->form == F_AND ? "and" : "or");
					finish(T_ERR);
					break;
				}
				f->mark = g_npatches;
				f->depth = g_depth;
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			t = pop_type();
			if (t != T_BOOL && t != T_ERR)
				err_node(f->cur, "%s takes bool values, this one is %s", f->form == F_AND ? "and" : "or", tn(t));
			if (next_of(f->cur) >= 0) {
				if (g_npatches == MAX_WORK) die("too many and/or operands");
				g_patches[g_npatches++] = emit(f->form == F_AND ? OP_JF : OP_JT, -1, 0, line);
				f->cur = next_of(f->cur);
				push_child(f->cur, T_NONE);
				break;
			}
			if (f->mark < g_npatches) {
				i32 end = emit(OP_JMP, -1, 0, line);
				for (i = f->mark; i < g_npatches; i++) patch(g_patches[i], g_ncode);
				emit(OP_PUSH_K, f->form == F_OR, 0, line);
				patch(end, g_ncode);
				g_npatches = f->mark;
			}
			g_depth = f->depth + 1;
			finish(T_BOOL);
			break;

		case F_NOT:
			if (f->phase == 0) {
				if (f->k != 1) { err_node(f->node, "not takes one bool"); finish(T_ERR); break; }
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			t = pop_type();
			if (t != T_BOOL && t != T_ERR) err_node(f->cur, "not takes a bool, this is %s", tn(t));
			emit(OP_NOT, 0, 0, line);
			finish(T_BOOL);
			break;

		case F_ARITH:
			if (f->phase == 0) {
				if (f->k == 0 || (f->k == 1 && f->op != '-')) {
					err_node(f->node, "%c takes two or more numbers", f->op);
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			t = pop_type();
			if (t != T_ERR && !numeric(t)) {
				err_node(f->cur, "%c works on int and float, not %s", f->op, tn(t));
				t = T_ERR;
			} else if (t == T_FLOAT && f->op == '%') {
				err_node(f->cur, "%% works on int only; this is a float");
				t = T_ERR;
			}
			if (f->phase == 1) {
				f->rt = t;
				if (f->k == 1) {
					if (t != T_ERR) emit(t == T_FLOAT ? OP_NEG_F : OP_NEG_I, 0, 0, line);
					finish(t);
					break;
				}
			} else if (f->rt == T_ERR || t == T_ERR) {
				f->rt = T_ERR;
			} else if (t != f->rt) {
				err_node(f->cur, "%c: %s and %s do not mix; convert one with (float x) or (int x)", f->op, tn(f->rt), tn(t));
				f->rt = T_ERR;
			} else {
				emit(arith_op(f->op, t), 0, 0, line);
			}
			f->phase = 2;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) push_child(f->cur, T_NONE); else finish(f->rt);
			break;

		case F_CMP:
			if (f->phase == 0) {
				if (f->k != 2) { err_node(f->node, "%s compares two values", g_cmp_names[f->op]); finish(T_ERR); break; }
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			if (f->phase == 1) {
				f->phase = 2;
				t = g_types[g_ntypes - 1];
				f->cur = next_of(f->cur);
				push_child(f->cur, t == T_ERR ? (Ty)T_NONE : t);
				break;
			}
			t2 = pop_type();
			t = pop_type();
			if (t != T_ERR && t2 != T_ERR) {
				if (t != t2)
					err_node(f->cur, "%s compares %s with %s; both sides need the same type", g_cmp_names[f->op], tn(t), tn(t2));
				else if (f->op < 4 && !numeric(t))
					err_node(f->cur, "%s orders int and float, not %s", g_cmp_names[f->op], tn(t));
				else if (t == T_UNIT)
					err_node(f->cur, "%s: unit has no value to compare", g_cmp_names[f->op]);
				else if (!comparable(t))
					err_node(f->cur, "%s compares int, float, bool, string and enum values, not %s", g_cmp_names[f->op], tn(t));
				else
					emit(cmp_op(f->op, t), 0, 0, line);
			}
			finish(T_BOOL);
			break;

		case F_PRINT: {
			const char *what = f->op ? "println" : "print";
			i32 fmt = next_of(first_of(f->node));
			Fmt *fr;
			if (f->phase == 0) {
				i32 codes = 0;
				if (fmt < 0 || g_nodes[fmt].kind != N_STR) {
					err_node(fmt < 0 ? f->node : fmt, "%s: the first argument must be a string literal (its %% codes are checked while compiling)", what);
					finish(T_ERR);
					break;
				}
				if (fmt_type(g_nodes[fmt].text, g_nodes[fmt].len, -1, &codes) == T_ERR) {
					err_node(fmt, "%s: unknown %% code; use %%d (int), %%f (float), %%s (string), %%b (bool), %%v (any value) or %%%%", what);
					finish(T_ERR);
					break;
				}
				if (codes != f->k - 1) {
					err_node(fmt, "%s: the format has %ld %% code%s but %ld value%s follow%s", what, (long)codes,
					         codes == 1 ? "" : "s", (long)(f->k - 1), f->k == 2 ? "" : "s", f->k == 2 ? "s" : "");
					finish(T_ERR);
					break;
				}
				if (g_nfmts == MAX_FMTS) die("too many print formats");
				if (g_nfmt_tys + codes > MAX_CONSTS) die("too many printed values");
				f->a = g_nfmts++;
				fr = &g_fmts[f->a];
				fr->str = add_str(g_nodes[fmt].text, g_nodes[fmt].len);
				fr->first = g_nfmt_tys;
				fr->n = codes;
				fr->cells = 0;
				g_nfmt_tys += codes; /* reserved now: a print inside an argument adds its own after these */
				f->cur = next_of(fmt);
				f->k = 0;
				if (f->cur < 0) {
					emit(f->op ? OP_PRINTLN : OP_PRINT, f->a, 0, line);
					finish(T_UNIT);
					break;
				}
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			fr = &g_fmts[f->a];
			t = pop_type();
			t2 = fmt_type(g_nodes[fmt].text, g_nodes[fmt].len, f->k, 0);
			if (t2 == T_ANY) {
				if (t == T_UNIT) err_node(f->cur, "%s: %%v needs a value; this is unit", what);
			} else if (t != T_ERR && t != t2) {
				err_node(f->cur, "%s: %% code %ld wants %s, this value is %s", what, (long)(f->k + 1), tn(t2), tn(t));
			}
			g_fmt_tys[fr->first + f->k] = t;
			fr->cells += cells(t);
			f->k++;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) { push_child(f->cur, T_NONE); break; }
			emit(f->op ? OP_PRINTLN : OP_PRINT, f->a, 0, line);
			finish(T_UNIT);
			break;
		}

		case F_CONV:
		case F_LEN:
		case F_SH:
			if (f->phase == 0) {
				if (f->k != 1) { err_node(f->node, "this takes exactly one value"); finish(T_ERR); break; }
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			t = pop_type();
			if (f->form == F_CONV) {
				if (t == T_INT && f->op == T_FLOAT) emit(OP_I2F, 0, 0, line);
				else if (t == T_FLOAT && f->op == T_INT) emit(OP_F2I, 0, 0, line);
				else if (t != T_ERR && !numeric(t))
					err_node(f->cur, "%s converts a number, not %s", f->op == T_INT ? "int" : "float", tn(t));
				finish(f->op);
				break;
			}
			if (t != T_STR && t != T_ERR)
				err_node(f->cur, "%s takes a string, not %s", f->form == F_LEN ? "len" : "sh", tn(t));
			emit(f->form == F_LEN ? OP_LEN_S : OP_SH, 0, 0, line);
			finish(T_INT);
			break;

		case F_CALL: {
			Fn *fn = &g_fns[f->a];
			if (f->phase == 0) {
				if (f->k != fn->nparams) {
					err_node(f->node, NAME_FMT " takes %ld argument%s, got %ld", fn->name, (long)fn->nparams,
					         fn->nparams == 1 ? "" : "s", (long)f->k);
					finish(T_ERR);
					break;
				}
				f->k = 0;
				if (f->cur < 0) {
					emit(OP_CALL, f->a, 0, line);
					finish(fn->ret);
					break;
				}
				f->phase = 1;
				push_child(f->cur, fn->params[0]);
				break;
			}
			t = pop_type();
			if (t != T_ERR && t != fn->params[f->k])
				err_node(f->cur, NAME_FMT ": argument %ld is %s, it must be %s", fn->name, (long)(f->k + 1), tn(t), tn(fn->params[f->k]));
			f->k++;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) { push_child(f->cur, fn->params[f->k]); break; }
			emit(OP_CALL, f->a, 0, line);
			finish(fn->ret);
			break;
		}

		case F_NEW: { /* (Vec2 1.0 2.0) in field order, or (Vec2 :y 2.0 :x 1.0) by name */
			const TyInfo *ty = &g_tys[f->a];
			const Field *fd;
			i32 value;
			if (f->phase == 0) {
				char names[256];
				if (ty->broken) { finish(T_ERR); break; }
				f->op = (u8)(f->cur >= 0 && g_nodes[f->cur].kind == N_KEY);
				if (!f->op && f->k != ty->n) {
					err_node(f->node, "%s has %ld field%s (%s), got %ld value%s", ty->name, (long)ty->n, ty->n == 1 ? "" : "s",
					         names_of((Ty)f->a, names, sizeof(names)), (long)f->k, f->k == 1 ? "" : "s");
					finish(T_ERR);
					break;
				}
				if (f->op && !check_named(f->node, f->cur, (Ty)f->a)) { finish(T_ERR); break; }
				f->k = 0;
				if (!ty->n) { finish((Ty)f->a); break; }
				if (f->op) { /* named values go to a scratch struct in the frame, then the whole struct is pushed */
					f->mark2 = g_slots;
					f->b = new_slots(ty->cells);
				}
				f->phase = 1;
				fd = f->op ? field_of_key((Ty)f->a, f->cur) : &g_fields[ty->first];
				push_child(f->op ? next_of(f->cur) : f->cur, fd->type);
				break;
			}
			t = pop_type();
			fd = f->op ? field_of_key((Ty)f->a, f->cur) : &g_fields[ty->first + f->k];
			value = f->op ? next_of(f->cur) : f->cur;
			if (t != T_ERR && t != fd->type)
				err_node(value, "%s: field %s is %s, the value is %s", ty->name, fd->name, tn(fd->type), tn(t));
			if (f->op) emit_store(0, f->b + fd->offset, fd->type, line);
			f->k++;
			f->cur = next_of(value);
			if (f->cur >= 0) {
				fd = f->op ? field_of_key((Ty)f->a, f->cur) : &g_fields[ty->first + f->k];
				push_child(f->op ? next_of(f->cur) : f->cur, fd->type);
				break;
			}
			if (f->op) {
				emit_load(0, f->b, (Ty)f->a, line);
				g_slots = f->mark2;
			}
			finish((Ty)f->a);
			break;
		}

		case F_SOME:
			if (f->phase == 0) {
				if (f->k != 1) { err_node(f->node, "some takes one value: (some 5)"); finish(T_ERR); break; }
				emit(OP_PUSH_K, 1, 0, line); /* the tag, then the value */
				f->phase = 1;
				push_child(f->cur, is_option(f->want) ? g_tys[f->want].elem : (Ty)T_NONE);
				break;
			}
			t = pop_type();
			if (t == T_UNIT) err_node(f->cur, "some needs a value; this is unit");
			finish(t == T_ERR || t == T_UNIT ? (Ty)T_ERR : intern_option(t));
			break;

		case F_MATCH:
			switch (f->phase) {
			case 0:
				if (f->k < 2) {
					err_node(f->node, "match takes a value and its arms: (match x (pattern body...) ...)");
					finish(T_ERR);
					break;
				}
				f->mark2 = g_slots;
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			case 1:
				t = pop_type();
				if (t == T_ERR || g_tys[t].broken) { finish(T_ERR); break; }
				if (!matchable(t)) {
					err_node(f->cur, "match works on bool, int, string, enum and option values, not %s", tn(t));
					finish(T_ERR);
					break;
				}
				f->rt = t;
				f->op = (u8)check_arms(f->node, next_of(f->cur), t);
				f->a = new_slots(cells(t)); /* the matched value stays here while the arms look at it */
				emit_store(0, f->a, t, line);
				f->depth = g_depth;
				f->c = g_npatches;
				f->mark = g_nlocals;
				f->k = 0; /* becomes 1 when two arms disagree */
				f->cur = next_of(f->cur);
				start_arm(f);
				break;
			default: /* an arm's body is done */
				t = pop_type();
				if (f->want == T_UNIT) {
					emit_pop(t, line);
					t = T_UNIT;
				}
				if (f->res == T_NONE || f->res == T_ERR) {
					f->res = t;
				} else if (t != f->res && t != T_ERR) {
					err_node(f->cur, "match: this arm is %s, the arms above are %s; every arm must give the same type", tn(t), tn(f->res));
					f->k = 1;
				}
				if (next_of(f->cur) >= 0) {
					if (g_npatches == MAX_WORK) die("too many match arms");
					g_patches[g_npatches++] = emit(OP_JMP, -1, 0, line);
					if (f->b >= 0) patch(f->b, g_ncode);
					g_depth = f->depth;
					f->cur = next_of(f->cur);
					start_arm(f);
					break;
				}
				if (f->b >= 0) patch(f->b, g_ncode);
				for (i = f->c; i < g_npatches; i++) patch(g_patches[i], g_ncode);
				g_npatches = f->c;
				g_nlocals = f->mark;
				g_slots = f->mark2;
				t = f->res == T_NONE ? (Ty)T_UNIT : f->res;
				g_depth = f->depth + cells(t);
				finish(f->k ? (Ty)T_ERR : t);
				break;
			}
			break;

		default:
			err_node(f->node, "internal: unknown form");
			finish(T_ERR);
			break;
		}
	}
}

static o compile_expr(i32 n, Ty want)
{
	i32 base = g_nwork;
	push_child(n, want);
	run_work(base);
}

/* new_name_ok NAME FORM WHAT: a new struct or enum gets a plain name that nothing else uses */
static int new_name_ok(i32 name, i32 form, const char *what)
{
	const char *s;
	if (!is_sym(name)) {
		err_node(name < 0 ? form : name, "%s needs a name: %s", what,
		         what[0] == 's' ? "(struct Vec2 (x : float) (y : float))" : "(enum Dir north south east west)");
		return 0;
	}
	s = g_nodes[name].text;
	if (strchr(s, '.')) { err_node(name, "a type name cannot contain a dot"); return 0; }
	if (is_reserved(s)) { err_node(name, NAME_FMT " is a built-in name", s); return 0; }
	if (find_fn(s) >= 0) { err_node(name, NAME_FMT " is already a function", s); return 0; }
	if (find_global(s) >= 0) { err_node(name, NAME_FMT " is already a global", s); return 0; }
	return 1;
}

/* add_type NAME KIND FIRST N CELLS: register a struct or an enum; in the REPL the same declaration read again is fine */
static o add_type(i32 name, u8 kind, i32 first, i32 n, i32 ncells, int repl, int broken)
{
	const char *s = g_nodes[name].text;
	Ty old = find_named_type(s);
	TyInfo *t;
	i32 i;
	if (old != T_NONE) {
		TyInfo *ot = &g_tys[old];
		int same = repl && ot->kind == kind && ot->n == n;
		for (i = 0; same && i < n; i++) {
			if (kind == K_STRUCT)
				same = !strcmp(g_fields[ot->first + i].name, g_fields[first + i].name) && g_fields[ot->first + i].type == g_fields[first + i].type;
			else
				same = !strcmp(g_variants[ot->first + i], g_variants[first + i]);
		}
		if (!same) err_node(name, NAME_FMT " is already a type (line %ld)", s, (long)ot->line);
		if (kind == K_STRUCT) g_nfields = first; else g_nvariants = first;
		return;
	}
	if (g_ntys == MAX_TYS) die("too many types");
	t = &g_tys[g_ntys++];
	memset(t, 0, sizeof(*t));
	t->name = arena_strndup(&g_perm, s, g_nodes[name].len);
	t->kind = kind;
	t->cells = ncells;
	t->first = first;
	t->n = n;
	t->line = g_nodes[name].line;
	t->broken = (u8)broken;
}

/* declared_below FROM NAME: a struct or enum called NAME comes after FROM at the top level */
static int declared_below(i32 from, const char *name)
{
	i32 n;
	for (n = next_of(from); n >= 0; n = next_of(n)) {
		if ((head_is(n, "struct") || head_is(n, "enum")) && sym_is(next_of(first_of(n)), name)) return 1;
	}
	return 0;
}

/* declare_struct STRUCT REPL: (struct Name (field : type) ...); a field's type must be declared above */
static o declare_struct(i32 n, int repl)
{
	i32 name = next_of(first_of(n)), p, q, first = g_nfields, ncells = 0, nf = 0, bad = 0;
	if (!new_name_ok(name, n, "struct")) return;
	for (p = next_of(name); p >= 0; p = next_of(p)) {
		i32 fname = first_of(p), colon = next_of(fname), ft = next_of(colon), base = ft;
		Ty t;
		if (g_nodes[p].kind != N_LIST || !is_sym(fname) || !sym_is(colon, ":") || ft < 0 || next_of(ft) >= 0) {
			err_node(p, "a field is (name : type)");
			bad++;
			continue;
		}
		if (is_reserved(g_nodes[fname].text) || strchr(g_nodes[fname].text, '.')) {
			err_node(fname, NAME_FMT " cannot be a field name", g_nodes[fname].text);
			bad++;
			continue;
		}
		for (q = next_of(name); q != p && !(g_nodes[q].kind == N_LIST && sym_is(first_of(q), g_nodes[fname].text)); q = next_of(q)) {}
		if (q != p) {
			err_node(fname, "field " NAME_FMT " twice", g_nodes[fname].text);
			bad++;
			continue;
		}
		while (head_is(base, "option")) base = next_of(first_of(base));
		if (sym_is(base, g_nodes[name].text)) {
			err_node(ft, NAME_FMT " cannot hold a " NAME_FMT ", not even in an option: its size would have no end",
			         g_nodes[name].text, g_nodes[name].text);
			bad++;
			continue;
		}
		if (is_sym(base) && find_named_type(g_nodes[base].text) == T_NONE && declared_below(n, g_nodes[base].text)) {
			err_node(base, NAME_FMT " is declared below; declare a type above the structs that use it", g_nodes[base].text);
			bad++;
			continue;
		}
		t = parse_type(ft);
		if (t == T_UNIT) err_node(ft, "a field cannot be unit");
		if (t == T_UNIT || t == T_ERR) { bad++; continue; }
		if (g_nfields == MAX_FIELDS) die("too many struct fields");
		g_fields[g_nfields].name = arena_strndup(&g_perm, g_nodes[fname].text, g_nodes[fname].len);
		g_fields[g_nfields].type = t;
		g_fields[g_nfields].offset = ncells;
		g_nfields++;
		ncells += cells(t);
		nf++;
	}
	add_type(name, K_STRUCT, first, nf, ncells, repl, bad);
}

/* declare_enum ENUM REPL: (enum Name variant ...) */
static o declare_enum(i32 n, int repl)
{
	i32 name = next_of(first_of(n)), p, i, first = g_nvariants, nv = 0, bad = 0;
	if (!new_name_ok(name, n, "enum")) return;
	if (next_of(name) < 0) {
		err_node(name, "enum " NAME_FMT " needs at least one variant: (enum Dir north south)", g_nodes[name].text);
		return;
	}
	for (p = next_of(name); p >= 0; p = next_of(p)) {
		if (!is_sym(p) || is_reserved(g_nodes[p].text) || strchr(g_nodes[p].text, '.')) {
			err_node(p, "a variant is a plain name, like north");
			bad++;
			continue;
		}
		for (i = first; i < g_nvariants && strcmp(g_variants[i], g_nodes[p].text); i++) {}
		if (i < g_nvariants) {
			err_node(p, "variant " NAME_FMT " twice", g_nodes[p].text);
			bad++;
			continue;
		}
		if (g_nvariants == MAX_VARIANTS) die("too many enum variants");
		g_variants[g_nvariants++] = arena_strndup(&g_perm, g_nodes[p].text, g_nodes[p].len);
		nv++;
	}
	add_type(name, K_ENUM, first, nv, 1, repl, bad);
}

/* fn_shape DEFN NAME PARAMS RET BODY: the parts of (defn name ((p : type) ...) [: type] body...) */
static int fn_shape(i32 n, i32 *name, i32 *params, i32 *ret, i32 *body)
{
	*name = next_of(first_of(n));
	*params = next_of(*name);
	*ret = -1;
	*body = next_of(*params);
	if (!is_sym(*name)) { err_node(*name < 0 ? n : *name, "defn needs a name: (defn name ((x : int)) : int body)"); return 0; }
	if (*params < 0 || g_nodes[*params].kind != N_LIST) {
		err_node(*params < 0 ? n : *params, "defn " NAME_FMT " needs a parameter list, () when it takes none", g_nodes[*name].text);
		return 0;
	}
	if (sym_is(*body, ":")) {
		*ret = next_of(*body);
		*body = next_of(*ret);
	}
	return 1;
}

/* declare_fn DEFN REPL: register the signature, so calls can come before the definition */
static o declare_fn(i32 n, int repl)
{
	i32 name, params, ret, body, p, fi;
	Fn fn;
	if (!fn_shape(n, &name, &params, &ret, &body)) return;
	memset(&fn, 0, sizeof(fn));
	if (is_reserved(g_nodes[name].text) || strchr(g_nodes[name].text, '.')) {
		err_node(name, strchr(g_nodes[name].text, '.') ? NAME_FMT ": a function name cannot contain a dot" : NAME_FMT " is a built-in name",
		         g_nodes[name].text);
		return;
	}
	if (find_named_type(g_nodes[name].text) != T_NONE) {
		err_node(name, NAME_FMT " is already a type", g_nodes[name].text);
		return;
	}
	for (p = first_of(params); p >= 0; p = next_of(p)) {
		i32 pn = first_of(p), colon = next_of(pn), pt = next_of(colon), q;
		if (g_nodes[p].kind != N_LIST || !is_sym(pn) || !sym_is(colon, ":") || pt < 0 || next_of(pt) >= 0) {
			err_node(p, "a parameter is (name : type)");
			return;
		}
		if (is_reserved(g_nodes[pn].text) || strchr(g_nodes[pn].text, '.'))
			err_node(pn, NAME_FMT " cannot be a parameter name", g_nodes[pn].text);
		for (q = first_of(params); q != p; q = next_of(q)) {
			if (!strcmp(g_nodes[first_of(q)].text, g_nodes[pn].text)) err_node(pn, "parameter " NAME_FMT " twice", g_nodes[pn].text);
		}
		if (fn.nparams == MAX_PARAMS) { err_node(p, "at most %d parameters", MAX_PARAMS); return; }
		fn.params[fn.nparams] = parse_type(pt);
		if (fn.params[fn.nparams] == T_UNIT) err_node(pt, "a parameter cannot be unit");
		fn.pcells += cells(fn.params[fn.nparams]);
		fn.nparams++;
	}
	fn.ret = ret >= 0 ? parse_type(ret) : (Ty)T_UNIT;
	fn.line = g_nodes[n].line;
	fn.entry = -1;
	fn.decl_unit = g_unit_id;
	fn.decl_node = n;
	if ((fi = find_fn(g_nodes[name].text)) >= 0) {
		Fn *old = &g_fns[fi];
		if (!repl || old->nparams != fn.nparams || old->ret != fn.ret || memcmp(old->params, fn.params, fn.nparams * sizeof(Ty))) {
			err_node(name, NAME_FMT " is already defined (line %ld)", g_nodes[name].text, (long)old->line);
			return;
		}
		old->decl_unit = g_unit_id; /* REPL: same signature, the new body replaces the old one */
		old->decl_node = n;
		return;
	}
	if (find_global(g_nodes[name].text) >= 0) {
		err_node(name, NAME_FMT " is already a global", g_nodes[name].text);
		return;
	}
	if (g_nfns == MAX_FNS) die("too many functions");
	fn.name = arena_strndup(&g_perm, g_nodes[name].text, g_nodes[name].len);
	g_fns[g_nfns++] = fn;
}

static o compile_fn(i32 n)
{
	i32 name, params, ret, body, p, fi, jmp, k = 0, nc;
	i32 s_slots = g_slots, s_max = g_maxslots, s_locals = g_nlocals, s_floor = g_local_floor;
	i32 s_depth = g_depth, s_maxdepth = g_maxdepth;
	Ty t, rt;
	if (!fn_shape(n, &name, &params, &ret, &body)) return;
	fi = find_fn(g_nodes[name].text);
	if (fi < 0 || g_fns[fi].decl_unit != g_unit_id || g_fns[fi].decl_node != n) return; /* declare_fn said why */
	jmp = emit(OP_JMP, -1, 0, g_nodes[n].line);
	g_fns[fi].entry = g_ncode;
	g_slots = g_maxslots = 0;
	g_depth = g_maxdepth = 0;
	g_local_floor = g_nlocals; /* the top level's locals live in another frame */
	for (p = first_of(params); p >= 0; p = next_of(p), k++)
		add_local(g_nodes[first_of(p)].text, g_fns[fi].params[k], new_slots(cells(g_fns[fi].params[k])));
	rt = g_fns[fi].ret;
	push_body(body, 1, rt);
	run_work(g_nwork - 1);
	t = pop_type();
	if (rt == T_UNIT) {
		emit_pop(t, g_nodes[n].line);
		emit(OP_RET_U, 0, 0, g_nodes[n].line);
	} else {
		if (t != T_ERR && rt != T_ERR && t != rt)
			err_node(name, NAME_FMT " must return %s, its body ends with %s", g_nodes[name].text, tn(rt), tn(t));
		nc = cells(rt);
		if (nc == 0) emit(OP_RET_U, 0, 0, g_nodes[n].line);
		else if (nc == 1) emit(OP_RET_V, 0, 0, g_nodes[n].line);
		else emit(OP_RET_N, nc, 0, g_nodes[n].line);
	}
	g_fns[fi].nslots = g_maxslots;
	g_fns[fi].maxdepth = g_maxdepth;
	g_slots = s_slots;
	g_maxslots = s_max;
	g_depth = s_depth;
	g_maxdepth = s_maxdepth;
	g_nlocals = s_locals;
	g_local_floor = s_floor;
	patch(jmp, g_ncode);
}

/* compile_def DEF REPL: (def name value) or (def name : type value) - a global */
static o compile_def(i32 n, int repl)
{
	i32 name = next_of(first_of(n)), x, value, gi;
	Ty declared = T_NONE, t, type;
	const char *s;
	if (!is_sym(name)) { err_node(name < 0 ? n : name, "def needs a name: (def hp : int 100)"); return; }
	s = g_nodes[name].text;
	x = next_of(name);
	value = x;
	if (sym_is(x, ":")) {
		declared = parse_type(next_of(x));
		value = next_of(next_of(x));
	}
	if (value < 0) { err_node(name, NAME_FMT " has no value", s); return; }
	if (next_of(value) >= 0) err_node(next_of(value), "def has one value");
	if (strchr(s, '.')) { err_node(name, NAME_FMT ": a global name cannot contain a dot", s); return; }
	if (is_reserved(s)) { err_node(name, NAME_FMT " is a built-in name", s); return; }
	if (find_fn(s) >= 0) { err_node(name, NAME_FMT " is already a function", s); return; }
	if (find_named_type(s) != T_NONE) { err_node(name, NAME_FMT " is already a type", s); return; }
	compile_expr(value, declared);
	t = pop_type();
	type = declared != T_NONE ? declared : t;
	if (declared != T_NONE && declared != T_ERR && t != T_ERR && t != declared)
		err_node(name, NAME_FMT " is declared %s, its value is %s", s, tn(declared), tn(t));
	if (type == T_UNIT) err_node(name, NAME_FMT " would hold no value (unit)", s);
	if ((gi = find_global(s)) >= 0) {
		if (!repl || g_globals[gi].type != type) {
			err_node(name, NAME_FMT " is already defined", s);
			return;
		}
	} else {
		if (g_nglobals == MAX_GLOBALS) die("too many globals");
		if (g_nglobal_cells + cells(type) > MAX_GLOBAL_CELLS) die("globals too large (MAX_GLOBAL_CELLS)");
		gi = g_nglobals++;
		g_globals[gi].name = arena_strndup(&g_perm, s, g_nodes[name].len);
		g_globals[gi].type = type;
		g_globals[gi].cell = g_nglobal_cells;
		g_nglobal_cells += cells(type);
	}
	emit_store(1, g_globals[gi].cell, type, g_nodes[name].line);
}

/* everything a failed unit may have added, so the REPL can forget it */
static struct {
	i32 ncode, nints, nfloats, nstrs, nfns, nglobals, nglobal_cells, ntys, nfields, nvariants, nfmts, nfmt_tys;
	size_t perm;
} g_snap;
static i32 g_snap_entry[MAX_FNS], g_snap_slots[MAX_FNS], g_snap_depth[MAX_FNS];

/* compile_unit TOP REPL: compile every top-level form; returns the entry point, or -1 after errors (and then nothing
 * of the unit stays). In the REPL the value of the last form is shown. */
static i32 compile_unit(i32 top, int repl)
{
	i32 n, entry, i;
	Ty t;
	g_snap.ncode = g_ncode;
	g_snap.nints = g_nints;
	g_snap.nfloats = g_nfloats;
	g_snap.nstrs = g_nstrs;
	g_snap.nfns = g_nfns;
	g_snap.nglobals = g_nglobals;
	g_snap.nglobal_cells = g_nglobal_cells;
	g_snap.ntys = g_ntys;
	g_snap.nfields = g_nfields;
	g_snap.nvariants = g_nvariants;
	g_snap.nfmts = g_nfmts;
	g_snap.nfmt_tys = g_nfmt_tys;
	g_snap.perm = g_perm.used;
	for (i = 0; i < g_nfns; i++) {
		g_snap_entry[i] = g_fns[i].entry;
		g_snap_slots[i] = g_fns[i].nslots;
		g_snap_depth[i] = g_fns[i].maxdepth;
	}
	g_unit_id++;
	g_nlocals = g_local_floor = 0;
	g_slots = g_maxslots = 0;
	g_depth = g_maxdepth = 0;
	g_ntypes = g_npatches = g_nwork = 0;

	for (n = top; n >= 0; n = next_of(n)) { /* types first, so signatures and bodies can use them */
		if (head_is(n, "struct")) declare_struct(n, repl);
		else if (head_is(n, "enum")) declare_enum(n, repl);
	}
	for (n = top; n >= 0; n = next_of(n)) {
		if (head_is(n, "defn")) declare_fn(n, repl);
	}
	entry = emit(OP_ENTER, 0, 0, top >= 0 ? g_nodes[top].line : 1);
	for (n = top; n >= 0; n = next_of(n)) {
		int shown = repl && next_of(n) < 0;
		if (head_is(n, "defn")) { compile_fn(n); continue; }
		if (head_is(n, "def")) { compile_def(n, repl); continue; }
		if (head_is(n, "struct") || head_is(n, "enum")) continue;
		compile_expr(n, shown ? (Ty)T_NONE : (Ty)T_UNIT);
		t = pop_type();
		if (shown && t != T_UNIT && t != T_ERR) emit(OP_SHOW, t, 0, g_nodes[n].line);
		else emit_pop(t, g_nodes[n].line);
	}
	emit(OP_HALT, 0, 0, 0);
	g_code[entry].a = g_maxslots;
	g_code[entry].b = g_maxdepth;
	if (!g_unit.nerrors) return entry;

	g_ncode = g_snap.ncode;
	g_nints = g_snap.nints;
	g_nfloats = g_snap.nfloats;
	g_nstrs = g_snap.nstrs;
	g_nfns = g_snap.nfns;
	g_nglobals = g_snap.nglobals;
	g_nglobal_cells = g_snap.nglobal_cells;
	g_ntys = g_snap.ntys;
	g_nfields = g_snap.nfields;
	g_nvariants = g_snap.nvariants;
	g_nfmts = g_snap.nfmts;
	g_nfmt_tys = g_snap.nfmt_tys;
	g_perm.used = g_snap.perm;
	for (i = 0; i < g_nfns; i++) {
		g_fns[i].entry = g_snap_entry[i];
		g_fns[i].nslots = g_snap_slots[i];
		g_fns[i].maxdepth = g_snap_depth[i];
	}
	return -1;
}
