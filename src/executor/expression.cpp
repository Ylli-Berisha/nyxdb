#include "executor/expression.h"

#include <cassert>
#include <functional>
#include <utility>
#include <variant>

namespace nyx {

namespace {

template <typename T> T get_at(const ColumnVector& c, size_t i);
template <> i32 get_at<i32>(const ColumnVector& c, size_t i) {
    return c.get_i32(i);
}
template <> i64 get_at<i64>(const ColumnVector& c, size_t i) {
    return c.get_i64(i);
}
template <> f64 get_at<f64>(const ColumnVector& c, size_t i) {
    return c.get_f64(i);
}

template <typename T> void set_at(ColumnVector& c, size_t i, T v);
template <> void set_at<i32>(ColumnVector& c, size_t i, i32 v) {
    c.set_i32(i, v);
}
template <> void set_at<i64>(ColumnVector& c, size_t i, i64 v) {
    c.set_i64(i, v);
}
template <> void set_at<f64>(ColumnVector& c, size_t i, f64 v) {
    c.set_f64(i, v);
}

template <typename T, typename Op>
ColumnVector arith_kernel(const ColumnVector& l, const ColumnVector& r, TypeId out_type, Op op) {
    bool nullable = l.nullable() || r.nullable();
    size_t n = l.size();
    ColumnVector out = ColumnVector::make(out_type, n, nullable);
    for (size_t i = 0; i < n; ++i) {
        if (nullable && (l.is_null(i) || r.is_null(i))) {
            out.set_null(i);
        } else {
            set_at<T>(out, i, op(get_at<T>(l, i), get_at<T>(r, i)));
        }
    }
    return out;
}

template <typename T>
Result<ColumnVector> div_kernel(const ColumnVector& l, const ColumnVector& r, TypeId out_type) {
    bool nullable = l.nullable() || r.nullable();
    size_t n = l.size();
    ColumnVector out = ColumnVector::make(out_type, n, nullable);
    for (size_t i = 0; i < n; ++i) {
        if (nullable && (l.is_null(i) || r.is_null(i))) {
            out.set_null(i);
        } else {
            T b = get_at<T>(r, i);
            if (b == T{})
                return Result<ColumnVector>::err("division by zero");
            set_at<T>(out, i, get_at<T>(l, i) / b);
        }
    }
    return Result<ColumnVector>::ok(std::move(out));
}

template <typename T, typename Op>
ColumnVector cmp_kernel(const ColumnVector& l, const ColumnVector& r, Op op) {
    bool nullable = l.nullable() || r.nullable();
    size_t n = l.size();
    ColumnVector out = ColumnVector::make(TypeId::INT32, n, nullable);
    for (size_t i = 0; i < n; ++i) {
        if (nullable && (l.is_null(i) || r.is_null(i))) {
            out.set_null(i);
        } else {
            out.set_i32(i, op(get_at<T>(l, i), get_at<T>(r, i)) ? 1 : 0);
        }
    }
    return out;
}

template <typename T>
Result<ColumnVector> binary_dispatch(BinaryOpKind op, const ColumnVector& l, const ColumnVector& r,
                                     TypeId type) {
    switch (op) {
    case BinaryOpKind::ADD:
        return Result<ColumnVector>::ok(arith_kernel<T>(l, r, type, std::plus<T>{}));
    case BinaryOpKind::SUB:
        return Result<ColumnVector>::ok(arith_kernel<T>(l, r, type, std::minus<T>{}));
    case BinaryOpKind::MUL:
        return Result<ColumnVector>::ok(arith_kernel<T>(l, r, type, std::multiplies<T>{}));
    case BinaryOpKind::DIV:
        return div_kernel<T>(l, r, type);
    case BinaryOpKind::LT:
        return Result<ColumnVector>::ok(cmp_kernel<T>(l, r, std::less<T>{}));
    case BinaryOpKind::LE:
        return Result<ColumnVector>::ok(cmp_kernel<T>(l, r, std::less_equal<T>{}));
    case BinaryOpKind::EQ:
        return Result<ColumnVector>::ok(cmp_kernel<T>(l, r, std::equal_to<T>{}));
    case BinaryOpKind::GE:
        return Result<ColumnVector>::ok(cmp_kernel<T>(l, r, std::greater_equal<T>{}));
    case BinaryOpKind::GT:
        return Result<ColumnVector>::ok(cmp_kernel<T>(l, r, std::greater<T>{}));
    case BinaryOpKind::NE:
        return Result<ColumnVector>::ok(cmp_kernel<T>(l, r, std::not_equal_to<T>{}));
    }
    return Result<ColumnVector>::err("unknown binary op");
}

bool is_comparison(BinaryOpKind op) {
    return op >= BinaryOpKind::LT && op <= BinaryOpKind::NE;
}

TypeId variant_type(const Value& v) {
    if (std::holds_alternative<i32>(v))
        return TypeId::INT32;
    if (std::holds_alternative<i64>(v))
        return TypeId::INT64;
    if (std::holds_alternative<f64>(v))
        return TypeId::DOUBLE;
    return TypeId::INVALID;
}

} // namespace

ColumnRef::ColumnRef(size_t index, TypeId type) : index_(index), type_(type) {
    assert(type_ != TypeId::INVALID);
}

Result<ColumnVector> ColumnRef::evaluate(const Chunk& input) {
    assert(index_ < input.column_count());
    assert(input.column(index_).type() == type_);
    return Result<ColumnVector>::ok(input.column(index_));
}

Literal::Literal(Value value) : value_(std::move(value)), type_(variant_type(value_)) {
    assert(type_ != TypeId::INVALID);
}

Result<ColumnVector> Literal::evaluate(const Chunk& input) {
    size_t n = input.row_count();
    ColumnVector out = ColumnVector::make(type_, n, false);
    if (type_ == TypeId::INT32) {
        i32 v = std::get<i32>(value_);
        for (size_t i = 0; i < n; ++i)
            out.set_i32(i, v);
    } else if (type_ == TypeId::INT64) {
        i64 v = std::get<i64>(value_);
        for (size_t i = 0; i < n; ++i)
            out.set_i64(i, v);
    } else {
        f64 v = std::get<f64>(value_);
        for (size_t i = 0; i < n; ++i)
            out.set_f64(i, v);
    }
    return Result<ColumnVector>::ok(std::move(out));
}

BinaryOp::BinaryOp(BinaryOpKind op, std::unique_ptr<Expression> left,
                   std::unique_ptr<Expression> right)
    : op_(op), left_(std::move(left)), right_(std::move(right)), input_type_(left_->output_type()),
      output_type_(is_comparison(op) ? TypeId::INT32 : input_type_) {
    assert(left_->output_type() == right_->output_type());
    assert(input_type_ == TypeId::INT32 || input_type_ == TypeId::INT64 ||
           input_type_ == TypeId::DOUBLE);
}

Result<ColumnVector> BinaryOp::evaluate(const Chunk& input) {
    auto lr = left_->evaluate(input);
    if (lr.is_err())
        return lr;
    auto rr = right_->evaluate(input);
    if (rr.is_err())
        return rr;

    const ColumnVector& l = lr.value();
    const ColumnVector& r = rr.value();

    switch (input_type_) {
    case TypeId::INT32:
        return binary_dispatch<i32>(op_, l, r, TypeId::INT32);
    case TypeId::INT64:
        return binary_dispatch<i64>(op_, l, r, TypeId::INT64);
    case TypeId::DOUBLE:
        return binary_dispatch<f64>(op_, l, r, TypeId::DOUBLE);
    default:
        return Result<ColumnVector>::err("unsupported input type");
    }
}

} // namespace nyx
