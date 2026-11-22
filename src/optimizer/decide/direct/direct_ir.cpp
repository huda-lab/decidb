#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

#include <algorithm>

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

namespace {

//! Whether any term holds algebra no rule reads. A part's own terms may be an ABS; what an ABS or a factor holds is
//! linear algebra only, so an ABS or a product there is unknown.
bool HasUnknownTerm(const vector<DecideSplitTerm> &terms, bool nested = false) {
	for (auto &term : terms) {
		bool readable = nested ? !term.opaque && term.degree <= 1 : !term.opaque || term.IsAbs();
		if (!readable || HasUnknownTerm(term.inner, true)) {
			return true;
		}
		for (auto &factor : term.factors) {
			if (HasUnknownTerm(factor.form, true)) {
				return true;
			}
		}
	}
	return false;
}

void MarkNotProved(DirectAnalysisStatus &status) {
	status = DirectAnalysisStatus::NOT_PROVED;
}

//! Reads one DECIDE node into the IR. It is the only direct-solve code that knows LogicalDecide's layout and the
//! bound-tree spelling of DECIDE constructs.
class DirectFrontEnd {
public:
	DirectFrontEnd(ClientContext &context_p, LogicalDecide &decide_p)
	    : context(context_p), decide(decide_p), splitter(context_p, decide_p),
	      canonicalizer(context_p, decide_p.decide_index, decide_p.bound.variable_scopes, decide_p.bound.keyed_scopes,
	                    decide_p.bound.keys, decide_p.bound.is_boolean_var) {
	}

	DirectIR Read() {
		// The IR describes what the user wrote. After OptimizeDecide the tree holds a solver formulation instead.
		D_ASSERT(!decide.optimized && !decide.num_auxiliary_vars);
		DirectIR ir;
		ir.render = [&decide = decide](const Expression &expr) {
			return RenderDecideSource(expr, decide.bound.source_text, decide.bound.keyed_scopes, &decide.bound.keys);
		};
		ir.place = [&decide = decide](const Expression &expr) {
			// What a NULL guard protects stands in the query, as the render quotes it.
			return DecideSpanLocation(decide.bound.source_text, DecideUnguarded(expr).GetQueryLocation());
		};
		ir.decide_index = decide.decide_index;
		D_ASSERT(decide.children.size() == 1 && decide.children[0]);
		source_bindings = decide.SourceBindings();
		ir.source_bindings = source_bindings;
		ReadCrossProduct(ir);
		ReadTableFacts(*decide.children[0], ir);
		// A frame selector reads the row its position selects on a timeline (language/guide.md §5.6), which the
		// operator computes and no rule's plan reads.
		if (!decide.bound.frames.empty()) {
			MarkNotProved(ir.status);
		}
		ReadDecisions(ir);
		ReadKeyedScopes(ir);
		ReadConstraints(ir);
		ReadObjective(ir.objective);
		ReadRowChecks(ir);
		return ir;
	}

private:
	ClientContext &context;
	LogicalDecide &decide;
	DecideTermSplitter splitter;
	DecideCanonicalizer canonicalizer;
	vector<ColumnBinding> source_bindings;

	//! DirectIR::cross_product: only DECIDE's own child counts. A projection, filter or join between them leaves it
	//! empty.
	void ReadCrossProduct(DirectIR &ir) const {
		auto &source = *decide.children[0];
		if (source.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
			ir.cross_product.left = source.children[0]->GetColumnBindings();
			ir.cross_product.right = source.children[1]->GetColumnBindings();
		}
	}

	//! DirectIR::not_null and DirectIR::keys: what the tables the source scans directly declare. The walk goes down
	//! through filters and cross products, which pass their children's bindings and values through; any other operator
	//! (a projection, a join) ends it, so a binding it makes is never recorded.
	void ReadTableFacts(const LogicalOperator &op, DirectIR &ir) const {
		if (op.type == LogicalOperatorType::LOGICAL_FILTER || op.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
			for (auto &child : op.children) {
				ReadTableFacts(*child, ir);
			}
		} else if (op.type == LogicalOperatorType::LOGICAL_GET && op.Cast<LogicalGet>().GetTable()) {
			auto &get = op.Cast<LogicalGet>();
			auto &table = *get.GetTable();
			auto &column_ids = get.GetColumnIds();
			auto first_not_null = ir.not_null.size();
			for (auto &constraint : table.GetConstraints()) {
				if (constraint->type != ConstraintType::NOT_NULL) {
					continue;
				}
				auto declared = constraint->Cast<NotNullConstraint>().index.index;
				for (idx_t i = 0; i < column_ids.size(); i++) {
					if (column_ids[i].GetPrimaryIndex() == declared) {
						ir.not_null.emplace_back(get.table_index, i);
					}
				}
			}
			// A UNIQUE constraint, the PRIMARY KEY included, is a key when the scan reads all its columns and the
			// table declares them all NOT NULL: UNIQUE lets NULLs repeat. A NOT NULL binding is a column the scan
			// reads.
			auto declared_and_read = [&](idx_t column, ColumnBinding &binding) {
				for (auto at = ir.not_null.begin() + first_not_null; at != ir.not_null.end(); ++at) {
					if (column_ids[at->column_index].GetPrimaryIndex() == column) {
						binding = *at;
						return true;
					}
				}
				return false;
			};
			for (auto &constraint : table.GetConstraints()) {
				if (constraint->type != ConstraintType::UNIQUE) {
					continue;
				}
				auto columns = constraint->Cast<UniqueConstraint>().GetLogicalIndexes(table.GetColumns());
				vector<ColumnBinding> key(columns.size());
				bool is_key = true;
				for (idx_t i = 0; i < columns.size(); i++) {
					is_key = is_key && declared_and_read(columns[i].index, key[i]);
				}
				if (is_key) {
					ir.keys.push_back(std::move(key));
				}
			}
		}
	}

	//! A predicate the rule's plan can evaluate: it reads only source columns.
	bool CheckablePredicate(const Expression *predicate) const {
		return !predicate || DirectReferencesOnlySource(*predicate, source_bindings);
	}

	//! The value checks as the plan makes them. Every check reads the source and the bounds' columns, so the plan
	//! makes each one.
	void ReadRowChecks(DirectIR &ir) const {
		for (auto &check : decide.value_checks) {
			D_ASSERT(CheckablePredicate(check.get()));
			ir.row_checks.push_back(check->Copy());
		}
	}

private:
	void ReadDecisions(DirectIR &ir) const {
		auto count = decide.bound.variables.size();
		D_ASSERT(decide.is_boolean_var.size() == count && decide.variable_scopes.size() == count);
		for (idx_t i = 0; i < count; i++) {
			auto &variable = decide.bound.variables[i];
			DirectIRDecision decision;
			decision.name = variable->GetName();
			decision.declaration = decide.bound.variable_scopes[i].declaration;
			decision.output_type = variable->return_type;
			decision.scope = decide.bound.variable_scopes[i].scope;
			if (decision.scope == DecideVarScope::KEYED) {
				decision.keyed_scope = decide.bound.variable_scopes[i].keyed_scope_idx;
			}
			if (decide.bound.is_boolean_var[i]) {
				decision.domain = DirectDomain::BOOL;
			} else if (variable->return_type == LogicalType::DOUBLE) {
				decision.domain = DirectDomain::REAL;
			} else {
				D_ASSERT(variable->return_type.IsIntegral());
				decision.domain = DirectDomain::INT;
			}
			ir.decisions.push_back(std::move(decision));
		}
	}

	void ReadKeyedScopes(DirectIR &ir) const {
		for (auto &scope : decide.bound.keyed_scopes) {
			DirectIRKeyedScope ir_node;
			// The IR is read before the value rewrite makes its helper scopes; a binder scope's key is columns.
			D_ASSERT(scope.value_link == DConstants::INVALID_INDEX);
			if (scope.signature != DConstants::INVALID_INDEX) {
				// A reducer's own `per` over a body with variants (D71): its instances are the key's classes split
				// by the body's values, which the operator numbers and no rule's plan reads.
				MarkNotProved(ir.status);
			}
			// A binder scope's key is columns of the source (the binder keeps every keyed decision's key live in it),
			// which a row check can evaluate, so reading it cannot decline. The empty key, `per ()`, has no columns.
			if (scope.key != DConstants::INVALID_INDEX) {
				bool read = ReadKey(scope.key, ir_node.key);
				D_ASSERT(read);
				(void)read;
			}
			ir.keyed_scopes.push_back(std::move(ir_node));
		}
	}

	//===--------------------------------------------------------------------===//
	// Parts: one additive term of a left side or the objective
	//===--------------------------------------------------------------------===//

	//! `filter` without the tests DecideCanonicalizer::SkipNullData added, which move to `tests`; null when nothing
	//! else is left.
	static unique_ptr<Expression> TakeSkipNullTests(unique_ptr<Expression> filter,
	                                                vector<unique_ptr<Expression>> &tests) {
		if (HasDecideTag(filter->GetAlias(), SKIP_NULL_TAG)) {
			tests.push_back(std::move(filter));
			return nullptr;
		}
		if (filter->GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
			return filter;
		}
		auto &conjunction = filter->Cast<BoundConjunctionExpression>();
		vector<unique_ptr<Expression>> kept;
		for (auto &child : conjunction.children) {
			auto rest = TakeSkipNullTests(std::move(child), tests);
			if (rest) {
				kept.push_back(std::move(rest));
			}
		}
		if (kept.empty()) {
			return nullptr;
		}
		if (kept.size() == 1) {
			return std::move(kept[0]);
		}
		conjunction.children = std::move(kept);
		return filter;
	}

	//! Each coefficient of `term` read as 0 on a row `valid` fails: a SUM that skips a row adds nothing for it. A term
	//! of degree 2 is guarded in the forms it multiplies and in its own coefficient (a square's weight), which a rule
	//! that needs a constant reads through (DecideUnguarded).
	void GuardCoefficients(const ExpressionBuilder &calc, const ExprPtr &valid, DecideSplitTerm &term) const {
		auto zero = [&](const LogicalType &type) {
			return calc.Constant(Value::INTEGER(0).DefaultCastAs(type));
		};
		// The guard is the engine's: a render prints the coefficient it guards (NULL_GUARD_TAG), which keeps the tags
		// that tell how the user wrote it (the plan loses them in StripTagsAbove).
		auto guard = [&](const Expression &coefficient) {
			auto guarded = calc.Out(calc.If(valid, calc.Wrap(coefficient.Copy()), zero(coefficient.return_type)));
			guarded->SetAlias(NULL_GUARD_TAG);
			return guarded;
		};
		for (auto &factor : term.factors) {
			for (auto &part : factor.form) {
				GuardCoefficients(calc, valid, part);
			}
		}
		if (term.coefficient) {
			term.coefficient = guard(*term.coefficient);
		} else if (term.factors.empty()) {
			// A term with no coefficient is 1 where the row is read, guarded like one that has a coefficient.
			term.coefficient = guard(*calc.Num(1));
		}
		// An ABS's argument is guarded term by term as a part's terms are: a term of degree 2 there has no coefficient
		// of its own, only the forms it multiplies, and HasUnknownTerm declines it afterwards.
		for (auto &inner : term.inner) {
			GuardCoefficients(calc, valid, inner);
		}
	}

	//! The key a reducer groups by, as the plan reads it; false when it reads more than the source.
	bool ReadKey(idx_t key, vector<DirectKeyElement> &elements) const {
		D_ASSERT(key < decide.keys.size());
		for (auto &element : DecideKeyElements(decide.bound.keys, key)) {
			if (!CheckablePredicate(&element.Grouping()) || !CheckablePredicate(element.expression.get())) {
				return false;
			}
			DirectKeyElement read;
			read.grouping = element.Grouping().Copy();
			read.label = element.expression->Copy();
			read.name = element.name;
			read.no_group = element.null_mode == DecideKeyNullMode::NO_GROUP;
			elements.push_back(std::move(read));
		}
		return true;
	}

	void ReadReducer(const BoundAggregateExpression &aggregate, DirectPart &part) const {
		idx_t frame;
		if (TryGetDecideFrame(aggregate, frame)) {
			MarkNotProved(part.status); // a frame's marker, no reducer
			return;
		}
		D_ASSERT(!aggregate.IsDistinct() && !aggregate.order_bys && aggregate.children.size() == 1);
		string order;
		auto name = StringUtil::Lower(aggregate.function.name);
		if (ExtractDecideTagPayload(aggregate.GetAlias(), NORM_MARKER_TAG_PREFIX, order)) {
			// Canonicalization replaced every order but 0 with its definition. Order 0 needs indicator variables, so no
			// rule models it, and any other tag is not one this reader knows: both decline instead of throwing.
			MarkNotProved(part.status);
			return;
		} else if (name == "sum") {
			part.reducer = DirectReducer::SUM;
		} else if (name == "avg") {
			part.reducer = DirectReducer::AVG;
		} else if (name == "min") {
			part.reducer = DirectReducer::MIN;
		} else {
			// The binder admits no other reducer in a DECIDE clause.
			D_ASSERT(name == "max");
			part.reducer = DirectReducer::MAX;
		}
		idx_t group_key;
		if (TryParseGroupKeyTag(aggregate.GetAlias(), group_key) && !ReadKey(group_key, part.by)) {
			MarkNotProved(part.status);
			return;
		}
		if (aggregate.filter) {
			part.filter = aggregate.filter->Copy();
		}
		TryParseQualifiedReducerTag(aggregate.GetAlias(), part.qualifier);
		splitter.Split(*aggregate.children[0], 1, part.terms);
		// A SUM skips a row whose data is NULL by reading its terms as 0 there, which every rule reads as it reads any
		// coefficient; the written `when` stays the reducer's.
		if (part.filter && part.reducer == DirectReducer::SUM) {
			vector<unique_ptr<Expression>> tests;
			part.filter = TakeSkipNullTests(std::move(part.filter), tests);
			if (!tests.empty()) {
				ExpressionBuilder calc(context);
				ExprPtr valid = calc.Wrap(std::move(tests[0]));
				for (idx_t i = 1; i < tests.size(); i++) {
					valid = calc.Wrap(make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
					                                                        calc.Out(valid), std::move(tests[i])));
				}
				for (auto &term : part.terms) {
					GuardCoefficients(calc, valid, term);
				}
			}
		}
	}

	DirectPart ReadPart(const CanonicalAtom &atom) const {
		DirectPart part;
		part.sign = atom.sign;
		const Expression *term = atom.term;
		if (atom.scaled) {
			part.scale = atom.scale.scale->Copy();
			part.scale_divides = atom.scale.divides;
			term = atom.scale.aggregate;
		}
		auto root = UnwrapDecideCasts(*term, decide.decide_index);
		if (root->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			ReadReducer(root->Cast<BoundAggregateExpression>(), part);
		} else {
			splitter.Split(*root, 1, part.terms);
		}
		if (HasUnknownTerm(part.terms)) {
			MarkNotProved(part.status);
		}
		return part;
	}

	void ReadParts(const Expression &side, vector<DirectPart> &parts) const {
		for (auto &atom : ReadCanonicalAtoms(side, decide.decide_index)) {
			parts.push_back(ReadPart(atom));
		}
	}

	//! One row-level part holding every term of a per-row side.
	DirectPart ReadRowPart(const Expression &side) const {
		DirectPart part;
		splitter.Split(side, 1, part.terms);
		if (HasUnknownTerm(part.terms)) {
			MarkNotProved(part.status);
		}
		return part;
	}

	void ReadScope(const Expression *when, DirectIRScope &scope) const {
		if (when) {
			scope.when = when->Copy();
		}
	}

	//===--------------------------------------------------------------------===//
	// Constraints
	//===--------------------------------------------------------------------===//

	DirectIRConstraint ReadConstraint(const Expression &leaf, const DecideEnclosingScope &wrappers) const {
		DirectIRConstraint ir_node;
		idx_t source_id;
		if (TryParseSourceClauseTag(leaf.GetAlias(), source_id)) {
			D_ASSERT(source_id < decide.constraint_sources.size());
			ir_node.written = DecideClauseRefOf(decide.bound.constraint_sources, decide.bound.source_text, source_id);
		} else {
			MarkNotProved(ir_node.status);
		}
		ReadScope(wrappers.Condition(), ir_node.scope);
		if (HasDecideTag(leaf.GetAlias(), VALUE_REWRITE_TAG)) {
			// No rule models the helper decision a reducer's value becomes.
			MarkNotProved(ir_node.status);
		}

		// The binder admits only comparisons and IN as a constraint leaf.
		if (leaf.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
			D_ASSERT(leaf.type == ExpressionType::COMPARE_IN);
			auto &membership = leaf.Cast<BoundOperatorExpression>();
			ir_node.comparison = ExpressionType::COMPARE_IN;
			ir_node.lhs.push_back(ReadRowPart(*membership.children[0]));
		} else {
			D_ASSERT(leaf.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON);
			auto &comparison = leaf.Cast<BoundComparisonExpression>();
			ir_node.comparison = comparison.type;
			auto shape = canonicalizer.ClassifyCanonicalComparison(comparison);
			// The canonicalizer rejected an invalid comparison when it bound the query.
			D_ASSERT(shape != CanonicalConstraintClass::INVALID);
			ir_node.aggregate = shape == CanonicalConstraintClass::AGGREGATE;
			if (ir_node.aggregate) {
				ReadParts(*comparison.left, ir_node.lhs);
			} else {
				ir_node.lhs.push_back(ReadRowPart(*comparison.left));
			}
			ir_node.rhs = comparison.right->Copy();
			// Direct rules run before the MIN/MAX rewrite tags an easy extremum, so only a per-row bound reads here.
			ir_node.singleton_row = DecideReadSingletonRow(context, decide, comparison, ir_node.bound);
			// Folding never reads a clause under a `when`: `when c: x <= 0` does not bound `x` everywhere.
			ir_node.variable_bound = ir_node.singleton_row && ir_node.bound.folds && !wrappers.when_wrapper;
		}
		for (auto &part : ir_node.lhs) {
			if (part.status == DirectAnalysisStatus::NOT_PROVED) {
				MarkNotProved(ir_node.status);
			}
		}
		return ir_node;
	}

	void ReadConstraints(DirectIR &ir) const {
		// The parser requires a SUCH THAT clause.
		D_ASSERT(decide.decide_constraints);
		ForEachScopedConstraintLeaf(*decide.bound.constraints,
		                            [&](const Expression &leaf, const DecideEnclosingScope &scope) {
			                            auto constraint = ReadConstraint(leaf, scope);
			                            if (scope.per_wrapper) {
				                            ReadPerClass(scope.PerKey(), constraint);
			                            }
			                            ir.constraints.push_back(std::move(constraint));
		                            });
	}

	//! `per K: C` is one constraint per class of K and variant of C (D6, D70). An aggregate clause bounds the whole
	//! total of each `by` group, and every instance of a group differs only in its bound, which the group's tightest
	//! bound covers, as for a clause with no `per`. A clause per row would hold in a variant alone, which no rule
	//! models.
	void ReadPerClass(idx_t key, DirectIRConstraint &constraint) const {
		constraint.per_class = true;
		if (!constraint.aggregate || (key != DConstants::INVALID_INDEX && !ReadKey(key, constraint.classes))) {
			MarkNotProved(constraint.status);
		}
	}

	//===--------------------------------------------------------------------===//
	// Objective
	//===--------------------------------------------------------------------===//

	//! Whether `expr` holds a reducer the canonicalizer tagged for the value rewrite: one holding a reducer (D81).
	static bool HoldsValueRewrite(const Expression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE &&
		    HasDecideTag(expr.GetAlias(), VALUE_REWRITE_TAG)) {
			return true;
		}
		bool holds = false;
		ExpressionIterator::EnumerateChildren(
		    expr, [&](const Expression &child) { holds = holds || HoldsValueRewrite(child); });
		return holds;
	}

	void ReadObjective(DirectIRObjective &objective) const {
		objective.offset = decide.bound.objective_constant_offset;
		objective.written =
		    DecideClauseRefOf(decide.bound.constraint_sources, decide.bound.source_text, DConstants::INVALID_INDEX);
		if (!decide.bound.objective) {
			return;
		}
		auto index = decide.decide_index;
		const Expression *body = UnwrapDecideCasts(*decide.bound.objective, index);
		if (HoldsValueRewrite(*body)) {
			// No rule models the helper decision a nested reducer becomes (D81).
			MarkNotProved(objective.status);
			return;
		}
		if (decide.bound.objective_signature != DConstants::INVALID_INDEX) {
			// The objective is produced once (D72): the operator refuses a second variant of it, which no rule's plan
			// checks.
			MarkNotProved(objective.status);
			return;
		}
		const Expression *when = nullptr;
		if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &wrapper = body->Cast<BoundConjunctionExpression>();
			D_ASSERT(IsWhenFilterWrapper(wrapper) && wrapper.children.size() == 2);
			when = wrapper.children[1].get();
			body = UnwrapDecideCasts(*wrapper.children[0], index);
		}
		ReadScope(when, objective.scope);
		D_ASSERT(body->GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION);
		ReadParts(*body, objective.parts);
		for (auto &part : objective.parts) {
			if (part.status == DirectAnalysisStatus::NOT_PROVED) {
				MarkNotProved(objective.status);
			}
		}
	}
};

} // namespace

DirectIR DirectIR::Read(ClientContext &context, LogicalDecide &decide) {
	return DirectFrontEnd(context, decide).Read();
}

const DirectPart *DirectIRConstraint::PlainSum() const {
	if (!aggregate || lhs.size() != 1) {
		return nullptr;
	}
	auto &part = lhs[0];
	if (part.sign != 1 || part.scale || part.reducer != DirectReducer::SUM ||
	    part.qualifier != DConstants::INVALID_INDEX) {
		return nullptr;
	}
	return &part;
}

unique_ptr<Expression> DirectIRConstraint::Membership(bool &ok) const {
	auto sum = PlainSum();
	ok = sum != nullptr;
	if (!ok || (!sum->filter && !scope.when)) {
		return nullptr;
	}
	if (!sum->filter || !scope.when) {
		return (sum->filter ? sum->filter : scope.when)->Copy();
	}
	// A row is in the sum when both keep it. The solver reads the sum's own `when` only on the rows the clause's `when`
	// keeps; a rule reads only a membership that cannot raise (DirectIsSourceOnlyPredicate), and over those the
	// conjunction keeps the same rows.
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, scope.when->Copy(),
	                                             sum->filter->Copy());
}

} // namespace duckdb
