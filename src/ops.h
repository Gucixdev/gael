/* ops.h - every VM instruction, once. The enum, the names for the disassembler and the operand count all come
 * from this list (an x-macro), so they cannot drift apart.
 *
 * X(NAME, operands): operand a is a constant, string, slot, function or jump target; b is an argument count or a
 * type. Stack effects are in the comments; i = int, f = float, b = bool, s = string. */

#ifndef GAEL_OPS_H_
#define GAEL_OPS_H_

#define GAEL_OPS(X) \
	X(HALT, 0)    /* stop the program */ \
	X(ENTER, 1)   /* -- 0...      a: slots of the top-level frame, zeroed */ \
	X(PUSH_I, 1)  /* -- i         a: index into the int constants */ \
	X(PUSH_F, 1)  /* -- f         a: index into the float constants */ \
	X(PUSH_S, 1)  /* -- s         a: string index */ \
	X(PUSH_B, 1)  /* -- b         a: 0 or 1 */ \
	X(POP, 0)     /* x -- */ \
	X(LOAD_L, 1)  /* -- x         a: local slot */ \
	X(STORE_L, 1) /* x --         a: local slot */ \
	X(LOAD_G, 1)  /* -- x         a: global slot */ \
	X(STORE_G, 1) /* x --         a: global slot */ \
	X(ADD_I, 0) X(SUB_I, 0) X(MUL_I, 0) X(DIV_I, 0) X(MOD_I, 0) X(NEG_I, 0) \
	X(ADD_F, 0) X(SUB_F, 0) X(MUL_F, 0) X(DIV_F, 0) X(NEG_F, 0) \
	X(LT_I, 0) X(LE_I, 0) X(GT_I, 0) X(GE_I, 0) X(EQ_I, 0) X(NE_I, 0) \
	X(LT_F, 0) X(LE_F, 0) X(GT_F, 0) X(GE_F, 0) X(EQ_F, 0) X(NE_F, 0) \
	X(EQ_S, 0) X(NE_S, 0) \
	X(NOT, 0)     /* b -- b */ \
	X(JMP, 1)     /* --           a: target */ \
	X(JF, 1)      /* b --         jump to a when false */ \
	X(JT, 1)      /* b --         jump to a when true */ \
	X(I2F, 0)     /* i -- f */ \
	X(F2I, 0)     /* f -- i       truncates toward zero */ \
	X(LEN_S, 0)   /* s -- i       length in bytes */ \
	X(CALL, 1)    /* args -- r    a: function index */ \
	X(RET_V, 0)   /* r --         return with a value */ \
	X(RET_U, 0)   /* --           return without one */ \
	X(PRINT, 2)   /* args --      a: format string, b: argument count */ \
	X(PRINTLN, 2) /* args --      the same, then a newline */ \
	X(SH, 0)      /* s -- i       run a shell command, its exit status */ \
	X(SHOW, 1)    /* x --         the REPL's "=> value : type"; a: the type */

#define GAEL_OP_ENUM_(name, n) OP_##name,
typedef enum { GAEL_OPS(GAEL_OP_ENUM_) OP_COUNT_ } Op;

#endif
