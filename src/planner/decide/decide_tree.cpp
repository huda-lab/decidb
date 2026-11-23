#include "duckdb/planner/decide/decide_tree.hpp"

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_linear_form.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_decide_null_test_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <sstream>

namespace duckdb {

//! The shape, provenance and bound-admission facts of a clause, read once. `clause` is canonical: a clause of SUCH
//! THAT, or a function-owned row as DecideCanonicalizer::CanonicalizeFunctionRow returns it.
static unique_ptr<DecideConstraint> ReadDecideClauseFacts(ClientContext &context, const LogicalDecide &op,
                                                          const BoundDecideClause &clause) {
	auto &comparison = clause.body->Cast<BoundComparisonExpression>();
	auto facts = make_uniq<DecideConstraint>();
	facts->comparison_type = comparison.type;
	facts->rhs_expr = comparison.right->Copy();
	facts->query_wide_bound = clause.query_wide_bound;
	facts->bound_holds_subquery = DecideHoldsSubqueryValue(op.bound.computed_values, *comparison.right);
	facts->tightest_frame_bound = clause.tightest_frame_bound;
	facts->origin = clause.origin;
	if (comparison.type == ExpressionType::COMPARE_NOTEQUAL) {
		facts->kind = ConstraintKind::USER_MECHANISM;
	}
	if (clause.structural) {
		facts->kind = ConstraintKind::STRUCTURAL;
	}
	facts->defined_var = clause.defines;
	facts->helper_row = clause.helper_row;
	facts->was_minmax_easy = clause.minmax_easy;
	facts->scope = clause.scope.Copy();
	facts->group_key = clause.group_key;
	DecideCanonicalizer canonicalizer(context, op.decide_index, op.bound);
	const auto shape = clause.shape;
	if (shape == CanonicalConstraintClass::INVALID) {
		throw InternalException("DECIDE constraint reached term extraction with no shape: '%s'", comparison.ToString());
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
	// A clause no `per` made a class root is one row per kept row, but its rows are the same row wherever they agree on
	// what it reads: one that reads no per-row decision (a query-wide or a keyed one is the same on the rows of one
	// instance) is one class, as `per ()` is, whose instances are its distinct sigmas (language/guide.md §6.2). One
	// that reads a per-row decision differs on every row and keeps its path.
	if (facts->scope.per == DecideClauseScope::PerRole::NONE && facts->kind == ConstraintKind::USER_PARAMETER &&
	    facts->defined_var == DConstants::INVALID_INDEX && !facts->was_minmax_easy && !facts->lhs_is_aggregate) {
		vector<DecideSignatureField> fields;
		canonicalizer.CollectSignatureFields(comparison, DConstants::INVALID_INDEX, fields);
		bool per_row = false;
		for (auto &field : fields) {
			if (field.kind != DecideSignatureFieldKind::DECISION) {
				continue;
			}
			const auto &variable = field.decision < op.bound.variable_scopes.size()
			                           ? op.bound.variable_scopes[field.decision]
			                           : DecideVarScopeInfo();
			const bool keyed = variable.IsKeyed() || (variable.scope == DecideVarScope::INLINED &&
			                                          variable.keyed_scope_idx < op.bound.keyed_scopes.size());
			per_row = per_row || (!variable.IsGlobal() && !keyed);
		}
		if (!per_row) {
			facts->scope.per = DecideClauseScope::PerRole::ONE_ROW_PER_CLASS;
		}
	}
	DecideSingletonRow bound;
	if (!clause.folded_bound && !facts->lhs_is_aggregate &&
	    DecideReadSingletonRow(context, op, comparison, clause.minmax_easy, bound)) {
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

class DecideTreeBuilder {
public:
	DecideTreeBuilder(ClientContext &context, LogicalDecide &op)
	    : context(context), op(op), tree(make_uniq<DecideTree>()), terms(context, op, *tree, reducer_nodes) {
		tree->written_sense = op.bound.sense;
		tree->variable_scopes = op.bound.variable_scopes;
		tree->keyed_scopes = op.bound.keyed_scopes;
		tree->value_nodes.assign(op.bound.variables.size(), DConstants::INVALID_INDEX);
		tree->definition_nodes.resize(op.bound.variables.size());
		tree->qualified_operands.assign(op.bound.keyed_scopes.size(), DConstants::INVALID_INDEX);
		tree->singleton_row_boxes = op.singleton_row_boxes;
		tree->user_singleton_row_bounds = op.user_singleton_row_bounds;
		tree->signed_helper_vars = op.signed_helper_vars;
		if (!op.children.empty()) {
			bindings = op.children[0]->GetColumnBindings();
		}
		tree->input_column_count = bindings.size();
		for (auto &key : op.bound.keys) {
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
		definition_nodes.assign(op.helper_definitions.size(), DConstants::INVALID_INDEX);
		frame_nodes.assign(op.bound.frames.size(), DConstants::INVALID_INDEX);
		record_nodes.assign(op.bound.bound_aggregates.size(), DConstants::INVALID_INDEX);
		tree->frame_aggregate_nodes.assign(op.frame_aggregates.size(), DConstants::INVALID_INDEX);
	}

	//! The nodes, their edges, sigma, gates and definitions, and each root's records.
	void Structure() {
		RegisterHelperDefinitions();
		LowerHelperDefinitions();
		for (auto &check : op.value_checks) {
			auto index = NewNode(DecideNodeKind::SCOPE);
			auto &node = *tree->nodes[index];
			node.root = node.hidden = node.value_check = true;
			node.text = "data value check";
			node.body = Lower(index, *check);
			tree->roots.push_back(index);
		}
		for (auto &clause : op.bound.clauses) {
			if (clause.body->GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
				Root(clause);
			} else if (clause.helper_row == DecideHelperRow::DEFINITION) {
				// The row stands beneath its F node, at the placeholder's clause position.
				auto definition = std::find_if(op.helper_definitions.begin(), op.helper_definitions.end(),
				                               [&](const unique_ptr<DecideHelperDefinition> &candidate) {
					                               return candidate->kind == DecideHelperKind::DEFINITION &&
					                                      candidate->helper == clause.defines;
				                               });
				if (definition == op.helper_definitions.end()) {
					throw InternalException("DECIDE tree: body-definition placeholder has no F node");
				}
				Root(MakeDefinitionRow(**definition), NumericCast<idx_t>(definition - op.helper_definitions.begin()));
			}
		}
		// The rows a helper definition owns follow every written constraint, in the order the definitions were made
		// (an indicator's by RewriteNorm, before every ABS), as children of their F nodes: an L0 indicator's three
		// links and an ABS's two envelope rows.
		for (idx_t i = 0; i < op.helper_definitions.size(); i++) {
			auto &definition = *op.helper_definitions[i];
			for (auto &link : definition.links) {
				Root(link, i);
			}
			if (definition.kind == DecideHelperKind::ABS) {
				Root(decide_rewrite::MakeAbsRow(context, op, definition, false), i);
				Root(decide_rewrite::MakeAbsRow(context, op, definition, true), i);
			}
		}
		if (op.bound.objective.body) {
			const Expression *body = op.bound.objective.body.get();
			const Expression *condition = op.bound.objective.scope.when.get();
			auto index = NewNode(DecideNodeKind::SCOPE);
			auto &node = *tree->nodes[index];
			reducer_nodes.clear();
			node.root = node.objective = true;
			node.per = DecideTreePer::ONE;
			node.text = op.bound.written_objective.empty() ? Text(*body) : op.bound.written_objective;
			Gates(index, condition);
			node.body = Lower(index, *body);
			UseSignature(node, op.bound.objective.scope.signature);
			node.compiled.objective = terms.Objective(op.bound.objective);
			tree->roots.push_back(index);
		}
		// A helper or data check can outlive the reading clause (D88). Preserve
		// that definition as an explicit root rather than relying on registry order.
		for (idx_t i = 0; i < op.helper_definitions.size(); i++) {
			if (op.helper_definitions[i]->kind == DecideHelperKind::REDUCER_VALUE &&
			    definition_nodes[i] == DConstants::INVALID_INDEX) {
				AddOrphan(Link(i));
			}
		}
		for (idx_t i = 0; i < op.bound.bound_aggregates.size(); i++) {
			if (record_nodes[i] == DConstants::INVALID_INDEX) {
				AddOrphan(Record(i));
			}
		}
		// Each helper's G or O holds its reducer's members, split after the objective's terms.
		for (idx_t i = 0; i < op.helper_definitions.size(); i++) {
			if (op.helper_definitions[i]->kind == DecideHelperKind::REDUCER_VALUE) {
				terms.Members(*op.helper_definitions[i], tree->nodes[definition_nodes[i]]->compiled);
			}
		}
		terms.Frames(*tree);
		RemoveInheritedPathGates();
		Definitions();
	}

	//! Gives each helper definition its F node, in the order made, before any body is lowered (declare, then define):
	//! a body reads the F node of whichever helper it names, an ABS or a product inside a nonlinear reducer body as
	//! much as a definition inside an ABS. A PRODUCT is read where it is kept and a REDUCER_VALUE gets its G where it
	//! is first read (Link); neither has an F node.
	void RegisterHelperDefinitions() {
		for (idx_t i = 0; i < op.helper_definitions.size(); i++) {
			auto &definition = *op.helper_definitions[i];
			switch (definition.kind) {
			case DecideHelperKind::ABS: {
				// The optimizer records the ABS; the tree builds its two rows beneath the F node (Structure), as it
				// does a definition's.
				FunctionNode(i).origin.source_clause_id = definition.origin.source_clause_id;
				break;
			}
			case DecideHelperKind::BOOLEAN_PRODUCT: {
				// The function's body names its two factors; the tree-owned BilinearLinkSpec records are handed to
				// stage 06 by way of these nodes.
				FunctionNode(i).origin = definition.origin;
				break;
			}
			case DecideHelperKind::L0_INDICATOR: {
				if (definition.links.size() != 3) {
					throw InternalException("DECIDE tree: malformed L0 indicator function");
				}
				auto &node = FunctionNode(i);
				node.text = definition.label;
				node.origin = definition.links[0].origin;
				break;
			}
			case DecideHelperKind::DEFINITION: {
				FunctionNode(i);
				Gates(definition_nodes[i], definition.when.get());
				break;
			}
			case DecideHelperKind::PRODUCT:
			case DecideHelperKind::REDUCER_VALUE:
				break;
			}
		}
	}

	//! Lowers each F node's body and the rows it is read on, once every F node exists.
	void LowerHelperDefinitions() {
		for (idx_t i = 0; i < op.helper_definitions.size(); i++) {
			auto &definition = *op.helper_definitions[i];
			const auto index = definition_nodes[i];
			switch (definition.kind) {
			case DecideHelperKind::ABS:
				tree->nodes[index]->body = Lower(index, *definition.body);
				Gates(index, definition.when.get());
				break;
			case DecideHelperKind::BOOLEAN_PRODUCT: {
				auto &node = *tree->nodes[index];
				vector<unique_ptr<Expression>> children;
				children.push_back(op.bound.variables[definition.bool_factor]->Copy());
				children.push_back(op.bound.variables[definition.other_factor]->Copy());
				auto body = DecideRebindOperator(context, "*", std::move(children));
				node.body = Lower(index, *body);
				break;
			}
			case DecideHelperKind::L0_INDICATOR: {
				// The first link holds the counted expression and the rows it is read on.
				auto &first_link = definition.links[0];
				Gates(index, first_link.scope.when.get());
				tree->nodes[index]->body = Lower(index, *first_link.body->Cast<BoundComparisonExpression>().right);
				break;
			}
			case DecideHelperKind::DEFINITION:
				tree->nodes[index]->body = Lower(index, *definition.body);
				break;
			case DecideHelperKind::PRODUCT:
			case DecideHelperKind::REDUCER_VALUE:
				break;
			}
		}
	}

	//! The row that defines a definition's helper from its body, on the rows its condition keeps, one per instance of
	//! the helper.
	BoundDecideClause MakeDefinitionRow(const DecideHelperDefinition &definition) const {
		const auto &body = *definition.body;
		auto left = BoundCastExpression::AddCastToType(context, op.bound.variables[definition.helper]->Copy(),
		                                               body.return_type);
		BoundDecideClause row;
		row.body = make_uniq<BoundComparisonExpression>(definition.relation, std::move(left), body.Copy());
		row.structural = true;
		row.defines = definition.helper;
		row.helper_row = DecideHelperRow::DEFINITION;
		row.origin = definition.origin;
		if (definition.helper < op.bound.variable_scopes.size()) {
			auto &scope = op.bound.variable_scopes[definition.helper];
			if (scope.IsGlobal() || scope.IsKeyed()) {
				row.scope = DecideClauseScope::Per(scope.IsKeyed() ? op.bound.keyed_scopes[scope.keyed_scope_idx].key
				                                                   : DConstants::INVALID_INDEX);
			}
		}
		row.scope.when = definition.when ? definition.when->Copy() : nullptr;
		return row;
	}

	//! The F node of helper definition `definition_index`: a hidden root holding the helper's value, one per instance
	//! of the helper's scope, named as a plan names the helper.
	//! Whether `node` is a row the F node of a definition of `kind` owns (DecideTree::OwnedBy, before the tree holds
	//! the definitions).
	bool OwnedByFunction(const DecideNode &node, DecideHelperKind kind) const {
		if (node.parent == DConstants::INVALID_INDEX) {
			return false;
		}
		auto &parent = *tree->nodes[node.parent];
		return parent.kind == DecideNodeKind::FUNCTION && op.helper_definitions[parent.definition]->kind == kind;
	}

	DecideNode &FunctionNode(idx_t definition_index) {
		const auto helper = op.helper_definitions[definition_index]->helper;
		auto label = std::find_if(op.aux_var_expressions.begin(), op.aux_var_expressions.end(),
		                          [&](const pair<idx_t, string> &entry) { return entry.first == helper; });
		if (helper >= tree->variable_scopes.size() || label == op.aux_var_expressions.end()) {
			throw InternalException("DECIDE tree: helper %llu has no scope or no label", helper);
		}
		auto index = NewNode(DecideNodeKind::FUNCTION);
		definition_nodes[definition_index] = index;
		auto &node = *tree->nodes[index];
		node.definition = definition_index;
		node.root = node.hidden = node.decision_value = true;
		node.defined_var = helper;
		node.text = label->second;
		auto &variable_scope = tree->variable_scopes[helper];
		if (variable_scope.IsKeyed() || variable_scope.scope == DecideVarScope::INLINED) {
			if (variable_scope.keyed_scope_idx >= op.bound.keyed_scopes.size()) {
				if (variable_scope.scope != DecideVarScope::INLINED ||
				    variable_scope.keyed_scope_idx != DConstants::INVALID_INDEX) {
					throw InternalException("DECIDE tree: helper %llu has an invalid keyed scope", helper);
				}
			} else {
				node.per = DecideTreePer::KEY;
				node.key = op.bound.keyed_scopes[variable_scope.keyed_scope_idx].key;
			}
		} else if (variable_scope.IsGlobal()) {
			node.per = DecideTreePer::ONE;
		}
		tree->function_nodes.push_back(index);
		tree->roots.push_back(index);
		return node;
	}

	//! The bodies from the records, then the order they read.
	unique_ptr<DecideTree> Finish() {
		tree->frame_nodes = frame_nodes;
		CompileDecideTree(context, op, *tree, definition_nodes, record_nodes);
		PostOrder();
		tree->definitions = std::move(op.helper_definitions);
		tree->Validate();
		return std::move(tree);
	}

	const DecideTree &Tree() const {
		return *tree;
	}

private:
	ClientContext &context;
	LogicalDecide &op;
	unique_ptr<DecideTree> tree;
	vector<ColumnBinding> bindings;
	//! Each helper definition's node: its F node, or a REDUCER_VALUE's G or O once it is read (Link). INVALID_INDEX
	//! for a PRODUCT.
	vector<idx_t> definition_nodes;
	vector<idx_t> frame_nodes;
	vector<idx_t> record_nodes;
	//! The G each reducer of the clause in hand became, by its address in that clause: cleared at each root, read by
	//! the term half as it splits the same clause.
	DecideReducerNodes reducer_nodes;
	//! The term half of the walk: stage 05's split of each canonical clause into its root's records.
	DecideTermLowering terms;

	string Text(const Expression &expression) const {
		return RenderDecideSource(expression,
		                          DecidePlanNames(op.bound.source_text, op.bound.keyed_scopes, op.bound.keys,
		                                          op.decide_index, op.aux_var_expressions, op.bound.frames));
	}

	//! How a message names a root, as it names a row (DecideWrittenClause): by the clause it comes from as the user
	//! wrote it (QuoteWrittenClause), and a row no clause claims, which a rewrite added for the objective, by the
	//! objective.
	string RootText(idx_t source) const {
		if (source < op.bound.constraint_sources.size()) {
			return QuoteWrittenClause(op.bound.constraint_sources, source, op.bound.source_text);
		}
		return DecideWrittenObjective(op.bound.sense, op.bound.written_objective);
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
		if (condition->GetExpressionClass() == ExpressionClass::BOUND_DECIDE_NULL_TEST) {
			auto &test = condition->Cast<BoundDecideNullTestExpression>();
			// A test the statistics proved true (StatisticsPropagator::FoldSkipNullTests) keeps every entry: it is no
			// member test, so no program is compiled for it and none runs per entry.
			if (test.KeepsEveryRow()) {
				return;
			}
			// An O container gathers positions before any reader's generated
			// presence test runs. Testing that same frame here would make it
			// depend on its own not-yet-computed result.
			if (node.kind == DecideNodeKind::FRAME) {
				return;
			}
			node.member_tests.push_back(Lower(owner, *test.test, true));
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
		if (signature >= op.bound.signatures.size()) {
			throw InternalException("DECIDE tree has invalid signature %llu", signature);
		}
		node.signature.clear();
		for (auto &field : op.bound.signatures[signature].fields) {
			auto copy = CopyField(field);
			if (copy.value) {
				auto record = FindRecord(*copy.value);
				if (record != DConstants::INVALID_INDEX) {
					copy.kind = DecideSignatureFieldKind::BY_KEY;
					copy.key = op.bound.bound_aggregates[record].key;
					copy.value.reset();
				} else {
					copy.value = Resolve(*copy.value);
				}
			}
			if (copy.kind == DecideSignatureFieldKind::DECISION) {
				for (auto &link : op.helper_definitions) {
					if (link->kind == DecideHelperKind::REDUCER_VALUE && copy.decision == link->helper) {
						copy.decision = DConstants::INVALID_INDEX;
						const idx_t frame = link->Frame();
						if (frame == DConstants::INVALID_INDEX) {
							copy.kind = DecideSignatureFieldKind::BY_KEY;
							copy.key = link->path_key;
						} else {
							copy.kind = DecideSignatureFieldKind::FRAME;
							copy.frame = frame;
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

	//! `owner` is the helper definition whose F node a row a function owns stands beneath.
	void Root(const BoundDecideClause &clause, idx_t owner = DConstants::INVALID_INDEX) {
		auto &scope = clause.scope;
		auto index = NewNode(DecideNodeKind::SCOPE);
		auto &node = *tree->nodes[index];
		node.root = true;
		reducer_nodes.clear();
		// ABS envelopes are built from the optimizer's record rather than written by the user. They are in their
		// natural form (`aux >= inner` / `aux >= -inner`), which can leave decisions on both sides of the comparison.
		// Bring these internal rows through the same canonical boundary as ordinary clauses before term extraction.
		// The F node reads the uncanonicalized operand from the record (DecideHelperDefinition::body), so this
		// rewrite only changes the row's linear form, not the F node's definition.
		const bool is_abs_envelope = IsDecideAbsEnvelope(clause.helper_row);
		const bool is_l0_link =
		    clause.helper_row == DecideHelperRow::L0_BOUND || clause.helper_row == DecideHelperRow::L0_TOLERANCE;
		const bool is_definition = clause.helper_row == DecideHelperRow::DEFINITION;
		const bool owned = is_abs_envelope || is_l0_link || is_definition;
		D_ASSERT(!owned || owner != DConstants::INVALID_INDEX);
		const idx_t function = owned ? definition_nodes[owner] : DConstants::INVALID_INDEX;
		BoundDecideClause canonical_envelope;
		const BoundDecideClause *canonical = &clause;
		if (owned) {
			DecideCanonicalizer canonicalizer(context, op.decide_index, op.bound);
			canonical_envelope = canonicalizer.CanonicalizeFunctionRow(clause);
			canonical = &canonical_envelope;
		}
		const auto *row = &canonical->body->Cast<BoundComparisonExpression>();
		auto facts = ReadDecideClauseFacts(context, op, *canonical);
		node.folded = clause.folded_bound;
		node.origin = clause.origin;
		node.text = RootText(node.origin.source_clause_id);
		node.defined_var = clause.defines;
		if (owned) {
			// Before the row is lowered: a row a function owns reads its helper as a decision (Lower).
			Edge(function, index);
		}
		if (is_definition) {
			node.hidden = true;
			node.text = "definition of " + tree->nodes[function]->text;
		}
		idx_t signature = DConstants::INVALID_INDEX;
		if (scope.HasPer()) {
			node.key = scope.class_key;
			node.per = node.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
			signature = scope.signature;
		}
		if (!is_abs_envelope && !is_l0_link && !is_definition) {
			Gates(index, scope.when.get());
		}
		node.body = Lower(index, *row->left);
		node.rhs = Lower(index, *row->right, canonical->query_wide_bound);
		if (owned) {
			auto &owner_node = *tree->nodes[function];
			owner_node.rows.push_back(index);
			if (is_abs_envelope || (is_l0_link && owner_node.rows.size() == 1)) {
				owner_node.signature.clear();
			}
		}
		UseSignature(node, signature);
		if (scope.HasPer() && node.key != DConstants::INVALID_INDEX && facts->kind == ConstraintKind::USER_PARAMETER &&
		    facts->defined_var == DConstants::INVALID_INDEX &&
		    facts->scope.per == DecideClauseScope::PerRole::ONE_ROW_PER_CLASS) {
			DecideCanonicalizer canonicalizer(context, op.decide_index, op.bound);
			node.merges_classes = canonicalizer.MergesClasses(*row, node.key);
		}
		if (!scope.HasPer() && facts->scope.per == DecideClauseScope::PerRole::ONE_ROW_PER_CLASS) {
			// Its sigma stays what it reads (UseSignature), whose distinct values are its instances.
			node.per = DecideTreePer::ONE;
		}
		auto end = std::remove_if(node.signature.begin(), node.signature.end(), [&](const DecideSignatureField &field) {
			return field.kind == DecideSignatureFieldKind::DECISION && field.decision == node.defined_var;
		});
		node.signature.erase(end, node.signature.end());
		GroupClass(node, *facts);
		if (owned) {
			// A function's link scope may carry a class signature that splits its instances.
			for (auto &field : node.signature) {
				Signature(*tree->nodes[function], CopyField(field));
			}
		} else {
			tree->roots.push_back(index);
		}
		// A folded bound holds a clause only when its repair rows come from it, for the repair model alone: emitting
		// one for a bound already in the column box would add a redundant model row per input row.
		if (!clause.folded_bound || clause.folded_clause_rows) {
			terms.Clause(*canonical, *facts);
			node.compiled.clause = std::move(facts);
		}
	}

	//! A clause in the grouped shape (the aggregate shape, an easy extremum) is one model row per group
	//! of its `by` key (language/guide.md §6.4), so a root no `per` made a class root is a class of that key, one for
	//! `by ()`. Its sigma stays what it reads: a frame bound's position still tells a group's anchors apart, and the
	//! group takes their tightest value (D85). A `per K:` keeps its class, which names the classes a row stands for,
	//! and a kept row in none of them still lies in a group, whose row reads it. The kept rows are grouped by the `by`
	//! key after the terms are computed, on every kept row: the root and its G's of that key read the key there.
	void GroupClass(DecideNode &node, const DecideConstraint &facts) {
		if (!facts.lhs_is_aggregate && !facts.was_minmax_easy) {
			return;
		}
		if (node.per == DecideTreePer::ROW) {
			node.key = facts.group_key;
			node.per = node.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
		} else {
			node.holds_rows_in_no_class = true;
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
		node.decision_value = node.decision_value || tree->nodes[child]->decision_value;
		DecideSignatureField field;
		if (tree->nodes[child]->kind == DecideNodeKind::FUNCTION) {
			field.kind = DecideSignatureFieldKind::DECISION;
			field.decision = op.helper_definitions[tree->nodes[child]->definition]->helper;
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
		for (idx_t i = 0; i < op.bound.bound_aggregates.size(); i++) {
			if (op.bound.bound_aggregates[i].IsReadBy(expression)) {
				return i;
			}
		}
		return DConstants::INVALID_INDEX;
	}

	//! With `carried`, the columns `expression` reads are carried, and neither they nor its decisions are sigma fields
	//! (D83). A query-wide value has one value everywhere. A generated NULL test (BoundDecideNullTestExpression) only
	//! settles membership: a member test, or the test around a recorded reducer's value, which is its key alone (D70,
	//! deck p. 81).
	//! A child read adds its key field either way.
	unique_ptr<Expression> Lower(idx_t owner, const Expression &expression, bool carried = false) {
		auto &node = *tree->nodes[owner];
		// A kept call reads its definition's F node (definition_nodes), or lowers as its product's expansion.
		const auto kept = DecideDefinitionOf(expression);
		if (kept.Is(DecideHelperKind::DEFINITION)) {
			return Read(owner, definition_nodes[kept.index], expression.return_type, expression.GetAlias());
		}
		if (kept.Is(DecideHelperKind::PRODUCT)) {
			return Lower(owner, *op.helper_definitions[kept.index]->body, carried);
		}
		carried = carried || IsDecideQueryWideValue(op.bound, expression) ||
		          expression.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_NULL_TEST;
		auto record = FindRecord(expression);
		if (record != DConstants::INVALID_INDEX) {
			return Read(owner, Record(record), expression.return_type, expression.GetAlias());
		}
		idx_t frame;
		if (TryGetDecideFrame(expression, frame)) {
			return Read(owner, Frame(frame, &expression), expression.return_type, expression.GetAlias());
		}
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER) {
			auto reducer = Aggregate(expression.Cast<BoundDecideReducerExpression>());
			reducer_nodes[&expression] = reducer;
			return Read(owner, reducer, expression.return_type, expression.GetAlias());
		}
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &reference = expression.Cast<BoundColumnRefExpression>();
			if (reference.binding.table_index == op.decide_index) {
				auto variable = reference.binding.column_index;
				for (auto function : tree->function_nodes) {
					auto &function_node = *tree->nodes[function];
					auto &definition = *op.helper_definitions[function_node.definition];
					// A row an indicator or a definition owns states the helper: it reads the decision, not the F.
					const bool reads_function = definition.kind == DecideHelperKind::BOOLEAN_PRODUCT ||
					                            ((definition.kind == DecideHelperKind::L0_INDICATOR ||
					                              definition.kind == DecideHelperKind::DEFINITION) &&
					                             !OwnedByFunction(node, definition.kind));
					if (reads_function && definition.helper == variable) {
						return Read(owner, function, expression.return_type, expression.GetAlias());
					}
				}
				for (idx_t link = 0; link < op.helper_definitions.size(); link++) {
					if (op.helper_definitions[link]->kind == DecideHelperKind::REDUCER_VALUE &&
					    op.helper_definitions[link]->helper == variable) {
						auto child = Link(link);
						const auto &read_text = tree->nodes[child]->text;
						return Read(owner, child, expression.return_type,
						            read_text.empty() ? expression.GetAlias() : read_text);
					}
				}
				node.decision_value = true;
				if (!carried &&
				    (variable >= op.bound.variable_scopes.size() || !op.bound.variable_scopes[variable].IsGlobal())) {
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
		if (kept.Is(DecideHelperKind::ABS)) {
			// The F node is registered before its readers so nested ABS references can resolve in gates. Lower its
			// operand on the first source-tree read: reducers and nested helpers then join the same tree as their
			// written occurrence, instead of leaving an unresolved aggregate expression in F metadata.
			const idx_t function = definition_nodes[kept.index];
			auto &abs_node = *tree->nodes[function];
			if (!abs_node.body) {
				abs_node.body = Lower(function, *expression.Cast<BoundFunctionExpression>().children[0]);
			}
			return Read(owner, function, expression.return_type, expression.GetAlias());
		}
		// The children are lowered from the clause's own expressions, so a reducer's address is the one the term half
		// splits (reducer_nodes); the copy only holds the results.
		auto result = expression.Copy();
		vector<const Expression *> originals;
		ExpressionIterator::EnumerateChildren(expression,
		                                      [&](const Expression &child) { originals.push_back(&child); });
		idx_t next = 0;
		ExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<Expression> &child) { child = Lower(owner, *originals[next++], carried); });
		return result;
	}

	//! The G of a reducer.
	idx_t Aggregate(const BoundDecideReducerExpression &aggregate) {
		auto index = NewNode(DecideNodeKind::REDUCER);
		auto &node = *tree->nodes[index];
		node.aggregate = aggregate.kind;
		node.text = Text(aggregate);
		node.decision_value = HasDecision(aggregate);
		node.key = aggregate.by;
		idx_t qualifier = aggregate.per;
		const auto frame_aggregate = aggregate.frame_aggregate;
		if (frame_aggregate < op.frame_aggregates.size()) {
			node.key = op.frame_aggregates[frame_aggregate].key;
			qualifier = op.frame_aggregates[frame_aggregate].per;
			node.text = op.frame_aggregates[frame_aggregate].text;
			tree->frame_aggregate_nodes[frame_aggregate] = index;
		}
		node.function = unique_ptr_cast<Expression, BoundAggregateExpression>(aggregate.ToAggregate());
		auto operand = NewNode(DecideNodeKind::SCOPE);
		Edge(index, operand);
		auto &entry = *tree->nodes[operand];
		entry.text = node.text + " operand";
		Gates(operand, aggregate.filter.get());
		if (qualifier < op.bound.keyed_scopes.size()) {
			node.qualifier_scope = qualifier;
			entry.key = op.bound.keyed_scopes[qualifier].key;
			entry.per = entry.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
			// The reducer's terms are read on every row its `when` keeps, and its qualifier keeps no value of a row
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
		if (norm_body && !node.decision_value) {
			// G's original data body is also the value validated before its
			// bound definition (SIGN for L0 can hide a NaN).
			node.body = Lower(operand, *norm_body);
		}
		if (qualifier < op.bound.keyed_scopes.size()) {
			Qualified(operand, qualifier);
		}
		return index;
	}

	//! A relation-qualified reducer's operand, once its body is lowered: its instances are its qualifier's classes
	//! split by the values its body reads (D70, D71), the keyed mapping of the qualifier's scope
	//! (DecideModelEmitter::SignatureMapping), whichever reducer it is, a written one or a helper's.
	void Qualified(idx_t operand, idx_t scope) {
		UseSignature(*tree->nodes[operand], op.bound.keyed_scopes[scope].signature);
		tree->qualified_operands[scope] = operand;
	}

	idx_t Link(idx_t link_index) {
		if (definition_nodes[link_index] != DConstants::INVALID_INDEX) {
			return definition_nodes[link_index];
		}
		auto &link = *op.helper_definitions[link_index];
		if (link.Frame() != DConstants::INVALID_INDEX) {
			auto index = Frame(link.Frame(), nullptr, link.Member().get());
			definition_nodes[link_index] = index;
			auto &node = *tree->nodes[index];
			node.definition = link_index;
			node.decision_value = true;
			if (link.helper < tree->value_nodes.size()) {
				tree->value_nodes[link.helper] = index;
			}
			return index;
		}
		auto &reducer = link.Reducer();
		auto index = NewNode(DecideNodeKind::REDUCER);
		definition_nodes[link_index] = index;
		auto &node = *tree->nodes[index];
		node.definition = link_index;
		node.aggregate = link.reducer;
		node.qualifier_scope = reducer.per;
		node.key = link.path_key;
		node.text = link.label;
		node.decision_value = true;
		Gates(index, link.when.get());
		if (link.helper < tree->value_nodes.size()) {
			tree->value_nodes[link.helper] = index;
		}
		auto operand = NewNode(DecideNodeKind::SCOPE);
		Edge(index, operand);
		auto &entry = *tree->nodes[operand];
		entry.text = node.text + " operand";
		Gates(operand, reducer.filter.get());
		if (reducer.per < op.bound.keyed_scopes.size()) {
			entry.key = op.bound.keyed_scopes[reducer.per].key;
			entry.per = entry.key == DConstants::INVALID_INDEX ? DecideTreePer::ONE : DecideTreePer::KEY;
		}
		entry.body = Lower(operand, *reducer.children[0]);
		if (reducer.per < op.bound.keyed_scopes.size()) {
			Qualified(operand, reducer.per);
		}
		return index;
	}

	idx_t Record(idx_t record_index) {
		if (record_nodes[record_index] != DConstants::INVALID_INDEX) {
			return record_nodes[record_index];
		}
		auto &record = op.bound.bound_aggregates[record_index];
		auto index = NewNode(DecideNodeKind::REDUCER);
		record_nodes[record_index] = index;
		auto &node = *tree->nodes[index];
		// The record's reducer over its arguments.
		auto read = record.function->Copy();
		auto &reducer = read->Cast<BoundDecideReducerExpression>();
		for (auto &argument : record.arguments) {
			reducer.children.push_back(argument->Copy());
		}
		node.function = unique_ptr_cast<Expression, BoundAggregateExpression>(reducer.ToAggregate());
		node.aggregate = reducer.kind;
		node.key = record.key;
		node.text = Text(reducer);
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
		if (auto norm_body = DecideWrittenNormBody(reducer)) {
			node.body = Lower(operand, *norm_body);
		}
		return index;
	}

	idx_t Frame(idx_t frame_index, const Expression *marker, const Expression *payload = nullptr) {
		if (frame_index >= op.bound.frames.size()) {
			throw InternalException("DECIDE tree references invalid frame %llu", frame_index);
		}
		if (frame_nodes[frame_index] != DConstants::INVALID_INDEX) {
			return frame_nodes[frame_index];
		}
		auto index = NewNode(DecideNodeKind::FRAME);
		frame_nodes[frame_index] = index;
		auto &node = *tree->nodes[index];
		auto &frame = op.bound.frames[frame_index];
		node.frame = frame_index;
		node.key = frame.timeline_key;
		// An anchor whose `within` column is NULL lies in no timeline, so it reads no value (D75): the O holds it in a
		// slot of its own, which no timeline is built from (DecideTreeColumns::ReadFrames).
		node.holds_rows_in_no_class = true;
		node.text = frame.text;
		node.aggregate = frame.reducer;
		Gates(index, frame.rows.get());
		if (frame.order) {
			node.over = Resolve(*frame.order);
		}
		if (marker && marker->GetExpressionClass() == ExpressionClass::BOUND_DECIDE_FRAME) {
			auto &aggregate = marker->Cast<BoundDecideFrameExpression>();
			if (auto fold = DecideFrameFold(frame, aggregate)) {
				node.function = unique_ptr_cast<Expression, BoundAggregateExpression>(std::move(fold));
			}
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
		}
		node.signature = std::move(payload_signature);
		return index;
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
			if (!node.Linked() && node.kind != DecideNodeKind::FRAME) {
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
			auto &node = *tree->nodes[i];
			auto variable = node.defined_var;
			const bool abs_envelope = node.compiled.clause && IsDecideAbsEnvelope(node.compiled.clause->helper_row);
			if (variable < definitions.size() && !abs_envelope &&
			    !OwnedByFunction(node, DecideHelperKind::L0_INDICATOR) &&
			    !OwnedByFunction(node, DecideHelperKind::DEFINITION)) {
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
	//! holding its value, and for a product (BilinearLinkSpec) what each factor is defined from.
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

DecideTreeConstruction::DecideTreeConstruction(ClientContext &context, LogicalDecide &op)
    : builder(make_uniq<Builder>(context, op)) {
	builder->Structure();
}

DecideTreeConstruction::~DecideTreeConstruction() {
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
		if (node.objective && written_sense == DecideSense::FEASIBILITY) {
			throw InternalException("DECIDE tree objective node %llu has no direction", i);
		}
		const auto value_var = ValueVar(node);
		if (node.kind == DecideNodeKind::FUNCTION) {
			auto &definition = *definitions[node.definition];
			const bool abs = definition.kind == DecideHelperKind::ABS;
			const idx_t expected_rows = abs                                                 ? 2
			                            : definition.kind == DecideHelperKind::L0_INDICATOR ? 3
			                            : definition.kind == DecideHelperKind::DEFINITION   ? 1
			                                                                                : 0;
			bool invalid = value_var >= value_nodes.size() ||
			               (!variable_scopes.empty() && value_var >= variable_scopes.size()) ||
			               node.defined_var != value_var || !node.body || node.rows.size() != expected_rows;
			for (idx_t r = 0; r < node.rows.size() && !invalid; r++) {
				const auto root = node.rows[r];
				invalid = root >= nodes.size() || !nodes[root]->compiled.clause ||
				          nodes[root]->compiled.clause->defined_var != value_var || nodes[root]->parent != i ||
				          std::find(node.children.begin(), node.children.end(), root) == node.children.end() ||
				          (abs && nodes[root]->compiled.clause->helper_row !=
				                      (r == 0 ? DecideHelperRow::ABS_ENVELOPE : DecideHelperRow::ABS_NEGATED_ENVELOPE));
			}
			if (invalid) {
				throw InternalException("DECIDE tree has an invalid function node %llu", i);
			}
		}
		if (node.kind == DecideNodeKind::FUNCTION) {
			DecideVarScopeInfo scope;
			if (!variable_scopes.empty()) {
				scope = variable_scopes[value_var];
			}
			const bool keyed = scope.IsKeyed() ||
			                   (scope.scope == DecideVarScope::INLINED && scope.keyed_scope_idx < keyed_scopes.size());
			const auto expected_per = scope.IsGlobal() ? DecideTreePer::ONE
			                          : keyed          ? DecideTreePer::KEY
			                                           : DecideTreePer::ROW;
			const auto expected_key = keyed ? keyed_scopes[scope.keyed_scope_idx].key : DConstants::INVALID_INDEX;
			if (node.per != expected_per || node.key != expected_key) {
				throw InternalException("DECIDE tree function node %llu does not match its helper's scope", i);
			}
		}
		if (node.key != DConstants::INVALID_INDEX && node.key >= keys.size()) {
			throw InternalException("DECIDE tree node %llu has an invalid key", i);
		}
		if (node.Linked() &&
		    (value_var >= value_nodes.size() || value_nodes[value_var] != i ||
		     (node.kind != DecideNodeKind::REDUCER && node.kind != DecideNodeKind::FRAME) ||
		     (node.kind == DecideNodeKind::FRAME) != (node.frame != DConstants::INVALID_INDEX) ||
		     (node.kind == DecideNodeKind::FRAME && node.qualifier_scope != DConstants::INVALID_INDEX))) {
			throw InternalException("DECIDE tree node %llu has invalid value-link ownership", i);
		}
		if (node.qualifier_scope != DConstants::INVALID_INDEX &&
		    (node.kind != DecideNodeKind::REDUCER || node.qualifier_scope >= keyed_scopes.size())) {
			throw InternalException("DECIDE tree node %llu has an invalid reducer qualifier", i);
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
		if (function >= nodes.size() || nodes[function]->kind != DecideNodeKind::FUNCTION ||
		    listed_functions[function]) {
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
		       << (node.kind == DecideNodeKind::SCOPE     ? "E"
		           : node.kind == DecideNodeKind::REDUCER ? "G"
		           : node.kind == DecideNodeKind::FRAME   ? "O"
		                                                  : "F");
		if (node.root) {
			result << (node.objective ? " objective" : node.hidden ? " definition" : " root");
		}
		result << " key=" << DecideKeyText(keys, node.key);
		if (node.kind == DecideNodeKind::REDUCER || (node.kind == DecideNodeKind::FRAME && frames[node.frame].range)) {
			result << " agg=" << StringUtil::Lower(EnumUtil::ToString(node.aggregate));
		}
		if (node.kind == DecideNodeKind::FUNCTION) {
			auto &definition = *definitions[node.definition];
			result << " function="
			       << (definition.kind == DecideHelperKind::ABS               ? "abs"
			           : definition.kind == DecideHelperKind::BOOLEAN_PRODUCT ? "boolean_product"
			           : definition.kind == DecideHelperKind::L0_INDICATOR    ? "l0_indicator"
			                                                                  : "definition")
			       << " value=x" << definition.helper << " body=(" << node.body->ToString() << ")";
			if (definition.kind == DecideHelperKind::ABS) {
				result << (definition.exact ? " exact=big-m" : " exact=pressure-or-bound");
			} else if (definition.kind == DecideHelperKind::L0_INDICATOR && definition.auto_m) {
				result << " M=auto";
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
		result << "] columns=" << node.columns.size();
		result << " : " << node.text << "\n";
	}
	result << "postorder=[";
	for (idx_t i = 0; i < eval_order.size(); i++) {
		result << (i ? "," : "") << eval_order[i];
	}
	result << "]\n";
	return result.str();
}

} // namespace duckdb
