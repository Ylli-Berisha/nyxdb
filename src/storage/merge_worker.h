#pragma once

#include "common/types.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nyx {

class Catalog;

class MergeWorker {
  public:
    explicit MergeWorker(Catalog* catalog, u32 merge_threshold = 8);
    ~MergeWorker();

    void start();
    void stop();

    struct SegSnapshot {
        std::string dir;
        u64 id;
        u64 base_row_id;
        u64 row_count;
        std::vector<u8> deleted_bitmap;
        usize idx;
    };

  private:
    void loop_();
    void maybe_merge_table_(const std::string& table_name);

    Catalog* catalog_;
    u32 merge_threshold_;
    std::thread thread_;
    std::atomic<bool> stop_flag_{false};
    std::mutex mu_;
    std::condition_variable cv_;
};

} // namespace nyx
