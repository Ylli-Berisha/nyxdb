#pragma once

#include "common/types.h"

namespace nyx {

class Table;

struct VacuumStats {
    u64 dead_rows;
    u64 total_rows;
};

VacuumStats table_dead_row_stats(const Table* t);
u64 vacuum_table(Table* t);

} // namespace nyx
