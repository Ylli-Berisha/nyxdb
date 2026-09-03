#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/limit.h"
#include "executor/table_scan.h"
#include "storage/disk/table.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <vector>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_limit_test";

namespace {

class CountingChild : public Operator {
  public:
    CountingChild(Schema schema, std::vector<size_t> chunk_row_counts)
        : schema_(std::move(schema)), chunk_row_counts_(std::move(chunk_row_counts)) {}

    Result<void> open() override {
        ++open_calls;
        return Result<void>::ok();
    }

    Result<std::optional<Chunk>> next() override {
        ++next_calls;
        if (idx_ >= chunk_row_counts_.size())
            return Result<std::optional<Chunk>>::ok(std::nullopt);
        size_t rc = chunk_row_counts_[idx_++];
        std::vector<ColumnVector> cols;
        cols.reserve(schema_.size());
        for (const auto& col : schema_) {
            auto cv = ColumnVector::make(col.type, rc, col.nullable);
            for (size_t i = 0; i < rc; ++i) {
                if (col.type == TypeId::INT64)
                    cv.set_i64(i, static_cast<i64>(base_row_ + i));
                else if (col.type == TypeId::INT32)
                    cv.set_i32(i, static_cast<i32>(base_row_ + i));
                else if (col.type == TypeId::DOUBLE)
                    cv.set_f64(i, static_cast<f64>(base_row_ + i));
            }
            cols.push_back(std::move(cv));
        }
        base_row_ += rc;
        return Result<std::optional<Chunk>>::ok(Chunk(rc, std::move(cols)));
    }

    void close() override { ++close_calls; }
    const Schema& output_schema() const override { return schema_; }

    int open_calls = 0;
    int next_calls = 0;
    int close_calls = 0;

  private:
    Schema schema_;
    std::vector<size_t> chunk_row_counts_;
    size_t idx_ = 0;
    size_t base_row_ = 0;
};

} // namespace

TEST(ChunkResizeTest, Prefix) {
    auto c0 = ColumnVector::make(TypeId::INT32, 5, false);
    auto c1 = ColumnVector::make(TypeId::INT64, 5, false);
    auto c2 = ColumnVector::make(TypeId::DOUBLE, 5, true);
    for (size_t i = 0; i < 5; ++i) {
        c0.set_i32(i, static_cast<i32>(i));
        c1.set_i64(i, static_cast<i64>(i) * 10);
        c2.set_f64(i, static_cast<f64>(i) + 0.25);
    }
    c2.set_null(1);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(c0));
    cols.push_back(std::move(c1));
    cols.push_back(std::move(c2));
    Chunk chunk(5, std::move(cols));

    chunk.resize(3);

    EXPECT_EQ(chunk.row_count(), 3u);
    EXPECT_EQ(chunk.column(0).size(), 3u);
    EXPECT_EQ(chunk.column(1).size(), 3u);
    EXPECT_EQ(chunk.column(2).size(), 3u);
    EXPECT_EQ(chunk.column(0).get_i32(0), 0);
    EXPECT_EQ(chunk.column(0).get_i32(2), 2);
    EXPECT_EQ(chunk.column(1).get_i64(1), 10);
    EXPECT_FALSE(chunk.column(2).is_null(0));
    EXPECT_TRUE(chunk.column(2).is_null(1));
    EXPECT_DOUBLE_EQ(chunk.column(2).get_f64(2), 2.25);
}

class LimitTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(TEST_ROOT); }
    void TearDown() override { fs::remove_all(TEST_ROOT); }

    Table make_int64_table(const std::string& name, const std::vector<i64>& values) {
        Schema s = {{"v", TypeId::INT64, false}};
        auto tres = Table::create(TEST_ROOT, name, s);
        EXPECT_TRUE(tres.is_ok());
        auto t = std::move(tres.value());
        std::vector<std::vector<Value>> rows;
        rows.reserve(values.size());
        for (i64 v : values)
            rows.push_back({Value{v}});
        if (!rows.empty()) {
            EXPECT_TRUE(t.insert_many(rows).is_ok());
        }
        return t;
    }
};

TEST_F(LimitTest, Zero) {
    std::vector<i64> values;
    for (i64 i = 0; i < 100; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 0);
    ASSERT_TRUE(l.open().is_ok());
    auto n = l.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
}

TEST_F(LimitTest, LargerThanTable) {
    std::vector<i64> values;
    for (i64 i = 0; i < 100; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 1000);
    ASSERT_TRUE(l.open().is_ok());

    size_t total = 0;
    while (true) {
        auto n = l.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        total += n.value()->row_count();
    }
    EXPECT_EQ(total, 100u);
}

TEST_F(LimitTest, ExactlyOneChunk) {
    std::vector<i64> values;
    for (i64 i = 0; i < 3000; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 1024);
    ASSERT_TRUE(l.open().is_ok());

    auto n = l.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    EXPECT_EQ(n.value()->row_count(), 1024u);

    auto n2 = l.next();
    ASSERT_TRUE(n2.is_ok());
    EXPECT_FALSE(n2.value().has_value());
}

TEST_F(LimitTest, InsideFirstChunk) {
    std::vector<i64> values;
    for (i64 i = 0; i < 3000; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 500);
    ASSERT_TRUE(l.open().is_ok());

    auto n = l.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 500u);
    for (size_t i = 0; i < 500; ++i)
        EXPECT_EQ(c.column(0).get_i64(i), static_cast<i64>(i));

    auto n2 = l.next();
    ASSERT_TRUE(n2.is_ok());
    EXPECT_FALSE(n2.value().has_value());
}

TEST_F(LimitTest, AcrossChunks) {
    std::vector<i64> values;
    for (i64 i = 0; i < 3000; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 1500);
    ASSERT_TRUE(l.open().is_ok());

    std::vector<i64> got;
    while (true) {
        auto n = l.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.push_back(c.column(0).get_i64(i));
    }
    ASSERT_EQ(got.size(), 1500u);
    for (size_t i = 0; i < 1500; ++i)
        EXPECT_EQ(got[i], static_cast<i64>(i));
}

TEST_F(LimitTest, EmptyChild) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "empty", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 10);
    ASSERT_TRUE(l.open().is_ok());
    auto n = l.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
}

TEST_F(LimitTest, OutputSchemaMatchesChild) {
    Schema s = {{"a", TypeId::INT32, false}, {"b", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "twocol", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    Limit l(std::move(scan), 10);
    const Schema& os = l.output_schema();
    ASSERT_EQ(os.size(), 2u);
    EXPECT_EQ(os[0].name, "a");
    EXPECT_EQ(os[1].name, "b");
}

TEST_F(LimitTest, RepeatedNextAfterExhausted) {
    std::vector<i64> values;
    for (i64 i = 0; i < 50; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 10);
    ASSERT_TRUE(l.open().is_ok());

    auto n1 = l.next();
    ASSERT_TRUE(n1.is_ok());
    ASSERT_TRUE(n1.value().has_value());
    EXPECT_EQ(n1.value()->row_count(), 10u);

    for (int i = 0; i < 3; ++i) {
        auto n = l.next();
        ASSERT_TRUE(n.is_ok());
        EXPECT_FALSE(n.value().has_value());
    }
}

TEST(LimitLifecycleTest, ClosesChildOnBudgetHit) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto child = std::make_unique<CountingChild>(s, std::vector<size_t>{100, 100, 100});
    auto* raw = child.get();

    Limit l(std::move(child), 150);
    ASSERT_TRUE(l.open().is_ok());
    EXPECT_EQ(raw->open_calls, 1);

    auto n1 = l.next();
    ASSERT_TRUE(n1.is_ok());
    ASSERT_TRUE(n1.value().has_value());
    EXPECT_EQ(n1.value()->row_count(), 100u);
    EXPECT_EQ(raw->close_calls, 0);

    auto n2 = l.next();
    ASSERT_TRUE(n2.is_ok());
    ASSERT_TRUE(n2.value().has_value());
    EXPECT_EQ(n2.value()->row_count(), 50u);
    EXPECT_EQ(raw->close_calls, 1);

    int next_calls_before = raw->next_calls;
    auto n3 = l.next();
    ASSERT_TRUE(n3.is_ok());
    EXPECT_FALSE(n3.value().has_value());
    EXPECT_EQ(raw->close_calls, 1);
    EXPECT_EQ(raw->next_calls, next_calls_before);
}

TEST(LimitLifecycleTest, ClosesChildWhenChildExhausts) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto child = std::make_unique<CountingChild>(s, std::vector<size_t>{100, 100});
    auto* raw = child.get();

    Limit l(std::move(child), 1000);
    ASSERT_TRUE(l.open().is_ok());

    while (true) {
        auto n = l.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
    }
    EXPECT_EQ(raw->close_calls, 1);
}

TEST(LimitLifecycleTest, CloseIsIdempotent) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto child = std::make_unique<CountingChild>(s, std::vector<size_t>{100, 100});
    auto* raw = child.get();

    Limit l(std::move(child), 50);
    ASSERT_TRUE(l.open().is_ok());

    auto n = l.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    EXPECT_EQ(raw->close_calls, 1);

    l.close();
    EXPECT_EQ(raw->close_calls, 1);

    l.close();
    EXPECT_EQ(raw->close_calls, 1);
}

TEST(LimitLifecycleTest, ZeroClosesChildEagerly) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto child = std::make_unique<CountingChild>(s, std::vector<size_t>{100});
    auto* raw = child.get();

    Limit l(std::move(child), 0);
    ASSERT_TRUE(l.open().is_ok());
    auto n = l.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
    EXPECT_EQ(raw->close_calls, 1);
    EXPECT_EQ(raw->next_calls, 0);
}

TEST(ChunkDropPrefixTest, Basic) {
    auto c0 = ColumnVector::make(TypeId::INT32, 5, false);
    auto c1 = ColumnVector::make(TypeId::INT64, 5, false);
    auto c2 = ColumnVector::make(TypeId::DOUBLE, 5, false);
    for (size_t i = 0; i < 5; ++i) {
        c0.set_i32(i, static_cast<i32>(i));
        c1.set_i64(i, static_cast<i64>(i) * 10);
        c2.set_f64(i, static_cast<f64>(i) + 0.5);
    }
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(c0));
    cols.push_back(std::move(c1));
    cols.push_back(std::move(c2));
    Chunk chunk(5, std::move(cols));

    chunk.drop_prefix(2);

    EXPECT_EQ(chunk.row_count(), 3u);
    EXPECT_EQ(chunk.column(0).get_i32(0), 2);
    EXPECT_EQ(chunk.column(0).get_i32(2), 4);
    EXPECT_EQ(chunk.column(1).get_i64(0), 20);
    EXPECT_EQ(chunk.column(1).get_i64(2), 40);
    EXPECT_DOUBLE_EQ(chunk.column(2).get_f64(0), 2.5);
    EXPECT_DOUBLE_EQ(chunk.column(2).get_f64(2), 4.5);
}

TEST(ChunkDropPrefixTest, WithNulls) {
    auto c = ColumnVector::make(TypeId::INT64, 6, true);
    for (size_t i = 0; i < 6; ++i)
        c.set_i64(i, static_cast<i64>(i));
    c.set_null(1);
    c.set_null(4);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(c));
    Chunk chunk(6, std::move(cols));

    chunk.drop_prefix(2);

    ASSERT_EQ(chunk.row_count(), 4u);
    EXPECT_FALSE(chunk.column(0).is_null(0));
    EXPECT_EQ(chunk.column(0).get_i64(0), 2);
    EXPECT_FALSE(chunk.column(0).is_null(1));
    EXPECT_EQ(chunk.column(0).get_i64(1), 3);
    EXPECT_TRUE(chunk.column(0).is_null(2));
    EXPECT_FALSE(chunk.column(0).is_null(3));
    EXPECT_EQ(chunk.column(0).get_i64(3), 5);
}

TEST(ChunkDropPrefixTest, DropAll) {
    auto c = ColumnVector::make(TypeId::INT32, 4, false);
    for (size_t i = 0; i < 4; ++i)
        c.set_i32(i, static_cast<i32>(i));
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(c));
    Chunk chunk(4, std::move(cols));

    chunk.drop_prefix(4);

    EXPECT_EQ(chunk.row_count(), 0u);
    EXPECT_EQ(chunk.column(0).size(), 0u);
}

TEST(ChunkDropPrefixTest, DropZero) {
    auto c = ColumnVector::make(TypeId::INT32, 3, false);
    for (size_t i = 0; i < 3; ++i)
        c.set_i32(i, static_cast<i32>(i) * 7);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(c));
    Chunk chunk(3, std::move(cols));

    chunk.drop_prefix(0);

    ASSERT_EQ(chunk.row_count(), 3u);
    EXPECT_EQ(chunk.column(0).get_i32(0), 0);
    EXPECT_EQ(chunk.column(0).get_i32(1), 7);
    EXPECT_EQ(chunk.column(0).get_i32(2), 14);
}

TEST_F(LimitTest, OffsetZeroBehavesLikeNoOffset) {
    std::vector<i64> values;
    for (i64 i = 0; i < 50; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 0, 10);
    ASSERT_TRUE(l.open().is_ok());

    auto n = l.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 10u);
    for (size_t i = 0; i < 10; ++i)
        EXPECT_EQ(c.column(0).get_i64(i), static_cast<i64>(i));
}

TEST_F(LimitTest, OffsetWithinFirstChunk) {
    std::vector<i64> values;
    for (i64 i = 0; i < 3000; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 100, 50);
    ASSERT_TRUE(l.open().is_ok());

    std::vector<i64> got;
    while (true) {
        auto n = l.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.push_back(c.column(0).get_i64(i));
    }
    ASSERT_EQ(got.size(), 50u);
    for (size_t i = 0; i < 50; ++i)
        EXPECT_EQ(got[i], static_cast<i64>(100 + i));
}

TEST_F(LimitTest, OffsetAcrossChunks) {
    std::vector<i64> values;
    for (i64 i = 0; i < 3000; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 1500, 100);
    ASSERT_TRUE(l.open().is_ok());

    std::vector<i64> got;
    while (true) {
        auto n = l.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.push_back(c.column(0).get_i64(i));
    }
    ASSERT_EQ(got.size(), 100u);
    for (size_t i = 0; i < 100; ++i)
        EXPECT_EQ(got[i], static_cast<i64>(1500 + i));
}

TEST_F(LimitTest, OffsetLargerThanTable) {
    std::vector<i64> values;
    for (i64 i = 0; i < 100; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 5000, 10);
    ASSERT_TRUE(l.open().is_ok());

    auto n = l.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
}

TEST_F(LimitTest, OffsetEqualsTableSize) {
    std::vector<i64> values;
    for (i64 i = 0; i < 100; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 100, 10);
    ASSERT_TRUE(l.open().is_ok());

    auto n = l.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
}

TEST_F(LimitTest, OffsetWithHugeLimit) {
    std::vector<i64> values;
    for (i64 i = 0; i < 3000; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Limit l(std::move(scan), 50, std::numeric_limits<size_t>::max());
    ASSERT_TRUE(l.open().is_ok());

    size_t total = 0;
    i64 first = -1;
    i64 last = -1;
    while (true) {
        auto n = l.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        if (total == 0)
            first = c.column(0).get_i64(0);
        last = c.column(0).get_i64(c.row_count() - 1);
        total += c.row_count();
    }
    EXPECT_EQ(total, 2950u);
    EXPECT_EQ(first, 50);
    EXPECT_EQ(last, 2999);
}

TEST(LimitLifecycleTest, OffsetExhaustsChildClosesEarly) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto child = std::make_unique<CountingChild>(s, std::vector<size_t>{100, 100});
    auto* raw = child.get();

    Limit l(std::move(child), 1000, 10);
    ASSERT_TRUE(l.open().is_ok());

    auto n = l.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
    EXPECT_EQ(raw->close_calls, 1);
}

TEST(LimitLifecycleTest, WholeChunkSkipDoesNotOverpull) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto child = std::make_unique<CountingChild>(s, std::vector<size_t>{100, 100, 100, 100, 100});
    auto* raw = child.get();

    Limit l(std::move(child), 250, 50);
    ASSERT_TRUE(l.open().is_ok());

    auto n = l.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    EXPECT_EQ(n.value()->row_count(), 50u);
    EXPECT_EQ(raw->next_calls, 3);
    EXPECT_EQ(raw->close_calls, 1);
}
