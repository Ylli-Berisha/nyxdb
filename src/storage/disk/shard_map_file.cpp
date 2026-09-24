#include "storage/disk/shard_map_file.h"

#include "executor/expression.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <variant>

namespace nyx {

static constexpr u8 MAGIC[4] = {'N', 'Y', 'X', 'P'};
static constexpr u16 VERSION = 0x0001;

template <typename T> static int cmp_val(const T& a, const T& b) {
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

static int compare_values(const Value& a, const Value& b) {
    return std::visit(
        [&](const auto& av) -> int {
            using T = std::decay_t<decltype(av)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return 0;
            else if constexpr (std::is_same_v<T, i32>)
                return cmp_val(av, std::get<i32>(b));
            else if constexpr (std::is_same_v<T, i64>)
                return cmp_val(av, std::get<i64>(b));
            else if constexpr (std::is_same_v<T, f64>)
                return cmp_val(av, std::get<f64>(b));
            else if constexpr (std::is_same_v<T, std::string>)
                return cmp_val(av, std::get<std::string>(b));
            else if constexpr (std::is_same_v<T, bool>)
                return cmp_val(static_cast<int>(av), static_cast<int>(std::get<bool>(b)));
            else if constexpr (std::is_same_v<T, Date>)
                return cmp_val(av.days, std::get<Date>(b).days);
            else if constexpr (std::is_same_v<T, Timestamp>)
                return cmp_val(av.micros, std::get<Timestamp>(b).micros);
            return 0;
        },
        a);
}

size_t shard_for_value(const ShardMapMeta& meta, const Value& val) {
    for (size_t i = 0; i < meta.partitions.size(); ++i) {
        const auto& p = meta.partitions[i];
        if (p.is_maxvalue)
            return i;
        if (is_null(val))
            continue;
        if (compare_values(val, p.upper_bound) < 0)
            return i;
    }
    return meta.partitions.size() - 1;
}

std::vector<size_t> prune_partitions(const ShardMapMeta& meta, u8 predicate_col_idx,
                                     int op_kind_int, const Value& literal) {
    std::vector<size_t> all;
    for (size_t i = 0; i < meta.partitions.size(); ++i)
        all.push_back(i);

    if (predicate_col_idx != meta.partition_col_idx)
        return all;

    auto op = static_cast<BinaryOpKind>(op_kind_int);
    std::vector<size_t> out;

    bool have_prev = false;
    Value prev_upper;

    for (size_t i = 0; i < meta.partitions.size(); ++i) {
        const auto& p = meta.partitions[i];
        if (p.is_maxvalue) {
            out.push_back(i);
            break;
        }

        switch (op) {
        case BinaryOpKind::EQ: {
            bool lower_ok = !have_prev || compare_values(prev_upper, literal) <= 0;
            bool upper_ok = compare_values(literal, p.upper_bound) < 0;
            if (lower_ok && upper_ok)
                out.push_back(i);
        } break;
        case BinaryOpKind::LT:
        case BinaryOpKind::LE:
            if (!have_prev || compare_values(prev_upper, literal) < 0)
                out.push_back(i);
            break;
        case BinaryOpKind::GT:
        case BinaryOpKind::GE:
            if (compare_values(p.upper_bound, literal) > 0)
                out.push_back(i);
            break;
        case BinaryOpKind::NE:
            out.push_back(i);
            break;
        default:
            out.push_back(i);
            break;
        }

        have_prev = true;
        prev_upper = p.upper_bound;
    }
    return out.empty() ? all : out;
}

static void put_u8(std::vector<u8>& buf, u8 v) {
    buf.push_back(v);
}

static void put_u16(std::vector<u8>& buf, u16 v) {
    buf.push_back(static_cast<u8>(v & 0xFF));
    buf.push_back(static_cast<u8>((v >> 8) & 0xFF));
}

static void put_u32(std::vector<u8>& buf, u32 v) {
    buf.push_back(static_cast<u8>(v & 0xFF));
    buf.push_back(static_cast<u8>((v >> 8) & 0xFF));
    buf.push_back(static_cast<u8>((v >> 16) & 0xFF));
    buf.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

static void put_u64(std::vector<u8>& buf, u64 v) {
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
}

static void put_str(std::vector<u8>& buf, const std::string& s) {
    buf.push_back(static_cast<u8>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

static void put_value(std::vector<u8>& buf, const Value& v) {
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                put_u8(buf, static_cast<u8>(TypeId::INT32));
                put_u32(buf, 0);
            } else if constexpr (std::is_same_v<T, i32>) {
                put_u8(buf, static_cast<u8>(TypeId::INT32));
                put_u32(buf, static_cast<u32>(x));
            } else if constexpr (std::is_same_v<T, i64>) {
                put_u8(buf, static_cast<u8>(TypeId::INT64));
                put_u64(buf, static_cast<u64>(x));
            } else if constexpr (std::is_same_v<T, f64>) {
                put_u8(buf, static_cast<u8>(TypeId::DOUBLE));
                u64 bits;
                std::memcpy(&bits, &x, 8);
                put_u64(buf, bits);
            } else if constexpr (std::is_same_v<T, std::string>) {
                put_u8(buf, static_cast<u8>(TypeId::VARCHAR));
                put_u16(buf, static_cast<u16>(x.size()));
                buf.insert(buf.end(), x.begin(), x.end());
            } else if constexpr (std::is_same_v<T, bool>) {
                put_u8(buf, static_cast<u8>(TypeId::BOOL));
                put_u8(buf, x ? 1u : 0u);
            } else if constexpr (std::is_same_v<T, Date>) {
                put_u8(buf, static_cast<u8>(TypeId::DATE));
                put_u32(buf, static_cast<u32>(x.days));
            } else if constexpr (std::is_same_v<T, Timestamp>) {
                put_u8(buf, static_cast<u8>(TypeId::TIMESTAMP));
                put_u64(buf, static_cast<u64>(x.micros));
            }
        },
        v);
}

static u8 get_u8(const u8* p) {
    return *p;
}
static u16 get_u16(const u8* p) {
    return static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8);
}
static u32 get_u32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
static u64 get_u64(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<u64>(p[i]) << (8 * i);
    return v;
}

namespace ShardMapFile {

Result<void> write(const std::string& path, const ShardMapMeta& meta) {
    std::vector<u8> buf;
    buf.insert(buf.end(), MAGIC, MAGIC + 4);
    put_u16(buf, VERSION);
    put_u8(buf, meta.partition_col_idx);
    put_str(buf, meta.partition_col);
    put_u16(buf, static_cast<u16>(meta.partitions.size()));

    for (const auto& p : meta.partitions) {
        put_str(buf, p.name);
        put_u8(buf, p.is_maxvalue ? 1u : 0u);
        if (!p.is_maxvalue)
            put_value(buf, p.upper_bound);
        put_str(buf, p.node_addr);
    }

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return Result<void>::err("shard_map write: cannot open " + path + ": " + strerror(errno));
    ssize_t n = ::write(fd, buf.data(), buf.size());
    int saved = errno;
    ::close(fd);
    if (n != static_cast<ssize_t>(buf.size()))
        return Result<void>::err("shard_map write: short write: " + std::string(strerror(saved)));
    return Result<void>::ok();
}

Result<ShardMapMeta> read(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return Result<ShardMapMeta>::err("shard_map read: cannot open " + path + ": " +
                                         strerror(errno));

    std::vector<u8> raw;
    {
        u8 tmp[4096];
        ssize_t n;
        while ((n = ::read(fd, tmp, sizeof(tmp))) > 0)
            raw.insert(raw.end(), tmp, tmp + n);
        ::close(fd);
    }

    const u8* p = raw.data();
    const u8* end = p + raw.size();

    auto need = [&](usize n) -> bool { return (end - p) >= static_cast<ptrdiff_t>(n); };

    if (!need(4) || std::memcmp(p, MAGIC, 4) != 0)
        return Result<ShardMapMeta>::err("shard_map read: bad magic");
    p += 4;

    if (!need(2))
        return Result<ShardMapMeta>::err("shard_map read: truncated");
    p += 2;

    if (!need(1))
        return Result<ShardMapMeta>::err("shard_map read: truncated");
    ShardMapMeta meta;
    meta.partition_col_idx = get_u8(p++);

    if (!need(1))
        return Result<ShardMapMeta>::err("shard_map read: truncated");
    u8 col_name_len = get_u8(p++);
    if (!need(col_name_len))
        return Result<ShardMapMeta>::err("shard_map read: truncated");
    meta.partition_col.assign(reinterpret_cast<const char*>(p), col_name_len);
    p += col_name_len;

    if (!need(2))
        return Result<ShardMapMeta>::err("shard_map read: truncated");
    u16 count = get_u16(p);
    p += 2;

    meta.partitions.reserve(count);
    for (u16 i = 0; i < count; ++i) {
        PartitionDef pd;

        if (!need(1))
            return Result<ShardMapMeta>::err("shard_map read: truncated");
        u8 nlen = get_u8(p++);
        if (!need(nlen))
            return Result<ShardMapMeta>::err("shard_map read: truncated");
        pd.name.assign(reinterpret_cast<const char*>(p), nlen);
        p += nlen;

        if (!need(1))
            return Result<ShardMapMeta>::err("shard_map read: truncated");
        pd.is_maxvalue = get_u8(p++) != 0;

        if (!pd.is_maxvalue) {
            if (!need(1))
                return Result<ShardMapMeta>::err("shard_map read: truncated");
            auto tid = static_cast<TypeId>(get_u8(p++));
            switch (tid) {
            case TypeId::INT32:
                if (!need(4))
                    return Result<ShardMapMeta>::err("shard_map read: truncated");
                pd.upper_bound = static_cast<i32>(get_u32(p));
                p += 4;
                break;
            case TypeId::INT64:
                if (!need(8))
                    return Result<ShardMapMeta>::err("shard_map read: truncated");
                pd.upper_bound = static_cast<i64>(get_u64(p));
                p += 8;
                break;
            case TypeId::DOUBLE: {
                if (!need(8))
                    return Result<ShardMapMeta>::err("shard_map read: truncated");
                u64 bits = get_u64(p);
                p += 8;
                f64 dv;
                std::memcpy(&dv, &bits, 8);
                pd.upper_bound = dv;
                break;
            }
            case TypeId::VARCHAR: {
                if (!need(2))
                    return Result<ShardMapMeta>::err("shard_map read: truncated");
                u16 slen = get_u16(p);
                p += 2;
                if (!need(slen))
                    return Result<ShardMapMeta>::err("shard_map read: truncated");
                pd.upper_bound = std::string(reinterpret_cast<const char*>(p), slen);
                p += slen;
                break;
            }
            case TypeId::BOOL:
                if (!need(1))
                    return Result<ShardMapMeta>::err("shard_map read: truncated");
                pd.upper_bound = get_u8(p++) != 0;
                break;
            case TypeId::DATE:
                if (!need(4))
                    return Result<ShardMapMeta>::err("shard_map read: truncated");
                pd.upper_bound = Date{static_cast<i32>(get_u32(p))};
                p += 4;
                break;
            case TypeId::TIMESTAMP:
                if (!need(8))
                    return Result<ShardMapMeta>::err("shard_map read: truncated");
                pd.upper_bound = Timestamp{static_cast<i64>(get_u64(p))};
                p += 8;
                break;
            default:
                return Result<ShardMapMeta>::err("shard_map read: unknown type");
            }
        }

        if (!need(1))
            return Result<ShardMapMeta>::err("shard_map read: truncated");
        u8 alen = get_u8(p++);
        if (!need(alen))
            return Result<ShardMapMeta>::err("shard_map read: truncated");
        pd.node_addr.assign(reinterpret_cast<const char*>(p), alen);
        p += alen;

        meta.partitions.push_back(std::move(pd));
    }

    return Result<ShardMapMeta>::ok(std::move(meta));
}

} // namespace ShardMapFile
} // namespace nyx
