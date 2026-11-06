# Work queue: direct solve takes A2, single-facility location

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: `direct_solve/README.md`, `direct_solve/architecture/rules.md`,
`direct_solve/rules/a2_single_facility_location/definition.md`. Checks: `direct_solve/framework/todo.md` § How to work
any task.

Each batch finishes completely. Lanes: G2, beside H1; G3 after G2; G4 after G3.

## Owner decisions

- [ ] A2-4 Negative data weights on the square — direct_solve/rules/a2_single_facility_location/todo.md § A2-4
- [ ] A2-7 Whether A2 takes the L0 loss — direct_solve/rules/a2_single_facility_location/todo.md § A2-7

## G2. Keys and data bounds

- [ ] A2-2 Keyed decision `per K:` — direct_solve/rules/a2_single_facility_location/todo.md § A2-2
- [ ] A2-3 Bounds read from data — direct_solve/rules/a2_single_facility_location/todo.md § A2-3

## G3. Weights and other losses (after G2)

- [ ] A2-4 Data weights on the square — direct_solve/rules/a2_single_facility_location/todo.md § A2-4
- [ ] A2-5 Absolute loss — direct_solve/rules/a2_single_facility_location/todo.md § A2-5
- [ ] A2-6 Maximum absolute loss — direct_solve/rules/a2_single_facility_location/todo.md § A2-6
- [ ] A2-7 L0 loss — direct_solve/rules/a2_single_facility_location/todo.md § A2-7

## H1. Framework fixes found on the way

- [ ] H-47 A square's constant past a DOUBLE — direct_solve/framework/todo.md § H-47

## G4. Measurement (the orchestrator, after G3)

- [ ] A2-8 Benchmark — direct_solve/rules/a2_single_facility_location/todo.md § A2-8
