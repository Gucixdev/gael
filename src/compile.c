/* compile.c - type checking and bytecode generation in one pass over the tree, without recursion.
 *
 * A work stack of frames says which node is being compiled and how far it got (phase). A finished expression leaves
 * exactly one type on the type stack, and code that leaves its value on the VM stack (no value for unit). An error
 * is reported with its line and column and the expression gets the type ERR, which agrees with everything: one
 * mistake gives one message, compilation goes on, and every error of a unit comes out in one run. */

typedef enum { T_ERR, T_UNIT, T_INT, T_FLOAT, T_BOOL, T_STR, T_NONE } Type;
static const char *const g_type_names[] = { "<error>", "unit", "int", "float", "bool", "string", "<none>" };

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
	u8 nparams, ret;
	u8 params[MAX_PARAMS];
	i32 entry, nslots, line;
	i32 decl_unit, decl_node; /* the defn that declared it, so only that one compiles a body into it */
} Fn;

typedef struct {
	const char *name;
	u8 type;
} Global;

typedef struct {
	const char *name;
	u8 type;
	i32 slot;
} Local;

/* what lives as long as the process: code, constants, strings, functions, globals */
#define MAX_CODE (1 << 18)
#define MAX_CONSTS (1 << 16)
#define MAX_FNS 4096
#define MAX_GLOBALS 4096
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
static i32 g_nglobals;
static u64 g_perm_mem[(8u << 20) / 8];
static Arena g_perm = { (u8 *)g_perm_mem, sizeof(g_perm_mem), 0, "permanent" };

/* what one compilation needs */
#define MAX_LOCALS 1024
#define MAX_WORK 4096
static Local g_locals[MAX_LOCALS];
static i32 g_nlocals, g_local_floor; /* names below the floor belong to an outer frame and are not visible */
static i32 g_slots, g_maxslots;
static u8 g_types[MAX_WORK];
static i32 g_ntypes;
static i32 g_patches[MAX_WORK];
static i32 g_npatches;

typedef enum {
	F_START, F_CALL, F_IF, F_WHEN, F_WHILE, F_DO, F_LET, F_SET, F_AND, F_OR, F_NOT, F_ARITH, F_CMP, F_PRINT,
	F_CONV, F_LEN, F_SH
} Form;

typedef struct {
	i32 node;  /* the form being compiled; -1 for a body (a run of sibling forms) */
	u8 form, phase;
	u8 op;     /* the operator, which comparison, which builtin */
	u8 rt;     /* running type, or a binding's declared type */
	u8 keep;   /* bodies: 1 keeps the value of the last form */
	i32 cur;   /* the child being compiled */
	i32 k;     /* argument number */
	i32 a, b;  /* code positions to patch, a function index, a binding's name */
	i32 mark;  /* scope or patch-stack height to go back to */
	i32 mark2; /* slot count to go back to */
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

static const char *tn(u8 t) { return g_type_names[t <= T_NONE ? t : T_ERR]; }

static i32 emit(Op op, i32 a, i32 b, i32 line)
{
	if (g_ncode == MAX_CODE) die("program too large (MAX_CODE instructions)");
	g_code[g_ncode].op = (u8)op;
	g_code[g_ncode].a = a;
	g_code[g_ncode].b = b;
	g_code[g_ncode].line = line;
	return g_ncode++;
}

static o patch(i32 at, i32 target) { g_code[at].a = target; }

static o push_type(u8 t)
{
	if (g_ntypes == MAX_WORK) die("expression nested too deep (type stack)");
	g_types[g_ntypes++] = t;
}

static u8 pop_type(void) { return g_ntypes ? g_types[--g_ntypes] : (u8)T_ERR; }

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

static u8 type_of_name(const char *s)
{
	if (!strcmp(s, "int")) return T_INT;
	if (!strcmp(s, "float")) return T_FLOAT;
	if (!strcmp(s, "bool")) return T_BOOL;
	if (!strcmp(s, "string")) return T_STR;
	if (!strcmp(s, "unit")) return T_UNIT;
	return T_NONE;
}

/* parse_type NODE: the type a symbol names, or an error and ERR */
static u8 parse_type(i32 n)
{
	u8 t = is_sym(n) ? type_of_name(g_nodes[n].text) : (u8)T_NONE;
	if (t != T_NONE) return t;
	if (n < 0) return T_ERR;
	err_node(n, "expected a type here: int, float, bool, string or unit");
	return T_ERR;
}

static const char *const g_reserved[] = {
	"if", "when", "while", "do", "let", "set", "and", "or", "not", "+", "-", "*", "/", "%", "<", "<=", ">", ">=", "=",
	"!=", "print", "println", "int", "float", "bool", "string", "unit", "len", "sh", "defn", "def", "true", "false", ":"
};

static int is_reserved(const char *s)
{
	size_t i;
	for (i = 0; i < GAEL_COUNT(g_reserved); i++) {
		if (!strcmp(s, g_reserved[i])) return 1;
	}
	return 0;
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

static i32 new_slot(void)
{
	i32 s = g_slots++;
	if (g_slots > g_maxslots) g_maxslots = g_slots;
	return s;
}

static o add_local(const char *name, u8 type, i32 slot)
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
	return f;
}

/* push_body FIRST KEEP: compile the forms FIRST, FIRST.next, ... as one expression; with KEEP the last one's value is
 * the result, otherwise every value is dropped and the result is unit */
static o push_body(i32 first, int keep)
{
	Frame *f = push_frame(-1, F_DO);
	f->cur = first;
	f->keep = (u8)keep;
}

static o finish(u8 t)
{
	push_type(t);
	g_nwork--;
}

static i32 line_of(Frame *f) { return f->node >= 0 ? g_nodes[f->node].line : (f->cur >= 0 ? g_nodes[f->cur].line : 0); }

static int numeric(u8 t) { return t == T_INT || t == T_FLOAT; }

static Op arith_op(u8 op, u8 t)
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

static Op cmp_op(u8 which, u8 t)
{
	static const Op ints[] = { OP_LT_I, OP_LE_I, OP_GT_I, OP_GE_I, OP_EQ_I, OP_NE_I };
	static const Op floats[] = { OP_LT_F, OP_LE_F, OP_GT_F, OP_GE_F, OP_EQ_F, OP_NE_F };
	if (t == T_STR) return which == 4 ? OP_EQ_S : OP_NE_S;
	return t == T_FLOAT ? floats[which] : ints[which];
}

/* fmt_type TEXT LEN K: the type the K-th % code of a format wants; T_NONE when it has fewer codes. With K = -1:
 * returns T_ERR when the format has an unknown code, T_NONE otherwise; *count gets the number of codes */
static u8 fmt_type(const char *s, u32 n, i32 want, i32 *count)
{
	u32 i;
	i32 k = 0;
	for (i = 0; i < n; i++) {
		u8 t;
		if (s[i] != '%') continue;
		if (i + 1 == n) return want < 0 ? (u8)T_ERR : (u8)T_NONE;
		switch (s[++i]) {
		case '%': continue;
		case 'd': t = T_INT; break;
		case 'f': t = T_FLOAT; break;
		case 's': t = T_STR; break;
		case 'b': t = T_BOOL; break;
		default: return want < 0 ? (u8)T_ERR : (u8)T_NONE;
		}
		if (k == want) return t;
		k++;
	}
	if (count) *count = k;
	return T_NONE;
}

static o compile_atom(i32 n)
{
	Node *nd = &g_nodes[n];
	i32 i;
	switch (nd->kind) {
	case N_INT: emit(OP_PUSH_I, add_int(nd->ival), 0, nd->line); push_type(T_INT); return;
	case N_FLOAT: emit(OP_PUSH_F, add_float(nd->fval), 0, nd->line); push_type(T_FLOAT); return;
	case N_STR: emit(OP_PUSH_S, add_str(nd->text, nd->len), 0, nd->line); push_type(T_STR); return;
	case N_KEY: err_node(n, "keywords like " NAME_FMT " are not values yet", nd->text); push_type(T_ERR); return;
	default: break;
	}
	if (!strcmp(nd->text, "true") || !strcmp(nd->text, "false")) {
		emit(OP_PUSH_B, nd->text[0] == 't', 0, nd->line);
		push_type(T_BOOL);
		return;
	}
	if ((i = find_local(nd->text)) >= 0) {
		emit(OP_LOAD_L, g_locals[i].slot, 0, nd->line);
		push_type(g_locals[i].type);
	} else if ((i = find_global(nd->text)) >= 0) {
		emit(OP_LOAD_G, i, 0, nd->line);
		push_type(g_globals[i].type);
	} else if (find_fn(nd->text) >= 0) {
		err_node(n, NAME_FMT " is a function; call it with (" NAME_FMT " ...)", nd->text, nd->text);
		push_type(T_ERR);
	} else {
		err_node(n, "unknown name " NAME_FMT, nd->text);
		push_type(T_ERR);
	}
}

/* start_list FRAME: decide which form a list is, check its shape, and start it */
static o start_list(Frame *f)
{
	i32 head = first_of(f->node), fi;
	const char *s;
	size_t k;
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
	else if (!strcmp(s, "defn") || !strcmp(s, "def")) {
		err_node(head, NAME_FMT " is only allowed at the top level of a file", s);
		finish(T_ERR);
		return;
	} else {
		for (k = 0; k < GAEL_COUNT(g_cmp_names); k++) {
			if (!strcmp(s, g_cmp_names[k])) { f->form = F_CMP; f->op = (u8)k; break; }
		}
		if (f->form == F_START) {
			if ((fi = find_fn(s)) < 0) {
				if (find_local(s) >= 0 || find_global(s) >= 0)
					err_node(head, NAME_FMT " is a variable, not a function", s);
				else
					err_node(head, "unknown function " NAME_FMT, s);
				finish(T_ERR);
				return;
			}
			f->form = F_CALL;
			f->a = fi;
		}
	}
}

/* run_work BASE: compile until the work stack is back at BASE; leaves one type per finished root */
static o run_work(i32 base)
{
	while (g_nwork > base) {
		Frame *f = &g_work[g_nwork - 1];
		i32 line = line_of(f), i;
		u8 t, t2;

		if (f->form == F_START) {
			if (f->node < 0) { finish(T_ERR); continue; } /* a missing operand; its form reported it */
			if (g_nodes[f->node].kind != N_LIST) {
				compile_atom(f->node);
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
				push_frame(f->cur, F_START);
				break;
			}
			t = pop_type();
			if (next_of(f->cur) < 0 && f->keep) { finish(t); break; }
			if (t != T_UNIT && t != T_ERR) emit(OP_POP, 0, 0, g_nodes[f->cur].line);
			if (next_of(f->cur) < 0) { finish(T_UNIT); break; }
			f->cur = next_of(f->cur);
			push_frame(f->cur, F_START);
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
				push_frame(f->cur, F_START);
				break;
			case 1:
				t = pop_type();
				if (t != T_BOOL && t != T_ERR) err_node(f->cur, "if: the condition is %s, it must be bool", tn(t));
				f->a = emit(OP_JF, -1, 0, line);
				f->cur = next_of(f->cur);
				f->phase = 2;
				push_frame(f->cur, F_START);
				break;
			case 2:
				if (next_of(f->cur) < 0) {
					t = pop_type();
					if (t != T_UNIT && t != T_ERR) emit(OP_POP, 0, 0, line);
					patch(f->a, g_ncode);
					finish(T_UNIT);
					break;
				}
				f->b = emit(OP_JMP, -1, 0, line);
				patch(f->a, g_ncode);
				f->cur = next_of(f->cur);
				f->phase = 3;
				push_frame(f->cur, F_START);
				break;
			default:
				t2 = pop_type();
				t = pop_type();
				if (t != t2 && t != T_ERR && t2 != T_ERR)
					err_node(f->cur, "if: the then branch is %s and the else branch is %s; they must match", tn(t), tn(t2));
				patch(f->b, g_ncode);
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
				push_frame(f->cur, F_START);
				break;
			case 1:
				t = pop_type();
				if (t != T_BOOL && t != T_ERR)
					err_node(f->cur, "%s: the condition is %s, it must be bool", f->form == F_WHEN ? "when" : "while", tn(t));
				f->b = emit(OP_JF, -1, 0, line);
				f->phase = 2;
				push_body(next_of(f->cur), 0);
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
					push_body(next_of(f->b), 1);
					break;
				}
				name = first_of(f->cur);
				if (g_nodes[f->cur].kind != N_LIST || !is_sym(name)) {
					err_node(f->cur, "a binding is (name value) or (name : type value)");
					f->cur = next_of(f->cur);
					break;
				}
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
				push_frame(init, F_START);
				break;
			}
			case 2: {
				const char *name = g_nodes[f->a].text;
				u8 type;
				t = pop_type();
				type = f->rt != T_NONE ? f->rt : t;
				if (f->rt != T_NONE && f->rt != T_ERR && t != T_ERR && t != f->rt)
					err_node(f->a, NAME_FMT " is declared %s, its value is %s", name, tn(f->rt), tn(t));
				if (type == T_UNIT) err_node(f->a, NAME_FMT " would hold no value (unit)", name);
				i = new_slot();
				emit(OP_STORE_L, i, 0, g_nodes[f->a].line);
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
				if (f->k != 2 || !is_sym(f->cur)) {
					err_node(f->node, "set takes a name and a value: (set x 5)");
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				push_frame(next_of(f->cur), F_START);
				break;
			} else {
				const char *name = g_nodes[f->cur].text;
				u8 want = T_NONE;
				t = pop_type();
				if ((i = find_local(name)) >= 0) {
					want = g_locals[i].type;
					emit(OP_STORE_L, g_locals[i].slot, 0, line);
				} else if ((i = find_global(name)) >= 0) {
					want = g_globals[i].type;
					emit(OP_STORE_G, i, 0, line);
				} else {
					err_node(f->cur, "set: unknown name " NAME_FMT, name);
				}
				if (want != T_NONE && want != T_ERR && t != T_ERR && t != want)
					err_node(next_of(f->cur), "set: " NAME_FMT " is %s, the value is %s", name, tn(want), tn(t));
				finish(T_UNIT);
			}
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
				f->phase = 1;
				push_frame(f->cur, F_START);
				break;
			}
			t = pop_type();
			if (t != T_BOOL && t != T_ERR)
				err_node(f->cur, "%s takes bool values, this one is %s", f->form == F_AND ? "and" : "or", tn(t));
			if (next_of(f->cur) >= 0) {
				if (g_npatches == MAX_WORK) die("too many and/or operands");
				g_patches[g_npatches++] = emit(f->form == F_AND ? OP_JF : OP_JT, -1, 0, line);
				f->cur = next_of(f->cur);
				push_frame(f->cur, F_START);
				break;
			}
			if (f->mark < g_npatches) {
				i32 end = emit(OP_JMP, -1, 0, line);
				for (i = f->mark; i < g_npatches; i++) patch(g_patches[i], g_ncode);
				emit(OP_PUSH_B, f->form == F_OR, 0, line);
				patch(end, g_ncode);
				g_npatches = f->mark;
			}
			finish(T_BOOL);
			break;

		case F_NOT:
			if (f->phase == 0) {
				if (f->k != 1) { err_node(f->node, "not takes one bool"); finish(T_ERR); break; }
				f->phase = 1;
				push_frame(f->cur, F_START);
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
				push_frame(f->cur, F_START);
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
			if (f->cur >= 0) push_frame(f->cur, F_START); else finish(f->rt);
			break;

		case F_CMP:
			if (f->phase == 0) {
				if (f->k != 2) { err_node(f->node, "%s compares two values", g_cmp_names[f->op]); finish(T_ERR); break; }
				f->phase = 1;
				push_frame(f->cur, F_START);
				break;
			}
			if (f->phase == 1) {
				f->phase = 2;
				f->cur = next_of(f->cur);
				push_frame(f->cur, F_START);
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
				else
					emit(cmp_op(f->op, t), 0, 0, line);
			}
			finish(T_BOOL);
			break;

		case F_PRINT: {
			const char *what = f->op ? "println" : "print";
			i32 fmt = first_of(f->node);
			fmt = next_of(fmt);
			if (f->phase == 0) {
				i32 codes = 0;
				if (fmt < 0 || g_nodes[fmt].kind != N_STR) {
					err_node(fmt < 0 ? f->node : fmt, "%s: the first argument must be a string literal (its %% codes are checked while compiling)", what);
					finish(T_ERR);
					break;
				}
				if (fmt_type(g_nodes[fmt].text, g_nodes[fmt].len, -1, &codes) == T_ERR) {
					err_node(fmt, "%s: unknown %% code; use %%d (int), %%f (float), %%s (string), %%b (bool) or %%%%", what);
					finish(T_ERR);
					break;
				}
				if (codes != f->k - 1) {
					err_node(fmt, "%s: the format has %ld %% code%s but %ld value%s follow%s", what, (long)codes,
					         codes == 1 ? "" : "s", (long)(f->k - 1), f->k == 2 ? "" : "s", f->k == 2 ? "s" : "");
					finish(T_ERR);
					break;
				}
				f->a = add_str(g_nodes[fmt].text, g_nodes[fmt].len);
				f->cur = next_of(fmt);
				f->k = 0;
				if (f->cur < 0) {
					emit(f->op ? OP_PRINTLN : OP_PRINT, f->a, 0, line);
					finish(T_UNIT);
					break;
				}
				f->phase = 1;
				push_frame(f->cur, F_START);
				break;
			}
			t = pop_type();
			t2 = fmt_type(g_nodes[fmt].text, g_nodes[fmt].len, f->k, 0);
			if (t != T_ERR && t != t2)
				err_node(f->cur, "%s: %% code %ld wants %s, this value is %s", what, (long)(f->k + 1), tn(t2), tn(t));
			f->k++;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) { push_frame(f->cur, F_START); break; }
			emit(f->op ? OP_PRINTLN : OP_PRINT, f->a, f->k, line);
			finish(T_UNIT);
			break;
		}

		case F_CONV:
		case F_LEN:
		case F_SH:
			if (f->phase == 0) {
				if (f->k != 1) { err_node(f->node, "this takes exactly one value"); finish(T_ERR); break; }
				f->phase = 1;
				push_frame(f->cur, F_START);
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
				push_frame(f->cur, F_START);
				break;
			}
			t = pop_type();
			if (t != T_ERR && t != fn->params[f->k])
				err_node(f->cur, NAME_FMT ": argument %ld is %s, it must be %s", fn->name, (long)(f->k + 1), tn(t), tn(fn->params[f->k]));
			f->k++;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) { push_frame(f->cur, F_START); break; }
			emit(OP_CALL, f->a, 0, line);
			finish(fn->ret);
			break;
		}

		default:
			err_node(f->node, "internal: unknown form");
			finish(T_ERR);
			break;
		}
	}
}

static o compile_expr(i32 n)
{
	i32 base = g_nwork;
	push_frame(n, F_START);
	run_work(base);
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
	if (is_reserved(g_nodes[name].text)) {
		err_node(name, NAME_FMT " is a built-in name", g_nodes[name].text);
		return;
	}
	for (p = first_of(params); p >= 0; p = next_of(p)) {
		i32 pn = first_of(p), colon = next_of(pn), pt = next_of(colon), q;
		if (g_nodes[p].kind != N_LIST || !is_sym(pn) || !sym_is(colon, ":") || pt < 0 || next_of(pt) >= 0) {
			err_node(p, "a parameter is (name : type)");
			return;
		}
		for (q = first_of(params); q != p; q = next_of(q)) {
			if (!strcmp(g_nodes[first_of(q)].text, g_nodes[pn].text)) err_node(pn, "parameter " NAME_FMT " twice", g_nodes[pn].text);
		}
		if (fn.nparams == MAX_PARAMS) { err_node(p, "at most %d parameters", MAX_PARAMS); return; }
		fn.params[fn.nparams] = parse_type(pt);
		if (fn.params[fn.nparams] == T_UNIT) err_node(pt, "a parameter cannot be unit");
		fn.nparams++;
	}
	fn.ret = ret >= 0 ? parse_type(ret) : (u8)T_UNIT;
	fn.line = g_nodes[n].line;
	fn.entry = -1;
	fn.decl_unit = g_unit_id;
	fn.decl_node = n;
	if ((fi = find_fn(g_nodes[name].text)) >= 0) {
		Fn *old = &g_fns[fi];
		if (!repl || old->nparams != fn.nparams || old->ret != fn.ret || memcmp(old->params, fn.params, fn.nparams)) {
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
	i32 name, params, ret, body, p, fi, jmp, k = 0;
	i32 s_slots = g_slots, s_max = g_maxslots, s_locals = g_nlocals, s_floor = g_local_floor;
	u8 t;
	if (!fn_shape(n, &name, &params, &ret, &body)) return;
	fi = find_fn(g_nodes[name].text);
	if (fi < 0 || g_fns[fi].decl_unit != g_unit_id || g_fns[fi].decl_node != n) return; /* declare_fn said why */
	jmp = emit(OP_JMP, -1, 0, g_nodes[n].line);
	g_fns[fi].entry = g_ncode;
	g_slots = g_maxslots = 0;
	g_local_floor = g_nlocals; /* the top level's locals live in another frame */
	for (p = first_of(params); p >= 0; p = next_of(p), k++) add_local(g_nodes[first_of(p)].text, g_fns[fi].params[k], new_slot());
	push_body(body, 1);
	run_work(g_nwork - 1);
	t = pop_type();
	if (g_fns[fi].ret == T_UNIT) {
		if (t != T_UNIT && t != T_ERR) emit(OP_POP, 0, 0, g_nodes[n].line);
		emit(OP_RET_U, 0, 0, g_nodes[n].line);
	} else {
		if (t != T_ERR && t != g_fns[fi].ret)
			err_node(name, NAME_FMT " must return %s, its body ends with %s", g_nodes[name].text, tn(g_fns[fi].ret), tn(t));
		emit(OP_RET_V, 0, 0, g_nodes[n].line);
	}
	g_fns[fi].nslots = g_maxslots;
	g_slots = s_slots;
	g_maxslots = s_max;
	g_nlocals = s_locals;
	g_local_floor = s_floor;
	patch(jmp, g_ncode);
}

/* compile_def DEF REPL: (def name value) or (def name : type value) - a global */
static o compile_def(i32 n, int repl)
{
	i32 name = next_of(first_of(n)), x, value, gi;
	u8 declared = T_NONE, t, type;
	if (!is_sym(name)) { err_node(name < 0 ? n : name, "def needs a name: (def hp : int 100)"); return; }
	x = next_of(name);
	value = x;
	if (sym_is(x, ":")) {
		declared = parse_type(next_of(x));
		value = next_of(next_of(x));
	}
	if (value < 0) { err_node(name, NAME_FMT " has no value", g_nodes[name].text); return; }
	if (next_of(value) >= 0) err_node(next_of(value), "def has one value");
	if (is_reserved(g_nodes[name].text)) { err_node(name, NAME_FMT " is a built-in name", g_nodes[name].text); return; }
	if (find_fn(g_nodes[name].text) >= 0) { err_node(name, NAME_FMT " is already a function", g_nodes[name].text); return; }
	compile_expr(value);
	t = pop_type();
	type = declared != T_NONE ? declared : t;
	if (declared != T_NONE && declared != T_ERR && t != T_ERR && t != declared)
		err_node(name, NAME_FMT " is declared %s, its value is %s", g_nodes[name].text, tn(declared), tn(t));
	if (type == T_UNIT) err_node(name, NAME_FMT " would hold no value (unit)", g_nodes[name].text);
	if ((gi = find_global(g_nodes[name].text)) >= 0) {
		if (!repl || g_globals[gi].type != type) {
			err_node(name, NAME_FMT " is already defined", g_nodes[name].text);
			return;
		}
	} else {
		if (g_nglobals == MAX_GLOBALS) die("too many globals");
		gi = g_nglobals++;
		g_globals[gi].name = arena_strndup(&g_perm, g_nodes[name].text, g_nodes[name].len);
		g_globals[gi].type = type;
	}
	emit(OP_STORE_G, gi, 0, g_nodes[name].line);
}

/* everything a failed unit may have added, so the REPL can forget it */
static struct {
	i32 ncode, nints, nfloats, nstrs, nfns, nglobals;
	size_t perm;
} g_snap;
static i32 g_snap_entry[MAX_FNS], g_snap_slots[MAX_FNS];

/* compile_unit TOP REPL: compile every top-level form; returns the entry point, or -1 after errors (and then nothing
 * of the unit stays). In the REPL the value of the last form is shown. */
static i32 compile_unit(i32 top, int repl)
{
	i32 n, entry, i;
	u8 t;
	g_snap.ncode = g_ncode;
	g_snap.nints = g_nints;
	g_snap.nfloats = g_nfloats;
	g_snap.nstrs = g_nstrs;
	g_snap.nfns = g_nfns;
	g_snap.nglobals = g_nglobals;
	g_snap.perm = g_perm.used;
	for (i = 0; i < g_nfns; i++) {
		g_snap_entry[i] = g_fns[i].entry;
		g_snap_slots[i] = g_fns[i].nslots;
	}
	g_unit_id++;
	g_nlocals = g_local_floor = 0;
	g_slots = g_maxslots = 0;
	g_ntypes = g_npatches = g_nwork = 0;

	for (n = top; n >= 0; n = next_of(n)) {
		if (head_is(n, "defn")) declare_fn(n, repl);
	}
	entry = emit(OP_ENTER, 0, 0, top >= 0 ? g_nodes[top].line : 1);
	for (n = top; n >= 0; n = next_of(n)) {
		if (head_is(n, "defn")) { compile_fn(n); continue; }
		if (head_is(n, "def")) { compile_def(n, repl); continue; }
		compile_expr(n);
		t = pop_type();
		if (repl && next_of(n) < 0 && t != T_UNIT && t != T_ERR) emit(OP_SHOW, t, 0, g_nodes[n].line);
		else if (t != T_UNIT && t != T_ERR) emit(OP_POP, 0, 0, g_nodes[n].line);
	}
	emit(OP_HALT, 0, 0, 0);
	g_code[entry].a = g_maxslots;
	if (!g_unit.nerrors) return entry;

	g_ncode = g_snap.ncode;
	g_nints = g_snap.nints;
	g_nfloats = g_snap.nfloats;
	g_nstrs = g_snap.nstrs;
	g_nfns = g_snap.nfns;
	g_nglobals = g_snap.nglobals;
	g_perm.used = g_snap.perm;
	for (i = 0; i < g_nfns; i++) {
		g_fns[i].entry = g_snap_entry[i];
		g_fns[i].nslots = g_snap_slots[i];
	}
	return -1;
}
