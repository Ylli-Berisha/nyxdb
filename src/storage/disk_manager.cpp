#include "storage/disk_manager.h"

#include "common/xxhash.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace nyx {

DiskManager::DiskManager(const std::string& path) : path_(path) {
    fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0)
        throw std::runtime_error("DiskManager: cannot open " + path + ": " + strerror(errno));

    struct stat st {};
    fstat(fd_, &st);
    next_page_id_ = static_cast<u64>(st.st_size) / PAGE_SIZE;
}

DiskManager::~DiskManager() {
    if (fd_ >= 0)
        close(fd_);
}

Result<void> DiskManager::read_page(PageId id, Page& out) {
    if (id >= next_page_id_)
        return Result<void>::err("read_page: page " + std::to_string(id) + " out of range");

    ssize_t n = pread(fd_, out.data, PAGE_SIZE, offset(id));
    if (n != static_cast<ssize_t>(PAGE_SIZE))
        return Result<void>::err("read_page: short read on page " + std::to_string(id));

    const PageHeader* hdr = out.header();

    if (hdr->page_id != id)
        return Result<void>::err("read_page: page_id mismatch on page " + std::to_string(id));

    u64 expected = xxhash64(out.payload(), PAGE_PAYLOAD_SIZE);
    if (hdr->checksum != expected)
        return Result<void>::err("read_page: checksum mismatch on page " + std::to_string(id));

    return Result<void>::ok();
}

Result<void> DiskManager::write_page(const Page& page) {
    PageId id = page.page_id();
    if (id == INVALID_PAGE_ID)
        return Result<void>::err("write_page: page has INVALID_PAGE_ID");

    Page tmp               = page;
    tmp.header()->page_id  = id;
    tmp.header()->checksum = xxhash64(tmp.payload(), PAGE_PAYLOAD_SIZE);

    ssize_t n = pwrite(fd_, tmp.data, PAGE_SIZE, offset(id));
    if (n != static_cast<ssize_t>(PAGE_SIZE))
        return Result<void>::err("write_page: short write on page " + std::to_string(id));

    return Result<void>::ok();
}

Result<PageId> DiskManager::allocate_page() {
    PageId id = next_page_id_++;

    Page blank{};
    blank.reset(id);
    auto res = write_page(blank);
    if (res.is_err())
        return Result<PageId>::err(res.error().message);

    return Result<PageId>::ok(id);
}

Result<void> DiskManager::fsync() {
    if (::fsync(fd_) != 0)
        return Result<void>::err("fsync failed: " + std::string(strerror(errno)));
    return Result<void>::ok();
}

} // namespace nyx
