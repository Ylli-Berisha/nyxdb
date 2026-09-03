#include "executor/project.h"

#include <cassert>
#include <utility>

namespace nyx {

Project::Project(std::unique_ptr<Operator> child, std::vector<ProjectItem> items)
    : child_(std::move(child)), items_(std::move(items)) {
    assert(child_ != nullptr);
    assert(!items_.empty());

    output_schema_.reserve(items_.size());
    for (const auto& it : items_) {
        assert(it.expr != nullptr);
        output_schema_.push_back(Column{it.name, it.expr->output_type(), it.nullable});
    }
}

Result<void> Project::open() {
    auto r = child_->open();
    if (r.is_err())
        return r;
    opened_ = true;
    return Result<void>::ok();
}

void Project::close() {
    child_->close();
}

Result<std::optional<Chunk>> Project::next() {
    assert(opened_);

    auto in = child_->next();
    if (in.is_err())
        return Result<std::optional<Chunk>>::err(in.error().message);
    if (!in.value().has_value())
        return Result<std::optional<Chunk>>::ok(std::nullopt);

    Chunk input = std::move(*in.value());
    size_t rc = input.row_count();

    std::vector<ColumnVector> outs;
    outs.reserve(items_.size());
    for (auto& it : items_) {
        auto r = it.expr->evaluate(input);
        if (r.is_err())
            return Result<std::optional<Chunk>>::err(r.error().message);
        outs.push_back(std::move(r.value()));
    }

    return Result<std::optional<Chunk>>::ok(Chunk(rc, std::move(outs)));
}

} // namespace nyx
