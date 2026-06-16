#!/usr/bin/env python3
"""
L0-count penalty prototype (feature branch `norm-penalty-regularization`).

The proposal prioritizes "minimal edits / minimal cause" objectives and says to
prefer the NUMBER of edits (L0/count, "in the spirit of lasso") over the
magnitude (L1), because magnitude objectives return diffuse, unhelpful answers.

Finding: L0 needs NO new engine syntax today. A per-row indicator + Big-M does it:

    DECIDE x IS REAL, z IS BOOLEAN
    SUCH THAT <primary constraints>
          AND x <= ub * z          -- z must be 1 if x is used (Big-M = ub)
    MINIMIZE <primary> + lambda * SUM(z)   -- penalize the COUNT of edits

This driver sweeps lambda against the built CLI, traces the primary-cost vs
edit-count frontier, finds the (crisp, finite) lambda_max where the count hits
its minimum, and exposes alpha = lambda / lambda_max.

Repair instance: meet SUM(x) >= 12 from baseline 0. Two "big" items
(ub 10, rate 3 — expensive, high capacity) and four "small" items
(ub 3, rate 1 — cheap, low capacity). Minimizing COST wants many cheap small
items (diffuse, 4 edits); minimizing COUNT wants few big items (sparse, 2 edits).

Run from repo root:  python3 experiments/lambda_select/l0_count_path.py

Modeling note: `x <= ub*z` only forces z=1 when x>0; it does NOT force z=0 when
x=0. That's fine because SUM(z) is minimized (positive lambda pulls z tight), but
we report the TRUE edit count from x (x>tol), never the raw z, to stay honest.
"""

import subprocess, sys
from pathlib import Path

DECIDB = Path("build/release/decidb")
ITEMS  = [(1,10.0,3.0),(2,10.0,3.0),(3,3.0,1.0),(4,3.0,1.0),(5,3.0,1.0),(6,3.0,1.0)]
NEED   = 12.0
RATE   = {i: r for i,_,r in ITEMS}
VALUES = ",".join(f"({i},{ub},{r})" for i,ub,r in ITEMS)

def solve(lam):
    q = (f"CREATE TABLE items AS SELECT * FROM (VALUES {VALUES}) t(id,ub,rate);\n"
         f"SELECT id, x FROM items DECIDE x IS REAL, z IS BOOLEAN "
         f"SUCH THAT SUM(x) >= {NEED} AND x <= ub * z "
         f"MINIMIZE SUM(rate*x) + {lam} * SUM(z);")
    out = subprocess.run([str(DECIDB), "-csv", "-c", q], capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"solve failed at lambda={lam}:\n{out.stderr}")
    rows = [ln.split(",") for ln in out.stdout.strip().splitlines()[1:]]
    return {int(i): float(x) for i, x in rows}

def cost(x):  return sum(RATE[i]*x[i] for i in x)
def count(x): return sum(1 for v in x.values() if v > 1e-6)   # TRUE edits, from x

# --- trace the cost vs edit-count frontier ----------------------------------
lambdas = [0,1,3,5,7,9,11,13,16,20,40,100]
print("=== L0-count penalty: primary cost vs number of edits as lambda grows ===")
print(f"{'lambda':>7} | {'cost':>6} {'#edits':>7} | x by item (1..6)")
print("-"*52)
sols = {}
for lm in lambdas:
    x = solve(lm); sols[lm] = x
    xs = " ".join(f"{x[i]:4.1f}" for i,_,_ in ITEMS)
    print(f"{lm:7.1f} | {cost(x):6.1f} {count(x):7d} | {xs}")

# --- lambda_max: smallest lambda reaching the minimum achievable count -------
min_count = min(count(sols[lm]) for lm in lambdas)
lam_max   = next(lm for lm in lambdas if count(sols[lm]) == min_count)
print(f"\nminimum achievable edits = {min_count}")
print(f"lambda_max (first lambda reaching min count) = {lam_max}  "
      f"(crisp & finite for L0 — unlike ridge/L2)")

# --- scale-free alpha knob ---------------------------------------------------
print(f"\n=== alpha knob (alpha = lambda / lambda_max) ===")
print(f"{'alpha':>6} | {'lambda':>7} {'cost':>6} {'#edits':>7}")
print("-"*36)
for a in [0.0, 0.1, 0.25, 0.5, 0.75, 1.0]:
    x = solve(a*lam_max)
    print(f"{a:6.2f} | {a*lam_max:7.2f} {cost(x):6.1f} {count(x):7d}")
