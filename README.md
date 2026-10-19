# gael

gael is a small statically typed language written as s-expressions. The compiler checks every type before anything
runs and reports all the errors of a file in one go, each with its line, column and a caret under the mistake. A
program without errors is compiled to bytecode and run on a stack VM. gael is meant to become the scripting language
of a game engine and of an Emacs-like editor written in it (`docs/SPEC.md`, in Polish); this repository is the
language, version 0.1.

## Build and run

You need bash and a C compiler that accepts C89 (gcc or clang).

```sh
./build                     # writes ./gael, linked statically when the C library allows it
./gael examples/fact.gael   # run a file
./gael                      # the REPL
./run examples/fib.gael     # rebuild when a source changed, then run with these arguments
./test                      # the examples, the error cases and a REPL session, on every build
```

`./test` builds with gcc and with clang (`-std=c89 -pedantic -Wall -Wextra -Werror`), and once more with
AddressSanitizer and UndefinedBehaviorSanitizer when one of the compilers has their runtime installed.

Command line:

```
gael               the REPL
gael FILE          compile FILE; when there are no errors, run it
gael -c FILE       only compile: report every type error, run nothing
gael -d FILE       compile and print the bytecode
gael -e CODE       compile and run CODE
gael -h            this help
```

The exit status is 0 when everything went fine, 1 after a compile or runtime error, and 2 for bad usage.

## What it looks like

```lisp
; recursion: the call stack lives in the VM, the C code never recurses
(defn fact ((n : int)) : int
  (if (<= n 1)
      1
      (* n (fact (- n 1)))))

(println "20! = %d" (fact 20))
```

```
$ ./gael examples/fact.gael
20! = 2432902008176640000
```

This file has three mistakes. gael reports all three and runs nothing:

```lisp
(defn f ((a : int)) : int
  (+ a true))
(println "%s" 1)
(set nothing 3)
```

```
$ ./gael tests/errors/three-errors.gael
tests/errors/three-errors.gael:2:8: error: + works on int and float, not bool
    (+ a true))
         ^
tests/errors/three-errors.gael:3:15: error: println: % code 1 wants string, this value is int
  (println "%s" 1)
                ^
tests/errors/three-errors.gael:4:6: error: set: unknown name nothing
  (set nothing 3)
       ^
```

The REPL keeps definitions between forms, shows each value with its type, and reads a form over several lines until
its parentheses close:

```
$ ./gael
gael 0.1 - :q quits, a form may span lines
gael> (def hp 100)
gael> (- hp 30)
=> 70 : int
gael> (defn sq ((n : int)) : int
  ...   (* n n))
gael> (sq 12)
=> 144 : int
gael> (+ hp 0.5)
<repl>:1:7: error: +: int and float do not mix; convert one with (float x) or (int x)
  (+ hp 0.5)
        ^
gael> :q
```

`gael -d examples/fact.gael` prints the bytecode, one instruction per line with its operands, the constant it uses
and the source line it came from.

## The language in 0.1

Types: `int` (64-bit, arithmetic wraps around), `float` (64-bit), `bool`, `string` and `unit` (no value). Nothing
converts on its own: `(+ 1 2.0)` is a compile error, `(float x)` and `(int x)` convert on purpose. A comment starts
with `;`.

| Form | Meaning |
|---|---|
| `(def hp : int 100)`, `(def hp 100)` | a global; without `: type` the type is the value's |
| `(defn name ((x : int) (y : float)) : int body...)` | a function, `()` when it takes nothing; without `: type` it returns unit. A function can be called above its definition |
| `(let ((x 1) (y : float 2.0)) body...)` | locals for the body |
| `(set x 5)` | assign to a local or a global; the value must have its type |
| `(if c then else)`, `(when c body...)`, `(while c body...)`, `(do body...)` | a condition must be a bool; `if` without `else` is unit |
| `(and a b...)`, `(or a b...)`, `(not a)` | on bools; `and` and `or` stop at the first answer |
| `(+ a b...)`, `-`, `*`, `/` | int with int or float with float; `(- x)` negates |
| `(% a b)` | the remainder of two ints |
| `<`, `<=`, `>`, `>=`, `=`, `!=` | compare two ints or two floats; `=` and `!=` also take bools and strings |
| `(print "fmt" args...)`, `(println "fmt" args...)` | `%d` int, `%f` float, `%s` string, `%b` bool, `%%`; the codes are checked against the arguments when compiling |
| `(len s)` | the length of a string in bytes |
| `(sh "command")` | run a shell command, get its exit status |

A function's last form is its value. An integer literal must fit in 64 bits, and a float needs digits on both sides
of its point (`4.2`, `4.2e3`; `1e3` and `.5` are errors). A runtime error stops the program and names the line it
happened on:

```
$ ./gael tests/errors/div-zero.gael
before
tests/errors/div-zero.gael:1: runtime error: division by zero
```

## How it is built

- C89 in one translation unit (`src/main.c` includes the rest), with the fixed-size types `i8`..`i64`, `u8`..`u64`,
  `f32`, `f64` and `o` for void (`src/base.h`).
- No `malloc`: every table is a static array and every arena a static block. Running out of one is a fatal error that
  names it.
- No recursion in C. The reader keeps its own stack of open lists, the compiler runs a work stack of frames (type
  checking and code generation happen in the same pass), and the VM keeps its own call stack, so deep recursion in a
  gael program is a gael runtime error, not a crash.
- Every instruction is listed once in `src/ops.h` (an x-macro); the opcode enum, the disassembler's names and the
  operand counts come from that list.
- The VM's cells carry no type tag: the compiler has already proved the types, so `ADD_I` reads two ints.

## Status

Version 0.1, tested on Linux x86-64 with gcc 13.3 and clang 18.1. `./test` passes 23 checks per build (six examples,
16 compile and runtime error cases, one REPL session) with gcc, clang and gcc with both sanitizers.

Not in 0.1: structs, arrays and other compound types, string building, modules and gems (packages, see the spec),
calling C, and the editor. The fixed limits are 8 parameters per function, 4096 functions, 4096 globals and 2^18
instructions; per unit (a file, or one REPL form) 2^18 syntax nodes and an 8 MiB arena for the source and the text
of its names and strings; at run time at most 4096 nested calls.

## License

MIT, see `LICENSE`.
