#include "replication/snapshot.h"

#include "database/database.h"
#include "server/wire.h"
#include "storage/wal/wal_record.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

namespace nyx::replication {

namespace fs = std::filesystem;

static constexpr usize CHUNK_SIZE = 64 * 1024;

SnapshotSender::SnapshotSender(Database* db, std::string data_root)
    : db_(db), data_root_(std::move(data_root)) {}

Result<void> SnapshotSender::send(u32 query_id, SendFn send_fn) {
    auto flush_r = db_->flush();
    if (flush_r.is_err())
        return flush_r;

    // Record post-flush WAL position — followers resume streaming from here.
    // If the WAL was checkpointed, current_wal_lsn() returns 0; use WAL_HEADER_SIZE
    // so the follower doesn't re-trigger STALE with from_offset=0.
    u64 resume_lsn = db_->current_wal_lsn();
    if (resume_lsn == 0)
        resume_lsn = WAL_HEADER_SIZE;

    std::vector<std::pair<std::string, u64>> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(data_root_, ec)) {
        if (!entry.is_directory())
            continue;
        for (const auto& f : fs::recursive_directory_iterator(entry.path(), ec)) {
            if (!f.is_regular_file())
                continue;
            auto rel = fs::relative(f.path(), data_root_, ec).string();
            if (ec)
                continue;
            files.push_back({rel, static_cast<u64>(f.file_size(ec))});
        }
    }

    std::vector<byte> meta;
    server::encode_u32(meta, static_cast<u32>(files.size()));
    for (const auto& [path, size] : files) {
        server::encode_str(meta, path);
        server::encode_u64(meta, size);
    }
    auto r = send_fn(server::FrameType::REPL_SNAPSHOT_META, meta);
    if (r.is_err())
        return r;

    for (u32 idx = 0; idx < static_cast<u32>(files.size()); ++idx) {
        std::string full_path = data_root_ + "/" + files[idx].first;
        int fd = ::open(full_path.c_str(), O_RDONLY);
        if (fd < 0)
            return Result<void>::err("snapshot: cannot open " + full_path + ": " + strerror(errno));

        byte chunk[CHUNK_SIZE];
        ssize_t n;
        while ((n = ::read(fd, chunk, CHUNK_SIZE)) > 0) {
            std::vector<byte> payload;
            server::encode_u32(payload, idx);
            payload.insert(payload.end(), chunk, chunk + n);
            auto sr = send_fn(server::FrameType::REPL_SNAPSHOT_DATA, payload);
            if (sr.is_err()) {
                ::close(fd);
                return sr;
            }
        }
        ::close(fd);
        if (n < 0)
            return Result<void>::err("snapshot: read failed on " + full_path);
    }

    std::vector<byte> end_payload;
    server::encode_u64(end_payload, resume_lsn);
    return send_fn(server::FrameType::REPL_SNAPSHOT_END, end_payload);
}

SnapshotReceiver::SnapshotReceiver(std::string data_root) : data_root_(std::move(data_root)) {}

SnapshotReceiver::~SnapshotReceiver() {
    if (current_fd_ >= 0)
        ::close(current_fd_);
}

Result<void> SnapshotReceiver::on_meta(const byte* payload, usize len) {
    if (current_fd_ >= 0) {
        ::close(current_fd_);
        current_fd_ = -1;
    }
    files_.clear();
    current_idx_ = 0;

    if (len < 4)
        return Result<void>::err("REPL_SNAPSHOT_META: truncated");

    u32 count = server::decode_u32(payload);
    payload += 4;
    len -= 4;

    for (u32 i = 0; i < count; ++i) {
        if (len < 2)
            return Result<void>::err("REPL_SNAPSHOT_META: truncated at file " + std::to_string(i));
        u16 path_len = server::decode_u16(payload);
        payload += 2;
        len -= 2;
        if (len < path_len + 8)
            return Result<void>::err("REPL_SNAPSHOT_META: truncated path/size");
        std::string rel_path(reinterpret_cast<const char*>(payload), path_len);
        payload += path_len;
        u64 size = server::decode_u64(payload);
        payload += 8;
        len -= static_cast<usize>(path_len) + 8;
        files_.push_back({std::move(rel_path), size});
    }

    return Result<void>::ok();
}

Result<void> SnapshotReceiver::on_data(const byte* payload, usize len) {
    if (len < 4)
        return Result<void>::err("REPL_SNAPSHOT_DATA: truncated");

    u32 file_idx = server::decode_u32(payload);
    payload += 4;
    len -= 4;

    if (file_idx >= static_cast<u32>(files_.size()))
        return Result<void>::err("REPL_SNAPSHOT_DATA: invalid file_idx");

    if (file_idx != current_idx_) {
        if (current_fd_ >= 0) {
            ::close(current_fd_);
            current_fd_ = -1;
        }
        current_idx_ = file_idx;
    }

    if (current_fd_ < 0) {
        std::string full_path = data_root_ + "/" + files_[file_idx].rel_path;
        auto parent = fs::path(full_path).parent_path();
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec)
            return Result<void>::err("snapshot: mkdir failed: " + ec.message());
        current_fd_ = ::open(full_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (current_fd_ < 0)
            return Result<void>::err("snapshot: cannot create " + full_path + ": " +
                                     strerror(errno));
    }

    ssize_t n = ::write(current_fd_, payload, len);
    if (n < 0)
        return Result<void>::err("snapshot: write failed: " + std::string(strerror(errno)));

    files_[file_idx].received += static_cast<u64>(n);
    return Result<void>::ok();
}

Result<u64> SnapshotReceiver::on_end(const byte* payload, usize len) {
    if (current_fd_ >= 0) {
        ::close(current_fd_);
        current_fd_ = -1;
    }
    if (len < 8)
        return Result<u64>::err("REPL_SNAPSHOT_END: truncated");
    return Result<u64>::ok(server::decode_u64(payload));
}

} // namespace nyx::replication
