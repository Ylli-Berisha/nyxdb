#include "catalog/catalog.h"
#include "database/database.h"
#include "storage/disk/table.h"
#include "storage/merge_worker.h"
#include "storage/wal/wal_reader.h"
#include "storage/wal/wal_record.h"
#include "storage/wal/wal_writer.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <thread>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_segment_db_test";

class SegmentDbTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(ROOT); }
    void TearDown() override { fs::remove_all(ROOT); }
};

static Result<ExecuteResult> exec(Database& db, const std::string& sql) {
    return db.execute(sql);
}

TEST_F(SegmentDbTest, WalSegmentFlushRecordRoundTrip) {
    std::string wal_path = ROOT + "/test_wal.bin";
    fs::create_directories(ROOT);

    {
        auto w = WalWriter::open(wal_path);
        ASSERT_TRUE(w.is_ok());
        ASSERT_TRUE(w.value().log_segment_flush("mytable", 65536u).is_ok());
    }

    auto r = WalReader::open(wal_path);
    ASSERT_TRUE(r.is_ok());
    auto recs = r.value().read_all();
    ASSERT_TRUE(recs.is_ok());
    ASSERT_EQ(recs.value().size(), 1u);

    const auto& rec = recs.value()[0];
    EXPECT_EQ(rec.type, WalRecord::Type::SegmentFlush);
    EXPECT_EQ(rec.table_name, "mytable");
    EXPECT_EQ(rec.sealed_row_count, 65536u);
}

TEST_F(SegmentDbTest, WalRecoveryAfterSegmentFlush) {
    static const std::string TROOT = ROOT + "/wal_tbl";
    Schema schema = {{"id", TypeId::INT32, false}};
    constexpr u64 T = 4;

    {
        auto res = Table::create(TROOT, "t", schema, T);
        ASSERT_TRUE(res.is_ok());
        auto tbl = std::move(res.value());
        for (i32 i = 0; i < static_cast<i32>(T + 3); ++i)
            ASSERT_TRUE(tbl.insert({Value{i}}).is_ok());
        ASSERT_TRUE(tbl.flush().is_ok());
    }

    {
        auto res = Table::open(TROOT, "t");
        ASSERT_TRUE(res.is_ok());
        auto tbl = std::move(res.value());
        EXPECT_EQ(tbl.segments().size(), 1u);
        EXPECT_EQ(tbl.segments()[0].meta().row_count, T);
        EXPECT_EQ(tbl.row_count(), T + 3);
    }

    static const std::string TROOT2 = ROOT + "/wal_tbl2";

    {
        auto res = Table::create(TROOT2, "t2", schema, T);
        ASSERT_TRUE(res.is_ok());
        auto tbl = std::move(res.value());
        for (i32 i = 0; i < static_cast<i32>(T); ++i)
            ASSERT_TRUE(tbl.insert({Value{i}}).is_ok());
        EXPECT_EQ(tbl.segments().size(), 1u);
        ASSERT_TRUE(tbl.flush().is_ok());
    }

    {
        auto w = WalWriter::open(TROOT2 + "/wal.bin");
        ASSERT_TRUE(w.is_ok());
        WalWriter& wal = w.value();

        std::vector<std::vector<Value>> rows_pre;
        for (i32 i = 0; i < static_cast<i32>(T); ++i)
            rows_pre.push_back({Value{i}});
        ASSERT_TRUE(wal.log_insert("t2", schema, rows_pre).is_ok());
        ASSERT_TRUE(wal.log_segment_flush("t2", T).is_ok());

        std::vector<std::vector<Value>> rows_post;
        for (i32 i = static_cast<i32>(T); i < static_cast<i32>(T) + 2; ++i)
            rows_post.push_back({Value{i}});
        ASSERT_TRUE(wal.log_insert("t2", schema, rows_post).is_ok());
    }

    auto cat_r = Catalog::load(TROOT2);
    ASSERT_TRUE(cat_r.is_ok());
    Catalog cat = std::move(cat_r.value());
    Table* tbl = cat.table("t2");
    ASSERT_NE(tbl, nullptr);

    EXPECT_EQ(tbl->segments().size(), 1u);
    EXPECT_EQ(tbl->row_count(), T + 2);
}

TEST_F(SegmentDbTest, ConcurrentReadWriteNoCrash) {
    static const std::string CDIR = ROOT + "/concurrent";
    auto db_r = Database::open(CDIR);
    ASSERT_TRUE(db_r.is_ok());
    auto db = std::make_unique<Database>(std::move(db_r.value()));

    ASSERT_TRUE(exec(*db, "CREATE TABLE t (id INT, v INT)").is_ok());

    std::atomic<int> errors{0};

    auto writer = [&] {
        for (int i = 0; i < 200; ++i) {
            if (!exec(*db, "INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                               std::to_string(i * 2) + ")")
                     .is_ok())
                ++errors;
        }
    };

    auto reader = [&] {
        for (int i = 0; i < 50; ++i) {
            if (!exec(*db, "SELECT id FROM t").is_ok())
                ++errors;
        }
    };

    std::thread w(writer);
    std::thread r1(reader);
    std::thread r2(reader);
    w.join();
    r1.join();
    r2.join();

    EXPECT_EQ(errors.load(), 0);
    auto count_r = exec(*db, "SELECT id FROM t");
    ASSERT_TRUE(count_r.is_ok());
    EXPECT_EQ(count_r.value().row_count(), 200u);
}

TEST_F(SegmentDbTest, MergeWorkerReducesSegmentCount) {
    static const std::string MROOT = ROOT + "/merge_tbl";
    Schema schema = {{"id", TypeId::INT32, false}};
    constexpr u64 T = 2;
    constexpr u32 MERGE_THRESH = 4;

    {
        auto res = Table::create(MROOT, "t", schema, T);
        ASSERT_TRUE(res.is_ok());
        auto tbl = std::move(res.value());
        for (i32 i = 0; i < static_cast<i32>(MERGE_THRESH * T); ++i)
            ASSERT_TRUE(tbl.insert({Value{i}}).is_ok());
        EXPECT_EQ(tbl.segments().size(), static_cast<usize>(MERGE_THRESH));
        ASSERT_TRUE(tbl.flush().is_ok());
    }

    auto cat_r = Catalog::load(MROOT);
    ASSERT_TRUE(cat_r.is_ok());
    Catalog cat = std::move(cat_r.value());

    MergeWorker worker(&cat, MERGE_THRESH);
    worker.start();
    std::this_thread::sleep_for(std::chrono::seconds(6));
    worker.stop();

    Table* tbl = cat.table("t");
    ASSERT_NE(tbl, nullptr);
    EXPECT_LT(tbl->segments().size(), static_cast<usize>(MERGE_THRESH));
    EXPECT_EQ(tbl->row_count(), MERGE_THRESH * T);
}
