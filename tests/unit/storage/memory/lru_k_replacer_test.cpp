#include "storage/memory/lru_k_replacer.h"

#include <gtest/gtest.h>

using namespace nyx;

TEST(LRUKReplacerTest, SizeStartsAtZero) {
    LRUKReplacer r(4, 2);
    EXPECT_EQ(r.size(), 0u);
}

TEST(LRUKReplacerTest, VictimFailsWhenNothingEvictable) {
    LRUKReplacer r(4, 2);
    FrameId out;
    EXPECT_FALSE(r.victim(out));
}

TEST(LRUKReplacerTest, RecordAccessThenUnpinMakesEvictable) {
    LRUKReplacer r(4, 2);
    r.record_access(0);
    EXPECT_EQ(r.size(), 0u);
    r.unpin(0);
    EXPECT_EQ(r.size(), 1u);
}

TEST(LRUKReplacerTest, PinRemovesFromEvictable) {
    LRUKReplacer r(4, 2);
    r.record_access(0);
    r.unpin(0);
    EXPECT_EQ(r.size(), 1u);
    r.pin(0);
    EXPECT_EQ(r.size(), 0u);
}

TEST(LRUKReplacerTest, LessThanKPreferredForEviction) {
    // Frame 0: 2 accesses (K reached). Frame 1: 1 access (< K).
    // Frame 1 should be evicted first.
    LRUKReplacer r(4, 2);
    r.record_access(0);
    r.record_access(0);
    r.record_access(1);
    r.unpin(0);
    r.unpin(1);

    FrameId out;
    ASSERT_TRUE(r.victim(out));
    EXPECT_EQ(out, 1u);
}

TEST(LRUKReplacerTest, KGroupEvictsLargestBackwardKDistance) {
    // Frame 0: accesses at t={1,4}. K-th recent = 1 (oldest).
    // Frame 1: accesses at t={2,3}. K-th recent = 2.
    // Frame 0 has larger backward K-distance -> evict frame 0.
    LRUKReplacer r(4, 2);
    r.record_access(0); // t=1
    r.record_access(1); // t=2
    r.record_access(1); // t=3
    r.record_access(0); // t=4
    r.unpin(0);
    r.unpin(1);

    FrameId out;
    ASSERT_TRUE(r.victim(out));
    EXPECT_EQ(out, 0u);
}

TEST(LRUKReplacerTest, VictimClearsFrame) {
    LRUKReplacer r(4, 2);
    r.record_access(0);
    r.record_access(1);
    r.unpin(0);
    r.unpin(1);

    FrameId out;
    ASSERT_TRUE(r.victim(out));
    // Evicted frame's history cleared -> size drops by 1
    EXPECT_EQ(r.size(), 1u);
    // Re-accessing the evicted frame starts a new history
    r.record_access(out);
    r.unpin(out);
    EXPECT_EQ(r.size(), 2u);
}

TEST(LRUKReplacerTest, PinIsIdempotent) {
    LRUKReplacer r(4, 2);
    r.record_access(0);
    r.unpin(0);
    EXPECT_EQ(r.size(), 1u);
    r.pin(0);
    r.pin(0); // double pin is fine
    EXPECT_EQ(r.size(), 0u);
}

TEST(LRUKReplacerTest, UnpinIsIdempotent) {
    LRUKReplacer r(4, 2);
    r.record_access(0);
    r.unpin(0);
    r.unpin(0); // second unpin is fine
    EXPECT_EQ(r.size(), 1u);
}

TEST(LRUKReplacerTest, RemoveUntrackedFrameIsNoOp) {
    LRUKReplacer r(4, 2);
    r.remove(2); // never accessed
    EXPECT_EQ(r.size(), 0u);
}

TEST(LRUKReplacerTest, RemoveEvictableFrameDropsSize) {
    LRUKReplacer r(4, 2);
    r.record_access(0);
    r.record_access(1);
    r.unpin(0);
    r.unpin(1);
    EXPECT_EQ(r.size(), 2u);
    r.remove(0);
    EXPECT_EQ(r.size(), 1u);
}

TEST(LRUKReplacerTest, LessThanKGroupEvictsEarliestFirstAccess) {
    // Frame 1 accessed first, frame 0 accessed second. Both < K.
    // Frame 1's front (first access) is older -> evict frame 1.
    LRUKReplacer r(4, 2);
    r.record_access(1);
    r.record_access(0);
    r.unpin(0);
    r.unpin(1);

    FrameId out;
    ASSERT_TRUE(r.victim(out));
    EXPECT_EQ(out, 1u);
}
