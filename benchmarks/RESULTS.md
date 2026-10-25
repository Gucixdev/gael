# Benchmark results

What `./bench` measured, newest at the end: the median wall time of each program in ms (the whole process, start
included) and the most memory one run held, in MB. The programs are in this folder, the same algorithm in every
language, written the way that language is usually written; each one's output is checked against `NAME.expected`
before it is timed. `start` is an empty program. `gdscript` is Godot's time minus Godot's own start (`start` in the
godot column), so it is the script alone. A cloud machine is shared and noisy: compare numbers from one run, not
across runs.

## 2026-10-25 4021b68

Intel(R) Xeon(R) Processor @ 2.80GHz, 2 cores, 8 GB; 10 runs after 1 warmup, median wall time.

- gael: gael 4021b68
- c: gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- python: Python 3.13.16
- lua: Lua 5.4.6
- luajit: LuaJIT 2.1.1703358377
- guile: guile (GNU Guile) 3.0.9
- chez: Chez Scheme 9.5.8
- chicken: Chicken 5.3.0
- sbcl: SBCL 2.2.9.debian
- node: Node v22.22.0
- godot: Godot 4.7.2.stable.official.ed1daf0bf

| ms | gael | c | python | lua | luajit | guile | chez | chicken | sbcl | node | godot | gdscript |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| start | 1.6 | 1.4 | 17.5 | 2.3 | 2.3 | 14.7 | 81.5 | 3.6 | 6.4 | 29.2 | 873.9 | 0.0 |
| fib | 526.4 | 29.0 | 908.6 | 395.3 | 59.9 | 188.7 | 130.1 | 498.0 | 112.6 | 97.5 | 4102.8 | 3228.9 |
| loop | 618.2 | 29.2 | 2971.2 | 302.7 | 95.2 | 411.0 | 408.1 | 1554.8 | 224.8 | 95.6 | 2334.7 | 1460.8 |
| map | 834.8 | 102.0 | 834.5 | 215.4 | 59.8 | 1144.8 | 400.4 | - | 317.1 | 408.5 | 1792.8 | 918.9 |
| nbody | 587.5 | 14.1 | 1148.7 | 493.5 | 73.5 | 2441.7 | 473.2 | 1056.1 | 560.5 | 81.3 | 3716.9 | 2843.0 |
| particles | 577.5 | 12.2 | 1718.1 | 435.8 | 47.4 | 1752.5 | 284.9 | 822.5 | 451.3 | 81.2 | 4648.5 | 3774.6 |
| sieve | 724.9 | 24.1 | 1053.6 | 557.7 | 175.6 | 355.0 | 277.6 | 580.1 | 151.6 | 204.8 | 2050.8 | 1176.9 |

| MB | gael | c | python | lua | luajit | guile | chez | chicken | sbcl | node | godot | gdscript |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| start | 2 | 1 | 8 | 2 | 2 | 9 | 48 | 6 | 19 | 39 | 208 | 208 |
| fib | 2 | 2 | 9 | 3 | 3 | 9 | 48 | 9 | 23 | 47 | 208 | 208 |
| loop | 2 | 2 | 9 | 3 | 2 | 9 | 48 | 6 | 25 | 49 | 209 | 209 |
| map | 67 | 36 | 111 | 28 | 17 | 63 | 95 | - | 112 | 105 | 211 | 211 |
| nbody | 2 | 2 | 9 | 3 | 3 | 10 | 48 | 18 | 82 | 49 | 209 | 209 |
| particles | 2 | 2 | 10 | 4 | 4 | 12 | 48 | 18 | 84 | 53 | 209 | 209 |
| sieve | 104 | 6 | 47 | 130 | 66 | 47 | 58 | 45 | 66 | 86 | 210 | 210 |

The bytecode VM against the script alone in Godot 4.7.2: gael is faster on all six, 6.1 times on fib, 2.4 on loop,
1.1 on map, 4.8 on nbody, 6.5 on particles, 1.6 on sieve; faster than Python on five and level on map. Lua 5.4's
interpreter (registers, not a stack) is 1.2 to 3.9 times faster than gael, LuaJIT and Node much more; C is 8 to 47
times faster (8 on the map, whose work is in C in both), the distance a VM keeps and gael → C is for. gael starts
almost as fast as C (1.6 ms against 1.4); Godot's engine takes 0.9 s. Memory: as small as C, but for the sieve (a bool takes a whole 8-byte cell in a vec, and the vec doubles: 104 MB
against C's 6) and the map (67 MB against 36).
