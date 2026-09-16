#include "storage/wal/wal_reader.h"
#include "storage/wal/wal_writer.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string WAL_PATH = "/tmp/nyxdb_wal_writer_test.bin";

class WalWriterTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove(WAL_PATH); }
    void TearDown() override { fs::remove(WAL_PATH); }
};

static WalWriter open_writer() {
    auto r = WalWriter::open(WAL_PATH);
    return std::move(r.value());
}

static WalReader open_reader() {
    auto r = WalReader::open(WAL_PATH);
    return std::move(r.value());
}

TEST_F(WalWriterTest, OpenCreatesFile) {
    auto r = WalWriter::open(WAL_PATH);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(fs::exists(WAL_PATH));
    EXPECT_GT(fs::file_size(WAL_PATH), 0u);
}

TEST_F(WalWriterTest, LogInsertRoundTrip) {
    Schema schema = {{"id", TypeId::INT32, false, 0}, {"val", TypeId::DOUBLE, false, 0}};
    std::vector<std::vector<Value>> rows = {{{i32{1}}, {f64{1.5}}}, {{i32{2}}, {f64{2.5}}}};

    {
        auto w = open_writer();
        ASSERT_TRUE(w.log_insert("t", schema, rows).is_ok());
    }

    auto rr = open_reader();
    auto rec_r = rr.read_all();
    ASSERT_TRUE(rec_r.is_ok());
    auto& records = rec_r.value();
    ASSERT_EQ(records.size(), 1u);

    EXPECT_EQ(records[0].type, WalRecord::Type::Insert);
    EXPECT_EQ(records[0].table_name, "t");
    ASSERT_EQ(records[0].rows.size(), 2u);
    EXPECT_EQ(std::get<i32>(records[0].rows[0][0]), 1);
    EXPECT_DOUBLE_EQ(std::get<f64>(records[0].rows[0][1]), 1.5);
    EXPECT_EQ(std::get<i32>(records[0].rows[1][0]), 2);
    EXPECT_DOUBLE_EQ(std::get<f64>(records[0].rows[1][1]), 2.5);
}

TEST_F(WalWriterTest, LogInsertVarchar) {
    Schema schema = {{"name", TypeId::VARCHAR, false, 30}};
    std::vector<std::vector<Value>> rows = {{{"alice"}}, {{"bob"}}};

    {
        auto w = open_writer();
        ASSERT_TRUE(w.log_insert("t", schema, rows).is_ok());
    }

    auto rr = open_reader();
    auto rec_r = rr.read_all();
    ASSERT_TRUE(rec_r.is_ok());
    auto& records = rec_r.value();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(std::get<std::string>(records[0].rows[0][0]), "alice");
    EXPECT_EQ(std::get<std::string>(records[0].rows[1][0]), "bob");
}

TEST_F(WalWriterTest, LogInsertWithNulls) {
    Schema schema = {{"id", TypeId::INT32, true, 0}};
    std::vector<std::vector<Value>> rows = {{{i32{1}}}, {{std::monostate{}}}};

    {
        auto w = open_writer();
        ASSERT_TRUE(w.log_insert("t", schema, rows).is_ok());
    }

    auto rr = open_reader();
    auto rec_r = rr.read_all();
    ASSERT_TRUE(rec_r.is_ok());
    auto& records = rec_r.value();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_FALSE(is_null(records[0].rows[0][0]));
    EXPECT_TRUE(is_null(records[0].rows[1][0]));
}

TEST_F(WalWriterTest, LogCreateTable) {
    Schema schema = {{"id", TypeId::INT32, false, 0}, {"name", TypeId::VARCHAR, false, 50}};

    {
        auto w = open_writer();
        ASSERT_TRUE(w.log_create_table("mytable", schema).is_ok());
    }

    auto rr = open_reader();
    auto rec_r = rr.read_all();
    ASSERT_TRUE(rec_r.is_ok());
    auto& records = rec_r.value();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].type, WalRecord::Type::CreateTable);
    EXPECT_EQ(records[0].table_name, "mytable");
    ASSERT_EQ(records[0].schema.size(), 2u);
    EXPECT_EQ(records[0].schema[0].name, "id");
    EXPECT_EQ(records[0].schema[1].name, "name");
    EXPECT_EQ(records[0].schema[1].max_len, 50u);
}

TEST_F(WalWriterTest, MultipleRecords) {
    Schema schema = {{"x", TypeId::INT64, false, 0}};

    {
        auto w = open_writer();
        ASSERT_TRUE(w.log_create_table("t", schema).is_ok());
        ASSERT_TRUE(w.log_insert("t", schema, {{{i64{10}}}}).is_ok());
        ASSERT_TRUE(w.log_insert("t", schema, {{{i64{20}}}}).is_ok());
    }

    auto rr = open_reader();
    auto rec_r = rr.read_all();
    ASSERT_TRUE(rec_r.is_ok());
    auto& records = rec_r.value();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].type, WalRecord::Type::CreateTable);
    EXPECT_EQ(std::get<i64>(records[1].rows[0][0]), 10);
    EXPECT_EQ(std::get<i64>(records[2].rows[0][0]), 20);
}

TEST_F(WalWriterTest, TornWriteStopsReplay) {
    Schema schema = {{"x", TypeId::INT32, false, 0}};
    {
        auto w = open_writer();
        ASSERT_TRUE(w.log_insert("t", schema, {{{i32{1}}}}).is_ok());
        ASSERT_TRUE(w.log_insert("t", schema, {{{i32{2}}}}).is_ok());
    }

    auto size = fs::file_size(WAL_PATH);
    fs::resize_file(WAL_PATH, size - 4);

    auto rr = open_reader();
    auto rec_r = rr.read_all();
    ASSERT_TRUE(rec_r.is_ok());
    auto& records = rec_r.value();
    EXPECT_EQ(records.size(), 1u);
    EXPECT_EQ(std::get<i32>(records[0].rows[0][0]), 1);
}

TEST_F(WalWriterTest, CheckpointDeletesFile) {
    auto w = open_writer();
    Schema schema = {{"x", TypeId::INT32, false, 0}};
    ASSERT_TRUE(w.log_insert("t", schema, {{{i32{1}}}}).is_ok());
    ASSERT_TRUE(w.checkpoint().is_ok());
    EXPECT_FALSE(fs::exists(WAL_PATH));
}

TEST_F(WalWriterTest, OffsetTracking) {
    auto w = open_writer();
    u64 after_header = w.current_offset();
    EXPECT_GT(after_header, 0u);

    Schema schema = {{"x", TypeId::INT32, false, 0}};
    ASSERT_TRUE(w.log_insert("t", schema, {{{i32{1}}}}).is_ok());
    EXPECT_GT(w.current_offset(), after_header);
}
