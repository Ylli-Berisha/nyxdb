#include "storage/disk/constraint_file.h"

#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_constraint_file_test";

class ConstraintFileTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::create_directories(ROOT); }
    void TearDown() override { fs::remove_all(ROOT); }
    std::string path(const std::string& n) { return ROOT + "/" + n; }
};

TEST_F(ConstraintFileTest, RoundTripSingle) {
    std::vector<ConstraintMeta> cs = {
        {ConstraintKind::UNIQUE, "uq_id", {0}},
    };
    auto p = path("single.bin");
    ASSERT_TRUE(ConstraintFile::write(p, cs).is_ok());

    auto r = ConstraintFile::read(p);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].kind, ConstraintKind::UNIQUE);
    EXPECT_EQ(r.value()[0].name, "uq_id");
    ASSERT_EQ(r.value()[0].col_indices.size(), 1u);
    EXPECT_EQ(r.value()[0].col_indices[0], 0u);
}

TEST_F(ConstraintFileTest, RoundTripMultiple) {
    std::vector<ConstraintMeta> cs = {
        {ConstraintKind::UNIQUE, "uq_a", {0}},
        {ConstraintKind::UNIQUE, "uq_bc", {1, 2}},
        {ConstraintKind::PRIMARY_KEY, "pk", {0}},
    };
    auto p = path("multi.bin");
    ASSERT_TRUE(ConstraintFile::write(p, cs).is_ok());

    auto r = ConstraintFile::read(p);
    ASSERT_TRUE(r.is_ok());
    const auto& out = r.value();
    ASSERT_EQ(out.size(), 3u);

    EXPECT_EQ(out[0].kind, ConstraintKind::UNIQUE);
    EXPECT_EQ(out[0].name, "uq_a");
    EXPECT_EQ(out[0].col_indices, std::vector<u8>{0});

    EXPECT_EQ(out[1].kind, ConstraintKind::UNIQUE);
    EXPECT_EQ(out[1].name, "uq_bc");
    EXPECT_EQ(out[1].col_indices, (std::vector<u8>{1, 2}));

    EXPECT_EQ(out[2].kind, ConstraintKind::PRIMARY_KEY);
    EXPECT_EQ(out[2].name, "pk");
}

TEST_F(ConstraintFileTest, EmptyConstraints) {
    auto p = path("empty.bin");
    ASSERT_TRUE(ConstraintFile::write(p, {}).is_ok());
    auto r = ConstraintFile::read(p);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(ConstraintFileTest, BadMagicReturnsError) {
    auto p = path("bad.bin");
    {
        int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        u8 junk[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
        ::write(fd, junk, 8);
        ::close(fd);
    }
    auto r = ConstraintFile::read(p);
    EXPECT_FALSE(r.is_ok());
}

TEST_F(ConstraintFileTest, MissingFileReturnsError) {
    EXPECT_FALSE(ConstraintFile::read(ROOT + "/nope.bin").is_ok());
}
