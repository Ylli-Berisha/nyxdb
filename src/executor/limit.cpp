#include "executor/limit.h"

#include <cassert>
#include <utility>

namespace nyx {

Limit::Limit(std::unique_ptr<Operator> child, size_t limit) : Limit(std::move(child), 0, limit) {}

Limit::Limit(std::unique_ptr<Operator> child, size_t offset, size_t limit)
    : child_(std::move(child)), offset_(offset), limit_(limit) {
    assert(child_ != nullptr);
}

Result<void> Limit::open() {
    auto r = child_->open();
    if (r.is_err())
        return r;
    opened_ = true;
    return Result<void>::ok();
}

void Limit::close_child_eager() {
    child_->close();
    exhausted_ = true;
}

void Limit::close() {
    if (!exhausted_)
        close_child_eager();
}

Result<std::optional<Chunk>> Limit::next() {
    assert(opened_);
    if (exhausted_)
        return Result<std::optional<Chunk>>::ok(std::nullopt);

    if (emitted_ >= limit_) {
        close_child_eager();
        return Result<std::optional<Chunk>>::ok(std::nullopt);
    }

    while (true) {
        auto in = child_->next();
        if (in.is_err())
            return Result<std::optional<Chunk>>::err(in.error().message);
        if (!in.value().has_value()) {
            close_child_eager();
            return Result<std::optional<Chunk>>::ok(std::nullopt);
        }

        Chunk chunk = std::move(*in.value());

        if (skipped_ < offset_) {
            size_t skip_left = offset_ - skipped_;
            if (chunk.logical_size() <= skip_left) {
                skipped_ += chunk.logical_size();
                continue;
            }
            chunk.drop_prefix(skip_left);
            skipped_ = offset_;
        }

        size_t remaining = limit_ - emitted_;
        if (chunk.logical_size() <= remaining) {
            emitted_ += chunk.logical_size();
            if (emitted_ == limit_)
                close_child_eager();
            return Result<std::optional<Chunk>>::ok(std::move(chunk));
        }

        chunk.resize(remaining);
        emitted_ = limit_;
        close_child_eager();
        return Result<std::optional<Chunk>>::ok(std::move(chunk));
    }
}

} // namespace nyx
