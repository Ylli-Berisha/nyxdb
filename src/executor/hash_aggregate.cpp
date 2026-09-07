#include "executor/hash_aggregate.h"

#include "common/xxhash.h"
#include "executor/table_scan.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <string>
#include <utility>

namespace nyx {

static constexpr u32 NO_GROUP = UINT32_MAX;
static constexpr size_t MAX_GROUP_KEY_BYTES = 128;
static constexpr size_t INITIAL_TABLE_SIZE = 16;

static u32 hash_group_row(const std::vector<ColumnVector>& key_cols, size_t row) {
    byte buf[MAX_GROUP_KEY_BYTES];
    size_t off = 0;
    for (const auto& col : key_cols) {
        size_t sz = type_size(col.type());
        assert(off + sz + 1 <= MAX_GROUP_KEY_BYTES);
        bool is_null = col.is_null(row);
        buf[off++] = is_null ? 0 : 1;
        if (is_null)
            std::memset(buf + off, 0, sz);
        else
            std::memcpy(buf + off, col.data() + row * sz, sz);
        off += sz;
    }
    return static_cast<u32>(xxhash64(buf, off));
}

static bool group_keys_equal(const std::vector<ColumnVector>& lc, size_t li,
                             const std::vector<ColumnVector>& rc, size_t ri) {
    assert(lc.size() == rc.size());
    for (size_t k = 0; k < lc.size(); ++k) {
        assert(lc[k].type() == rc[k].type());
        bool ln = lc[k].is_null(li);
        bool rn = rc[k].is_null(ri);
        if (ln != rn)
            return false;
        if (ln)
            continue;
        size_t sz = type_size(lc[k].type());
        if (std::memcmp(lc[k].data() + li * sz, rc[k].data() + ri * sz, sz) != 0)
            return false;
    }
    return true;
}

HashAggregate::HashAggregate(std::unique_ptr<Operator> child,
                             std::vector<std::unique_ptr<Expression>> group_keys,
                             std::vector<AggregateSpec> aggregates)
    : child_(std::move(child)), group_keys_(std::move(group_keys)),
      aggregates_(std::move(aggregates)) {
    assert(child_ != nullptr);
    assert(!aggregates_.empty());
    for (const auto& gk : group_keys_) {
        assert(gk != nullptr);
        TypeId t = gk->output_type();
        assert(t == TypeId::INT32 || t == TypeId::INT64 || t == TypeId::DOUBLE);
    }

    for (const auto& spec : aggregates_) {
        Expression* arg = spec.arg.get();

        if (spec.kind == AggregateKind::AVG) {
            assert(arg != nullptr);
            TypeId in = arg->output_type();
            assert(in == TypeId::INT32 || in == TypeId::INT64 || in == TypeId::DOUBLE);
            TypeId sum_state = (in == TypeId::INT32) ? TypeId::INT64 : in;

            u32 sum_idx = static_cast<u32>(internal_aggs_.size());
            internal_aggs_.push_back(InternalAgg{InternalAggKind::SUM, arg, in, sum_state});
            group_state_cols_.push_back(ColumnVector::empty(sum_state, true, 0));

            u32 count_idx = static_cast<u32>(internal_aggs_.size());
            internal_aggs_.push_back(InternalAgg{InternalAggKind::COUNT, arg, in, TypeId::INT64});
            group_state_cols_.push_back(ColumnVector::empty(TypeId::INT64, false, 0));

            output_bindings_.push_back(
                OutputBinding{OutputBinding::Kind::AVG_DIVIDE, TypeId::DOUBLE, sum_idx, count_idx});
            output_schema_.push_back({"avg", TypeId::DOUBLE, true});
            continue;
        }

        InternalAgg ia{};
        bool state_nullable = false;
        const char* name = "agg";

        switch (spec.kind) {
        case AggregateKind::COUNT_STAR:
            ia = {InternalAggKind::COUNT_STAR, nullptr, TypeId::INT64, TypeId::INT64};
            state_nullable = false;
            name = "count_star";
            break;
        case AggregateKind::COUNT: {
            assert(arg != nullptr);
            TypeId in = arg->output_type();
            ia = {InternalAggKind::COUNT, arg, in, TypeId::INT64};
            state_nullable = false;
            name = "count";
            break;
        }
        case AggregateKind::SUM: {
            assert(arg != nullptr);
            TypeId in = arg->output_type();
            assert(in == TypeId::INT32 || in == TypeId::INT64 || in == TypeId::DOUBLE);
            TypeId state = (in == TypeId::INT32) ? TypeId::INT64 : in;
            ia = {InternalAggKind::SUM, arg, in, state};
            state_nullable = true;
            name = "sum";
            break;
        }
        case AggregateKind::MIN: {
            assert(arg != nullptr);
            TypeId in = arg->output_type();
            assert(in == TypeId::INT32 || in == TypeId::INT64 || in == TypeId::DOUBLE);
            ia = {InternalAggKind::MIN, arg, in, in};
            state_nullable = true;
            name = "min";
            break;
        }
        case AggregateKind::MAX: {
            assert(arg != nullptr);
            TypeId in = arg->output_type();
            assert(in == TypeId::INT32 || in == TypeId::INT64 || in == TypeId::DOUBLE);
            ia = {InternalAggKind::MAX, arg, in, in};
            state_nullable = true;
            name = "max";
            break;
        }
        case AggregateKind::AVG:
            assert(false && "AVG handled above");
            break;
        }

        u32 state_idx = static_cast<u32>(internal_aggs_.size());
        internal_aggs_.push_back(ia);
        group_state_cols_.push_back(ColumnVector::empty(ia.state_type, state_nullable, 0));
        output_bindings_.push_back(
            OutputBinding{OutputBinding::Kind::DIRECT, ia.state_type, state_idx, 0u});
        output_schema_.push_back({name, ia.state_type, state_nullable});
    }

    if (group_keys_.empty()) {
        append_initial_state_();
        num_groups_ = 1;
    } else {
        Schema new_schema;
        new_schema.reserve(group_keys_.size() + output_schema_.size());
        for (size_t k = 0; k < group_keys_.size(); ++k) {
            new_schema.push_back({"key_" + std::to_string(k), group_keys_[k]->output_type(), true});
        }
        for (auto& c : output_schema_)
            new_schema.push_back(std::move(c));
        output_schema_ = std::move(new_schema);

        for (auto& gk : group_keys_)
            group_key_cols_.push_back(ColumnVector::empty(gk->output_type(), true, 0));

        table_.assign(INITIAL_TABLE_SIZE, Entry{0u, NO_GROUP});
        mask_ = static_cast<u32>(INITIAL_TABLE_SIZE - 1);
        num_groups_ = 0;
    }
}

Result<void> HashAggregate::open() {
    auto r = child_->open();
    if (r.is_err())
        return r;
    opened_ = true;
    return Result<void>::ok();
}

void HashAggregate::close() {
    if (child_)
        child_->close();
}

void HashAggregate::append_initial_state_() {
    for (auto& col : group_state_cols_) {
        if (col.nullable())
            col.append_null();
        else
            col.append_i64(0);
    }
}

void HashAggregate::resize_table_() {
    size_t new_size = table_.size() * 2;
    std::vector<Entry> new_table(new_size, Entry{0u, NO_GROUP});
    u32 new_mask = static_cast<u32>(new_size - 1);
    for (u32 g = 0; g < num_groups_; ++g) {
        u32 h = hash_group_row(group_key_cols_, g);
        u32 slot = h & new_mask;
        while (new_table[slot].group_idx != NO_GROUP)
            slot = (slot + 1u) & new_mask;
        new_table[slot] = Entry{h, g};
    }
    table_ = std::move(new_table);
    mask_ = new_mask;
}

u32 HashAggregate::find_or_create_group_(const std::vector<ColumnVector>& key_cols, u32 row) {
    u32 h = hash_group_row(key_cols, row);
    u32 slot = h & mask_;
    while (table_[slot].group_idx != NO_GROUP) {
        const Entry& e = table_[slot];
        if (e.hash == h && group_keys_equal(group_key_cols_, e.group_idx, key_cols, row))
            return e.group_idx;
        slot = (slot + 1u) & mask_;
    }

    u32 new_idx = num_groups_++;
    for (size_t k = 0; k < group_key_cols_.size(); ++k) {
        const ColumnVector& src = key_cols[k];
        ColumnVector& dst = group_key_cols_[k];
        if (src.is_null(row)) {
            dst.append_null();
        } else {
            switch (src.type()) {
            case TypeId::INT32:
                dst.append_i32(src.get_i32(row));
                break;
            case TypeId::INT64:
                dst.append_i64(src.get_i64(row));
                break;
            case TypeId::DOUBLE:
                dst.append_f64(src.get_f64(row));
                break;
            default:
                assert(false);
            }
        }
    }
    append_initial_state_();
    table_[slot] = Entry{h, new_idx};

    if (static_cast<size_t>(num_groups_) * 2 > table_.size())
        resize_table_();
    return new_idx;
}

void HashAggregate::update_state_(size_t agg_idx, u32 group_idx,
                                  const std::optional<ColumnVector>& arg_col, u32 arg_row) {
    const InternalAgg& ia = internal_aggs_[agg_idx];
    ColumnVector& state = group_state_cols_[agg_idx];

    switch (ia.kind) {
    case InternalAggKind::COUNT_STAR:
        state.set_i64(group_idx, state.get_i64(group_idx) + 1);
        return;
    case InternalAggKind::COUNT:
        assert(arg_col.has_value());
        if (arg_col->is_null(arg_row))
            return;
        state.set_i64(group_idx, state.get_i64(group_idx) + 1);
        return;
    case InternalAggKind::SUM: {
        assert(arg_col.has_value());
        if (arg_col->is_null(arg_row))
            return;
        switch (ia.state_type) {
        case TypeId::INT64: {
            i64 add = (ia.input_type == TypeId::INT32) ? static_cast<i64>(arg_col->get_i32(arg_row))
                                                       : arg_col->get_i64(arg_row);
            i64 cur = state.is_null(group_idx) ? 0 : state.get_i64(group_idx);
            state.set_i64(group_idx, cur + add);
            return;
        }
        case TypeId::DOUBLE: {
            f64 add = arg_col->get_f64(arg_row);
            f64 cur = state.is_null(group_idx) ? 0.0 : state.get_f64(group_idx);
            state.set_f64(group_idx, cur + add);
            return;
        }
        default:
            assert(false);
            return;
        }
    }
    case InternalAggKind::MIN: {
        assert(arg_col.has_value());
        if (arg_col->is_null(arg_row))
            return;
        switch (ia.state_type) {
        case TypeId::INT32: {
            i32 v = arg_col->get_i32(arg_row);
            if (state.is_null(group_idx) || v < state.get_i32(group_idx))
                state.set_i32(group_idx, v);
            return;
        }
        case TypeId::INT64: {
            i64 v = arg_col->get_i64(arg_row);
            if (state.is_null(group_idx) || v < state.get_i64(group_idx))
                state.set_i64(group_idx, v);
            return;
        }
        case TypeId::DOUBLE: {
            f64 v = arg_col->get_f64(arg_row);
            if (state.is_null(group_idx) || v < state.get_f64(group_idx))
                state.set_f64(group_idx, v);
            return;
        }
        default:
            assert(false);
            return;
        }
    }
    case InternalAggKind::MAX: {
        assert(arg_col.has_value());
        if (arg_col->is_null(arg_row))
            return;
        switch (ia.state_type) {
        case TypeId::INT32: {
            i32 v = arg_col->get_i32(arg_row);
            if (state.is_null(group_idx) || v > state.get_i32(group_idx))
                state.set_i32(group_idx, v);
            return;
        }
        case TypeId::INT64: {
            i64 v = arg_col->get_i64(arg_row);
            if (state.is_null(group_idx) || v > state.get_i64(group_idx))
                state.set_i64(group_idx, v);
            return;
        }
        case TypeId::DOUBLE: {
            f64 v = arg_col->get_f64(arg_row);
            if (state.is_null(group_idx) || v > state.get_f64(group_idx))
                state.set_f64(group_idx, v);
            return;
        }
        default:
            assert(false);
            return;
        }
    }
    }
}

Result<void> HashAggregate::aggregate_all_() {
    while (true) {
        auto in = child_->next();
        if (in.is_err())
            return Result<void>::err(in.error().message);
        if (!in.value().has_value())
            break;
        Chunk chunk = std::move(*in.value());
        if (chunk.logical_size() == 0)
            continue;
        chunk.materialize();

        std::vector<ColumnVector> key_cols;
        if (!group_keys_.empty()) {
            key_cols.reserve(group_keys_.size());
            for (auto& gk : group_keys_) {
                auto ev = gk->evaluate(chunk);
                if (ev.is_err())
                    return Result<void>::err(ev.error().message);
                key_cols.push_back(std::move(ev.value()));
            }
        }

        std::vector<std::optional<ColumnVector>> arg_cols;
        arg_cols.reserve(internal_aggs_.size());
        for (auto& ia : internal_aggs_) {
            if (ia.arg) {
                auto ev = ia.arg->evaluate(chunk);
                if (ev.is_err())
                    return Result<void>::err(ev.error().message);
                arg_cols.push_back(std::move(ev.value()));
            } else {
                arg_cols.push_back(std::nullopt);
            }
        }

        u32 rc = static_cast<u32>(chunk.row_count());
        for (u32 r = 0; r < rc; ++r) {
            u32 group_idx = group_keys_.empty() ? 0u : find_or_create_group_(key_cols, r);
            for (size_t a = 0; a < internal_aggs_.size(); ++a)
                update_state_(a, group_idx, arg_cols[a], r);
        }
    }
    aggregated_ = true;
    child_->close();
    return Result<void>::ok();
}

Chunk HashAggregate::emit_slice_() {
    size_t n = std::min<size_t>(TableScan::CHUNK_SIZE, num_groups_ - emit_cursor_);

    std::vector<ColumnVector> out_cols;
    out_cols.reserve(group_key_cols_.size() + output_bindings_.size());

    for (const auto& gk : group_key_cols_) {
        ColumnVector out = ColumnVector::empty(gk.type(), true, n);
        switch (gk.type()) {
        case TypeId::INT32:
            for (size_t i = 0; i < n; ++i) {
                size_t g = emit_cursor_ + i;
                if (gk.is_null(g))
                    out.append_null();
                else
                    out.append_i32(gk.get_i32(g));
            }
            break;
        case TypeId::INT64:
            for (size_t i = 0; i < n; ++i) {
                size_t g = emit_cursor_ + i;
                if (gk.is_null(g))
                    out.append_null();
                else
                    out.append_i64(gk.get_i64(g));
            }
            break;
        case TypeId::DOUBLE:
            for (size_t i = 0; i < n; ++i) {
                size_t g = emit_cursor_ + i;
                if (gk.is_null(g))
                    out.append_null();
                else
                    out.append_f64(gk.get_f64(g));
            }
            break;
        default:
            assert(false);
        }
        out_cols.push_back(std::move(out));
    }

    for (size_t c = 0; c < output_bindings_.size(); ++c) {
        const OutputBinding& b = output_bindings_[c];
        bool nullable = output_schema_[group_key_cols_.size() + c].nullable;
        ColumnVector out = ColumnVector::empty(b.output_type, nullable, n);

        if (b.kind == OutputBinding::Kind::AVG_DIVIDE) {
            const ColumnVector& sum_col = group_state_cols_[b.state_idx_a];
            const ColumnVector& count_col = group_state_cols_[b.state_idx_b];
            for (size_t i = 0; i < n; ++i) {
                size_t g = emit_cursor_ + i;
                i64 count = count_col.get_i64(g);
                if (count == 0) {
                    out.append_null();
                } else {
                    f64 sum_d = (sum_col.type() == TypeId::DOUBLE)
                                    ? sum_col.get_f64(g)
                                    : static_cast<f64>(sum_col.get_i64(g));
                    out.append_f64(sum_d / static_cast<f64>(count));
                }
            }
            out_cols.push_back(std::move(out));
            continue;
        }

        const ColumnVector& src = group_state_cols_[b.state_idx_a];
        switch (b.output_type) {
        case TypeId::INT32:
            for (size_t i = 0; i < n; ++i) {
                size_t g = emit_cursor_ + i;
                if (nullable && src.is_null(g))
                    out.append_null();
                else
                    out.append_i32(src.get_i32(g));
            }
            break;
        case TypeId::INT64:
            for (size_t i = 0; i < n; ++i) {
                size_t g = emit_cursor_ + i;
                if (nullable && src.is_null(g))
                    out.append_null();
                else
                    out.append_i64(src.get_i64(g));
            }
            break;
        case TypeId::DOUBLE:
            for (size_t i = 0; i < n; ++i) {
                size_t g = emit_cursor_ + i;
                if (nullable && src.is_null(g))
                    out.append_null();
                else
                    out.append_f64(src.get_f64(g));
            }
            break;
        default:
            assert(false);
        }
        out_cols.push_back(std::move(out));
    }
    emit_cursor_ += n;
    return Chunk(n, std::move(out_cols));
}

Result<std::optional<Chunk>> HashAggregate::next() {
    assert(opened_);
    if (!aggregated_) {
        auto r = aggregate_all_();
        if (r.is_err())
            return Result<std::optional<Chunk>>::err(r.error().message);
    }
    if (emit_cursor_ >= num_groups_)
        return Result<std::optional<Chunk>>::ok(std::nullopt);
    return Result<std::optional<Chunk>>::ok(emit_slice_());
}

} // namespace nyx
