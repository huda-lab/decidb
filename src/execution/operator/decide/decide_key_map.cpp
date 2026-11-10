#include "duckdb/execution/operator/decide/decide_key_map.hpp"

#include "duckdb/execution/operator/aggregate/aggregate_object.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <algorithm>
#include <cstring>

namespace duckdb {

static idx_t DecideKeyMapRegisterSize(const AggregateFunction &) {
	return sizeof(idx_t);
}

//! A group's register holds no slot until a row of the probe that created it claims one.
static void DecideKeyMapRegisterInitialize(const AggregateFunction &, data_ptr_t state) {
	Store<idx_t>(DConstants::INVALID_INDEX, state);
}

//! The register is an aggregate the table initializes and never updates, combines or finalizes.
static vector<AggregateObject> DecideKeyMapRegister() {
	AggregateFunction slot("decide_key_slot", {}, LogicalType::UBIGINT, DecideKeyMapRegisterSize,
	                       DecideKeyMapRegisterInitialize, nullptr, nullptr, nullptr,
	                       FunctionNullHandling::DEFAULT_NULL_HANDLING);
	vector<AggregateObject> registers;
	registers.emplace_back(std::move(slot), nullptr, 0, sizeof(idx_t), AggregateType::NON_DISTINCT,
	                       PhysicalType::UINT64);
	return registers;
}

DecideKeyMap::DecideKeyMap(ClientContext &context, const vector<LogicalType> &types, bool memo)
    : table(context, BufferAllocator::Get(context), types, {}, DecideKeyMapRegister()), addresses(LogicalType::POINTER),
      created(STANDARD_VECTOR_SIZE), offset(table.GetLayout().GetAggrOffset()) {
	D_ASSERT(!types.empty());
	D_ASSERT(!memo || std::all_of(types.begin(), types.end(),
	                              [](const LogicalType &type) { return type.id() == LogicalTypeId::UBIGINT; }));
	if (memo) {
		memo_width = types.size();
	}
}

void DecideKeyMap::Probe(DataChunk &keys, const SelectionVector *selection, idx_t count, idx_t *slots, idx_t &next) {
	DataChunk probe;
	if (selection) {
		probe.InitializeEmpty(keys.GetTypes());
		probe.Slice(keys, *selection, count);
	}
	auto &probed = selection ? probe : keys;
	// The table pins its rows (radix_bits 0), so a register's address holds for the table's lifetime, as flat's
	// BuildGroupIds relies on.
	table.FindOrCreateGroups(probed, addresses, created);
	auto rows = FlatVector::GetData<data_ptr_t>(addresses);
	for (idx_t k = 0; k < count; k++) {
		auto slot = Load<idx_t>(rows[k] + offset);
		if (slot == DConstants::INVALID_INDEX) {
			slot = next++;
			Store<idx_t>(slot, rows[k] + offset);
		}
		slots[selection ? selection->get_index(k) : k] = slot;
	}
}

void DecideKeyMap::FindOrCreate(DataChunk &keys, idx_t *slots, idx_t &next) {
	const auto count = keys.size();
	if (count == 0) {
		return;
	}
	if (memo_width == 0) {
		Probe(keys, nullptr, count, slots, next);
		return;
	}
	// Each row's ids, row after row. The caller numbers them, so none is NULL.
	const auto width = memo_width;
	unsafe_vector<uint64_t> ids(count * width);
	for (idx_t c = 0; c < width; c++) {
		UnifiedVectorFormat format;
		keys.data[c].ToUnifiedFormat(count, format);
		auto data = UnifiedVectorFormat::GetData<uint64_t>(format);
		for (idx_t i = 0; i < count; i++) {
			ids[i * width + c] = data[format.sel->get_index(i)];
		}
	}
	SelectionVector missing(count);
	idx_t missing_count = 0;
	const auto remembered_slots = memo_slots.data();
	const auto remembered_count = memo_slots.size();
	for (idx_t i = 0; i < count; i++) {
		const auto row = ids.data() + i * width;
		const auto first = row[0];
		bool remembered = first < remembered_count && remembered_slots[first] != DConstants::INVALID_INDEX;
		for (idx_t c = 1; c < width && remembered; c++) {
			remembered = row[c] == memo_keys[first * width + c];
		}
		if (remembered) {
			slots[i] = remembered_slots[first];
		} else {
			missing.set_index(missing_count++, i);
		}
	}
	if (missing_count == 0) {
		return;
	}
	Probe(keys, &missing, missing_count, slots, next);
	for (idx_t k = 0; k < missing_count; k++) {
		const auto i = missing.get_index(k);
		const auto row = ids.data() + i * width;
		if (row[0] >= memo_slots.size()) {
			memo_slots.resize(MaxValue<idx_t>(row[0] + 1, memo_slots.size() * 2), DConstants::INVALID_INDEX);
			memo_keys.resize(memo_slots.size() * width);
		}
		memcpy(memo_keys.data() + row[0] * width, row, width * sizeof(uint64_t));
		memo_slots[row[0]] = slots[i];
	}
}

} // namespace duckdb
