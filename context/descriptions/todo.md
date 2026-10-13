# Work queue: direct-solve R1, the full definition

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `.claude/CLAUDE.md`, `08_direct_solve/README.md`, `08_direct_solve/architecture/rules.md`,
`08_direct_solve/rules/r1/definition.md`. Checks: `08_direct_solve/rules/_harness/todo.md` § How to work any task.

## 1. Owner decisions
- [ ] R1-00 settle R1's scope — 08_direct_solve/rules/r1/todo.md § R1-00

## 3. Core rule (needs: 1)
- [ ] R1-02 one decision, one `<=` budget — 08_direct_solve/rules/r1/todo.md § R1-02
- [ ] R1-03 contract fixture and fuzz generator — 08_direct_solve/rules/r1/todo.md § R1-03 (needs: R1-02)

## 4. Widenings (needs: 3)
- [ ] R1-04 budget as `=`, `>=` or `BETWEEN` — 08_direct_solve/rules/r1/todo.md § R1-04
- [ ] R1-05 budget per group and under `WHEN` — 08_direct_solve/rules/r1/todo.md § R1-05
- [ ] R1-06 shrinking returns across several decisions — 08_direct_solve/rules/r1/todo.md § R1-06

## 5. Unblocked by R1 (needs: R1-02)
- [ ] S1-12 S1's pins use the shared range reader — 08_direct_solve/rules/s1/todo.md § S1-12

## 6. Measurement (needs: 4)
- [ ] R1-07 performance table — 08_direct_solve/rules/r1/todo.md § R1-07
