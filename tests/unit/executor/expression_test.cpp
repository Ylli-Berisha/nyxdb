#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/expression.h"

#include <gtest/gtest.h>
#include <memory>

using namespace nyx;

static Chunk make_i64_chunk(const std::vector<i64>& a, const std::vector<i64>& b) {
    auto ca = ColumnVector::make(TypeId::INT64, a.size(), false);
    auto cb = ColumnVector::make(TypeId::INT64, b.size(), false);
    for (size_t i = 0; i < a.size(); ++i)
        ca.set_i64(i, a[i]);
    for (size_t i = 0; i < b.size(); ++i)
        cb.set_i64(i, b[i]);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(ca));
    cols.push_back(std::move(cb));
    return Chunk(a.size(), std::move(cols));
}

TEST(ExpressionTest, LiteralI32BroadcastsToChunkSize) {
    auto placeholder = ColumnVector::make(TypeId::INT32, 100, false);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(placeholder));
    Chunk c(100, std::move(cols));

    Literal lit(Value{static_cast<i32>(42)});
    auto r = lit.evaluate(c);
    ASSERT_TRUE(r.is_ok());
    const ColumnVector& v = r.value();
    EXPECT_EQ(v.type(), TypeId::INT32);
    EXPECT_EQ(v.size(), 100u);
    for (size_t i = 0; i < 100; ++i)
        EXPECT_EQ(v.get_i32(i), 42);
}

TEST(ExpressionTest, LiteralI64AndDoubleBroadcast) {
    auto placeholder = ColumnVector::make(TypeId::INT32, 5, false);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(placeholder));
    Chunk c(5, std::move(cols));

    Literal li(Value{static_cast<i64>(1234567890123LL)});
    auto ri = li.evaluate(c);
    ASSERT_TRUE(ri.is_ok());
    EXPECT_EQ(ri.value().type(), TypeId::INT64);
    for (size_t i = 0; i < 5; ++i)
        EXPECT_EQ(ri.value().get_i64(i), 1234567890123LL);

    Literal ld(Value{3.14});
    auto rd = ld.evaluate(c);
    ASSERT_TRUE(rd.is_ok());
    EXPECT_EQ(rd.value().type(), TypeId::DOUBLE);
    for (size_t i = 0; i < 5; ++i)
        EXPECT_DOUBLE_EQ(rd.value().get_f64(i), 3.14);
}

TEST(ExpressionTest, ColumnRefCopiesInputColumn) {
    Chunk c = make_i64_chunk({10, 20, 30, 40}, {1, 2, 3, 4});
    ColumnRef ref(1, TypeId::INT64);
    auto r = ref.evaluate(c);
    ASSERT_TRUE(r.is_ok());
    const ColumnVector& v = r.value();
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v.get_i64(0), 1);
    EXPECT_EQ(v.get_i64(3), 4);
}

TEST(ExpressionTest, AddInt64NoNulls) {
    Chunk c = make_i64_chunk({1, 2, 3, 4, 5}, {10, 20, 30, 40, 50});
    BinaryOp add(BinaryOpKind::ADD, std::make_unique<ColumnRef>(0, TypeId::INT64),
                 std::make_unique<ColumnRef>(1, TypeId::INT64));
    auto r = add.evaluate(c);
    ASSERT_TRUE(r.is_ok());
    const ColumnVector& v = r.value();
    EXPECT_EQ(v.type(), TypeId::INT64);
    ASSERT_EQ(v.size(), 5u);
    EXPECT_EQ(v.get_i64(0), 11);
    EXPECT_EQ(v.get_i64(4), 55);
}

TEST(ExpressionTest, SubMulDivInt32) {
    auto ca = ColumnVector::make(TypeId::INT32, 3, false);
    auto cb = ColumnVector::make(TypeId::INT32, 3, false);
    ca.set_i32(0, 100);
    ca.set_i32(1, 50);
    ca.set_i32(2, 12);
    cb.set_i32(0, 10);
    cb.set_i32(1, 5);
    cb.set_i32(2, 3);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(ca));
    cols.push_back(std::move(cb));
    Chunk c(3, std::move(cols));

    auto sub = BinaryOp(BinaryOpKind::SUB, std::make_unique<ColumnRef>(0, TypeId::INT32),
                        std::make_unique<ColumnRef>(1, TypeId::INT32))
                   .evaluate(c);
    ASSERT_TRUE(sub.is_ok());
    EXPECT_EQ(sub.value().get_i32(0), 90);
    EXPECT_EQ(sub.value().get_i32(1), 45);
    EXPECT_EQ(sub.value().get_i32(2), 9);

    auto mul = BinaryOp(BinaryOpKind::MUL, std::make_unique<ColumnRef>(0, TypeId::INT32),
                        std::make_unique<ColumnRef>(1, TypeId::INT32))
                   .evaluate(c);
    ASSERT_TRUE(mul.is_ok());
    EXPECT_EQ(mul.value().get_i32(0), 1000);
    EXPECT_EQ(mul.value().get_i32(2), 36);

    auto div = BinaryOp(BinaryOpKind::DIV, std::make_unique<ColumnRef>(0, TypeId::INT32),
                        std::make_unique<ColumnRef>(1, TypeId::INT32))
                   .evaluate(c);
    ASSERT_TRUE(div.is_ok());
    EXPECT_EQ(div.value().get_i32(0), 10);
    EXPECT_EQ(div.value().get_i32(2), 4);
}

TEST(ExpressionTest, ArithmeticF64) {
    auto ca = ColumnVector::make(TypeId::DOUBLE, 3, false);
    auto cb = ColumnVector::make(TypeId::DOUBLE, 3, false);
    ca.set_f64(0, 1.5);
    ca.set_f64(1, 4.0);
    ca.set_f64(2, 9.0);
    cb.set_f64(0, 0.5);
    cb.set_f64(1, 2.0);
    cb.set_f64(2, 3.0);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(ca));
    cols.push_back(std::move(cb));
    Chunk c(3, std::move(cols));

    auto add = BinaryOp(BinaryOpKind::ADD, std::make_unique<ColumnRef>(0, TypeId::DOUBLE),
                        std::make_unique<ColumnRef>(1, TypeId::DOUBLE))
                   .evaluate(c);
    ASSERT_TRUE(add.is_ok());
    EXPECT_DOUBLE_EQ(add.value().get_f64(0), 2.0);
    EXPECT_DOUBLE_EQ(add.value().get_f64(2), 12.0);

    auto div = BinaryOp(BinaryOpKind::DIV, std::make_unique<ColumnRef>(0, TypeId::DOUBLE),
                        std::make_unique<ColumnRef>(1, TypeId::DOUBLE))
                   .evaluate(c);
    ASSERT_TRUE(div.is_ok());
    EXPECT_DOUBLE_EQ(div.value().get_f64(0), 3.0);
    EXPECT_DOUBLE_EQ(div.value().get_f64(1), 2.0);
    EXPECT_DOUBLE_EQ(div.value().get_f64(2), 3.0);
}

TEST(ExpressionTest, ComparisonLTOutputsInt32) {
    Chunk c = make_i64_chunk({1, 5, 3, 8, 2}, {2, 5, 4, 7, 10});
    BinaryOp lt(BinaryOpKind::LT, std::make_unique<ColumnRef>(0, TypeId::INT64),
                std::make_unique<ColumnRef>(1, TypeId::INT64));
    EXPECT_EQ(lt.output_type(), TypeId::INT32);
    auto r = lt.evaluate(c);
    ASSERT_TRUE(r.is_ok());
    const ColumnVector& v = r.value();
    EXPECT_EQ(v.type(), TypeId::INT32);
    EXPECT_EQ(v.get_i32(0), 1); // 1 < 2
    EXPECT_EQ(v.get_i32(1), 0); // 5 < 5
    EXPECT_EQ(v.get_i32(2), 1); // 3 < 4
    EXPECT_EQ(v.get_i32(3), 0); // 8 < 7
    EXPECT_EQ(v.get_i32(4), 1); // 2 < 10
}

TEST(ExpressionTest, AllComparisons) {
    auto ca = ColumnVector::make(TypeId::INT64, 3, false);
    ca.set_i64(0, 5);
    ca.set_i64(1, 10);
    ca.set_i64(2, 15);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(ca));
    Chunk c(3, std::move(cols));

    auto build_cmp = [](BinaryOpKind k) {
        return BinaryOp(k, std::make_unique<ColumnRef>(0, TypeId::INT64),
                        std::make_unique<Literal>(Value{static_cast<i64>(10)}));
    };

    auto rlt = build_cmp(BinaryOpKind::LT).evaluate(c);
    ASSERT_TRUE(rlt.is_ok());
    EXPECT_EQ(rlt.value().get_i32(0), 1);
    EXPECT_EQ(rlt.value().get_i32(1), 0);
    EXPECT_EQ(rlt.value().get_i32(2), 0);

    auto rle = build_cmp(BinaryOpKind::LE).evaluate(c);
    ASSERT_TRUE(rle.is_ok());
    EXPECT_EQ(rle.value().get_i32(0), 1);
    EXPECT_EQ(rle.value().get_i32(1), 1);
    EXPECT_EQ(rle.value().get_i32(2), 0);

    auto req = build_cmp(BinaryOpKind::EQ).evaluate(c);
    ASSERT_TRUE(req.is_ok());
    EXPECT_EQ(req.value().get_i32(0), 0);
    EXPECT_EQ(req.value().get_i32(1), 1);
    EXPECT_EQ(req.value().get_i32(2), 0);

    auto rge = build_cmp(BinaryOpKind::GE).evaluate(c);
    ASSERT_TRUE(rge.is_ok());
    EXPECT_EQ(rge.value().get_i32(0), 0);
    EXPECT_EQ(rge.value().get_i32(1), 1);
    EXPECT_EQ(rge.value().get_i32(2), 1);

    auto rgt = build_cmp(BinaryOpKind::GT).evaluate(c);
    ASSERT_TRUE(rgt.is_ok());
    EXPECT_EQ(rgt.value().get_i32(0), 0);
    EXPECT_EQ(rgt.value().get_i32(1), 0);
    EXPECT_EQ(rgt.value().get_i32(2), 1);

    auto rne = build_cmp(BinaryOpKind::NE).evaluate(c);
    ASSERT_TRUE(rne.is_ok());
    EXPECT_EQ(rne.value().get_i32(0), 1);
    EXPECT_EQ(rne.value().get_i32(1), 0);
    EXPECT_EQ(rne.value().get_i32(2), 1);
}

TEST(ExpressionTest, NullPropagationInAdd) {
    auto ca = ColumnVector::make(TypeId::INT64, 4, true);
    auto cb = ColumnVector::make(TypeId::INT64, 4, false);
    ca.set_i64(0, 10);
    ca.set_null(1);
    ca.set_i64(2, 30);
    ca.set_null(3);
    cb.set_i64(0, 1);
    cb.set_i64(1, 2);
    cb.set_i64(2, 3);
    cb.set_i64(3, 4);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(ca));
    cols.push_back(std::move(cb));
    Chunk c(4, std::move(cols));

    BinaryOp add(BinaryOpKind::ADD, std::make_unique<ColumnRef>(0, TypeId::INT64),
                 std::make_unique<ColumnRef>(1, TypeId::INT64));
    auto r = add.evaluate(c);
    ASSERT_TRUE(r.is_ok());
    const ColumnVector& v = r.value();
    ASSERT_TRUE(v.nullable());
    EXPECT_FALSE(v.is_null(0));
    EXPECT_EQ(v.get_i64(0), 11);
    EXPECT_TRUE(v.is_null(1));
    EXPECT_FALSE(v.is_null(2));
    EXPECT_EQ(v.get_i64(2), 33);
    EXPECT_TRUE(v.is_null(3));
}

TEST(ExpressionTest, NullPropagationInCompare) {
    auto ca = ColumnVector::make(TypeId::INT64, 4, true);
    ca.set_i64(0, 3);
    ca.set_null(1);
    ca.set_i64(2, 10);
    ca.set_null(3);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(ca));
    Chunk c(4, std::move(cols));

    BinaryOp lt(BinaryOpKind::LT, std::make_unique<ColumnRef>(0, TypeId::INT64),
                std::make_unique<Literal>(Value{static_cast<i64>(5)}));
    auto r = lt.evaluate(c);
    ASSERT_TRUE(r.is_ok());
    const ColumnVector& v = r.value();
    ASSERT_TRUE(v.nullable());
    EXPECT_FALSE(v.is_null(0));
    EXPECT_EQ(v.get_i32(0), 1); // 3 < 5
    EXPECT_TRUE(v.is_null(1));
    EXPECT_FALSE(v.is_null(2));
    EXPECT_EQ(v.get_i32(2), 0); // 10 < 5
    EXPECT_TRUE(v.is_null(3));
}

TEST(ExpressionTest, DivByZeroReturnsErr) {
    auto ca = ColumnVector::make(TypeId::INT64, 3, false);
    auto cb = ColumnVector::make(TypeId::INT64, 3, false);
    ca.set_i64(0, 10);
    ca.set_i64(1, 20);
    ca.set_i64(2, 30);
    cb.set_i64(0, 2);
    cb.set_i64(1, 0);
    cb.set_i64(2, 3);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(ca));
    cols.push_back(std::move(cb));
    Chunk c(3, std::move(cols));

    BinaryOp div(BinaryOpKind::DIV, std::make_unique<ColumnRef>(0, TypeId::INT64),
                 std::make_unique<ColumnRef>(1, TypeId::INT64));
    auto r = div.evaluate(c);
    EXPECT_TRUE(r.is_err());
}

TEST(ExpressionTest, ComposedExpression) {
    Chunk c = make_i64_chunk({10, 40, 70, 30}, {5, 20, 25, 60});

    auto sum =
        std::make_unique<BinaryOp>(BinaryOpKind::ADD, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<ColumnRef>(1, TypeId::INT64));
    BinaryOp cmp(BinaryOpKind::LT, std::move(sum),
                 std::make_unique<Literal>(Value{static_cast<i64>(100)}));

    auto r = cmp.evaluate(c);
    ASSERT_TRUE(r.is_ok());
    const ColumnVector& v = r.value();
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v.get_i32(0), 1); // 15 < 100
    EXPECT_EQ(v.get_i32(1), 1); // 60 < 100
    EXPECT_EQ(v.get_i32(2), 1); // 95 < 100
    EXPECT_EQ(v.get_i32(3), 1); // 90 < 100
}
