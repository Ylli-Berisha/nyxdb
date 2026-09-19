#include "storage/disk/schema.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace nyx {
namespace SchemaFile {

static constexpr u8 MAGIC[4] = {'N', 'Y', 'X', '3'};
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

static void append_default(std::vector<u8>& buf, const Value& v) {
    buf.push_back(static_cast<u8>(std::visit(
        [](const auto& x) -> TypeId {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, i32>)
                return TypeId::INT32;
            else if constexpr (std::is_same_v<T, i64>)
                return TypeId::INT64;
            else if constexpr (std::is_same_v<T, f64>)
                return TypeId::DOUBLE;
            else if constexpr (std::is_same_v<T, std::string>)
                return TypeId::VARCHAR;
            else if constexpr (std::is_same_v<T, bool>)
                return TypeId::BOOL;
            else if constexpr (std::is_same_v<T, Date>)
                return TypeId::DATE;
            else if constexpr (std::is_same_v<T, Timestamp>)
                return TypeId::TIMESTAMP;
            else
                return TypeId::INT32;
        },
        v)));

    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            u8 tmp[8];
            if constexpr (std::is_same_v<T, i32>) {
                std::memcpy(tmp, &x, 4);
                buf.insert(buf.end(), tmp, tmp + 4);
            } else if constexpr (std::is_same_v<T, i64>) {
                std::memcpy(tmp, &x, 8);
                buf.insert(buf.end(), tmp, tmp + 8);
            } else if constexpr (std::is_same_v<T, f64>) {
                std::memcpy(tmp, &x, 8);
                buf.insert(buf.end(), tmp, tmp + 8);
            } else if constexpr (std::is_same_v<T, std::string>) {
                u16 len = static_cast<u16>(x.size());
                put_u16(tmp, len);
                buf.insert(buf.end(), tmp, tmp + 2);
                buf.insert(buf.end(), x.begin(), x.end());
            } else if constexpr (std::is_same_v<T, bool>) {
                buf.push_back(x ? 1u : 0u);
            } else if constexpr (std::is_same_v<T, Date>) {
                std::memcpy(tmp, &x.days, 4);
                buf.insert(buf.end(), tmp, tmp + 4);
            } else if constexpr (std::is_same_v<T, Timestamp>) {
                std::memcpy(tmp, &x.micros, 8);
                buf.insert(buf.end(), tmp, tmp + 8);
            }
        },
        v);
}

static bool read_default(int fd, Value& out) {
    u8 type_byte;
    if (::read(fd, &type_byte, 1) != 1)
        return false;
    TypeId t = static_cast<TypeId>(type_byte);
    u8 tmp[8];
    switch (t) {
    case TypeId::INT32: {
        if (::read(fd, tmp, 4) != 4)
            return false;
        i32 v;
        std::memcpy(&v, tmp, 4);
        out = v;
        return true;
    }
    case TypeId::INT64: {
        if (::read(fd, tmp, 8) != 8)
            return false;
        i64 v;
        std::memcpy(&v, tmp, 8);
        out = v;
        return true;
    }
    case TypeId::DOUBLE: {
        if (::read(fd, tmp, 8) != 8)
            return false;
        f64 v;
        std::memcpy(&v, tmp, 8);
        out = v;
        return true;
    }
    case TypeId::VARCHAR: {
        if (::read(fd, tmp, 2) != 2)
            return false;
        u16 len = read_u16(tmp);
        std::string s(len, '\0');
        if (::read(fd, s.data(), len) != static_cast<ssize_t>(len))
            return false;
        out = std::move(s);
        return true;
    }
    case TypeId::BOOL: {
        u8 b;
        if (::read(fd, &b, 1) != 1)
            return false;
        out = (b != 0);
        return true;
    }
    case TypeId::DATE: {
        if (::read(fd, tmp, 4) != 4)
            return false;
        i32 v;
        std::memcpy(&v, tmp, 4);
        out = Date{v};
        return true;
    }
    case TypeId::TIMESTAMP: {
        if (::read(fd, tmp, 8) != 8)
            return false;
        i64 v;
        std::memcpy(&v, tmp, 8);
        out = Timestamp{v};
        return true;
    }
    default:
        return false;
    }
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
        u8 max_len_bytes[2];
        put_u16(max_len_bytes, col.max_len);
        buf.insert(buf.end(), max_len_bytes, max_len_bytes + 2);
        u8 name_len[2];
        put_u16(name_len, static_cast<u16>(col.name.size()));
        buf.insert(buf.end(), name_len, name_len + 2);
        buf.insert(buf.end(), col.name.begin(), col.name.end());

        if (col.default_value.has_value()) {
            buf.push_back(1u);
            append_default(buf, *col.default_value);
        } else {
            buf.push_back(0u);
        }
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
        u8 col_hdr[6];
        n = ::read(fd, col_hdr, sizeof(col_hdr));
        if (n != static_cast<ssize_t>(sizeof(col_hdr))) {
            ::close(fd);
            return Result<Schema>::err("schema read: short read on column header");
        }

        TypeId type = static_cast<TypeId>(col_hdr[0]);
        bool nullable = (col_hdr[1] & FLAG_NULLABLE) != 0;
        u16 max_len = read_u16(col_hdr + 2);
        u16 name_len = read_u16(col_hdr + 4);

        std::string name;
        name.resize(name_len);
        n = ::read(fd, name.data(), name_len);
        if (n != static_cast<ssize_t>(name_len)) {
            ::close(fd);
            return Result<Schema>::err("schema read: short read on column name");
        }

        u8 has_default;
        if (::read(fd, &has_default, 1) != 1) {
            ::close(fd);
            return Result<Schema>::err("schema read: short read on has_default");
        }

        std::optional<Value> def;
        if (has_default) {
            Value v;
            if (!read_default(fd, v)) {
                ::close(fd);
                return Result<Schema>::err("schema read: short read on default value");
            }
            def = std::move(v);
        }

        schema.push_back({std::move(name), type, nullable, max_len, std::move(def)});
    }

    ::close(fd);
    return Result<Schema>::ok(std::move(schema));
}

} // namespace SchemaFile
} // namespace nyx
