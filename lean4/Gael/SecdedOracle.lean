/- The oracle: Gael.Secded's `encode` run on 203 words (0, 1, all ones and 200 from a 64-bit LCG), one line each: the
   word in decimal and the check byte in decimal. lean4/oracle.gael prints the same lines from the gem; lean4/run diffs them. -/
import Gael.Secded

open Gael.Secded

def lcg (x : Nat) : Nat := (x * 6364136223846793005 + 1442695040888963407) % 2 ^ 64

def line (n : Nat) : String :=
  let e := encode (fun i => n.testBit i)
  s!"{n} {e.1 + (if e.2 then 128 else 0)}"

def main : IO Unit := do
  IO.println (line 0)
  IO.println (line 1)
  IO.println (line (2 ^ 64 - 1))
  let mut x := 1
  for _ in [0:200] do
    x := lcg x
    IO.println (line x)
