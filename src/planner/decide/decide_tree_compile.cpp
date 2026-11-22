//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/planner/decide/decide_tree_compile.cpp
//
// The tree's bodies (language/guide.md §6.4): the last step of the tree's construction (DecideTreeConstruction, a
// Builder with a director in plan_decide.cpp). Stage 05 has lowered each clause into its root and each reducer's terms
// into its G, naming on each term the G it ranges over; this numbers the clause roots, finds the node each term is
// computed on and compiles it into programs there.
//===----------------------------------------------------------------------===//

#include "duckdb/planner/decide/decide_tree.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <algorithm>
#include <sstream>

namespace duckdb {

//! The compile's helpers, named apart: the planner's DECIDE files build as one unit.
namespace decide_tree_compile {

//! Where a hole's index starts while a node's programs are compiled, above any column of its layout: its program
//! columns grow until the last program is compiled.
static constexpr idx_t DECIDE_HOLE_BASE = idx_t(1) << 48;

} // namespace decide_tree_compile

idx_t DecideTreeOperand(const DecideTree &tree, idx_t reducer) {
	idx_t operand = DConstants::INVALID_INDEX;
	for (auto child : tree.nodes[reducer]->children) {
		if (tree.nodes[child]->kind != DecideNodeKind::SCOPE) {
			continue;
		}
		if (operand != DConstants::INVALID_INDEX) {
			throw InternalException("DECIDE tree: %s has two operands", tree.nodes[reducer]->text);
		}
		operand = child;
	}
	if (operand == DConstants::INVALID_INDEX) {
		throw InternalException("DECIDE tree: %s has no operand", tree.nodes[reducer]->text);
	}
	return operand;
}

idx_t DecideTreeMembersOf(const DecideTree &tree, idx_t reducer) {
	while (true) {
		const Expression *body = tree.nodes[DecideTreeOperand(tree, reducer)]->body.get();
		if (!body || body->GetExpressionClass() != ExpressionClass::BOUND_REF ||
		    body->Cast<BoundReferenceExpression>().index < tree.input_column_count) {
			return reducer;
		}
		const auto inner = body->Cast<BoundReferenceExpression>().index - tree.input_column_count;
		if (tree.nodes[inner]->kind != DecideNodeKind::REDUCER) {
			return reducer;
		}
		reducer = inner;
	}
}

namespace decide_tree_compile {

//! Where a term of `holder`'s body is computed: on the operand of the G it ranges over, else on the holder itself.
static DecideTermSite Site(const DecideTree &tree, idx_t holder, idx_t child, idx_t qualifier_scope,
                           idx_t qualifier_reducer) {
	DecideTermSite site;
	site.node = child == DConstants::INVALID_INDEX ? holder : DecideTreeOperand(tree, child);
	if (qualifier_scope != DConstants::INVALID_INDEX) {
		if (qualifier_reducer >= tree.nodes.size() ||
		    tree.nodes[qualifier_reducer]->qualifier_scope != qualifier_scope ||
		    tree.nodes[qualifier_reducer]->kind != DecideNodeKind::REDUCER) {
			throw InternalException("DECIDE tree: a relation-qualified term has no matching reducer occurrence");
		}
		site.qualifier_operand = DecideTreeOperand(tree, qualifier_reducer);
	}
	return site;
}

//! A product group is computed whole where its reducer ranges: its weight and every term of its forms, whose records
//! carry its reducer (ApplyAggregateMetadata stamps the group, Stamp its forms' terms).
static void SiteGroups(const DecideTree &tree, idx_t holder, const vector<DecideProductGroup> &groups,
                       DecideBody &body) {
	for (auto &group : groups) {
		DecideGroupSites sites;
		sites.weight = Site(tree, holder, group.child, group.qualifier_scope_idx, group.qualifier_reducer);
		sites.left.assign(group.left.size(), sites.weight);
		sites.right.assign(group.right.size(), sites.weight);
		body.groups.push_back(std::move(sites));
	}
}

static void SiteClause(const DecideTree &tree, idx_t holder, DecideBody &body) {
	auto &clause = *body.clause;
	for (auto &term : clause.lhs_terms) {
		body.linear.push_back(Site(tree, holder, term.child, term.qualifier_scope_idx, term.qualifier_reducer));
	}
	SiteGroups(tree, holder, clause.product_groups, body);
}

static void SiteObjective(const DecideTree &tree, idx_t holder, DecideBody &body) {
	auto &objective = *body.objective;
	for (auto &term : objective.terms) {
		body.linear.push_back(Site(tree, holder, term.child, term.qualifier_scope_idx, term.qualifier_reducer));
	}
	SiteGroups(tree, holder, objective.product_groups, body);
}

//! A G's or an O's members: computed on the G's operand, or on the O, which holds a frame's positions.
static void SiteMembers(const DecideTree &tree, idx_t holder, DecideBody &body) {
	const auto site = tree.nodes[holder]->kind == DecideNodeKind::FRAME ? holder : DecideTreeOperand(tree, holder);
	body.linear.assign(body.members.size(), DecideTermSite {site});
	body.fallback_sites.assign(body.fallback.size(), DecideTermSite {site});
}

//! The clause roots holding a clause, numbered in leaf order: the constraints, then the folded bounds whose repair
//! rows come from their clause (a folded bound with none holds no clause): the order of the repair group ids.
static void NumberClauses(DecideTree &tree, const vector<idx_t> &roots) {
	for (auto root : roots) {
		auto &node = *tree.nodes[root];
		tree.constraint_count += node.compiled.clause && !node.folded;
	}
	idx_t constraint = 0;
	for (auto root : roots) {
		auto &node = *tree.nodes[root];
		auto &body = node.compiled;
		if (!body.clause) {
			continue;
		}
		body.clause_index = node.folded ? tree.constraint_count + tree.singleton_row_count++ : constraint++;
		body.kind = DecideBodyKind::CONSTRAINT;
		body.order = body.clause_index;
		SiteClause(tree, root, body);
	}
}

//! Each helper's G or O holds its reducer's members and a frame's else terms (LowerDecideTree).
static void LinkBodies(DecideTree &tree, const vector<idx_t> &link_nodes) {
	for (idx_t link = 0; link < link_nodes.size(); link++) {
		auto &body = tree.nodes[link_nodes[link]]->compiled;
		body.kind = DecideBodyKind::VALUE_LINK;
		body.order = link;
		SiteMembers(tree, link_nodes[link], body);
	}
}

//! Compiles stage 05's expressions into programs over a node's layout, as DuckDB's ColumnBindingResolver turns a plan's
//! column references into chunk positions: by ToChunkReferences, with each leaf mapped to the
//! layout. A read the tree computes no value for yet becomes a NULL of its type and names the reason on the body.
class DecideProgramCompiler {
public:
	DecideProgramCompiler(ClientContext &context, const LogicalDecide &op, DecideTree &tree,
	                      const vector<idx_t> &record_nodes)
	    : context(context), op(op), tree(tree), record_nodes(record_nodes) {
	}

	//! A program on `site` computing `expression`, of `owner`'s body; returns its index on the site. A message about a
	//! value that is not finite names `written`, and points at it when it has a location of its own.
	idx_t Add(idx_t site, DecideProgramRole role, const Expression &expression, idx_t owner, int sign = 1) {
		auto &owner_body = tree.nodes[owner]->compiled;
		DecideProgram program;
		program.role = role;
		program.expression = Translate(site, expression, owner_body);
		program.written = expression.Copy();
		program.sign = sign;
		program.owner = owner;
		auto &programs = tree.nodes[site]->programs;
		programs.push_back(std::move(program));
		return programs.size() - 1;
	}

	//! A term's coefficient on its site.
	void AddTerm(const DecideTerm &term, DecideTermSite &site, idx_t owner) {
		site.program = Add(site.node, DecideProgramRole::COEFFICIENT, *term.coefficient, owner, term.sign);
	}

	//! A product group's weight, none when stage 05 left it out (1), then each term of its forms.
	void AddGroup(const DecideProductGroup &group, DecideGroupSites &sites, idx_t owner) {
		if (group.weight) {
			sites.weight.program =
			    Add(sites.weight.node, DecideProgramRole::COEFFICIENT, *group.weight, owner, group.sign);
		}
		auto add_form = [&](const vector<DecideTerm> &terms, vector<DecideTermSite> &form) {
			for (idx_t t = 0; t < terms.size(); t++) {
				form[t].program =
				    Add(form[t].node, DecideProgramRole::COEFFICIENT, *terms[t].coefficient, owner, terms[t].sign);
			}
		};
		add_form(group.left, sites.left);
		add_form(group.right, sites.right);
	}

	vector<DecideProgram> &Programs(idx_t node) {
		return tree.nodes[node]->programs;
	}

	//! What a variant of `root`'s clause is named by at its first row (DecideBody::names): its class
	//! key's elements as written, then each value and `by` key its sigma reads, and each frame's
	//! timeline and order. Each is named as the query writes it (DecideBody::texts). A key's or a value's error is the
	//! descent's or the terms' to raise, so these are never reported.
	void AddNames(idx_t root) {
		auto &node = *tree.nodes[root];
		auto add = [&](const Expression &expression, const string &text) {
			node.compiled.names.push_back(Add(root, DecideProgramRole::VALUE, expression, root));
			node.compiled.texts.push_back(text);
		};
		for (auto &element : DecideKeyElements(tree.keys, node.key)) {
			add(*element.expression, element.name);
		}
		for (auto &field : node.signature) {
			if (field.kind == DecideSignatureFieldKind::VALUE) {
				add(*field.value, field.text);
			} else if (field.kind == DecideSignatureFieldKind::BY_KEY) {
				for (auto &element : DecideKeyElements(tree.keys, field.key)) {
					add(*element.expression, element.name);
				}
			} else if (field.kind == DecideSignatureFieldKind::FRAME) {
				// Its timeline, and its anchor's place there when it reads relative to it.
				auto &frame = op.frames[field.frame];
				for (auto &element : DecideKeyElements(tree.keys, frame.timeline_key)) {
					add(*element.expression, element.name);
				}
				if (frame.Relative()) {
					add(*frame.order, frame.order_name);
					if (frame.order_collation) {
						node.compiled.position_names.emplace_back(node.compiled.names.size() - 1, field.frame);
					}
				}
			}
		}
	}

	//! A node's member tests, over its own carried columns, once every term is compiled.
	void AddTests(idx_t index) {
		auto &node = *tree.nodes[index];
		for (idx_t i = 0; i < node.member_tests.size(); i++) {
			DecideProgram program;
			program.role = DecideProgramRole::TEST;
			program.expression = TranslateTree(index, *node.member_tests[i], "a member test");
			program.written = node.member_tests[i]->Copy();
			node.tests.push_back(node.programs.size());
			node.programs.push_back(std::move(program));
		}
	}

	//! What a node computes from the data alone, in the tree's own terms: a data check's message on its hidden root,
	//! and a data reducer's arguments on its operand, which the G folds: a norm's are its desugared reducer's (a sum
	//! of squares, of absolute values), and a NaN in its body is its data check's to refuse (DecideWrittenNormBody).
	void AddData(idx_t index) {
		auto &node = *tree.nodes[index];
		if (node.value_check) {
			node.compiled.check = AddValue(index, *node.body, "a data check");
		}
		if (node.kind != DecideNodeKind::REDUCER || node.decision_value || !node.function) {
			return;
		}
		const auto operand = DecideTreeOperand(tree, index);
		auto &operand_node = *tree.nodes[operand];
		for (auto &argument : operand_node.arguments) {
			operand_node.compiled.arguments.push_back(AddValue(operand, *argument, "a data reducer's argument"));
		}
	}

private:
	ClientContext &context;
	const LogicalDecide &op;
	DecideTree &tree;
	const vector<idx_t> &record_nodes;
	//! A program of `index`'s own computing `expression`, in the tree's terms.
	idx_t AddValue(idx_t index, const Expression &expression, const char *what) {
		auto &node = *tree.nodes[index];
		DecideProgram program;
		program.role = DecideProgramRole::VALUE;
		program.expression = TranslateTree(index, expression, what);
		program.written = expression.Copy();
		node.programs.push_back(std::move(program));
		return node.programs.size() - 1;
	}

	//! A source column's place in `site`'s layout: among its carried columns, else among its program columns, added.
	idx_t Column(idx_t site, const LogicalType &type, idx_t column) {
		auto &node = *tree.nodes[site];
		const BoundReferenceExpression read(type, column);
		for (idx_t i = 0; i < node.columns.size(); i++) {
			if (node.columns[i]->Equals(read)) {
				return i;
			}
		}
		for (idx_t i = 0; i < node.program_columns.size(); i++) {
			if (node.program_columns[i]->Equals(read)) {
				return node.columns.size() + i;
			}
		}
		node.program_columns.push_back(read.Copy());
		return node.columns.size() + node.program_columns.size() - 1;
	}

	//! Stage 05's `expression` over `site`'s layout. Stage 05 reads the source as the operator's input columns, a data
	//! reducer as its record's placeholder, a frame as a column after the input's, and an admitted end its inputs a and
	//! r as DecideAdmittedInput.
	unique_ptr<Expression> Translate(idx_t site, const Expression &expression, DecideBody &body) {
		DecideChunkLeaf leaf = [&](const Expression &leaf_expression) -> unique_ptr<Expression> {
			const auto kind = leaf_expression.GetExpressionClass();
			if (kind != ExpressionClass::BOUND_REF && kind != ExpressionClass::BOUND_COLUMN_REF) {
				return nullptr;
			}
			auto &type = leaf_expression.return_type;
			for (idx_t record = 0; record < op.bound_aggregates.size(); record++) {
				if (op.bound_aggregates[record].IsReadBy(leaf_expression)) {
					return Hole(site, record_nodes[record], type);
				}
			}
			if (kind == ExpressionClass::BOUND_REF) {
				const auto column = leaf_expression.Cast<BoundReferenceExpression>().index;
				if (column >= tree.frame_reads.column_base) {
					return FrameColumn(site, column - tree.frame_reads.column_base, type);
				}
				return make_uniq<BoundReferenceExpression>(type, Column(site, type, column));
			}
			auto &binding = leaf_expression.Cast<BoundColumnRefExpression>().binding;
			if (binding.table_index == DConstants::INVALID_INDEX) {
				// An admitted end's input, bound once the node's layout is known (BindAdmittedInputs).
				return leaf_expression.Copy();
			}
			if (binding.table_index == op.decide_index) {
				throw InternalException("DECIDE tree: a coefficient reads a decision: %s", leaf_expression.ToString());
			}
			throw InternalException("DECIDE tree: a coefficient reads an unresolved column: %s",
			                        leaf_expression.ToString());
		};
		return ToChunkReferences(expression, context, leaf);
	}

	//! A G over data's value as `site` reads it, or an O's frame column `column`: a hole of its layout, after its
	//! program columns, which are known once every program is compiled (BindLayout).
	unique_ptr<Expression> Hole(idx_t site, idx_t node, const LogicalType &type,
	                            idx_t column = DConstants::INVALID_INDEX) {
		auto &holes = tree.nodes[site]->holes;
		idx_t hole = 0;
		while (hole < holes.size() && (holes[hole].node != node || holes[hole].column != column)) {
			hole++;
		}
		if (hole == holes.size()) {
			holes.push_back({node, type, column});
		}
		return make_uniq<BoundReferenceExpression>(type, DECIDE_HOLE_BASE + hole);
	}

	//! Frame column `j` (DecideFrameReads::columns) as `site` reads it, of type `type`: the column of its
	//! frame's O at the entry's row (language/guide.md §5.6), or a reducer's over frames, its G's fold.
	unique_ptr<Expression> FrameColumn(idx_t site, idx_t j, const LogicalType &type) {
		auto &column = tree.frame_reads.columns[j];
		if (column.kind == DecideFrameColumn::Kind::AGGREGATE) {
			idx_t aggregate = DConstants::INVALID_INDEX;
			if (!TryParseFrameAggregateTag(column.value->GetAlias(), aggregate) ||
			    aggregate >= tree.frame_aggregate_nodes.size() ||
			    tree.frame_aggregate_nodes[aggregate] == DConstants::INVALID_INDEX) {
				throw InternalException("DECIDE tree: %s has no node", column.text);
			}
			return Hole(site, tree.frame_aggregate_nodes[aggregate], type);
		}
		if (tree.frame_nodes[column.frame] == DConstants::INVALID_INDEX) {
			throw InternalException("DECIDE tree: frame %llu has no node", column.frame);
		}
		return Hole(site, tree.frame_nodes[column.frame], type, j);
	}

	//! An O's value as an expression Lower read in the tree's terms reads it: over data, its frame's one value (a
	//! point's VALUE or a range's REDUCED column), of the read's type; over decisions, a reducer's NULL test of it, its
	//! DEFINED column, as stage 05 reads both (DecideFrameColumn).
	unique_ptr<Expression> FrameRead(idx_t site, const DecideNode &frame, const LogicalType &type) {
		auto &columns = tree.frame_reads.columns;
		for (idx_t j = 0; j < columns.size(); j++) {
			auto kind = columns[j].kind;
			const bool read = frame.decision_value
			                      ? kind == DecideFrameColumn::Kind::DEFINED
			                      : kind == DecideFrameColumn::Kind::VALUE || kind == DecideFrameColumn::Kind::REDUCED;
			if (read && columns[j].frame == frame.frame) {
				auto hole = FrameColumn(site, j, columns[j].type);
				return frame.decision_value ? std::move(hole)
				                            : BoundCastExpression::AddCastToType(context, std::move(hole), type);
			}
		}
		throw InternalException("DECIDE tree: %s has no column its reader reads", frame.text);
	}

public:
	//! What each frame column's O computes on its anchors (DecideFrameColumnPrograms), in stage 05's terms: its value,
	//! a range's reducer's arguments, and its `else`.
	void AddFrameColumns() {
		auto &columns = tree.frame_reads.columns;
		tree.frame_columns.resize(columns.size());
		for (idx_t j = 0; j < columns.size(); j++) {
			auto &column = columns[j];
			if (column.kind == DecideFrameColumn::Kind::AGGREGATE || column.kind == DecideFrameColumn::Kind::DEFINED) {
				continue;
			}
			auto &programs = tree.frame_columns[j];
			programs.node = tree.frame_nodes[column.frame];
			auto add = [&](const Expression &expression) {
				DecideProgram program;
				program.role = DecideProgramRole::VALUE;
				program.expression = Translate(programs.node, expression, tree.nodes[programs.node]->compiled);
				program.written = expression.Copy();
				auto &node = *tree.nodes[programs.node];
				node.programs.push_back(std::move(program));
				return node.programs.size() - 1;
			};
			if (column.kind == DecideFrameColumn::Kind::REDUCED) {
				for (auto &argument : column.value->Cast<BoundAggregateExpression>().children) {
					programs.arguments.push_back(add(*argument));
				}
			} else {
				programs.value = add(*column.value);
			}
			if (column.fallback) {
				programs.fallback = add(*column.fallback);
			}
		}
	}

private:
	//! An expression Lower already read in the tree's terms (`what`: a member test, a data check, a data reducer's
	//! argument): a source column among its node's carried ones, and a G's or an O's value over data as a hole. Such an
	//! expression reads data alone.
	unique_ptr<Expression> TranslateTree(idx_t index, const Expression &expression, const char *what) {
		auto &node = *tree.nodes[index];
		DecideChunkLeaf leaf = [&](const Expression &leaf_expression) -> unique_ptr<Expression> {
			auto &type = leaf_expression.return_type;
			if (leaf_expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				// A generated NULL test reads data, a reducer's own (DecideCanonicalizer::SkipNullData) or a key's,
				// and a data check or a data reducer reads no decision.
				throw InternalException("DECIDE tree: %s reads a decision", node.text);
			}
			if (leaf_expression.GetExpressionClass() != ExpressionClass::BOUND_REF) {
				return nullptr;
			}
			const auto column = leaf_expression.Cast<BoundReferenceExpression>().index;
			if (column >= tree.input_column_count) {
				auto &child = *tree.nodes[column - tree.input_column_count];
				if (child.kind == DecideNodeKind::REDUCER && !child.decision_value && child.function) {
					return Hole(index, column - tree.input_column_count, type);
				}
				if (child.kind == DecideNodeKind::FRAME) {
					return FrameRead(index, child, type);
				}
				throw InternalException("DECIDE tree: %s of %s reads %s, which has no data value", what, node.text,
				                        child.text);
			}
			for (idx_t i = 0; i < node.columns.size(); i++) {
				if (node.columns[i]->Equals(leaf_expression)) {
					return make_uniq<BoundReferenceExpression>(type, i);
				}
			}
			throw InternalException("DECIDE tree: %s reads a column its node does not carry", node.text);
		};
		return ToChunkReferences(expression, context, leaf);
	}
};

//! A clause root's programs: its terms, its bound and admitted ends, its products and squares.
static void CompileClause(DecideProgramCompiler &compiler, idx_t root, DecideBody &body) {
	auto &clause = *body.clause;
	for (idx_t t = 0; t < clause.lhs_terms.size(); t++) {
		compiler.AddTerm(clause.lhs_terms[t], body.linear[t], root);
	}
	// A constant bound is a program too: it folds where it is executed.
	const auto role = clause.tightest_frame_bound ? DecideProgramRole::TIGHTEST_BOUND : DecideProgramRole::BOUND;
	body.rhs = compiler.Add(root, role, *clause.rhs_expr, root);
	if (clause.admitted_lower) {
		// A bound on one decision, admitted on each row its term is read on: an easy extremum's on its members, the
		// entries of its reducer's operand, whose filter (DecideConstraint::admitted_member) the descent routes. As
		// They are computed in this order: the members, then a and r on them, then each end over the row and those two.
		const auto site = body.linear[0].node;
		const auto a = compiler.Add(site, DecideProgramRole::VALUE, *clause.admitted_a, root);
		const auto r = compiler.Add(site, DecideProgramRole::VALUE, *clause.admitted_r, root);
		body.lower = compiler.Add(site, DecideProgramRole::BOUND, *clause.admitted_lower, root);
		body.upper = compiler.Add(site, DecideProgramRole::BOUND, *clause.admitted_upper, root);
		auto &programs = compiler.Programs(site);
		for (auto end : {body.lower, body.upper}) {
			programs[end].written = clause.rhs_expr->Copy();
			programs[end].arguments = {a, r};
		}
	}
	for (idx_t g = 0; g < clause.product_groups.size(); g++) {
		compiler.AddGroup(clause.product_groups[g], body.groups[g], root);
	}
	if (clause.scope.signature != DConstants::INVALID_INDEX) {
		compiler.AddNames(root);
	}
}

//! The objective's programs: its terms, then its product groups.
static void CompileObjective(DecideProgramCompiler &compiler, idx_t root, DecideBody &body) {
	auto &objective = *body.objective;
	for (idx_t t = 0; t < objective.terms.size(); t++) {
		compiler.AddTerm(objective.terms[t], body.linear[t], root);
	}
	for (idx_t g = 0; g < objective.product_groups.size(); g++) {
		compiler.AddGroup(objective.product_groups[g], body.groups[g], root);
	}
}

//! A helper's members, then a frame helper's else terms.
static void CompileMembers(DecideProgramCompiler &compiler, idx_t holder, DecideBody &body) {
	for (idx_t t = 0; t < body.members.size(); t++) {
		compiler.AddTerm(body.members[t], body.linear[t], holder);
	}
	for (idx_t t = 0; t < body.fallback.size(); t++) {
		compiler.AddTerm(body.fallback[t], body.fallback_sites[t], holder);
	}
}

static void CompilePrograms(ClientContext &context, const LogicalDecide &op, DecideTree &tree,
                            const vector<idx_t> &record_nodes) {
	DecideProgramCompiler compiler(context, op, tree, record_nodes);
	for (idx_t i = 0; i < tree.nodes.size(); i++) {
		auto &body = tree.nodes[i]->compiled;
		if (body.clause) {
			CompileClause(compiler, i, body);
		} else if (body.objective) {
			CompileObjective(compiler, i, body);
		} else {
			CompileMembers(compiler, i, body);
		}
	}
	for (idx_t i = 0; i < tree.nodes.size(); i++) {
		compiler.AddTests(i);
		compiler.AddData(i);
	}
	compiler.AddFrameColumns();
}

//! A node's layout is known once every program on it is compiled: its holes follow its program columns, and an
//! admitted end reads its inputs a and r after them, bound after its chunk's columns
//! (DecideBindAdmittedInputs).
static void BindLayout(DecideTree &tree) {
	for (auto &node : tree.nodes) {
		const auto columns = node->columns.size() + node->program_columns.size();
		const auto width = columns + node->holes.size();
		const BoundReferenceExpression a(LogicalType::DOUBLE, width);
		const BoundReferenceExpression r(LogicalType::DOUBLE, width + 1);
		for (auto &program : node->programs) {
			ExpressionIterator::EnumerateExpression(program.expression, [&](Expression &expression) {
				if (expression.GetExpressionClass() == ExpressionClass::BOUND_REF) {
					auto &reference = expression.Cast<BoundReferenceExpression>();
					if (reference.index >= DECIDE_HOLE_BASE) {
						reference.index = columns + reference.index - DECIDE_HOLE_BASE;
					}
				}
			});
			program.expression = DecideBindAdmittedInputs(*program.expression, a, r);
		}
	}
}

} // namespace decide_tree_compile

void CompileDecideTree(ClientContext &context, const LogicalDecide &op, DecideTree &tree,
                       const DecideTreeSources &sources) {
	decide_tree_compile::NumberClauses(tree, sources.clause_roots);
	if (sources.objective_root != DConstants::INVALID_INDEX && tree.nodes[sources.objective_root]->compiled.objective) {
		auto &body = tree.nodes[sources.objective_root]->compiled;
		body.kind = DecideBodyKind::OBJECTIVE;
		body.order = 0;
		decide_tree_compile::SiteObjective(tree, sources.objective_root, body);
	}
	decide_tree_compile::LinkBodies(tree, sources.link_nodes);
	decide_tree_compile::CompilePrograms(context, op, tree, sources.record_nodes);
	decide_tree_compile::BindLayout(tree);
}

namespace decide_tree_compile {

static string Signed(idx_t value) {
	return value == DConstants::INVALID_INDEX ? "-1" : to_string(value);
}

static string BodyKindName(DecideBodyKind kind) {
	switch (kind) {
	case DecideBodyKind::CONSTRAINT:
		return "constraints";
	case DecideBodyKind::OBJECTIVE:
		return "objective";
	case DecideBodyKind::VALUE_LINK:
		return "links";
	default:
		return "other";
	}
}

static string RoleName(DecideProgramRole role) {
	switch (role) {
	case DecideProgramRole::COEFFICIENT:
		return "COEFFICIENT";
	case DecideProgramRole::BOUND:
		return "BOUND";
	case DecideProgramRole::TIGHTEST_BOUND:
		return "TIGHTEST_BOUND";
	case DecideProgramRole::TEST:
		return "TEST";
	case DecideProgramRole::VALUE:
		return "VALUE";
	}
	throw InternalException("DECIDE tree: an unknown program role");
}

static string Label(const unique_ptr<Expression> &coefficient) {
	return coefficient ? coefficient->GetName() : string();
}

static void Terms(std::ostringstream &out, const vector<DecideTerm> &terms, const vector<DecideTermSite> &sites,
                  const string &indent, const char *kind) {
	for (idx_t t = 0; t < terms.size(); t++) {
		out << indent << kind << " " << t << " var=" << Signed(terms[t].variable_index)
		    << " child=" << Signed(terms[t].child) << " site=" << Signed(sites[t].node)
		    << " program=" << Signed(sites[t].program) << " label=" << Label(terms[t].coefficient) << "\n";
	}
}

static void Groups(std::ostringstream &out, const vector<DecideProductGroup> &groups,
                   const vector<DecideGroupSites> &sites) {
	for (idx_t g = 0; g < groups.size(); g++) {
		out << "  group " << g << " child=" << Signed(groups[g].child) << " square=" << groups[g].square
		    << " weight_sign=" << groups[g].weight_sign << " site=" << Signed(sites[g].weight.node)
		    << " program=" << Signed(sites[g].weight.program) << " label=" << Label(groups[g].weight) << "\n";
		Terms(out, groups[g].left, sites[g].left, "    ", "left");
		Terms(out, groups[g].right, sites[g].right, "    ", "right");
	}
}

//! What a node's block holds: a clause, the objective, a G's members, or entries with programs.
static string Head(const DecideNode &node) {
	auto &body = node.compiled;
	string head = body.clause                         ? "clause " + to_string(body.clause_index)
	              : body.objective                    ? "objective"
	              : body.kind == DecideBodyKind::NONE ? "entries"
	                                                  : "members";
	if (body.kind != DecideBodyKind::NONE) {
		head += " body=" + BodyKindName(body.kind) + ":" + to_string(body.order);
	}
	return head;
}

static void ClauseLines(std::ostringstream &out, const DecideConstraint &clause, const DecideBody &body) {
	Terms(out, clause.lhs_terms, body.linear, "  ", "term");
	Groups(out, clause.product_groups, body.groups);
	out << "  bound rhs=" << Signed(body.rhs) << " lower=" << Signed(body.lower) << " upper=" << Signed(body.upper)
	    << "\n";
}

static void ObjectiveLines(std::ostringstream &out, const DecideObjective &objective, const DecideBody &body) {
	Terms(out, objective.terms, body.linear, "  ", "term");
	Groups(out, objective.product_groups, body.groups);
}

//! A program as it computes: an alias would print in place of the subtree it names.
static string Computed(const Expression &expression) {
	auto copy = expression.Copy();
	ExpressionIterator::EnumerateExpression(copy, [](Expression &child) { child.alias.clear(); });
	return copy->ToString();
}

static void ProgramLines(std::ostringstream &out, const DecideNode &node) {
	for (idx_t c = 0; c < node.program_columns.size(); c++) {
		out << "  column #" << node.columns.size() + c << " reads input "
		    << node.program_columns[c]->Cast<BoundReferenceExpression>().index << "\n";
	}
	for (idx_t h = 0; h < node.holes.size(); h++) {
		out << "  hole #" << node.columns.size() + node.program_columns.size() + h << " reads node "
		    << node.holes[h].node;
		if (node.holes[h].column != DConstants::INVALID_INDEX) {
			out << " column " << node.holes[h].column;
		}
		out << "\n";
	}
	for (idx_t p = 0; p < node.programs.size(); p++) {
		auto &program = node.programs[p];
		out << "  program " << p << " " << RoleName(program.role) << " expr=" << Computed(*program.expression)
		    << " written=" << program.written->ToString() << " owner=" << Signed(program.owner);
		for (idx_t a = 0; a < program.arguments.size(); a++) {
			out << (a ? "," : " arguments=") << program.arguments[a];
		}
		out << "\n";
	}
}

} // namespace decide_tree_compile

vector<const DecideConstraint *> DecideTree::Constraints() const {
	vector<const DecideConstraint *> result(constraint_count);
	for (auto &node : nodes) {
		auto &body = node->compiled;
		if (body.clause && body.clause_index < constraint_count) {
			result[body.clause_index] = body.clause.get();
		}
	}
	return result;
}

const DecideObjective *DecideTree::Objective() const {
	for (auto root : roots) {
		if (nodes[root]->compiled.objective) {
			return nodes[root]->compiled.objective.get();
		}
	}
	return nullptr;
}

string DecideTree::BodiesToString() const {
	std::ostringstream out;
	for (idx_t i = 0; i < nodes.size(); i++) {
		auto &node = *nodes[i];
		auto &body = node.compiled;
		if (body.kind == DecideBodyKind::NONE && node.programs.empty()) {
			continue;
		}
		out << "node " << i << " " << decide_tree_compile::Head(node) << "\n";
		if (body.clause) {
			decide_tree_compile::ClauseLines(out, *body.clause, body);
		}
		if (body.objective) {
			decide_tree_compile::ObjectiveLines(out, *body.objective, body);
		}
		decide_tree_compile::Terms(out, body.members, body.linear, "  ", "term");
		if (!body.fallback.empty()) {
			out << "  fallback\n";
			decide_tree_compile::Terms(out, body.fallback, body.fallback_sites, "    ", "term");
		}
		decide_tree_compile::ProgramLines(out, node);
	}
	return out.str();
}

} // namespace duckdb
