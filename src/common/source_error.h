#pragma once

#include "common/types.h"
#include "parser/source_loc.h"

#include <string>
#include <string_view>

namespace nyx {

std::string render_source_error(std::string_view source, SourceLoc loc, const std::string& kind,
                                const std::string& message);

} // namespace nyx
