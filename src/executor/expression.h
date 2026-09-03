#pragma once

#include "common/result.h"
#include "common/types.h"
#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "storage/disk/type_id.h"
#include "storage/disk/value.h"

#include <memory>

namespace nyx {

enum class BinaryOpKind : u8 {
    ADD,
    SUB,
    MUL,
    DIV,
    LT,
    LE,
    EQ,
    GE,
    GT,
    NE,
};

enum class LogicalOpKind : u8 {
    AND,
    OR,
};

enum class NullCheckKind : u8 {
    IS_NULL,
    IS_NOT_NULL,
};

class Expression {
  public:
    virtual ~Expression() = default;
    virtual TypeId output_type() const = 0;
    virtual Result<ColumnVector> evaluate(const Chunk& input) = 0;
};

class ColumnRef : public Expression {
  public:
    ColumnRef(size_t index, TypeId type);
    TypeId output_type() const override { return type_; }
    Result<ColumnVector> evaluate(const Chunk& input) override;

  private:
    size_t index_;
    TypeId type_;
};

class Literal : public Expression {
  public:
    explicit Literal(Value value);
    TypeId output_type() const override { return type_; }
    Result<ColumnVector> evaluate(const Chunk& input) override;

  private:
    Value value_;
    TypeId type_;
};

class BinaryOp : public Expression {
  public:
    BinaryOp(BinaryOpKind op, std::unique_ptr<Expression> left, std::unique_ptr<Expression> right);
    TypeId output_type() const override { return output_type_; }
    Result<ColumnVector> evaluate(const Chunk& input) override;

  private:
    BinaryOpKind op_;
    std::unique_ptr<Expression> left_;
    std::unique_ptr<Expression> right_;
    TypeId input_type_;
    TypeId output_type_;
};

class LogicalOp : public Expression {
  public:
    LogicalOp(LogicalOpKind op, std::unique_ptr<Expression> left,
              std::unique_ptr<Expression> right);
    TypeId output_type() const override { return TypeId::INT32; }
    Result<ColumnVector> evaluate(const Chunk& input) override;

  private:
    LogicalOpKind op_;
    std::unique_ptr<Expression> left_;
    std::unique_ptr<Expression> right_;
};

class NotOp : public Expression {
  public:
    explicit NotOp(std::unique_ptr<Expression> child);
    TypeId output_type() const override { return TypeId::INT32; }
    Result<ColumnVector> evaluate(const Chunk& input) override;

  private:
    std::unique_ptr<Expression> child_;
};

class NullCheckOp : public Expression {
  public:
    NullCheckOp(NullCheckKind kind, std::unique_ptr<Expression> child);
    TypeId output_type() const override { return TypeId::INT32; }
    Result<ColumnVector> evaluate(const Chunk& input) override;

  private:
    NullCheckKind kind_;
    std::unique_ptr<Expression> child_;
};

} // namespace nyx
