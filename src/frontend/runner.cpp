#include "frontend/runner.h"

namespace nyx::frontend {

Result<void> run_create_table(Catalog& catalog, const bound::BoundCreateTable& stmt) {
    return catalog.add_table(stmt.table_name, stmt.schema);
}

Result<u64> run_insert(Catalog& catalog, const bound::BoundInsert& stmt) {
    Table* t = catalog.table(stmt.table_name);
    if (!t)
        return Result<u64>::err("table not found: " + stmt.table_name);
    return t->insert_many(stmt.rows);
}

} // namespace nyx::frontend
