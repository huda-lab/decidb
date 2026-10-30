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
	//! SQUARE: the decision the inner expression is linear in.
	idx_t decision = 0;
	int sign = 1;
	//! CONSTANT: the value, and what a message names for it (DirectNamesOf).
	unique_ptr<Expression> value;
	DirectNumberNames value_names;
	//! SQUARE: `square_scale * (inner)^2`, where the inner expression is linear in one decision.
	double square_scale = 1.0;
	vector<DirectSignedValue> inner_linear;
	vector<DirectSignedValue> inner_constant;
	//! What a message names for the sums of the two (DirectNamesOf).
	DirectNumberNames inner_linear_names;
	DirectNumberNames inner_constant_names;
};

struct Part {
	//! The part's sign and constant factor, and its linear terms.
	DirectLinearPart linear;
	unique_ptr<Expression> filter;
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
	unique_ptr<Expression> objective_when;
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
	if (condition.objective_when) {
		return false;
	}
	for (auto &clause : condition.clauses) {
		if (clause.when || !constant(*clause.rhs) || !constant_values(clause.coefficient)) {
			return false;
		}
	}
	for (auto &part : condition.parts) {
		if (part.filter) {
			return false;
		}
		for (auto &term : part.linear.terms) {
			if (!constant(*term.coefficient)) {
				return false;
			}
		}
		for (auto &term : part.terms) {
			if (term.kind == Term::Kind::SQUARE) {
				if (!constant_values(term.inner_linear) || !constant_values(term.inner_constant)) {
					return false;
				}
			} else if (!constant(*term.value)) {
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
			if (part.reducer != DirectReducer::SUM || part.inner || part.qualifier != DConstants::INVALID_INDEX ||
			    !part.by.empty()) {
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
	if (objective.scope.when) {
		if (!DirectIsSourceOnlyPredicate(*objective.scope.when, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		condition.objective_when = objective.scope.when->Copy();
	}
	idx_t decision_count = ir.decisions.size();
	for (auto &part : objective.parts) {
		Part prepared;
		if (!DirectReadLinearPart(ir, context, part, objective.written, prepared.linear)) {
			return false;
		}
		for (auto &term : prepared.linear.terms) {
			if (term.decision >= decision_count) {
				return false;
			}
		}
		if (part.filter) {
			if (!DirectIsSourceOnlyPredicate(*part.filter, ir.decide_index, ir.source_bindings)) {
				return false;
			}
			prepared.filter = part.filter->Copy();
		}
		for (auto &term : part.terms) {
			if (term.kind == DecideTermKind::LINEAR) {
				continue;
			}
			Term built;
			built.sign = term.sign;
			if (term.kind == DecideTermKind::CONSTANT) {
				built.kind = Term::Kind::CONSTANT;
				if (!term.coefficient || !DirectProveValue(ir, *term.coefficient)) {
					return false;
				}
				built.value = term.coefficient->Copy();
				vector<DirectSignedValue> value;
				value.push_back({term.sign, term.coefficient->Copy()});
				built.value_names = DirectNamesOf(ir, value, objective.written);
			} else {
				built.kind = Term::Kind::SQUARE;
				built.square_scale = term.square_scale;
				if (!std::isfinite(term.square_scale) || term.square_scale == 0.0) {
					return false;
				}
				if (condition.has_square) {
					return false;
				}
				condition.has_square = true;
				bool has_decision = false;
				for (auto &inner : term.inner) {
					if (inner.kind == DecideTermKind::LINEAR) {
						if (inner.var_a >= decision_count || (has_decision && built.decision != inner.var_a)) {
							return false;
						}
						built.decision = inner.var_a;
						has_decision = true;
					} else if (inner.kind != DecideTermKind::CONSTANT) {
						return false;
					}
				}
				if (!has_decision) {
					return false;
				}
				for (auto &inner : term.inner) {
					if (!inner.coefficient || !DirectProveValue(ir, *inner.coefficient)) {
						return false;
					}
					DirectSignedValue value;
					value.sign = inner.sign;
					value.expr = inner.coefficient->Copy();
					(inner.kind == DecideTermKind::LINEAR ? built.inner_linear : built.inner_constant)
					    .push_back(std::move(value));
				}
				built.inner_linear_names = DirectNamesOf(ir, built.inner_linear, objective.written);
				built.inner_constant_names = DirectNamesOf(ir, built.inner_constant, objective.written);
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

//! One projection being assembled.
struct Stage {
	explicit Stage(idx_t index_p) : index(index_p) {
	}
	idx_t index;
	vector<unique_ptr<Expression>> expressions;

	ColumnBinding Add(unique_ptr<Expression> expression) {
		ColumnBinding binding(index, expressions.size());
		expressions.push_back(std::move(expression));
		return binding;
	}
	unique_ptr<LogicalOperator> Finish(unique_ptr<LogicalOperator> child) {
		auto projection = make_uniq<LogicalProjection>(index, std::move(expressions));
		projection->children.push_back(std::move(child));
		return std::move(projection);
	}
};

//! Builds the relational plan for one A1 problem whose condition holds, stage by stage. Each stage is one projection
//! that reads only the columns of the stage before it:
//!
//!   P1 raw values (every coefficient, bound and mask, each computed on the rows that read it and NULL elsewhere)
//!   P2 per-clause bounds, per-decision objective coefficients, and the check on every raw value
//!   P3 each decision's box
//!   P4 each decision's answer and whether its row is infeasible
//!   breaker: every row is validated before one is released
//!   P5 the source columns, the typed answers, and the breaker
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
	}

	DirectSubstitute Build(unique_ptr<LogicalOperator> source) const {
		Stage p1(binder.GenerateTableIndex());
		vector<unique_ptr<Expression>> checks;
		Raw raw;
		LayoutRawValues(p1, checks, raw);
		unique_ptr<LogicalOperator> plan = p1.Finish(std::move(source));

		Stage p2(binder.GenerateTableIndex());
		Bounds bounds;
		auto passthrough2 = Passthrough(p2, raw.source);
		ComputeBounds(p2, raw, checks, bounds);
		plan = p2.Finish(std::move(plan));

		Stage p3(binder.GenerateTableIndex());
		auto passthrough3 = Passthrough(p3, passthrough2);
		Box box;
		ComputeBox(p3, bounds, box);
		plan = p3.Finish(std::move(plan));

		Stage p4(binder.GenerateTableIndex());
		auto passthrough4 = Passthrough(p4, passthrough3);
		Answers answers;
		ComputeAnswers(p4, box, answers);
		plan = p4.Finish(std::move(plan));

		DirectSubstitute substitute;
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			substitute.prunable_decisions.push_back(1);
		}
		for (idx_t i = 0; i < source_bindings.size() + condition.decisions.size(); i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + condition.decisions.size());

		Stage p5(binder.GenerateTableIndex());
		if (condition.constant_problem) {
			// Every row sees the same values, so the first row raises whatever the others would, and nothing has to be
			// held back: the plan is pipelined, and the validation column raises wherever a row is read. An empty
			// source has no row to raise on and returns no rows. A bound the solver rejects outright (against the
			// variable's built-in domain) is never proved, so it cannot differ on an empty input.
			for (idx_t i = 0; i < source_bindings.size(); i++) {
				p5.Add(calc.Out(calc.Col(source_types[i], passthrough4[i])));
			}
			for (idx_t d = 0; d < condition.decisions.size(); d++) {
				p5.Add(TypedAnswer(d, answers.value[d]));
			}
			p5.Add(ValidAnswers(answers));
			substitute.child = p5.Finish(std::move(plan));
			substitute.pipelined = true;
			return substitute;
		}
		auto breaker = DirectValidationBreaker(binder, std::move(plan), ValidAnswers(answers));
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			p5.Add(calc.Out(calc.Col(source_types[i], breaker.input_bindings[i])));
		}
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			p5.Add(TypedAnswer(d, breaker.input_bindings[answers.value[d].column_index]));
		}
		p5.Add(calc.Out(calc.Col(LogicalType::BIGINT, breaker.breaker)));
		substitute.child = p5.Finish(std::move(breaker.plan));
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

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;

	vector<ColumnBinding> Passthrough(Stage &stage, const vector<ColumnBinding> &columns) const {
		vector<ColumnBinding> result;
		for (idx_t i = 0; i < columns.size(); i++) {
			result.push_back(stage.Add(calc.Out(calc.Col(source_types[i], columns[i]))));
		}
		return result;
	}

	//! `sum(sign * value)` over the proved values, read as DOUBLE. Null for none.
	unique_ptr<Expression> SumOf(const vector<DirectSignedValue> &values) const {
		unique_ptr<Expression> total;
		for (auto &value : values) {
			auto term = DirectScaledTerm(optimizer, value.expr->Copy(), value.sign, false, 1.0, false);
			total =
			    total ? calc.Out(calc.Add(calc.Wrap(std::move(total)), calc.Wrap(std::move(term)))) : std::move(term);
		}
		return total;
	}

	//! The check of a raw value of P1, evaluated over P1: it raises what DirectNumberMessage names, reading the values
	//! the number is computed from in the source columns P1 carries first.
	unique_ptr<Expression> ValidNumber(const Stage &p1, ColumnBinding number, const DirectNumberNames &names) const {
		auto message =
		    DirectNumberMessage(calc, calc.Col(LogicalType::DOUBLE, number), names, [&](const Expression &input) {
			    return calc.Wrap(DirectOverProjection(input, source_bindings, p1.index));
		    });
		return DirectRaiseMessage(optimizer, calc, message);
	}

	//! The check of a number computed from values checked before it: past a DOUBLE it names the objective.
	unique_ptr<Expression> TooLarge(ColumnBinding number) const {
		return DirectRaiseMessage(
		    optimizer, calc,
		    DirectNumberMessage(calc, calc.Col(LogicalType::DOUBLE, number), condition.objective_written));
	}

	//===------------------------------------------------------------------===//
	// P1: raw values
	//===------------------------------------------------------------------===//

	void LayoutRawValues(Stage &p1, vector<unique_ptr<Expression>> &checks, Raw &raw) const {
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
			auto coefficient = p1.Add(calc.Out(on_kept(calc.Wrap(SumOf(clause.coefficient)))));
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
			checks.push_back(read_on(ValidNumber(p1, coefficient, clause.coefficient_names)));
			checks.push_back(read_on(DirectValidBoundPredicate(optimizer, rhs, clause.rhs_text)));
			raw.clauses.push_back(columns);
		}
		bool has_shared_mask = false;
		ColumnBinding shared_mask;
		for (auto &part : condition.parts) {
			Raw::PartColumns columns;
			// The rows the part reads, as an expression over the source; null for every row.
			ExprPtr kept;
			if (condition.objective_when || part.filter) {
				columns.has_mask = true;
				kept = calc.Wrap(DirectActiveWhen(calc, condition.objective_when.get()));
				if (part.filter) {
					kept = calc.If(kept, calc.Wrap(DirectActiveWhen(calc, part.filter.get())), calc.Bool(false));
				}
				if (!part.filter && has_shared_mask) {
					columns.mask = shared_mask;
				} else {
					columns.mask = p1.Add(calc.Out(kept));
					if (!part.filter) {
						has_shared_mask = true;
						shared_mask = columns.mask;
					}
				}
			}
			AddObjectiveValues(p1, checks, part, columns, kept);
			raw.parts.push_back(std::move(columns));
		}
	}

	void AddObjectiveValues(Stage &p1, vector<unique_ptr<Expression>> &all_checks, const Part &part,
	                        Raw::PartColumns &columns, const ExprPtr &kept) const {
		// A value is computed and checked only on the rows the part keeps.
		vector<unique_ptr<Expression>> checks;
		auto on_kept = [&](ExprPtr value) {
			return kept ? DirectOnKeptRows(calc, kept, value) : value;
		};
		// Linear coefficients add up per decision. A constant adds nothing to the answer, so like the solver path it
		// is read one term at a time, with the part's factor applied, and never added up; each must still be valid,
		// and is checked after the sums.
		vector<ExprPtr> linear_sums(condition.decisions.size());
		vector<unique_ptr<Expression>> constant_checks;
		for (auto &term : part.linear.terms) {
			auto value = calc.Wrap(DirectScaledTerm(optimizer, term.coefficient->Copy(), term.sign, false, 1.0, false));
			auto &sum = linear_sums[term.decision];
			sum = sum ? calc.Add(sum, value) : value;
		}
		for (auto &term : part.terms) {
			if (term.kind == Term::Kind::CONSTANT) {
				auto value = calc.Wrap(DirectScaledTerm(optimizer, term.value->Copy(), term.sign, part.linear.scaled,
				                                        part.linear.scale, part.linear.divides));
				constant_checks.push_back(ValidNumber(p1, p1.Add(calc.Out(on_kept(value))), term.value_names));
			} else {
				Raw::SquareColumns square;
				square.decision = term.decision;
				square.scale = term.sign * term.square_scale;
				square.linear = p1.Add(calc.Out(on_kept(calc.Wrap(SumOf(term.inner_linear)))));
				checks.push_back(ValidNumber(p1, square.linear, term.inner_linear_names));
				if (!term.inner_constant.empty()) {
					square.constant = p1.Add(calc.Out(on_kept(calc.Wrap(SumOf(term.inner_constant)))));
					checks.push_back(ValidNumber(p1, square.constant, term.inner_constant_names));
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
			checks.push_back(ValidNumber(p1, binding, part.linear.names[d]));
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

	void ComputeBounds(Stage &p2, const Raw &raw, vector<unique_ptr<Expression>> &checks, Bounds &bounds) const {
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
				checks.push_back(DirectRaiseMessage(
				    optimizer, calc, DirectNumberMessage(calc, linear_total, condition.objective_written)));
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

	void ComputeBox(Stage &p3, const Bounds &bounds, Box &box) const {
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

	void ComputeAnswers(Stage &p4, const Box &box, Answers &answers) const {
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
			ExprPtr turning = whole ? calc.Fn("ceil", calc.Div(calc.Sub(calc.Neg(b), a), calc.Mul(calc.Num(2.0), a)))
			                        : calc.Div(calc.Neg(b), calc.Mul(calc.Num(2.0), a));
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

	//! The condition the breaker evaluates on every row. It is true, or the row raises the error that applies: a
	//! coefficient the arithmetic made non-finite, an empty box, an improvement without limit, or a whole number the
	//! answer cannot hold exactly. Every raw value was checked in P2 and arrives as `valid_data`.
	unique_ptr<Expression> ValidAnswers(const Answers &answers) const {
		vector<unique_ptr<Expression>> checks;
		// The raw values come first: P2 folded their checks into one flag that is true or false, never an error.
		checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, answers.valid_data)));
		for (idx_t d = 0; d < condition.decisions.size(); d++) {
			auto value = [&]() {
				return calc.Col(LogicalType::DOUBLE, answers.value[d]);
			};
			checks.push_back(TooLarge(answers.a[d]));
			checks.push_back(TooLarge(answers.b[d]));
			checks.push_back(DirectRaiseIf(calc, calc.Out(calc.Col(LogicalType::BOOLEAN, answers.infeasible[d])),
			                               DirectInfeasibleErrorPredicate(optimizer)));
			checks.push_back(DirectRaiseUnless(calc, calc.Out(calc.Fn("isfinite", value())),
			                                   DirectUnboundedErrorPredicate(optimizer)));
			if (DirectIsWholeNumbered(condition.decisions[d].domain)) {
				checks.push_back(DirectRaiseUnless(calc, WithinExactRange(answers.value[d]),
				                                   OutOfRangeError(condition.decisions[d].name, answers.value[d])));
			}
		}
		return DirectAllHold(calc, std::move(checks));
	}

	//! The answer in the decision's own output type. The breaker has already raised for anything that is not finite or
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
	return DirectBuildOverOneRead(
	    reader, [&](unique_ptr<LogicalOperator> rows) { return plan_builder.Build(std::move(rows)); });
}

} // namespace direct_a1_separable_box

unique_ptr<DirectSolveRule> MakeA1SeparableBoxRule() {
	return make_uniq<direct_a1_separable_box::Rule>();
}

} // namespace duckdb
