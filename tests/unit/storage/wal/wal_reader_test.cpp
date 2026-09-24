#include "storage/wal/wal_reader.h"
#include "storage/wal/wal_writer.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_wal_reader_test";

class WalReaderTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        fs::create_directories(ROOT);
    }
    void TearDown() override { fs::remove_all(ROOT); }
    std::string wal_path() { return ROOT + "/test.wal"; }

    static Schema int_schema() { return {{"id", TypeId::INT32, false, 0, std::nullopt}}; }
    static Schema two_col_schema() {
        return {
            {"id", TypeId::INT32, false, 0, std::nullopt},
            {"val", TypeId::VARCHAR, true, 32, std::nullopt},
        };
    }
};

TEST_F(WalReaderTest, ReadInsert) {
    auto wp = wal_path();
    {
        auto w = WalWriter::open(wp);
        ASSERT_TRUE(w.is_ok());
        auto rows = std::vector<std::vector<Value>>{{Value{i32(1)}}};
        ASSERT_TRUE(w.value().log_insert("t", int_schema(), rows).is_ok());
    }
    auto r = WalReader::open(wp);
    ASSERT_TRUE(r.is_ok());
    auto recs = r.value().read_all();
    ASSERT_TRUE(recs.is_ok());
    ASSERT_EQ(recs.value().size(), 1u);
    EXPECT_EQ(recs.value()[0].type, WalRecord::Type::Insert);
    EXPECT_EQ(recs.value()[0].table_name, "t");
    ASSERT_EQ(recs.value()[0].rows.size(), 1u);
    EXPECT_EQ(std::get<i32>(recs.value()[0].rows[0][0]), 1);
}

TEST_F(WalReaderTest, ReadCreateTable) {
    auto wp = wal_path();
    {
        auto w = WalWriter::open(wp);
        ASSERT_TRUE(w.is_ok());
        ASSERT_TRUE(w.value().log_create_table("orders", two_col_schema()).is_ok());
    }
    auto r = WalReader::open(wp);
    ASSERT_TRUE(r.is_ok());
    auto recs = r.value().read_all();
    ASSERT_TRUE(recs.is_ok());
    ASSERT_EQ(recs.value().size(), 1u);
    EXPECT_EQ(recs.value()[0].type, WalRecord::Type::CreateTable);
    EXPECT_EQ(recs.value()[0].table_name, "orders");
    ASSERT_EQ(recs.value()[0].schema.size(), 2u);
    EXPECT_EQ(recs.value()[0].schema[0].name, "id");
    EXPECT_EQ(recs.value()[0].schema[1].name, "val");
}

TEST_F(WalReaderTest, ReadDelete) {
    auto wp = wal_path();
    {
        auto w = WalWriter::open(wp);
        ASSERT_TRUE(w.is_ok());
        ASSERT_TRUE(w.value().log_delete("t", {0, 5, 10}).is_ok());
    }
    auto recs = WalReader::open(wp).value().read_all();
    ASSERT_TRUE(recs.is_ok());
    ASSERT_EQ(recs.value().size(), 1u);
    EXPECT_EQ(recs.value()[0].type, WalRecord::Type::Delete);
    EXPECT_EQ(recs.value()[0].row_indices, (std::vector<u64>{0, 5, 10}));
}

TEST_F(WalReaderTest, ReadUpdate) {
    auto wp = wal_path();
    {
        auto w = WalWriter::open(wp);
        ASSERT_TRUE(w.is_ok());
        std::vector<std::vector<Value>> new_rows = {{Value{i32(99)}, Value{std::string("new")}}};
        ASSERT_TRUE(w.value().log_update("t", {3}, two_col_schema(), new_rows).is_ok());
    }
    auto recs = WalReader::open(wp).value().read_all();
    ASSERT_TRUE(recs.is_ok());
    ASSERT_EQ(recs.value().size(), 1u);
    const auto& rec = recs.value()[0];
    EXPECT_EQ(rec.type, WalRecord::Type::Update);
    EXPECT_EQ(rec.row_indices, std::vector<u64>{3});
    ASSERT_EQ(rec.rows.size(), 1u);
    EXPECT_EQ(std::get<i32>(rec.rows[0][0]), 99);
    EXPECT_EQ(std::get<std::string>(rec.rows[0][1]), "new");
}

TEST_F(WalReaderTest, ReadSegmentFlush) {
    auto wp = wal_path();
    {
        auto w = WalWriter::open(wp);
        ASSERT_TRUE(w.is_ok());
        ASSERT_TRUE(w.value().log_segment_flush("t", 1024).is_ok());
    }
    auto recs = WalReader::open(wp).value().read_all();
    ASSERT_TRUE(recs.is_ok());
    ASSERT_EQ(recs.value().size(), 1u);
    EXPECT_EQ(recs.value()[0].type, WalRecord::Type::SegmentFlush);
    EXPECT_EQ(recs.value()[0].sealed_row_count, 1024u);
}

TEST_F(WalReaderTest, MultipleRecordsInOrder) {
    auto wp = wal_path();
    {
        auto w = WalWriter::open(wp);
        ASSERT_TRUE(w.is_ok());
        ASSERT_TRUE(w.value().log_create_table("t", int_schema()).is_ok());
        auto rows = std::vector<std::vector<Value>>{{Value{i32(1)}}, {Value{i32(2)}}};
        ASSERT_TRUE(w.value().log_insert("t", int_schema(), rows).is_ok());
        ASSERT_TRUE(w.value().log_delete("t", {0}).is_ok());
    }
    auto recs = WalReader::open(wp).value().read_all();
    ASSERT_TRUE(recs.is_ok());
    ASSERT_EQ(recs.value().size(), 3u);
    EXPECT_EQ(recs.value()[0].type, WalRecord::Type::CreateTable);
    EXPECT_EQ(recs.value()[1].type, WalRecord::Type::Insert);
    EXPECT_EQ(recs.value()[2].type, WalRecord::Type::Delete);
}

TEST_F(WalReaderTest, BytesConsumedTracked) {
    auto wp = wal_path();
    {
        auto w = WalWriter::open(wp);
        ASSERT_TRUE(w.is_ok());
        auto rows = std::vector<std::vector<Value>>{{Value{i32(42)}}};
        ASSERT_TRUE(w.value().log_insert("t", int_schema(), rows).is_ok());
    }
    u64 consumed = 0;
    auto recs = WalReader::open(wp).value().read_all(&consumed);
    ASSERT_TRUE(recs.is_ok());
    EXPECT_GT(consumed, 0u);
}

TEST_F(WalReaderTest, MissingFileReturnsError) {
    EXPECT_FALSE(WalReader::open(ROOT + "/nope.wal").is_ok());
}
