#!/usr/bin/env python3
"""
Smart-lambda layer for penalty-regularized DECIDE objectives (feature branch
`norm-penalty-regularization`).

Context: the penalty mechanism ALREADY works in DeciDB --
    MINIMIZE SUM(cost*x) + lambda * SUM(POWER(x - t, 2))   (ridge / dense)
    MINIMIZE SUM(cost*x) + lambda * SUM(ABS(x - t))        (L1 / sparse-ish)
both parse and solve today. The open problem is choosing/interpreting lambda.

This driver sweeps lambda by shelling out to the built CLI, traces the
trade-off (L-)curve, finds the saturation point lambda_max (where the penalty
fully dominates and the solution stops moving), and exposes a scale-free knob
    alpha = lambda / lambda_max   in [0, 1]
so the user states "how much shape vs. goal" instead of a magic number.

Run from repo root:  python3 experiments/lambda_select/lambda_path.py
Requires:            build/release/decidb
"""

import subprocess, math, sys
from pathlib import Path

DECIDB = Path("build/release/decidb")

# --- the problem (data lives here so we can recompute objective/penalty) -----
COST   = [1.0, 2.0, 3.0, 4.0]      # per-item cost (primary objective coeffs)
TARGET = 2.0                        # ridge pulls each x toward this
UB     = 4.0                        # x upper bound
NEED   = 6.0                        # SUM(x) >= NEED
N      = len(COST)

VALUES = ",".join(f"({i+1},{c})" for i, c in enumerate(COST))

def solve(lam):
    """Run the penalty query at a given lambda; return the list of x values."""
    q = (f"CREATE TABLE items AS SELECT * FROM (VALUES {VALUES}) t(id,cost);\n"
         f"SELECT id, x FROM items DECIDE x IS REAL "
         f"SUCH THAT SUM(x) >= {NEED} AND x <= {UB} "
         f"MINIMIZE SUM(cost*x) + {lam} * SUM(POWER(x - {TARGET}, 2));")
    out = subprocess.run([str(DECIDB), "-csv", "-c", q],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"solve failed at lambda={lam}:\n{out.stderr}")
    rows = [ln.split(",") for ln in out.stdout.strip().splitlines()[1:]]  # skip header
    return [float(x) for _id, x in rows]

def primary(x):  return sum(COST[i]*x[i] for i in range(N))     # cost we pay
def ridge(x):    return sum((x[i]-TARGET)**2 for i in range(N)) # shape penalty
def spread(x):   m = sum(x)/N; return math.sqrt(sum((v-m)**2 for v in x)/N)

# --- 1. trace the L-curve ----------------------------------------------------
lambdas = [0.0] + [0.01*(2**k) for k in range(13)]
print("=== Ridge penalty L-curve (cost vs. shape as lambda grows) ===")
print(f"{'lambda':>8} | {'cost':>7} {'penalty':>8} {'spread':>7} | x values")
print("-"*64)
sols = {}
for lm in lambdas:
    x = solve(lm); sols[lm] = x
    xs = " ".join(f"{v:4.2f}" for v in x)
    print(f"{lm:8.3f} | {primary(x):7.3f} {ridge(x):8.3f} {spread(x):7.3f} | {xs}")

# --- 2. lambda_max = effective saturation -----------------------------------
# NOTE: ridge/L2 approaches the dense point ASYMPTOTICALLY (never exactly hits
# it at finite lambda), unlike L1 where lambda_max is a crisp finite breakpoint.
# So for L2 we define lambda_max as the smallest lambda that removes >=95% of the
# unpenalized "shape" (spread). For L1 you'd instead detect the exact collapse.
full      = solve(10_000.0)                  # penalty fully dominates
spread0   = spread(sols[0.0])
threshold = 0.05 * spread0                    # 95% of the way to dense
lam_max   = next((lm for lm in lambdas if spread(sols[lm]) <= threshold), lambdas[-1])
print(f"\nfully-penalized solution = {[round(v,3) for v in full]} "
      f"(uniform = pure 'dense', reached only asymptotically for L2)")
print(f"lambda_max (95% shape reduction) ~ {lam_max:.3f}")

# --- 3. scale-free alpha knob ------------------------------------------------
print(f"\n=== alpha knob (alpha = lambda / lambda_max) ===")
print(f"{'alpha':>6} | {'lambda':>8} {'cost':>7} {'spread':>7} | x values")
print("-"*56)
for a in [0.0, 0.05, 0.1, 0.25, 0.5, 1.0]:
    lm = a*lam_max
    x = solve(lm)
    xs = " ".join(f"{v:4.2f}" for v in x)
    print(f"{a:6.2f} | {lm:8.3f} {primary(x):7.3f} {spread(x):7.3f} | {xs}")
