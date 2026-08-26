#include <gtest/gtest.h>
#include "storage/disk_manager.h"

#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_FILE = "/tmp/nyxdb_dm_test.col";

class DiskManagerTest : public ::testing::Test {
protected:
    void SetUp()    override { fs::remove(TEST_FILE); }
    void TearDown() override { fs::remove(TEST_FILE); }
};

TEST_F(DiskManagerTest, AllocateAndWriteRead) {
    DiskManager dm(TEST_FILE);

    auto id_res = dm.allocate_page();
    ASSERT_TRUE(id_res.is_ok());
    PageId id = id_res.value();
    EXPECT_EQ(id, 0u);

    Page page{};
    page.reset(id);
    std::memset(page.payload(), 0xAB, PAGE_PAYLOAD_SIZE);

    ASSERT_TRUE(dm.write_page(page).is_ok());

    Page read_back{};
    ASSERT_TRUE(dm.read_page(id, read_back).is_ok());
    EXPECT_EQ(std::memcmp(read_back.payload(), page.payload(), PAGE_PAYLOAD_SIZE), 0);
}

TEST_F(DiskManagerTest, PageCountGrowsOnAllocate) {
    DiskManager dm(TEST_FILE);
    EXPECT_EQ(dm.page_count(), 0u);
    dm.allocate_page();
    EXPECT_EQ(dm.page_count(), 1u);
    dm.allocate_page();
    EXPECT_EQ(dm.page_count(), 2u);
}

TEST_F(DiskManagerTest, ChecksumMismatchDetected) {
    DiskManager dm(TEST_FILE);
    auto id_res = dm.allocate_page();
    ASSERT_TRUE(id_res.is_ok());

    Page blank{};
    blank.reset(id_res.value());
    dm.write_page(blank);

    int fd = open(TEST_FILE.c_str(), O_RDWR);
    byte corrupt = 0xFF;
    pwrite(fd, &corrupt, 1, PAGE_HEADER_SIZE);
    close(fd);

    Page out{};
    auto res = dm.read_page(id_res.value(), out);
    EXPECT_TRUE(res.is_err());
    EXPECT_NE(res.error().message.find("checksum"), std::string::npos);
}

TEST_F(DiskManagerTest, ReadOutOfRangeFails) {
    DiskManager dm(TEST_FILE);
    Page out{};
    auto res = dm.read_page(99, out);
    EXPECT_TRUE(res.is_err());
}

TEST_F(DiskManagerTest, PersistsAcrossReopens) {
    PageId id;
    {
        DiskManager dm(TEST_FILE);
        auto res = dm.allocate_page();
        ASSERT_TRUE(res.is_ok());
        id = res.value();

        Page page{};
        page.reset(id);
        std::memset(page.payload(), 0x42, PAGE_PAYLOAD_SIZE);
        ASSERT_TRUE(dm.write_page(page).is_ok());
    }

    DiskManager dm2(TEST_FILE);
    EXPECT_EQ(dm2.page_count(), 1u);

    Page out{};
    ASSERT_TRUE(dm2.read_page(id, out).is_ok());
    EXPECT_EQ(out.payload()[0], 0x42);
}
