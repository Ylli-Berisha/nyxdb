#include "executor/selection_vector.h"

#include <gtest/gtest.h>
#include <vector>

using namespace nyx;

// --- Builder / finalize / representation selection ---

TEST(SelectionVectorBuilderTest, EmptyProducesDense) {
    SelectionVector::Builder b;
    auto sv = b.finalize(100, false);
    EXPECT_EQ(sv.repr(), SelectionVector::Repr::DENSE);
    EXPECT_EQ(sv.size(), 0u);
    EXPECT_TRUE(sv.empty());
}

TEST(SelectionVectorBuilderTest, DenseWhenScattered) {
    SelectionVector::Builder b;
    // Scattered: every 10th row, low density, high run_count.
    for (u32 i = 0; i < 100; i += 10)
        b.append(i);
    auto sv = b.finalize(1000, false);
    EXPECT_EQ(sv.repr(), SelectionVector::Repr::DENSE);
    EXPECT_EQ(sv.size(), 10u);
}

TEST(SelectionVectorBuilderTest, BitmapWhenHighDensity) {
    SelectionVector::Builder b;
    // 95 out of 100 rows selected (density = 0.95, > 0.9 threshold).
    for (u32 i = 0; i < 100; ++i) {
        if (i % 20 != 0 || i == 0)
            b.append(i);
    }
    // The above skips i=20, 40, 60, 80 → 96 rows. Density = 96/100 = 0.96.
    auto sv = b.finalize(100, false);
    EXPECT_EQ(sv.repr(), SelectionVector::Repr::BITMAP);
    EXPECT_EQ(sv.size(), 96u);
}

TEST(SelectionVectorBuilderTest, RangesWhenLongRuns) {
    SelectionVector::Builder b;
    // Two large contiguous runs → avg_run > 32, low density (avoids bitmap).
    for (u32 i = 0; i < 100; ++i)
        b.append(i);
    for (u32 i = 500; i < 600; ++i)
        b.append(i);
    // 200 rows out of 10000. avg_run = 200 / 2 = 100. density = 0.02.
    auto sv = b.finalize(10000, false);
    EXPECT_EQ(sv.repr(), SelectionVector::Repr::RANGES);
    EXPECT_EQ(sv.size(), 200u);
    ASSERT_EQ(sv.ranges().size(), 2u);
    EXPECT_EQ(sv.ranges()[0], (std::pair<u32, u32>{0, 100}));
    EXPECT_EQ(sv.ranges()[1], (std::pair<u32, u32>{500, 100}));
}

TEST(SelectionVectorBuilderTest, DenseWhenOrderMatters) {
    SelectionVector::Builder b;
    // High density but order_matters=true → must stay DENSE.
    for (u32 i = 0; i < 100; ++i)
        b.append(i);
    auto sv = b.finalize(100, true);
    EXPECT_EQ(sv.repr(), SelectionVector::Repr::DENSE);
}

TEST(SelectionVectorBuilderTest, RunCountTracking) {
    SelectionVector::Builder b;
    b.append(0);
    b.append(1);
    b.append(2); // one run
    b.append(10);
    b.append(11); // second run
    b.append(20); // third run
    EXPECT_EQ(b.run_count(), 3u);
    EXPECT_EQ(b.size(), 6u);
}

// --- Direct construction ---

TEST(SelectionVectorTest, FromDense) {
    auto sv = SelectionVector::from_dense({1, 3, 5, 7}, 10);
    EXPECT_EQ(sv.repr(), SelectionVector::Repr::DENSE);
    EXPECT_EQ(sv.size(), 4u);
    EXPECT_EQ(sv.row_count(), 10u);
    EXPECT_FALSE(sv.empty());
}

TEST(SelectionVectorTest, FromBitmap) {
    // 0b00001010 means positions 1 and 3 are set.
    std::vector<u8> bm = {0b00001010};
    auto sv = SelectionVector::from_bitmap(bm, 8, 2);
    EXPECT_EQ(sv.repr(), SelectionVector::Repr::BITMAP);
    EXPECT_EQ(sv.size(), 2u);
    EXPECT_EQ(sv.row_count(), 8u);
}

TEST(SelectionVectorTest, FromRanges) {
    auto sv = SelectionVector::from_ranges({{0, 5}, {10, 3}}, 20);
    EXPECT_EQ(sv.repr(), SelectionVector::Repr::RANGES);
    EXPECT_EQ(sv.size(), 8u);
    EXPECT_EQ(sv.row_count(), 20u);
}

// --- for_each iteration ---

TEST(SelectionVectorIterationTest, ForEachDense) {
    auto sv = SelectionVector::from_dense({1, 3, 5, 7}, 10);
    std::vector<u32> got;
    sv.for_each([&](u32 i) { got.push_back(i); });
    EXPECT_EQ(got, (std::vector<u32>{1, 3, 5, 7}));
}

TEST(SelectionVectorIterationTest, ForEachBitmap) {
    // Bits at positions 1, 3, 5, 7 (byte 0), and 9 (byte 1).
    // Byte 0: 0b10101010 = 170. Byte 1: 0b00000010 = 2 (bit 1 → position 9).
    std::vector<u8> bm = {0b10101010, 0b00000010};
    auto sv = SelectionVector::from_bitmap(bm, 16, 5);
    std::vector<u32> got;
    sv.for_each([&](u32 i) { got.push_back(i); });
    EXPECT_EQ(got, (std::vector<u32>{1, 3, 5, 7, 9}));
}

TEST(SelectionVectorIterationTest, ForEachRanges) {
    auto sv = SelectionVector::from_ranges({{5, 3}, {20, 2}}, 30);
    std::vector<u32> got;
    sv.for_each([&](u32 i) { got.push_back(i); });
    EXPECT_EQ(got, (std::vector<u32>{5, 6, 7, 20, 21}));
}

// --- Random access at() ---

TEST(SelectionVectorAccessTest, AtDense) {
    auto sv = SelectionVector::from_dense({10, 20, 30}, 100);
    EXPECT_EQ(sv.at(0), 10u);
    EXPECT_EQ(sv.at(1), 20u);
    EXPECT_EQ(sv.at(2), 30u);
}

TEST(SelectionVectorAccessTest, AtRanges) {
    auto sv = SelectionVector::from_ranges({{100, 5}, {200, 3}}, 300);
    EXPECT_EQ(sv.at(0), 100u);
    EXPECT_EQ(sv.at(4), 104u);
    EXPECT_EQ(sv.at(5), 200u);
    EXPECT_EQ(sv.at(7), 202u);
}

// --- truncate ---

TEST(SelectionVectorTruncateTest, DenseTruncate) {
    auto sv = SelectionVector::from_dense({1, 3, 5, 7, 9}, 10);
    sv.truncate(3);
    EXPECT_EQ(sv.size(), 3u);
    std::vector<u32> got;
    sv.for_each([&](u32 i) { got.push_back(i); });
    EXPECT_EQ(got, (std::vector<u32>{1, 3, 5}));
}

TEST(SelectionVectorTruncateTest, BitmapTruncate) {
    // Bits at 1, 3, 5, 7 → 4 live.
    std::vector<u8> bm = {0b10101010};
    auto sv = SelectionVector::from_bitmap(bm, 8, 4);
    sv.truncate(2); // Keep first 2 → positions 1, 3.
    EXPECT_EQ(sv.size(), 2u);
    std::vector<u32> got;
    sv.for_each([&](u32 i) { got.push_back(i); });
    EXPECT_EQ(got, (std::vector<u32>{1, 3}));
}

TEST(SelectionVectorTruncateTest, RangesTruncate) {
    auto sv = SelectionVector::from_ranges({{0, 10}, {50, 10}}, 100);
    sv.truncate(15); // Keep first 15: full first range (10) + 5 of second.
    EXPECT_EQ(sv.size(), 15u);
    ASSERT_EQ(sv.ranges().size(), 2u);
    EXPECT_EQ(sv.ranges()[0], (std::pair<u32, u32>{0, 10}));
    EXPECT_EQ(sv.ranges()[1], (std::pair<u32, u32>{50, 5}));
}

TEST(SelectionVectorTruncateTest, TruncateNoOpWhenLargerThanSize) {
    auto sv = SelectionVector::from_dense({1, 2, 3}, 10);
    sv.truncate(10);
    EXPECT_EQ(sv.size(), 3u);
}

// --- drop_prefix ---

TEST(SelectionVectorDropPrefixTest, DenseDrop) {
    auto sv = SelectionVector::from_dense({1, 3, 5, 7, 9}, 10);
    sv.drop_prefix(2);
    EXPECT_EQ(sv.size(), 3u);
    std::vector<u32> got;
    sv.for_each([&](u32 i) { got.push_back(i); });
    EXPECT_EQ(got, (std::vector<u32>{5, 7, 9}));
}

TEST(SelectionVectorDropPrefixTest, RangesDropWithinRange) {
    auto sv = SelectionVector::from_ranges({{0, 10}, {50, 10}}, 100);
    sv.drop_prefix(5); // Drop 5 of first range.
    EXPECT_EQ(sv.size(), 15u);
    ASSERT_EQ(sv.ranges().size(), 2u);
    EXPECT_EQ(sv.ranges()[0], (std::pair<u32, u32>{5, 5}));
    EXPECT_EQ(sv.ranges()[1], (std::pair<u32, u32>{50, 10}));
}

TEST(SelectionVectorDropPrefixTest, RangesDropWholeFirstRange) {
    auto sv = SelectionVector::from_ranges({{0, 10}, {50, 10}}, 100);
    sv.drop_prefix(10);
    EXPECT_EQ(sv.size(), 10u);
    ASSERT_EQ(sv.ranges().size(), 1u);
    EXPECT_EQ(sv.ranges()[0], (std::pair<u32, u32>{50, 10}));
}

TEST(SelectionVectorDropPrefixTest, RangesDropStraddling) {
    auto sv = SelectionVector::from_ranges({{0, 10}, {50, 10}}, 100);
    sv.drop_prefix(13); // Drop all of first range + 3 of second.
    EXPECT_EQ(sv.size(), 7u);
    ASSERT_EQ(sv.ranges().size(), 1u);
    EXPECT_EQ(sv.ranges()[0], (std::pair<u32, u32>{53, 7}));
}

TEST(SelectionVectorDropPrefixTest, BitmapDropFallsBackToDense) {
    std::vector<u8> bm = {0b10101010};
    auto sv = SelectionVector::from_bitmap(bm, 8, 4);
    sv.drop_prefix(2); // Drops positions 1, 3; keeps 5, 7.
    EXPECT_EQ(sv.repr(), SelectionVector::Repr::DENSE);
    EXPECT_EQ(sv.size(), 2u);
    std::vector<u32> got;
    sv.for_each([&](u32 i) { got.push_back(i); });
    EXPECT_EQ(got, (std::vector<u32>{5, 7}));
}

TEST(SelectionVectorDropPrefixTest, DropZeroIsNoOp) {
    auto sv = SelectionVector::from_dense({1, 2, 3}, 10);
    sv.drop_prefix(0);
    EXPECT_EQ(sv.size(), 3u);
}

// --- to_dense ---

TEST(SelectionVectorToDenseTest, FromBitmap) {
    std::vector<u8> bm = {0b10101010};
    auto sv = SelectionVector::from_bitmap(bm, 8, 4);
    auto dense = sv.to_dense();
    EXPECT_EQ(dense.repr(), SelectionVector::Repr::DENSE);
    EXPECT_EQ(dense.size(), 4u);
    EXPECT_EQ(dense.dense_indices(), (std::vector<u32>{1, 3, 5, 7}));
}

TEST(SelectionVectorToDenseTest, FromRanges) {
    auto sv = SelectionVector::from_ranges({{5, 3}, {10, 2}}, 20);
    auto dense = sv.to_dense();
    EXPECT_EQ(dense.repr(), SelectionVector::Repr::DENSE);
    EXPECT_EQ(dense.dense_indices(), (std::vector<u32>{5, 6, 7, 10, 11}));
}

TEST(SelectionVectorToDenseTest, FromDenseIsCopy) {
    auto sv = SelectionVector::from_dense({1, 2, 3}, 10);
    auto dense = sv.to_dense();
    EXPECT_EQ(dense.repr(), SelectionVector::Repr::DENSE);
    EXPECT_EQ(dense.dense_indices(), (std::vector<u32>{1, 2, 3}));
}
