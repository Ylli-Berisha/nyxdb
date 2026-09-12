#pragma once

#include "binder/bound_ast.h"
#include "catalog/catalog.h"
#include "common/result.h"
#include "common/types.h"

namespace nyx::frontend {

Result<void> run_create_table(Catalog& catalog, const bound::BoundCreateTable& stmt);
Result<u64>  run_insert(Catalog& catalog, const bound::BoundInsert& stmt);

} // namespace nyx::frontend
