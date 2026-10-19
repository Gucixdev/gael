/* read.c - source text to a flat tree of s-expressions.
 *
 * Every form is a Node in one array; a list links its children by index (first, next). The reader keeps its own
 * stack of open lists instead of recursing, so nesting depth is a number in this file, not the C stack.
 * Errors carry the line and column of the character that caused them. */

typedef enum { N_LIST, N_INT, N_FLOAT, N_STR, N_SYM, N_KEY } NodeKind;

typedef struct {
	u8 kind;
	i32 first, next; /* children and siblings by index, -1 for none */
	i32 line, col;
	i64 ival;
	f64 fval;
	const char *text; /* symbols, keywords, strings: NUL-terminated, in the unit arena */
	u32 len;
} Node;

#define MAX_NODES (1 << 18)
#define MAX_DEPTH 256
#define MAX_ERRORS 32

static Node g_nodes[MAX_NODES];
static i32 g_nnodes;

static u64 g_unit_mem[(8u << 20) / 8];
static Arena g_unit_arena = { (u8 *)g_unit_mem, sizeof(g_unit_mem), 0, "unit" };

typedef struct {
	const char *file;
	const char *src;
	size_t len;
	i32 nerrors;
} Unit;

static Unit g_unit;

static o show_source_line(i32 line, i32 col)
{
	const char *p = g_unit.src, *end = g_unit.src + g_unit.len, *eol;
	i32 l = 1, k;
	while (p < end && l < line) {
		if (*p++ == '\n') l++;
	}
	if (l != line || p >= end) return;
	eol = p;
	while (eol < end && *eol != '\n') eol++;
	fprintf(stderr, "  %.*s\n  ", (int)GAEL_MIN(eol - p, 200), p);
	for (k = 1; k < col && k < 200; k++) fputc(p[k - 1] == '\t' ? '\t' : ' ', stderr);
	fputs("^\n", stderr);
}

/* error_at LINE COL FORMAT ...: one diagnostic, "file:line:col: error: ...", with the source line under it. The
 * arguments must stay short (names are passed through %.*s with a cap), so the fixed buffer cannot overflow. */
static o error_at(i32 line, i32 col, const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	g_unit.nerrors++;
	if (g_unit.nerrors > MAX_ERRORS) {
		if (g_unit.nerrors == MAX_ERRORS + 1) fprintf(stderr, "%s: too many errors, stopping here\n", g_unit.file);
		return;
	}
	va_start(ap, fmt);
	vsprintf(msg, fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s:%ld:%ld: error: %s\n", g_unit.file, (long)line, (long)col, msg);
	show_source_line(line, col);
}

static i32 new_node(NodeKind kind, i32 line, i32 col)
{
	Node *n;
	if (g_nnodes == MAX_NODES) die("too many forms in one unit (MAX_NODES)");
	n = &g_nodes[g_nnodes];
	memset(n, 0, sizeof(*n));
	n->kind = (u8)kind;
	n->first = n->next = -1;
	n->line = line;
	n->col = col;
	return g_nnodes++;
}

static int is_delim(int c)
{
	return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')' || c == '"' || c == ';';
}

/* atom TEXT LEN: the kind of a token - an int, a float, a :keyword, else a symbol */
static NodeKind atom_kind(const char *s, size_t n)
{
	size_t i = 0, digits = 0;
	if (n > 1 && (s[0] == '-' || s[0] == '+')) i = 1;
	while (i < n && s[i] >= '0' && s[i] <= '9') i++, digits++;
	if (digits && i == n) return N_INT;
	if (digits && i < n && s[i] == '.') {
		size_t frac = 0;
		i++;
		while (i < n && s[i] >= '0' && s[i] <= '9') i++, frac++;
		if (frac && i < n && (s[i] == 'e' || s[i] == 'E')) {
			size_t exp = 0;
			i++;
			if (i < n && (s[i] == '-' || s[i] == '+')) i++;
			while (i < n && s[i] >= '0' && s[i] <= '9') i++, exp++;
			if (!exp) return N_SYM;
		}
		if (frac && i == n) return N_FLOAT;
	}
	if (s[0] == ':' && n > 1) return N_KEY;
	return N_SYM;
}

/* like_number TEXT LEN: starts with a digit (after an optional sign) or with '.' and a digit; such a token that is
 * not an int or a float is a typo in a number, not a name */
static int like_number(const char *s, size_t n)
{
	size_t i = (n > 1 && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
	if (i + 1 < n && s[i] == '.') i++;
	return i < n && s[i] >= '0' && s[i] <= '9';
}

/* read_unit: parse g_unit.src; returns the first top-level node (the rest follow by `next`), -1 when empty */
static i32 read_unit(void)
{
	const char *s = g_unit.src;
	size_t i = 0, n = g_unit.len;
	i32 line = 1, col = 1;
	i32 open[MAX_DEPTH];      /* the lists not closed yet */
	i32 last[MAX_DEPTH + 1];  /* last[0]: last top-level form; last[d]: last child of open[d - 1] */
	i32 depth = 0, top = -1, node, k;

	last[0] = -1;
	while (i < n) {
		char c = s[i];
		i32 l0 = line, c0 = col;
		if (c == '\n') { i++; line++; col = 1; continue; }
		if (c == ' ' || c == '\t' || c == '\r') { i++; col++; continue; }
		if (c == ';') {
			while (i < n && s[i] != '\n') i++, col++;
			continue;
		}
		if (c == ')') {
			i++, col++;
			if (!depth) { error_at(l0, c0, "unexpected ')'"); continue; }
			depth--;
			continue;
		}
		if (c == '(') {
			i++, col++;
			node = new_node(N_LIST, l0, c0);
		} else if (c == '"') {
			char *buf;
			size_t len = 0, j = i + 1;
			int closed = 0;
			/* escapes only shrink a string: the distance to the closing quote is enough room */
			while (j < n && s[j] != '"') j += (s[j] == '\\' && j + 1 < n) ? 2 : 1;
			buf = (char *)arena_alloc(&g_unit_arena, j - i + 1);
			i++, col++;
			while (i < n) {
				char d = s[i];
				if (d == '"') { i++, col++; closed = 1; break; }
				if (d == '\n') { line++; col = 1; buf[len++] = d; i++; continue; }
				if (d == '\\' && i + 1 < n) {
					char e = s[i + 1];
					i += 2, col += 2;
					switch (e) {
					case 'n': buf[len++] = '\n'; break;
					case 't': buf[len++] = '\t'; break;
					case '\\': buf[len++] = '\\'; break;
					case '"': buf[len++] = '"'; break;
					case '0': buf[len++] = '\0'; break;
					default: error_at(line, col - 2, "unknown escape \\%c in a string", e); break;
					}
					continue;
				}
				buf[len++] = d;
				i++, col++;
			}
			buf[len] = 0;
			if (!closed) error_at(l0, c0, "string not closed");
			node = new_node(N_STR, l0, c0);
			g_nodes[node].text = buf;
			g_nodes[node].len = (u32)len;
		} else {
			size_t start = i, len;
			NodeKind kind;
			i32 prev = last[depth];
			while (i < n && !is_delim((unsigned char)s[i])) i++, col++;
			len = i - start;
			if (s[start] == '.' && len > 1 && start > 0 && s[start - 1] == ')' && prev >= 0 && g_nodes[prev].kind == N_LIST) {
				/* (f x).pos.x reads as (. (f x) pos.x): the list just closed becomes that form, in place */
				i32 copy = new_node(N_LIST, g_nodes[prev].line, g_nodes[prev].col);
				i32 dot = new_node(N_SYM, l0, c0), path = new_node(N_SYM, l0, c0 + 1);
				g_nodes[copy].first = g_nodes[prev].first;
				g_nodes[dot].text = ".";
				g_nodes[dot].len = 1;
				g_nodes[path].text = arena_strndup(&g_unit_arena, s + start + 1, len - 1);
				g_nodes[path].len = (u32)(len - 1);
				g_nodes[prev].first = dot;
				g_nodes[dot].next = copy;
				g_nodes[copy].next = path;
				continue;
			}
			kind = atom_kind(s + start, len);
			node = new_node(kind, l0, c0);
			g_nodes[node].text = arena_strndup(&g_unit_arena, s + start, len);
			g_nodes[node].len = (u32)len;
			if (kind == N_INT) {
				u64 v = 0, limit;
				int neg = s[start] == '-';
				size_t j = (s[start] == '-' || s[start] == '+') ? 1 : 0;
				limit = neg ? (u64)1 << 63 : ((u64)1 << 63) - 1;
				for (; j < len; j++) {
					u64 dgt = (u64)(s[start + j] - '0');
					if (v > (limit - dgt) / 10) { error_at(l0, c0, "integer does not fit in 64 bits"); v = 0; break; }
					v = v * 10 + dgt;
				}
				g_nodes[node].ival = neg ? (i64)(0 - v) : (i64)v;
			} else if (kind == N_FLOAT) {
				g_nodes[node].fval = strtod(g_nodes[node].text, 0);
			} else if (kind == N_SYM && like_number(s + start, len)) {
				error_at(l0, c0, "%.*s is not a number; an int is written 42, a float 4.2 or 4.2e3",
					(int)(len < 64 ? len : 64), s + start); /* msg is 1024 bytes */
			}
		}
		/* link the new form under the innermost open list, or at the top level */
		if (!depth) {
			if (last[0] < 0) top = node; else g_nodes[last[0]].next = node;
			last[0] = node;
		} else {
			i32 parent = open[depth - 1];
			if (g_nodes[parent].first < 0) g_nodes[parent].first = node; else g_nodes[last[depth]].next = node;
			last[depth] = node;
		}
		if (g_nodes[node].kind == N_LIST) {
			if (depth == MAX_DEPTH) { error_at(l0, c0, "nested deeper than %d lists", MAX_DEPTH); return top; }
			open[depth++] = node;
			last[depth] = -1;
		}
	}
	for (k = depth; k > 0; k--) error_at(g_nodes[open[k - 1]].line, g_nodes[open[k - 1]].col, "'(' not closed");
	return top;
}
