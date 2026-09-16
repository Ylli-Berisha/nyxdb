#include "frontend/runner.h"

namespace nyx::frontend {

Result<void> run_create_table(Catalog& catalog, const bound::BoundCreateTable& stmt) {
    return catalog.add_table(stmt.table_name, stmt.schema);
}

Result<u64> run_insert(Catalog& catalog, const bound::BoundInsert& stmt) {
    return catalog.insert(stmt.table_name, stmt.rows);
}

Result<void> run_drop_table(Catalog& catalog, const bound::BoundDropTable& stmt) {
    return catalog.drop_table(stmt.table_name, stmt.if_exists);
}

} // namespace nyx::frontend
