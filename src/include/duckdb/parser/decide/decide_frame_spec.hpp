//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/parser/decide/decide_frame_spec.hpp
//
// The scalar part of a frame expression, carried through the parsed tree as one
// VARCHAR constant (child 1 of a FRAME_TAG wrapper) so the wrapper stays an
// ordinary FunctionExpression for Copy / Equals / serialization.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

enum class DecideFrameSelectorKind : uint8_t { FIRST = 0, LAST = 1, PREVIOUS = 2, NEXT = 3 };

//! Which position a frame reads: `first`, `last`, `k previous`, `k next`.
struct DecideFrameSelector {
	DecideFrameSelectorKind kind = DecideFrameSelectorKind::PREVIOUS;
	//! Only meaningful for PREVIOUS / NEXT; 1 when the distance was omitted.
	idx_t distance = 1;

	string ToString() const;
	bool operator==(const DecideFrameSelector &other) const {
		return kind == other.kind && distance == other.distance;
	}
};

//! What a missing position does: read as NULL (default), read as the fill value, or
//! -- for a range -- require every position (`all`, else the whole range is NULL).
enum class DecideFramePolicy : uint8_t { ELSE_NULL = 0, ELSE_VALUE = 1, ALL = 2 };

struct DecideFrameSpec {
	//! false: `AT(sel: e)`; true: `agg(FROM sel TO sel: e)`.
	bool is_range = false;
	//! Lower-case aggregate name of a range frame: sum, avg, min, max.
	string aggregate;
	DecideFrameSelector from_selector;
	DecideFrameSelector to_selector;
	idx_t every = 1;
	DecideFramePolicy policy = DecideFramePolicy::ELSE_NULL;
	bool descending = false;
	bool cyclic = false;
	//! How many trailing children of the wrapper are WITHIN partition columns, and
	//! whether WITHIN was written at all (`WITHIN ()` is written and empty).
	bool has_within = false;

	//! The compact text stored in the wrapper's spec constant, and its inverse.
	string Encode() const;
	static DecideFrameSpec Decode(const string &encoded);

	bool operator==(const DecideFrameSpec &other) const;
};

} // namespace duckdb
