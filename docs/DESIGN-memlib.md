# memlib: bit-flip-safe bits and AoS/SoA layout (design, before code)

Whiteboard order: scope, layout, proof (`lean4/`), numbers (`ampl/`), code (`gems/memlib/`), close.

## Purpose

Two gems that keep data safe and in the right shape, for gael programs that run for years or far from the ground
(a satellite, a rack with no ECC RAM, a long game server), and for files and the GPU, which want AoS:

- `memlib/bitspace`: a space of bits (a bool is one bit) that corrects any single flipped bit in memory and reports
  two, so bitrot and cosmic rays do not turn into wrong answers.
- `memlib/layout`: AoS to SoA and back, over any flat stream, with a proved round trip.

Run by him, by agents writing gael, and by gems (`artlib/image` writes AoS with it).

## What gael already has

A `(vec bool)` and the bool fields of structs in vecs and pools are one bit each, 64 to a cell, already SoA columns
(docs/internals.md). A local `bool` is one VM cell. None of it can notice a flipped bit: that is the gap `bitspace` fills.

## Features

| # | Feature | Inputs / outputs / errors / bound | Verdict |
|---|---|---|---|
| 1 | `(Bitspace.new n)` | n bits, all false; n up to 2^32 (the bound is the heap) | need |
| 2 | `(b.get i)` `(b.set i v)` | `get`: `(option bool)`, none outside 0..n-1 or in a damaged cell; `set`: true when done. Both check the cell and heal it on the way | need |
| 3 | `(b.scrub)` | walks every cell; returns `(Scrub fixed lost)`: single flips healed, cells with 2 flips found | need |
| 4 | `(b.count)` | number of true bits, from healed cells | need |
| 5 | lost cell policy | a cell with 2 flipped bits is not guessed: `get` gives none and `set` false (a wrong bool is worse than none); `scrub` counts them | need |
| 6 | code = SECDED (72,64): 64 data bits + 8 check bits per cell | corrects 1, detects 2, 12.5 % overhead (AMPL checks this choice) | need |
| 7 | check bits in their own column (`(vec u8)`), data in `(vec u64)` | SoA again: scrub reads the two columns in order | need |
| 8 | `(Bitspace.damage b i)` | flips bit i of the stored cell, for tests and for a program that tests its own recovery | need |
| 9 | the VM's own `(vec bool)` made ECC | every access pays 7 parities; the VM heap is not ours to guess at | no: use `Bitspace` where it matters |
| 10 | triple redundancy (TMR) | 200 % overhead; AMPL shows SECDED + scrub beats it | no |
| 11 | interleaved cells against multi-bit upsets (a neighbour pair flips together) | maybe: two flips in one cell are detected, not healed; interleaving 2 cells would heal them. Cheapest check: a measured MBU rate for the target memory | maybe |
| 12 | `(aos-split bytes stride)` : `(option (vec (vec u8)))` | stride planes, plane j holds bytes j, j+stride, ...; none unless stride > 0 divides the length | need |
| 13 | `(aos-join planes)` : `(option (vec u8))` | inverse; none when the planes differ in length | need |
| 14 | `Image` to-aos / from-aos on top of 12-13 | same results as now | need |
| 15 | typed struct fields to AoS (reflection) | needs macros that read a struct's fields; gael macros are not checked for it. Cheapest check: can a macro list a struct's fields? | maybe |
| 17 | SECDED(39,32) cells, for orbit-like numbers (AMPL: cheaper to scrub at 2 % CPU) | maybe: only if a program needs it; cheapest check: a second code constant in the gem and the same Lean proof for width 32 | maybe |
| 16 | a `.rs` transformer | not needed: gael itself, in the gem | no |

## Interfaces (worked)

```
(enable memlib/bitspace memlib/layout)
(def b (Bitspace.new 1000))
(b.set 70 true)
(Bitspace.damage b 70)             ; a cosmic ray: bit 70's cell has one bit flipped
(println "%v" (b.get 70))          ; true: healed on read
(def r (b.scrub))                  ; (Scrub :fixed 0 :lost 0): the read already healed it
(Bitspace.damage b 70) (Bitspace.damage b 71)   ; two rays in one cell
(b.get 5)                          ; none: the cell holding bits 0..63 is damaged

(def planes (aos-split bytes 4))   ; RGBA RGBA ... to R..., G..., B..., A...
(def back (aos-join planes))       ; = bytes
```

## Layout

Layout: `Bitspace`: `n : int` (bits), `cells : (vec u64)` (ceil(n/64) cells), `checks : (vec u8)` (one byte per cell, same index).

Cell `i` holds bits 64i..64i+63, bit `k` of the cell is bit `64i+k` of the space. Check byte `c` of a cell:
bits 0..6 = c7, the xor of the positions of the set data bits; bit 7 = parity of the 64 data bits and the 7 bits of c7.
Position of data bit k: the k-th number from 3 up that is not a power of two (so 3, 5, 6, 7, 9, ..., 71).
Invariant: `len cells = len checks = ceil(n/64)`; for an undamaged cell `c = encode(cell)`.

Decode, with s = xor of positions of set data bits xor c7 and o = parity(cell) xor parity(c7) xor bit 7:

| s | o | meaning | action |
|---|---|---|---|
| 0 | 0 | clean | none |
| 0 | 1 | the overall parity bit flipped | rewrite the check byte |
| power of 2 | 1 | check bit flipped | rewrite the check byte |
| position of data bit k | 1 | data bit k flipped | flip it back |
| other (outside 3..71) | 1 | 3 or more flips | lost |
| anything but 0 | 0 | two flips | lost |

## Limits

- Cost per `get`: 7 parities of masked words, about 110 VM ops: measured 1.0 microsecond per checked `get` against 18 ns for a `(vec bool)` get (1,000,000 reads, 2026-11-24). Hot paths that do not need safety use `(vec bool)`.
- Memory: 8 bytes data + 1 byte check per 64 bits = 12.5 % overhead.
- Failure model, assumed (no measurement of his hardware): ground DRAM 2.4e-11 flips per bit-hour (25,000 FIT per Mbit),
  low orbit 1000 times that; flips are independent; a cell is lost when two flips land in it between two checks.
  `ampl/bitspace.mod` picks the code and the scrub interval under these numbers.
- Silent corruption needs 3 flips in one cell between checks (detected as 2 or mis-healed): the model prints that rate too.

## Done

- [x] Lean (`lean4/`): `decode (encode w) = clean`; one flipped bit anywhere in the 72 is healed to `w`; two flipped
      bits are never healed to a wrong word (reported lost); `aos-join (aos-split x) = x`.
- [x] AMPL (`ampl/bitspace.mod`, `.dat`, `run`): SECDED(72,64) is the optimum for ground and orbit under the stated
      numbers, or the choice changes; `run` fails when `gems/memlib/bitspace.gael` disagrees with the chosen code.
      Result: ground (lambda 2.4e-11, loss budget 1e-6 words/h, 2 % CPU) = SECDED(72,64) scrubbed every 24 h; orbit (1000 times
      the rate, 1e-3/h) = SECDED(39,32) every 6 h at 2 % CPU, SECDED(72,64) hourly at 5 %. The gem has (72,64) only: the (39,32) variant is not built (it would be a `maybe` row 17).
- [x] `gems/memlib/bitspace.gael`, `layout.gael` with tests: every single flip of 72 bits in several words heals;
      every double flip of a word is lost; scrub counts; errors named.
- [x] Differential: the gem's encode equals the Lean oracle's on seeded random words.
- [~] MODELS.md, REFERENCE.md, language.md, PLAN.md updated; `./test` green on the staged state; the `get` cost measured.
- [ ] `maybe` rows 11 and 15 each have their cheap check run, and become need or no.
