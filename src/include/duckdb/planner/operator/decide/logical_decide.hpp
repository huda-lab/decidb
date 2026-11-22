//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/planner/operator/decide/logical_decide.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/decide_value_messages.hpp"

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/decide_solver_capabilities.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/planner/decide/decide_tree_terms.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"

namespace duckdb {

//! One column of a key: a data expression and how it compares NULL.
struct DecideKeyElement {
	//! The column or data expression, as bound.
	unique_ptr<Expression> expression;
	//! How a message names it: as written for a column element (`g`), qualified by its
	//! relation (`D.depotID`) for a column a relation element stands for.
	string name;
	DecideKeyNullMode null_mode = DecideKeyNullMode::NO_GROUP;
	//! `expression` under its collation, as GROUP BY groups (`PushCollation`); null when
	//! its type has none, or when `expression` holds a subquery and so took its collation
	//! in place (Bind). Rows group on it, and a label quotes `expression`.
	unique_ptr<Expression> collated;

	//! `expression` as a key element, its grouping form built as GROUP BY builds a group's.
	static DecideKeyElement Bind(ClientContext &context, unique_ptr<Expression> expression, string name,
	                             DecideKeyNullMode null_mode);

	//! The expression rows group on: `collated`, else `expression`.
	const Expression &Grouping() const {
		return collated ? *collated : *expression;
	}

	DecideKeyElement Copy() const;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideKeyElement Deserialize(Deserializer &deserializer);
};

//! A grouping key: the expressions whose values decide which rows share a class. Every
//! key the clause writes is one entry of LogicalDecide::keys -- a declaration's `per K:`,
//! a reducer's own `per K'`, a reducer's `by (key)`, a
//! constraint's `per K:` and a nested reducer's path key -- and keys with the same columns
//! share it, whichever spelling wrote them. A relation element (`per D`) stands for every
//! column D stores. The empty key (`per ()`, `by ()`) is no entry: INVALID_INDEX, one
//! class.
struct DecideKey {
	//! In one order, whichever order wrote them (FindOrCreateDecideKey).
	vector<DecideKeyElement> elements;
	//! The key as first written, for messages and EXPLAIN: `D.region, S.dispatchDay +
	//! S.transitDays`, `D`.
	string text;
	//! A reducer's `by` wrote it.
	bool written_as_by = false;

	//! The element names, in element order.
	vector<string> Names() const;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideKey Deserialize(Deserializer &deserializer);
};

//! The entry of `keys` holding `elements` as a set -- order and repetition aside, elements
//! compared as rows group on them (Grouping) -- appending it, as written, when no key so
//! far holds them; INVALID_INDEX for no elements. An element written on its own and
//! through its relation is the written one.
idx_t FindOrCreateDecideKey(vector<DecideKeyElement> elements, const string &text, vector<DecideKey> &keys);
//! The elements of `keys[key]`; none for INVALID_INDEX, the empty key.
const vector<DecideKeyElement> &DecideKeyElements(const vector<DecideKey> &keys, idx_t key);
//! The key as first written; `()` for the empty key.
string DecideKeyText(const vector<DecideKey> &keys, idx_t key);

//! The classes some decisions are keyed by: one decision instance per class of `key`.
//! A declaration's `per K:` and a reducer's own `per K'` (whose classes
//! a reducer counts once) each make one; scopes on the same key share it.
struct KeyedScopeInfo {
	//! Index into LogicalDecide::keys.
	idx_t key = DConstants::INVALID_INDEX;
	//! Which decide_variables indices this scope keys
	vector<idx_t> scoped_variable_indices;
	//! A helper decision's scope (DecideOptimizer::RewriteScopedReducers): the link into
	//! `LogicalDecide::reducer_value_links` it holds. Its classes are the groups of
	//! `key` -- the link's `by` key -- among the rows the link's clause keeps, so
	//! execution builds the mapping from the link. INVALID_INDEX for every scope the
	//! binder creates. Deliberately absent from the serialization: the optimizer creates
	//! these scopes, and an optimized plan is never serialized.
	idx_t value_link = DConstants::INVALID_INDEX;
	//! A relation-qualified reducer's scope of its own (DecideCanonicalizer::BuildConstraintSignatures): the classes
	//! of `key` among the rows the reducer reads, each split by the values of its body's signature (D70, D71), an
	//! index into LogicalDecide::signatures, so the reducer reads one value per instance, not per class. Execution
	//! builds its mapping from the signature. INVALID_INDEX for every other scope.
	idx_t signature = DConstants::INVALID_INDEX;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static KeyedScopeInfo Deserialize(Deserializer &deserializer);
};

//! A frame selector (language/guide.md §5.6), named by a FRAME_TAG_PREFIX tag on the aggregate it binds to:
//! `first(e)` for a point frame, the frame's reducer for a range. At an anchor row it reads the anchor's timeline --
//! the rows sharing its `within` key, ordered by `order` -- at the positions it selects.
struct DecideFrameSpec {
	//! A range, agg(from F1 to F2 ...: e); else a point, at(F: e)
	bool range = false;
	//! A range's reducer, lower case: sum, avg, min, max or count; empty for a point
	string reducer;
	//! The point, or the range's first end, and its k
	DecideFrameAnchor from = DecideFrameAnchor::FIRST;
	int64_t from_k = 1;
	//! The range's last end
	DecideFrameAnchor to = DecideFrameAnchor::FIRST;
	int64_t to_k = 1;
	int64_t every = 1;
	//! `all`: a range with a missing position selects nothing
	bool all = false;
	//! `else v`, read at the anchor row for a missing position, a number; null when unwritten
	unique_ptr<Expression> fallback;
	//! The timeline's order key, and its direction
	unique_ptr<Expression> order;
	//! What positions compare: the key's value (reference 0 of what the pass read, which messages quote) under the
	//! collation binding settled, as a bound ORDER BY's (D74), so a prepared plan or a plan's round trip compares as it
	//! was bound. Null when the key compares as it is.
	unique_ptr<Expression> order_collation;
	bool descending = false;
	bool cyclic = false;
	//! The timeline's partition, the `within` key: an index into LogicalDecide::keys, INVALID_INDEX for one timeline.
	//! A row whose written `within` column is NULL is in no timeline (D5, D75).
	idx_t within_key = DConstants::INVALID_INDEX;
	//! The frame as written, for messages
	string text;
	//! Where the frame stands in the query (its expression's location), which an error about the frame points at.
	//! Invalid where the query text holds none.
	optional_idx location;
	//! The frame in DeciDB's own spelling, lower case with its defaults left out (`at(previous: x) over (t.period)`):
	//! a plan's layers, a helper's label and the frame's column print it, as they reprint every node that reads a
	//! decision (DecideSourceNames::written).
	string spelling;

	//! Where the frame stands, set by DecideCanonicalizer (PlaceFrames). Its timelines are read over the rows its
	//! scoped expression reads -- its path's (language/guide.md §5.5), `rows` keeps them, null for every row -- grouped
	//! by `timeline_key` (into LogicalDecide::keys): the path's `by` keys, then `within`; INVALID_INDEX for one
	//! timeline.
	unique_ptr<Expression> rows;
	idx_t timeline_key = DConstants::INVALID_INDEX;
	//! How a message names each element its timelines are split by (`T.productID`) and its order key (`T.period`).
	vector<string> timeline_names;
	string order_name;
	//! D75: the key whose every class among `rows` lies in one timeline -- the path's `by` keys and the `per` key
	//! of the scoped expression it stands in, an index into LogicalDecide::keys; INVALID_INDEX, one class, for
	//! `per ()` along no `by` -- and the message's subject, empty when that key determines the timeline already.
	idx_t partition_key = DConstants::INVALID_INDEX;
	string partition_subject;

	//! Whether its position is read relative to the anchor's: `previous` or `next`.
	bool Relative() const {
		return from == DecideFrameAnchor::PREVIOUS || from == DecideFrameAnchor::NEXT ||
		       (range && (to == DecideFrameAnchor::PREVIOUS || to == DecideFrameAnchor::NEXT));
	}
	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideFrameSpec Deserialize(Deserializer &deserializer);
};

//! One field of a variant signature: a value the scoped expression's body reads once per instance
//! (language/guide.md §7.1; D70, D83).
struct DecideSignatureField {
	DecideSignatureFieldKind kind = DecideSignatureFieldKind::VALUE;
	//! VALUE: a column the body's data reads (D83), or data that reads no column, whole.
	unique_ptr<Expression> value;
	//! DECISION: the decision (index into decide_variables), per row or keyed.
	idx_t decision = DConstants::INVALID_INDEX;
	//! BY_KEY: the reducer's `by` key (index into LogicalDecide::keys).
	idx_t key = DConstants::INVALID_INDEX;
	//! FRAME: the frame (index into LogicalDecide::frames), compared by its keys (D82).
	idx_t frame = DConstants::INVALID_INDEX;
	//! The field as a message names it: `P.lim`, `ship`, `by (P.region)`, a frame as written.
	string text;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideSignatureField Deserialize(Deserializer &deserializer);
};

//! The variant signature of one scoped expression -- a `per K:` clause, a reducer with its own `per` (or the colon
//! form), or the objective (language/guide.md §7.1; D70): its rows split into classes by `key`, and each class into
//! one instance per distinct value of `fields` over its rows. Built once by DecideCanonicalizer from the canonical
//! clauses; only an expression with a field has one.
struct DecideSignature {
	//! The rows it reads: its `when`, and a reducer's own; null for every row. A field is never computed anywhere else.
	unique_ptr<Expression> rows;
	//! The `per` key (index into LogicalDecide::keys); INVALID_INDEX for `per ()`. A row in no class of it (a NULL
	//! written key) is no instance.
	idx_t key = DConstants::INVALID_INDEX;
	vector<DecideSignatureField> fields;
	//! How a plan names the scoped expression: a clause's key as written (`per P.depotID`), a reducer as written
	//! (`sum(per D: ...)`), `the objective`.
	string text;
	//! The clause it belongs to (LogicalDecide::constraint_sources); INVALID_INDEX for the objective's.
	idx_t source_clause_id = DConstants::INVALID_INDEX;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideSignature Deserialize(Deserializer &deserializer);
};

//! A reducer a bound reads from the data (`sum(cap) by (g)`), recorded by the bound-side rewrite
//! (DecideOptimizer::RewriteRhsAggregates) and not yet computed. The bound's expressions read its result at the
//! placeholder `column`: the column of the aggregate's side table (DirectSourceReader::AddSideTable), which no operator
//! produces until the path builds the table. The value is the aggregate over the rows of each group of `key` that the
//! reducer reads, on every row of the group. Not serialized: the optimizer makes it, and an optimized plan is never
//! serialized.
struct DecideBoundAggregate {
	//! The placeholder the bound reads: its own table index, and the column after the group key's.
	ColumnBinding column;
	//! The BoundAggregateExpression with its function and result type bound; its arguments and filter are the members
	//! below.
	unique_ptr<Expression> function;
	//! What the function reads, in its order: none for COUNT(*), and for a collated MIN or MAX the value and the key it
	//! compares (BindMinMax).
	vector<unique_ptr<Expression>> arguments;
	//! The rows the reducer reads: the clause's kept rows that pass its own `when`; null for every row.
	unique_ptr<Expression> reads;
	//! The reducer's `by` key (an index into `keys`), INVALID_INDEX for the whole query.
	idx_t key = DConstants::INVALID_INDEX;
	//! Whether the reducer is `sum(per K: body)`, which counts each class of K in its group once, and its key K
	//! (INVALID_INDEX for `per ()`: one class).
	bool counts_classes = false;
	idx_t class_key = DConstants::INVALID_INDEX;

	//! Whether `expression` reads the reducer's value: a reference to its placeholder column.
	bool IsReadBy(const Expression &expression) const;
	bool Equals(const DecideBoundAggregate &other) const {
		return key == other.key && counts_classes == other.counts_classes && class_key == other.class_key &&
		       Expression::Equals(function, other.function) && Expression::ListEquals(arguments, other.arguments) &&
		       Expression::Equals(reads, other.reads);
	}
};

//! A source column's logical identity and the name the user wrote for it.
//! Keeping these together makes it impossible for the binding/name association to
//! drift through mismatched parallel vectors.
struct DecideSourceColumnName {
	ColumnBinding binding;
	string name;

	//! Generated from `storage/serialization/nodes.json`.
	void Serialize(Serializer &serializer) const;
	static DecideSourceColumnName Deserialize(Deserializer &deserializer);
};

//! The binding under which LogicalDecide::source_columns names column `binding` of the DECIDE input `source`. A scan's
//! bindings index its column list, which projection pushdown shortens, so a scan's column is named by its table ordinal
//! (as LogicalGet::ResolveTypes walks the list). Any other column is named by its binding. A column the user wrote no
//! name for is named by no column (INVALID_INDEX): a row id, which is synthetic, and a projection's computed column
//! without an alias, which the binder records under its own ToString. A plain column reference, bound or resolved,
//! forwards a name, and so does the compression the optimizer wraps a join in: a projection the binder did not name
//! (`named`) is the optimizer's, and its column is named as the column it reads.
ColumnBinding DecideSourceColumnKey(const LogicalOperator &source, const ColumnBinding &binding,
                                    const vector<DecideSourceColumnName> &named);

//! One expression the optimizer reads or rewrites, with what its owner says about it (LogicalDecide::EnumerateBodies).
//! A clause is read one comparison at a time, since a relation pushes the comparison it stands in and not the clause
//! beside it; every other owner holds one expression.
struct DecideOwnedBody {
	enum class Owner : uint8_t {
		//! A clause of SUCH THAT: a comparison, or the placeholder an earlier rewrite left.
		CLAUSE,
		//! The body of a nonlinear reducer's definition (LogicalDecide::definition_functions).
		DEFINITION,
		//! The objective.
		OBJECTIVE,
		//! The members of a reducer's helper decision (LogicalDecide::reducer_value_links).
		VALUE_LINK,
		//! One of the three rows an L0 indicator stands for, a clause as SUCH THAT's is
		//! (LogicalDecide::l0_indicator_functions).
		L0_LINK,
	};

	DecideOwnedBody(Owner owner, idx_t index, unique_ptr<Expression> &expr) : owner(owner), index(index), expr(expr) {
	}

	Owner owner;
	//! The entry of the owner's container the body belongs to: a definition, a link or an indicator. INVALID_INDEX for
	//! a clause and the objective.
	idx_t index;
	//! The slot that owns the expression, so a rewrite may replace the expression itself.
	unique_ptr<Expression> &expr;
	//! The sign a term of `expr` must carry, read as a sum from sign +1 (the way a comparison reads as `LHS - RHS`), to
	//! be pushed down by its owner: +1 under `<=`, `<` and the minimized objective, -1 under `>=` and `>`, and 0 when
	//! nothing is pushed (`=`, `<>`, a leaf that is no comparison). A definition `t >= q` pushes `q` down, `t <= q` up;
	//! a value link its sides, as the members of its helper are read (ReducerValueLink::need_envelope, need_closing).
	int push = 0;
	//! The rows the body is read on: the `when` of its clause, of its definition or its link, which a reducer's own
	//! `when` in the body narrows further. Null for every row. Borrowed from the clause or the container, or from
	//! `kept_rows` when the owner keeps two conditions (a link's `when` and its own filter).
	const Expression *rows = nullptr;
	unique_ptr<Expression> kept_rows;
	//! The record the body is the body of, with its scope and origin: a clause, the objective or an L0 link. Null for
	//! a definition and a value link, which hold an expression and no clause.
	optional_ptr<BoundDecideClause> clause;

	//! The clause the body was written in (the source clause, or the objective), which every row made of it carries on
	//! to a message. A body with no clause of its own (a helper's) has none of the two.
	DecideRowOrigin Origin() const {
		return clause ? clause->origin : DecideRowOrigin();
	}
};

//! The bound DECIDE part of a query: its decisions, its clauses and what they name. The binder fills it, and it is
//! moved whole into LogicalDecide.
struct BoundDecide {
	// The variables to be decided (e.g., x, y)
	vector<unique_ptr<Expression>> variables;

	// Per-variable boolean flag: true if the variable was declared IS BOOLEAN.
	// Indexed by position in decide_variables. Auxiliary variables appended later
	// should also push_back their boolean status.
	vector<bool> is_boolean_var;

	//! Per-variable scope assignment (row / keyed / query-wide).
	//! Defaults to row-scoped; keyed_scope_idx indexes keyed_scopes.
	vector<DecideVarScopeInfo> variable_scopes;

	//! Every key the clause writes, one entry per set of columns (DecideKey). Scopes,
	//! reducers' `by`, the `per K:` of a clause and the scope checks index it.
	vector<DecideKey> keys;

	//! Every key scope: of `per K:` and `sum(T: ...)`, one per key. Empty when no decision
	//! or reducer is keyed.
	vector<KeyedScopeInfo> keyed_scopes;

	//! Every frame selector written in the clause, indexed by FRAME_TAG_PREFIX tags.
	vector<DecideFrameSpec> frames;

	//! The bound SUCH THAT clauses, one record per row, in the order written; a row a rewrite adds follows them.
	vector<BoundDecideClause> clauses;

	//! Stable source display registry, indexed by source_clause_id.
	vector<ConstraintSourceInfo> constraint_sources;

	//! The bound objective, minimized, with its `when`; its body is null when the query writes none.
	BoundDecideClause objective;

	//! The direction the objective was written in (MINIMIZE, MAXIMIZE, or FEASIBILITY for none). Read only by what
	//! shows the user the objective: a message, EXPLAIN, and a value shown (a timeout's best objective so far,
	//! DIAGNOSE's achievable objective). Everything else reads `objective`, which is always a minimization
	//! (DecideCanonicalizer::CanonicalizeObjective): a MAXIMIZE objective is its negation, carried by the sign of each
	//! term.
	DecideSense sense = DecideSense::FEASIBILITY;

	// Additive constant peeled from the objective body by
	// DecideCanonicalizer::CanonicalizeObjective (e.g. the `3` in
	// `MAXIMIZE SUM(x) + 3`, which is the `-3` of the minimization). It moves no argmax/argmin; it travels to the
	// solver model's objective constant (SolverModel::obj_constant), so an objective value a solve reports is the
	// minimized objective's, which a shown value negates for a MAXIMIZE query. SetObjective ADDS
	// to it, so a later optimizer rewrite cannot discard the user's constant.
	// Zero when nothing was peeled.
	double objective_constant_offset = 0.0;

	//! The DECIDE tail as the user wrote it, with where the parser found each expression in it (the Clang
	//! SourceManager pattern). A bound node's query location names its span; a message cuts the text instead of
	//! reprinting the node (MSG-1).
	DecideSourceText source_text;

	//! The variant signature of every scoped expression with a field (D70), indexed by DecideClauseScope::signature of
	//! a `per K:` clause and by KeyedScopeInfo::signature for a reducer's own `per`. Built by the planner, right after
	//! canonicalization.
	vector<DecideSignature> signatures;

	//! Every expression held here, each slot once.
	void EnumerateExpressions(const std::function<void(unique_ptr<Expression> &expression)> &callback);
};

class LogicalDecide : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_DECIDE;

public:
	LogicalDecide(idx_t decide_index, BoundDecide bound);

	LogicalDecide();

	// The table index for the new columns
	idx_t decide_index;

	//! The bound DECIDE part, as the binder made it and as the rewrites change it
	BoundDecide bound;

	//! The objective as WRITTEN and as CANONICALIZED, the objective's counterpart to
	//! `constraint_sources`. The objective is one side of a comparison with no relation
	//! to orient, so it needs no registry -- there is exactly one of it -- but a plan
	//! that shows what became of a constraint owes the same account of the objective.
	//! Both are captured in Binder::CreatePlan around the single CanonicalizeObjective
	//! call. `written_objective` is the text the user wrote (the parser's OBJECTIVE span), as a clause is cut by its
	//! `location`; `canonical_objective` is DeciDB's rendering of the canonical objective, which the plan's
	//! rewrite rows are compared with. `objective_canonical_differs` says canonicalization changed it, judged on
	//! the renderings before and after (a clause's `canonical_differs` is reported by the canonicalizer instead).
	string written_objective;
	string canonical_objective;
	bool objective_canonical_differs = false;

	//! The user's own name for every column reachable from the DECIDE input, captured
	//! from the BindContext where name resolution happens. This is the ONLY place the
	//! user's spelling exists for a source that has no catalog entry: a `t(a, b, c)`
	//! alias list over `(VALUES ...)`, a subquery, or a CTE is recorded on the binding
	//! and never reaches the plan, whose projection carries the binder's positional
	//! `col0` / `col1` placeholders instead. The unbounded diagnosis uses it to label a
	//! categorical slice with a name the user actually wrote.
	//!
	//! Each entry keeps the logical binding and its name together. The generated
	//! serializer carries the record directly.
	vector<DecideSourceColumnName> source_columns;

	//! The `DIAGNOSE` prefix, carried from the statement that was written. True means
	//! this query was asked to explain its failure: the operator arms the diagnosis
	//! engines and reports its findings as a relation instead of raising. False — the
	//! default, and every unprefixed query — means a failed solve reports its status
	//! and stops, and no diagnostic work is done or paid for.
	//!
	//! It is a property of the STATEMENT, not of the session. Nothing reads it back out
	//! of a setting; it travels parser → binder → here → PhysicalDecide.
	bool diagnose = false;

	// Number of auxiliary variables at the end of decide_variables (created by binder and optimizer)
	idx_t num_auxiliary_vars = 0;

	//! The intact Boolean-product expressions and the plan-time helper allocation needed by stage 05. Executable
	//! bilinear links are materialized from these specs as BOOLEAN_PRODUCT F nodes in DecideTree.
	vector<DecideBilinearFunctionGroup> bilinear_function_groups;

	//! The three formulation rows for each L0 indicator, retained as function-owned tree input rather than ordinary
	//! DECIDE clauses. They stay as expressions through the remaining optimizer passes so nested ABS/products receive
	//! their usual helpers; stage 06 evaluates the rows from the indicator F node's child scopes.
	struct L0IndicatorFunction {
		idx_t value_var = DConstants::INVALID_INDEX;
		bool auto_m = false;
		double m = 1.0;
		string label;
		vector<BoundDecideClause> links;
	};
	vector<L0IndicatorFunction> l0_indicator_functions;

	//! The plan-time helper index, source body and relation for a nonlinear reducer definition. Written reducer bodies
	//! carry a tag that makes the tree read this F value at their original site; tree construction synthesizes the
	//! helper's definition row from this untagged source body.
	struct DefinitionFunction {
		idx_t value_var = DConstants::INVALID_INDEX;
		ExpressionType relation = ExpressionType::INVALID;
		unique_ptr<Expression> body;
		unique_ptr<Expression> condition;
		//! The first clause that reads the helper, which a message about its row quotes; INVALID_INDEX for none.
		idx_t source_clause_id = DConstants::INVALID_INDEX;
	};
	vector<DefinitionFunction> definition_functions;

	//! One ABS over a decision, as RewriteAbs records it: the shape and nothing of its encoding. Stage 05 allocates the
	//! auxiliary and decides whether the exact encoding is needed (FindExactAbs); it emits no row and allocates no
	//! indicator. The tree gives the ABS an F node and its two lower-envelope rows, `aux >= operand` and `aux >=
	//! -operand`, read on the rows the ABS is read on (MakeAbsRow); stage 06 states them, and for an exact ABS either
	//! the backend's `aux = |t|` or a sign indicator with its Big-M rows (EmitAbsEnvelopes, EmitAbsConstraints). The
	//! ABS is a pseudo-operation that the lowering legalizes or expands.
	struct AbsFunction {
		idx_t aux_idx = DConstants::INVALID_INDEX; //!< the helper decision standing for |operand|
		//! The bound operand. The retained function expression keeps its own copy in the clause; this one is read by
		//! the F node, which cannot find itself while lowering a clause a rewrite has consumed (an ABS that is a factor
		//! of a Boolean product).
		unique_ptr<Expression> operand;
		//! True when nothing the clause or the objective pushes pins `aux` to |operand| (a hard direction, an objective
		//! sense pushing it up, an unknown sign, an ABS inside another ABS, a body its readers push up): FindExactAbs.
		bool exact = false;
		//! The rows the ABS is read on: the `when` its clause keeps that also passes the `when` of each reducer around
		//! it. Null for every row.
		unique_ptr<Expression> when;
		//! The auxiliary's scope (ScopeOf its operand): the rows are stated once per instance of it.
		DecideVarScopeInfo scope;
		//! The clause the ABS was written in (or the objective), which the rows it makes carry to EXPLAIN and
		//! diagnostics.
		DecideRowOrigin origin;
		//! The ABS as the user wrote it (RenderDecideSource), for a message that points at it.
		string written;
	};
	vector<AbsFunction> abs_functions;

	//! The helper decisions that hold a value of either sign: a value rewrite's reducer (RewriteScopedReducers), a
	//! definition function's output, and a product with a non-Boolean factor (AddBooleanProduct). FoldSingletonRows
	//! gives them no default floor of 0; a value helper is boxed later by its members' reach, a product by its
	//! factor's box, and a body helper stays unboxed, so stage 08 states the extremum over it natively.
	vector<idx_t> signed_helper_vars;

	//! Maps an auxiliary variable's index in decide_variables to a human-readable
	//! source expression (the user's original ABS(...) / MAX(...) / product / <>),
	//! captured at optimizer time for diagnosis variable-naming (F6 variable
	//! provenance). Sparse — only auxiliary variables appear. Consumed by the
	//! unbounded diagnosis to name an escaping aux column by its source expression
	//! rather than its internal __abs_aux_N__ name.
	vector<pair<idx_t, string>> aux_var_expressions;

	//! A decision reducer of a scoped comparison in the VALUE shape
	//! (CanonicalConstraintClass::VALUE), replaced by the helper decision `value_var`
	//! that holds `agg([when c'] [per K']: e) by (Γ)` over the rows the clause keeps: one
	//! instance per Γ group of those rows, defined once by stage 06 -- an equality row per
	//! group for SUM/AVG, an extremum link per group for MIN/MAX. Produced by
	//! DecideOptimizer::RewriteScopedReducers; an optimized plan is never serialized.
	//!
	//! One link per reducer over one set of rows, shared by every row that reads it: the
	//! copies an earlier rewrite made (an ABS envelope pair) and every clause holding the
	//! same reducer. The sides a shared MIN/MAX helper needs are the union of its rows'
	//! needs. Its definition belongs to no clause (owner ruling B10), so it carries no
	//! clause provenance: a DROP of a reading clause leaves it, and DIAGNOSE reads it as
	//! part of the fixed model.
	struct ReducerValueLink {
		idx_t value_var = DConstants::INVALID_INDEX;
		string agg_name; //!< "sum", "avg", "min" or "max"
		string label;    //!< the reducer as the user wrote it, `by` included, for messages
		//! The clause that first read it, for a message naming that clause as written (DecideWrittenClause);
		//! INVALID_INDEX is the objective.
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		unique_ptr<Expression> inner_expr; //!< e, as bound; the ABS and bilinear rewrites applied
		unique_ptr<Expression> filter;     //!< the reducer's own `when`, or null
		//! Its own `per K'`: the key scope whose classes it counts once; INVALID_INDEX
		//! for per row.
		idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;
		//! Its path key (language/guide.md §5.5), into `keys`: every enclosing reducer's `by`
		//! key and its own; INVALID_INDEX for `by ()` throughout, every kept row.
		idx_t group_key = DConstants::INVALID_INDEX;
		//! The rows its path keeps: the clause's `when`, then each enclosing reducer's own; null for every row.
		unique_ptr<Expression> when;
		//! The sides that pin the helper, read from the way the clause pushes it; both when the sign of its
		//! factor is not known before the query runs or the comparison is `=` or `<>`. MIN/MAX: the
		//! extremum link's envelope and closing sides (ExtremumLinkSpec). SUM/AVG: `value >= members`
		//! (`need_envelope`) and `value <= members` (`need_closing`), the equality when both.
		bool need_envelope = false;
		bool need_closing = false;
		//! The clause's `per` key (into `keys`, INVALID_INDEX for `per ()`), DECIDE_ROW_KEY for a clause with none.
		idx_t class_key = DECIDE_ROW_KEY;
		//! A range frame over decisions (index into `frames`; DecideOptimizer::RewriteFrameRanges): the helper has
		//! one instance per window of the frame, its members the payload `inner_expr` at the rows the window reads
		//! and, with an `else`, its `else` at the window's anchor once per missing position (the O's
		//! DecideBody::members and fallback, LowerDecideTree). INVALID_INDEX for a reducer, whose groups are its path
		//! key's.
		idx_t frame = DConstants::INVALID_INDEX;
	};
	//! Innermost first: a link reads only helpers of links before it -- a range's, made first, and the reducers
	//! nested in it -- which stage 06's inlining relies on.
	vector<ReducerValueLink> reducer_value_links;

	//! What a rewrite made of a construct, for EXPLAIN: the construct as a plan prints it
	//! (`subject`) and what it became (`kind`), worded by the renderer in one place
	//! (RenderDecideRewriteAccount). One per construct a rewrite replaced, under the
	//! clause it came from; INVALID_INDEX is the objective. Produced by the rewrites; an
	//! optimized plan is never serialized.
	struct RewriteAccount {
		enum class Kind : uint8_t {
			AVG_SUM,              //!< an AVG over decisions, a sum scaled by its row count
			NORM_SUM_ABS,         //!< `norm(e, 1)`, a sum of absolute values
			NORM_SUM_SQUARES,     //!< `norm(e, 2)`, a sum of squares
			NORM_MAX_ABS,         //!< `norm(e, 'inf')`, a maximum of absolute values
			EXTREMUM_EACH_MEMBER, //!< a MIN/MAX in its cheap direction, a bound on each member
			HELPER,           //!< an ABS, a product of decisions, a reducer or a MIN/MAX held by one helper decision
			HELPER_PER_GROUP, //!< a reducer held by a helper decision per `by` group
			HELPER_PER_VALUE, //!< `x IN (...)`, a helper decision per listed value
			HELPER_PER_ROW,   //!< `norm(e, 0)`, a helper decision per row
			FRAME_HELPER_PER_TIMELINE, //!< a range frame held by a helper decision per timeline
			FRAME_HELPER_PER_POSITION, //!< a range frame held by a helper decision per position
			NOT_EQUAL,                 //!< `<>`, a helper decision choosing between `<` and `>`
			BODY_AT_LEAST,             //!< a reducer's non-linear body, a helper per row at least its value
			BODY_AT_MOST,              //!< the same, at most its value
			BODY_EQUAL,                //!< the same, equal to its value
			//! What the data made of a clause: a scoped expression's classes holding more than one variant (D70, D71),
			//! instances skipped for having no value (deck pp. 50, 56). Recorded by execution, so only EXPLAIN ANALYZE
			//! prints it, and `subject` is the whole line.
			RUN_TIME,
			//! A helper decision that stage 06 wrote where it is read, with no column (D89). Recorded by execution
			//! with the helper's plan account as `subject`, which it replaces in EXPLAIN ANALYZE.
			VALUE_HELPER_INLINED,
		};
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		Kind kind = Kind::HELPER;
		string subject;
	};
	vector<RewriteAccount> rewrite_accounts;

	// --- Singleton-row variable bounds (set by DecideOptimizer::FoldSingletonRows) ---

	//! A user-written simple bound that was folded into a decision column's box instead
	//! of being emitted as a model row. Recorded so the linear form can check that its
	//! clause was flattened for the repair model (BoundDecideClause::folded_clause_rows), which is what
	//! gives the diagnosis its rows and quotes the bound as the user wrote it.
	struct UserBoundSpec {
		idx_t decide_var_idx; //!< index into decide_variables
		char sense;           //!< '<' (<= K), '>' (>= K), '=' (== K)
		//! The bound as written, moved onto the decision (`2 * x <= 9` is 4.5; a strict
		//! `x < 10` is 9). The box holds the admitted ends; the domain check reads this.
		double k;
		DecideRowOrigin origin;
	};

	//! Per-decision-variable box, sized to decide_variables. Physical execution copies
	//! these into SolverInput instead of re-walking the clauses. A box whose
	//! lower side was never written resolves to the default 0 floor; an explicit negative
	//! lower bound (`x >= -5`, `BETWEEN -10 AND 10`, a negative IN minimum) is honored.
	vector<DecideBoundBox> singleton_row_boxes;
	//! Every folded user bound, in folding order. A BETWEEN contributes two specs.
	vector<UserBoundSpec> user_singleton_row_bounds;
	//! A bound on the members of an easy extremum's `by` groups, a constant on each (`max(x) by (g) <= -5`): no box
	//! takes it, since a row in no group is no member, but the domain check reads it as written.
	vector<UserBoundSpec> member_singleton_row_bounds;

	//! The run-time requirements of the values the clauses read (language/guide.md §7.3), in the order the solver makes
	//! them: each a BOOLEAN over the source row and the bounds' columns, true on a row whose value is valid and raising
	//! on any other. The bound-side rewrite and the ABS rewrite append them (decide_rewrite::AddValueCheck); a direct
	//! plan makes them on the source rows, and the tree evaluates them on the rows it reads.
	vector<unique_ptr<Expression>> value_checks;

	//! The reducers the bounds read from the data, in the order the bound-side rewrite met them. The tree computes each
	//! on the rows it reads (language/guide.md §6.1); a direct plan builds each as a side table
	//! (DirectSourceReader::AddSideTable) and empties the list. Never serialized (the optimizer creates it).
	vector<DecideBoundAggregate> bound_aggregates;
	//! Semantic expressions before a destructive algebraic rewrite. Enumerated so their source bindings stay live.
	vector<unique_ptr<Expression>> retained_expressions;

	//! A reducer over data whose argument reads a frame (language/guide.md §5.6): no side table can read a frame, whose
	//! values execution computes, so the reducer stays in the tree, tagged with its entry (FRAME_AGGREGATE_TAG_PREFIX)
	//! over the rows it reads (its filter), and stage 05 makes it a frame column execution computes after the frames
	//! (DecideFrameColumn::Kind::AGGREGATE). Made by the bound-side rewrite; not serialized (an optimized plan never
	//! is).
	struct FrameAggregate {
		//! Its path key: it has one value per group (FindOrCreateDecidePathKey); INVALID_INDEX for the whole query.
		idx_t key = DConstants::INVALID_INDEX;
		//! Its own `per` key, whose classes it counts once per signature; INVALID_INDEX for `per row`.
		idx_t class_key = DConstants::INVALID_INDEX;
		//! The reducer as written.
		string text;
	};
	vector<FrameAggregate> frame_aggregates;

	//! The registered name ("gurobi", "highs") of the solver this query will run on,
	//! chosen ONCE by ChooseDecideSolver before any rewrite pass and carried from here
	//! to PhysicalDecide. A NAME and not a handle: layer 3 describes the query, and a
	//! live backend handle here would let a logical plan open a solver session, which
	//! is stage 07's job. Stage 07 turns the name back into a backend when it is
	//! actually time to solve (SolverRegistry::Find).
	//!
	//! Empty until a solver is chosen. Deliberately NOT serialized: which solver a host
	//! has is a property of the host, not of the query, so a plan deserialized elsewhere
	//! re-resolves it rather than carrying a choice that machine may not be able to honor.
	string solver_backend_name;

	//! Stage 05's FORMULATION DECISION: which constructs this query leaves for the
	//! backend to state itself, and which it lowers into plain rows. Decided once, here,
	//! from the chosen backend's construct table — because choosing a formulation is
	//! stage 05's job and nothing below it may re-decide. Stage 08 READS these and
	//! translates; it never asks a backend what it supports.
	//!
	//! `SolverConstructSupport` is a table of yes/no per construct, so it is exactly the
	//! shape of the decision; reusing it keeps the question ("can the backend?") and the
	//! answer ("then don't lower it") spelled the same way rather than in two parallel
	//! vocabularies. All-false until a solver is chosen, which is also the value that
	//! reproduces the pre-capability lowering behaviour.
	SolverConstructSupport use_native_constructs;

	//! Stage 05's formulation POLICY, beside the capability table above: whether a
	//! declared construct is used wherever it is declared, or only where the lowering
	//! has no valid Big-M. False is the shipping answer — the lowering is the smaller
	//! model wherever it works, so native is the fallback (see NativeConstructPolicy).
	//! True only under the test-only `DECIDB_NATIVE_CONSTRUCTS=force`.
	//!
	//! Whether a given SITE has a valid Big-M depends on evaluated data, so stage 08
	//! answers that question. It does not re-decide the policy: it applies this one to
	//! a fact only it can see, the same way the `<>` range collapse does.
	bool force_native_constructs = false;

	//! True once the solver formulation has begun (ChooseDecideSolver): the rewrites after it express the query for the
	//! backend chosen. The bound-side rewrite (RewriteRhsAggregates) and the direct-solve rules run before it.
	//!
	//! It is what makes the serialization contract enforceable: the wire format carries
	//! the BOUND plan — what the binder and the canonicalizer produced — and nothing
	//! this flag guards. Stage 05's output is a formulation chosen from the constructs
	//! THIS HOST's solver declares, which is the same reason `solver_backend_name` and
	//! `use_native_constructs` below are not serialized either. An optimized DECIDE plan
	//! is not portable by construction, so it declines to be copied rather than
	//! arriving somewhere else as a formulation that host cannot honor.
	//!
	//! Not serialized: anything deserialized is a bound plan, so false is the only
	//! value a fresh node can have.
	bool optimized = false;

public:
	//! Add a clause to SUCH THAT, canonicalizing it on the way in.
	//! This is the ONLY way a clause may enter LogicalDecide after planning,
	//! and it exists so that constraints synthesized by DecideOptimizer (Big-M rows, an IN's links)
	//! arrive in the same canonical shape as the ones the user wrote. Together with the canonicalization performed
	//! in Binder::CreatePlan, it is one of exactly two call sites of DecideCanonicalizer for a new clause -- do not add
	//! a third, and do not append to `bound.clauses` directly.
	void AddClause(ClientContext &context, BoundDecideClause clause);

	//! Canonicalize `clause`, one of `bound.clauses`, again, in place: for a pass that rewrote its body where it sits.
	void CanonicalizeClause(ClientContext &context, BoundDecideClause &clause);

	//! Replace the objective, canonicalizing it on the way in. This is the objective's
	//! counterpart to AddClause and the ONLY way DecideOptimizer may install a
	//! rewritten objective (AVG scaling, MIN/MAX auxiliaries, ABS envelopes, bilinear
	//! links), so optimizer output is re-canonicalized and verified as constraint output is.
	//!
	//! Any additive constant the rewrite leaves behind is ADDED to
	//! objective_constant_offset rather than replacing it, so the offset peeled from
	//! what the user wrote survives every later rewrite.
	void SetObjective(ClientContext &context, BoundDecideClause objective);

	//! Calls back with every expression this operator owns: the decisions, each clause's body and `when`, the
	//! objective's, the helper decisions' reducers, every key and the scope checks.
	//! This is the single place that
	//! knows LogicalDecide's expression layout -- generic DuckDB passes (the
	//! LogicalOperatorVisitor dispatcher, ColumnBindingResolver, RemoveUnusedColumns)
	//! call through it instead of each re-listing these fields by hand, so no visitor can miss one.
	void EnumerateExpressions(const std::function<void(unique_ptr<Expression> *)> &callback);
	//! Calls back with every body the optimizer's passes read or rewrite: the objective, the clauses, the definitions,
	//! the value links and the L0 links, in that order (DecideOwnedBody). The order is the one the columns a pass adds
	//! stand in, which the model dumps pin. The Visitor pattern, as DuckDB's
	//! `LogicalOperatorVisitor::EnumerateExpressions` gives one: this is the single place that knows which containers
	//! hold a body and what each says about it, so a pass that reads bodies filters by `owner` in its callback instead
	//! of looping over the containers itself, and a container that gains a body is added here and in no pass.
	//!
	//! The callback may replace the expression in `body.expr` and append to `decide_variables` and the other
	//! containers, but not to the container of the body it was handed. A pass that rewrote the objective hands it back
	//! through SetObjective once the enumeration is over, and one that rewrote a clause through CanonicalizeClause,
	//! the canonicalizing boundaries.
	void EnumerateBodies(const std::function<void(DecideOwnedBody &)> &callback);
	//! The columns the node's expressions read from its input: the child's, then the placeholders of the bound
	//! aggregates the path has not built yet (bound_aggregates).
	vector<ColumnBinding> SourceBindings();

	// --- Implement virtual functions ---

	// The output columns are the child's columns plus the new decide variables
	vector<ColumnBinding> GetColumnBindings() override;

	// Resolve the output types
	void ResolveTypes() override;

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	//! Serialization is defined by `storage/serialization/logical_operator.json` and
	//! generated into `serialize_logical_operator.cpp`; there is no hand-written copy.
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

	//! Bound plans only — see `optimized` above.
	bool SupportSerialization() const override {
		return !optimized;
	}

protected:
	// The table indices that this operator produces
	vector<idx_t> GetTableIndex() const override;
};

} // namespace duckdb
