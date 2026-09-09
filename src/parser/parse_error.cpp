#include "parser/parse_error.h"

namespace nyx {

namespace {

std::pair<u32, u32> line_col(std::string_view source, u32 offset) {
    u32 line = 1;
    u32 col = 1;
    u32 limit = offset < source.size() ? offset : static_cast<u32>(source.size());
    for (u32 i = 0; i < limit; ++i) {
        if (source[i] == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
    }
    return {line, col};
}

std::string_view line_at(std::string_view source, u32 offset) {
    if (source.empty())
        return {};
    if (offset > source.size())
        offset = static_cast<u32>(source.size());
    usize start = offset;
    while (start > 0 && source[start - 1] != '\n')
        --start;
    usize end = offset;
    while (end < source.size() && source[end] != '\n')
        ++end;
    return source.substr(start, end - start);
}

} // namespace

std::string render_parse_error(std::string_view source, SourceLoc loc, const std::string& message) {
    auto [line, col] = line_col(source, loc.offset);
    std::string_view snippet = line_at(source, loc.offset);

    std::string out;
    out += "parse error at line ";
    out += std::to_string(line);
    out += ", col ";
    out += std::to_string(col);
    out += ": ";
    out += message;
    out += "\n  ";
    out += snippet;
    out += "\n  ";
    out.append(col - 1, ' ');
    u32 caret_len = loc.length == 0 ? 1 : loc.length;
    out.append(caret_len, '^');
    return out;
}

} // namespace nyx
