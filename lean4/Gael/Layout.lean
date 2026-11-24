/- Gael.Layout: AoS and SoA over a flat stream, as gems/memlib/layout.gael does it. The stream is `x : Nat → α`, the
   stride s is the number of fields per record; plane j holds field j of every record. Split then join gives the stream
   back, and join then split gives the planes back, so no byte is lost, doubled or moved. -/

namespace Gael.Layout

def split {α : Type} (s : Nat) (x : Nat → α) : Nat → Nat → α := fun j r => x (r * s + j)
def join {α : Type} (s : Nat) (p : Nat → Nat → α) : Nat → α := fun i => p (i % s) (i / s)

theorem join_split {α : Type} (s : Nat) (x : Nat → α) (i : Nat) : join s (split s x) i = x i := by
  unfold join split
  congr 1
  have := Nat.div_add_mod i s
  rw [Nat.mul_comm] at this
  exact this

theorem split_join {α : Type} (s : Nat) (p : Nat → Nat → α) (j : Nat) (hj : j < s) (r : Nat) :
    split s (join s p) j r = p j r := by
  unfold split join
  have h1 : (r * s + j) % s = j := by rw [Nat.mul_comm, Nat.mul_add_mod]; exact Nat.mod_eq_of_lt hj
  have h2 : (r * s + j) / s = r := by
    rw [Nat.mul_comm, Nat.mul_add_div (by omega), Nat.div_eq_of_lt hj]; rfl
  rw [h1, h2]

end Gael.Layout
