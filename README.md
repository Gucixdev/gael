# gael

gael is a small statically typed language written as s-expressions. The compiler checks every type before anything
runs and reports all the errors of a file in one go, each with its line, column and a caret under the mistake. A
program without errors is compiled to bytecode and run on a stack VM. gael is meant to become the scripting language
of a game engine and of an Emacs-like editor written in it (`docs/SPEC.md`, in Polish); this repository is the
language, version 0.3. `docs/REFERENCE.md` is the dictionary: every form, operator, function and type, with what it
takes, gives and reports.

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
gael -x FILE           compile FILE and list its @export globals: name : type = value
gael -h                this help
GAEL_HEAP=SIZE  the heap for strings, vecs, maps and pools, and as much again for arenas and for the program's
                own text (default 1G; K, M, G; the system gives pages only where they are written)
GAEL_CHECK=1    freed memory is overwritten, and a program that ran to its end reports what it never freed
GAEL_HOME=DIR   gael's own folder (default ~/.gael): the REPL runs main.gael from there first and appends
                what ran to repldump.gael; set to nothing, it does neither
```

gael's own folder is like `~/.emacs.d`: the REPL starts by running `main.gael` from it, and every form that ran
without an error is appended to `repldump.gael` there, under a line with the date and the directory of the session.
`gael < ~/.gael/repldump.gael` plays a session again.

The exit status is 0 when everything went fine, 1 after a compile or runtime error (or what `GAEL_CHECK=1` found),
and 2 for bad usage.

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
tests/errors/three-errors.gael:2:8: error: + works on numbers, not bool
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
<repl>:1:7: error: +: int and float do not mix; convert one, like (int x) or (float x)
  (+ hp 0.5)
        ^
gael> :q
```

`examples/` has one file per feature with its output next to it (`NAME.out`). `gael -d examples/fact.gael` prints
the bytecode, one instruction per line with its operands, the constant it uses
and the source line it came from.

## The language in 0.3

A tour; `docs/REFERENCE.md` has the exact entry for each name (`gael -n` lists them all).

Types: `int` (64-bit, arithmetic wraps around), `float` (64-bit), `bool`, `string`, `unit` (no value), structs,
enums, `(option type)`, `(vec type)`, `(map key value)`, `(pool type)`, `(handle type)`, `(fn (types) type)`, `(task type)`, and numbers with a size:
`i8`, `i16`, `i32`, `i64` (the same as `int`), `u8`, `u16`, `u32`, `u64`, `f32`, `f64` (the same as `float`). Arithmetic
stays in its size and wraps around; a number literal takes the size expected where it stands (a declared type, a
parameter, a field, the other operand) and must fit in it, so `(def b : u8 300)` is a compile error. Nothing converts
on its own: `(+ 1 2.0)` is a compile error, and `(float x)`, `(int x)`, `(u8 x)`, `(f32 x)` and the like convert on
purpose: between ints the value wraps around, a float to an int that cannot hold it is a runtime error. A comment
starts with `;`.

| Form | Meaning |
|---|---|
| `(use "lib/geometry.gael")` | compile that file first, its path relative to this one; a file used by several others is compiled once, and a file that ends up using itself is an error |
| `(def hp : int 100)`, `(def hp 100)` | a global; without `: type` the type is the value's |
| `(defn name ((x : int) (y : float)) : int body...)` | a function, `()` when it takes nothing; without `: type` it returns unit. A function can be called above its definition |
| `inc`, `(fn ((x : int)) : int (* x x))`, `(f 3)` | one namespace, as in Scheme: a function's name is a value of type `(fn (int) int)`, `fn` makes one without a name, and a variable, a field or an expression of that type is called like a function; a local of the same name hides a function |
| `(let ((x 1) (y : float 2.0)) body...)` | locals for the body |
| `(set x 5)`, `(set b.pos.x 3.0)` | assign to a variable or a field of one; the value must have its type |
| `(if c then else)`, `(when c body...)`, `(while c body...)`, `(do body...)` | a condition must be a bool; `if` without `else` is unit |
| `(return x)`, `(return)` | leave the function now, with `x` (none when it returns unit): `(when (< hp 0) (return false))` |
| `(and a b...)`, `(or a b...)`, `(not a)`, `(nand a b...)`, `(nor a b...)` | on bools; all but `not` stop at the first answer |
| `(xor a b...)`, `(xnor a b...)` | true when an odd number of the bools are true, and the opposite; every one is evaluated |
| `(+ a b...)`, `-`, `*`, `/` | numbers of one type; `(- x)` negates |
| `(% a b)` | the remainder of two ints |
| `(sqrt x)`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `(atan2 y x)`, `exp`, `ln`, `(pow x y)`, `floor`, `ceil` | the C library's functions of floats |
| `<`, `<=`, `>`, `>=`, `=`, `!=` | compare two numbers of one type; `=` and `!=` also take bools, strings, enum values, handles and node paths |
| `(bit-and a b...)`, `bit-or`, `bit-xor`, `(bit-not a)`, `(shl a n)`, `(shr a n)` | the bits of ints of one type; a shift count runs from 0 to the type's bits - 1 (anything else is a runtime error), and `shr` keeps the sign of a signed type. Ints can also be written `0xff` and `0b1010` |
| `$Player`, `$Enemies/Goblin` | a node path, as in GDScript: a value of type `NodePath`; what it points at comes with the scene tree |
| `(struct Vec2 (x : float) (y : float))` | a struct; a field can have any type declared above it, other structs and options included; `(hp : int 100)` has a default |
| `(Vec2 1.0 2.0)`, `(Vec2 :y 2.0 :x 1.0)` | a struct value, with the fields in order or named; a field left out takes its default (only the last ones, in order) |
| `(defn Vec2.add ((a : Vec2) (b : Vec2)) : Vec2 ...)`, `(a.dot b)` | a function of a struct or an enum, and a method call: `(Vec2.dot a b)`. Hooks: `add`, `sub`, `mul`, `div`, `rem`, `negate` for the operators, `equal` and `compare` for comparing, `text` for printing, `len`, `get`, `put`, `for`, and `init` for `(Vec2 ...)` (below) |
| `v.x`, `b.pos.x`, `(get bodies 0).pos.x` | a field of a variable, or of any expression's value: `(expr).name` right after the parenthesis reads as `(. expr name)` |
| `(enum Dir north east south west)`, `Dir.north` | an enum and one of its values |
| `(vec 1 2 3)`, `(vec)` | a vec; an empty one takes its element type from where it goes, like `none` |
| `(push v x)`, `(get v i)`, `(put v i x)`, `(pop v)`, `(len v)` | add at the end, read, replace, take the last one off (an option), count; `get` and `put` check the index at run time |
| `(map ("a" 1) ("b" 2))`, `(map)` | a map; the key is an int, a bool, a string, an enum value or a handle; the entries stay in the order they went in |
| `(insert m k v)`, `(lookup m k)`, `(has m k)`, `(remove m k)` | add or replace, find (an option), check, take out (an option of the old value) |
| `(keys m)`, `(values m)`, `(len m)` | the keys and the values as vecs, in order; the number of entries |
| `(pool 1000)` | a pool of 1000 slots that never grows; its element type comes from where it goes, like `(def ps : (pool Particle) (pool 1000))` |
| `(spawn p x)`, `(get p h)`, `(put p h x)`, `(len p)` | put `x` into a free slot and get its `(handle type)`; read and replace an element; count them. `get` and `put` with a handle whose element was removed are runtime errors, and so is a spawn into a full pool |
| `(lookup p h)`, `(has p h)`, `(remove p h)` | the element as an option (`none` once it was removed), whether it is still there, take it out (an option) so its slot is free again |
| `(for (x v) body...)`, `(for (k m) body...)`, `(for (h p) body...)`, `(for (i 0 n) body...)` | go over a vec (`x` is a copy of each element), over a map's keys, over the handles of a pool's elements, or over 0, 1, ..., n - 1 |
| `(arena body...)` | the strings, vecs, maps and pools the body makes (also in the functions it calls) come from one block that is handed back at once when it ends; its value is the body's last one |
| `(keep x)` | `x` with what it holds from the innermost arena copied out to the heap, so it outlives the arena |
| `(defmacro name (pattern template)...)` | a macro: the first rule whose pattern matches a use rewrites it, before the types are checked (below) |
| `(string-of x)`, `(name-of "print-" x)` | in a template: `x` written as a string, and a name joined from parts, as C's `#` and `##` |
| `(async (f x...))` | run `f` as a task, at once until it first waits; a `(task type)` of what `f` returns |
| `(await t)`, `(sleep 0.5)`, `(yield)` | wait for a task's result, for seconds of task time, or for the next step. In a task they let the others run; in the main program `await` and `sleep` run the tasks meanwhile |
| `(tasks-step dt)`, `(tasks-run)` | from the main program: task time goes `dt` seconds on and each task that can goes on until it waits again (the number of tasks left); run them until none is left, waiting in real time |
| `(done? t)`, `(task-time)` | whether a task has ended; the task time in seconds |
| `(enable name...)`, `(disable name...)` | turn gems on, loading each one after what it requires, or off (below) |
| `(some 5)`, `none` | the two kinds of `(option int)` value; `none` takes its type from where it goes: a declared binding, a parameter, a field, a return type, or the else branch of an `if` whose then branch has one |
| `(match x (pattern body...)...)` | patterns are `_`, int and string literals, `true` and `false`, enum values (`north` or `Dir.north`), `(some name)` and `none`; every value must be covered, and an arm that can never run is an error |
| `(print "fmt" args...)`, `(println "fmt" args...)` | `%d` int, `%f` float, `%s` string, `%b` bool, `%v` any value, `%%`; the codes are checked against the arguments when compiling. `eprint` and `eprintln` write to stderr |
| `(format "fmt" args...)` | the same codes as `println`, checked the same way, into a new string |
| `(len s)` | the length of a string in bytes |
| `(sh "command")` | run a command with `/bin/sh`, get what it printed, without the newlines at the end |
| `(system "command")` | run a command, its output goes where gael's goes; get its exit status |
| `(run "command")` | run a command; `true` when it exits with 0, so `(and (run "make") (run "make install"))` stops at the first failure |
| `(chain x (f a) g)` | hand a value on: each step gets the value so far as its first argument, so this is `(g (f x a))` |
| `(pipe "cmd" "cmd"...)`, `(pipe-in text "cmd"...)` | commands joined like bash's `\|`, and their output; `pipe-in` gives the first command `text` on stdin |
| `(env "HOME")`, `(pid)` | an environment variable (`""` when it is not set), the process id |
| `(parse-int s)`, `(parse-float s)` | `(some n)` when the whole string, spaces around it aside, is a number; otherwise `none` |
| `(substr s from to)`, `(byte s i)`, `(trim s)` | the bytes from `from` up to `to`, one byte as an int, the string without spaces, tabs and newlines around it; the indexes are checked at run time |
| `(find s part)` | where `part` first starts in `s`, as an `(option int)` |
| `(split s sep)`, `(join v sep)` | the pieces between the separators as a `(vec string)`, and the pieces put back together |
| `(char n)` | the one-byte string of a byte, 0 to 255 |
| `(read-file path)`, `(write-file path text)`, `(append-file path text)` | the whole file as an `(option string)`; write or append a string, `true` when it worked |
| `(read-line)` | the next line of standard input without its newline, as an `(option string)`; `none` at the end |
| `(args)`, `(getenv name)` | the arguments after the file name as a `(vec string)`; an environment variable as an `(option string)` |
| `(exit status)` | end the program with this exit status |

All the files of a program share one set of names. They are compiled in the order their `use`s need, and nothing
runs until every one of them compiled without an error; then their top levels run in the same order (in the REPL a
used file runs when it is loaded). A struct is a value and is copied; a vec, a map or a pool is a reference, so
copying one (also inside a struct) shares its contents. A struct can hold a vec, a pool or a handle of itself, which
makes a tree, and options of itself in that vec. A pool is for things that come and go, like entities and particles:
a handle names a slot and the generation the slot had when the element went in, so a handle kept after its element
was removed never reads the element that took the slot later. A function's last form is its value, and a call there
is a tail call, as in Scheme: the last form of the body, and from there both branches of an `if` and the end of a
`do`, `let`, `when` or match arm. It takes over the caller's frame, so recursion in that place is a loop and runs any
number of times; other calls nest, and a few thousand deep the program stops with a stack overflow. An integer literal
must fit in 64 bits, and a float needs digits on both sides of its point (`4.2`, `4.2e3`; `1e3` and `.5` are errors).

Memory needs no care in the program: a string made at run time, a vec, a map and a pool count their owners, and when
the last one lets go it is freed on the spot, together with what it held. The compiler knows every type, so it puts
the counting in; a read that only looks at a variable (`len`, `get`, `=`, `lookup`) borrows it and counts nothing. A
cycle, like a node kept in its own list of children, is never freed. With `GAEL_CHECK=1` a program that runs to its
end reports what it never freed, and freed memory is overwritten, so a read of it shows.

An arena is for memory with a known end, like one frame of a game or one request to a server: what its body makes is
handed back in one go when it ends. Its objects are counted too, so when an arena ends while something made in it is
still held (put in a global, say, or in a cycle), that is a runtime error at the arena instead of a dangling
reference. Where the compiler can see a value made in an arena go somewhere that outlives it (`set` or `push` to a
variable from outside, or the arena's own value), that is a compile error. `keep` is the way out: it copies the value.

A struct or an enum can have functions of its own, and some names are hooks the language calls, chosen when compiling,
the way Python calls `__add__` but with words instead of underscores:

```
(struct Vec2 (x : float 0.0) (y : float 0.0))
(defn Vec2.add ((a : Vec2) (b : Vec2)) : Vec2 (Vec2 (+ a.x b.x) (+ a.y b.y)))
(defn Vec2.text ((a : Vec2)) : string (format "<%f %f>" a.x a.y))
(defn Vec2.dot ((a : Vec2) (b : Vec2)) : float (+ (* a.x b.x) (* a.y b.y)))
(let ((a (Vec2 1.0 2.0)) (b (Vec2 3.0)))   ; b.y is 0.0
  (println "%v %f" (+ a b) (a.dot b)))      ; <4.0 2.0> 3.0
```

`(a.dot b)` is `(Vec2.dot a b)`; a value goes into a call as a copy, so a method gives a new value instead of changing
its own (`(set p (p.hurt 10))`). `init` makes `(Player "Ann")` call `Player.init`, and a field can have a default
value. `examples/methods.gael` has every hook.

A macro rewrites code before its types are checked, as Scheme's `syntax-rules` does:

```
(defmacro swap
  ((_ a b) (let ((tmp a)) (set a b) (set b tmp))))
(defmacro cond
  ((_ (:else body ...)) (do body ...))
  ((_ (c body ...)) (when c body ...))
  ((_ (c body ...) clause ...) (if c (do body ...) (cond clause ...))))
```

Each rule is a pattern and a template. In the pattern, after the `_` that stands for the macro's name, a symbol
matches any one form and names it, `_` matches anything, a keyword like `:else`, a number or a string matches only
itself, and `...` after an element matches it any number of times (one `...` per list). In the template each name
becomes what it matched and `x ...` repeats `x` for each match. Macros are hygienic: the `tmp` of `swap` is never
the `tmp` of the code that uses it, and a name the template uses without binding it means what it means at the top
level, even where the use has a local of that name. A macro can be used anywhere in its file and in the files that
use that file; at the top level it can make globals, functions and types, several at once with `(do ...)`.
`examples/macros.gael` has more. In a template, `(string-of x)` and `(name-of "print-" x)` do what `#` and `##` do in
C, so one list written once can make an enum, a table of its names and a function per entry: the x-macros of
`examples/xmacros.gael`.

A `fn` can use the locals of the code around it, a closure: it keeps a copy of each one it uses, made when the `fn`
is made (as a GDScript lambda does). Setting one inside changes the fn's own copy, which stays from call to call, so
`(let ((n 0)) (fn () : int (set n (+ n 1)) n))` counts; a vec, a map or a pool is a reference, so what is in it is
shared. A `fn` made in a loop keeps that turn's values. `examples/closures.gael` has more.

A task is a function running on a stack of its own (a stackful coroutine): it can wait anywhere, also deep in the
functions it calls, and they need no mark for it - no async colouring as in Rust or C#. `(await t)` has the type of
what the task's function returns, checked when compiling. A game's loop gives the tasks a step each frame:

```
(defn patrol ((i : int)) : unit
  (while true
    (walk-to i 5.0)          ; walk-to sleeps, so patrol waits there
    (walk-to i 0.0)))
(async (patrol 0))
(while running
  (tasks-step dt)
  (draw))
```

A task cannot wait inside an `(arena ...)` (the arena would end while other code runs), and what a task makes is its
own, not part of the arena of the code that resumed it. A task that never ends holds what it holds: one stopped from
outside (cancel) is not there yet. `examples/tasks.gael` has more.

A gem is a package by name: `gems/NAME.gael`, or a folder `gems/NAME/` whose `main.gael` uses the rest, next to the
program or in gael's own folder. Its main file starts with a manifest:

```
(gem inventory :version "0.2.0" :requires (ecs) :provides (bags) :excludes (inventory-legacy))
```

`(enable inventory ecs-flat)` loads the gems named, each after what it requires; no other file under `gems/` is read,
so many installed gems cost nothing at start. A requirement is met by the gem of that name or by one that provides
it (here `ecs-flat` provides `ecs`), and two gems that are on may not provide the same thing or exclude each other:
`gems ecs-archetype and ecs-sparse both provide ecs; turn on one of them`. A gem's code uses only its own names and
those of the gems it requires, checked at each use when compiling: `gem sneaky uses ecs-spawn of gem ecs-flat, which
it does not require; add ecs-flat to its :requires`. The program uses every gem that is on. In the REPL
`(enable name)` compiles a gem into the running program, and `(disable name)` turns one off: its names cannot be used
until it is on again (code compiled before goes on working). `examples/gems.gael` uses two gems.

`@export` before a `def` marks a global the way GDScript's `@export` marks a variable, for an editor's inspector to
show and change; an annotation gael does not know is a compile error. `gael -x` lists them, with the value when it is
written as a literal:

```
$ ./gael -x examples/export.gael
speed : float = 10.0	examples/export.gael:3
player-name : string = "Kacper"	examples/export.gael:4
lives : u8 = 3	examples/export.gael:5
spawn-point : NodePath = $Level/Start	examples/export.gael:7
```

A runtime error stops the program and names the line it happened on:

```
$ ./gael tests/errors/div-zero.gael
before
tests/errors/div-zero.gael:1: runtime error: division by zero
```

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
- A task has its own value stack and frames, blocks on the heap that double when a call needs more; switching to a
  task or back saves one set of registers (stack, frames, pc, sp, fp) and loads the other. A task's function returns
  to a `TASK_END` instruction that keeps the result in the task object. The main program's waits re-run their own
  instruction after each task they let run, so the scheduler needs no loop in C around the interpreter.
- Macros (`src/macro.c`) are checked when defined, kept in a table of their own (a unit's nodes are gone when the next
  one is read) and matched and copied with explicit stacks. Hygiene is a stamp: each symbol a template brings in
  carries the stamp of its expansion, and the compiler finds a local by its name and that stamp.
- The top level's names (functions, globals, types, macros) and the built-in ones are in one hash index
  (`src/names.c`), so compiling grows with the size of the program, not with its square.
- Every instruction is listed once in `src/ops.h` (an x-macro); the opcode enum, the disassembler's names and the
  operand counts come from that list.
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

## Status

Version 0.3, tested on Linux x86-64 with gcc 13.3 and clang 18.1. `./test` passes 120 checks per build (36 examples,
75 compile and runtime error cases, three REPL sessions, six README examples) with gcc, clang and gcc with both
sanitizers, every run with `GAEL_CHECK=1`. GitHub Actions runs shellcheck on the scripts and `./test` on every push
(`.github/workflows/test.yml`, ubuntu-24.04).

Not in 0.3: versions of gems, calling C, and the editor. The fixed limits are 8 parameters per function, 65536
functions, 65536 globals, 65536 types and 2^24 instructions; per unit (a file, or one REPL form) 2^21 syntax nodes
and a 64 MiB arena for the source and its names; at run time a stack of 65536 cells and at most 4096 nested calls (a
tail call does not nest), the same for each task. Strings made at run time, vecs, maps and pools live on the heap; the text arena, as big, holds
the program's own strings and a string while it is being built (a format, a command's output, a file). Both are 1 GiB
of address space unless `GAEL_HEAP` says otherwise, and filling either is a runtime error.

## License

MIT, see `LICENSE`.
