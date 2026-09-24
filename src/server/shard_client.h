#pragma once

#include "common/result.h"
#include "common/types.h"
#include "database/database.h"

#include <string>

namespace nyx::server {

class ShardClient {
  public:
    static Result<ExecuteResult> execute(const std::string& addr, const std::string& token,
                                         const std::string& sql);
};

} // namespace nyx::server
