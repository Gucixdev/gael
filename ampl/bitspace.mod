# Which code protects a bit space, and how often to scrub it: least overhead that keeps the loss rate and the CPU
# inside their budgets. Assumptions are in bitspace.dat and docs/DESIGN-memlib.md (a wrong flip rate gives a perfectly
# optimal wrong answer).
set CODES;                      # none, parity, secded64, secded32, tmr
set SCRUBS;                     # scrub intervals
param w{CODES} >= 1;            # data bits per word
param c{CODES} >= 0;            # check bits per word
param fixes{CODES} binary;      # 1 when one flipped bit is healed
param ops{CODES} >= 0;          # VM operations to check one word
param t{SCRUBS} > 0;            # scrub interval in hours
param N > 0;                    # bits to protect
param lambda > 0;               # flips per bit per hour
param ns_op > 0;                # nanoseconds per VM operation
param max_loss >= 0;            # words lost (or flips met unnoticed) per hour we accept
param max_cpu >= 0;             # share of one core the scrubbing may take

# words lost per hour: a healing code loses a word when 2 flips land in it between two scrubs, a detecting or empty
# one loses it at the first flip
param loss{k in CODES, s in SCRUBS} :=
    if fixes[k] = 1 then (N / w[k]) * ((w[k] + c[k]) * (w[k] + c[k] - 1) / 2) * lambda^2 * t[s]
    else N * (w[k] + c[k]) / w[k] * lambda;
param cpu{k in CODES, s in SCRUBS} := (N / w[k]) * ops[k] * ns_op * 1e-9 / t[s] / 3600;
param extra{k in CODES} := N * c[k] / w[k];            # check bits on top of N

var x{CODES, SCRUBS} binary;
minimize overhead: sum{k in CODES, s in SCRUBS} (extra[k] + 1e3 * cpu[k, s]) * x[k, s];
s.t. one: sum{k in CODES, s in SCRUBS} x[k, s] = 1;
s.t. loss_ok: sum{k in CODES, s in SCRUBS} loss[k, s] * x[k, s] <= max_loss;
s.t. cpu_ok:  sum{k in CODES, s in SCRUBS} cpu[k, s] * x[k, s] <= max_cpu;
