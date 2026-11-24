# lean4 - gael, machine-checked

Lean 4 proofs about gael's C code: a Lean model of each C function, theorems about the model, and a test that
holds the C code to the model. The shared parts (the audit command, bit lemmas, the oracle driver, the run steps)
are in [holyc/lean4](../../lean4/README.md); [core/lean4](../../core/lean4/README.md) shows a full differential
test on them.

## Status

Proved (core Lean, axioms propext and Quot.sound only): `Gael.Secded`, the Hamming SECDED (72,64) code of `gems/memlib/bitspace.gael`: a clean cell decodes to itself, any one flipped bit of the 72 is healed, any two are reported lost; `Gael.Layout`: AoS split then join gives the stream back, and the other way. Design: [docs/DESIGN-memlib.md](../docs/DESIGN-memlib.md).

## Run

```sh
gael/lean4/run     # build, axiom audit, source scan (needs elan; core Lean only, no Mathlib)
```
