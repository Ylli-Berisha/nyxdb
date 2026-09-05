#include "executor/filter.h"

#include "executor/selection_vector.h"

#include <cassert>
#include <utility>

namespace nyx {

Filter::Filter(std::unique_ptr<Operator> child, std::unique_ptr<Expression> predicate,
               FilterStrategy strategy)
    : child_(std::move(child)), predicate_(std::move(predicate)), strategy_(strategy) {
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

        const ColumnVector& mask = pred.value();

        if (strategy_ == FilterStrategy::COMPACT) {
            if (chunk.has_sel())
                chunk.compact_from_sel(mask);
            else
                chunk.compact_in_place(mask);
            if (chunk.row_count() == 0)
                continue;
        } else {
            SelectionVector::Builder b;
            if (chunk.has_sel()) {
                size_t k = 0;
                chunk.sel().for_each([&](u32 phys) {
                    if (!mask.is_null(k) && mask.get_i32(k) != 0)
                        b.append(phys);
                    ++k;
                });
            } else {
                for (u32 i = 0; i < static_cast<u32>(mask.size()); ++i) {
                    if (!mask.is_null(i) && mask.get_i32(i) != 0)
                        b.append(i);
                }
            }
            if (b.empty())
                continue;
            auto sv = b.finalize(static_cast<u32>(chunk.row_count()), false);
            chunk.set_sel(std::move(sv));
        }

        return Result<std::optional<Chunk>>::ok(std::move(chunk));
    }
}

} // namespace nyx
