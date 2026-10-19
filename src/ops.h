/* ops.h - every VM instruction, once. The enum, the names for the disassembler and the operand count all come
 * from this list (an x-macro), so they cannot drift apart.
 *
 * X(NAME, operands): operand a is a constant, string, slot, function, type, format or jump target; b is a count.
 * Stack effects are in the comments; i = int, f = float, b = bool, s = string, x... = a value of several cells. */

#ifndef GAEL_OPS_H_
#define GAEL_OPS_H_

#define GAEL_OPS(X) \
	X(HALT, 0)     /* stop the program */ \
	X(ENTER, 2)    /* -- 0...      a: slots of the top-level frame, zeroed; b: its deepest operand stack */ \
	X(PUSH_I, 1)   /* -- i         a: index into the int constants */ \
	X(PUSH_F, 1)   /* -- f         a: index into the float constants */ \
	X(PUSH_S, 1)   /* -- s         a: string constant */ \
	X(PUSH_K, 1)   /* -- k         a: the value itself (a bool, an enum variant, an option tag) */ \
	X(ZERO, 1)     /* -- 0...      a: how many zero cells (the inside of none) */ \
	X(POP, 0)      /* x -- */ \
	X(POPN, 1)     /* x... --      a: cells */ \
	X(LOAD_L, 1)   /* -- x         a: local slot */ \
	X(STORE_L, 1)  /* x --         a: local slot */ \
	X(LOAD_G, 1)   /* -- x         a: global cell */ \
	X(STORE_G, 1)  /* x --         a: global cell */ \
	X(LOADN_L, 2)  /* -- x...      a: first slot, b: cells */ \
	X(STOREN_L, 2) /* x... --      a: first slot, b: cells */ \
	X(LOADN_G, 2)  /* -- x...      a: first global cell, b: cells */ \
	X(STOREN_G, 2) /* x... --      a: first global cell, b: cells */ \
	X(ADD_I, 0) X(SUB_I, 0) X(MUL_I, 0) X(DIV_I, 0) X(MOD_I, 0) X(NEG_I, 0) \
	X(ADD_F, 0) X(SUB_F, 0) X(MUL_F, 0) X(DIV_F, 0) X(NEG_F, 0) \
	X(LT_I, 0) X(LE_I, 0) X(GT_I, 0) X(GE_I, 0) X(EQ_I, 0) X(NE_I, 0) \
	X(LT_F, 0) X(LE_F, 0) X(GT_F, 0) X(GE_F, 0) X(EQ_F, 0) X(NE_F, 0) \
	X(EQ_S, 0) X(NE_S, 0) \
	X(NOT, 0)      /* b -- b */ \
	X(JMP, 1)      /* --           a: target */ \
	X(JF, 1)       /* b --         jump to a when false */ \
	X(JT, 1)       /* b --         jump to a when true */ \
	X(I2F, 0)      /* i -- f */ \
	X(F2I, 0)      /* f -- i       truncates toward zero */ \
	X(LEN_S, 0)    /* s -- i       length in bytes */ \
	X(CALL, 1)     /* args -- r    a: function index */ \
	X(RET_V, 0)    /* r --         return with a one-cell value */ \
	X(RET_N, 1)    /* r... --      return with a value of a cells */ \
	X(RET_U, 0)    /* --           return without a value */ \
	X(PRINT, 2)    /* args --      a: format record (the string and the type of each value), b: 1 for stderr */ \
	X(PRINTLN, 2)  /* args --      the same, then a newline */ \
	X(FORMAT, 1)   /* args -- s    the same, into a new string */ \
	X(SH, 0)       /* s -- s       run a shell command, its output without the newlines at the end */ \
	X(SYSTEM, 0)   /* s -- i       run a shell command, its exit status */ \
	X(PARSE_I, 0)  /* s -- o       (option int): the string as an int, or none */ \
	X(PARSE_F, 0)  /* s -- o       (option float) */ \
	X(SUBSTR, 0)   /* s i i -- s   the bytes from the first index up to the second, the same text (no copy) */ \
	X(BYTE, 0)     /* s i -- i     one byte, 0 to 255 */ \
	X(FIND, 0)     /* s s -- o     (option int): where the second string first starts in the first */ \
	X(SPLIT, 0)    /* s s -- v     (vec string): the pieces between the separators (no copies) */ \
	X(JOIN, 0)     /* v s -- s     the strings of a vec with the separator between them, a new string */ \
	X(CHAR, 0)     /* i -- s       the one-byte string of 0 to 255 */ \
	X(TRIM, 0)     /* s -- s       without the spaces, tabs and newlines around it (no copy) */ \
	X(READ_FILE, 0)  /* s -- o     (option string): the whole file, or none when it cannot be read */ \
	X(WRITE_FILE, 0) /* s s -- b   write the second string to the file named by the first; false on failure */ \
	X(READ_LINE, 0)  /* -- o       (option string): one line of standard input without its newline; none at the end */ \
	X(GETENV, 0)   /* s -- o       (option string): an environment variable */ \
	X(ARGS, 0)     /* -- v         (vec string): the arguments after the file name */ \
	X(EXIT, 0)     /* i --         end the program with this exit status */ \
	X(INC_L, 1)    /* --           a: local slot; adds 1 to the int in it */ \
	X(VEC_NEW, 2)  /* x... -- v    a: elements on the stack, b: cells of one */ \
	X(VEC_LEN, 0)  /* v -- i */ \
	X(VEC_PUSH, 1) /* v x... --    a: cells of an element */ \
	X(VEC_GET, 1)  /* v i -- x...  a: cells of an element; the index is checked */ \
	X(VEC_PUT, 1)  /* v i x... --  a: cells of an element; the index is checked */ \
	X(VEC_POP, 1)  /* v -- o...    (option element): the last one, taken off; none when empty */ \
	X(SHOW, 1)     /* x... --      the REPL's "=> value : type"; a: the type */

#define GAEL_OP_ENUM_(name, n) OP_##name,
typedef enum { GAEL_OPS(GAEL_OP_ENUM_) OP_COUNT_ } Op;

#endif
