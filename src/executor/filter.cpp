#include "executor/filter.h"

#include <cassert>
#include <utility>

namespace nyx {

Filter::Filter(std::unique_ptr<Operator> child, std::unique_ptr<Expression> predicate)
    : child_(std::move(child)), predicate_(std::move(predicate)) {
    assert(child_ != nullptr);
    assert(predicate_ != nullptr);
    assert(predicate_->output_type() == TypeId::INT32);
}

Result<void> Filter::open() {
    auto r = child_->open();
    if (r.is_err())
        return r;
    opened_ = true;
    return Result<void>::ok();
}

void Filter::close() {
    child_->close();
}

Result<std::optional<Chunk>> Filter::next() {
    assert(opened_);
    while (true) {
        auto in = child_->next();
        if (in.is_err())
            return Result<std::optional<Chunk>>::err(in.error().message);
        if (!in.value().has_value())
            return Result<std::optional<Chunk>>::ok(std::nullopt);

        Chunk chunk = std::move(*in.value());

        auto pred = predicate_->evaluate(chunk);
        if (pred.is_err())
            return Result<std::optional<Chunk>>::err(pred.error().message);

        chunk.compact_in_place(pred.value());

        if (chunk.row_count() == 0)
            continue;

        return Result<std::optional<Chunk>>::ok(std::move(chunk));
    }
}

} // namespace nyx
