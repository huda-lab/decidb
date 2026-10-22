# Work queue: direct solve — R1 continuous knapsack

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `08_direct_solve/architecture/rules.md`, `08_direct_solve/rules/r1_continuous_knapsack/definition.md`,
`08_direct_solve/framework/done.md`. Checks: `08_direct_solve/framework/todo.md` § How to work any task.

## 1. Foundations
- [ ] R1-08 Construction and proof for the admitted class — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-08
- [ ] H-36 Remove the fuzz suite — 08_direct_solve/framework/todo.md § H-36

## 2. Core rule (needs: 1)
- [ ] R1-02 One decision, one `<=` budget — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-02 (needs: R1-08)

## 3. Widenings (needs: 2)
- [ ] R1-03 Conformance fixture — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-03
- [ ] R1-04 Budget as `=`, `>=` or `BETWEEN` — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-04
- [ ] R1-05 Budget per group and under `WHEN` — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-05
- [ ] R1-06 Several decisions in one budget — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-06

## 4. Data-valued budget (needs: 3)
- [ ] R1-10 Budget from a column or an aggregate — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-10 (needs: R1-05)

## 5. Measurement (needs: 4)
- [ ] R1-07 Performance table — 08_direct_solve/rules/r1_continuous_knapsack/todo.md § R1-07

## Blocked
- [ ] H-02 Front end for the new language — 08_direct_solve/framework/todo.md § H-02 (needs: a stable language branch)
