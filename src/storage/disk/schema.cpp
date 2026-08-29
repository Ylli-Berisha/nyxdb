#include "storage/disk/schema.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace nyx {
namespace SchemaFile {

static constexpr u8 MAGIC[4] = {'N', 'Y', 'X', '1'};
static constexpr u8 FLAG_NULLABLE = 0x01;

static void put_u16(u8* dst, u16 v) {
    dst[0] = static_cast<u8>(v & 0xFF);
    dst[1] = static_cast<u8>((v >> 8) & 0xFF);
}

static void put_u32(u8* dst, u32 v) {
    dst[0] = static_cast<u8>(v & 0xFF);
    dst[1] = static_cast<u8>((v >> 8) & 0xFF);
    dst[2] = static_cast<u8>((v >> 16) & 0xFF);
    dst[3] = static_cast<u8>((v >> 24) & 0xFF);
}

static u16 read_u16(const u8* src) {
    return static_cast<u16>(src[0]) | (static_cast<u16>(src[1]) << 8);
}

static u32 read_u32(const u8* src) {
    return static_cast<u32>(src[0]) | (static_cast<u32>(src[1]) << 8) |
           (static_cast<u32>(src[2]) << 16) | (static_cast<u32>(src[3]) << 24);
}

Result<void> write(const std::string& path, const Schema& schema) {
    std::vector<u8> buf;
    buf.insert(buf.end(), MAGIC, MAGIC + 4);
    u8 count_bytes[4];
    put_u32(count_bytes, static_cast<u32>(schema.size()));
    buf.insert(buf.end(), count_bytes, count_bytes + 4);

    for (const auto& col : schema) {
        buf.push_back(static_cast<u8>(col.type));
        buf.push_back(col.nullable ? FLAG_NULLABLE : 0);
        u8 name_len[2];
        put_u16(name_len, static_cast<u16>(col.name.size()));
        buf.insert(buf.end(), name_len, name_len + 2);
        buf.insert(buf.end(), col.name.begin(), col.name.end());
    }

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return Result<void>::err("schema write: cannot open " + path + ": " + strerror(errno));

    ssize_t n = ::write(fd, buf.data(), buf.size());
    int save_errno = errno;
    ::close(fd);

    if (n != static_cast<ssize_t>(buf.size()))
        return Result<void>::err("schema write: short write: " + std::string(strerror(save_errno)));

    return Result<void>::ok();
}

Result<Schema> read(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return Result<Schema>::err("schema read: cannot open " + path + ": " + strerror(errno));

    u8 header[8];
    ssize_t n = ::read(fd, header, sizeof(header));
    if (n != static_cast<ssize_t>(sizeof(header))) {
        ::close(fd);
        return Result<Schema>::err("schema read: short read on header");
    }

    if (std::memcmp(header, MAGIC, 4) != 0) {
        ::close(fd);
        return Result<Schema>::err("schema read: bad magic");
    }

    u32 count = read_u32(header + 4);

    Schema schema;
    schema.reserve(count);

    for (u32 i = 0; i < count; ++i) {
        u8 col_hdr[4];
        n = ::read(fd, col_hdr, sizeof(col_hdr));
        if (n != static_cast<ssize_t>(sizeof(col_hdr))) {
            ::close(fd);
            return Result<Schema>::err("schema read: short read on column header");
        }

        TypeId type = static_cast<TypeId>(col_hdr[0]);
        bool nullable = (col_hdr[1] & FLAG_NULLABLE) != 0;
        u16 name_len = read_u16(col_hdr + 2);

        std::string name;
        name.resize(name_len);
        n = ::read(fd, name.data(), name_len);
        if (n != static_cast<ssize_t>(name_len)) {
            ::close(fd);
            return Result<Schema>::err("schema read: short read on column name");
        }

        schema.push_back({std::move(name), type, nullable});
    }

    ::close(fd);
    return Result<Schema>::ok(std::move(schema));
}

} // namespace SchemaFile
} // namespace nyx
