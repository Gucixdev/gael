/* vm.c - the bytecode machine: one operand stack of untyped cells and a stack of call frames.
 *
 * The compiler already proved every type, so a cell carries no tag: ADD_I reads two ints, ADD_F two floats. Strings
 * are indexes into the string table. A struct or an option is a run of cells. Integer arithmetic wraps (done in u64,
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
static Cell g_vstack[VM_STACK];
static VFrame g_vframes[VM_FRAMES];

static int vm_error(i32 line, const char *msg)
{
	fflush(stdout);
	fprintf(stderr, "%s:%ld: runtime error: %s\n", g_unit.file, (long)line, msg);
	return 1;
}

static o print_float(FILE *out, f64 v)
{
	char buf[64];
	sprintf(buf, "%.15g", v);
	fputs(buf, out);
	if (!strpbrk(buf, ".eni")) fputs(".0", out); /* 2.0 stays visibly a float; inf and nan stay as they are */
}

/* print_quoted OUT STR: a string inside a struct or an option, written the way the source would write it */
static o print_quoted(FILE *out, const Str *s)
{
	u32 i;
	putc('"', out);
	for (i = 0; i < s->len; i++) {
		char c = s->p[i];
		if (c == '"' || c == '\\') { putc('\\', out); putc(c, out); }
		else if (c == '\n') fputs("\\n", out);
		else if (c == '\t') fputs("\\t", out);
		else putc(c, out);
	}
	putc('"', out);
}

#define PRINT_DEPTH 64

/* print_value OUT TYPE CELLS: one value, as the REPL shows it: (Vec2 :x 1.0 :y 2.0), (some 3), none, north */
static o print_value(FILE *out, Ty type, const Cell *c)
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
		if (ti->kind == K_STRUCT || ti->kind == K_OPTION) {
			const Field *fd = 0;
			if (*i < 0) {
				if (ti->kind == K_OPTION && !v->i) { fputs("none", out); n--; continue; }
				fprintf(out, ti->kind == K_OPTION ? "(some" : "(%s", ti->name);
				*i = 0;
			}
			if (ti->kind == K_STRUCT && *i < ti->n) fd = &g_fields[ti->first + *i];
			if (ti->kind == K_OPTION && *i < 1) putc(' ', out);
			else if (!fd) { putc(')', out); n--; continue; }
			else fprintf(out, " :%s ", fd->name);
			(*i)++;
			if (n == PRINT_DEPTH) { fputs("...", out); continue; }
			st[n].t = fd ? fd->type : ti->elem;
			st[n].i = -1;
			st[n].c = fd ? v + fd->offset : v + 1;
			n++;
			continue;
		}
		if (ti->kind == K_ENUM) fputs(g_variants[ti->first + v->i], out);
		else if (st[n - 1].t == T_INT) fprintf(out, "%ld", (long)v->i);
		else if (st[n - 1].t == T_FLOAT) print_float(out, v->f);
		else if (st[n - 1].t == T_BOOL) fputs(v->i ? "true" : "false", out);
		else if (st[n - 1].t == T_STR && n > 1) print_quoted(out, &g_strs[v->i]);
		else if (st[n - 1].t == T_STR) fwrite(g_strs[v->i].p, 1, g_strs[v->i].len, out);
		else fputs("()", out);
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
		char c = fmt->p[i];
		if (c != '%') { putchar(c); continue; }
		if (fmt->p[++i] == '%') { putchar('%'); continue; }
		print_value(stdout, g_fmt_tys[fm->first + k], args);
		args += cells(g_fmt_tys[fm->first + k]);
		k++;
	}
}

static int str_eq(i64 a, i64 b)
{
	return g_strs[a].len == g_strs[b].len && !memcmp(g_strs[a].p, g_strs[b].p, g_strs[a].len);
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
		case OP_PUSH_S: s[sp++].i = in->a; break;
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
		case OP_LEN_S: s[sp - 1].i = (i64)g_strs[s[sp - 1].i].len; break;

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
			print_fmt(&g_fmts[in->a], s + sp);
			if (in->op == OP_PRINTLN) putchar('\n');
			break;
		case OP_SH: {
			int st;
			fflush(stdout);
			st = system(g_strs[s[sp - 1].i].p);
			s[sp - 1].i = st == -1 ? -1 : (st >> 8) & 0xff; /* the exit status as the shell reports it (Linux) */
			break;
		}
		case OP_SHOW:
			sp -= cells((Ty)in->a);
			fputs("=> ", stdout);
			print_value(stdout, (Ty)in->a, s + sp);
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
		if (in->op == OP_PUSH_F) { fputs("\t(", stdout); print_float(stdout, g_floats[in->a]); putchar(')'); }
		if (in->op == OP_PUSH_S) printf("\t(\"%.40s\")", g_strs[in->a].p);
		if (in->op == OP_PRINT || in->op == OP_PRINTLN) printf("\t(\"%.40s\")", g_strs[g_fmts[in->a].str].p);
		if (in->op == OP_CALL) printf("\t(%.40s)", g_fns[in->a].name);
		if (in->op == OP_SHOW) printf("\t(%.40s)", tn((Ty)in->a));
		if (in->line) printf("\t; line %ld", (long)in->line);
		putchar('\n');
	}
}
