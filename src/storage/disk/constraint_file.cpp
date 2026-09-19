#include "storage/disk/constraint_file.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace nyx {
namespace ConstraintFile {

static constexpr u8 MAGIC[4] = {'N', 'Y', 'X', 'K'};

static void put_u32(u8* dst, u32 v) {
    dst[0] = static_cast<u8>(v & 0xFF);
    dst[1] = static_cast<u8>((v >> 8) & 0xFF);
    dst[2] = static_cast<u8>((v >> 16) & 0xFF);
    dst[3] = static_cast<u8>((v >> 24) & 0xFF);
}

static u32 read_u32(const u8* src) {
    return static_cast<u32>(src[0]) | (static_cast<u32>(src[1]) << 8) |
           (static_cast<u32>(src[2]) << 16) | (static_cast<u32>(src[3]) << 24);
}

Result<void> write(const std::string& path, const std::vector<ConstraintMeta>& constraints) {
    std::vector<u8> buf;
    buf.insert(buf.end(), MAGIC, MAGIC + 4);
    u8 cnt[4];
    put_u32(cnt, static_cast<u32>(constraints.size()));
    buf.insert(buf.end(), cnt, cnt + 4);

    for (const auto& c : constraints) {
        buf.push_back(static_cast<u8>(c.kind));
        buf.push_back(static_cast<u8>(c.name.size()));
        buf.insert(buf.end(), c.name.begin(), c.name.end());
        buf.push_back(static_cast<u8>(c.col_indices.size()));
        buf.insert(buf.end(), c.col_indices.begin(), c.col_indices.end());
    }

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return Result<void>::err("constraint write: cannot open " + path + ": " + strerror(errno));

    ssize_t n = ::write(fd, buf.data(), buf.size());
    int save_errno = errno;
    ::close(fd);

    if (n != static_cast<ssize_t>(buf.size()))
        return Result<void>::err("constraint write: short write: " +
                                 std::string(strerror(save_errno)));
    return Result<void>::ok();
}

Result<std::vector<ConstraintMeta>> read(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return Result<std::vector<ConstraintMeta>>::err("constraint read: cannot open " + path +
                                                        ": " + strerror(errno));

    u8 header[8];
    ssize_t n = ::read(fd, header, sizeof(header));
    if (n != static_cast<ssize_t>(sizeof(header))) {
        ::close(fd);
        return Result<std::vector<ConstraintMeta>>::err("constraint read: short read on header");
    }

    if (std::memcmp(header, MAGIC, 4) != 0) {
        ::close(fd);
        return Result<std::vector<ConstraintMeta>>::err("constraint read: bad magic");
    }

    u32 count = read_u32(header + 4);
    std::vector<ConstraintMeta> out;
    out.reserve(count);

    for (u32 i = 0; i < count; ++i) {
        u8 kind_byte;
        if (::read(fd, &kind_byte, 1) != 1) {
            ::close(fd);
            return Result<std::vector<ConstraintMeta>>::err("constraint read: short read on kind");
        }

        u8 name_len;
        if (::read(fd, &name_len, 1) != 1) {
            ::close(fd);
            return Result<std::vector<ConstraintMeta>>::err(
                "constraint read: short read on name_len");
        }

        std::string name(name_len, '\0');
        if (name_len > 0 && ::read(fd, name.data(), name_len) != static_cast<ssize_t>(name_len)) {
            ::close(fd);
            return Result<std::vector<ConstraintMeta>>::err("constraint read: short read on name");
        }

        u8 col_count;
        if (::read(fd, &col_count, 1) != 1) {
            ::close(fd);
            return Result<std::vector<ConstraintMeta>>::err(
                "constraint read: short read on col_count");
        }

        std::vector<u8> cols(col_count);
        if (col_count > 0 &&
            ::read(fd, cols.data(), col_count) != static_cast<ssize_t>(col_count)) {
            ::close(fd);
            return Result<std::vector<ConstraintMeta>>::err(
                "constraint read: short read on col_indices");
        }

        out.push_back({static_cast<ConstraintKind>(kind_byte), std::move(name), std::move(cols)});
    }

    ::close(fd);
    return Result<std::vector<ConstraintMeta>>::ok(std::move(out));
}

} // namespace ConstraintFile
} // namespace nyx
