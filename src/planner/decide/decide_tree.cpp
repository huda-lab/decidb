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
	if (TryParseDecideIndexTag(comparison.alias, ABS_FUNCTION_NEG_TAG_PREFIX, facts->abs_aux_idx)) {
		facts->kind = ConstraintKind::STRUCTURAL;
	} else if (TryParseDecideIndexTag(comparison.alias, ABS_FUNCTION_TAG_PREFIX, facts->abs_aux_idx)) {
		facts->abs_is_pos_bound = true;
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
		tree->qualified_operands.assign(op.keyed_scopes.size(), DConstants::INVALID_INDEX);
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
		tree->frame_aggregate_nodes.assign(op.frame_aggregates.size(), DConstants::INVALID_INDEX);
	}

	//! The nodes, their edges, sigma, gates and definitions, before stage 05 lowers the clauses.
	void Structure() {
		// Register definition F nodes before lowering any other function body: an ABS or product written inside a
		// nonlinear reducer body reads that F node. Its own body is lowered only after ABS, product and L0 nodes have
		// been registered, so the reverse dependency resolves too.
		RegisterDefinitionFunctions();
		PrepareBilinearFunctions();
		PrepareAbsFunctions();
		PrepareL0IndicatorFunctions();
		LowerDefinitionFunctionBodies();
		for (auto &check : op.retained_row_checks) {
			auto index = NewNode(DecideNodeKind::SCOPE);
			auto &node = *tree->nodes[index];
			node.root = node.hidden = node.value_check = true;
			node.text = "data value check";
			node.body = Lower(index, *check);
			tree->roots.push_back(index);
		}
		if (op.decide_constraints) {
			ForEachScopedConstraintLeaf(
			    *op.decide_constraints, [&](const Expression &expression, const DecideEnclosingScope &scope) {
				    if (expression.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
					    Root(expression.Cast<BoundComparisonExpression>(), scope);
				    } else if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
				               HasDecideTag(expression.GetAlias(), BODY_DEFINITION_TAG)) {
					    idx_t value_var = DConstants::INVALID_INDEX;
					    if (!TryParseDecideIndexTag(expression.GetAlias(), DEFINES_TAG_PREFIX, value_var)) {
						    throw InternalException("DECIDE tree: body-definition placeholder has no helper");
					    }
					    auto function = std::find_if(
					        tree->function_nodes.begin(), tree->function_nodes.end(), [&](idx_t candidate) {
						        return tree->nodes[candidate]->function_kind == DecideFunctionKind::DEFINITION &&
						               tree->nodes[candidate]->value_var == value_var;
					        });
					    if (function == tree->function_nodes.end() ||
					        !tree->nodes[*function]->definition_expression) {
						    throw InternalException("DECIDE tree: body-definition placeholder has no F node");
					    }
					    ForEachScopedConstraintLeaf(
					        *tree->nodes[*function]->definition_expression,
					        [&](const Expression &row, const DecideEnclosingScope &row_scope) {
						        if (row.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
							        Root(row.Cast<BoundComparisonExpression>(), row_scope);
						        }
				        });
				    }
			    });
		}
		// RewriteNorm used to append these structural link rows after every written constraint. Preserve that order
		// while keeping the rows as children of their L0 indicator F nodes in the tree.
		for (auto &indicator : op.l0_indicator_functions) {
			for (auto &expression : indicator.links) {
				ForEachScopedConstraintLeaf(*expression, [&](const Expression &leaf, const DecideEnclosingScope &scope) {
					if (leaf.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
						Root(leaf.Cast<BoundComparisonExpression>(), scope);
					}
				});
			}
		}
		for (auto &envelope : op.abs_function_envelopes) {
			for (auto *expression : {&envelope.positive, &envelope.negative}) {
				if (!*expression) {
					continue;
				}
				ForEachScopedConstraintLeaf(**expression, [&](const Expression &leaf, const DecideEnclosingScope &scope) {
					if (leaf.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
						Root(leaf.Cast<BoundComparisonExpression>(), scope);
					}
				});
			}
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

	//! Create ABS F nodes before lowering clauses, so retained function expressions can read their nodes even when
	//! the envelope roots occur later in source order.
	void PrepareAbsFunctions() {
		struct AbsFunctionSpec {
			idx_t aux;
			unique_ptr<Expression> condition;
			// Keep the LogicalDecide-owned operand alive: Lower records reducer addresses in sources.reducer_nodes.
			const Expression *operand = nullptr;
			idx_t source_clause;
		};
		vector<AbsFunctionSpec> specs;
		for (auto &envelope : op.abs_function_envelopes) {
			if (!envelope.positive) {
				continue;
			}
			ForEachScopedConstraintLeaf(*envelope.positive,
			                            [&](const Expression &leaf, const DecideEnclosingScope &scope) {
				if (leaf.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
					return;
				}
				auto &comparison = leaf.Cast<BoundComparisonExpression>();
				idx_t aux;
				if (!TryParseDecideIndexTag(comparison.GetAlias(), ABS_FUNCTION_TAG_PREFIX, aux)) {
					return;
				}
				idx_t source = DConstants::INVALID_INDEX;
				TryParseSourceClauseTag(comparison.GetAlias(), source);
				AbsFunctionSpec spec;
				spec.aux = aux;
				spec.condition = scope.Condition() ? scope.Condition()->Copy() : nullptr;
				spec.operand = envelope.operand.get();
				spec.source_clause = source;
				specs.push_back(std::move(spec));
			});
		}
		for (auto &spec : specs) {
			AbsFunctionNode(spec.aux, spec.source_clause);
		}
		for (auto &spec : specs) {
			auto function = std::find_if(tree->function_nodes.begin(), tree->function_nodes.end(), [&](idx_t index) {
				return tree->nodes[index]->value_var == spec.aux;
			});
			if (function == tree->function_nodes.end() || !spec.operand) {
				throw InternalException("DECIDE tree: ABS helper %llu has no function operand", spec.aux);
			}
			auto &node = *tree->nodes[*function];
			if (!node.body) {
				node.body = Lower(*function, *spec.operand);
			}
		}
		// All F nodes exist now, so gates can themselves resolve nested ABS function reads.
		for (auto &spec : specs) {
			auto function = std::find_if(tree->function_nodes.begin(), tree->function_nodes.end(), [&](idx_t index) {
				return tree->nodes[index]->value_var == spec.aux;
			});
			if (function != tree->function_nodes.end()) {
				Gates(*function, spec.condition.get());
			}
		}
	}

	//! Each optimizer-allocated Boolean product becomes a rooted F node. The function's body names its two decision
	//! factors; tree-owned BilinearLink records are handed to stage 06 by way of these nodes.
	void PrepareBilinearFunctions() {
		for (auto &group : op.bilinear_function_groups) {
			for (auto &product : group.products) {
				if (product.aux_idx >= op.decide_variables.size() ||
				    product.first_var_idx >= op.decide_variables.size() ||
				    product.second_var_idx >= op.decide_variables.size()) {
					throw InternalException("DECIDE tree: bilinear function references an invalid decision");
				}
				const auto is_boolean = [&](idx_t var) {
					return var < op.is_boolean_var.size() && op.is_boolean_var[var];
				};
				const auto bool_var = is_boolean(product.first_var_idx) ? product.first_var_idx
				                                                     : product.second_var_idx;
				const auto other_var = bool_var == product.first_var_idx ? product.second_var_idx
				                                                     : product.first_var_idx;

				auto index = NewNode(DecideNodeKind::FUNCTION);
				auto &node = *tree->nodes[index];
				node.function_kind = DecideFunctionKind::BOOLEAN_PRODUCT;
				node.product_bool_var = bool_var;
				node.product_other_var = other_var;
				node.root = node.hidden = node.decision_value = true;
				node.value_var = node.defined_var = product.aux_idx;
				node.source_clause_id = product.source_clause_id;
				node.removal_group_id = product.removal_group_id;
				auto label = std::find_if(op.aux_var_expressions.begin(), op.aux_var_expressions.end(),
				                          [&](const auto &entry) { return entry.first == product.aux_idx; });
				node.text = label == op.aux_var_expressions.end()
				                ? "Boolean product helper " + std::to_string(product.aux_idx)
				                : label->second;
				if (product.aux_idx < tree->variable_scopes.size()) {
					auto &variable_scope = tree->variable_scopes[product.aux_idx];
					if (variable_scope.IsKeyed() || variable_scope.scope == DecideVarScope::INLINED) {
						if (variable_scope.keyed_scope_idx >= op.keyed_scopes.size()) {
							if (variable_scope.scope != DecideVarScope::INLINED ||
							    variable_scope.keyed_scope_idx != DConstants::INVALID_INDEX) {
								throw InternalException("DECIDE tree: product helper %llu has an invalid keyed scope",
							                        product.aux_idx);
							}
						} else {
							node.per = DecideTreePer::KEY;
							node.key = op.keyed_scopes[variable_scope.keyed_scope_idx].key;
						}
					} else if (variable_scope.IsGlobal()) {
						node.per = DecideTreePer::ONE;
					}
				}
				tree->function_nodes.push_back(index);
				tree->roots.push_back(index);
			}
		}

		// Register every product F before lowering any product body, so a nested helper can be read if a later rewrite
		// made one product's factor another product's auxiliary.
		for (auto function : tree->function_nodes) {
			auto &node = *tree->nodes[function];
			if (node.function_kind != DecideFunctionKind::BOOLEAN_PRODUCT) {
				continue;
			}
			vector<unique_ptr<Expression>> children;
			children.push_back(op.decide_variables[node.product_bool_var]->Copy());
			children.push_back(op.decide_variables[node.product_other_var]->Copy());
			auto body = DecideRebindOperator(context, "*", std::move(children));
			node.body = Lower(function, *body);
		}
	}

	//! Each L0 indicator's three structural links are owned by its F node. The links remain optimizer metadata so
	//! stage-05 rewrites can prepare nested ABS and product functions; stage 06 evaluates them as child roots.
	void PrepareL0IndicatorFunctions() {
		for (auto &indicator : op.l0_indicator_functions) {
			if (indicator.value_var >= op.decide_variables.size() || indicator.links.size() != 3) {
				throw InternalException("DECIDE tree: malformed L0 indicator function");
			}
			auto index = NewNode(DecideNodeKind::FUNCTION);
			auto &node = *tree->nodes[index];
			node.function_kind = DecideFunctionKind::L0_INDICATOR;
			node.root = node.hidden = node.decision_value = true;
			node.value_var = node.defined_var = indicator.value_var;
			node.l0_auto_m = indicator.auto_m;
			node.l0_m = indicator.m;
			node.text = indicator.label;
			if (indicator.value_var < tree->variable_scopes.size()) {
				auto &variable_scope = tree->variable_scopes[indicator.value_var];
				if (variable_scope.IsKeyed() || variable_scope.scope == DecideVarScope::INLINED) {
					if (variable_scope.keyed_scope_idx >= op.keyed_scopes.size()) {
						if (variable_scope.scope != DecideVarScope::INLINED ||
						    variable_scope.keyed_scope_idx != DConstants::INVALID_INDEX) {
							throw InternalException("DECIDE tree: L0 indicator %llu has an invalid keyed scope",
							                        indicator.value_var);
						}
					} else {
						node.per = DecideTreePer::KEY;
						node.key = op.keyed_scopes[variable_scope.keyed_scope_idx].key;
					}
				} else if (variable_scope.IsGlobal()) {
					node.per = DecideTreePer::ONE;
				}
			}
			ForEachScopedConstraintLeaf(*indicator.links[0], [&](const Expression &leaf, const DecideEnclosingScope &) {
				if (leaf.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
					TryParseSourceClauseTag(leaf.GetAlias(), node.source_clause_id);
					TryParseRemovalGroupTag(leaf.GetAlias(), node.removal_group_id);
				}
			});
			tree->function_nodes.push_back(index);
			tree->roots.push_back(index);
		}

		// Register every indicator before lowering its operand, so nested ABS/product references can resolve.
		for (auto &indicator : op.l0_indicator_functions) {
			auto function = std::find_if(tree->function_nodes.begin(), tree->function_nodes.end(), [&](idx_t index) {
				return tree->nodes[index]->function_kind == DecideFunctionKind::L0_INDICATOR &&
				       tree->nodes[index]->value_var == indicator.value_var;
			});
			if (function == tree->function_nodes.end()) {
				throw InternalException("DECIDE tree: L0 indicator %llu has no function node", indicator.value_var);
			}
			const Expression *operand = nullptr;
			const Expression *condition = nullptr;
			ForEachScopedConstraintLeaf(*indicator.links[0], [&](const Expression &leaf, const DecideEnclosingScope &scope) {
				if (leaf.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
					operand = leaf.Cast<BoundComparisonExpression>().right.get();
					condition = scope.Condition();
				}
			});
			if (!operand) {
				throw InternalException("DECIDE tree: L0 indicator %llu has no counted operand", indicator.value_var);
			}
			auto &node = *tree->nodes[*function];
			Gates(*function, condition);
			node.body = Lower(*function, *operand);
		}
	}

	//! A nonlinear reducer body's helper and structural definition row form an F node. The optimizer retains the body
	//! and relation; this tree constructs the row beneath the F node at the placeholder's original clause position.
	void RegisterDefinitionFunctions() {
		for (auto &definition : op.definition_functions) {
			const auto value_var = definition.value_var;
			if (value_var >= op.decide_variables.size() || !definition.body) {
				throw InternalException("DECIDE tree: nonlinear body definition has no valid helper or source body");
			}
			auto index = NewNode(DecideNodeKind::FUNCTION);
			auto &node = *tree->nodes[index];
			node.function_kind = DecideFunctionKind::DEFINITION;
			node.root = node.hidden = node.decision_value = true;
			node.value_var = node.defined_var = value_var;
			auto label = std::find_if(op.aux_var_expressions.begin(), op.aux_var_expressions.end(),
			                          [&](const auto &entry) { return entry.first == value_var; });
			node.text = label == op.aux_var_expressions.end() ? "nonlinear body helper " + std::to_string(value_var)
			                                                  : label->second;
			node.definition_relation = definition.relation == ExpressionType::COMPARE_EQUAL
			                               ? DecideDefinitionRelation::EQUAL
			                           : definition.relation == ExpressionType::COMPARE_GREATERTHANOREQUALTO
			                               ? DecideDefinitionRelation::AT_LEAST
			                           : definition.relation == ExpressionType::COMPARE_LESSTHANOREQUALTO
			                                   ? DecideDefinitionRelation::AT_MOST
			                                   : DecideDefinitionRelation::INVALID;
			if (node.definition_relation == DecideDefinitionRelation::INVALID) {
				throw InternalException("DECIDE tree: nonlinear body definition has an unsupported comparison");
			}
			if (value_var < tree->variable_scopes.size()) {
				auto &variable_scope = tree->variable_scopes[value_var];
				if (variable_scope.IsKeyed() || variable_scope.scope == DecideVarScope::INLINED) {
					if (variable_scope.keyed_scope_idx >= op.keyed_scopes.size()) {
						if (variable_scope.scope != DecideVarScope::INLINED ||
						    variable_scope.keyed_scope_idx != DConstants::INVALID_INDEX) {
							throw InternalException("DECIDE tree: body helper %llu has an invalid keyed scope", value_var);
						}
					} else {
						node.per = DecideTreePer::KEY;
						node.key = op.keyed_scopes[variable_scope.keyed_scope_idx].key;
					}
				} else if (variable_scope.IsGlobal()) {
					node.per = DecideTreePer::ONE;
				}
			}
			Gates(index, definition.condition.get());
			node.definition_expression = MakeDefinitionExpression(definition);
			tree->function_nodes.push_back(index);
			tree->roots.push_back(index);
		}

	}

	//! Lower definition bodies after all F nodes exist, allowing definitions to read nested ABS, product, L0 and
	//! definition values through the tree.
	void LowerDefinitionFunctionBodies() {
		for (auto &definition : op.definition_functions) {
			auto function = std::find_if(tree->function_nodes.begin(), tree->function_nodes.end(), [&](idx_t index) {
				return tree->nodes[index]->function_kind == DecideFunctionKind::DEFINITION &&
			       tree->nodes[index]->value_var == definition.value_var;
			});
			if (function == tree->function_nodes.end()) {
				throw InternalException("DECIDE tree: nonlinear body helper %llu has no function node", definition.value_var);
			}
			auto &node = *tree->nodes[*function];
			node.body = Lower(*function, *definition.body);
		}
	}

	unique_ptr<Expression> MakeDefinitionExpression(const LogicalDecide::DefinitionFunction &definition) const {
		const auto &body = *definition.body;
		auto left = BoundCastExpression::AddCastToType(context, op.decide_variables[definition.value_var]->Copy(),
		                                              body.return_type);
		auto row = make_uniq<BoundComparisonExpression>(definition.relation, std::move(left), body.Copy());
		auto alias = row->GetAlias();
		AddDecideTag(alias, STRUCTURAL_CONSTRAINT_TAG);
		AddDecideTag(alias, MakeDefinesTag(definition.value_var));
		AddDecideTag(alias, BODY_DEFINITION_TAG);
		row->SetAlias(std::move(alias));
		unique_ptr<Expression> result = std::move(row);
		if (definition.condition) {
			auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			wrapper->children.push_back(std::move(result));
			wrapper->children.push_back(definition.condition->Copy());
			wrapper->alias = WHEN_FILTER_TAG;
			result = std::move(wrapper);
		}
		if (definition.value_var < op.variable_scopes.size()) {
			auto &scope = op.variable_scopes[definition.value_var];
			if (scope.IsGlobal() || scope.IsKeyed()) {
				auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
				wrapper->children.push_back(std::move(result));
				wrapper->alias = PER_CLASS_TAG;
				if (scope.IsKeyed()) {
					AddDecideTag(wrapper->alias, MakePerKeyTag(op.keyed_scopes[scope.keyed_scope_idx].key));
				}
				result = std::move(wrapper);
			}
		}
		return result;
	}

	//! The ABS F node owns the helper's scope and both transitional envelope roots. Its operand stays as an expression
	//! until the function lowering compiles it; the envelope roots already carry executable row programs.
	idx_t AbsFunctionNode(idx_t aux, idx_t source_clause) {
		for (auto existing : tree->function_nodes) {
			if (tree->nodes[existing]->value_var == aux) {
				return existing;
			}
		}
		if (!tree->variable_scopes.empty() && aux >= tree->variable_scopes.size()) {
			throw InternalException("DECIDE tree: ABS helper %llu has no variable scope", aux);
		}
		auto index = NewNode(DecideNodeKind::FUNCTION);
		auto &node = *tree->nodes[index];
		node.function_kind = DecideFunctionKind::ABS;
		node.root = true;
		node.value_var = aux;
		node.defined_var = aux;
		node.hidden = true;
		node.decision_value = true;
		node.source_clause_id = source_clause;
		auto label = std::find_if(op.aux_var_expressions.begin(), op.aux_var_expressions.end(),
		                          [&](const auto &entry) { return entry.first == aux; });
		node.text = label == op.aux_var_expressions.end() ? "abs helper " + std::to_string(aux) : label->second;
		if (aux < tree->variable_scopes.size()) {
			auto &variable_scope = tree->variable_scopes[aux];
			if (variable_scope.IsKeyed() || variable_scope.scope == DecideVarScope::INLINED) {
				if (variable_scope.keyed_scope_idx >= op.keyed_scopes.size()) {
					if (variable_scope.scope != DecideVarScope::INLINED ||
					    variable_scope.keyed_scope_idx != DConstants::INVALID_INDEX) {
						throw InternalException("DECIDE tree: ABS helper %llu has an invalid keyed scope", aux);
					}
				} else {
					node.per = DecideTreePer::KEY;
					node.key = op.keyed_scopes[variable_scope.keyed_scope_idx].key;
				}
			} else if (variable_scope.IsGlobal()) {
				node.per = DecideTreePer::ONE;
			}
		}
		for (auto &link : op.abs_maximize_links) {
			if (link.aux_idx == aux) {
				node.abs_needs_bigm = true;
				node.abs_indicator_var = link.y_idx;
				break;
			}
		}
		tree->function_nodes.push_back(index);
		tree->roots.push_back(index);
		return index;
	}

	//! The bodies from what stage 05 lowered, then the order and ranks they read.
	unique_ptr<DecideTree> Finish() {
		sources.link_nodes = link_nodes;
		sources.record_nodes = record_nodes;
		tree->frame_nodes = frame_nodes;
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
		if (found != sources.reducer_nodes.end()) {
			return DecideTreeMembersOf(*tree, found->second);
		}
		// ABS envelope metadata is lowered independently from the retained source expression, so its canonicalized
		// reducer nodes are equal copies rather than the same addresses. Reuse the corresponding G when the reducer's
		// full expression and semantic tags match; otherwise keep the pointer-identity invariant strict.
		if (!inject) {
			for (auto &entry : sources.reducer_nodes) {
				if (entry.first && entry.first->GetAlias() == reducer.GetAlias() &&
				    DecideExpressionsEqual(*entry.first, reducer)) {
					return DecideTreeMembersOf(*tree, entry.second);
				}
			}
		}
		throw InternalException("DECIDE tree: stage 05 lowers a reducer the tree has no node for: %s",
		                        reducer.ToString());
	}

	//! DecideTreeConstruction::TakeClause.
	void TakeClause(const Expression &leaf, unique_ptr<DecideConstraint> clause, bool singleton_row) {
		auto root = sources.root_of.find(&leaf);
		D_ASSERT(root != sources.root_of.end());
		auto &node = *tree->nodes[root->second];
		D_ASSERT(node.folded == singleton_row && !node.compiled.clause);
		(void)singleton_row;
		node.compiled.clause = std::move(clause);
	}

	void TakeObjective(unique_ptr<DecideObjective> objective) {
		D_ASSERT(sources.objective_root != DConstants::INVALID_INDEX);
		tree->nodes[sources.objective_root]->compiled.objective = std::move(objective);
	}

	void TakeComposedTerms(idx_t spec, idx_t term, vector<DecideTerm> terms) {
		tree->nodes[sources.composed_reducers[spec][term]]->compiled.members = std::move(terms);
	}

	void TakeLinkTerms(idx_t link, vector<DecideTerm> members, vector<DecideTerm> fallback) {
		auto &body = tree->nodes[link_nodes[link]]->compiled;
		body.members = std::move(members);
		body.fallback = std::move(fallback);
	}

	void TakeFrameReads(DecideFrameReads frame_reads) {
		tree->frame_reads = std::move(frame_reads);
	}

	const DecideTree &Tree() const {
		return *tree;
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
	//! Canonical function-link roots live in `sources.reducer_nodes` by expression address. Keep their ASTs alive until
	//! the linear form builder has resolved those reducers; otherwise the address map holds pointers into freed copies.
	vector<unique_ptr<Expression>> retained_function_links;
	//! Each gate a helper's G or a frame's O inherits from the nodes above it (RemoveInheritedPathGates): the node
	//! holding it, then the node inheriting it.
	vector<pair<idx_t, idx_t>> inherited_gates;

	//! Where flat reads each keyed scope's classes and each auxiliary's definitions
	//!: every scope in turn, a helper's once the definitions of the auxiliaries
	//! its members read are, then every decision's definitions in turn. Flat's other recursions number nothing earlier
	//! here: from a decision into its own scope, since the declared scopes come before the rewrites' helpers and a
	//! nested helper before its reader, and from a definition into what it reads, since the rewrites number a nested
	//! auxiliary first. Per scope and per clause, its place.
	void KeyedMappingOrder(vector<idx_t> &scope_order, vector<idx_t> &definition_order) const {
		vector<vector<idx_t>> definitions(op.decide_variables.size());
		for (auto root : sources.clause_roots) {
			auto &body = tree->nodes[root]->compiled;
			if (body.clause && body.clause_index < tree->constraint_count &&
			    body.clause->defined_var < definitions.size()) {
				definitions[body.clause->defined_var].push_back(body.clause_index);
			}
		}
		scope_order.assign(op.keyed_scopes.size(), DConstants::INVALID_INDEX);
		definition_order.assign(tree->constraint_count, DConstants::INVALID_INDEX);
		vector<bool> reached(definitions.size(), false);
		idx_t next = 0;
		auto read = [&](idx_t variable) {
			if (variable >= definitions.size() || reached[variable]) {
				return;
			}
			reached[variable] = true;
			for (auto c : definitions[variable]) {
				definition_order[c] = next++;
			}
		};
		for (idx_t scope = 0; scope < op.keyed_scopes.size(); scope++) {
			const auto link = op.keyed_scopes[scope].value_link;
			if (link != DConstants::INVALID_INDEX) {
				for (auto &term : tree->nodes[link_nodes[link]]->compiled.members) {
					read(term.variable_index);
				}
			}
			scope_order[scope] = next++;
		}
		for (idx_t variable = 0; variable < definitions.size(); variable++) {
			read(variable);
		}
	}

	//! A node's errors rank at the earliest phase that reads it, in flat's order of phases: a bound's reducer is read
	//! after the data checks on the rows, a definition where its auxiliary's absence is resolved, a helper's rows where
	//! its classes are, a composed MIN/MAX clause after the objective, and the objective's composed terms after the
	//! composed clauses.
	void ErrorRanks() {
		vector<idx_t> scope_order, definition_order;
		KeyedMappingOrder(scope_order, definition_order);
		vector<bool> assigned(tree->nodes.size(), false);
		std::function<void(idx_t, DecideErrorRank)> visit = [&](idx_t index, DecideErrorRank rank) {
			auto &node = *tree->nodes[index];
			auto &aggregates = tree->frame_aggregate_nodes;
			const auto aggregate = idx_t(std::find(aggregates.begin(), aggregates.end(), index) - aggregates.begin());
			// Where its clause evaluates it, if not where it is first computed.
			DecideErrorRank evaluated;
			if (node.value_check) {
				rank = {DecideErrorPhase::SOURCE_CHECK, 0, rank.clause, 0};
			} else if (std::find(record_nodes.begin(), record_nodes.end(), index) != record_nodes.end()) {
				rank = {DecideErrorPhase::SOURCE_CHECK, 1, rank.clause, 0};
			} else if (node.kind == DecideNodeKind::FRAME) {
				rank = {DecideErrorPhase::FRAMES, 1, node.frame, 0};
				if (node.link != DConstants::INVALID_INDEX) {
					// A range frame's helper reads its members' coefficients after every clause.
					evaluated = {DecideErrorPhase::VALUE_LINKS, 0, node.link, 0};
				}
			} else if (aggregate < aggregates.size()) {
				// A reducer over data reading frames is computed once every frame's columns are, in its column's
				// order, before any keyed mapping.
				rank = {DecideErrorPhase::FRAMES, 1, op.frames.size() + aggregate, 0};
			} else if (node.kind == DecideNodeKind::FUNCTION) {
				// F's route and its `when` are the first reads of an ABS instance; its envelope bodies
				// keep their own clause ranks below it.
			} else if (node.defined_var != DConstants::INVALID_INDEX) {
				// Flat resolves each auxiliary's absence in turn, reading its definitions in their order (D88), then
				// evaluates each definition again at its place among the clauses.
				evaluated = {node.compiled.phase, 0, node.compiled.order, 0};
				rank = {DecideErrorPhase::KEYED_MAPPING, definition_order[node.compiled.order], node.compiled.order, 0};
			} else if (node.link != DConstants::INVALID_INDEX) {
				// Flat reads a helper's rows (its path's `when`s, its reducer's, its members' NULL tests and its `by`
				// key) where it builds its classes, and its members' coefficients after every clause. The nodes holding
				// its path's `when`s read them there too (DecideNode::when_rank), and a reader's sigma leaves the key's
				// errors to it (DecideDescent::Signature).
				evaluated = {DecideErrorPhase::VALUE_LINKS, 0, node.link, 0};
				const auto scope = op.variable_scopes[node.value_var].keyed_scope_idx;
				rank = {DecideErrorPhase::KEYED_MAPPING, scope_order[scope], node.link, 0};
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
			node.clause_rank = evaluated.phase == DecideErrorPhase::INVALID ? rank : evaluated;
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
			} else if (ReadsWhenInComposedFilters(node)) {
				// Its `when` is the first part of its first composed term's filter, read at the terms' rank, window by
				// window, before any term's own filter, which ranks after it (DecideNode::filter_group).
				node.route_rank = tree->nodes[node.compiled.composed[0]]->route_rank;
				node.filter_group = 0;
			}
		}
		WhenRanks();
	}

	//! A gate a helper's G or a frame's O inherits is read where that node's rows are read: where the helper's classes
	//! are built and where every frame is built, which may come before the clause holding it (language/guide.md §7.3).
	//! Its holder reads it at the earliest of those ranks and its own (DecideNode::when_rank).
	void WhenRanks() {
		for (auto &node : tree->nodes) {
			node->when_rank = node->route_rank;
		}
		for (auto &gate : inherited_gates) {
			auto &holder = *tree->nodes[gate.first];
			auto &reader = tree->nodes[gate.second]->route_rank;
			if (reader < holder.when_rank) {
				holder.when_rank = reader;
			}
		}
	}

	//! Whether flat reads a root's `when` only in its composed terms' filters, in front of each term's own
	//! (ScopeComposedTerms): a composed clause's, and the objective's where flat's objective has no term to carry it
	//! (LogicalDecide::retained_objective_when).
	bool ReadsWhenInComposedFilters(const DecideNode &node) const {
		return !node.compiled.composed.empty() &&
		       (!node.objective || op.retained_objective_when != DConstants::INVALID_INDEX);
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
		// ABS envelopes are optimizer metadata rather than user clauses. They are built in their natural form
		// (`aux >= inner` / `aux >= -inner`), which can leave decisions on both sides of the comparison. Bring
		// these internal rows through the same canonical boundary as ordinary clauses before term extraction.
		// PrepareAbsFunctions has already read the uncanonicalized operand from the envelope metadata, so this
		// rewrite only changes the row's linear form, not the F node's definition.
		idx_t abs_aux = DConstants::INVALID_INDEX;
		const bool is_abs_envelope = TryParseAbsFunctionEnvelopeTag(comparison.GetAlias(), abs_aux);
		idx_t l0_value = DConstants::INVALID_INDEX;
		const bool is_l0_link = TryParseDecideIndexTag(comparison.GetAlias(), L0_INDICATOR_FUNCTION_TAG_PREFIX, l0_value);
		const bool is_definition = HasDecideTag(comparison.GetAlias(), BODY_DEFINITION_TAG);
		idx_t definition_value = DConstants::INVALID_INDEX;
		if (is_definition && (!TryParseDecideIndexTag(comparison.GetAlias(), DEFINES_TAG_PREFIX, definition_value) ||
		                      definition_value >= op.decide_variables.size())) {
			throw InternalException("DECIDE tree: nonlinear body definition has no valid helper variable");
		}
		idx_t l0_function = DConstants::INVALID_INDEX;
		idx_t definition_function = DConstants::INVALID_INDEX;
		unique_ptr<Expression> canonical_envelope;
		const BoundComparisonExpression *row = &comparison;
		if (is_abs_envelope || is_l0_link || is_definition) {
			DecideCanonicalizer canonicalizer(context, op.decide_index, op.variable_scopes, op.keyed_scopes, op.keys,
			                                  op.is_boolean_var);
			canonical_envelope = canonicalizer.CanonicalizeComparison(comparison);
			row = &canonical_envelope->Cast<BoundComparisonExpression>();
		}
		node.facts = ReadDecideClauseFacts(context, op, *row, scope);
		node.comparison_type = row->type;
		node.folded = HasDecideTag(comparison.GetAlias(), SINGLETON_ROW_BOUND_TAG);
		node.repair_rows = HasDecideTag(comparison.GetAlias(), SINGLETON_ROW_CLAUSE_ROWS_TAG);
		node.minmax_easy = HasDecideTag(comparison.GetAlias(), MINMAX_EASY_REWRITE_TAG);
		node.tightest_bound = HasDecideTag(comparison.GetAlias(), FRAME_TIGHTEST_BOUND_TAG);
		TryParseSourceClauseTag(comparison.GetAlias(), node.source_clause_id);
		node.text = RootText(node.source_clause_id);
		TryParseRemovalGroupTag(comparison.GetAlias(), node.removal_group_id);
		TryParseDecideIndexTag(comparison.GetAlias(), DEFINES_TAG_PREFIX, node.defined_var);
		if (is_l0_link) {
			auto function = std::find_if(tree->function_nodes.begin(), tree->function_nodes.end(), [&](idx_t candidate) {
				return tree->nodes[candidate]->function_kind == DecideFunctionKind::L0_INDICATOR &&
				       tree->nodes[candidate]->value_var == l0_value;
			});
			if (function == tree->function_nodes.end()) {
				throw InternalException("DECIDE tree: L0 link has no indicator function for helper %llu", l0_value);
			}
			l0_function = *function;
			node.l0_function_owner = l0_function;
			if (node.defined_var != DConstants::INVALID_INDEX && node.defined_var != l0_value) {
				throw InternalException("DECIDE tree: L0 link defines a different indicator than its F node");
			}
			node.defined_var = l0_value;
			node.facts->defined_var = l0_value;
			node.facts->kind = ConstraintKind::STRUCTURAL;
		}
		if (is_definition) {
			auto function = std::find_if(tree->function_nodes.begin(), tree->function_nodes.end(), [&](idx_t candidate) {
				return tree->nodes[candidate]->function_kind == DecideFunctionKind::DEFINITION &&
				       tree->nodes[candidate]->value_var == definition_value;
			});
			if (function == tree->function_nodes.end()) {
				throw InternalException("DECIDE tree: body definition has no function node for helper %llu", definition_value);
			}
			definition_function = *function;
			node.definition_function_owner = definition_function;
			node.hidden = true;
			node.text = "definition of " + tree->nodes[definition_function]->text;
			if (node.defined_var != DConstants::INVALID_INDEX && node.defined_var != definition_value) {
				throw InternalException("DECIDE tree: body definition names a different helper than its F node");
			}
			node.defined_var = definition_value;
			node.facts->defined_var = definition_value;
			node.facts->kind = ConstraintKind::STRUCTURAL;
		}
		idx_t signature = DConstants::INVALID_INDEX;
		if (scope.per_wrapper) {
			node.key = scope.PerKey();
			node.per = node.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
			TryParseSignatureTag(scope.per_wrapper->GetAlias(), signature);
		}
		sources.root_of[&comparison] = index;
		if (!is_abs_envelope && !is_l0_link && !is_definition) {
			Gates(index, scope.Condition());
		}
		node.body = Lower(index, *row->left);
		node.rhs = Lower(index, *row->right);
		node.facts->rhs_expr = node.rhs->Copy();
		idx_t abs_function = DConstants::INVALID_INDEX;
		if (is_abs_envelope) {
			if (node.facts->abs_is_pos_bound) {
				abs_function = AbsFunctionNode(node.facts->abs_aux_idx, node.source_clause_id);
			} else {
				for (auto function : tree->function_nodes) {
					if (tree->nodes[function]->value_var == node.facts->abs_aux_idx) {
						abs_function = function;
						break;
					}
				}
				if (abs_function == DConstants::INVALID_INDEX) {
					throw InternalException("DECIDE tree: ABS lower-envelope row has no positive row for helper %llu",
					                        node.facts->abs_aux_idx);
				}
			}
			tree->nodes[abs_function]->abs_envelope_roots.push_back(index);
			tree->nodes[abs_function]->signature.clear();
		} else if (is_l0_link) {
			auto &l0 = *tree->nodes[l0_function];
			l0.l0_link_roots.push_back(index);
			if (l0.l0_link_roots.size() == 1) {
				l0.signature.clear();
			}
		}
		UseSignature(node, signature);
		sources.clause_roots.push_back(index);
		auto end = std::remove_if(node.signature.begin(), node.signature.end(), [&](const DecideSignatureField &field) {
			return field.kind == DecideSignatureFieldKind::DECISION && field.decision == node.defined_var;
		});
		node.signature.erase(end, node.signature.end());
		GroupClass(node);
		if (is_abs_envelope || is_l0_link || is_definition) {
			node.facts->defined_var = node.defined_var;
			if (is_abs_envelope) {
				Edge(abs_function, index);
			} else if (is_l0_link) {
				Edge(l0_function, index);
			} else {
				auto &function = *tree->nodes[definition_function];
				function.definition_roots.push_back(index);
				Edge(definition_function, index);
			}
			// A function's link scope may carry a class signature that splits its instances.
			for (auto &field : node.signature) {
				if (is_abs_envelope) {
					Signature(*tree->nodes[abs_function], CopyField(field));
				} else if (is_l0_link) {
					Signature(*tree->nodes[l0_function], CopyField(field));
				} else {
					Signature(*tree->nodes[definition_function], CopyField(field));
				}
			}
		} else {
			tree->roots.push_back(index);
		}
		if (canonical_envelope) {
			retained_function_links.push_back(std::move(canonical_envelope));
		}
	}

	//! A clause in the grouped shape (the aggregate shape, an easy or a composed extremum) is one model row per group
	//! of its `by` key (language/guide.md §6.4), so a root no `per` made a class root is a class of that key, one for
	//! `by ()`. Its sigma stays what it reads: a frame bound's position still tells a group's anchors apart, and the
	//! group takes their tightest value (D85). A `per K:` keeps its class, which names the classes a row stands for,
	//! and a kept row in none of them still lies in a group, whose row reads it. Flat groups the kept rows by the `by`
	//! key after it computes the terms, on every kept row: the root and its G's of that key read the key there.
	void GroupClass(DecideNode &node) {
		if (!node.facts->lhs_is_aggregate && !node.facts->was_minmax_easy) {
			return;
		}
		if (node.per == DecideTreePer::ROW) {
			node.key = node.facts->group_key;
			node.per = node.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
			node.key_block = DecideBlock::BY_KEY;
		} else {
			node.holds_rows_in_no_class = true;
		}
		for (auto child : node.children) {
			auto &reducer = *tree->nodes[child];
			if (reducer.kind == DecideNodeKind::REDUCER && reducer.key == node.facts->group_key) {
				reducer.key_block = DecideBlock::BY_KEY;
			}
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
		if (tree->nodes[child]->kind == DecideNodeKind::FUNCTION) {
			field.kind = DecideSignatureFieldKind::DECISION;
			field.decision = tree->nodes[child]->value_var;
		} else if (tree->nodes[child]->kind == DecideNodeKind::FRAME) {
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
		idx_t definition_value;
		if (TryParseDecideIndexTag(expression.GetAlias(), DEFINITION_FUNCTION_TAG_PREFIX, definition_value)) {
			auto function = std::find_if(tree->function_nodes.begin(), tree->function_nodes.end(), [&](idx_t index) {
				auto &candidate = *tree->nodes[index];
				return candidate.function_kind == DecideFunctionKind::DEFINITION &&
				       candidate.value_var == definition_value;
			});
			if (function == tree->function_nodes.end()) {
				throw InternalException("DECIDE tree: nonlinear body helper %llu has no function node", definition_value);
			}
			return Read(owner, *function, expression.return_type, expression.GetAlias());
		}
		idx_t product_group;
		if (TryParseDecideIndexTag(expression.GetAlias(), BILINEAR_FUNCTION_GROUP_TAG_PREFIX, product_group)) {
			if (product_group >= op.bilinear_function_groups.size() ||
			    !op.bilinear_function_groups[product_group].expansion) {
				throw InternalException("DECIDE tree: bilinear function group %llu has no expansion", product_group);
			}
			return Lower(owner, *op.bilinear_function_groups[product_group].expansion, carried);
		}
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
				for (auto function : tree->function_nodes) {
					auto &function_node = *tree->nodes[function];
					const bool l0_reader = function_node.function_kind == DecideFunctionKind::L0_INDICATOR &&
					                       node.l0_function_owner == DConstants::INVALID_INDEX;
					const bool definition_reader = function_node.function_kind == DecideFunctionKind::DEFINITION &&
					                               node.definition_function_owner == DConstants::INVALID_INDEX;
					if ((function_node.function_kind == DecideFunctionKind::BOOLEAN_PRODUCT || l0_reader ||
					     definition_reader) &&
					    function_node.value_var == variable) {
						return Read(owner, function, expression.return_type, expression.GetAlias());
					}
				}
				for (idx_t link = 0; link < op.reducer_value_links.size(); link++) {
					if (op.reducer_value_links[link].value_var == variable) {
						auto child = Link(link);
						const auto &read_text = tree->nodes[child]->text;
						return Read(owner, child, expression.return_type,
						            read_text.empty() ? expression.GetAlias() : read_text);
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
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			idx_t abs_aux_idx;
			if (TryParseDecideIndexTag(expression.GetAlias(), ABS_FUNCTION_TAG_PREFIX, abs_aux_idx)) {
				auto function = std::find_if(tree->function_nodes.begin(), tree->function_nodes.end(), [&](idx_t index) {
					return tree->nodes[index]->value_var == abs_aux_idx;
				});
				if (function == tree->function_nodes.end()) {
					throw InternalException("DECIDE tree: ABS helper %llu has no function node", abs_aux_idx);
				}
				// The F node is registered before its readers so nested ABS references can resolve in gates. Lower its
				// operand on the first source-tree read: reducers and nested helpers then join the same tree as their
				// written occurrence, instead of leaving an unresolved aggregate expression in F metadata.
				auto &abs_node = *tree->nodes[*function];
				if (!abs_node.body) {
					auto &abs = expression.Cast<BoundFunctionExpression>();
					if (abs.children.size() != 1) {
						throw InternalException("DECIDE tree: ABS function helper %llu has no operand", abs_aux_idx);
					}
					abs_node.body = Lower(*function, *abs.children[0]);
				}
				return Read(owner, *function, expression.return_type, expression.GetAlias());
			}
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
			tree->frame_aggregate_nodes[frame_aggregate] = index;
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
			// Flat reads the reducer's terms on every row its `when` keeps, and its qualifier keeps no value of a row
			// in no class: such a row still lies in its group.
			entry.holds_rows_in_no_class = true;
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
			Qualified(operand, qualifier);
		}
		return index;
	}

	//! A relation-qualified reducer's operand, once its body is lowered: its instances are its qualifier's classes
	//! split by the values its body reads (D70, D71), the keyed mapping of the qualifier's scope
	//! (DecideModelEmitter::SignatureMapping), whichever reducer it is, a written one, a helper's or a composed term's.
	void Qualified(idx_t operand, idx_t scope) {
		UseSignature(*tree->nodes[operand], op.keyed_scopes[scope].signature);
		tree->qualified_operands[scope] = operand;
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
		// Flat reads a helper's `by` key on every row its path keeps, after its reducer's `when` and its members' NULL
		// tests: a row the key raises on still reaches them.
		node.key_block = DecideBlock::BY_KEY;
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
			Qualified(operand, link.qualifier_scope_idx);
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
		// An anchor whose `within` column is NULL lies in no timeline, so it reads no value (D75): the O holds it in a
		// slot of its own, which no timeline is built from (DecideTreeColumns::ReadFrames).
		node.holds_rows_in_no_class = true;
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
		// Its message names its clause, as its root's does.
		node.source_clause_id = tree->nodes[owner]->source_clause_id;
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
		if (term.qualifier_scope_idx < op.keyed_scopes.size()) {
			Qualified(operand, term.qualifier_scope_idx);
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
	//! Each gate removed is read by the ancestors holding it (inherited_gates).
	void RemoveInheritedPathGates() {
		vector<vector<idx_t>> parents(tree->nodes.size());
		for (idx_t i = 0; i < tree->nodes.size(); i++) {
			for (auto child : tree->nodes[i]->children) {
				parents[child].push_back(i);
			}
		}
		std::function<bool(idx_t, const Expression &, vector<bool> &, vector<idx_t> &)> inherited =
		    [&](idx_t index, const Expression &condition, vector<bool> &visiting, vector<idx_t> &holders) {
			    if (parents[index].empty() || tree->nodes[index]->root || visiting[index]) {
				    return false;
			    }
			    visiting[index] = true;
			    for (auto parent : parents[index]) {
				    bool holds = false;
				    for (auto &gate : tree->nodes[parent]->when) {
					    holds = holds || gate->Equals(condition);
				    }
				    if (holds) {
					    holders.push_back(parent);
				    } else if (!inherited(parent, condition, visiting, holders)) {
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
				vector<idx_t> holders;
				if (!inherited(i, *gate, visiting, holders)) {
					return false;
				}
				for (auto holder : holders) {
					inherited_gates.emplace_back(holder, i);
				}
				return true;
			});
			node.when.erase(end, node.when.end());
		}
	}

	void Definitions() {
		auto &definitions = tree->definition_nodes;
		for (idx_t i = 0; i < tree->nodes.size(); i++) {
			auto &node = *tree->nodes[i];
			auto variable = node.defined_var;
			const bool abs_envelope = node.facts && node.facts->abs_aux_idx != DConstants::INVALID_INDEX;
			const bool function_link = node.l0_function_owner != DConstants::INVALID_INDEX ||
			                           node.definition_function_owner != DConstants::INVALID_INDEX;
			if (variable < definitions.size() && !abs_envelope && !function_link) {
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

void DecideTreeConstruction::TakeClause(const Expression &leaf, unique_ptr<DecideConstraint> clause,
                                        bool singleton_row) {
	builder->TakeClause(leaf, std::move(clause), singleton_row);
}

void DecideTreeConstruction::TakeObjective(unique_ptr<DecideObjective> objective) {
	builder->TakeObjective(std::move(objective));
}

void DecideTreeConstruction::TakeComposedTerms(idx_t spec, idx_t term, vector<DecideTerm> terms) {
	builder->TakeComposedTerms(spec, term, std::move(terms));
}

void DecideTreeConstruction::TakeLinkTerms(idx_t link, vector<DecideTerm> members, vector<DecideTerm> fallback) {
	builder->TakeLinkTerms(link, std::move(members), std::move(fallback));
}

void DecideTreeConstruction::TakeFrameReads(DecideFrameReads frame_reads) {
	builder->TakeFrameReads(std::move(frame_reads));
}

const DecideTree &DecideTreeConstruction::Tree() const {
	return builder->Tree();
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
		if (node.kind == DecideNodeKind::FUNCTION) {
			const bool common_invalid =
			    (node.function_kind != DecideFunctionKind::ABS &&
			     node.function_kind != DecideFunctionKind::BOOLEAN_PRODUCT &&
			     node.function_kind != DecideFunctionKind::L0_INDICATOR &&
			     node.function_kind != DecideFunctionKind::DEFINITION) ||
			    node.value_var >= value_nodes.size() ||
			    (!variable_scopes.empty() && node.value_var >= variable_scopes.size()) ||
			    node.defined_var != node.value_var || !node.body;
			const bool abs_invalid = node.function_kind == DecideFunctionKind::ABS &&
			                         (node.product_bool_var != DConstants::INVALID_INDEX ||
			                          node.product_other_var != DConstants::INVALID_INDEX ||
			                          (node.abs_indicator_var != DConstants::INVALID_INDEX &&
			                           node.abs_indicator_var >= value_nodes.size()) ||
			                          (node.abs_indicator_var != DConstants::INVALID_INDEX && !node.abs_needs_bigm));
			const bool product_invalid = node.function_kind == DecideFunctionKind::BOOLEAN_PRODUCT &&
			                             (node.product_bool_var >= value_nodes.size() ||
			                              node.product_other_var >= value_nodes.size() ||
			                              node.abs_needs_bigm ||
			                              node.abs_indicator_var != DConstants::INVALID_INDEX ||
			                              !node.abs_envelope_roots.empty());
			const bool l0_invalid = node.function_kind == DecideFunctionKind::L0_INDICATOR &&
			                        (node.product_bool_var != DConstants::INVALID_INDEX ||
			                         node.product_other_var != DConstants::INVALID_INDEX || node.abs_needs_bigm ||
			                         node.abs_indicator_var != DConstants::INVALID_INDEX ||
			                         !node.abs_envelope_roots.empty() || node.l0_link_roots.size() != 3 ||
			                         !(node.l0_m > 0.0));
			const bool definition_invalid = node.function_kind == DecideFunctionKind::DEFINITION &&
			                               (node.product_bool_var != DConstants::INVALID_INDEX ||
			                                node.product_other_var != DConstants::INVALID_INDEX || node.abs_needs_bigm ||
			                                node.abs_indicator_var != DConstants::INVALID_INDEX ||
			                                !node.abs_envelope_roots.empty() || !node.l0_link_roots.empty() ||
			                                node.definition_roots.size() != 1 ||
			                                node.definition_relation == DecideDefinitionRelation::INVALID);
			if (common_invalid || abs_invalid || product_invalid || l0_invalid || definition_invalid) {
				throw InternalException("DECIDE tree has an invalid function node %llu", i);
			}
		}
		if (node.kind == DecideNodeKind::FUNCTION && node.function_kind == DecideFunctionKind::ABS) {
			bool positive = false;
			bool negative = false;
			for (auto root : node.abs_envelope_roots) {
				if (root >= nodes.size() || !nodes[root]->compiled.clause ||
				    nodes[root]->compiled.clause->abs_aux_idx != node.value_var || nodes[root]->parent != i ||
				    std::find(node.children.begin(), node.children.end(), root) == node.children.end()) {
					throw InternalException("DECIDE tree ABS function node %llu has an invalid envelope row", i);
				}
				if (nodes[root]->compiled.clause->abs_is_pos_bound) {
					positive = true;
				} else {
					negative = true;
				}
			}
			if (!positive || !negative) {
				throw InternalException("DECIDE tree ABS function node %llu does not own both envelope rows", i);
			}
		}
		if (node.kind == DecideNodeKind::FUNCTION && node.function_kind == DecideFunctionKind::L0_INDICATOR) {
			for (auto root : node.l0_link_roots) {
				if (root >= nodes.size() || !nodes[root]->compiled.clause ||
				    nodes[root]->compiled.clause->defined_var != node.value_var ||
				    nodes[root]->l0_function_owner != i || nodes[root]->parent != i ||
				    std::find(node.children.begin(), node.children.end(), root) == node.children.end()) {
					throw InternalException("DECIDE tree L0 indicator function node %llu has an invalid link row", i);
				}
			}
		}
		if (node.kind == DecideNodeKind::FUNCTION && node.function_kind == DecideFunctionKind::DEFINITION) {
			for (auto root : node.definition_roots) {
				if (root >= nodes.size() || !nodes[root]->compiled.clause ||
				    nodes[root]->compiled.clause->defined_var != node.value_var ||
				    nodes[root]->definition_function_owner != i || nodes[root]->parent != i ||
				    std::find(node.children.begin(), node.children.end(), root) == node.children.end()) {
					throw InternalException("DECIDE tree definition function node %llu has an invalid definition row", i);
				}
			}
		}
		if (node.kind == DecideNodeKind::FUNCTION) {
			DecideVarScopeInfo scope;
			if (!variable_scopes.empty()) {
				scope = variable_scopes[node.value_var];
			}
			const bool keyed = scope.IsKeyed() ||
			                   (scope.scope == DecideVarScope::INLINED && scope.keyed_scope_idx < keyed_scopes.size());
			const auto expected_per = scope.IsGlobal() ? DecideTreePer::ONE
			                         : keyed             ? DecideTreePer::KEY
			                                            : DecideTreePer::ROW;
			const auto expected_key = keyed ? keyed_scopes[scope.keyed_scope_idx].key : DConstants::INVALID_INDEX;
			if (node.per != expected_per || node.key != expected_key) {
				throw InternalException("DECIDE tree function node %llu does not match its helper's scope", i);
			}
		}
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
		// The descent reads of a row its node's own `when` parts, keys and bare source columns alone, each key once
		// (language/guide.md §6.1): a carried column, a program column and a signature's value are each a source
		// column.
		auto source = [&](const Expression &column) {
			return column.GetExpressionClass() == ExpressionClass::BOUND_REF &&
			       column.Cast<BoundReferenceExpression>().index < input_column_count;
		};
		for (auto &column : node.columns) {
			if (!source(*column)) {
				throw InternalException("DECIDE tree entry carries a non-source column");
			}
		}
		for (auto &column : node.program_columns) {
			if (!source(*column)) {
				throw InternalException("DECIDE tree node %llu's program reads a non-source column", i);
			}
		}
		for (auto &field : node.signature) {
			if (field.kind == DecideSignatureFieldKind::VALUE && field.value && !source(*field.value)) {
				throw InternalException("DECIDE tree node %llu's signature reads a non-source value", i);
			}
		}
		for (auto &read : node.child_reads) {
			if (read.node >= nodes.size() || read.column != input_column_count + read.node) {
				throw InternalException("DECIDE tree body has an invalid child result");
			}
		}
		// A program reads its node's layout, then the results of its arguments, and no column of a plan. A hole reads a
		// G over data, or a frame's column of its O.
		for (auto &hole : node.holes) {
			const bool frame = hole.column != DConstants::INVALID_INDEX;
			if (hole.node >= nodes.size() ||
			    (frame ? hole.column >= frame_columns.size() || nodes[hole.node]->kind != DecideNodeKind::FRAME
			           : nodes[hole.node]->kind != DecideNodeKind::REDUCER || nodes[hole.node]->decision_value)) {
				throw InternalException("DECIDE tree node %llu reads a hole of no data reducer or frame", i);
			}
		}
		for (auto &program : node.programs) {
			const auto width =
			    node.columns.size() + node.program_columns.size() + node.holes.size() + program.arguments.size();
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
	vector<bool> listed_functions(nodes.size(), false);
	for (auto function : function_nodes) {
		if (function >= nodes.size() || nodes[function]->kind != DecideNodeKind::FUNCTION || listed_functions[function]) {
			throw InternalException("DECIDE tree has an invalid or repeated function node");
		}
		listed_functions[function] = true;
	}
	for (idx_t i = 0; i < nodes.size(); i++) {
		if ((nodes[i]->kind == DecideNodeKind::FUNCTION) != listed_functions[i]) {
			throw InternalException("DECIDE tree function-node index is incomplete");
		}
	}
	for (auto root : roots) {
		if (root >= nodes.size() || !nodes[root]->root ||
		    (nodes[root]->kind != DecideNodeKind::SCOPE && nodes[root]->kind != DecideNodeKind::FUNCTION)) {
			throw InternalException("DECIDE tree has an invalid root");
		}
	}
}

string DecideTree::ToString() const {
	std::ostringstream result;
	for (idx_t i = 0; i < nodes.size(); i++) {
		auto &node = *nodes[i];
		result << i << " "
		       << (node.kind == DecideNodeKind::SCOPE       ? "E"
		           : node.kind == DecideNodeKind::REDUCER   ? "G"
		           : node.kind == DecideNodeKind::FRAME     ? "O"
		                                                   : "F");
		if (node.root) {
			result << (node.objective ? " objective" : node.hidden ? " definition" : " root");
		}
		result << " key=" << DecideKeyText(keys, node.key);
		if (!node.aggregate.empty()) {
			result << " agg=" << node.aggregate;
		}
		if (node.kind == DecideNodeKind::FUNCTION) {
			if (node.function_kind == DecideFunctionKind::ABS) {
				result << " function=abs value=x" << node.value_var << " body=(" << node.body->ToString() << ")"
				       << (node.abs_needs_bigm ? " exact=big-m" : " exact=pressure-or-bound");
				if (node.abs_indicator_var != DConstants::INVALID_INDEX) {
					result << " indicator=x" << node.abs_indicator_var;
				}
			} else if (node.function_kind == DecideFunctionKind::BOOLEAN_PRODUCT) {
				result << " function=boolean_product value=x" << node.value_var << " body=("
				       << node.body->ToString() << ")";
			} else if (node.function_kind == DecideFunctionKind::L0_INDICATOR) {
				result << " function=l0_indicator value=x" << node.value_var << " body=(" << node.body->ToString() << ")"
				       << (node.l0_auto_m ? " M=auto" : " M=" + std::to_string(node.l0_m));
			} else if (node.function_kind == DecideFunctionKind::DEFINITION) {
				const auto relation = node.definition_relation == DecideDefinitionRelation::AT_LEAST ? "at-least"
				                     : node.definition_relation == DecideDefinitionRelation::AT_MOST ? "at-most"
				                     : node.definition_relation == DecideDefinitionRelation::EQUAL ? "equal"
				                                                                            : "invalid";
				result << " function=definition value=x" << node.value_var << " relation=" << relation << " body=("
				       << node.body->ToString() << ")";
			}
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
