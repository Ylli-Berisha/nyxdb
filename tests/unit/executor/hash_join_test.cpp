#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/expression.h"
#include "executor/hash_join.h"
#include "executor/table_scan.h"
#include "storage/disk/table.h"

#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <tuple>
#include <vector>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_hash_join_test";

using Row4 = std::tuple<i64, i64, i64, i64>;

class HashJoinTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(TEST_ROOT); }
    void TearDown() override { fs::remove_all(TEST_ROOT); }

    Table make_kv_table(const std::string& name, const std::vector<std::pair<i64, i64>>& rows,
                        bool nullable_id = false, const std::vector<size_t>& null_positions = {}) {
        Schema s = {{"id", TypeId::INT64, nullable_id}, {"v", TypeId::INT64, false}};
        auto tres = Table::create(TEST_ROOT, name, s);
        EXPECT_TRUE(tres.is_ok());
        auto t = std::move(tres.value());
        std::vector<std::vector<Value>> data;
        data.reserve(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
            bool is_null =
                std::find(null_positions.begin(), null_positions.end(), i) != null_positions.end();
            Value id_v = is_null ? Value{std::monostate{}} : Value{rows[i].first};
            data.push_back({id_v, Value{rows[i].second}});
        }
        EXPECT_TRUE(t.insert_many(data).is_ok());
        return t;
    }

    static std::vector<std::unique_ptr<Expression>> col0_i64_key() {
        std::vector<std::unique_ptr<Expression>> k;
        k.push_back(std::make_unique<ColumnRef>(0, TypeId::INT64));
        return k;
    }

    static std::vector<Row4> drain(Operator& op) {
        EXPECT_TRUE(op.open().is_ok());
        std::vector<Row4> out;
        while (true) {
            auto n = op.next();
            EXPECT_TRUE(n.is_ok());
            if (!n.value().has_value())
                break;
            const Chunk& c = *n.value();
            EXPECT_FALSE(c.has_sel());
            for (size_t i = 0; i < c.row_count(); ++i) {
                out.emplace_back(c.column(0).get_i64(i), c.column(1).get_i64(i),
                                 c.column(2).get_i64(i), c.column(3).get_i64(i));
            }
        }
        op.close();
        return out;
    }

    static std::vector<Row4> sorted(std::vector<Row4> v) {
        std::sort(v.begin(), v.end());
        return v;
    }
};

TEST_F(HashJoinTest, BasicInnerJoin) {
    auto l = make_kv_table("l", {{1, 10}, {2, 20}, {3, 30}});
    auto r = make_kv_table("r", {{1, 100}, {2, 200}, {3, 300}});

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key());

    auto got = sorted(drain(join));
    std::vector<Row4> expected = {{1, 10, 1, 100}, {2, 20, 2, 200}, {3, 30, 3, 300}};
    EXPECT_EQ(got, expected);
}

TEST_F(HashJoinTest, NoMatches) {
    auto l = make_kv_table("l", {{1, 10}, {2, 20}, {3, 30}});
    auto r = make_kv_table("r", {{4, 400}, {5, 500}, {6, 600}});

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key());

    auto got = drain(join);
    EXPECT_TRUE(got.empty());
}

TEST_F(HashJoinTest, DuplicateKeysOnBuild) {
    auto l = make_kv_table("l", {{1, 10}});
    auto r = make_kv_table("r", {{1, 100}, {1, 101}, {1, 102}});

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key());

    auto got = sorted(drain(join));
    std::vector<Row4> expected = {{1, 10, 1, 100}, {1, 10, 1, 101}, {1, 10, 1, 102}};
    EXPECT_EQ(got, expected);
}

TEST_F(HashJoinTest, DuplicateKeysOnProbe) {
    auto l = make_kv_table("l", {{1, 10}, {1, 11}, {1, 12}});
    auto r = make_kv_table("r", {{1, 100}});

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key());

    auto got = sorted(drain(join));
    std::vector<Row4> expected = {{1, 10, 1, 100}, {1, 11, 1, 100}, {1, 12, 1, 100}};
    EXPECT_EQ(got, expected);
}

TEST_F(HashJoinTest, DuplicatesBothSides) {
    auto l = make_kv_table("l", {{1, 10}, {1, 11}});
    auto r = make_kv_table("r", {{1, 100}, {1, 101}});

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key());

    auto got = sorted(drain(join));
    std::vector<Row4> expected = {
        {1, 10, 1, 100},
        {1, 10, 1, 101},
        {1, 11, 1, 100},
        {1, 11, 1, 101},
    };
    EXPECT_EQ(got, expected);
}

TEST_F(HashJoinTest, EmptyBuild) {
    Schema s = {{"id", TypeId::INT64, false}, {"v", TypeId::INT64, false}};
    auto rt = Table::create(TEST_ROOT, "r_empty", s);
    ASSERT_TRUE(rt.is_ok());
    auto r = std::move(rt.value());

    auto l = make_kv_table("l", {{1, 10}, {2, 20}});

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key());

    auto got = drain(join);
    EXPECT_TRUE(got.empty());
}

TEST_F(HashJoinTest, EmptyProbe) {
    auto r = make_kv_table("r", {{1, 100}, {2, 200}});

    Schema s = {{"id", TypeId::INT64, false}, {"v", TypeId::INT64, false}};
    auto lt = Table::create(TEST_ROOT, "l_empty", s);
    ASSERT_TRUE(lt.is_ok());
    auto l = std::move(lt.value());

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key());

    auto got = drain(join);
    EXPECT_TRUE(got.empty());
}

TEST_F(HashJoinTest, LargeBuild) {
    std::vector<std::pair<i64, i64>> l_rows;
    std::vector<std::pair<i64, i64>> r_rows;
    for (i64 i = 0; i < 10000; ++i) {
        l_rows.push_back({i, i * 10});
        r_rows.push_back({i, i * 100});
    }
    auto l = make_kv_table("l", l_rows);
    auto r = make_kv_table("r", r_rows);

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key());

    auto got = sorted(drain(join));
    ASSERT_EQ(got.size(), 10000u);
    for (size_t i = 0; i < 10000; ++i) {
        i64 expected_id = static_cast<i64>(i);
        EXPECT_EQ(std::get<0>(got[i]), expected_id);
        EXPECT_EQ(std::get<1>(got[i]), expected_id * 10);
        EXPECT_EQ(std::get<2>(got[i]), expected_id);
        EXPECT_EQ(std::get<3>(got[i]), expected_id * 100);
    }
}

TEST_F(HashJoinTest, NullKeysDropped) {
    auto l = make_kv_table("l", {{1, 10}, {0, 11}, {2, 12}, {0, 13}, {3, 14}}, true, {1, 3});
    auto r = make_kv_table("r", {{1, 100}, {0, 200}, {3, 300}}, true, {1});

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key());

    auto got = sorted(drain(join));
    std::vector<Row4> expected = {{1, 10, 1, 100}, {3, 14, 3, 300}};
    EXPECT_EQ(got, expected);
}

TEST_F(HashJoinTest, OutputChunksBoundedByCHUNK_SIZE) {
    auto r = make_kv_table("r", {{42, 4200}});

    std::vector<std::pair<i64, i64>> l_rows;
    for (i64 i = 0; i < 3000; ++i)
        l_rows.push_back({42, i});
    auto l = make_kv_table("l", l_rows);

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key());

    ASSERT_TRUE(join.open().is_ok());
    std::vector<size_t> chunk_sizes;
    size_t total = 0;
    while (true) {
        auto n = join.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        chunk_sizes.push_back(n.value()->row_count());
        total += n.value()->row_count();
    }
    join.close();

    EXPECT_EQ(total, 3000u);
    for (size_t s : chunk_sizes)
        EXPECT_LE(s, static_cast<size_t>(TableScan::CHUNK_SIZE));
    EXPECT_GE(chunk_sizes.size(), 3u);
}

TEST_F(HashJoinTest, MultiKeyBothInt64) {
    Schema s = {
        {"a", TypeId::INT64, false}, {"b", TypeId::INT64, false}, {"v", TypeId::INT64, false}};
    auto lt = Table::create(TEST_ROOT, "l_mk", s);
    ASSERT_TRUE(lt.is_ok());
    auto l = std::move(lt.value());
    auto rt = Table::create(TEST_ROOT, "r_mk", s);
    ASSERT_TRUE(rt.is_ok());
    auto r = std::move(rt.value());

    std::vector<std::vector<Value>> l_rows = {
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(10)}, Value{static_cast<i64>(100)}},
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(20)}, Value{static_cast<i64>(101)}},
        {Value{static_cast<i64>(2)}, Value{static_cast<i64>(10)}, Value{static_cast<i64>(102)}},
    };
    ASSERT_TRUE(l.insert_many(l_rows).is_ok());
    std::vector<std::vector<Value>> r_rows = {
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(10)}, Value{static_cast<i64>(1000)}},
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(99)}, Value{static_cast<i64>(1001)}},
        {Value{static_cast<i64>(2)}, Value{static_cast<i64>(10)}, Value{static_cast<i64>(1002)}},
    };
    ASSERT_TRUE(r.insert_many(r_rows).is_ok());

    std::vector<std::unique_ptr<Expression>> build_keys;
    build_keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT64));
    build_keys.push_back(std::make_unique<ColumnRef>(1, TypeId::INT64));
    std::vector<std::unique_ptr<Expression>> probe_keys;
    probe_keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT64));
    probe_keys.push_back(std::make_unique<ColumnRef>(1, TypeId::INT64));

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1, 2});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1, 2});
    HashJoin join(std::move(build_scan), std::move(probe_scan), std::move(build_keys),
                  std::move(probe_keys));

    ASSERT_TRUE(join.open().is_ok());
    std::vector<std::tuple<i64, i64, i64, i64, i64, i64>> got;
    while (true) {
        auto n = join.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i64(i), c.column(1).get_i64(i), c.column(2).get_i64(i),
                             c.column(3).get_i64(i), c.column(4).get_i64(i),
                             c.column(5).get_i64(i));
    }
    join.close();
    std::sort(got.begin(), got.end());
    std::vector<std::tuple<i64, i64, i64, i64, i64, i64>> expected = {
        {1, 10, 100, 1, 10, 1000},
        {2, 10, 102, 2, 10, 1002},
    };
    EXPECT_EQ(got, expected);
}

TEST_F(HashJoinTest, MultiKeyMixedTypes) {
    Schema ls = {
        {"a", TypeId::INT32, false}, {"b", TypeId::DOUBLE, false}, {"v", TypeId::INT64, false}};
    Schema rs = {
        {"a", TypeId::INT32, false}, {"b", TypeId::DOUBLE, false}, {"w", TypeId::INT64, false}};
    auto lt = Table::create(TEST_ROOT, "l_mkt", ls);
    ASSERT_TRUE(lt.is_ok());
    auto l = std::move(lt.value());
    auto rt = Table::create(TEST_ROOT, "r_mkt", rs);
    ASSERT_TRUE(rt.is_ok());
    auto r = std::move(rt.value());

    std::vector<std::vector<Value>> l_rows = {
        {Value{static_cast<i32>(1)}, Value{static_cast<f64>(1.5)}, Value{static_cast<i64>(10)}},
        {Value{static_cast<i32>(2)}, Value{static_cast<f64>(2.5)}, Value{static_cast<i64>(20)}},
        {Value{static_cast<i32>(1)}, Value{static_cast<f64>(9.9)}, Value{static_cast<i64>(30)}},
    };
    ASSERT_TRUE(l.insert_many(l_rows).is_ok());
    std::vector<std::vector<Value>> r_rows = {
        {Value{static_cast<i32>(1)}, Value{static_cast<f64>(1.5)}, Value{static_cast<i64>(100)}},
        {Value{static_cast<i32>(2)}, Value{static_cast<f64>(2.5)}, Value{static_cast<i64>(200)}},
        {Value{static_cast<i32>(3)}, Value{static_cast<f64>(3.5)}, Value{static_cast<i64>(300)}},
    };
    ASSERT_TRUE(r.insert_many(r_rows).is_ok());

    std::vector<std::unique_ptr<Expression>> build_keys;
    build_keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT32));
    build_keys.push_back(std::make_unique<ColumnRef>(1, TypeId::DOUBLE));
    std::vector<std::unique_ptr<Expression>> probe_keys;
    probe_keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT32));
    probe_keys.push_back(std::make_unique<ColumnRef>(1, TypeId::DOUBLE));

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1, 2});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1, 2});
    HashJoin join(std::move(build_scan), std::move(probe_scan), std::move(build_keys),
                  std::move(probe_keys));

    ASSERT_TRUE(join.open().is_ok());
    std::vector<std::tuple<i32, f64, i64, i32, f64, i64>> got;
    while (true) {
        auto n = join.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i32(i), c.column(1).get_f64(i), c.column(2).get_i64(i),
                             c.column(3).get_i32(i), c.column(4).get_f64(i),
                             c.column(5).get_i64(i));
    }
    join.close();
    std::sort(got.begin(), got.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(std::get<0>(got[0]), 1);
    EXPECT_EQ(std::get<1>(got[0]), 1.5);
    EXPECT_EQ(std::get<2>(got[0]), 10);
    EXPECT_EQ(std::get<5>(got[0]), 100);
    EXPECT_EQ(std::get<0>(got[1]), 2);
    EXPECT_EQ(std::get<1>(got[1]), 2.5);
    EXPECT_EQ(std::get<2>(got[1]), 20);
    EXPECT_EQ(std::get<5>(got[1]), 200);
}

TEST_F(HashJoinTest, MultiKeyNullInAnyKeyDropped) {
    Schema s = {
        {"a", TypeId::INT64, true}, {"b", TypeId::INT64, true}, {"v", TypeId::INT64, false}};
    auto lt = Table::create(TEST_ROOT, "l_nk", s);
    ASSERT_TRUE(lt.is_ok());
    auto l = std::move(lt.value());
    auto rt = Table::create(TEST_ROOT, "r_nk", s);
    ASSERT_TRUE(rt.is_ok());
    auto r = std::move(rt.value());

    std::vector<std::vector<Value>> l_rows = {
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(10)}, Value{static_cast<i64>(100)}},
        {Value{static_cast<i64>(1)}, Value{std::monostate{}}, Value{static_cast<i64>(101)}},
        {Value{std::monostate{}}, Value{static_cast<i64>(10)}, Value{static_cast<i64>(102)}},
        {Value{static_cast<i64>(2)}, Value{static_cast<i64>(20)}, Value{static_cast<i64>(103)}},
    };
    ASSERT_TRUE(l.insert_many(l_rows).is_ok());
    std::vector<std::vector<Value>> r_rows = {
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(10)}, Value{static_cast<i64>(1000)}},
        {Value{static_cast<i64>(2)}, Value{std::monostate{}}, Value{static_cast<i64>(1001)}},
        {Value{static_cast<i64>(2)}, Value{static_cast<i64>(20)}, Value{static_cast<i64>(1002)}},
    };
    ASSERT_TRUE(r.insert_many(r_rows).is_ok());

    std::vector<std::unique_ptr<Expression>> build_keys;
    build_keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT64));
    build_keys.push_back(std::make_unique<ColumnRef>(1, TypeId::INT64));
    std::vector<std::unique_ptr<Expression>> probe_keys;
    probe_keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT64));
    probe_keys.push_back(std::make_unique<ColumnRef>(1, TypeId::INT64));

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1, 2});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1, 2});
    HashJoin join(std::move(build_scan), std::move(probe_scan), std::move(build_keys),
                  std::move(probe_keys));

    ASSERT_TRUE(join.open().is_ok());
    std::vector<std::tuple<i64, i64, i64, i64, i64, i64>> got;
    while (true) {
        auto n = join.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i) {
            EXPECT_FALSE(c.column(0).is_null(i));
            EXPECT_FALSE(c.column(1).is_null(i));
            EXPECT_FALSE(c.column(3).is_null(i));
            EXPECT_FALSE(c.column(4).is_null(i));
            got.emplace_back(c.column(0).get_i64(i), c.column(1).get_i64(i), c.column(2).get_i64(i),
                             c.column(3).get_i64(i), c.column(4).get_i64(i),
                             c.column(5).get_i64(i));
        }
    }
    join.close();
    std::sort(got.begin(), got.end());
    std::vector<std::tuple<i64, i64, i64, i64, i64, i64>> expected = {
        {1, 10, 100, 1, 10, 1000},
        {2, 20, 103, 2, 20, 1002},
    };
    EXPECT_EQ(got, expected);
}

TEST_F(HashJoinTest, LeftOuterEmitsNullForUnmatched) {
    auto l = make_kv_table("l", {{1, 10}, {2, 20}, {3, 30}, {4, 40}});
    auto r = make_kv_table("r", {{1, 100}, {3, 300}});

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key(),
                  JoinType::LEFT_OUTER);

    ASSERT_TRUE(join.open().is_ok());
    std::vector<std::tuple<i64, i64, bool, i64, bool, i64>> got;
    while (true) {
        auto n = join.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i) {
            bool id_null = c.column(2).is_null(i);
            bool w_null = c.column(3).is_null(i);
            got.emplace_back(c.column(0).get_i64(i), c.column(1).get_i64(i), id_null,
                             id_null ? 0 : c.column(2).get_i64(i), w_null,
                             w_null ? 0 : c.column(3).get_i64(i));
        }
    }
    join.close();
    std::sort(got.begin(), got.end());
    std::vector<std::tuple<i64, i64, bool, i64, bool, i64>> expected = {
        {1, 10, false, 1, false, 100},
        {2, 20, true, 0, true, 0},
        {3, 30, false, 3, false, 300},
        {4, 40, true, 0, true, 0},
    };
    EXPECT_EQ(got, expected);
}

TEST_F(HashJoinTest, LeftOuterBuildColsForcedNullable) {
    Schema ls = {{"lid", TypeId::INT64, false}, {"lv", TypeId::INT64, false}};
    auto lt = Table::create(TEST_ROOT, "l_left", ls);
    ASSERT_TRUE(lt.is_ok());
    auto l = std::move(lt.value());
    Schema rs = {{"rid", TypeId::INT64, false}, {"rw", TypeId::INT64, false}};
    auto rt = Table::create(TEST_ROOT, "r_left", rs);
    ASSERT_TRUE(rt.is_ok());
    auto r = std::move(rt.value());

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key(),
                  JoinType::LEFT_OUTER);

    const Schema& os = join.output_schema();
    ASSERT_EQ(os.size(), 4u);
    EXPECT_FALSE(os[0].nullable);
    EXPECT_FALSE(os[1].nullable);
    EXPECT_TRUE(os[2].nullable);
    EXPECT_TRUE(os[3].nullable);
}

TEST_F(HashJoinTest, SemiJoinDeduplicatesBuildDuplicates) {
    auto l = make_kv_table("l", {{1, 10}, {2, 20}, {3, 30}});
    auto r = make_kv_table("r", {{1, 100}, {1, 101}, {1, 102}, {3, 300}});

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key(),
                  JoinType::SEMI);

    ASSERT_EQ(join.output_schema().size(), 2u);

    ASSERT_TRUE(join.open().is_ok());
    std::vector<std::pair<i64, i64>> got;
    while (true) {
        auto n = join.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        ASSERT_EQ(c.column_count(), 2u);
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i64(i), c.column(1).get_i64(i));
    }
    join.close();
    std::sort(got.begin(), got.end());
    std::vector<std::pair<i64, i64>> expected = {{1, 10}, {3, 30}};
    EXPECT_EQ(got, expected);
}

TEST_F(HashJoinTest, AntiJoinEmitsUnmatchedIncludingNullKeys) {
    auto l = make_kv_table("l", {{1, 10}, {2, 20}, {0, 30}, {3, 40}}, true, {2});
    auto r = make_kv_table("r", {{1, 100}, {3, 300}});

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key(),
                  JoinType::ANTI);

    ASSERT_EQ(join.output_schema().size(), 2u);

    ASSERT_TRUE(join.open().is_ok());
    std::vector<std::pair<bool, i64>> got_id;
    std::vector<i64> got_v;
    while (true) {
        auto n = join.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        ASSERT_EQ(c.column_count(), 2u);
        for (size_t i = 0; i < c.row_count(); ++i) {
            bool id_null = c.column(0).is_null(i);
            got_id.emplace_back(id_null, id_null ? 0 : c.column(0).get_i64(i));
            got_v.push_back(c.column(1).get_i64(i));
        }
    }
    join.close();
    ASSERT_EQ(got_v.size(), 2u);
    std::vector<std::pair<bool, i64>> want_id = {{false, 2}, {true, 0}};
    std::vector<i64> want_v = {20, 30};
    std::sort(got_v.begin(), got_v.end());
    std::sort(want_v.begin(), want_v.end());
    EXPECT_EQ(got_v, want_v);
    std::sort(got_id.begin(), got_id.end());
    std::sort(want_id.begin(), want_id.end());
    EXPECT_EQ(got_id, want_id);
}

TEST_F(HashJoinTest, OutputSchemaMatchesConcat) {
    Schema ls = {{"lid", TypeId::INT64, false}, {"lv", TypeId::INT64, false}};
    auto lt = Table::create(TEST_ROOT, "l_schema", ls);
    ASSERT_TRUE(lt.is_ok());
    auto l = std::move(lt.value());

    Schema rs = {{"rid", TypeId::INT64, false}, {"rw", TypeId::INT64, false}};
    auto rt = Table::create(TEST_ROOT, "r_schema", rs);
    ASSERT_TRUE(rt.is_ok());
    auto r = std::move(rt.value());

    auto build_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto probe_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    HashJoin join(std::move(build_scan), std::move(probe_scan), col0_i64_key(), col0_i64_key());

    const Schema& os = join.output_schema();
    ASSERT_EQ(os.size(), 4u);
    EXPECT_EQ(os[0].name, "lid");
    EXPECT_EQ(os[1].name, "lv");
    EXPECT_EQ(os[2].name, "rid");
    EXPECT_EQ(os[3].name, "rw");
}
