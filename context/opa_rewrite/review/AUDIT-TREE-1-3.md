# Audit of TREE-1 … TREE-3 (independent reviewer, 2026-11-23) — summary of its report

Verdict standard (owner): KEEP if the built form is cleaner, simpler, no bloat, fewer ways to go wrong.

REMEDY, ranked:
1. Shape has FOUR writers and a sentinel (`CanonicalizeClauses` skips when `shape != INVALID`; scoped pass builds a
   second canonicalizer; `CanonicalizeFunctionRow` uses another rule; objective reuses the enum as a boolean).
   decide_canonicalizer.cpp:1586-1592,1517-1543,1098-1107,1252; decide_rewrite_scoped.cpp:575-579; decide_tree.cpp:623.
   Form: the canonicalizing boundary sets shape every time, nobody else (option-B patch) + delete the scoped pass's
   call + classify function rows with the same function. Needs owner's ruling on R26 wording ("readers never recompute").
2. "Canonicalize what changed" tail copied in 3 passes, variants in 3 more; ABS/bilinear/minmax never re-enter; guard
   is debug-only. Form: Template Method — callback marks `DecideOwnedBody::changed`; `EnumerateBodies` re-canonicalizes
   marked clauses once. Removes the Copy+Equals change detection (rhs_bounds.cpp:78-84, scoped.cpp:624-626).
3. `VerifyBoundReads` (decide_optimizer.cpp:79-98) is weaker than the rule: record the one fact (bound not query-wide)
   or make the two legitimate rewrites update the record.
4. `constraint_sources`: KEEP the table; drop duplicated facts (`between_partner`/`between_upper`, `qualifier`); fix
   design § 3.2.2/§ 3.4 which still say it is deleted.
5. Parser reducer node wraps a parsed call, prints by splicing after first `(`, 5 cast sites: hold the
   `FunctionExpression` by type. One AND splitter (delete the transformer's, transform_select_node.cpp:144-151).
6. Small: `written_objective`, `canonical_objective`, `objective_canonical_differs`, `source_columns` into
   `BoundDecide`; canonicalizer factory taking `BoundDecide &` (13 sites of 7 args); subquery collection through
   `EnumerateExpressions` (plan_decide.cpp:48-64); optimizer-only clause facts out of nodes.json.

KEEP: `ParsedDecide` struct; `BoundDecide` by composition; one `per` on a rewrite-made row; `when c: (a and b)`.
NEEDS OWNER (language): colon form `sum(D: e)` removal — owner did approve this earlier ("sum(D: e) is just a syntax
error") — and two tailored grammar errors removed (owner: "explore other remnants… remove them also").
