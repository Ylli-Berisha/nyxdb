#include "storage/wal/wal_reader.h"

#include "common/xxhash.h"
#include "storage/wal/wal_record.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace nyx {

WalReader::WalReader(int fd) : fd_(fd) {}

WalReader::~WalReader() {
    if (fd_ >= 0)
        ::close(fd_);
}

WalReader::WalReader(WalReader&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

WalReader& WalReader::operator=(WalReader&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Result<WalReader> WalReader::open(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return Result<WalReader>::err("WalReader: cannot open " + path + ": " + strerror(errno));
    return Result<WalReader>::ok(WalReader(fd));
}

static bool read_exact(int fd, u8* buf, usize n) {
    usize done = 0;
    while (done < n) {
        ssize_t r = ::read(fd, buf + done, n - done);
        if (r <= 0)
            return false;
        done += static_cast<usize>(r);
    }
    return true;
}

Result<std::vector<WalRecord>> WalReader::read_all() {
    u8 hdr[WAL_HEADER_SIZE];
    if (!read_exact(fd_, hdr, WAL_HEADER_SIZE))
        return Result<std::vector<WalRecord>>::err("WalReader: cannot read header");
    if (std::memcmp(hdr, WAL_MAGIC, 4) != 0)
        return Result<std::vector<WalRecord>>::err("WalReader: bad magic");

    std::vector<WalRecord> records;
    u64 pos = WAL_HEADER_SIZE;

    while (true) {
        u8 type_byte;
        ssize_t r = ::read(fd_, &type_byte, 1);
        if (r == 0)
            break;
        if (r < 0)
            break;

        if (type_byte != WAL_TYPE_INSERT && type_byte != WAL_TYPE_CREATE &&
            type_byte != WAL_TYPE_DELETE && type_byte != WAL_TYPE_UPDATE)
            break;

        u64 record_start = pos;
        std::vector<u8> payload;
        payload.push_back(type_byte);
        pos += 1;

        u8 name_len_bytes[2];
        if (!read_exact(fd_, name_len_bytes, 2))
            break;
        payload.insert(payload.end(), name_len_bytes, name_len_bytes + 2);
        pos += 2;

        u16 name_len = wal_read_u16(name_len_bytes);
        std::string table_name(name_len, '\0');
        if (!read_exact(fd_, reinterpret_cast<u8*>(table_name.data()), name_len))
            break;
        payload.insert(payload.end(), table_name.begin(), table_name.end());
        pos += name_len;

        WalRecord rec;
        rec.type = static_cast<WalRecord::Type>(type_byte);
        rec.byte_offset = record_start;
        rec.table_name = table_name;

        if (type_byte == WAL_TYPE_CREATE) {
            u8 col_count_bytes[2];
            if (!read_exact(fd_, col_count_bytes, 2))
                break;
            payload.insert(payload.end(), col_count_bytes, col_count_bytes + 2);
            pos += 2;
            u16 col_count = wal_read_u16(col_count_bytes);

            bool ok = true;
            for (u16 c = 0; c < col_count && ok; ++c) {
                u8 col_hdr[6];
                if (!read_exact(fd_, col_hdr, 6)) {
                    ok = false;
                    break;
                }
                payload.insert(payload.end(), col_hdr, col_hdr + 6);
                pos += 6;

                TypeId type_id = static_cast<TypeId>(col_hdr[0]);
                bool nullable = col_hdr[1] != 0;
                u16 max_len = wal_read_u16(col_hdr + 2);
                u16 cname_len = wal_read_u16(col_hdr + 4);

                std::string cname(cname_len, '\0');
                if (!read_exact(fd_, reinterpret_cast<u8*>(cname.data()), cname_len)) {
                    ok = false;
                    break;
                }
                payload.insert(payload.end(), cname.begin(), cname.end());
                pos += cname_len;

                rec.schema.push_back({std::move(cname), type_id, nullable, max_len});
            }
            if (!ok)
                break;

        } else if (type_byte == WAL_TYPE_DELETE) {
            u8 cnt_bytes[8];
            if (!read_exact(fd_, cnt_bytes, 8))
                break;
            payload.insert(payload.end(), cnt_bytes, cnt_bytes + 8);
            pos += 8;

            u64 idx_count = wal_read_u64(cnt_bytes);
            bool ok = true;
            for (u64 i = 0; i < idx_count && ok; ++i) {
                u8 idx_bytes[8];
                if (!read_exact(fd_, idx_bytes, 8)) {
                    ok = false;
                    break;
                }
                payload.insert(payload.end(), idx_bytes, idx_bytes + 8);
                pos += 8;
                rec.row_indices.push_back(wal_read_u64(idx_bytes));
            }
            if (!ok)
                break;

        } else if (type_byte == WAL_TYPE_UPDATE) {
            u8 del_cnt_bytes[8];
            if (!read_exact(fd_, del_cnt_bytes, 8))
                break;
            payload.insert(payload.end(), del_cnt_bytes, del_cnt_bytes + 8);
            pos += 8;

            u64 del_count = wal_read_u64(del_cnt_bytes);
            bool ok = true;
            for (u64 i = 0; i < del_count && ok; ++i) {
                u8 idx_bytes[8];
                if (!read_exact(fd_, idx_bytes, 8)) {
                    ok = false;
                    break;
                }
                payload.insert(payload.end(), idx_bytes, idx_bytes + 8);
                pos += 8;
                rec.row_indices.push_back(wal_read_u64(idx_bytes));
            }
            if (!ok)
                break;

            u8 counts[6];
            if (!read_exact(fd_, counts, 6))
                break;
            payload.insert(payload.end(), counts, counts + 6);
            pos += 6;

            u32 row_count = wal_read_u32(counts);
            u16 col_count = wal_read_u16(counts + 4);

            for (u16 c = 0; c < col_count && ok; ++c) {
                u8 col_hdr[3];
                if (!read_exact(fd_, col_hdr, 3)) {
                    ok = false;
                    break;
                }
                payload.insert(payload.end(), col_hdr, col_hdr + 3);
                pos += 3;
                rec.schema.push_back(
                    {"", static_cast<TypeId>(col_hdr[0]), false, wal_read_u16(col_hdr + 1)});
            }
            if (!ok)
                break;

            for (u32 row_idx = 0; row_idx < row_count && ok; ++row_idx) {
                std::vector<Value> row;
                for (u16 c = 0; c < col_count && ok; ++c) {
                    u8 is_null;
                    if (!read_exact(fd_, &is_null, 1)) {
                        ok = false;
                        break;
                    }
                    payload.push_back(is_null);
                    pos += 1;

                    if (is_null) {
                        row.emplace_back(std::monostate{});
                        continue;
                    }

                    TypeId t = rec.schema[c].type;
                    if (t == TypeId::INT32) {
                        u8 vb[4];
                        if (!read_exact(fd_, vb, 4)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), vb, vb + 4);
                        pos += 4;
                        row.emplace_back(static_cast<i32>(wal_read_u32(vb)));
                    } else if (t == TypeId::INT64) {
                        u8 vb[8];
                        if (!read_exact(fd_, vb, 8)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), vb, vb + 8);
                        pos += 8;
                        row.emplace_back(static_cast<i64>(wal_read_u64(vb)));
                    } else if (t == TypeId::DOUBLE) {
                        u8 vb[8];
                        if (!read_exact(fd_, vb, 8)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), vb, vb + 8);
                        pos += 8;
                        u64 bits = wal_read_u64(vb);
                        f64 dv;
                        std::memcpy(&dv, &bits, 8);
                        row.emplace_back(dv);
                    } else if (t == TypeId::BOOL) {
                        u8 vb;
                        if (!read_exact(fd_, &vb, 1)) {
                            ok = false;
                            break;
                        }
                        payload.push_back(vb);
                        pos += 1;
                        row.emplace_back(vb != 0);
                    } else if (t == TypeId::DATE) {
                        u8 vb[4];
                        if (!read_exact(fd_, vb, 4)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), vb, vb + 4);
                        pos += 4;
                        row.emplace_back(Date{static_cast<i32>(wal_read_u32(vb))});
                    } else if (t == TypeId::TIMESTAMP) {
                        u8 vb[8];
                        if (!read_exact(fd_, vb, 8)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), vb, vb + 8);
                        pos += 8;
                        row.emplace_back(Timestamp{static_cast<i64>(wal_read_u64(vb))});
                    } else {
                        u8 slen_bytes[2];
                        if (!read_exact(fd_, slen_bytes, 2)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), slen_bytes, slen_bytes + 2);
                        pos += 2;
                        u16 slen = wal_read_u16(slen_bytes);
                        std::string s(slen, '\0');
                        if (!read_exact(fd_, reinterpret_cast<u8*>(s.data()), slen)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), s.begin(), s.end());
                        pos += slen;
                        row.emplace_back(std::move(s));
                    }
                }
                if (!ok)
                    break;
                rec.rows.push_back(std::move(row));
            }
            if (!ok)
                break;

        } else {
            u8 counts[6];
            if (!read_exact(fd_, counts, 6))
                break;
            payload.insert(payload.end(), counts, counts + 6);
            pos += 6;

            u32 row_count = wal_read_u32(counts);
            u16 col_count = wal_read_u16(counts + 4);

            bool ok = true;
            for (u16 c = 0; c < col_count && ok; ++c) {
                u8 col_hdr[3];
                if (!read_exact(fd_, col_hdr, 3)) {
                    ok = false;
                    break;
                }
                payload.insert(payload.end(), col_hdr, col_hdr + 3);
                pos += 3;
                rec.schema.push_back(
                    {"", static_cast<TypeId>(col_hdr[0]), false, wal_read_u16(col_hdr + 1)});
            }
            if (!ok)
                break;

            for (u32 row_idx = 0; row_idx < row_count && ok; ++row_idx) {
                std::vector<Value> row;
                for (u16 c = 0; c < col_count && ok; ++c) {
                    u8 is_null;
                    if (!read_exact(fd_, &is_null, 1)) {
                        ok = false;
                        break;
                    }
                    payload.push_back(is_null);
                    pos += 1;

                    if (is_null) {
                        row.emplace_back(std::monostate{});
                        continue;
                    }

                    TypeId t = rec.schema[c].type;
                    if (t == TypeId::INT32) {
                        u8 vb[4];
                        if (!read_exact(fd_, vb, 4)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), vb, vb + 4);
                        pos += 4;
                        row.emplace_back(static_cast<i32>(wal_read_u32(vb)));
                    } else if (t == TypeId::INT64) {
                        u8 vb[8];
                        if (!read_exact(fd_, vb, 8)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), vb, vb + 8);
                        pos += 8;
                        row.emplace_back(static_cast<i64>(wal_read_u64(vb)));
                    } else if (t == TypeId::DOUBLE) {
                        u8 vb[8];
                        if (!read_exact(fd_, vb, 8)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), vb, vb + 8);
                        pos += 8;
                        u64 bits = wal_read_u64(vb);
                        f64 dv;
                        std::memcpy(&dv, &bits, 8);
                        row.emplace_back(dv);
                    } else if (t == TypeId::BOOL) {
                        u8 vb;
                        if (!read_exact(fd_, &vb, 1)) {
                            ok = false;
                            break;
                        }
                        payload.push_back(vb);
                        pos += 1;
                        row.emplace_back(vb != 0);
                    } else if (t == TypeId::DATE) {
                        u8 vb[4];
                        if (!read_exact(fd_, vb, 4)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), vb, vb + 4);
                        pos += 4;
                        row.emplace_back(Date{static_cast<i32>(wal_read_u32(vb))});
                    } else if (t == TypeId::TIMESTAMP) {
                        u8 vb[8];
                        if (!read_exact(fd_, vb, 8)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), vb, vb + 8);
                        pos += 8;
                        row.emplace_back(Timestamp{static_cast<i64>(wal_read_u64(vb))});
                    } else {
                        u8 slen_bytes[2];
                        if (!read_exact(fd_, slen_bytes, 2)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), slen_bytes, slen_bytes + 2);
                        pos += 2;
                        u16 slen = wal_read_u16(slen_bytes);
                        std::string s(slen, '\0');
                        if (!read_exact(fd_, reinterpret_cast<u8*>(s.data()), slen)) {
                            ok = false;
                            break;
                        }
                        payload.insert(payload.end(), s.begin(), s.end());
                        pos += slen;
                        row.emplace_back(std::move(s));
                    }
                }
                if (!ok)
                    break;
                rec.rows.push_back(std::move(row));
            }
            if (!ok)
                break;
        }

        u8 cs_bytes[8];
        if (!read_exact(fd_, cs_bytes, 8))
            break;
        pos += 8;

        u64 stored = wal_read_u64(cs_bytes);
        u64 actual = xxhash64(payload.data(), payload.size());
        if (stored != actual)
            break;

        records.push_back(std::move(rec));
    }

    return Result<std::vector<WalRecord>>::ok(std::move(records));
}

} // namespace nyx
