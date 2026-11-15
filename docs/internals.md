## How it is built

- C89 in one translation unit (`src/main.c` includes the rest), with the fixed-size types `i8`..`i64`, `u8`..`u64`,
  `f32`, `f64` and `o` for void (`src/base.h`), the same names as gael's own sized numbers.
- No `malloc`: every table that grows with the program (nodes, code, functions, types, names, macros) and the VM's
  heap and text arena are each one reservation of address space (mmap with `MAP_NORESERVE`), which the system fills
  with pages only where something is written, so the limits are generous and cost nothing until they are reached; the
  rest are small static arrays. Running out of a table is a fatal error that names it.
- No recursion in C. The reader keeps its own stack of open lists, the compiler runs a work stack of frames (type
  checking and code generation happen in the same pass), and the VM keeps its own call stack, so deep recursion in a
  gael program is a gael runtime error, not a crash. A tail call (`TAILCALL`) moves its arguments down over its own
  frame and jumps, after the compiler has let go of the frame's locals, so it adds no frame.
- A function value is one cell: minus the function's number plus one, or a closure - an object with the function's
  number and the values it keeps, which a `fn` reads and sets at offsets in it (its handle comes in slot 0). An
  indirect call puts a closure there, or 0 for a plain function made by `fn`.
- A coroutine has its own value stack and frames, blocks on the heap that double when a call needs more; switching to a
  coroutine or back saves one set of registers (stack, frames, pc, sp, fp) and loads the other. A coroutine's function
  returns to a `CORO_END` instruction that keeps the result in the coroutine object. The main program's waits re-run
  their own instruction after each coroutine they let run, so the scheduler needs no loop in C around the interpreter.
- Macros (`src/macro.c`) are checked when defined, kept in a table of their own (a unit's nodes are gone when the next
  one is read) and matched and copied with explicit stacks. Hygiene is a stamp: each symbol a template brings in
  carries the stamp of its expansion, and the compiler finds a local by its name and that stamp.
- The top level's names (functions, globals, types, macros) and the built-in ones are in one hash index
  (`src/names.c`), so compiling grows with the size of the program, not with its square.
- Every instruction is listed once in `src/ops.h` (an x-macro); the opcode enum, the disassembler's names, the
  operand counts and, with gcc and clang, the table of label addresses the VM dispatches through come from that list.
  Each instruction's code jumps straight to the next one's (computed goto, as Lua does; a plain switch with other
  compilers).
- In `if`, `when` and `while`, `and`, `or`, `not`, `nand` and `nor` compile to jumps alone, the short circuit as code:
  `(when (or (< x 0.0) (> x w)) ...)` makes no bool and tests none.
- Once a unit compiles, runs of simple instructions are fused into one superinstruction: a local or a constant fed
  straight into arithmetic (`(- n 1)` is one `SUB_I_LK`), a comparison straight into a jump, the step at the end of a
  `for`. The rest of a run becomes NOPs the superinstruction skips, so nothing moves and no jump changes; `gael -d`
  shows the fused code.
- A map keeps its entries in insertion order and an index of slots; each slot holds an entry's number and 32 bits of
  its key's hash, so a probe reads an entry only when those agree.
- A vec and a pool keep their elements as columns, one per cell of the element type (SoA); a column of bools is a run
  of bits, so `(vec bool)` and the bool fields of structs in them take one bit each.
- The VM's cells carry no type tag: the compiler has already proved the types, so `ADD_I` reads two ints. A value
  takes as many cells as its type says: one for a number, a bool, a string, an enum value or a vec, the sum of its
  fields for a struct, and one more than the value inside for an option. A string is one cell too: the program's own
  strings point into the text arena and are not counted, a string made at run time is an object on the heap.
  `substr`, `split` and `trim` point into the program's own text and copy out of a string made at run time.
- The heap is that one array with a buddy allocator: blocks of 2^k cells, split to fit and merged with their free
  neighbour when freed. A growing vec doubles in place when the block after it is free. Objects refer to each other by
  index, not by pointer. Freeing is a loop over a chain of objects whose count reached 0, not a recursion.
- After the heap's cells comes an area as big for arenas: a stack of bump regions, one for each `(arena ...)` that is
  running. A new object goes to the innermost one; freeing a block there does nothing, and the region goes back in
  one step when its arena ends. Each arena counts its objects that are alive, which is how it knows that nothing
  made in it is still held.
- A vec of structs is stored as columns, one per field (SoA), so going over one field of every element reads one
  run of memory; the program sees ordinary values, `get` gathers an element and `put` spreads it out again.
- The compiler counts how deep each function's operand stack can get, and the VM checks that the whole frame fits
  before it enters it, so no instruction can write past the stack.
