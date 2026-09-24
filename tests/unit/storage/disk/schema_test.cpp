#include "storage/disk/schema.h"

#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_schema_test";

class SchemaTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::create_directories(ROOT); }
    void TearDown() override { fs::remove_all(ROOT); }
    std::string path(const std::string& name) { return ROOT + "/" + name; }
};

TEST_F(SchemaTest, RoundTripAllTypes) {
    Schema s = {
        {"id", TypeId::INT32, false, 0, std::nullopt},
        {"big", TypeId::INT64, false, 0, std::nullopt},
        {"score", TypeId::DOUBLE, false, 0, std::nullopt},
        {"name", TypeId::VARCHAR, true, 64, std::nullopt},
        {"flag", TypeId::BOOL, false, 0, std::nullopt},
        {"dt", TypeId::DATE, true, 0, std::nullopt},
        {"ts", TypeId::TIMESTAMP, false, 0, std::nullopt},
    };
    auto p = path("all_types.bin");
    ASSERT_TRUE(SchemaFile::write(p, s).is_ok());

    auto r = SchemaFile::read(p);
    ASSERT_TRUE(r.is_ok());
    const Schema& out = r.value();
    ASSERT_EQ(out.size(), s.size());

    EXPECT_EQ(out[0].name, "id");
    EXPECT_EQ(out[0].type, TypeId::INT32);
    EXPECT_FALSE(out[0].nullable);
    EXPECT_EQ(out[1].name, "big");
    EXPECT_EQ(out[1].type, TypeId::INT64);
    EXPECT_EQ(out[2].name, "score");
    EXPECT_EQ(out[2].type, TypeId::DOUBLE);
    EXPECT_EQ(out[3].name, "name");
    EXPECT_EQ(out[3].type, TypeId::VARCHAR);
    EXPECT_TRUE(out[3].nullable);
    EXPECT_EQ(out[3].max_len, 64u);
    EXPECT_EQ(out[4].name, "flag");
    EXPECT_EQ(out[4].type, TypeId::BOOL);
    EXPECT_EQ(out[5].name, "dt");
    EXPECT_EQ(out[5].type, TypeId::DATE);
    EXPECT_EQ(out[6].name, "ts");
    EXPECT_EQ(out[6].type, TypeId::TIMESTAMP);
}

TEST_F(SchemaTest, DefaultValuesAllTypes) {
    Schema s = {
        {"a", TypeId::INT32, false, 0, Value{i32(42)}},
        {"b", TypeId::INT64, false, 0, Value{i64(99)}},
        {"c", TypeId::DOUBLE, false, 0, Value{f64(3.14)}},
        {"d", TypeId::VARCHAR, false, 32, Value{std::string("hello")}},
        {"e", TypeId::BOOL, false, 0, Value{true}},
        {"f", TypeId::DATE, false, 0, Value{Date{19723}}},
        {"g", TypeId::TIMESTAMP, false, 0, Value{Timestamp{1234567890LL}}},
    };
    auto p = path("defaults.bin");
    ASSERT_TRUE(SchemaFile::write(p, s).is_ok());

    auto r = SchemaFile::read(p);
    ASSERT_TRUE(r.is_ok());
    const Schema& out = r.value();
    ASSERT_EQ(out.size(), 7u);

    ASSERT_TRUE(out[0].default_value.has_value());
    EXPECT_EQ(std::get<i32>(*out[0].default_value), 42);
    ASSERT_TRUE(out[1].default_value.has_value());
    EXPECT_EQ(std::get<i64>(*out[1].default_value), 99);
    ASSERT_TRUE(out[2].default_value.has_value());
    EXPECT_DOUBLE_EQ(std::get<f64>(*out[2].default_value), 3.14);
    ASSERT_TRUE(out[3].default_value.has_value());
    EXPECT_EQ(std::get<std::string>(*out[3].default_value), "hello");
    ASSERT_TRUE(out[4].default_value.has_value());
    EXPECT_EQ(std::get<bool>(*out[4].default_value), true);
    ASSERT_TRUE(out[5].default_value.has_value());
    EXPECT_EQ(std::get<Date>(*out[5].default_value).days, 19723);
    ASSERT_TRUE(out[6].default_value.has_value());
    EXPECT_EQ(std::get<Timestamp>(*out[6].default_value).micros, 1234567890LL);
}

TEST_F(SchemaTest, EmptySchema) {
    Schema s;
    auto p = path("empty.bin");
    ASSERT_TRUE(SchemaFile::write(p, s).is_ok());
    auto r = SchemaFile::read(p);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(SchemaTest, BadMagicReturnsError) {
    auto p = path("bad_magic.bin");
    // write garbage
    {
        int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        u8 junk[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0};
        ::write(fd, junk, 8);
        ::close(fd);
    }
    auto r = SchemaFile::read(p);
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message.find("bad magic"), std::string::npos);
}

TEST_F(SchemaTest, MissingFileReturnsError) {
    auto r = SchemaFile::read(ROOT + "/nonexistent.bin");
    EXPECT_FALSE(r.is_ok());
}
