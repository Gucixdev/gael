# gael

gael is a small statically typed language written as s-expressions. The compiler checks every type before anything
runs and reports all the errors of a file in one go, each with its line, column and a caret under the mistake. A
program without errors is compiled to bytecode and run on a stack VM. gael is meant to become the scripting language
of a game engine and of an Emacs-like editor written in it; this repository is the
language, version 0.3. `docs/REFERENCE.md` is the dictionary: every form, operator, function and type, with what it
takes, gives and reports.

## Build and run

You need bash and a C compiler that accepts C89 (gcc or clang).

```sh
./build                     # writes ./gael (GAEL_STATIC=1: linked statically, every gpu-each on the VM)
./gael examples/fact.gael   # run a file
./gael                      # the REPL
./run examples/fib.gael     # rebuild when a source changed, then run with these arguments
./test                      # the examples, the error cases and a REPL session, on every build
./bench                     # time gael against other languages on the same programs
./install [-rebuild]        # build and install into ~/.local (lib/gael with its gems, bin/gael); PREFIX= changes it
```

`./test` builds with gcc and with clang (`-std=c89 -pedantic -Wall -Wextra -Werror`), and once more with
AddressSanitizer and UndefinedBehaviorSanitizer when one of the compilers has their runtime installed. `./bench` runs
the programs in `benchmarks/` (function calls, a tight loop, a sieve, n-body, particles, a hash map, and an empty
program for the start) in gael and in each of C, Python, Lua, LuaJIT, Guile, Chez Scheme, Chicken, SBCL, Node and
GDScript that is installed, checks that they print the same, and appends the medians to `benchmarks/RESULTS.md`.

Command line:

```
gael                   the REPL
gael FILE ARGS...      compile FILE; when there are no errors, run it; (args) gives it ARGS
gael run FILE ARGS...  the same
gael check FILE        only compile: report every error and warning, run nothing (also gael -c)
gael test FILE...      compile the files, run their defs (not the rest of their top level), then each
                       (@test "name" ...) in them: ok or FAIL for each; exit 1 when one failed. A
                       gem's main file (gems/NAME.gael) runs that gem's own tests
gael doc               the cheat sheet for writing gael (docs/MODELS.md next to the binary)
gael -d FILE           compile and print the bytecode
gael -m FILE           print FILE with its macros expanded: the code the compiler checks
gael -g FILE           print the WGSL that each gpu-each in FILE runs on the GPU
gael -e CODE ARGS...   compile and run CODE
gael -x FILE           compile FILE and list its @export globals (name : type = value) and its
                       (@warn ...) and (@err ...) marks, the way an editor shows them
gael -h                this help
GAEL_HEAP=SIZE  the heap for strings, vecs, maps and pools, and as much again for arenas and for the program's
                own text (default 1G; K, M, G; the system gives pages only where they are written)
GAEL_CHECK=1    freed memory is overwritten, and a program that ran to its end reports what it never freed
GAEL_HOME=DIR   gael's own folder (default ~/.gael): the REPL runs main.gael from there first and appends
                what ran to repldump.gael; set to nothing, it does neither
GAEL_GPU=0|1    0: every gpu-each runs on the VM; 1: a missing GPU is an error (default: the GPU when there is one)
GAEL_WGPU=FILE  the wgpu-native library for gpu-each (default: lib/libwgpu_native.so next to gael, then the system's)
```

gael's own folder is like `~/.emacs.d`: the REPL starts by running `main.gael` from it, and every form that ran
without an error is appended to `repldump.gael` there. A session starts with a line that says when and in which
directory; each form comes after a line with its date and time and, when it gave a function of a file a new body,
which one (`;; 2026-10-25 16:28:57, replaces hurt (game.gael:12)`). The files themselves are never
written: a change made while the program runs lives in the running program and in the dump, as in Smalltalk's changes
file. `gael < ~/.gael/repldump.gael` plays a session again.

The exit status is 0 when everything went fine, 1 after a compile or runtime error (or what `GAEL_CHECK=1` found),
and 2 for bad usage.

## More

[What it looks like](docs/tour.md), [the language in 0.3](docs/language.md), [how it is built](docs/internals.md), [reference](docs/REFERENCE.md).

## Status

Version 0.3, tested on Linux x86-64 with gcc 13.3 and clang 18.1. `./test` passes 158 checks per build (44 examples, 84
compile and runtime error cases, three warning cases, four gael test cases, the tests of each of the ten gems that come
with gael, one WGSL case, three REPL sessions, nine README examples), the C89 mirror of WebGPU's structs against
`webgpu.h`, and with `GAEL_WGPU` set the GPU runs of `gpu-each`, drawing, and windows on a headless weston of the
test's own (tested on Mesa's lavapipe, Vulkan on the CPU) with gcc, clang, gcc with the plain switch dispatch and gcc with both sanitizers, every run with `GAEL_CHECK=1`.

Not in 0.3: versions of gems, calling C, and the editor. The fixed limits are 8 parameters per function, 65536
functions, 65536 globals, 65536 types and 2^24 instructions; per unit (a file, or one REPL form) 2^21 syntax nodes and a
64 MiB arena for the source and its names; at run time a stack of 65536 cells and at most 4096 nested calls (a tail call
does not nest), the same for each coroutine. Strings made at run time, vecs, maps and pools live on the heap; the text
arena, as big, holds the program's own strings and a string while it is being built (a format, a command's output, a
file). Both are 1 GiB of address space unless `GAEL_HEAP` says otherwise, and filling either is a runtime error.

## License

MIT, see `LICENSE`.
