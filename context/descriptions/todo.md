# Work queue: finish direct-solve S1, A1 and the engine, then new classes

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `08_direct_solve/README.md`, `08_direct_solve/architecture/rules.md`.
Checks: `08_direct_solve/rules/_harness/todo.md` § How to work any task.

## 1. Quick
- [ ] H-15 shared test fixture defined once — 08_direct_solve/rules/_harness/todo.md § H-15
- [ ] N-03 strict-bound shifts call the shared function — 08_direct_solve/rules/_harness/todo.md § N-03
- [ ] N-07 one message and range check for 2^53 — 08_direct_solve/rules/_harness/todo.md § N-07
- [ ] N-05 one evaluate-to-DOUBLE function — 08_direct_solve/rules/_harness/todo.md § N-05
- [ ] N-06 test that both paths reject the same values — 08_direct_solve/rules/_harness/todo.md § N-06
- [ ] S1-14 S1 queries with no objective — 08_direct_solve/rules/s1/todo.md § S1-14

## 2. Larger
- [ ] S1-15 S1 offset or negated count body — 08_direct_solve/rules/s1/todo.md § S1-15
- [ ] S1-08 S1 limits from MIN, MAX, SUM, AVG, COUNT(col) — 08_direct_solve/rules/s1/todo.md § S1-08
- [ ] N-04 A1 and solver bound absorption are one function — 08_direct_solve/rules/_harness/todo.md § N-04
- [ ] H-14 read a stored table twice instead of holding rows — 08_direct_solve/rules/_harness/todo.md § H-14

## 3. Solver
- [ ] `BOOL` decision returns values outside 0 and 1 — 06_issues/bugs/todo.md § A `BOOL` decision takes a value outside 0 and 1

## 4. Measurements, quiet machine (needs: 2)
- [ ] H-16 re-measure memory with the built read-twice plan — 08_direct_solve/rules/_harness/todo.md § H-16 (needs: H-14)
- [ ] A1-05 refresh A1 performance table — 08_direct_solve/rules/a1/todo.md § A1-05
- [ ] S1-13 refresh S1 performance table — 08_direct_solve/rules/s1/todo.md § S1-13

## 5. New problem classes
- [ ] R-A3-01 A3 fixed-small multi-affine box — 08_direct_solve/rules/a3/todo.md § R-A3-01
- [ ] R-A5-01 A5 objective-aligned monotone corner — 08_direct_solve/rules/a5/todo.md § R-A5-01
- [ ] R-A6-01 A6 one squared residual over a box — 08_direct_solve/rules/a6/todo.md § R-A6-01
- [ ] R-A2-01 A2 one shared mean, median, midpoint or mode — 08_direct_solve/rules/a2/todo.md § R-A2-01
- [ ] R-A4-01 A4 bounded two-parameter least squares — 08_direct_solve/rules/a4/todo.md § R-A4-01
- [ ] R-S2-01 S2 nested upper quotas — 08_direct_solve/rules/s2/todo.md § R-S2-01
- [ ] R-S3-01 S3 maximum item count under one budget — 08_direct_solve/rules/s3/todo.md § R-S3-01
- [ ] R-S4-01 S4 exact count and budget, best item — 08_direct_solve/rules/s4/todo.md § R-S4-01
- [ ] R-R1-01 R1 continuous piecewise-linear allocation — 08_direct_solve/rules/r1/todo.md § R-R1-01
- [ ] R-R2-01 R2 bounded integer marginal units — 08_direct_solve/rules/r2/todo.md § R-R2-01
- [ ] R-R3-01 R3 strictly convex quadratic allocation — 08_direct_solve/rules/r3/todo.md § R-R3-01
- [ ] R-R4-01 R4 proportional max-min fairness — 08_direct_solve/rules/r4/todo.md § R-R4-01
- [ ] R-N1-01 N1 projection under one sum-of-squares bound — 08_direct_solve/rules/n1/todo.md § R-N1-01
- [ ] R-N2-01 N2 linear support over a diagonal ellipsoid — 08_direct_solve/rules/n2/todo.md § R-N2-01
- [ ] R-O1-01 O1 ordered complete assignment — 08_direct_solve/rules/o1/todo.md § R-O1-01
- [ ] R-O2-01 O2 balanced one-dimensional Monge transport — 08_direct_solve/rules/o2/todo.md § R-O2-01
- [ ] R-D1-01 D1 exact keyed decomposition — 08_direct_solve/rules/d1/todo.md § R-D1-01

## 6. Blocked
- [ ] S1-12 one per-row bound builder for S1 and A1 — 08_direct_solve/rules/s1/todo.md § S1-12 (needs: R-A3-01, R-A5-01 or R-A6-01 built)
- [ ] H-03 cost choice among competing direct plans — 08_direct_solve/rules/_harness/todo.md § H-03 (needs: two rules proving one query)
- [ ] H-02 adapter for the new language — 08_direct_solve/rules/_harness/todo.md § H-02 (needs: a stable language branch)
