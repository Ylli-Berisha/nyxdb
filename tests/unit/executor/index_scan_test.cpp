#include "catalog/catalog.h"
#include "database/database.h"
#include "executor/index_scan.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <numeric>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_index_scan_test";

class IndexScanTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        {
            auto r = Database::open(ROOT);
            ASSERT_TRUE(r.is_ok()) << r.error().message;
            Database db(std::move(r.value()));

            auto ok = [&](const std::string& sql) {
                auto res = db.execute(sql);
                ASSERT_TRUE(res.is_ok()) << res.error().message;
            };
            ok("CREATE TABLE t (id INT NOT NULL, val INT NOT NULL)");
            for (int i = 1; i <= 20; ++i)
                ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) +
                   ")");
            ok("CREATE INDEX idx_id ON t (id)");
        }
        // Load a fresh catalog so we own the Table and BTreeIndex pointers.
        auto cr = Catalog::load(ROOT);
        ASSERT_TRUE(cr.is_ok()) << cr.error().message;
        cat_ = std::make_unique<Catalog>(std::move(cr.value()));
    }
    void TearDown() override {
        cat_.reset();
        fs::remove_all(ROOT);
    }

    std::vector<i32> scan_ids(std::optional<IndexScan::Bound> lo,
                              std::optional<IndexScan::Bound> hi) {
        Table* tbl = cat_->table("t");
        BTreeIndex* idx = cat_->btree_index("t", "idx_id");
        EXPECT_NE(tbl, nullptr);
        EXPECT_NE(idx, nullptr);

        IndexScan scan(tbl, idx, {0, 1}, std::move(lo), std::move(hi));
        EXPECT_TRUE(scan.open().is_ok());

        std::vector<i32> ids;
        while (true) {
            auto chunk = scan.next();
            EXPECT_TRUE(chunk.is_ok());
            if (!chunk.value().has_value())
                break;
            auto& cv = chunk.value()->column(0);
            for (usize r = 0; r < chunk.value()->row_count(); ++r)
                ids.push_back(cv.get_i32(r));
        }
        return ids;
    }

    std::unique_ptr<Catalog> cat_;
};

TEST_F(IndexScanTest, FullScan) {
    auto ids = scan_ids(std::nullopt, std::nullopt);
    ASSERT_EQ(ids.size(), 20u);
    for (int i = 0; i < 20; ++i)
        EXPECT_EQ(ids[static_cast<usize>(i)], i + 1);
}

TEST_F(IndexScanTest, PointLookup) {
    IndexScan::Bound lo{{Value{i32(7)}}, true};
    IndexScan::Bound hi{{Value{i32(7)}}, true};
    auto ids = scan_ids(lo, hi);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 7);
}

TEST_F(IndexScanTest, RangeInclusive) {
    IndexScan::Bound lo{{Value{i32(5)}}, true};
    IndexScan::Bound hi{{Value{i32(10)}}, true};
    auto ids = scan_ids(lo, hi);
    ASSERT_EQ(ids.size(), 6u);
    EXPECT_EQ(ids.front(), 5);
    EXPECT_EQ(ids.back(), 10);
}

TEST_F(IndexScanTest, RangeExclusiveBounds) {
    IndexScan::Bound lo{{Value{i32(5)}}, false};
    IndexScan::Bound hi{{Value{i32(10)}}, false};
    auto ids = scan_ids(lo, hi);
    ASSERT_EQ(ids.size(), 4u); // 6,7,8,9
    EXPECT_EQ(ids.front(), 6);
    EXPECT_EQ(ids.back(), 9);
}

TEST_F(IndexScanTest, LoBoundOnly) {
    IndexScan::Bound lo{{Value{i32(18)}}, true};
    auto ids = scan_ids(lo, std::nullopt);
    ASSERT_EQ(ids.size(), 3u); // 18,19,20
}

TEST_F(IndexScanTest, HiBoundOnly) {
    IndexScan::Bound hi{{Value{i32(3)}}, true};
    auto ids = scan_ids(std::nullopt, hi);
    ASSERT_EQ(ids.size(), 3u); // 1,2,3
}

TEST_F(IndexScanTest, EmptyRange) {
    IndexScan::Bound lo{{Value{i32(100)}}, true};
    auto ids = scan_ids(lo, std::nullopt);
    EXPECT_TRUE(ids.empty());
}
