# Stage 08 — Execution: open work

## P9 is complete; recursive formulation is still open

The top-down descent, per-node containers, bottom-up data evaluation, and frame pass match the deck's execution tree.
P9's F nodes also own ABS envelopes, Boolean products, L0 indicators, and nonlinear reducer definitions. The remaining
architecture gap is the deck's formulation at E/G/O/F nodes (ANR.pdf, pp. 111–119):

- The tree keeps node-owned operator algebra, but `DecideModelEmitter` converts it to row-indexed
  `EvaluatedConstraint` and `ReducerValueLinkSpec` records.
- `BuildSolverInput`, `FormulateModel`, `InlineReducerValueLinks`, and `SolverModel::Build` still perform the numeric
  formulation over flattened solver rows and columns.
- The output models for the three worked examples match the deck; their formulas are not yet recursively lowered from
  the tree nodes themselves.

The next implementation phase should move one formula family at a time into node-owned formulation, starting with
linear E/G terms and retaining the existing solver-row representation as the backend boundary. Each step needs exact
model comparisons and the full DECIDE suite before the old flat path is removed.

Other documented edge differences remain in `done.md` §1a: collated frame error naming (D12), the D88 variant-name
edge, side-plan versus tree error order for a gated reducer argument, and unreferenced hidden records. Direct-solve
rules and DIAGNOSE's separate named-column scan also bypass or revisit parts of the normal tree execution; their scope
should be decided as the formulation work proceeds.
