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
tests/errors/three-errors.gael:2:8: error: + works on numbers, not bool; give it numbers of one type
    (+ a true))
         ^
tests/errors/three-errors.gael:3:15: error: println: % code 1 wants string, this value is int
  (println "%s" 1)
                ^
tests/errors/three-errors.gael:4:6: error: set: unknown name nothing; set changes a variable that exists: declare it first with (let ((nothing value)) ...) or (def nothing value)
  (set nothing 3)
       ^
```

A warning is code that runs but is almost surely a mistake - a local nothing reads, a value worked out and thrown
away, code after a `return`. gael says so and runs the program anyway; a release build will stop at one. A name that
starts with `_` is on purpose:

```
$ ./gael tests/warnings/unreachable.gael
tests/warnings/unreachable.gael:5:5: warning: this never runs: the return on line 4 comes first; remove it, or move it above
      (println "never"))
      ^
tests/warnings/unreachable.gael:9:1: warning: this never runs: the exit on line 8 comes first; remove it, or move it above
  (println "after the exit")
  ^
0 8
```

Tests sit next to the code they test: `(@test "name" body...)` at the top level, with `assert` inside. `gael FILE`
runs the program and leaves the tests be; `gael test FILE` runs the defs, then each test in a run of its own, so a
failing one stops only itself. `@test` before a top-level form keeps it for the tests, like
`@test (defn sample-world () ...)`. A gem keeps its tests the same way: `gael test gems/NAME.gael` runs them, and a
program that turns the gem on neither runs nor compiles them. Everything that starts with `@` is for the tools and
for while you work on the program
- `(@warn "...")` and `(@err "...")` are marks an editor shows at their line, `(@debug ...)` is code for while
working - and a release build leaves it all out:

```
$ ./gael test examples/testing.gael
ok clamp keeps a value inside
ok clamp cuts both ends
ok words counts the words
3 passed, 0 failed
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
