# Prototype Design Decisions

Status: the **direction** below is agreed for planning, but no implementation
interface is frozen. The six design gates are in [todo.md](todo.md). A failed
gate changes the design before implementation begins; it does not invite a
local S1 workaround. The [architecture](architecture.md) defines component
ownership.

## 1. Output bindings

Direction: prefer a thin logical-only boundary exposing the old DECIDE
bindings and positional schema. Open: specify and test binding resolution,
projection maps, nested/correlated plans, serialization, and physical lowering.
If that interface cannot be made sound, use a boundary-scoped remap instead.

## 2. Value validation

Direction: NULL/non-finite checks are mandatory over all relevant source rows.
Try an always-true-or-throw relational guard before ranking. Open: establish
survival through pruning and optimizer movement, including unused `x`, zero
capacity, and parent filters. If existing operators cannot guarantee it,
choose a generic validation mechanism or narrow admission explicitly.

## 3. Optimizer movement

Direction: block only transformations that change the decision input or suppress
mandatory checks. Open: audit remaining passes and enforce that boundary while
retaining safe pruning and optimization inside the generated plan.

## 4. Explanation

Direction: create one structured direct-decision record with selected rule,
proof facts, guards, and miss reason. Open: expose it reliably in explanation
and profiling even when a logical-only boundary disappears from the physical
plan.

## 5. Feature control

Direction: DECIDE session setting `off` (default), `auto`, and `require`; no
syntax change. Open: validation, forced-solver conflict, `DIAGNOSE` policy,
and prepared-plan timing.

## 6. Numeric contract

Direction: use the solver's DOUBLE coefficient domain and initially admit a
finite non-negative capacity safely representable by rank. Open: exact range,
cast and zero behavior, error parity, and differential-test tolerance.

The first rule's deliberately narrower software contract is in
[02_first_rule/spec.md](../02_first_rule/spec.md). Narrow admission with a clean
solver fallback is a scope choice, not a substitute for a proof. Conversely,
matching one attractive subtree while ignoring another term is never eligible.

Decisions already supported by source inspection:

- Attempt before solver choice in `src/optimizer/decide/decide_optimizer.cpp`.
- Read the bound canonical tree, not a flattened prepared model built later in
  `src/optimizer/decide/decide_linear_form.cpp`.
- Preserve `BOOL`'s SQL `INTEGER` output (`decide_declarations_binder.cpp`) and
  child-plus-decision bindings (`logical_decide.cpp`).
- Construct bound logical operators, not SQL text to reparse. Ordinary execution
  remains the point of the feature; a specialized S1 executor is out of scope.
- Keep `DIAGNOSE` and explicitly forced solver tests on the solver path until an
  equivalent direct contract exists.
