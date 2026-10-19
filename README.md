# gael

gael is a small statically typed language written as s-expressions. The compiler checks every type before anything
runs and reports all the errors of a file in one go, each with its line, column and a caret under the mistake. A
program without errors is compiled to bytecode and run on a stack VM. gael is meant to become the scripting language
of a game engine and of an Emacs-like editor written in it (`docs/SPEC.md`, in Polish); this repository is the
language, version 0.3.

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
gael                   the REPL
gael FILE ARGS...      compile FILE; when there are no errors, run it; (args) gives it ARGS
gael -c FILE           only compile: report every type error, run nothing
gael -d FILE           compile and print the bytecode
gael -e CODE ARGS...   compile and run CODE
gael -h                this help
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
gael 0.3 - :q quits, a form may span lines
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

`examples/` has one file per feature with its output next to it (`NAME.out`). `gael -d examples/fact.gael` prints
the bytecode, one instruction per line with its operands, the constant it uses
and the source line it came from.

## The language in 0.3

Types: `int` (64-bit, arithmetic wraps around), `float` (64-bit), `bool`, `string`, `unit` (no value), structs,
enums, `(option type)`, `(vec type)` and `(map key value)`. Nothing converts on its own: `(+ 1 2.0)` is a compile error, `(float x)` and `(int x)`
convert on purpose. A comment starts with `;`.

| Form | Meaning |
|---|---|
| `(use "lib/geometry.gael")` | compile that file first, its path relative to this one; a file used by several others is compiled once, and a file that ends up using itself is an error |
| `(def hp : int 100)`, `(def hp 100)` | a global; without `: type` the type is the value's |
| `(defn name ((x : int) (y : float)) : int body...)` | a function, `()` when it takes nothing; without `: type` it returns unit. A function can be called above its definition |
| `(let ((x 1) (y : float 2.0)) body...)` | locals for the body |
| `(set x 5)`, `(set b.pos.x 3.0)` | assign to a variable or a field of one; the value must have its type |
| `(if c then else)`, `(when c body...)`, `(while c body...)`, `(do body...)` | a condition must be a bool; `if` without `else` is unit |
| `(and a b...)`, `(or a b...)`, `(not a)` | on bools; `and` and `or` stop at the first answer |
| `(&& a b...)`, `(\|\| a b...)` | the same, but an argument may also be a command, true when it exits with 0, as in bash: `(&& "make" "make install")` |
| `(+ a b...)`, `-`, `*`, `/` | int with int or float with float; `(- x)` negates |
| `(% a b)` | the remainder of two ints |
| `<`, `<=`, `>`, `>=`, `=`, `!=` | compare two ints or two floats; `=` and `!=` also take bools, strings and enum values |
| `(struct Vec2 (x : float) (y : float))` | a struct; a field can have any type declared above it, other structs and options included |
| `(Vec2 1.0 2.0)`, `(Vec2 :y 2.0 :x 1.0)` | a struct value, with the fields in order or named; every field must be given |
| `v.x`, `b.pos.x`, `(get bodies 0).pos.x` | a field of a variable, or of any expression's value: `(expr).name` right after the parenthesis reads as `(. expr name)` |
| `(enum Dir north east south west)`, `Dir.north` | an enum and one of its values |
| `(vec 1 2 3)`, `(vec)` | a vec; an empty one takes its element type from where it goes, like `none` |
| `(push v x)`, `(get v i)`, `(put v i x)`, `(pop v)`, `(len v)` | add at the end, read, replace, take the last one off (an option), count; `get` and `put` check the index at run time |
| `(map ("a" 1) ("b" 2))`, `(map)` | a map; the key is an int, a bool, a string or an enum value; the entries stay in the order they went in |
| `(insert m k v)`, `(lookup m k)`, `(has m k)`, `(remove m k)` | add or replace, find (an option), check, take out (an option of the old value) |
| `(keys m)`, `(values m)`, `(len m)` | the keys and the values as vecs, in order; the number of entries |
| `(for (x v) body...)`, `(for (k m) body...)`, `(for (i 0 n) body...)` | go over a vec (`x` is a copy of each element), over a map's keys, or over 0, 1, ..., n - 1 |
| `(some 5)`, `none` | the two kinds of `(option int)` value; `none` takes its type from where it goes: a declared binding, a parameter, a field, a return type, or the else branch of an `if` whose then branch has one |
| `(match x (pattern body...)...)` | patterns are `_`, int and string literals, `true` and `false`, enum values (`north` or `Dir.north`), `(some name)` and `none`; every value must be covered, and an arm that can never run is an error |
| `(print "fmt" args...)`, `(println "fmt" args...)` | `%d` int, `%f` float, `%s` string, `%b` bool, `%v` any value, `%%`; the codes are checked against the arguments when compiling. `eprint` and `eprintln` write to stderr |
| `(format "fmt" args...)` | the same codes as `println`, checked the same way, into a new string |
| `(len s)` | the length of a string in bytes |
| `(sh "command")` | run a command with `/bin/sh`, get what it printed, without the newlines at the end |
| `(system "command")` | run a command, its output goes where gael's goes; get its exit status |
| `(\|> x (f a) g)` | bash's pipe for values: each step gets the value so far as its first argument, so this is `(g (f x a))` |
| `(\| "cmd" "cmd"...)`, `(<<< text "cmd"...)` | a pipeline of commands and its output; `<<<` gives the first command `text` on stdin |
| `(> text path)`, `(>> text path)` | write or append, in bash's order, `true` when it worked; `>` on numbers still compares |
| `($ "cmd")`, `$$`, `$HOME` | like bash's `$(cmd)`, the process id, an environment variable (`""` when it is not set) |
| `(parse-int s)`, `(parse-float s)` | `(some n)` when the whole string, spaces around it aside, is a number; otherwise `none` |
| `(substr s from to)`, `(byte s i)`, `(trim s)` | the bytes from `from` up to `to`, one byte as an int, the string without spaces, tabs and newlines around it; the indexes are checked at run time |
| `(find s part)` | where `part` first starts in `s`, as an `(option int)` |
| `(split s sep)`, `(join v sep)` | the pieces between the separators as a `(vec string)`, and the pieces put back together |
| `(char n)` | the one-byte string of a byte, 0 to 255 |
| `(read-file path)`, `(write-file path text)` | the whole file as an `(option string)`; write a string to a file, `true` when it worked |
| `(read-line)` | the next line of standard input without its newline, as an `(option string)`; `none` at the end |
| `(args)`, `(getenv name)` | the arguments after the file name as a `(vec string)`; an environment variable as an `(option string)` |
| `(exit status)` | end the program with this exit status |

All the files of a program share one set of names. They are compiled in the order their `use`s need, and nothing
runs until every one of them compiled without an error; then their top levels run in the same order (in the REPL a
used file runs when it is loaded). A struct is a value and is copied; a vec or a map is a handle, so copying one (also inside a struct) shares its
contents. A
struct can hold a vec of itself, which makes a tree. A function's last form is its value. An integer literal must fit in 64 bits, and a float needs digits on both sides
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
  takes as many cells as its type says: one for a number, a bool, a string, an enum value or a vec, the sum of its
  fields for a struct, and one more than the value inside for an option. A string's cell holds its offset and length
  in the text arena, so `substr`, `split` and `trim` return pieces of the same text without copying it.
- The compiler counts how deep each function's operand stack can get, and the VM checks that the whole frame fits
  before it enters it, so no instruction can write past the stack.

## Status

Version 0.3, tested on Linux x86-64 with gcc 13.3 and clang 18.1. `./test` passes 63 checks per build (16 examples,
41 compile and runtime error cases, one REPL session, five README examples) with gcc, clang and gcc with both sanitizers. GitHub Actions
runs shellcheck on the scripts and `./test` on every push (`.github/workflows/test.yml`, ubuntu-24.04).

Not in 0.3: gems
(packages with versions, see the spec), calling C, and the editor. The fixed limits are 8 parameters per function, 4096 functions, 4096 globals, 4096
types and 2^18 instructions; per unit (a file, or one REPL form) 2^18 syntax nodes and an 8 MiB arena for the source
and its names; at run time a stack of 65536 cells and at most 4096 nested calls. Every string, the program's and the
ones `format` and `sh` make, lives in one 32 MiB arena, and every vec and map on a heap of 4M cells (32 MiB); neither is
freed yet, and filling one is a runtime error.

## License

MIT, see `LICENSE`.
