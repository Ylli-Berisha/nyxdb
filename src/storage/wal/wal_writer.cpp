#include "storage/wal/wal_writer.h"

#include "common/xxhash.h"
#include "storage/wal/wal_record.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nyx {

WalWriter::WalWriter(int fd, u64 offset, std::string path)
    : fd_(fd), offset_(offset), path_(std::move(path)) {}

WalWriter::~WalWriter() {
    if (fd_ >= 0)
        ::close(fd_);
}

WalWriter::WalWriter(WalWriter&& other) noexcept
    : fd_(other.fd_), offset_(other.offset_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

WalWriter& WalWriter::operator=(WalWriter&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = other.fd_;
        offset_ = other.offset_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

Result<WalWriter> WalWriter::open(const std::string& path) {
    bool is_new = (::access(path.c_str(), F_OK) != 0);

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return Result<WalWriter>::err("WalWriter: cannot open " + path + ": " + strerror(errno));

    u64 offset = 0;
    if (is_new) {
        u8 hdr[WAL_HEADER_SIZE];
        std::memcpy(hdr, WAL_MAGIC, 4);
        wal_put_u16(hdr + 4, WAL_VERSION);
        ssize_t n = ::write(fd, hdr, WAL_HEADER_SIZE);
        if (n != static_cast<ssize_t>(WAL_HEADER_SIZE)) {
            ::close(fd);
            return Result<WalWriter>::err("WalWriter: header write failed");
        }
        if (::fsync(fd) != 0) {
            ::close(fd);
            return Result<WalWriter>::err("WalWriter: fsync failed");
        }
        offset = WAL_HEADER_SIZE;
    } else {
        struct stat st {};
        ::fstat(fd, &st);
        offset = static_cast<u64>(st.st_size);
    }

    return Result<WalWriter>::ok(WalWriter(fd, offset, path));
}

Result<void> WalWriter::write_record(std::vector<u8>& buf) {
    u64 checksum = xxhash64(buf.data(), buf.size());
    u8 cs[8];
    wal_put_u64(cs, checksum);
    buf.insert(buf.end(), cs, cs + 8);

    ssize_t n = ::write(fd_, buf.data(), buf.size());
    if (n != static_cast<ssize_t>(buf.size()))
        return Result<void>::err("WalWriter: short write");
    if (::fsync(fd_) != 0)
        return Result<void>::err("WalWriter: fsync failed");

    offset_ += static_cast<u64>(buf.size());
    return Result<void>::ok();
}

Result<void> WalWriter::log_insert(const std::string& table, const Schema& schema,
                                   const std::vector<std::vector<Value>>& rows) {
    if (rows.empty())
        return Result<void>::ok();

    std::vector<u8> buf;
    buf.push_back(WAL_TYPE_INSERT);

    u8 tmp[8];
    wal_put_u16(tmp, static_cast<u16>(table.size()));
    buf.insert(buf.end(), tmp, tmp + 2);
    buf.insert(buf.end(), table.begin(), table.end());

    wal_put_u32(tmp, static_cast<u32>(rows.size()));
    buf.insert(buf.end(), tmp, tmp + 4);
    wal_put_u16(tmp, static_cast<u16>(schema.size()));
    buf.insert(buf.end(), tmp, tmp + 2);

    for (const auto& col : schema) {
        buf.push_back(static_cast<u8>(col.type));
        wal_put_u16(tmp, col.max_len);
        buf.insert(buf.end(), tmp, tmp + 2);
    }

    for (const auto& row : rows) {
        for (size_t c = 0; c < row.size(); ++c) {
            const Value& v = row[c];
            if (is_null(v)) {
                buf.push_back(1);
            } else {
                buf.push_back(0);
                TypeId t = schema[c].type;
                if (t == TypeId::INT32) {
                    wal_put_u32(tmp, static_cast<u32>(std::get<i32>(v)));
                    buf.insert(buf.end(), tmp, tmp + 4);
                } else if (t == TypeId::INT64) {
                    wal_put_u64(tmp, static_cast<u64>(std::get<i64>(v)));
                    buf.insert(buf.end(), tmp, tmp + 8);
                } else if (t == TypeId::DOUBLE) {
                    f64 dv = std::get<f64>(v);
                    u64 bits;
                    std::memcpy(&bits, &dv, 8);
                    wal_put_u64(tmp, bits);
                    buf.insert(buf.end(), tmp, tmp + 8);
                } else if (t == TypeId::BOOL) {
                    buf.push_back(std::get<bool>(v) ? 1u : 0u);
                } else if (t == TypeId::DATE) {
                    wal_put_u32(tmp, static_cast<u32>(std::get<Date>(v).days));
                    buf.insert(buf.end(), tmp, tmp + 4);
                } else if (t == TypeId::TIMESTAMP) {
                    wal_put_u64(tmp, static_cast<u64>(std::get<Timestamp>(v).micros));
                    buf.insert(buf.end(), tmp, tmp + 8);
                } else {
                    const std::string& s = std::get<std::string>(v);
                    wal_put_u16(tmp, static_cast<u16>(s.size()));
                    buf.insert(buf.end(), tmp, tmp + 2);
                    buf.insert(buf.end(), s.begin(), s.end());
                }
            }
        }
    }

    return write_record(buf);
}

Result<void> WalWriter::log_create_table(const std::string& table, const Schema& schema) {
    std::vector<u8> buf;
    buf.push_back(WAL_TYPE_CREATE);

    u8 tmp[8];
    wal_put_u16(tmp, static_cast<u16>(table.size()));
    buf.insert(buf.end(), tmp, tmp + 2);
    buf.insert(buf.end(), table.begin(), table.end());

    wal_put_u16(tmp, static_cast<u16>(schema.size()));
    buf.insert(buf.end(), tmp, tmp + 2);
    for (const auto& col : schema) {
        buf.push_back(static_cast<u8>(col.type));
        buf.push_back(col.nullable ? 1 : 0);
        wal_put_u16(tmp, col.max_len);
        buf.insert(buf.end(), tmp, tmp + 2);
        wal_put_u16(tmp, static_cast<u16>(col.name.size()));
        buf.insert(buf.end(), tmp, tmp + 2);
        buf.insert(buf.end(), col.name.begin(), col.name.end());
    }

    return write_record(buf);
}

Result<void> WalWriter::log_delete(const std::string& table, const std::vector<u64>& row_indices) {
    std::vector<u8> buf;
    buf.push_back(WAL_TYPE_DELETE);

    u8 tmp[8];
    wal_put_u16(tmp, static_cast<u16>(table.size()));
    buf.insert(buf.end(), tmp, tmp + 2);
    buf.insert(buf.end(), table.begin(), table.end());

    wal_put_u64(tmp, static_cast<u64>(row_indices.size()));
    buf.insert(buf.end(), tmp, tmp + 8);

    for (u64 idx : row_indices) {
        wal_put_u64(tmp, idx);
        buf.insert(buf.end(), tmp, tmp + 8);
    }

    return write_record(buf);
}

Result<void> WalWriter::log_update(const std::string& table, const std::vector<u64>& old_indices,
                                   const Schema& schema,
                                   const std::vector<std::vector<Value>>& new_rows) {
    std::vector<u8> buf;
    buf.push_back(WAL_TYPE_UPDATE);

    u8 tmp[8];
    wal_put_u16(tmp, static_cast<u16>(table.size()));
    buf.insert(buf.end(), tmp, tmp + 2);
    buf.insert(buf.end(), table.begin(), table.end());

    wal_put_u64(tmp, static_cast<u64>(old_indices.size()));
    buf.insert(buf.end(), tmp, tmp + 8);
    for (u64 idx : old_indices) {
        wal_put_u64(tmp, idx);
        buf.insert(buf.end(), tmp, tmp + 8);
    }

    wal_put_u32(tmp, static_cast<u32>(new_rows.size()));
    buf.insert(buf.end(), tmp, tmp + 4);
    wal_put_u16(tmp, static_cast<u16>(schema.size()));
    buf.insert(buf.end(), tmp, tmp + 2);

    for (const auto& col : schema) {
        buf.push_back(static_cast<u8>(col.type));
        wal_put_u16(tmp, col.max_len);
        buf.insert(buf.end(), tmp, tmp + 2);
    }

    for (const auto& row : new_rows) {
        for (size_t c = 0; c < row.size(); ++c) {
            const Value& v = row[c];
            if (is_null(v)) {
                buf.push_back(1);
            } else {
                buf.push_back(0);
                TypeId t = schema[c].type;
                if (t == TypeId::INT32) {
                    wal_put_u32(tmp, static_cast<u32>(std::get<i32>(v)));
                    buf.insert(buf.end(), tmp, tmp + 4);
                } else if (t == TypeId::INT64) {
                    wal_put_u64(tmp, static_cast<u64>(std::get<i64>(v)));
                    buf.insert(buf.end(), tmp, tmp + 8);
                } else if (t == TypeId::DOUBLE) {
                    f64 dv = std::get<f64>(v);
                    u64 bits;
                    std::memcpy(&bits, &dv, 8);
                    wal_put_u64(tmp, bits);
                    buf.insert(buf.end(), tmp, tmp + 8);
                } else if (t == TypeId::BOOL) {
                    buf.push_back(std::get<bool>(v) ? 1u : 0u);
                } else if (t == TypeId::DATE) {
                    wal_put_u32(tmp, static_cast<u32>(std::get<Date>(v).days));
                    buf.insert(buf.end(), tmp, tmp + 4);
                } else if (t == TypeId::TIMESTAMP) {
                    wal_put_u64(tmp, static_cast<u64>(std::get<Timestamp>(v).micros));
                    buf.insert(buf.end(), tmp, tmp + 8);
                } else {
                    const std::string& s = std::get<std::string>(v);
                    wal_put_u16(tmp, static_cast<u16>(s.size()));
                    buf.insert(buf.end(), tmp, tmp + 2);
                    buf.insert(buf.end(), s.begin(), s.end());
                }
            }
        }
    }

    return write_record(buf);
}

Result<void> WalWriter::checkpoint() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (::unlink(path_.c_str()) != 0 && errno != ENOENT)
        return Result<void>::err("WalWriter::checkpoint: unlink failed: " +
                                 std::string(strerror(errno)));
    offset_ = 0;
    return Result<void>::ok();
}

} // namespace nyx
