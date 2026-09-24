#include "storage/disk/index_file.h"
#include "storage/disk/index_page.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_index_file_test";

class IndexFileTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        fs::create_directories(ROOT);
    }
    void TearDown() override { fs::remove_all(ROOT); }
    std::string path(const std::string& n) { return ROOT + "/" + n; }

    static std::vector<IndexColSpec> int32_col() { return {{TypeId::INT32, 0}}; }
};

TEST_F(IndexFileTest, CreateAndMetadata) {
    auto r = IndexFile::create(path("idx.bin"), int32_col(), {0}, false);
    ASSERT_TRUE(r.is_ok()) << r.error().message;

    IndexFile& f = r.value();
    EXPECT_EQ(f.col_specs().size(), 1u);
    EXPECT_EQ(f.col_specs()[0].type, TypeId::INT32);
    EXPECT_EQ(f.col_indices()[0], 0u);
    EXPECT_FALSE(f.is_unique());
    EXPECT_EQ(f.entry_count(), 0u);
    EXPECT_FALSE(f.is_dirty());
    EXPECT_GT(f.key_size(), 0u);
    EXPECT_GT(f.node_capacity(), 0u);
}

TEST_F(IndexFileTest, CreateUniqueFlag) {
    auto r = IndexFile::create(path("uniq.bin"), int32_col(), {0}, true);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().is_unique());
}

TEST_F(IndexFileTest, OpenRoundTrip) {
    auto p = path("rt.bin");
    {
        auto r = IndexFile::create(p, int32_col(), {0}, true);
        ASSERT_TRUE(r.is_ok());
        ASSERT_TRUE(r.value().flush().is_ok());
    }
    auto r = IndexFile::open(p);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_TRUE(r.value().is_unique());
    EXPECT_EQ(r.value().col_specs()[0].type, TypeId::INT32);
}

TEST_F(IndexFileTest, AllocateLeafNode) {
    auto r = IndexFile::create(path("alloc.bin"), int32_col(), {0}, false);
    ASSERT_TRUE(r.is_ok());
    IndexFile& f = r.value();

    auto pid = f.allocate_node(IDX_PAGE_LEAF);
    ASSERT_TRUE(pid.is_ok());
    EXPECT_GT(pid.value(), 0u);

    auto ph = f.fetch_page(pid.value());
    ASSERT_TRUE(ph.is_ok());
    const auto* hdr = reinterpret_cast<const IndexPageHeader*>(ph.value()->payload());
    EXPECT_EQ(static_cast<IndexPageType>(hdr->page_type), IDX_PAGE_LEAF);
    EXPECT_EQ(hdr->entry_count, 0u);
}

TEST_F(IndexFileTest, AllocateInternalNode) {
    auto r = IndexFile::create(path("internal.bin"), int32_col(), {0}, false);
    ASSERT_TRUE(r.is_ok());
    auto pid = r.value().allocate_node(IDX_PAGE_INTERNAL);
    ASSERT_TRUE(pid.is_ok());

    auto ph = r.value().fetch_page(pid.value());
    ASSERT_TRUE(ph.is_ok());
    const auto* hdr = reinterpret_cast<const IndexPageHeader*>(ph.value()->payload());
    EXPECT_EQ(static_cast<IndexPageType>(hdr->page_type), IDX_PAGE_INTERNAL);
}

TEST_F(IndexFileTest, SetDirtyPersists) {
    auto p = path("dirty.bin");
    {
        auto r = IndexFile::create(p, int32_col(), {0}, false);
        ASSERT_TRUE(r.is_ok());
        ASSERT_TRUE(r.value().set_dirty(true).is_ok());
    }
    auto r = IndexFile::open(p);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().is_dirty());
}

TEST_F(IndexFileTest, ResetTreeClearsEntries) {
    auto p = path("reset.bin");
    auto r = IndexFile::create(p, int32_col(), {0}, false);
    ASSERT_TRUE(r.is_ok());
    IndexFile& f = r.value();

    ASSERT_TRUE(f.allocate_node(IDX_PAGE_LEAF).is_ok());
    ASSERT_TRUE(f.reset_tree().is_ok());
    EXPECT_EQ(f.entry_count(), 0u);
    EXPECT_FALSE(f.is_dirty());
}

TEST_F(IndexFileTest, OpenNonExistentReturnsError) {
    auto r = IndexFile::open(ROOT + "/missing.bin");
    EXPECT_FALSE(r.is_ok());
}

TEST_F(IndexFileTest, CreateOnExistingFileReturnsError) {
    auto p = path("exists.bin");
    ASSERT_TRUE(IndexFile::create(p, int32_col(), {0}, false).is_ok());
    auto r = IndexFile::create(p, int32_col(), {0}, false);
    EXPECT_FALSE(r.is_ok());
}
