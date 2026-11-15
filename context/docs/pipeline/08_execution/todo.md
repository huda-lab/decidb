# Stage 08 — Execution: open work

## Recursive formulation is underway

The top-down descent, per-node containers, bottom-up data evaluation, and frame pass match the deck's execution tree.
P9's F nodes also own ABS envelopes, Boolean products, L0 indicators, and nonlinear reducer definitions. The remaining
architecture gap is the deck's formulation at E/G/O/F nodes (ANR.pdf, pp. 111–119):

- The tree keeps node-owned operator algebra, but `DecideModelEmitter` converts it to row-indexed
  `EvaluatedConstraint` and `ReducerValueLinkSpec` records.
- `BuildSolverInput`, `FormulateModel`, `InlineReducerValueLinks`, and `SolverModel::Build` still perform the numeric
  formulation over flattened solver rows and columns.
- The output models for the three worked examples match the deck. Root E formulas and the remaining formula families
  are not yet recursively lowered from the tree nodes themselves.

The first recursive formulation slice composes a nested linear child at its enclosing G node. While walking the tree
children-first, `DecideModelEmitter::ValueLinks` writes a child SUM/AVG group's member formula into its parent's
member formula, using the child's exact keyed instance for each row. Nested MIN/MAX and frame values remain explicit.
This implements the G-to-G composition in the SUM-inside-SUM slides before the value links enter stage 06. The same
pass now composes child G SUM/AVG formulas into an aggregate E row. It carries each resulting decision's source row, so
stage 06 maps terms to columns without rediscovering reducer membership. MIN/MAX and frame children remain explicit.
The F1 SUM-inside-SUM rows now match pp. 116–119 exactly with only the six declared decision columns; MAX-inside-SUM
matches pp. 112–115 with two MAX helper columns and the same three root rows. Three-level SUM/AVG/SUM coefficients
also match the node-by-node formula.

This E slice applies to aggregate rows. Per-row clauses keep shared helper columns, since writing one query-wide SUM
into every row can multiply a compact formula into millions of terms. Next, extend the formulation records for
quadratic, composed, objective and function-node expressions. Keep the established solver model as the backend
boundary. Retain the old lowering only for shapes not yet migrated, compare exact model dumps at each stage, then
remove it after the full DECIDE suite covers the migrated forms.

Other documented edge differences remain in `done.md` §1a: collated frame error naming (D12), the D88 variant-name
edge, side-plan versus tree error order for a gated reducer argument, and unreferenced hidden records. Direct-solve
rules and DIAGNOSE's separate named-column scan also bypass or revisit parts of the normal tree execution; their scope
should be decided as the formulation work proceeds.
