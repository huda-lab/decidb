//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_evaluator.hpp
//
// Which evaluator runs a query, and what two runs are compared on: Branch by Abstraction with a whole Parallel Run. The
// flat evaluator stays whole while the deck's algorithm is built beside it, and `verify` runs a second evaluator on the
// same rows and compares what each would hand the model. All of this goes when the flat evaluator does.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

enum class DecideEvaluator : uint8_t {
	FLAT,       //!< the evaluator of record
	VERIFY,     //!< flat, which alone prints, then a second run in silence, the two compared
	TREE,       //!< the deck's algorithm, beside the side plans
	TREE_NOSIDE //!< the deck's algorithm with no side plans
};

//! The evaluator DECIDB_EVALUATOR names (flat, verify, tree, tree-noside; unset is flat). Read once, as the environment
//! of a running process does not change. It is no setting, so it stays off the wire and out of every plan.
DecideEvaluator SelectedDecideEvaluator();

//! A structural dump of the tree is requested (DECIDB_DUMP_TREE, DECIDB_DUMP_CONTAINERS): under flat, its descent is
//! walked only to print it.
bool DecideTreeDumpRequested();

//! Build the semantic graph for a tree execution, for verify's second run, which the tree makes wherever its model
//! emitter has an arm for the query, or for a requested structural dump. The tree-only rewrites leave the plan flat
//! reads as written, so verify's flat run reads flat's plan.
bool DecideTreeRequested();

//! What a run of an evaluator leaves for the model, in the form two runs compare, taken before BuildSolverInput
//! consumes it. SolverInput itself cannot be compared: it holds a std::function and mutable caches, and a clause's
//! coefficients are filled on rows it never emits. Each field is in one group, which an injected difference names.
class DecideEvaluationRecord {
public:
	struct Field {
		string group;
		string name;
		string text;
		vector<double> numbers;
	};

	void Add(string group, string name, string text, vector<double> numbers = {}) {
		fields.push_back({std::move(group), std::move(name), std::move(text), std::move(numbers)});
	}

	vector<Field> fields;
};

//! Raises INTERNAL at the first thing the two runs differ in: one raised and the other did not, their messages, or a
//! field. Fields compare group by group, each group's in their order, so a group that records more shifts no other.
//! Doubles compare within a few ulps, since a second evaluator may sum in another order. Two runs that raised alike
//! stopped where they stopped, so their fields are not compared. DECIDB_EVALUATOR_INJECT=<group>[,...] changes the
//! second run's first field of each group, <group>:ulp nudges it by one ulp and <group>:extra adds a field to the
//! group: how a test shows the comparison can fail. DECIDB_VERIFY_ALLOW=<bug>:<group>[:<field prefix>][,...] names a
//! difference a test knows of, the group's fields whose name begins with the prefix's words: those fields are compared
//! apart, and a difference there is written to DECIDB_VERIFY_ALLOW_LOG as its bug rather than raised. The group
//! `raised` names a difference in what the runs raised, after which nothing is compared.
void CompareDecideEvaluations(const DecideEvaluationRecord &flat, const ErrorData &flat_error,
                              DecideEvaluationRecord second, const ErrorData &second_error);

} // namespace duckdb
