#include "duckdb/execution/operator/decide/decide_key_map.hpp"

#include "duckdb/execution/operator/aggregate/aggregate_object.hpp"
#include "duckdb/storage/buffer_manager.hpp"

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

DecideKeyMap::DecideKeyMap(ClientContext &context, const vector<LogicalType> &types)
    : table(context, BufferAllocator::Get(context), types, {}, DecideKeyMapRegister()), addresses(LogicalType::POINTER),
      created(STANDARD_VECTOR_SIZE), offset(table.GetLayout().GetAggrOffset()) {
	D_ASSERT(!types.empty());
}

void DecideKeyMap::FindOrCreate(DataChunk &keys, idx_t *slots, idx_t &next) {
	if (keys.size() == 0) {
		return;
	}
	// The table pins its rows (radix_bits 0), so a register's address holds for the table's lifetime, as flat's
	// BuildGroupIds relies on.
	table.FindOrCreateGroups(keys, addresses, created);
	auto rows = FlatVector::GetData<data_ptr_t>(addresses);
	for (idx_t i = 0; i < keys.size(); i++) {
		auto slot = Load<idx_t>(rows[i] + offset);
		if (slot == DConstants::INVALID_INDEX) {
			slot = next++;
			Store<idx_t>(slot, rows[i] + offset);
		}
		slots[i] = slot;
	}
}

} // namespace duckdb
