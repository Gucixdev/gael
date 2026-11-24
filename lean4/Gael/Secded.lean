/- Gael.Secded: the Hamming SECDED (72,64) code of gems/memlib/bitspace.gael, and what it guarantees.
   A cell is 64 data bits `w : Nat → Bool` (only indices below 64 count), 7 check bits `c : Nat` (below 128) and an
   overall parity bit `o`. Data bit k has the code position `pos k`: the k-th number from 3 up that is not a power of 2.
   Theorems: a clean cell decodes to itself; any one flipped bit among the 72 is healed; any two are reported lost
   (never healed to a wrong word). -/

namespace Gael.Secded

/-- the 64 positions, 3 up to 71 without the powers of two -/
def positions : List Nat :=
  (List.range 72).filter (fun p => decide (3 ≤ p) && p &&& (p - 1) != 0)

def pos (k : Nat) : Nat := positions.getD k 0

/-- xor of the positions of the set bits among the first n data bits -/
def X (w : Nat → Bool) : Nat → Nat
  | 0 => 0
  | n + 1 => X w n ^^^ (if w n then pos n else 0)

/-- parity of the first n data bits -/
def P (w : Nat → Bool) : Nat → Bool
  | 0 => false
  | n + 1 => xor (P w n) (w n)

/-- parity of the 7 bits of a check number -/
def par7 (c : Nat) : Bool := (List.range 7).foldl (fun a i => xor a (c.testBit i)) false

def flip (w : Nat → Bool) (k : Nat) : Nat → Bool := fun i => if i = k then !w i else w i

/-- what the stored check byte should be: c7 and the overall bit -/
def encode (w : Nat → Bool) : Nat × Bool :=
  let c := X w 64
  (c, xor (P w 64) (par7 c))

/-- what to do with syndrome s and overall-parity mismatch odd: the healed data, or None when lost -/
def verdict (w : Nat → Bool) (s : Nat) (odd : Bool) : Option (Nat → Bool) :=
  if !odd then
    (if s = 0 then some w else none)
  else if s = 0 then some w
  else if s < 128 ∧ s &&& (s - 1) = 0 then some w
  else
    match (List.range 64).find? (fun k => pos k == s) with
    | some k => some (flip w k)
    | none => none

/-- the received cell: Some the healed data, None when the cell is lost -/
def decode (w : Nat → Bool) (c : Nat) (o : Bool) : Option (Nat → Bool) :=
  verdict w (X w 64 ^^^ c) (xor (xor (P w 64) (par7 c)) o)

theorem flip_flip (w : Nat → Bool) (k : Nat) : flip (flip w k) k = w := by
  funext i; unfold flip; by_cases h : i = k <;> simp [h]

theorem xor_swap (a b c : Nat) : a ^^^ (b ^^^ c) = b ^^^ (a ^^^ c) := by
  rw [← Nat.xor_assoc, Nat.xor_comm a b, Nat.xor_assoc]

theorem xor_cancel (a p : Nat) : p ^^^ (a ^^^ p) = a := by
  rw [Nat.xor_comm a p, ← Nat.xor_assoc, Nat.xor_self, Nat.zero_xor]

theorem X_congr (w w' : Nat → Bool) (n : Nat) (h : ∀ i, i < n → w' i = w i) : X w' n = X w n := by
  induction n with
  | zero => rfl
  | succ n ih =>
    simp only [X]
    rw [ih (fun i hi => h i (Nat.lt_succ_of_lt hi)), h n (Nat.lt_succ_self n)]

theorem X_flip (w : Nat → Bool) (k n : Nat) (hk : k < n) : X (flip w k) n = X w n ^^^ pos k := by
  induction n with
  | zero => omega
  | succ n ih =>
    simp only [X]
    by_cases h : k = n
    · subst h
      have := X_congr (flip w k) w k (fun i hi => by simp [flip, Nat.ne_of_lt hi])
      rw [this]
      by_cases hw : w k
      · simp only [flip, hw, ite_true, Bool.not_true]
        simp [Nat.xor_assoc, Nat.xor_self]
      · simp [flip, hw, Nat.xor_comm]
    · have hk' : k < n := by omega
      rw [ih hk']
      have : flip w k n = w n := by simp [flip, Ne.symm h]
      rw [this]
      generalize (if w n = true then pos n else 0) = t
      rw [Nat.xor_assoc, Nat.xor_assoc, Nat.xor_comm (pos k)]

theorem P_congr (w w' : Nat → Bool) (n : Nat) (h : ∀ i, i < n → w' i = w i) : P w' n = P w n := by
  induction n with
  | zero => rfl
  | succ n ih =>
    simp only [P]
    rw [ih (fun i hi => h i (Nat.lt_succ_of_lt hi)), h n (Nat.lt_succ_self n)]

theorem P_flip (w : Nat → Bool) (k n : Nat) (hk : k < n) : P (flip w k) n = !P w n := by
  induction n with
  | zero => omega
  | succ n ih =>
    simp only [P]
    by_cases h : k = n
    · subst h
      rw [P_congr (flip w k) w k (fun i hi => by simp [flip, Nat.ne_of_lt hi])]
      cases hw : w k <;> simp [flip, hw]
    · rw [ih (by omega)]
      have : flip w k n = w n := by simp [flip, Ne.symm h]
      rw [this]
      cases P w n <;> cases w n <;> simp

set_option maxRecDepth 100000 in
/-- flipping check bit j of a 7-bit number flips its parity -/
theorem par7_flip : ∀ c, c < 128 → ∀ j, j < 7 → par7 (c ^^^ (1 <<< j)) = !par7 c := by decide

theorem pos_ne_zero : ∀ k, k < 64 → pos k ≠ 0 := by decide
theorem pos_lt : ∀ k, k < 64 → pos k < 128 := by decide
set_option maxRecDepth 100000 in
theorem pos_not_pow2 : ∀ k, k < 64 → ¬ (pos k &&& (pos k - 1) = 0) := by decide
theorem pos_lt_all : ∀ k, pos k < 128 := by
  intro k
  by_cases h : k < 64
  · exact pos_lt k h
  · have hl : positions.length = 64 := by decide
    have : positions.length ≤ k := by omega
    have hn : positions[k]? = none := List.getElem?_eq_none this
    simp [pos, List.getD_eq_getElem?_getD, hn]
set_option maxRecDepth 100000 in
theorem pos_inj : ∀ a, a < 64 → ∀ b, b < 64 → pos a = pos b → a = b := by decide
set_option maxRecDepth 100000 in
theorem find_pos : ∀ k, k < 64 → (List.range 64).find? (fun j => pos j == pos k) = some k := by decide

theorem X_lt (w : Nat → Bool) : ∀ n, X w n < 128 := by
  intro n; induction n with
  | zero => simp [X]
  | succ n ih =>
    simp only [X]
    have h : (if w n then pos n else 0) < 128 := by
      split
      · exact pos_lt_all n
      · omega
    exact Nat.xor_lt_two_pow (n := 7) ih h

theorem pow2_facts : ∀ j, j < 7 → (1 <<< j) < 128 ∧ (1 <<< j) &&& ((1 <<< j) - 1) = 0 ∧ 1 <<< j ≠ 0 := by decide

theorem verdict_clean (w : Nat → Bool) : verdict w 0 false = some w := by simp [verdict]
theorem verdict_zero (w : Nat → Bool) : verdict w 0 true = some w := by simp [verdict]
theorem verdict_pow2 (w : Nat → Bool) (s : Nat) (h : s < 128) (h2 : s &&& (s - 1) = 0) (h0 : s ≠ 0) :
    verdict w s true = some w := by simp [verdict, h0, h, h2]
theorem verdict_data (w : Nat → Bool) (k : Nat) (hk : k < 64) : verdict w (pos k) true = some (flip w k) := by
  have h1 := pos_ne_zero k hk
  have h2 := pos_not_pow2 k hk
  have h3 := find_pos k hk
  have h4 : ¬ (pos k < 128 ∧ pos k &&& (pos k - 1) = 0) := fun h => h2 h.2
  simp [verdict, h1, h4, h3]
theorem verdict_two (w : Nat → Bool) (s : Nat) (h : s ≠ 0) : verdict w s false = none := by simp [verdict, h]

/-- a clean cell decodes to its data -/
theorem decode_clean (w : Nat → Bool) : decode w (encode w).1 (encode w).2 = some w := by
  have : X w 64 ^^^ (encode w).1 = 0 := by simp [encode, Nat.xor_self]
  simp only [decode, this]
  have : xor (xor (P w 64) (par7 (encode w).1)) (encode w).2 = false := by
    simp [encode]
  rw [this, verdict_clean]


/-- the flips: data bit k, check bit j, the overall bit -/
theorem heal_data (w : Nat → Bool) (k : Nat) (hk : k < 64) :
    decode (flip w k) (encode w).1 (encode w).2 = some w := by
  have hs : X (flip w k) 64 ^^^ (encode w).1 = pos k := by
    rw [X_flip w k 64 hk]; simp only [encode]
    rw [Nat.xor_comm (X w 64) (pos k), Nat.xor_assoc, Nat.xor_self, Nat.xor_zero]
  have ho : xor (xor (P (flip w k) 64) (par7 (encode w).1)) (encode w).2 = true := by
    rw [P_flip w k 64 hk]; simp [encode]
  have := verdict_data (flip w k) k hk
  simp only [decode, hs, ho, this, flip_flip]

theorem heal_check (w : Nat → Bool) (j : Nat) (hj : j < 7) :
    decode w ((encode w).1 ^^^ (1 <<< j)) (encode w).2 = some w := by
  obtain ⟨h1, h2, h3⟩ := pow2_facts j hj
  have hs : X w 64 ^^^ ((encode w).1 ^^^ (1 <<< j)) = 1 <<< j := by
    simp [encode, ← Nat.xor_assoc, Nat.xor_self]
  have hc : (encode w).1 < 128 := X_lt w 64
  have ho : xor (xor (P w 64) (par7 ((encode w).1 ^^^ (1 <<< j)))) (encode w).2 = true := by
    rw [par7_flip _ hc j hj]; simp [encode]
  simp only [decode, hs, ho]
  exact verdict_pow2 w _ h1 h2 h3

theorem heal_overall (w : Nat → Bool) :
    decode w (encode w).1 (!(encode w).2) = some w := by
  have hs : X w 64 ^^^ (encode w).1 = 0 := by simp [encode, Nat.xor_self]
  have ho : xor (xor (P w 64) (par7 (encode w).1)) (!(encode w).2) = true := by
    simp [encode]
  simp only [decode, hs, ho]
  exact verdict_zero w


/-! Two flipped bits are never healed to a wrong word: every pair of positions among the 72 gives `none`. -/

theorem xor_aux1 (x a b : Nat) : x ^^^ a ^^^ b ^^^ x = a ^^^ b := by
  rw [Nat.xor_comm _ x, ← Nat.xor_assoc, ← Nat.xor_assoc, Nat.xor_self, Nat.zero_xor]
theorem xor_aux2 (x a b : Nat) : x ^^^ (x ^^^ a ^^^ b) = a ^^^ b := by
  rw [← Nat.xor_assoc, ← Nat.xor_assoc, Nat.xor_self, Nat.zero_xor]
theorem xor_aux3 (x p e : Nat) : (x ^^^ p) ^^^ (x ^^^ e) = p ^^^ e := by
  rw [Nat.xor_comm x p, Nat.xor_assoc, ← Nat.xor_assoc x x e, Nat.xor_self, Nat.zero_xor]
theorem xor_aux4 (x p : Nat) : x ^^^ p ^^^ x = p := by
  rw [Nat.xor_comm x p, Nat.xor_assoc, Nat.xor_self, Nat.xor_zero]
theorem xor_ne (a b : Nat) (h : a ≠ b) : a ^^^ b ≠ 0 := by
  intro h0
  apply h
  have := congrArg (fun x => x ^^^ b) h0
  simpa [Nat.xor_assoc, Nat.xor_self] using this

theorem pow2_ne_pos (k j : Nat) (hk : k < 64) (hj : j < 7) : pos k ≠ 1 <<< j := by
  intro h
  have h2 := pos_not_pow2 k hk
  obtain ⟨_, h4, _⟩ := pow2_facts j hj
  apply h2; rw [h]; exact h4

set_option maxRecDepth 100000 in
theorem pow2_inj : ∀ j, j < 7 → ∀ j', j' < 7 → j ≠ j' → (1 <<< j) ≠ (1 <<< j') := by decide

theorem lost_dd (w : Nat → Bool) (a b : Nat) (ha : a < 64) (hb : b < 64) (hab : a ≠ b) :
    decode (flip (flip w a) b) (encode w).1 (encode w).2 = none := by
  have hs : X (flip (flip w a) b) 64 ^^^ (encode w).1 = pos a ^^^ pos b := by
    rw [X_flip _ b 64 hb, X_flip w a 64 ha]; simp only [encode]; exact xor_aux1 _ _ _
  have ho : xor (xor (P (flip (flip w a) b) 64) (par7 (encode w).1)) (encode w).2 = false := by
    rw [P_flip _ b 64 hb, P_flip w a 64 ha]; simp [encode]
  have hne : pos a ^^^ pos b ≠ 0 := xor_ne _ _ (fun h => hab (pos_inj a ha b hb h))
  simp only [decode, hs, ho]; exact verdict_two _ _ hne

theorem lost_dc (w : Nat → Bool) (a j : Nat) (ha : a < 64) (hj : j < 7) :
    decode (flip w a) ((encode w).1 ^^^ (1 <<< j)) (encode w).2 = none := by
  have hs : X (flip w a) 64 ^^^ ((encode w).1 ^^^ (1 <<< j)) = pos a ^^^ (1 <<< j) := by
    rw [X_flip w a 64 ha]; simp only [encode]; exact xor_aux3 _ _ _
  have hc : (encode w).1 < 128 := X_lt w 64
  have ho : xor (xor (P (flip w a) 64) (par7 ((encode w).1 ^^^ (1 <<< j)))) (encode w).2 = false := by
    rw [P_flip w a 64 ha, par7_flip _ hc j hj]; simp [encode]
  have hne : pos a ^^^ (1 <<< j) ≠ 0 := xor_ne _ _ (pow2_ne_pos a j ha hj)
  simp only [decode, hs, ho]; exact verdict_two _ _ hne

theorem lost_do (w : Nat → Bool) (a : Nat) (ha : a < 64) :
    decode (flip w a) (encode w).1 (!(encode w).2) = none := by
  have hs : X (flip w a) 64 ^^^ (encode w).1 = pos a := by
    rw [X_flip w a 64 ha]; simp only [encode]; exact xor_aux4 _ _
  have ho : xor (xor (P (flip w a) 64) (par7 (encode w).1)) (!(encode w).2) = false := by
    rw [P_flip w a 64 ha]; simp [encode]
  simp only [decode, hs, ho]; exact verdict_two _ _ (pos_ne_zero a ha)

theorem lost_cc (w : Nat → Bool) (j j' : Nat) (hj : j < 7) (hj' : j' < 7) (hne : j ≠ j') :
    decode w (((encode w).1 ^^^ (1 <<< j)) ^^^ (1 <<< j')) (encode w).2 = none := by
  have hs : X w 64 ^^^ (((encode w).1 ^^^ (1 <<< j)) ^^^ (1 <<< j')) = (1 <<< j) ^^^ (1 <<< j') := by
    simp only [encode]; exact xor_aux2 _ _ _
  have hc : (encode w).1 < 128 := X_lt w 64
  have hc2 : (encode w).1 ^^^ (1 <<< j) < 128 := by
    obtain ⟨h1, _, _⟩ := pow2_facts j hj
    exact Nat.xor_lt_two_pow (n := 7) hc h1
  have ho : xor (xor (P w 64) (par7 (((encode w).1 ^^^ (1 <<< j)) ^^^ (1 <<< j')))) (encode w).2 = false := by
    rw [par7_flip _ hc2 j' hj', par7_flip _ hc j hj]; simp [encode]
  have hne2 : (1 <<< j) ^^^ (1 <<< j') ≠ 0 := xor_ne _ _ (pow2_inj j hj j' hj' hne)
  simp only [decode, hs, ho]; exact verdict_two _ _ hne2

theorem lost_co (w : Nat → Bool) (j : Nat) (hj : j < 7) :
    decode w ((encode w).1 ^^^ (1 <<< j)) (!(encode w).2) = none := by
  obtain ⟨_, _, h3⟩ := pow2_facts j hj
  have hs : X w 64 ^^^ ((encode w).1 ^^^ (1 <<< j)) = 1 <<< j := by
    simp [encode, ← Nat.xor_assoc, Nat.xor_self]
  have hc : (encode w).1 < 128 := X_lt w 64
  have ho : xor (xor (P w 64) (par7 ((encode w).1 ^^^ (1 <<< j)))) (!(encode w).2) = false := by
    rw [par7_flip _ hc j hj]; simp [encode]
  simp only [decode, hs, ho]; exact verdict_two _ _ h3

end Gael.Secded
