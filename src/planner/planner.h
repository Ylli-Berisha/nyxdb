#pragma once

#include "binder/bound_ast.h"
#include "catalog/catalog.h"
#include "common/result.h"
#include "executor/expression.h"
#include "executor/index_scan.h"
#include "executor/operator.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace nyx {

class Planner {
  public:
    explicit Planner(Catalog& catalog);
    Result<std::unique_ptr<Operator>> plan(const bound::BoundSelect& stmt);

  private:
    struct ColCtx {
        std::vector<u32> binding_offsets;
        bool post_aggregate = false;
        u32 num_group_keys = 0;
        std::unordered_map<u64, u32> group_col_map;
    };

    using KeyVecs = std::pair<std::vector<std::unique_ptr<Expression>>,
                              std::vector<std::unique_ptr<Expression>>>;

    std::unique_ptr<Expression>
    lower_expr_(const bound::BoundExpr& e, const ColCtx& ctx,
                const std::vector<bound::BoundProjection>* projs = nullptr);

    struct IndexScanChoice {
        BTreeIndex* index;
        std::optional<IndexScan::Bound> lo;
        std::optional<IndexScan::Bound> hi;
    };

    std::optional<IndexScanChoice> try_index_scan_(const bound::BoundBinding& binding,
                                                   const bound::BoundExpr& where);

    Result<std::unique_ptr<Operator>> build_scans_(const bound::BoundSelect& stmt, ColCtx& ctx,
                                                   bool& where_consumed);
    Result<std::unique_ptr<Operator>> build_aggregate_(std::unique_ptr<Operator> child,
                                                       const bound::BoundSelect& stmt, ColCtx& ctx);
    Result<std::unique_ptr<Operator>> build_join_(std::unique_ptr<Operator> left,
                                                  const bound::BoundBinding& right_binding,
                                                  const bound::BoundExpr& on, u32 right_bid,
                                                  u32 left_col_count, ColCtx& ctx);
    Result<KeyVecs> decompose_on_(const bound::BoundExpr& on, const ColCtx& ctx, u32 build_bid);
    std::unique_ptr<Operator> build_filter_(std::unique_ptr<Operator> child,
                                            const bound::BoundExpr& pred, const ColCtx& ctx);
    std::unique_ptr<Operator> build_project_(std::unique_ptr<Operator> child,
                                             const bound::BoundSelect& stmt, const ColCtx& ctx);
    std::unique_ptr<Operator> build_sort_(std::unique_ptr<Operator> child,
                                          const bound::BoundSelect& stmt, const ColCtx& ctx);
    std::unique_ptr<Operator> build_limit_(std::unique_ptr<Operator> child,
                                           const bound::BoundSelect& stmt);

    Catalog& catalog_;
};

} // namespace nyx
