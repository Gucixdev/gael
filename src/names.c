/* names.c - the top level's names in one hash index.
 *
 * Functions, globals, structs and enums, and macros share one namespace, and the built-in names are taken. A slot
 * holds a name once, with its number in each table that has it (-1 when that table does not), so finding a function
 * is a probe or a few instead of a walk over every function: compiling grew with the square of the number of
 * functions before. Open addressing with linear probing, FNV-1a; slots are never deleted - a failed REPL unit, which
 * takes names back, clears the index and fills it again from the tables (compile.c). */

#define NAME_SLOTS (1 << 15) /* a power of two, at least twice the names there can be */

typedef struct {
	const char *name; /* lives as long as the table entry it names */
	u32 hash;
	u8 reserved;      /* a form, an operator, a builtin or a built-in type */
	i32 fn, global, type, macro;
} NameSlot;

static NameSlot g_names[NAME_SLOTS];
static i32 g_nnames;

static u32 name_hash(const char *s)
{
	u32 h = 2166136261u;
	while (*s) {
		h ^= (u8)*s++;
		h *= 16777619u;
	}
	return h;
}

/* name_at NAME HASH: the slot that holds NAME, or the empty one where it would go */
static NameSlot *name_at(const char *s, u32 h)
{
	u32 i = h & (NAME_SLOTS - 1);
	while (g_names[i].name && (g_names[i].hash != h || strcmp(g_names[i].name, s))) i = (i + 1) & (NAME_SLOTS - 1);
	return &g_names[i];
}

/* names_get NAME: its slot, 0 when nothing has that name */
static const NameSlot *names_get(const char *s)
{
	const NameSlot *sl = name_at(s, name_hash(s));
	return sl->name ? sl : 0;
}

/* names_put NAME: its slot, made when it is new; NAME must outlive it */
static NameSlot *names_put(const char *s)
{
	u32 h = name_hash(s);
	NameSlot *sl = name_at(s, h);
	if (!sl->name) {
		if (g_nnames == NAME_SLOTS / 2) die("too many names (NAME_SLOTS)");
		sl->name = s;
		sl->hash = h;
		sl->reserved = 0;
		sl->fn = sl->global = sl->type = sl->macro = -1;
		g_nnames++;
	}
	return sl;
}

static o names_clear(void)
{
	memset(g_names, 0, sizeof(g_names));
	g_nnames = 0;
}
