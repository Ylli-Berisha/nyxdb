#include "common/result.h"

#include <gtest/gtest.h>

using namespace nyx;

TEST(ResultTest, OkHoldsValue) {
    auto r = Result<int>::ok(42);
    EXPECT_TRUE(r.is_ok());
    EXPECT_FALSE(r.is_err());
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultTest, ErrHoldsMessage) {
    auto r = Result<int>::err("something went wrong");
    EXPECT_TRUE(r.is_err());
    EXPECT_FALSE(r.is_ok());
    EXPECT_EQ(r.error().message, "something went wrong");
}

TEST(ResultTest, VoidOk) {
    auto r = Result<void>::ok();
    EXPECT_TRUE(r.is_ok());
}

TEST(ResultTest, VoidErr) {
    auto r = Result<void>::err("failed");
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.error().message, "failed");
}

TEST(ResultTest, MoveSemantics) {
    auto r          = Result<std::string>::ok("hello");
    std::string val = std::move(r.value());
    EXPECT_EQ(val, "hello");
}
