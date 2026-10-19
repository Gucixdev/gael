# gael

gael is a small statically typed language written as s-expressions. The compiler checks every type before anything
runs and reports all the errors of a file in one go, each with its line, column and a caret under the mistake. A
program without errors is compiled to bytecode and run on a stack VM. gael is meant to become the scripting language
of a game engine and of an Emacs-like editor written in it (`docs/SPEC.md`, in Polish); this repository is the
language, version 0.2.

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

Structs are values: passing one to a function or storing it copies it, and nothing is allocated.

```lisp
(struct Vec2 (x : float) (y : float))
(struct Body (name : string) (pos : Vec2) (vel : Vec2))

(defn add ((a : Vec2) (b : Vec2)) : Vec2
  (Vec2 (+ a.x b.x) (+ a.y b.y)))

(defn step ((b : Body) (dt : float)) : Body
  (set b.pos (add b.pos (Vec2 (* b.vel.x dt) (* b.vel.y dt))))
  b)

; fields in order, or by name in any order
(def ball (Body :name "ball" :pos (Vec2 0.0 10.0) :vel (Vec2 :y -2.0 :x 1.5)))

(let ((later (step (step ball 0.5) 0.5)))
  (println "%s moved to %f, %f" later.name later.pos.x later.pos.y)
  (println "%v" later))
(println "the original is still at %v" ball.pos)
```

```
$ ./gael examples/structs.gael
ball moved to 1.5, 8.0
(Body :name "ball" :pos (Vec2 :x 1.5 :y 8.0) :vel (Vec2 :x 1.5 :y -2.0))
the original is still at (Vec2 :x 0.0 :y 10.0)
```

A `match` must handle every value. These four each miss one:

```lisp
(enum Dir north east south west)
(defn name ((d : Dir)) : string
  (match d
    (north "n")
    (east "e")))
(defn yes ((b : bool)) : int
  (match b (true 1)))
(defn opt ((x : (option int))) : int
  (match x ((some v) v)))
(defn count ((n : int)) : string
  (match n (0 "zero") (1 "one")))
```

```
$ ./gael tests/errors/match-missing.gael
tests/errors/match-missing.gael:3:3: error: match on Dir does not cover south, west; add arms for them or a _ arm
    (match d
    ^
tests/errors/match-missing.gael:7:3: error: match on bool does not cover false; add an arm for it or a _ arm
    (match b (true 1)))
    ^
tests/errors/match-missing.gael:9:3: error: match on (option int) does not cover none; add an arm for it or a _ arm
    (match x ((some v) v)))
    ^
tests/errors/match-missing.gael:11:3: error: match on int needs a _ arm for the values the arms do not list
    (match n (0 "zero") (1 "one")))
    ^
```

The REPL keeps definitions between forms, shows each value with its type, and reads a form over several lines until
its parentheses close:

```
$ ./gael
gael 0.2 - :q quits, a form may span lines
gael> (def hp 100)
gael> (- hp 30)
=> 70 : int
gael> (struct Vec2 (x : float) (y : float))
gael> (defn scale ((v : Vec2) (k : float)) : Vec2
  ...   (Vec2 (* v.x k) (* v.y k)))
gael> (scale (Vec2 :y 2.0 :x 1.0) 3.0)
=> (Vec2 :x 3.0 :y 6.0) : Vec2
gael> (+ hp 0.5)
<repl>:1:7: error: +: int and float do not mix; convert one with (float x) or (int x)
  (+ hp 0.5)
        ^
gael> :q
```

`gael -d examples/fact.gael` prints the bytecode, one instruction per line with its operands, the constant it uses
and the source line it came from.

## The language in 0.2

Types: `int` (64-bit, arithmetic wraps around), `float` (64-bit), `bool`, `string`, `unit` (no value), structs,
enums and `(option type)`. Nothing converts on its own: `(+ 1 2.0)` is a compile error, `(float x)` and `(int x)`
convert on purpose. A comment starts with `;`.

| Form | Meaning |
|---|---|
| `(def hp : int 100)`, `(def hp 100)` | a global; without `: type` the type is the value's |
| `(defn name ((x : int) (y : float)) : int body...)` | a function, `()` when it takes nothing; without `: type` it returns unit. A function can be called above its definition |
| `(let ((x 1) (y : float 2.0)) body...)` | locals for the body |
| `(set x 5)`, `(set b.pos.x 3.0)` | assign to a variable or a field of one; the value must have its type |
| `(if c then else)`, `(when c body...)`, `(while c body...)`, `(do body...)` | a condition must be a bool; `if` without `else` is unit |
| `(and a b...)`, `(or a b...)`, `(not a)` | on bools; `and` and `or` stop at the first answer |
| `(+ a b...)`, `-`, `*`, `/` | int with int or float with float; `(- x)` negates |
| `(% a b)` | the remainder of two ints |
| `<`, `<=`, `>`, `>=`, `=`, `!=` | compare two ints or two floats; `=` and `!=` also take bools, strings and enum values |
| `(struct Vec2 (x : float) (y : float))` | a struct; a field can have any type declared above it, other structs and options included |
| `(Vec2 1.0 2.0)`, `(Vec2 :y 2.0 :x 1.0)` | a struct value, with the fields in order or named; every field must be given |
| `v.x`, `b.pos.x` | a field of a variable |
| `(enum Dir north east south west)`, `Dir.north` | an enum and one of its values |
| `(some 5)`, `none` | the two kinds of `(option int)` value; `none` takes its type from where it goes: a declared binding, a parameter, a field, a return type, or the else branch of an `if` whose then branch has one |
| `(match x (pattern body...)...)` | patterns are `_`, int and string literals, `true` and `false`, enum values (`north` or `Dir.north`), `(some name)` and `none`; every value must be covered, and an arm that can never run is an error |
| `(print "fmt" args...)`, `(println "fmt" args...)` | `%d` int, `%f` float, `%s` string, `%b` bool, `%v` any value, `%%`; the codes are checked against the arguments when compiling |
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
- The VM's cells carry no type tag: the compiler has already proved the types, so `ADD_I` reads two ints. A value
  takes as many cells as its type says: one for a number, a bool, a string or an enum value, the sum of its fields
  for a struct, and one more than the value inside for an option.
- The compiler counts how deep each function's operand stack can get, and the VM checks that the whole frame fits
  before it enters it, so no instruction can write past the stack.

## Status

Version 0.2, tested on Linux x86-64 with gcc 13.3 and clang 18.1. `./test` passes 35 checks per build (nine examples,
25 compile and runtime error cases, one REPL session) with gcc, clang and gcc with both sanitizers.

Not in 0.2: arrays, maps and other collections, building strings at run time (and `sh` returning a command's
output), modules and gems (packages, see the spec), calling C, and the editor. The fixed limits are 8 parameters per
function, 4096 functions, 4096 globals, 4096 types and 2^18 instructions; per unit (a file, or one REPL form) 2^18
syntax nodes and an 8 MiB arena for the source and the text of its names and strings; at run time a stack of 65536
cells and at most 4096 nested calls.

## License

MIT, see `LICENSE`.
