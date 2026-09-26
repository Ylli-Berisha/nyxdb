#pragma once

#include "binder/bound_ast.h"
#include "catalog/catalog.h"
#include "common/result.h"
#include "common/types.h"

namespace nyx::frontend {

Result<void> run_create_table(Catalog& catalog, const bound::BoundCreateTable& stmt);
Result<u64> run_insert(Catalog& catalog, const bound::BoundInsert& stmt);
Result<void> run_drop_table(Catalog& catalog, const bound::BoundDropTable& stmt);
Result<u64> run_delete(Catalog& catalog, const bound::BoundDelete& stmt);
Result<u64> run_update(Catalog& catalog, const bound::BoundUpdate& stmt);
Result<void> run_create_index(Catalog& catalog, const bound::BoundCreateIndex& stmt);
Result<void> run_drop_index(Catalog& catalog, const bound::BoundDropIndex& stmt);
Result<u64> run_vacuum(Catalog& catalog, const bound::BoundVacuum& stmt);
Result<void> run_alter_add_partition(Catalog& catalog, const bound::BoundAlterAddPartition& stmt);

} // namespace nyx::frontend
