#pragma once
#include "common/types.hpp"
#include <cstddef>

namespace arb {

// Memory-maps a binary file of packed MarketTick records.
// File must be a multiple of sizeof(MarketTick) = 64 bytes.
// After open(), data() returns a pointer to the first tick; count() is the tick count.
// RAII: destructor calls close(). Non-copyable.
class FileSource {
    int    fd_        = -1;
    void*  mapped_    = nullptr;
    size_t file_size_ = 0;

public:
    FileSource() = default;
    ~FileSource() { close(); }
    FileSource(const FileSource&)            = delete;
    FileSource& operator=(const FileSource&) = delete;

    // Returns false and prints to stderr on failure (cold path — no exceptions from here,
    // but caller should check and exit).
    bool open(const char* path) noexcept;

    [[nodiscard]] const MarketTick* data() const noexcept;
    [[nodiscard]] size_t count() const noexcept;    // number of MarketTick records

    void close() noexcept;
};

} // namespace arb
