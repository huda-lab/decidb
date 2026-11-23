#include "duckdb/common/decide_profile.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"
#include "duckdb/decidb/solver/ilp_solver.hpp"
#include "duckdb/optimizer/decide/decide_solver_gate.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <functional>

namespace duckdb {

//! A column name as SQL: quoted only where SQL requires it. An unreserved keyword (`zone`) is a valid
//! column reference as it stands, so it stays bare.
static string QuoteColumnName(const string &name) {
	if (KeywordHelper::RequiresQuotes(name) &&
	    KeywordHelper::KeywordCategoryType(name) != KeywordCategory::KEYWORD_UNRESERVED) {
		return KeywordHelper::WriteQuoted(name, '"');
	}
	return name;
}

//! The name the user wrote for each column of the DECIDE input, empty for a column they named nothing. Read off the
//! plan before CreatePlan moves the child's expressions out; the unbounded diagnosis is its only reader.
static vector<string> UserWrittenColumnNames(LogicalDecide &op) {
	// Capture child column bindings BEFORE CreatePlan: several logical operators
	// (notably LogicalProjection) move their `expressions` vector into the physical
	// op during CreatePlan, which leaves GetColumnBindings() returning an empty
	// vector afterward. LogicalGet isn't affected because its bindings come from
	// column_ids, which is why base-table keyed scopes worked and subquery /
	// CTE-backed scopes silently collapsed to a single class.
	auto child_bindings = op.children[0]->GetColumnBindings();
	// Capture user-written column names off the child projection BEFORE CreatePlan
	// moves its `expressions` vector out (same reason as child_bindings above). These
	// back-fill names for columns that appear ONLY in the outer SELECT — never in the
	// DECIDE clause — so the unbounded diagnosis can characterize an escaping slice by
	// them. We keep only names the user actually wrote: an explicit `AS name` alias, or
	// a bare source-column reference. A computed projection expression with no alias
	// carries only a generated name (its ToString), which we deliberately drop so the
	// diagnosis never prints a label the user never typed.
	// Resolve each child column to the name the USER wrote for it, using the map the
	// binder captured from its BindContext (LogicalDecide::source_column_*). That is the
	// same table name resolution itself used, so every source kind is covered uniformly:
	// a base table's catalog names, a `t(a, b, c)` alias list over `(VALUES ...)`, a
	// subquery's or a CTE's output names. Reading names off the plan instead cannot work
	// — an alias list is recorded on the binding and never reaches the plan, whose
	// projection carries the binder's positional `col0` / `col1` placeholders.
	//
	// Captured BEFORE CreatePlan for the same reason as `child_bindings`: several
	// operators move their contents out during planning.
	//
	// The map is keyed by DecideSourceColumnKey: a scan's column by its table ordinal, any other by its binding.
	// Getting this wrong silently mislabels one column as another rather than failing.
	//
	// The binder's map answers "what is this column called", not "did the user write that
	// name": a projection expression with no alias is recorded under its own `ToString`, so
	// an unaliased computed column would be labelled `CASE WHEN ((i <= 25)) THEN ('A') ELSE
	// 'B' END`. The plan is the right place to judge that — it still holds the expression —
	// so the key names no column for it, while the spelling comes from the binder.
	vector<string> child_userwritten_names(child_bindings.size(), string());
	for (idx_t i = 0; i < child_bindings.size(); i++) {
		auto key = DecideSourceColumnKey(*op.children[0], child_bindings[i], op.bound.source_columns);
		if (key.column_index == DConstants::INVALID_INDEX) {
			continue;
		}
		for (const auto &source_column : op.bound.source_columns) {
			if (source_column.binding == key) {
				child_userwritten_names[i] = source_column.name;
				break;
			}
		}
	}
	return child_userwritten_names;
}

//! Name each column of the DECIDE input as the unbounded diagnosis reads it: `input_column_names` and
//! `input_column_in_clause`, aligned with the materialized data chunk. Only a query under DIAGNOSE reads them, so only
//! one under DIAGNOSE builds them. `child_userwritten_names` is UserWrittenColumnNames of the plan the operator was
//! made from.
static void NameInputColumns(DecideDisplay &display, const DecideTree &tree, LogicalDecide &op,
                             const vector<string> &child_userwritten_names) {
	// Resolve source column names for the surviving (post-pruning) child columns,
	// positionally aligned with the materialized data chunk. Used by the unbounded
	// diagnosis to label escaping categorical groups (affected_rows).
	//
	// Harvest names from the DECIDE clause's own bound references to source columns
	// (`when` / `per` / `by` / objective / constraint / key columns of keyed decisions). By physical-plan
	// time the column-binding resolver has rewritten table-column references to
	// BoundReferenceExpressions whose `index` is the position in the child data chunk
	// — exactly the indexing of gstate.data — so this aligns directly and survives
	// pruning (decision-variable references stay BoundColumnRef and are skipped).
	// Columns only in the outer SELECT (not the DECIDE clause) get no name from this
	// harvest; they are back-filled below from the child projection's user-written
	// names. Anything still empty after that (a computed column with no user alias) is
	// left blank on purpose: the unbounded diagnosis suppresses categorical rules over
	// unnamed columns rather than inventing a positional `colN` the user never typed.
	display.input_column_names.assign(child_userwritten_names.size(), string());
	display.input_column_in_clause.assign(child_userwritten_names.size(), false);
	// Names are kept as SQL, as the query writes them (`t.depot`, `"my col"`), so a remedy that prints one is
	// pasteable. A reference is quoted from the DECIDE text at its query location, as a message quotes a clause; one
	// the text does not hold (a column only the SELECT list names, a reference rewritten without its location) keeps
	// `fallback`: its bare column name, or a relation element's `D.depotID` (DecideKeyElement::name).
	std::function<void(const Expression &)> harvest;
	auto written_name = [&](const BoundReferenceExpression &ref, const string &fallback) {
		string text;
		int binding;
		return display.source_text.CutExpr(ref.GetQueryLocation(), text, binding) ? text : fallback;
	};
	auto harvest_key = [&](idx_t key) {
		for (auto &element : DecideKeyElements(tree.keys, key)) {
			auto &expr = *element.expression;
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_REF) {
				harvest(expr);
				continue;
			}
			auto &ref = expr.Cast<BoundReferenceExpression>();
			if (ref.index < display.input_column_names.size()) {
				display.input_column_in_clause[ref.index] = true;
				if (display.input_column_names[ref.index].empty()) {
					display.input_column_names[ref.index] = written_name(ref, element.name);
				}
			}
		}
	};
	harvest = [&](const Expression &e) {
		if (e.GetExpressionClass() == ExpressionClass::BOUND_REF) {
			auto &r = e.Cast<BoundReferenceExpression>();
			if (r.index < display.input_column_names.size()) {
				// Reached from the DECIDE clause, so it explains rather than correlates
				// — recorded even when an earlier reference already supplied the name.
				display.input_column_in_clause[r.index] = true;
				if (display.input_column_names[r.index].empty()) {
					display.input_column_names[r.index] =
					    written_name(r, QuoteColumnName(StripDecideTags(r.GetName())));
				}
			}
		}
		ExpressionIterator::EnumerateChildren(e, harvest);
	};
	// A `per K:` names its key after its clause, as written.
	auto harvest_clause = [&](const BoundDecideClause &clause) {
		harvest(*clause.body);
		if (clause.scope.when) {
			harvest(*clause.scope.when);
		}
		if (clause.scope.HasPer()) {
			harvest_key(clause.scope.class_key);
		}
	};
	for (auto &clause : display.clauses) {
		harvest_clause(clause);
	}
	if (display.objective.body) {
		harvest_clause(display.objective);
	}
	// A `by` key and a scope check read the clause's data too. A key only a declaration writes names no column: a
	// column the clauses never name stays unnamed.
	for (idx_t key = 0; key < tree.keys.size(); key++) {
		if (tree.keys[key].written_as_by) {
			harvest_key(key);
		}
	}
	for (auto &signature : tree.signatures) {
		if (signature.rows) {
			harvest(*signature.rows);
		}
		harvest_key(signature.key);
		for (auto &field : signature.fields) {
			if (field.value) {
				harvest(*field.value);
			}
			harvest_key(field.key);
		}
	}
	// A lifted reducer's expressions live beside the tree: a helper decision's (helper_definitions). The columns
	// they read explain the answer too.
	for (auto &link : op.helper_definitions) {
		if (link->kind != DecideHelperKind::REDUCER_VALUE) {
			continue;
		}
		auto *filter = link->Frame() == DConstants::INVALID_INDEX ? link->Reducer().filter.get() : nullptr;
		for (auto *expr : {link->Member().get(), filter, link->when.get()}) {
			if (expr) {
				harvest(*expr);
			}
		}
	}
	// Back-fill SELECT-only columns from the child projection's user-written names.
	// Only slots the DECIDE-clause harvest left empty are touched, so a name the user
	// referenced in the clause always wins over the raw projection alias.
	for (idx_t i = 0; i < display.input_column_names.size(); i++) {
		if (display.input_column_names[i].empty() && !child_userwritten_names[i].empty()) {
			display.input_column_names[i] = QuoteColumnName(child_userwritten_names[i]);
		}
	}
}

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalDecide &op) {
	DecideProfileScope profile("frontend.physical_decide");
	// Stage 05 carries meaning, not just speed: it chooses the backend, reduces each data bound to its group's tightest
	// value with its value checks, and folds bounds into column boxes. A plan it never saw (the optimizer switched off,
	// or PRAGMA enable_verification's unoptimized run) cannot be answered, so it is refused in SQL terms.
	if (!op.optimized) {
		throw InvalidInputException("DECIDE needs the optimizer 'decide_optimizer', which is disabled");
	}
	D_ASSERT(op.children.size() == 1);
	// Both clauses are verified after ALL optimizer rewriting and before any logical
	// expression is moved into the physical operator. The objective is checked here
	// for the same reason the constraints are: every rewrite that touches it now
	// re-enters through LogicalDecide::SetObjective, so a rewrite that breaks the
	// canonical form should fail loudly rather than reach the extractor.
	DecideCanonicalizer canonicalizer(context, op.decide_index, op.bound);
	for (auto &clause : op.bound.clauses) {
		canonicalizer.VerifyCanonical(clause);
	}
	if (op.bound.objective.body) {
		canonicalizer.VerifyCanonicalObjective(op.bound.objective);
	}
	// One walk lowers the verified clauses into the tree's nodes and, from the same expressions, each root's terms
	// (DecideTreeConstruction; the term half is stage 05's, src/optimizer/decide/decide_linear_form.cpp). It runs
	// here because this is the first point at which column bindings are final. The
	// lowered terms hold copies of the coefficient subtrees, and RemoveUnusedColumns,
	// ColumnLifetimeAnalyzer and late materialization all rebind the operator's own
	// expressions after the DECIDE optimizer runs -- copies taken earlier would keep
	// bindings that no longer name the right input columns.
	auto construction = make_uniq<DecideTreeConstruction>(context, op);
	// With the shape of every constraint and the objective settled, check the model
	// class this query needs against the backend chosen for it. A model class is a
	// gate, not an optimization -- nothing lowers a quadratic constraint into linear
	// rows -- so an unsupported one is refused here, before the query reads a row,
	// and the message blames the host rather than the query.
	RequireDecideSolverSupport(op, construction->Tree());
	// After the refusal, so a query the backend cannot answer is refused in SQL terms first.
	auto tree = construction->Finish();
	// The user's names for the child's columns, read before CreatePlan moves the child's expressions out.
	vector<string> child_userwritten_names;
	if (op.diagnose) {
		child_userwritten_names = UserWrittenColumnNames(op);
	}
	auto child_plan = CreatePlan(*op.children[0]);
	// What the tree's construction read of the LogicalDecide is read; each fact the plan states moves in, so the tree
	// is its one home.
	tree->decide_variables = std::move(op.bound.variables);
	tree->num_auxiliary_vars = op.num_auxiliary_vars;
	tree->is_boolean_var = std::move(op.bound.is_boolean_var);
	tree->objective_constant_offset = op.bound.objective_constant_offset;
	tree->signatures = std::move(op.bound.signatures);
	tree->objective_signature = op.bound.objective.scope.signature;
	tree->frames = std::move(op.bound.frames);
	tree->diagnose = op.diagnose;
	auto decide_op = make_uniq<PhysicalDecide>(op.types, op.estimated_cardinality, std::move(child_plan));
	auto &display = decide_op->display;
	display.decide_index = op.decide_index;
	display.clauses = std::move(op.bound.clauses);
	display.objective = std::move(op.bound.objective);
	display.constraint_sources = std::move(op.bound.constraint_sources);
	display.written_objective = std::move(op.bound.written_objective);
	display.canonical_objective = std::move(op.bound.canonical_objective);
	display.objective_canonical_differs = op.bound.objective_canonical_differs;
	display.source_text = std::move(op.bound.source_text);
	display.aux_var_expressions = std::move(op.aux_var_expressions);
	display.rewrite_accounts = std::move(op.rewrite_accounts);
	decide_op->debug = GetDecideDebug(context);
	decide_op->solver_backend_name = op.solver_backend_name;
	decide_op->use_native_constructs = op.use_native_constructs;
	decide_op->force_native_constructs = op.force_native_constructs;
	if (op.diagnose) {
		NameInputColumns(display, *tree, op, child_userwritten_names);
	}
	decide_op->tree = std::move(tree);
	// The refusal of a bound the decision's type cannot hold reads the plan alone, so it is raised here.
	decide_op->CheckDecisionDomains(op.member_singleton_row_bounds);
	return std::move(decide_op);
}

} // namespace duckdb
