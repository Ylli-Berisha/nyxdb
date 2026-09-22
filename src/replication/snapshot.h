#pragma once

#include "common/result.h"
#include "common/types.h"
#include "server/wire.h"

#include <functional>
#include <string>
#include <vector>

namespace nyx {
class Database;
}

namespace nyx::replication {

class SnapshotSender {
  public:
    using SendFn = std::function<Result<void>(server::FrameType, const std::vector<byte>&)>;

    SnapshotSender(Database* db, std::string data_root);
    Result<void> send(u32 query_id, SendFn send_fn);

  private:
    Database* db_;
    std::string data_root_;
};

struct SnapFile {
    std::string rel_path;
    u64 expected_size = 0;
    u64 received = 0;
};

class SnapshotReceiver {
  public:
    explicit SnapshotReceiver(std::string data_root);
    ~SnapshotReceiver();

    SnapshotReceiver(const SnapshotReceiver&) = delete;
    SnapshotReceiver& operator=(const SnapshotReceiver&) = delete;

    Result<void> on_meta(const byte* payload, usize len);
    Result<void> on_data(const byte* payload, usize len);
    Result<u64> on_end(const byte* payload, usize len);

  private:
    std::string data_root_;
    std::vector<SnapFile> files_;
    u32 current_idx_ = 0;
    int current_fd_ = -1;
};

} // namespace nyx::replication
