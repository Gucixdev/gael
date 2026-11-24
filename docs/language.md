## The language in 0.3

A tour; `docs/REFERENCE.md` has the exact entry for each name (`gael -n` lists them all). gael is written mostly by
models, so `docs/MODELS.md` (`gael doc` prints it) is all of it in under 3000 tokens, small enough for the context of
a model on a home machine: the mistakes to avoid, every form with an example, the gems and the commands.

Types: `int` (64-bit, arithmetic wraps around), `float` (64-bit), `bool`, `string`, `unit` (no value), structs, enums,
`(option type)`, `(vec type)`, `(map key value)`, `(pool type)`, `(handle type)`, `(fn (types) type)`,
`(coroutine type)`, and numbers with a size: `i8`, `i16`, `i32`, `i64` (the same as `int`), `u8`, `u16`, `u32`, `u64`,
`f32`, `f64` (the same as `float`). Arithmetic stays in its size and wraps around; a number literal takes the size expected where it
stands (a declared type, a parameter, a field, the other operand) and must fit in it, so `(def b : u8 300)` is a compile
error. Nothing converts on its own: `(+ 1 2.0)` is a compile error, and `(float x)`, `(int x)`, `(u8 x)`, `(f32 x)` and
the like convert on purpose: between ints the value wraps around, a float to an int that cannot hold it is a runtime
error. A comment starts with `;`.

| Form | Meaning |
|---|---|
| `(use "player.gael")` | compile that file of the program first, its path relative to this one; a file used by several others is compiled once, and a file that ends up using itself is an error. Code for other programs goes in a gem |
| `(def hp : int 100)`, `(def hp 100)` | a global; without `: type` the type is the value's |
| `(defn name ((x : int) (y : float)) : int body...)` | a function, `()` when it takes nothing; without `: type` it returns unit. A function can be called above its definition |
| `inc`, `(fn ((x : int)) : int (* x x))`, `(f 3)` | one namespace, as in Scheme: a function's name is a value of type `(fn (int) int)`, `fn` makes one without a name, and a variable, a field or an expression of that type is called like a function; a local of the same name hides a function |
| `(let ((x 1) (y : float 2.0)) body...)` | locals for the body |
| `(set x 5)`, `(set b.pos.x 3.0)` | assign to a variable or a field of one; the value must have its type |
| `(if c then else)`, `(when c body...)`, `(while c body...)`, `(do body...)` | a condition must be a bool; `if` without `else` is unit |
| `(return x)`, `(return)` | leave the function now, with `x` (none when it returns unit): `(when (< hp 0) (return false))` |
| `(assert c)`, `(assert c "why")` | a runtime error when `c` is false: `assertion failed: (> hp 0)`, or the message |
| `(and a b...)`, `(or a b...)`, `(not a)`, `(nand a b...)`, `(nor a b...)` | on bools; all but `not` stop at the first answer |
| `(xor a b...)`, `(xnor a b...)` | true when an odd number of the bools are true, and the opposite; every one is evaluated |
| `(+ a b...)`, `-`, `*`, `/` | numbers of one type; `(- x)` negates |
| `(% a b)` | the remainder of two ints |
| `(min a b...)`, `(max a b...)`, `(abs x)`, `(clamp x lo hi)` | on anything `<` orders, each value worked out once; the names stay free for a variable or a field |
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
| `(coroutine (f x...))` | run `f` as a coroutine, at once until it first pauses; a `(coroutine type)` of what `f` returns |
| `(poll co)`, `(done? co)` | never waits: `(some result)` once the coroutine has ended, `none` before; whether it has ended |
| `(sleep 0.5)`, `(yield)` | a coroutine pauses for seconds of coroutine time, or until the next step; the main program's `sleep` sleeps in real time, running the coroutines meanwhile |
| `(step-coroutines dt)`, `(run-coroutines)` | from the main program: coroutine time goes `dt` seconds on and each coroutine that can goes on until it pauses again (the number of coroutines left); run them until none is left, as a script's or a server's loop |
| `(coroutine-time)` | the coroutine time in seconds |
| `(enable name...)`, `(disable name...)` | turn gems on, loading each one after what it requires, or off (below) |
| `(some 5)`, `none` | the two kinds of `(option int)` value; `none` takes its type from where it goes: a declared binding, a parameter, a field, a return type, or the else branch of an `if` whose then branch has one |
| `(match x (pattern body...)...)` | patterns are `_`, int and string literals, `true` and `false`, enum values (`north` or `Dir.north`), `(some name)` and `none`; every value must be covered, and an arm that can never run is an error |
| `(print "fmt" args...)`, `(println "fmt" args...)` | `%d` int, `%x` int in hex, `%f` float, `%s` string, `%b` bool, `%v` any value, `%%`; the codes are checked against the arguments when compiling. `eprint` and `eprintln` write to stderr |
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

`(a.dot b)` is `(Vec2.dot a b)`, and `((get bodies 0).dot b)` the same on an expression's value; a value goes into a
call as a copy, so a method gives a new value instead of changing its own (`(set p (p.hurt 10))`). `init` makes `(Player "Ann")` call `Player.init`, and a field can have a default
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
`examples/xmacros.gael`. A macro that refuses a use says why with `(compile-error "...")`, at the place of the use.

`gael -m FILE` prints the code the compiler checks, every macro expanded - also those inside functions - and laid
out to read:

```
$ ./gael -m tests/expand/swap.gael
(let ((x 1) (y 2))
  (let ((tmp x)) (set x y) (set y tmp))
  (when (not (< x y)) (println "x %d is not below y %d now" x y)))
```

A `fn` can use the locals of the code around it, a closure: it keeps a copy of each one it uses, made when the `fn`
is made (as a GDScript lambda does). Setting one inside changes the fn's own copy, which stays from call to call, so
`(let ((n 0)) (fn () : int (set n (+ n 1)) n))` counts; a vec, a map or a pool is a reference, so what is in it is
shared. A `fn` made in a loop keeps that turn's values. `examples/closures.gael` has more.

A coroutine is a function running on a stack of its own (it is stackful): it can pause anywhere, also deep in the
functions it calls, and they need no mark for it - no async colouring as in Rust or C#. Nothing ever waits for one:
`(poll co)` gives `(some result)` once it has ended and `none` before, its type checked when compiling. A game's loop
gives the coroutines a step each frame:

```
(defn patrol ((i : int)) : unit
  (while true
    (walk-to i 5.0)          ; walk-to sleeps, so patrol pauses there
    (walk-to i 0.0)))
(coroutine (patrol 0))
(while running
  (step-coroutines dt)
  (draw))
```

A coroutine cannot pause inside an `(arena ...)` (the arena would end while other code runs), and what a coroutine makes
is its own, not part of the arena of the code that resumed it. A coroutine that never ends holds what it holds: one
stopped from outside (cancel) is not there yet. `examples/coroutines.gael` has more.

GPU code is gael too, as in Mojo: `gpu-each` runs its body once per element of a vec, all at once, on the GPU when
there is one, and as a loop on the VM when not, with the same result. The body works on `f32`, `i32` and `u32` fields
and values, and the compiler checks that each run writes only its own element:

```
(struct Ball (x : f32) (y : f32) (vy : f32))
(def dt : f32 0.016)
(gpu-each (b balls)
  (set b.vy (- b.vy (* 9.8 dt)))
  (set b.y (max 0.0 (+ b.y (* b.vy dt)))))
```

gael writes the body as WGSL (`gael -g FILE` shows it) and runs it through wgpu-native, loaded when the first
`gpu-each` runs (`GAEL_WGPU=FILE`, or `lib/libwgpu_native.so` next to gael), so the same program uses Vulkan on Linux
with Wayland and on Android, Metal on Apple and Direct3D 12 on Windows; gael itself needs nothing of it to build.
`GAEL_GPU=0` keeps everything on the VM. `examples/gpu.gael` bounces balls.

The same GPU draws: `(new-canvas 640 480)` is an image on it, `draw-rect`, `draw-circle` and `draw-line` batch shapes
in 0xRRGGBBAA colors, `show-frame` draws them, and `save-png` and `canvas-pixel` read the result back.
`(open-window "game" 640 480)` is a canvas on the screen, on Wayland for now: `show-frame` presents it at the screen's
rate, and `key-down?`, `mouse-x`, `mouse-y` and `mouse-down?` read its keys and mouse.

A gem is a package by name: `gems/NAME.gael`, or a folder `gems/NAME/` whose `main.gael` uses the rest, next to the
program, in gael's own folder, or in the `gems/` that comes with gael, next to its binary. Its main file starts with a
manifest:

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
until it is on again (code compiled before goes on working). `examples/gems.gael` turns on two of the gems that come
with gael, and `wfc` brings `rng` with it.

The gems that come with gael are in `gems/`. `ecs` is an entity component system the way flecs and the C ones go:
64-bit entity ids with a generation, a column per component, components and tags as bits of one mask per entity, and
queries that name what they need; `pqueue` (a priority queue) and `events` (a log and a schedule of events by tick)
are ported from holyc, used in `examples/queues.gael`; `hfsm` is a hierarchical state machine whose state is an enum
value, so `examples/guards.gael` keeps one in an ECS column; `rng` (PCG32), `noise`, `easing` and `curve` make the
map of `examples/procgen.gael`; `wfc` is wave function collapse, which grows the island of `examples/wfc.gael`;
`geometry` has `Vec2`, `Vec3` and `Rect` with `+ - * =` on them; `json` reads and writes JSON and answers jq-style filters
(`examples/json.gael`), `html` writes HTML with htmx's `hx-*` attributes as one call and `markdown` turns Markdown into it,
escaping raw HTML (`examples/page.gael`).
`examples/ecs.gael` runs a few ticks of an ECS:

```lisp
(enable ecs)
(ecs-world Game
  (components (pos : Vec2 (Vec2)) (vel : Vec2 (Vec2)) (hp : int 100) (name : string ""))
  (tags player enemy burning))

(defn burn () : unit                            ; burning enemies lose 10 hp a tick
  (ecs-each Game (e hp name) :with (burning enemy)
    (set hp (- hp 10))
    (when (<= hp 0)
      (println "  %s burns up" name)
      (e.despawn))))
```

`@export` before a `def` marks a global the way GDScript's `@export` marks a variable, for an editor's inspector to
show and change; an annotation gael does not know is a compile error. `gael -x` lists them, with the value when it is
written as a literal:

```
$ ./gael -x examples/export.gael
speed : float = 10.0	examples/export.gael:3
player-name : string = "Ada"	examples/export.gael:4
lives : u8 = 3	examples/export.gael:5
spawn-point : NodePath = $Level/Start	examples/export.gael:7
```

A runtime error stops the program and names the line and column it happened on, then what to do about it:

```
$ ./gael tests/errors/div-zero.gael
before
tests/errors/div-zero.gael:1:39: runtime error: division by zero; test the divisor first: (if (= b 0) 0 (/ a b))
```
