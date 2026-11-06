#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_a1_separable_box {

constexpr const char *RULE_NAME = "A1_SEPARABLE_BOX";

//! A term of an objective part that is not linear: a constant, or a square. The linear terms are read by the shared
//! DirectReadLinearPart.
struct Term {
	enum class Kind : uint8_t { CONSTANT, SQUARE };
	Kind kind = Kind::CONSTANT;
	DirectConstantTerm constant;
	DirectSquareTerm square;
};

struct Part {
	//! The part's sign and constant factor, and its linear terms.
	DirectLinearPart linear;
	//! The rows the part reads (DirectObjectivePartKept): the objective's `when` and the part's own filter. Null for
	//! every row.
	unique_ptr<Expression> kept;
	//! The constant and square terms, in the order they came.
	vector<Term> terms;
};

struct Decision {
	//! The declared name, which the whole-number range error quotes.
	string name;
	DirectDomain domain = DirectDomain::REAL;
	LogicalType output_type;
	//! The decision's box before any per-row bound: its default, replaced or tightened by the bounds the solver folds
	//! into the variable.
	double lower = 0.0;
	double upper = POSITIVE_INFINITY;
};

struct Condition final : DirectApplicabilityCondition {
	idx_t decide_index = 0;
	DecideSense sense = DecideSense::FEASIBILITY;
	//! The objective as written, which a message names when a decision's quadratic or linear coefficient, computed from
	//! finite values, is too large for a DOUBLE (TOO_LARGE), as the solver path names it.
	string objective_written;
	vector<Decision> decisions;
	vector<DirectBoundClause> clauses;
	vector<Part> parts;
	bool has_square = false;
	//! Every value is a constant and nothing is conditional, so a check that fails on one row fails on all of them.
	bool constant_problem = false;
};

//! No value reads a source column and nothing is conditional: every row sees the same bounds and coefficients.
bool IsConstantProblem(const Condition &condition) {
	auto constant = [](const Expression &expr) {
		return expr.IsFoldable();
	};
	auto constant_values = [&](const vector<DirectSignedValue> &values) {
		for (auto &value : values) {
			if (!constant(*value.expr)) {
				return false;
			}
		}
		return true;
	};
	for (auto &clause : condition.clauses) {
		if (clause.when || !constant(*clause.rhs) || !constant_values(clause.coefficient)) {
			return false;
		}
	}
	for (auto &part : condition.parts) {
		if (part.kept) {
			return false;
		}
		for (auto &term : part.linear.terms) {
			if (!constant(*term.coefficient)) {
				return false;
			}
		}
		for (auto &term : part.terms) {
			if (term.kind == Term::Kind::SQUARE) {
				if (!constant_values(term.square.inner_linear) || !constant_values(term.square.inner_constant)) {
					return false;
				}
			} else if (!constant(*term.constant.value)) {
				return false;
			}
		}
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Rule
//===--------------------------------------------------------------------===//

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}

	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (ir.decisions.empty() || !ir.keyed_scopes.empty()) {
			return nullptr;
		}
		for (auto &decision : ir.decisions) {
			if (decision.scope != DecideVarScope::ROW) {
				return nullptr;
			}
		}
		for (auto &ir_node : ir.constraints) {
			if (ir_node.aggregate) {
				return nullptr;
			}
			if (!DirectIsSingletonRow(ir, ir_node)) {
				return nullptr;
			}
		}
		for (auto &part : ir.objective.parts) {
			if (!part.IsPlainSum()) {
				return nullptr;
			}
			for (auto &term : part.terms) {
				if (term.kind != DecideTermKind::LINEAR && term.kind != DecideTermKind::CONSTANT &&
				    term.kind != DecideTermKind::SQUARE) {
					return nullptr;
				}
			}
		}
		auto condition = make_uniq<Condition>();
		condition->decide_index = ir.decide_index;
		condition->sense = ir.objective.sense;
		condition->objective_written = ir.objective.written;
		for (auto &decision : ir.decisions) {
			Decision prepared;
			prepared.name = decision.name;
			prepared.domain = decision.domain;
			prepared.output_type = decision.output_type;
			condition->decisions.push_back(std::move(prepared));
		}
		if (!ProveBounds(ir, context, *condition) || !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		condition->constant_problem = IsConstantProblem(*condition);
		// The rows are read once to validate them and once to answer, unless every row raises the same error.
		condition->source_reads = condition->constant_problem ? 1 : 2;
		return std::move(condition);
	}

	string Summary() const override {
		return "min/max SUM(c*x), l <= x <= u, each row independent";
	}

	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate) const override;

private:
	bool ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! The decision's box before any per-row bound, and the clauses the plan evaluates per row, from the shared reader.
bool Rule::ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	DirectBoundReading reading;
	if (!DirectReadBounds(ir, context, reading)) {
		return false;
	}
	condition.clauses = std::move(reading.clauses);
	for (idx_t decision = 0; decision < condition.decisions.size(); decision++) {
		condition.decisions[decision].lower = reading.boxes[decision].lower;
		condition.decisions[decision].upper = reading.boxes[decision].upper;
	}
	return true;
}

bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	auto &objective = ir.objective;
	idx_t decision_count = ir.decisions.size();
	for (auto &part : objective.parts) {
		Part prepared;
		if (!DirectReadLinearPart(ir, context, part, objective.written, prepared.linear) ||
		    !DirectObjectivePartKept(ir, part, prepared.kept)) {
			return false;
		}
		for (auto &term : prepared.linear.terms) {
			if (term.decision >= decision_count) {
				return false;
			}
		}
		for (auto &term : part.terms) {
			if (term.kind == DecideTermKind::LINEAR) {
				continue;
			}
			Term built;
			if (term.kind == DecideTermKind::CONSTANT) {
				built.kind = Term::Kind::CONSTANT;
				if (!DirectReadConstantTerm(ir, term, objective.written, built.constant)) {
					return false;
				}
			} else {
				built.kind = Term::Kind::SQUARE;
				if (condition.has_square || !DirectReadSquareTerm(ir, term, objective.written, built.square)) {
					return false;
				}
				condition.has_square = true;
			}
			prepared.terms.push_back(std::move(built));
		}
		condition.parts.push_back(std::move(prepared));
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//

//! Builds the relational plan for one A1 problem whose condition holds, stage by stage. Each stage is one projection
//! that reads only the columns of the stage before it:
//!
//!   P1 raw values (every coefficient, bound and mask, each computed on the rows that read it and NULL elsewhere)
//!   P2 per-clause bounds, per-decision objective coefficients, and the check on every raw value
//!   P3 each decision's box
//!   P4 each decision's answer and whether its row is infeasible
//!
//! The solver reads every row before it returns, so a bad value on the last row raises even under an outer LIMIT 1.
//! The plan keeps that obligation without holding rows (verify, then stream). When the rows differ, one read runs
//! P1-P4 into a verdict, `bool_and` of every row's checks, true or raising, and the reader gates the second read on
//! it (DirectSourceReader::Gate): that read runs P1-P4 again and releases each row as it comes. When every row sees
//! the same values, the first row raises whatever the others would and the plan is one pipelined read.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
	}

	//! One row: true when every row of `source` is valid, and otherwise the error of a row that is not.
	unique_ptr<LogicalOperator> BuildVerdict(unique_ptr<LogicalOperator> source) const {
		auto stages = RunStages(std::move(source));
		return DirectAllRowsValid(optimizer, std::move(stages.plan), ValidAnswers(stages.answers));
	}

	//! The source columns and the answers. A source that is read twice has already been validated by the verdict.
	DirectSubstitute Build(unique_ptr<LogicalOperator> source) const {
		auto stages = RunStages(std::move(source));
		DirectSubstitute substitute;
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			substitute.prunable_decisions.push_back(1);
		}
		for (idx_t i = 0; i < source_bindings.size() + condition.decisions.size(); i++) {
			substitute.slot_map.push_back(i);
		}
		DirectProjectionStage p5(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p5.Add(calc.Out(calc.Col(source_types[i], stages.passthrough[i])));
		}
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			p5.Add(TypedAnswer(d, stages.answers.value[d]));
		}
		if (condition.constant_problem) {
			// The validation column raises wherever a row is read. An empty source has no row to raise on and returns
			// no rows. A bound the solver rejects outright (against the variable's built-in domain) is never proved,
			// so it cannot differ on an empty input.
			substitute.validated_columns.push_back(source_bindings.size() + condition.decisions.size());
			p5.Add(ValidAnswers(stages.answers));
		}
		substitute.child = p5.Finish(std::move(stages.plan));
		substitute.pipelined = true;
		return substitute;
	}

private:
	//! Columns of P1.
	struct Raw {
		vector<ColumnBinding> source;
		vector<DirectBoundColumns> clauses;
		struct SquareColumns {
			idx_t decision = 0;
			double scale = 1.0;
			ColumnBinding linear;
			ColumnBinding constant;
		};
		struct PartColumns {
			bool has_mask = false;
			ColumnBinding mask;
			//! The sum of the linear coefficients of each decision, with their signs.
			vector<idx_t> linear_decision;
			vector<ColumnBinding> linear;
			vector<SquareColumns> squares;
		};
		vector<PartColumns> parts;
	};

	//! Columns of P2: each clause's bounds on its decision, and each decision's objective coefficients.
	struct Bounds {
		vector<ColumnBinding> lower;
		vector<ColumnBinding> upper;
		//! Each decision's static lower end on the row (DirectStaticFloor).
		vector<ColumnBinding> floor;
		vector<ColumnBinding> a;
		vector<ColumnBinding> b;
		ColumnBinding valid_data;
	};

	//! Columns of P3.
	struct Box {
		vector<ColumnBinding> lower;
		vector<ColumnBinding> upper;
		vector<ColumnBinding> a;
		vector<ColumnBinding> b;
		ColumnBinding valid_data;
	};

	//! Columns of P4.
	struct Answers {
		vector<ColumnBinding> value;
		vector<ColumnBinding> infeasible;
		vector<ColumnBinding> a;
		vector<ColumnBinding> b;
		ColumnBinding valid_data;
	};

	//! P1-P4 over a read: the plan, where each source column is in it, and the answers.
	struct Stages {
		unique_ptr<LogicalOperator> plan;
		vector<ColumnBinding> passthrough;
		Answers answers;
	};

	Stages RunStages(unique_ptr<LogicalOperator> source) const {
		DirectProjectionStage p1(binder.GenerateTableIndex());
		vector<unique_ptr<Expression>> checks;
		Raw raw;
		LayoutRawValues(p1, checks, raw);
		Stages stages;
		stages.plan = p1.Finish(std::move(source));

		DirectProjectionStage p2(binder.GenerateTableIndex());
		Bounds bounds;
		auto passthrough2 = Passthrough(p2, raw.source);
		ComputeBounds(p2, raw, checks, bounds);
		stages.plan = p2.Finish(std::move(stages.plan));

		DirectProjectionStage p3(binder.GenerateTableIndex());
		auto passthrough3 = Passthrough(p3, passthrough2);
		Box box;
		ComputeBox(p3, bounds, box);
		stages.plan = p3.Finish(std::move(stages.plan));

		DirectProjectionStage p4(binder.GenerateTableIndex());
		stages.passthrough = Passthrough(p4, passthrough3);
		ComputeAnswers(p4, box, stages.answers);
		stages.plan = p4.Finish(std::move(stages.plan));
		return stages;
	}

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;

	vector<ColumnBinding> Passthrough(DirectProjectionStage &stage, const vector<ColumnBinding> &columns) const {
		vector<ColumnBinding> result;
		for (idx_t i = 0; i < columns.size(); i++) {
			result.push_back(stage.Add(calc.Out(calc.Col(source_types[i], columns[i]))));
		}
		return result;
	}

	//===------------------------------------------------------------------===//
	// P1: raw values
	//===------------------------------------------------------------------===//

	void LayoutRawValues(DirectProjectionStage &p1, vector<unique_ptr<Expression>> &checks, Raw &raw) const {
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			raw.source.push_back(p1.Add(calc.Out(calc.Col(source_types[i], source_bindings[i]))));
		}
		for (auto &clause : condition.clauses) {
			DirectBoundColumns columns;
			// A value is computed only on the rows the clause keeps.
			auto on_kept = [&](ExprPtr value) {
				return clause.when ? DirectOnKeptRows(calc, calc.Wrap(DirectActiveWhen(calc, clause.when.get())), value)
				                   : value;
			};
			auto coefficient = p1.Add(calc.Out(on_kept(calc.Wrap(DirectSumOf(optimizer, clause.coefficient)))));
			auto rhs = p1.Add(calc.Out(on_kept(calc.Cast(calc.Of(*clause.rhs), LogicalType::DOUBLE))));
			columns.lower = p1.Add(calc.Out(on_kept(calc.Of(*clause.lower))));
			columns.upper = p1.Add(calc.Out(on_kept(calc.Of(*clause.upper))));
			if (clause.when) {
				columns.has_when = true;
				columns.when = p1.Add(DirectActiveWhen(calc, clause.when.get()));
			}
			auto read_on = [&](unique_ptr<Expression> check) {
				return columns.has_when
				           ? DirectCheckOnKeptRows(calc, calc.Out(calc.Col(LogicalType::BOOLEAN, columns.when)),
				                                   std::move(check))
				           : std::move(check);
			};
			if (auto check = DirectCheckNumber(optimizer, coefficient, clause.coefficient_names, clause.coefficient,
			                                   source_bindings, p1.index)) {
				checks.push_back(read_on(std::move(check)));
			}
			checks.push_back(read_on(DirectValidBoundPredicate(optimizer, rhs, clause.rhs_text)));
			raw.clauses.push_back(columns);
		}
		for (auto &part : condition.parts) {
			Raw::PartColumns columns;
			// The rows the part reads, as an expression over the source; null for every row.
			ExprPtr kept;
			if (part.kept) {
				columns.has_mask = true;
				kept = calc.Wrap(DirectActiveWhen(calc, part.kept.get()));
				columns.mask = p1.Add(calc.Out(kept));
			}
			AddObjectiveValues(p1, checks, part, columns, kept);
			raw.parts.push_back(std::move(columns));
		}
	}

	void AddObjectiveValues(DirectProjectionStage &p1, vector<unique_ptr<Expression>> &all_checks, const Part &part,
	                        Raw::PartColumns &columns, const ExprPtr &kept) const {
		// A value is computed and checked only on the rows the part keeps.
		vector<unique_ptr<Expression>> checks;
		auto on_kept = [&](ExprPtr value) {
			return DirectOnKeptRows(calc, kept, value);
		};
		// Linear coefficients add up per decision. A constant adds nothing to the answer, so like the solver path it
		// is read one term at a time, with the part's factor applied, and never added up; each must still be valid,
		// and is checked after the sums.
		vector<ExprPtr> linear_sums(condition.decisions.size());
		vector<vector<DirectSignedValue>> linear_values(condition.decisions.size());
		vector<unique_ptr<Expression>> constant_checks;
		for (auto &term : part.linear.terms) {
			auto value = calc.Wrap(DirectScaledTerm(optimizer, term.coefficient->Copy(), term.sign, false, 1.0, false));
			auto &sum = linear_sums[term.decision];
			sum = sum ? calc.Add(sum, value) : value;
			linear_values[term.decision].push_back({term.sign, term.coefficient->Copy()});
		}
		for (auto &term : part.terms) {
			if (term.kind == Term::Kind::CONSTANT) {
				auto &constant = term.constant;
				auto value = calc.Wrap(DirectScaledTerm(optimizer, constant.value->Copy(), constant.sign,
				                                        part.linear.scaled, part.linear.scale, part.linear.divides));
				vector<DirectSignedValue> read;
				read.push_back({constant.sign, constant.value->Copy()});
				if (auto check = DirectCheckNumber(optimizer, p1.Add(calc.Out(on_kept(value))), constant.value_names,
				                                   read, source_bindings, p1.index, part.linear.scaled,
				                                   part.linear.scale, part.linear.divides)) {
					constant_checks.push_back(std::move(check));
				}
			} else {
				auto &read = term.square;
				Raw::SquareColumns square;
				square.decision = read.decision;
				square.scale = read.sign * read.scale;
				square.linear = p1.Add(calc.Out(on_kept(calc.Wrap(DirectSumOf(optimizer, read.inner_linear)))));
				if (auto check = DirectCheckNumber(optimizer, square.linear, read.inner_linear_names, read.inner_linear,
				                                   source_bindings, p1.index)) {
					checks.push_back(std::move(check));
				}
				if (!read.inner_constant.empty()) {
					square.constant = p1.Add(calc.Out(on_kept(calc.Wrap(DirectSumOf(optimizer, read.inner_constant)))));
					if (auto check = DirectCheckNumber(optimizer, square.constant, read.inner_constant_names,
					                                   read.inner_constant, source_bindings, p1.index)) {
						checks.push_back(std::move(check));
					}
				} else {
					square.constant = p1.Add(calc.Out(calc.Num(0.0)));
				}
				columns.squares.push_back(square);
			}
		}
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			if (!linear_sums[d]) {
				continue;
			}
			auto binding = p1.Add(calc.Out(on_kept(linear_sums[d])));
			if (auto check = DirectCheckNumber(optimizer, binding, part.linear.names[d], linear_values[d],
			                                   source_bindings, p1.index)) {
				checks.push_back(std::move(check));
			}
			columns.linear_decision.push_back(d);
			columns.linear.push_back(binding);
		}
		for (auto &check : constant_checks) {
			checks.push_back(std::move(check));
		}
		for (auto &check : checks) {
			all_checks.push_back(
			    columns.has_mask ? DirectCheckOnKeptRows(calc, calc.Out(calc.Col(LogicalType::BOOLEAN, columns.mask)),
			                                             std::move(check))
			                     : std::move(check));
		}
	}

	//===------------------------------------------------------------------===//
	// P2: per-clause bounds, objective coefficients, raw-value checks
	//===------------------------------------------------------------------===//

	void ComputeBounds(DirectProjectionStage &p2, const Raw &raw, vector<unique_ptr<Expression>> &checks,
	                   Bounds &bounds) const {
		for (idx_t j = 0; j < condition.clauses.size(); j++) {
			ExprPtr lower, upper;
			DirectClauseBounds(calc, raw.clauses[j], lower, upper);
			bounds.lower.push_back(p2.Add(calc.Out(lower)));
			bounds.upper.push_back(p2.Add(calc.Out(upper)));
		}
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			// The default floor of 0 is lowered on the rows a clause with a lower bound under a `when` keeps.
			vector<ColumnBinding> lowering_whens;
			for (idx_t j = 0; j < condition.clauses.size(); j++) {
				if (condition.clauses[j].decision == d && condition.clauses[j].lifts_floor) {
					D_ASSERT(raw.clauses[j].has_when);
					lowering_whens.push_back(raw.clauses[j].when);
				}
			}
			bounds.floor.push_back(
			    p2.Add(calc.Out(DirectStaticFloor(calc, condition.decisions[d].lower, lowering_whens))));
		}
		double sense = condition.sense == DecideSense::MAXIMIZE ? -1.0 : 1.0;
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			ExprPtr a_total = calc.Num(0.0);
			ExprPtr b_total = calc.Num(0.0);
			// The linear coefficient the parts' like terms merge into, `b + b` for SUM(b * x) + SUM(b * x), as the
			// solver path reads it. P1 checked each value, so a total past a DOUBLE names the objective.
			ExprPtr linear_total = calc.Num(0.0);
			bool has_linear = false;
			for (idx_t p = 0; p < condition.parts.size(); p++) {
				auto &part = condition.parts[p];
				auto &columns = raw.parts[p];
				ExprPtr a_part = calc.Num(0.0);
				ExprPtr linear_part = calc.Num(0.0);
				ExprPtr cross_part = calc.Num(0.0);
				bool touched = false;
				for (idx_t i = 0; i < columns.linear.size(); i++) {
					if (columns.linear_decision[i] == d) {
						linear_part = calc.Add(linear_part, calc.Col(LogicalType::DOUBLE, columns.linear[i]));
						touched = true;
						has_linear = true;
					}
				}
				for (auto &square : columns.squares) {
					if (square.decision != d) {
						continue;
					}
					auto k = calc.Num(square.scale);
					auto w = calc.Col(LogicalType::DOUBLE, square.linear);
					auto t = calc.Col(LogicalType::DOUBLE, square.constant);
					a_part = calc.Add(a_part, calc.Mul(k, calc.Mul(w, w)));
					cross_part = calc.Add(cross_part, calc.Mul(calc.Num(2.0), calc.Mul(k, calc.Mul(w, t))));
					touched = true;
				}
				if (!touched) {
					continue;
				}
				auto scaled = [&](ExprPtr value) {
					auto result =
					    calc.Wrap(DirectScaledTerm(optimizer, calc.Out(value), part.linear.sign, part.linear.scaled,
					                               part.linear.scale, part.linear.divides));
					return columns.has_mask
					           ? calc.If(calc.Col(LogicalType::BOOLEAN, columns.mask), result, calc.Num(0.0))
					           : result;
				};
				auto linear_scaled = scaled(linear_part);
				a_total = calc.Add(a_total, scaled(a_part));
				b_total = calc.Add(b_total, calc.Add(linear_scaled, scaled(cross_part)));
				linear_total = calc.Add(linear_total, linear_scaled);
			}
			if (has_linear) {
				checks.push_back(DirectRaiseTooLarge(optimizer, linear_total, condition.objective_written));
			}
			bounds.a.push_back(p2.Add(calc.Out(calc.Mul(calc.Num(sense), a_total))));
			bounds.b.push_back(p2.Add(calc.Out(calc.Mul(calc.Num(sense), b_total))));
		}
		// Every check is true or raises; a row passes when all of them do.
		bounds.valid_data = p2.Add(DirectAllHold(calc, std::move(checks)));
	}

	//===------------------------------------------------------------------===//
	// P3: each decision's box
	//===------------------------------------------------------------------===//

	void ComputeBox(DirectProjectionStage &p3, const Bounds &bounds, Box &box) const {
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			vector<ColumnBinding> clause_lowers, clause_uppers;
			for (idx_t j = 0; j < condition.clauses.size(); j++) {
				if (condition.clauses[j].decision == d) {
					clause_lowers.push_back(bounds.lower[j]);
					clause_uppers.push_back(bounds.upper[j]);
				}
			}
			ExprPtr lower, upper;
			DirectRowBox(calc, calc.Col(LogicalType::DOUBLE, bounds.floor[d]), condition.decisions[d].upper,
			             clause_lowers, clause_uppers, lower, upper);
			box.lower.push_back(p3.Add(calc.Out(lower)));
			box.upper.push_back(p3.Add(calc.Out(upper)));
			box.a.push_back(p3.Add(calc.Out(calc.Col(LogicalType::DOUBLE, bounds.a[d]))));
			box.b.push_back(p3.Add(calc.Out(calc.Col(LogicalType::DOUBLE, bounds.b[d]))));
		}
		box.valid_data = p3.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, bounds.valid_data)));
	}

	//===------------------------------------------------------------------===//
	// P4: each decision's answer
	//===------------------------------------------------------------------===//

	void ComputeAnswers(DirectProjectionStage &p4, const Box &box, Answers &answers) const {
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			auto lower = calc.Col(LogicalType::DOUBLE, box.lower[d]);
			auto upper = calc.Col(LogicalType::DOUBLE, box.upper[d]);
			auto a = calc.Col(LogicalType::DOUBLE, box.a[d]);
			auto b = calc.Col(LogicalType::DOUBLE, box.b[d]);
			auto zero = calc.Num(0.0);
			bool whole = DirectIsWholeNumbered(condition.decisions[d].domain);

			// No row holds when the box is empty; the answer is then never read.
			auto infeasible = DirectBoxIsEmpty(calc, lower, upper);

			// Convex: the turning point, clamped into the box (for whole numbers, the first integer whose next step
			// no longer improves, which keeps the smaller one on a tie).
			// The halves are taken before the division: 2*a passes a DOUBLE for |a| above 9e307, and -0.5*b cannot.
			auto half_down = calc.Mul(calc.Num(-0.5), b);
			ExprPtr turning = whole ? calc.Fn("ceil", calc.Div(calc.Sub(half_down, calc.Mul(calc.Num(0.5), a)), a))
			                        : calc.Div(half_down, a);
			auto convex = calc.Min({calc.Max({turning, lower}), upper});
			// Concave: the better endpoint, from the sign of f(upper) - f(lower) = (upper - lower) * s.
			auto s = calc.Add(calc.Mul(a, calc.Add(upper, lower)), b);
			auto concave = calc.If(calc.Cmp(ExpressionType::COMPARE_LESSTHAN, s, zero), upper, lower);
			// Linear: the end the slope points away from. With no slope at all the lowest value allowed, or zero
			// clamped into the box when the box is open below.
			auto flat = calc.If(calc.Fn("isfinite", lower), lower, calc.Min({calc.Max({zero, lower}), upper}));
			auto linear = calc.If(calc.Cmp(ExpressionType::COMPARE_LESSTHAN, b, zero), upper,
			                      calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, b, zero), lower, flat));
			auto chosen = calc.If(calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, a, zero), convex,
			                      calc.If(calc.Cmp(ExpressionType::COMPARE_LESSTHAN, a, zero), concave, linear));
			// An end that is not finite means the objective improves without limit. Adding 0.0 turns -0.0 into 0.0.
			auto value = calc.Add(chosen, zero);

			answers.infeasible.push_back(p4.Add(calc.Out(infeasible)));
			answers.a.push_back(p4.Add(calc.Out(calc.Col(LogicalType::DOUBLE, box.a[d]))));
			answers.b.push_back(p4.Add(calc.Out(calc.Col(LogicalType::DOUBLE, box.b[d]))));
			answers.value.push_back(p4.Add(calc.Out(value)));
		}
		answers.valid_data = p4.Add(calc.Out(calc.Col(LogicalType::BOOLEAN, box.valid_data)));
	}

	//! The answer rounded as the solver rounds it (`round`, half away from zero).
	unique_ptr<Expression> RoundedAnswer(ColumnBinding value) const {
		return calc.Out(calc.Fn("round", calc.Col(LogicalType::DOUBLE, value)));
	}

	//! `abs(round(value)) <= 2^53`, the SQL spelling of `DecideRoundedAnswerWithin(value, -2^53, 2^53)`, the test the
	//! solver path applies to an answer on its way into a BIGINT column. Callers test finiteness first, so an infinite
	//! or NaN value never reaches it.
	unique_ptr<Expression> WithinExactRange(ColumnBinding value) const {
		return calc.Out(calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                         calc.Fn("abs", calc.Wrap(RoundedAnswer(value))), calc.Num(DECIDE_MAX_EXACT_INTEGER)));
	}

	//! The solver's error for an answer beyond the exact range, character for character: the name is filled in now and
	//! the value (`%.0f` of the rounded answer, as the solver prints it) at run time.
	unique_ptr<Expression> OutOfRangeError(const string &name, ColumnBinding value) const {
		vector<ExprPtr> parts {calc.Constant(Value(DecideWholeNumberRangeMessageBefore(name))),
		                       calc.Fn("printf", calc.Constant(Value("%.0f")), calc.Wrap(RoundedAnswer(value))),
		                       calc.Constant(Value(DecideWholeNumberRangeMessageAfter(name)))};
		return DirectErrorPredicate(optimizer, calc.Out(calc.Fn("concat", parts)));
	}

	//! The condition every row must meet. It is true, or the row raises the error that applies: a coefficient the
	//! arithmetic made non-finite, an empty box, an improvement without limit, or a whole number the answer cannot hold
	//! exactly. Every raw value was checked in P2 and arrives as `valid_data`. A row that passes them all may warn
	//! (LosesPrecision).
	unique_ptr<Expression> ValidAnswers(const Answers &answers) const {
		vector<unique_ptr<Expression>> checks;
		// The raw values come first: P2 folded their checks into one flag that is true or false, never an error.
		checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, answers.valid_data)));
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			auto value = [&]() {
				return calc.Col(LogicalType::DOUBLE, answers.value[d]);
			};
			checks.push_back(DirectRaiseTooLarge(optimizer, calc.Col(LogicalType::DOUBLE, answers.a[d]),
			                                     condition.objective_written));
			checks.push_back(DirectRaiseTooLarge(optimizer, calc.Col(LogicalType::DOUBLE, answers.b[d]),
			                                     condition.objective_written));
			checks.push_back(DirectRaiseIf(calc, calc.Out(calc.Col(LogicalType::BOOLEAN, answers.infeasible[d])),
			                               DirectInfeasibleErrorPredicate(optimizer)));
			checks.push_back(DirectRaiseUnless(calc, calc.Out(calc.Fn("isfinite", value())),
			                                   DirectUnboundedErrorPredicate(optimizer)));
			if (DirectIsWholeNumbered(condition.decisions[d].domain)) {
				checks.push_back(DirectRaiseUnless(calc, WithinExactRange(answers.value[d]),
				                                   OutOfRangeError(condition.decisions[d].name, answers.value[d])));
			}
		}
		checks.push_back(LosesPrecision(answers));
		return DirectAllHold(calc, std::move(checks));
	}

	//! A decision with a square divides its linear coefficient by its square one (the turning point) or weighs the two
	//! against each other (the better end), so either one below the normal range of a DOUBLE has lost precision there,
	//! and the row warns. With no square the answer reads only the sign of the linear coefficient, which holds.
	unique_ptr<Expression> LosesPrecision(const Answers &answers) const {
		vector<ExprPtr> unstable;
		auto zero = calc.Num(0.0);
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			auto a = calc.Col(LogicalType::DOUBLE, answers.a[d]);
			auto b = calc.Col(LogicalType::DOUBLE, answers.b[d]);
			auto has_square = calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, a, zero);
			auto has_linear = calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, b, zero);
			auto small_linear = calc.If(has_linear, DirectBelowNormalRange(calc, b), calc.Bool(false));
			unstable.push_back(
			    calc.If(has_square, calc.Any({DirectBelowNormalRange(calc, a), small_linear}), calc.Bool(false)));
		}
		return DirectWarnIf(calc, calc.Out(calc.Any(unstable)));
	}

	//! The answer in the decision's own output type. The verdict has already raised for anything that is not finite or
	//! not exactly representable, so the guard only keeps the cast from raising on its own if that ever changed.
	unique_ptr<Expression> TypedAnswer(idx_t decision, ColumnBinding value) const {
		auto &prepared = condition.decisions[decision];
		if (!DirectIsWholeNumbered(prepared.domain)) {
			return calc.Out(calc.Col(LogicalType::DOUBLE, value));
		}
		auto column = calc.Col(LogicalType::DOUBLE, value);
		auto finite = calc.If(calc.Fn("isfinite", column), calc.Wrap(WithinExactRange(value)), calc.Bool(false));
		return calc.Out(
		    calc.If(finite, calc.Cast(column, prepared.output_type), calc.Constant(Value(prepared.output_type))));
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes());
	if (!condition.constant_problem) {
		// Verify, then stream: the verdict is read first, and every read after it waits for it.
		reader.Gate(DirectBuildOverRead(reader, reader.ReadSource(), [&](unique_ptr<LogicalOperator> rows) {
			return plan_builder.BuildVerdict(std::move(rows));
		}));
	}
	return DirectBuildOverOneRead(
	    reader, [&](unique_ptr<LogicalOperator> rows) { return plan_builder.Build(std::move(rows)); });
}

} // namespace direct_a1_separable_box

unique_ptr<DirectSolveRule> MakeA1SeparableBoxRule() {
	return make_uniq<direct_a1_separable_box::Rule>();
}

} // namespace duckdb
