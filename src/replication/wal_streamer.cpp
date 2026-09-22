#include "replication/wal_streamer.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace nyx::replication {

WalStreamer::WalStreamer(std::string wal_path) : wal_path_(std::move(wal_path)) {}

Result<std::vector<byte>> WalStreamer::get_batch(u64 from_offset, u64 max_bytes) const {
    int fd = ::open(wal_path_.c_str(), O_RDONLY);
    if (fd < 0)
        return Result<std::vector<byte>>::err("WalStreamer: cannot open WAL: " +
                                              std::string(strerror(errno)));

    off_t file_size = ::lseek(fd, 0, SEEK_END);
    if (file_size < 0 || from_offset >= static_cast<u64>(file_size)) {
        ::close(fd);
        return Result<std::vector<byte>>::ok({});
    }

    if (::lseek(fd, static_cast<off_t>(from_offset), SEEK_SET) < 0) {
        ::close(fd);
        return Result<std::vector<byte>>::err("WalStreamer: lseek failed");
    }

    u64 available = static_cast<u64>(file_size) - from_offset;
    u64 to_read = std::min(max_bytes, available);

    std::vector<byte> buf(static_cast<usize>(to_read));
    ssize_t n = ::read(fd, buf.data(), static_cast<size_t>(to_read));
    ::close(fd);

    if (n < 0)
        return Result<std::vector<byte>>::err("WalStreamer: read failed");

    buf.resize(static_cast<usize>(n));
    return Result<std::vector<byte>>::ok(std::move(buf));
}

u64 WalStreamer::current_size() const {
    int fd = ::open(wal_path_.c_str(), O_RDONLY);
    if (fd < 0)
        return 0;
    off_t size = ::lseek(fd, 0, SEEK_END);
    ::close(fd);
    return static_cast<u64>(size < 0 ? 0 : size);
}

} // namespace nyx::replication
