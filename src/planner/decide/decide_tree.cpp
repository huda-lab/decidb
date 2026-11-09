#include "duckdb/planner/decide/decide_tree.hpp"

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <sstream>

namespace duckdb {

string DecideTreeReducerName(const BoundAggregateExpression &aggregate) {
	string payload;
	if (ExtractDecideTagPayload(aggregate.GetAlias(), MINMAX_CLAUSE_TAG_PREFIX, payload)) {
		auto separator = payload.find('_');
		if (separator != string::npos) {
			return payload.substr(separator + 1);
		}
	}
	return HasDecideTag(aggregate.GetAlias(), AVG_REWRITE_TAG) ? "avg" : StringUtil::Lower(aggregate.function.name);
}

unique_ptr<DecideConstraint> ReadDecideClauseFacts(ClientContext &context, const LogicalDecide &op,
                                                   const BoundComparisonExpression &comparison,
                                                   const DecideEnclosingScope &scope) {
	auto facts = make_uniq<DecideConstraint>();
	facts->comparison_type = comparison.type;
	facts->rhs_expr = comparison.right->Copy();
	facts->tightest_frame_bound = HasDecideTag(comparison.alias, FRAME_TIGHTEST_BOUND_TAG);
	TryParseSourceClauseTag(comparison.alias, facts->source_clause_id);
	TryParseRemovalGroupTag(comparison.alias, facts->removal_group_id);
	string payload;
	if (ExtractDecideTagPayload(comparison.alias, NE_CLAUSE_TAG_PREFIX, payload)) {
		facts->ne_clause_idx = std::stoull(payload);
		facts->kind = ConstraintKind::USER_MECHANISM;
	}
	if (HasDecideTag(comparison.alias, STRUCTURAL_CONSTRAINT_TAG)) {
		facts->kind = ConstraintKind::STRUCTURAL;
	}
	TryParseDecideIndexTag(comparison.alias, DEFINES_TAG_PREFIX, facts->defined_var);
	if (ExtractDecideTagPayload(comparison.alias, ABS_UB_POS_TAG_PREFIX, payload)) {
		facts->abs_aux_idx = std::stoull(payload);
		facts->abs_is_pos_bound = true;
		facts->kind = ConstraintKind::STRUCTURAL;
	} else if (ExtractDecideTagPayload(comparison.alias, ABS_UB_NEG_TAG_PREFIX, payload)) {
		facts->abs_aux_idx = std::stoull(payload);
		facts->kind = ConstraintKind::STRUCTURAL;
	}
	facts->was_minmax_easy = HasDecideTag(comparison.alias, MINMAX_EASY_REWRITE_TAG);
	facts->scope = DecideClauseScope(scope);
	TryParseGroupKeyTag(comparison.alias, facts->group_key);
	DecideCanonicalizer canonicalizer(context, op.decide_index, op.variable_scopes, op.keyed_scopes, op.keys,
	                                  op.is_boolean_var);
	auto shape = canonicalizer.ClassifyCanonicalComparison(comparison);
	if (shape == CanonicalConstraintClass::INVALID) {
		throw InternalException("DECIDE constraint reached term extraction with invalid aggregate/per-row "
		                        "homogeneity: '%s'. Canonical validation must reject this during planning.",
		                        comparison.ToString());
	}
	if (shape == CanonicalConstraintClass::VALUE) {
		throw InternalException("DECIDE constraint in the VALUE shape reached term extraction: the value "
		                        "rewrite did not replace its reducers in '%s'.",
		                        comparison.ToString());
	}
	if (facts->scope.per != DecideClauseScope::PerRole::NONE && !facts->was_minmax_easy &&
	    shape != CanonicalConstraintClass::AGGREGATE) {
		facts->scope.per = DecideClauseScope::PerRole::ONE_ROW_PER_CLASS;
	}
	facts->lhs_is_aggregate = !facts->was_minmax_easy && shape == CanonicalConstraintClass::AGGREGATE;
	for (auto &atom : ReadCanonicalAtoms(*comparison.left, op.decide_index)) {
		auto expression = atom.scaled ? static_cast<const Expression *>(atom.scale.aggregate) : atom.term;
		expression = UnwrapDecideCasts(*expression, op.decide_index);
		if (expression->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE &&
		    ExtractDecideTagPayload(expression->GetAlias(), MINMAX_CLAUSE_TAG_PREFIX, payload)) {
			auto separator = payload.find('_');
			if (separator == string::npos) {
				throw InternalException("DECIDE extremum carrier has no semantic reducer name");
			}
			facts->minmax_clause_idx = std::stoull(payload.substr(0, separator));
			facts->minmax_agg_type = payload.substr(separator + 1);
			facts->kind = ConstraintKind::USER_MECHANISM;
		}
	}
	const bool folded = HasDecideTag(comparison.alias, SINGLETON_ROW_BOUND_TAG);
	DecideSingletonRow bound;
	if (!folded && !facts->lhs_is_aggregate && DecideReadSingletonRow(context, op, comparison, bound)) {
		facts->admitted_lower = std::move(bound.lower);
		facts->admitted_upper = std::move(bound.upper);
		facts->admitted_a = std::move(bound.a);
		facts->admitted_r = std::move(bound.r);
		facts->admitted_member = std::move(bound.member);
		if (bound.LiftsDefaultFloor()) {
			facts->lifted_floor = bound.var_idx;
			facts->lifted_floor_k = bound.written_k;
		}
	}
	return facts;
}

//! The construction's helpers, in a namespace of their own rather than an anonymous one: the header's
//! DecideTreeConstruction::Builder derives from DecideTreeBuilder.
namespace decide_tree_build {

static unique_ptr<Expression> CopyOrNull(const unique_ptr<Expression> &expression) {
	return expression ? expression->Copy() : nullptr;
}

static void AddIndex(vector<idx_t> &indexes, idx_t index) {
	if (std::find(indexes.begin(), indexes.end(), index) == indexes.end()) {
		indexes.push_back(index);
	}
}

static DecideSignatureField CopyField(const DecideSignatureField &field) {
	DecideSignatureField result;
	result.kind = field.kind;
	result.value = CopyOrNull(field.value);
	result.decision = field.decision;
	result.key = field.key;
	result.frame = field.frame;
	result.text = field.text;
	return result;
}

static unique_ptr<DecideFrameSpec> CopyFrame(const DecideFrameSpec &frame) {
	auto result = make_uniq<DecideFrameSpec>();
	result->range = frame.range;
	result->reducer = frame.reducer;
	result->from = frame.from;
	result->from_k = frame.from_k;
	result->to = frame.to;
	result->to_k = frame.to_k;
	result->every = frame.every;
	result->all = frame.all;
	result->fallback = CopyOrNull(frame.fallback);
	result->order = CopyOrNull(frame.order);
	result->order_collation = CopyOrNull(frame.order_collation);
	result->descending = frame.descending;
	result->cyclic = frame.cyclic;
	result->within_key = frame.within_key;
	result->text = frame.text;
	result->rows = CopyOrNull(frame.rows);
	result->timeline_key = frame.timeline_key;
	result->timeline_names = frame.timeline_names;
	result->order_name = frame.order_name;
	result->partition_key = frame.partition_key;
	result->partition_subject = frame.partition_subject;
	return result;
}

class DecideTreeBuilder {
public:
	DecideTreeBuilder(ClientContext &context, const LogicalDecide &op) : context(context), op(op) {
		tree = make_uniq<DecideTree>();
		tree->decide_index = op.decide_index;
		tree->source_fragments = op.source_fragments;
		tree->variable_scopes = op.variable_scopes;
		tree->keyed_scopes = op.keyed_scopes;
		tree->value_nodes.assign(op.decide_variables.size(), DConstants::INVALID_INDEX);
		tree->definition_nodes.resize(op.decide_variables.size());
		tree->bilinear_links = op.bilinear_links;
		tree->abs_maximize_links = op.abs_maximize_links;
		tree->singleton_row_boxes = op.singleton_row_boxes;
		tree->user_singleton_row_bounds = op.user_singleton_row_bounds;
		tree->signed_helper_vars = op.signed_helper_vars;
		if (!op.children.empty()) {
			bindings = op.children[0]->GetColumnBindings();
		}
		tree->input_column_count = bindings.size();
		for (auto &key : op.keys) {
			DecideKey copy;
			copy.text = key.text;
			copy.written_as_by = key.written_as_by;
			for (auto &element : key.elements) {
				auto copied = element.Copy();
				copied.expression = Resolve(*element.expression);
				if (element.collated) {
					copied.collated = Resolve(*element.collated);
				}
				copy.elements.push_back(std::move(copied));
			}
			tree->keys.push_back(std::move(copy));
		}
		link_nodes.assign(op.reducer_value_links.size(), DConstants::INVALID_INDEX);
		frame_nodes.assign(op.frames.size(), DConstants::INVALID_INDEX);
		record_nodes.assign(op.retained_bound_aggregates.size(), DConstants::INVALID_INDEX);
	}

	//! The nodes, their edges, sigma, gates and definitions, before stage 05 lowers the clauses.
	void Structure() {
		for (auto &check : op.retained_row_checks) {
			auto index = NewNode(DecideNodeKind::SCOPE);
			auto &node = *tree->nodes[index];
			node.root = node.hidden = node.value_check = true;
			node.text = "data value check";
			node.body = Lower(index, *check);
			tree->roots.push_back(index);
		}
		if (op.decide_constraints) {
			ForEachScopedConstraintLeaf(*op.decide_constraints,
			                            [&](const Expression &expression, const DecideEnclosingScope &scope) {
				                            if (expression.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
					                            Root(expression.Cast<BoundComparisonExpression>(), scope);
				                            }
			                            });
		}
		idx_t objective_root = DConstants::INVALID_INDEX;
		if (op.decide_objective) {
			const Expression *body = op.decide_objective.get();
			const Expression *condition = nullptr;
			if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
				auto &wrapper = body->Cast<BoundConjunctionExpression>();
				if (IsWhenFilterWrapper(wrapper) && wrapper.children.size() == 2) {
					body = wrapper.children[0].get();
					condition = wrapper.children[1].get();
				}
			}
			if (!condition && op.retained_objective_when < op.retained_expressions.size()) {
				condition = op.retained_expressions[op.retained_objective_when].get();
			}
			auto index = NewNode(DecideNodeKind::SCOPE);
			objective_root = sources.objective_root = index;
			auto &node = *tree->nodes[index];
			node.root = node.objective = true;
			node.per = DecideTreePer::ONE;
			node.text = op.written_objective.empty() ? Text(*body) : op.written_objective;
			Gates(index, condition);
			node.body = Lower(index, *body);
			if (op.flat_objective_agg == ObjectiveAggregateType::MIN_AGG ||
			    op.flat_objective_agg == ObjectiveAggregateType::MAX_AGG) {
				for (auto child : node.children) {
					if (tree->nodes[child]->kind == DecideNodeKind::REDUCER) {
						tree->nodes[child]->aggregate =
						    op.flat_objective_agg == ObjectiveAggregateType::MIN_AGG ? "min" : "max";
						break;
					}
				}
			}
			UseSignature(node, op.objective_signature);
			tree->roots.push_back(index);
		}
		// Composed comparisons have already left the final AST. Their records are
		// semantic roots, not prepared coefficients; reconstruct their G/E operands.
		for (auto &composed : op.composed_minmax_constraints) {
			auto index = NewNode(DecideNodeKind::SCOPE);
			auto &node = *tree->nodes[index];
			node.root = true;
			node.comparison_type = composed.outer_cmp;
			node.facts = make_uniq<DecideConstraint>();
			node.facts->comparison_type = composed.outer_cmp;
			node.facts->kind = ConstraintKind::USER_MECHANISM;
			node.facts->source_clause_id = composed.source_clause_id;
			node.facts->removal_group_id = composed.removal_group_id;
			node.source_clause_id = composed.source_clause_id;
			node.removal_group_id = composed.removal_group_id;
			node.text = RootText(composed.source_clause_id);
			Scope(index, composed.scope);
			node.facts->scope.when = composed.scope.when ? composed.scope.when->Copy() : nullptr;
			node.facts->scope.per = composed.scope.per;
			node.facts->scope.class_key = composed.scope.class_key;
			node.facts->scope.signature = composed.scope.signature;
			node.facts->lhs_is_aggregate = true;
			if (composed.rhs_expr) {
				node.rhs = Lower(index, *composed.rhs_expr);
				node.facts->rhs_expr = node.rhs->Copy();
			}
			sources.composed_roots.push_back(index);
			sources.composed_reducers.emplace_back();
			for (auto &term : composed.terms) {
				Composed(index, term, sources.composed_reducers.back());
			}
			GroupClass(node);
			tree->roots.push_back(index);
		}
		if (!op.composed_minmax_objective_terms.empty()) {
			auto index = objective_root;
			if (index == DConstants::INVALID_INDEX) {
				index = NewNode(DecideNodeKind::SCOPE);
				tree->roots.push_back(index);
			}
			auto &node = *tree->nodes[index];
			node.root = node.objective = true;
			node.per = DecideTreePer::ONE;
			node.text = op.written_objective.empty() ? "composed objective" : op.written_objective;
			sources.composed_reducers.emplace_back();
			for (auto &term : op.composed_minmax_objective_terms) {
				Composed(index, term, sources.composed_reducers.back());
			}
			UseSignature(node, op.objective_signature);
		}
		// A helper or data check can outlive the reading clause (D88). Preserve
		// that definition as an explicit root rather than relying on registry order.
		for (idx_t i = 0; i < op.reducer_value_links.size(); i++) {
			if (link_nodes[i] == DConstants::INVALID_INDEX) {
				AddOrphan(Link(i));
			}
		}
		for (idx_t i = 0; i < op.retained_bound_aggregates.size(); i++) {
			if (record_nodes[i] == DConstants::INVALID_INDEX) {
				AddOrphan(Record(i));
			}
		}
		RemoveInheritedPathGates();
		Definitions();
	}

	//! The bodies from what stage 05 lowered, then the order and ranks they read.
	unique_ptr<DecideTree> Finish() {
		sources.link_nodes = link_nodes;
		CompileDecideTree(context, op, *tree, sources);
		PostOrder();
		ErrorRanks();
		tree->Validate();
		return std::move(tree);
	}

	//! The G a reducer stage 05 lowers ranges over (DecideTreeConstruction::ReducerChild).
	idx_t ReducerChild(const BoundAggregateExpression &reducer) const {
		static const bool inject = [] {
			const char *setting = std::getenv("DECIDB_EVALUATOR_INJECT");
			return setting && string(setting) == "g1";
		}();
		auto found = inject ? sources.reducer_nodes.end() : sources.reducer_nodes.find(&reducer);
		if (found == sources.reducer_nodes.end()) {
			throw InternalException("DECIDE tree: stage 05 lowers a reducer the tree has no node for: %s",
			                        reducer.ToString());
		}
		return DecideTreeMembersOf(*tree, found->second);
	}

private:
	ClientContext &context;
	const LogicalDecide &op;
	unique_ptr<DecideTree> tree;
	vector<ColumnBinding> bindings;
	vector<idx_t> link_nodes;
	vector<idx_t> frame_nodes;
	vector<idx_t> record_nodes;
	DecideTreeSources sources;

	//! A node's errors rank at the earliest phase that reads it, in flat's order of phases: a bound's reducer is read
	//! after the data checks on the rows, a composed MIN/MAX clause after the objective, and the objective's composed
	//! terms after the composed clauses.
	void ErrorRanks() {
		vector<bool> assigned(tree->nodes.size(), false);
		std::function<void(idx_t, DecideErrorRank)> visit = [&](idx_t index, DecideErrorRank rank) {
			auto &node = *tree->nodes[index];
			if (node.value_check) {
				rank = {DecideErrorPhase::SOURCE_CHECK, 0, rank.clause, 0};
			} else if (std::find(record_nodes.begin(), record_nodes.end(), index) != record_nodes.end()) {
				rank = {DecideErrorPhase::SOURCE_CHECK, 1, rank.clause, 0};
			} else if (node.kind == DecideNodeKind::FRAME) {
				rank = {DecideErrorPhase::FRAMES, 1, node.frame, 0};
			} else if (node.defined_var != DConstants::INVALID_INDEX) {
				rank = {DecideErrorPhase::KEYED_MAPPING, 0, rank.clause, 0};
			} else if (node.link != DConstants::INVALID_INDEX) {
				rank = {DecideErrorPhase::VALUE_LINKS, 0, node.link, 0};
			} else if (node.compiled.phase != DecideErrorPhase::INVALID) {
				// A body stage 05 lowered ranks by the list flat's evaluation reads it from: a constraint by its place
				// there, the objective, the composed clauses, then the objective's composed terms.
				rank = {node.compiled.phase, 0, node.compiled.order, 0};
			}
			if (assigned[index] && !(rank < node.value_rank)) {
				return;
			}
			assigned[index] = true;
			node.route_rank = node.value_rank = rank;
			if (node.kind == DecideNodeKind::FRAME) {
				// A frame's timelines are built and checked, frame after frame, before any frame's values are read
				// (language/guide.md §5.6).
				node.route_rank.step = 0;
			}
			for (auto child : node.children) {
				visit(child, rank);
			}
		};
		for (auto root : tree->roots) {
			auto &node = *tree->nodes[root];
			DecideErrorRank rank {DecideErrorPhase::CONSTRAINTS, 0, node.source_clause_id, 0};
			if (node.source_clause_id == DConstants::INVALID_INDEX) {
				rank.clause = root;
			}
			if (node.objective) {
				rank.phase = DecideErrorPhase::OBJECTIVE;
			}
			visit(root, rank);
			if (node.objective && op.objective_signature != DConstants::INVALID_INDEX) {
				// An objective with a signature is checked to be produced once (D72, language/guide.md §7.3), and the
				// check reads its kept rows and its signature before it counts its instances.
				node.route_rank.phase = DecideErrorPhase::SCOPE_CHECK;
			}
		}
	}

	string Text(const Expression &expression) const {
		return RenderDecideSource(expression, DecidePlanNames(op.source_fragments, op.keyed_scopes, op.keys,
		                                                      op.decide_index, op.aux_var_expressions));
	}

	//! How a message names a root, as flat names a row (DecideWrittenClause): by the clause it comes from as the user
	//! wrote it (QuoteWrittenClause), and a row no clause claims, which a rewrite added for the objective, by the
	//! objective.
	string RootText(idx_t source) const {
		if (source < op.constraint_sources.size()) {
			return QuoteWrittenClause(op.constraint_sources[source]);
		}
		return DecideWrittenObjective(op.decide_sense, op.written_objective);
	}

	unique_ptr<Expression> Resolve(const Expression &expression) const {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &reference = expression.Cast<BoundColumnRefExpression>();
			if (reference.binding.table_index == op.decide_index) {
				return expression.Copy();
			}
			for (idx_t i = 0; i < bindings.size(); i++) {
				if (bindings[i] == reference.binding) {
					return make_uniq<BoundReferenceExpression>(expression.GetAlias(), expression.return_type, i);
				}
			}
			throw InternalException("DECIDE tree cannot resolve source column %s", expression.ToString());
		}
		auto copy = expression.Copy();
		ExpressionIterator::EnumerateChildren(*copy, [&](unique_ptr<Expression> &child) { child = Resolve(*child); });
		return copy;
	}

	idx_t NewNode(DecideNodeKind kind) {
		auto index = tree->nodes.size();
		tree->nodes.push_back(make_uniq<DecideNode>(kind));
		return index;
	}

	void Edge(idx_t parent, idx_t child) {
		AddIndex(tree->nodes[parent]->children, child);
		AddIndex(tree->nodes[parent]->dependencies, child);
		if (tree->nodes[child]->parent == DConstants::INVALID_INDEX) {
			tree->nodes[child]->parent = parent;
		}
	}

	void Signature(DecideNode &node, DecideSignatureField field) {
		for (auto &known : node.signature) {
			if (known.kind == field.kind && known.decision == field.decision && known.key == field.key &&
			    known.frame == field.frame && Expression::Equals(known.value, field.value)) {
				return;
			}
		}
		node.signature.push_back(std::move(field));
	}

	void Column(DecideNode &node, const Expression &expression, bool in_signature) {
		for (auto &known : node.columns) {
			if (known->Equals(expression)) {
				if (!in_signature) {
					return;
				}
				break;
			}
		}
		bool found = false;
		for (auto &known : node.columns) {
			found = found || known->Equals(expression);
		}
		if (!found) {
			node.columns.push_back(expression.Copy());
		}
		if (in_signature) {
			DecideSignatureField field;
			field.kind = DecideSignatureFieldKind::VALUE;
			field.value = expression.Copy();
			field.text = Text(expression);
			Signature(node, std::move(field));
		}
	}

	void Gates(idx_t owner, const Expression *condition) {
		auto &node = *tree->nodes[owner];
		if (!condition) {
			return;
		}
		if (HasDecideTag(condition->GetAlias(), SKIP_NULL_TAG)) {
			// An O container gathers positions before any reader's generated
			// presence test runs. Testing that same frame here would make it
			// depend on its own not-yet-computed result.
			if (node.kind == DecideNodeKind::FRAME) {
				return;
			}
			node.member_tests.push_back(Lower(owner, *condition, true));
			return;
		}
		if (condition->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
			for (auto &child : condition->Cast<BoundConjunctionExpression>().children) {
				Gates(owner, child.get());
			}
			return;
		}
		node.when.push_back(Resolve(*condition));
	}

	void UseSignature(DecideNode &node, idx_t signature) {
		if (signature == DConstants::INVALID_INDEX) {
			// A class root's sigma is the binder's signature of its written clause, and none means no field: one
			// instance per class (D70, D72). Lower read what the rewrites made of the clause, and its helpers and
			// member tests (an ABS auxiliary, an easy extremum's member) are not fields of it. A root per row has no
			// class to split: its reads tell apart the instances of a keyed helper it defines (a range frame's
			// windows). An operand the binder did not sign keeps the values its body reads.
			if (node.root && node.per != DecideTreePer::ROW) {
				node.signature.clear();
			}
			return;
		}
		if (signature >= op.signatures.size()) {
			throw InternalException("DECIDE tree has invalid signature %llu", signature);
		}
		node.signature.clear();
		for (auto &field : op.signatures[signature].fields) {
			auto copy = CopyField(field);
			if (copy.value) {
				auto record = FindRecord(*copy.value);
				if (record != DConstants::INVALID_INDEX) {
					copy.kind = DecideSignatureFieldKind::BY_KEY;
					copy.key = op.retained_bound_aggregates[record].aggregate.key;
					copy.value.reset();
				} else {
					copy.value = Resolve(*copy.value);
				}
			}
			if (copy.kind == DecideSignatureFieldKind::DECISION) {
				for (auto &link : op.reducer_value_links) {
					if (copy.decision == link.value_var) {
						copy.decision = DConstants::INVALID_INDEX;
						if (link.frame == DConstants::INVALID_INDEX) {
							copy.kind = DecideSignatureFieldKind::BY_KEY;
							copy.key = link.group_key;
						} else {
							copy.kind = DecideSignatureFieldKind::FRAME;
							copy.frame = link.frame;
						}
						break;
					}
				}
			}
			Signature(node, std::move(copy));
		}
	}

	void Scope(idx_t owner, const DecideClauseScope &scope) {
		auto &node = *tree->nodes[owner];
		Gates(owner, scope.when.get());
		if (scope.per != DecideClauseScope::PerRole::NONE) {
			node.key = scope.class_key;
			node.per = node.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
		}
		UseSignature(node, scope.signature);
	}

	void Root(const BoundComparisonExpression &comparison, const DecideEnclosingScope &scope) {
		auto index = NewNode(DecideNodeKind::SCOPE);
		auto &node = *tree->nodes[index];
		node.root = true;
		node.facts = ReadDecideClauseFacts(context, op, comparison, scope);
		node.comparison_type = comparison.type;
		node.folded = HasDecideTag(comparison.GetAlias(), SINGLETON_ROW_BOUND_TAG);
		node.repair_rows = HasDecideTag(comparison.GetAlias(), SINGLETON_ROW_CLAUSE_ROWS_TAG);
		node.minmax_easy = HasDecideTag(comparison.GetAlias(), MINMAX_EASY_REWRITE_TAG);
		node.tightest_bound = HasDecideTag(comparison.GetAlias(), FRAME_TIGHTEST_BOUND_TAG);
		TryParseSourceClauseTag(comparison.GetAlias(), node.source_clause_id);
		node.text = RootText(node.source_clause_id);
		TryParseRemovalGroupTag(comparison.GetAlias(), node.removal_group_id);
		TryParseDecideIndexTag(comparison.GetAlias(), DEFINES_TAG_PREFIX, node.defined_var);
		Gates(index, scope.Condition());
		idx_t signature = DConstants::INVALID_INDEX;
		if (scope.per_wrapper) {
			node.key = scope.PerKey();
			node.per = node.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
			TryParseSignatureTag(scope.per_wrapper->GetAlias(), signature);
		}
		node.body = Lower(index, *comparison.left);
		node.rhs = Lower(index, *comparison.right);
		node.facts->rhs_expr = node.rhs->Copy();
		UseSignature(node, signature);
		sources.clause_roots.push_back(index);
		auto end = std::remove_if(node.signature.begin(), node.signature.end(), [&](const DecideSignatureField &field) {
			return field.kind == DecideSignatureFieldKind::DECISION && field.decision == node.defined_var;
		});
		node.signature.erase(end, node.signature.end());
		GroupClass(node);
		tree->roots.push_back(index);
	}

	//! A clause in the grouped shape (the aggregate shape, an easy or a composed extremum) is one model row per group
	//! of its `by` key (language/guide.md §6.4), so a root no `per` made a class root is a class of that key, one for
	//! `by ()`. Its sigma stays what it reads: a frame bound's position still tells a group's anchors apart, and the
	//! group takes their tightest value (D85). A `per K:` keeps its class, which names the classes a row stands for.
	void GroupClass(DecideNode &node) {
		if (node.per == DecideTreePer::ROW && (node.facts->lhs_is_aggregate || node.facts->was_minmax_easy)) {
			node.key = node.facts->group_key;
			node.per = node.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
		}
	}

	bool HasDecision(const Expression &expression) const {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
		    expression.Cast<BoundColumnRefExpression>().binding.table_index == op.decide_index) {
			return true;
		}
		bool found = false;
		ExpressionIterator::EnumerateChildren(expression,
		                                      [&](const Expression &child) { found = found || HasDecision(child); });
		return found;
	}

	unique_ptr<Expression> Read(idx_t owner, idx_t child, const LogicalType &type, const string &alias) {
		Edge(owner, child);
		auto &node = *tree->nodes[owner];
		const idx_t column = tree->input_column_count + child;
		node.child_reads.push_back({child, column, tree->nodes[child]->decision_value});
		node.decision_value = node.decision_value || tree->nodes[child]->decision_value;
		DecideSignatureField field;
		if (tree->nodes[child]->kind == DecideNodeKind::FRAME) {
			field.kind = DecideSignatureFieldKind::FRAME;
			field.frame = tree->nodes[child]->frame;
		} else {
			field.kind = DecideSignatureFieldKind::BY_KEY;
			field.key = tree->nodes[child]->key;
		}
		field.text = tree->nodes[child]->text;
		Signature(node, std::move(field));
		return make_uniq<BoundReferenceExpression>(alias, type, column);
	}

	idx_t FindRecord(const Expression &expression) const {
		for (idx_t i = 0; i < op.retained_bound_aggregates.size(); i++) {
			if (op.retained_bound_aggregates[i].placeholder->Equals(expression)) {
				return i;
			}
		}
		return DConstants::INVALID_INDEX;
	}

	//! With `carried`, the columns `expression` reads are carried, and neither they nor its decisions are sigma fields
	//! (D83). A query-wide value has one value everywhere. A generated NULL test only settles membership: a member
	//! test, or the test around a bound's reducer value (NamedReducerValue), which is its key alone (D70, deck p. 81).
	//! A child read adds its key field either way.
	unique_ptr<Expression> Lower(idx_t owner, const Expression &expression, bool carried = false) {
		auto &node = *tree->nodes[owner];
		idx_t retained;
		if (TryParseDecideIndexTag(expression.GetAlias(), TREE_RETAINED_EXPRESSION_TAG_PREFIX, retained)) {
			if (retained >= op.retained_expressions.size()) {
				throw InternalException("DECIDE tree: a retained expression is missing");
			}
			auto lowered = Lower(owner, *op.retained_expressions[retained], carried);
			// Stage 05 lowers the inlined reducer this tag marks; it ranges over the nest the copy became.
			if (expression.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE &&
			    lowered->GetExpressionClass() == ExpressionClass::BOUND_REF &&
			    lowered->Cast<BoundReferenceExpression>().index >= tree->input_column_count) {
				sources.reducer_nodes[&expression] =
				    lowered->Cast<BoundReferenceExpression>().index - tree->input_column_count;
			}
			return lowered;
		}
		carried = carried || IsQueryWideValueTag(expression.GetAlias()) || IsQueryWideBoundTag(expression.GetAlias()) ||
		          HasDecideTag(expression.GetAlias(), BOUND_REDUCER_TAG);
		auto record = FindRecord(expression);
		if (record != DConstants::INVALID_INDEX) {
			return Read(owner, Record(record), expression.return_type, expression.GetAlias());
		}
		idx_t frame;
		if (TryGetDecideFrame(expression, frame)) {
			return Read(owner, Frame(frame, &expression), expression.return_type, expression.GetAlias());
		}
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			auto reducer = Aggregate(expression.Cast<BoundAggregateExpression>());
			sources.reducer_nodes[&expression] = reducer;
			return Read(owner, reducer, expression.return_type, expression.GetAlias());
		}
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &reference = expression.Cast<BoundColumnRefExpression>();
			if (reference.binding.table_index == op.decide_index) {
				auto variable = reference.binding.column_index;
				for (idx_t link = 0; link < op.reducer_value_links.size(); link++) {
					if (op.reducer_value_links[link].value_var == variable) {
						return Read(owner, Link(link), expression.return_type, expression.GetAlias());
					}
				}
				node.decision_value = true;
				if (!carried && (variable >= op.variable_scopes.size() || !op.variable_scopes[variable].IsGlobal())) {
					DecideSignatureField field;
					field.kind = DecideSignatureFieldKind::DECISION;
					field.decision = variable;
					field.text = Text(expression);
					Signature(node, std::move(field));
				}
				return expression.Copy();
			}
			auto resolved = Resolve(expression);
			Column(node, *resolved, !carried);
			return resolved;
		}
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_REF) {
			Column(node, expression, !carried);
			return expression.Copy();
		}
		// The children are lowered from the operator's own expressions, so a reducer's address is the one stage 05
		// lowers (reducer_nodes); the copy only holds the results.
		auto result = expression.Copy();
		vector<const Expression *> originals;
		ExpressionIterator::EnumerateChildren(expression,
		                                      [&](const Expression &child) { originals.push_back(&child); });
		idx_t next = 0;
		ExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<Expression> &child) { child = Lower(owner, *originals[next++], carried); });
		return result;
	}

	idx_t Aggregate(const BoundAggregateExpression &aggregate) {
		auto index = NewNode(DecideNodeKind::REDUCER);
		auto &node = *tree->nodes[index];
		node.aggregate = DecideTreeReducerName(aggregate);
		node.text = Text(aggregate);
		node.decision_value = HasDecision(aggregate);
		node.key = DecideReducerGroupKey(aggregate);
		if (node.key == DConstants::INVALID_INDEX) {
			TryParseDecideIndexTag(aggregate.GetAlias(), TREE_REDUCER_KEY_TAG_PREFIX, node.key);
		}
		idx_t frame_aggregate = DConstants::INVALID_INDEX;
		if (TryParseFrameAggregateTag(aggregate.GetAlias(), frame_aggregate) &&
		    frame_aggregate < op.frame_aggregates.size()) {
			node.key = op.frame_aggregates[frame_aggregate].key;
			node.text = op.frame_aggregates[frame_aggregate].text;
		}
		node.function = unique_ptr_cast<Expression, BoundAggregateExpression>(aggregate.Copy());
		auto operand = NewNode(DecideNodeKind::SCOPE);
		Edge(index, operand);
		auto &entry = *tree->nodes[operand];
		entry.text = node.text + " operand";
		Gates(operand, aggregate.filter.get());
		idx_t qualifier;
		const bool qualified = TryParseQualifiedReducerTag(aggregate.GetAlias(), qualifier) ||
		                       TryParseDecideIndexTag(aggregate.GetAlias(), TREE_REDUCER_SCOPE_TAG_PREFIX, qualifier);
		if (qualified && qualifier < op.keyed_scopes.size()) {
			entry.key = op.keyed_scopes[qualifier].key;
			entry.per = entry.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
		}
		for (auto &argument : aggregate.children) {
			entry.arguments.push_back(Lower(operand, *argument));
		}
		if (!entry.arguments.empty()) {
			entry.body = entry.arguments[0]->Copy();
		}
		const Expression *norm_body = DecideWrittenNormBody(aggregate);
		if (frame_aggregate < op.frame_aggregates.size()) {
			norm_body = DecideNormBody(aggregate, op.frame_aggregates[frame_aggregate].norm_order);
		}
		if (norm_body && !node.decision_value) {
			// G's original data body is also the value validated before its
			// bound definition (SIGN for L0 can hide a NaN).
			node.body = Lower(operand, *norm_body);
		}
		if (qualified && qualifier < op.keyed_scopes.size()) {
			UseSignature(entry, op.keyed_scopes[qualifier].signature);
		}
		return index;
	}

	idx_t Link(idx_t link_index) {
		if (link_nodes[link_index] != DConstants::INVALID_INDEX) {
			return link_nodes[link_index];
		}
		auto &link = op.reducer_value_links[link_index];
		if (link.frame != DConstants::INVALID_INDEX) {
			auto index = Frame(link.frame, nullptr, link.inner_expr.get());
			link_nodes[link_index] = index;
			auto &node = *tree->nodes[index];
			node.link = link_index;
			node.value_var = link.value_var;
			node.decision_value = true;
			if (link.value_var < tree->value_nodes.size()) {
				tree->value_nodes[link.value_var] = index;
			}
			return index;
		}
		auto index = NewNode(DecideNodeKind::REDUCER);
		link_nodes[link_index] = index;
		auto &node = *tree->nodes[index];
		node.link = link_index;
		node.value_var = link.value_var;
		node.aggregate = link.agg_name;
		node.key = link.group_key;
		node.text = link.label;
		node.decision_value = true;
		Gates(index, link.when.get());
		if (link.value_var < tree->value_nodes.size()) {
			tree->value_nodes[link.value_var] = index;
		}
		auto operand = NewNode(DecideNodeKind::SCOPE);
		Edge(index, operand);
		auto &entry = *tree->nodes[operand];
		entry.text = node.text + " operand";
		Gates(operand, link.filter.get());
		if (link.qualifier_scope_idx < op.keyed_scopes.size()) {
			entry.key = op.keyed_scopes[link.qualifier_scope_idx].key;
			entry.per = entry.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
		}
		if (link.inner_expr) {
			entry.body = Lower(operand, *link.inner_expr);
		}
		if (link.qualifier_scope_idx < op.keyed_scopes.size()) {
			UseSignature(entry, op.keyed_scopes[link.qualifier_scope_idx].signature);
		}
		return index;
	}

	idx_t Record(idx_t record_index) {
		if (record_nodes[record_index] != DConstants::INVALID_INDEX) {
			return record_nodes[record_index];
		}
		auto &record = op.retained_bound_aggregates[record_index].aggregate;
		auto index = NewNode(DecideNodeKind::REDUCER);
		record_nodes[record_index] = index;
		auto &node = *tree->nodes[index];
		node.function = unique_ptr_cast<Expression, BoundAggregateExpression>(record.function->Copy());
		node.aggregate = DecideTreeReducerName(*node.function);
		node.key = record.key;
		for (auto &argument : record.arguments) {
			node.function->children.push_back(argument->Copy());
		}
		node.text = Text(*node.function);
		auto operand = NewNode(DecideNodeKind::SCOPE);
		Edge(index, operand);
		auto &entry = *tree->nodes[operand];
		entry.text = node.text + " operand";
		Gates(operand, record.reads.get());
		if (record.counts_classes) {
			entry.key = record.class_key;
			entry.per = entry.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
		}
		for (auto &argument : record.arguments) {
			entry.arguments.push_back(Lower(operand, *argument));
		}
		if (!entry.arguments.empty()) {
			entry.body = entry.arguments[0]->Copy();
		}
		if (auto norm_body = DecideWrittenNormBody(*node.function)) {
			node.body = Lower(operand, *norm_body);
		}
		return index;
	}

	idx_t Frame(idx_t frame_index, const Expression *marker, const Expression *payload = nullptr) {
		if (frame_index >= op.frames.size()) {
			throw InternalException("DECIDE tree references invalid frame %llu", frame_index);
		}
		if (frame_nodes[frame_index] != DConstants::INVALID_INDEX) {
			return frame_nodes[frame_index];
		}
		auto index = NewNode(DecideNodeKind::FRAME);
		frame_nodes[frame_index] = index;
		auto &node = *tree->nodes[index];
		auto &frame = op.frames[frame_index];
		node.frame = frame_index;
		node.frame_spec = CopyFrame(frame);
		node.key = frame.timeline_key;
		node.text = frame.text;
		node.aggregate = frame.reducer;
		Gates(index, frame.rows.get());
		if (frame.order) {
			node.over = Resolve(*frame.order);
			node.frame_spec->order = node.over->Copy();
		}
		if (marker && marker->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			auto &aggregate = marker->Cast<BoundAggregateExpression>();
			node.function = unique_ptr_cast<Expression, BoundAggregateExpression>(aggregate.Copy());
			for (auto &argument : aggregate.children) {
				node.arguments.push_back(Lower(index, *argument));
			}
			if (!node.arguments.empty()) {
				node.body = node.arguments[0]->Copy();
				node.decision_value = HasDecision(aggregate);
			}
		}
		if (payload && !node.body) {
			node.decision_value = HasDecision(*payload);
			node.body = Lower(index, *payload);
		}
		// Position conflicts compare the payload. Fallback columns belong to the
		// anchor, so retain them as carried values without adding them to sigma.
		auto payload_signature = std::move(node.signature);
		if (frame.fallback) {
			node.fallback = Lower(index, *frame.fallback);
			node.frame_spec->fallback = node.fallback->Copy();
		}
		node.signature = std::move(payload_signature);
		return index;
	}

	void Composed(idx_t owner, const LogicalDecide::ComposedMinMaxTerm &term, vector<idx_t> &reducers) {
		auto reducer = NewNode(DecideNodeKind::REDUCER);
		reducers.push_back(reducer);
		auto &node = *tree->nodes[reducer];
		node.aggregate = term.agg_name;
		node.text = term.inner_expr ? term.agg_name + "(" + Text(*term.inner_expr) + ")" : term.agg_name;
		node.decision_value = true;
		auto operand = NewNode(DecideNodeKind::SCOPE);
		Edge(reducer, operand);
		auto &entry = *tree->nodes[operand];
		entry.text = node.text + " operand";
		Gates(operand, term.filter.get());
		// ScopeComposedTerms prepends the clause/objective predicate to each
		// flat term. It belongs to the root E in this graph, so remove only
		// that known prefix; a repeated predicate written by the operand stays.
		auto &inherited = tree->nodes[owner]->when;
		bool prefix = entry.when.size() >= inherited.size();
		for (idx_t i = 0; prefix && i < inherited.size(); i++) {
			prefix = entry.when[i]->Equals(*inherited[i]);
		}
		if (prefix) {
			entry.when.erase(entry.when.begin(), entry.when.begin() + inherited.size());
		}
		if (term.qualifier_scope_idx < op.keyed_scopes.size()) {
			entry.key = op.keyed_scopes[term.qualifier_scope_idx].key;
			entry.per = entry.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
		}
		if (term.inner_expr) {
			entry.body = Lower(operand, *term.inner_expr);
		}
		auto value = Read(owner, reducer, LogicalType::DOUBLE, node.text);
		if (term.scale) {
			vector<unique_ptr<Expression>> scaled;
			scaled.push_back(std::move(value));
			scaled.push_back(Lower(owner, *term.scale));
			value = DecideRebindOperator(context, term.scale_divides ? "/" : "*", std::move(scaled));
		}
		if (term.sign < 0) {
			vector<unique_ptr<Expression>> negated;
			negated.push_back(std::move(value));
			value = DecideRebindOperator(context, "-", std::move(negated));
		}
		if (tree->nodes[owner]->body) {
			vector<unique_ptr<Expression>> added;
			added.push_back(std::move(tree->nodes[owner]->body));
			added.push_back(std::move(value));
			tree->nodes[owner]->body = DecideRebindOperator(context, "+", std::move(added));
		} else {
			tree->nodes[owner]->body = std::move(value);
		}
	}

	void AddOrphan(idx_t child) {
		auto root = NewNode(DecideNodeKind::SCOPE);
		tree->nodes[root]->root = tree->nodes[root]->hidden = true;
		tree->nodes[root]->text = "definition: " + tree->nodes[child]->text;
		Edge(root, child);
		tree->roots.push_back(root);
	}

	void Decisions(const Expression &expression, vector<idx_t> &variables) const {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &reference = expression.Cast<BoundColumnRefExpression>();
			if (reference.binding.table_index == op.decide_index) {
				AddIndex(variables, reference.binding.column_index);
			}
		}
		ExpressionIterator::EnumerateChildren(expression,
		                                      [&](const Expression &child) { Decisions(child, variables); });
	}

	//! Record tables carry a complete path predicate. Descent already inherited
	//! the parent's reach, so a path part guaranteed on every incoming edge is
	//! not another written `when` of the child. Own operand filters stay intact.
	void RemoveInheritedPathGates() {
		vector<vector<idx_t>> parents(tree->nodes.size());
		for (idx_t i = 0; i < tree->nodes.size(); i++) {
			for (auto child : tree->nodes[i]->children) {
				parents[child].push_back(i);
			}
		}
		std::function<bool(idx_t, const Expression &, vector<bool> &)> inherited =
		    [&](idx_t index, const Expression &condition, vector<bool> &visiting) {
			    if (parents[index].empty() || tree->nodes[index]->root || visiting[index]) {
				    return false;
			    }
			    visiting[index] = true;
			    for (auto parent : parents[index]) {
				    bool holds = false;
				    for (auto &gate : tree->nodes[parent]->when) {
					    holds = holds || gate->Equals(condition);
				    }
				    if (!holds && !inherited(parent, condition, visiting)) {
					    visiting[index] = false;
					    return false;
				    }
			    }
			    visiting[index] = false;
			    return true;
		    };
		for (idx_t i = 0; i < tree->nodes.size(); i++) {
			auto &node = *tree->nodes[i];
			if (node.link == DConstants::INVALID_INDEX && node.kind != DecideNodeKind::FRAME) {
				continue;
			}
			auto end = std::remove_if(node.when.begin(), node.when.end(), [&](const unique_ptr<Expression> &gate) {
				vector<bool> visiting(tree->nodes.size(), false);
				return inherited(i, *gate, visiting);
			});
			node.when.erase(end, node.when.end());
		}
	}

	void Definitions() {
		auto &definitions = tree->definition_nodes;
		for (idx_t i = 0; i < tree->nodes.size(); i++) {
			auto variable = tree->nodes[i]->defined_var;
			if (variable < definitions.size()) {
				definitions[variable].push_back(i);
			}
		}
		for (idx_t i = 0; i < tree->nodes.size(); i++) {
			auto &node = *tree->nodes[i];
			auto &variables = node.decisions;
			for (auto *expression : {node.body.get(), node.rhs.get(), node.fallback.get()}) {
				if (expression) {
					Decisions(*expression, variables);
				}
			}
			for (auto &argument : node.arguments) {
				Decisions(*argument, variables);
			}
			for (auto variable : variables) {
				if (variable != node.defined_var) {
					Sources(variable, i);
				}
			}
		}
	}

	//! A reader of a decision waits for what the decision is defined from (D88): the roots defining it, the G or O
	//! holding its value, and for a product (BilinearLink) what each factor is defined from.
	void Sources(idx_t variable, idx_t reader) {
		if (variable >= tree->definition_nodes.size()) {
			return;
		}
		auto &dependencies = tree->nodes[reader]->dependencies;
		for (auto definition : tree->definition_nodes[variable]) {
			if (definition != reader) {
				AddIndex(dependencies, definition);
			}
		}
		const auto value = tree->value_nodes[variable];
		if (value != DConstants::INVALID_INDEX) {
			AddIndex(dependencies, value);
		}
		for (auto &product : op.bilinear_links) {
			if (product.aux_idx == variable) {
				Sources(product.bool_var_idx, reader);
				Sources(product.other_var_idx, reader);
			}
		}
	}

	void PostOrder() {
		vector<uint8_t> state(tree->nodes.size(), 0);
		std::function<void(idx_t)> visit = [&](idx_t index) {
			if (state[index] == 1) {
				throw InternalException("DECIDE tree has a dependency cycle at %s", tree->nodes[index]->text);
			}
			if (state[index] == 2) {
				return;
			}
			state[index] = 1;
			for (auto dependency : tree->nodes[index]->dependencies) {
				visit(dependency);
			}
			state[index] = 2;
			tree->eval_order.push_back(index);
		};
		for (auto root : tree->roots) {
			visit(root);
		}
		for (idx_t i = 0; i < tree->nodes.size(); i++) {
			visit(i);
		}
	}
};

} // namespace decide_tree_build

struct DecideTreeConstruction::Builder : decide_tree_build::DecideTreeBuilder {
	using decide_tree_build::DecideTreeBuilder::DecideTreeBuilder;
};

DecideTreeConstruction::DecideTreeConstruction(ClientContext &context, const LogicalDecide &op)
    : builder(make_uniq<Builder>(context, op)) {
	builder->Structure();
}

DecideTreeConstruction::~DecideTreeConstruction() {
}

idx_t DecideTreeConstruction::ReducerChild(const BoundAggregateExpression &reducer) const {
	return builder->ReducerChild(reducer);
}

unique_ptr<DecideTree> DecideTreeConstruction::Finish() {
	return builder->Finish();
}

void DecideTree::Validate() const {
	vector<idx_t> position(nodes.size(), DConstants::INVALID_INDEX);
	if (eval_order.size() != nodes.size()) {
		throw InternalException("DECIDE tree post-order does not cover every node");
	}
	for (idx_t i = 0; i < eval_order.size(); i++) {
		auto node = eval_order[i];
		if (node >= nodes.size() || position[node] != DConstants::INVALID_INDEX) {
			throw InternalException("DECIDE tree post-order contains an invalid or repeated node");
		}
		position[node] = i;
	}
	for (idx_t i = 0; i < nodes.size(); i++) {
		auto &node = *nodes[i];
		if (node.key != DConstants::INVALID_INDEX && node.key >= keys.size()) {
			throw InternalException("DECIDE tree node %llu has an invalid key", i);
		}
		for (auto child : node.children) {
			if (child >= nodes.size() ||
			    std::find(node.dependencies.begin(), node.dependencies.end(), child) == node.dependencies.end()) {
				throw InternalException("DECIDE tree node %llu has an invalid child", i);
			}
		}
		for (auto dependency : node.dependencies) {
			if (dependency >= nodes.size() || position[dependency] >= position[i]) {
				throw InternalException("DECIDE tree node %llu runs before a dependency", i);
			}
		}
		for (auto &column : node.columns) {
			if (column->GetExpressionClass() != ExpressionClass::BOUND_REF ||
			    column->Cast<BoundReferenceExpression>().index >= input_column_count) {
				throw InternalException("DECIDE tree entry carries a non-source column");
			}
		}
		for (auto &read : node.child_reads) {
			if (read.node >= nodes.size() || read.column != input_column_count + read.node) {
				throw InternalException("DECIDE tree body has an invalid child result");
			}
		}
		// A program reads its node's layout, then the results of its arguments, and no column of a plan.
		for (auto &program : node.programs) {
			const auto width = node.columns.size() + node.program_columns.size() + program.arguments.size();
			std::function<void(const Expression &)> check = [&](const Expression &expression) {
				if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF ||
				    (expression.GetExpressionClass() == ExpressionClass::BOUND_REF &&
				     expression.Cast<BoundReferenceExpression>().index >= width)) {
					throw InternalException("DECIDE tree node %llu has a program reading outside its layout", i);
				}
				ExpressionIterator::EnumerateChildren(expression, check);
			};
			check(*program.expression);
		}
	}
	for (auto root : roots) {
		if (root >= nodes.size() || !nodes[root]->root || nodes[root]->kind != DecideNodeKind::SCOPE) {
			throw InternalException("DECIDE tree has an invalid root");
		}
	}
}

string DecideTree::ToString() const {
	std::ostringstream result;
	for (idx_t i = 0; i < nodes.size(); i++) {
		auto &node = *nodes[i];
		result << i << " "
		       << (node.kind == DecideNodeKind::SCOPE     ? "E"
		           : node.kind == DecideNodeKind::REDUCER ? "G"
		                                                  : "O");
		if (node.root) {
			result << (node.objective ? " objective" : node.hidden ? " definition" : " root");
		}
		result << " key=" << DecideKeyText(keys, node.key);
		if (!node.aggregate.empty()) {
			result << " agg=" << node.aggregate;
		}
		result << " per=" << (node.per == DecideTreePer::ROW ? "row" : node.per == DecideTreePer::KEY ? "key" : "one");
		result << " when=[";
		for (idx_t w = 0; w < node.when.size(); w++) {
			result << (w ? ", " : "") << node.when[w]->ToString();
		}
		result << "] sigma=[";
		for (idx_t f = 0; f < node.signature.size(); f++) {
			result << (f ? ", " : "") << node.signature[f].text;
		}
		result << "] reads=[";
		for (idx_t d = 0; d < node.dependencies.size(); d++) {
			result << (d ? "," : "") << node.dependencies[d];
		}
		result << "] columns=" << node.columns.size() << " : " << node.text << "\n";
	}
	result << "postorder=[";
	for (idx_t i = 0; i < eval_order.size(); i++) {
		result << (i ? "," : "") << eval_order[i];
	}
	result << "]\n";
	return result.str();
}

} // namespace duckdb
