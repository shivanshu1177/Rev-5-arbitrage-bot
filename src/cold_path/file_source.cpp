#include "cold_path/file_source.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

namespace arb {

bool FileSource::open(const char* path) noexcept {
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        std::perror(path);
        return false;
    }

    struct stat st{};
    if (::fstat(fd_, &st) < 0) {
        std::perror("fstat");
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    file_size_ = static_cast<size_t>(st.st_size);

    if (file_size_ == 0) {
        std::fprintf(stderr, "FileSource: empty file: %s\n", path);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (file_size_ % sizeof(MarketTick) != 0) {
        std::fprintf(stderr,
            "FileSource: file size %zu is not a multiple of sizeof(MarketTick)=%zu\n",
            file_size_, sizeof(MarketTick));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    mapped_ = ::mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped_ == MAP_FAILED) {
        std::perror("mmap");
        mapped_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Advise the kernel to prefetch sequentially — maximises memory bandwidth on M2.
    ::madvise(mapped_, file_size_, MADV_SEQUENTIAL);

    return true;
}

const MarketTick* FileSource::data() const noexcept {
    return static_cast<const MarketTick*>(mapped_);
}

size_t FileSource::count() const noexcept {
    return (mapped_ != nullptr) ? (file_size_ / sizeof(MarketTick)) : 0;
}

void FileSource::close() noexcept {
    if (mapped_ != nullptr) {
        ::munmap(mapped_, file_size_);
        mapped_    = nullptr;
        file_size_ = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace arb
