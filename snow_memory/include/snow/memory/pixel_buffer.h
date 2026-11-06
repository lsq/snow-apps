// SPDX-License-Identifier: Apache-2.0
#ifndef SNOW_MEMORY_PIXEL_BUFFER_H
#define SNOW_MEMORY_PIXEL_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__) || defined(__unix__)
#include <sys/mman.h>
#endif

#include <atomic>

namespace snow::memory {

// Small images benefit from heap size classes. Megabyte-sized rasters instead
// own whole VM regions, so their last owner returns pages without waiting for
// allocator cache eviction. Keep this policy paired across allocation/release.
inline constexpr std::size_t kMappedPixelBufferMinimum = 1024 * 1024;

// Bytes currently owned by live VM regions. Asking the OS whether an address is
// still mapped cannot answer "was this buffer released": after munmap the address
// space is free, and a later mapping -- the allocator's own arena, another image --
// can occupy the very pages being queried, so the probe reports a leak that is not
// there (and, on the flip side, misses nothing). Count the ownership instead.
inline std::atomic<std::size_t>& liveMappedBytesCounter() noexcept {
    static std::atomic<std::size_t> counter{0};
    return counter;
}

[[nodiscard]] inline std::size_t liveMappedBytes() noexcept {
    return liveMappedBytesCounter().load(std::memory_order_relaxed);
}

inline void releasePixelBuffer(std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr)
        return;
    if (size >= kMappedPixelBufferMinimum) {
#if defined(_WIN32)
        VirtualFree(data, 0, MEM_RELEASE);
        liveMappedBytesCounter().fetch_sub(size, std::memory_order_relaxed);
        return;
#elif defined(__APPLE__) || defined(__unix__)
        munmap(data, size);
        liveMappedBytesCounter().fetch_sub(size, std::memory_order_relaxed);
        return;
#endif
    }
    std::free(data);
}

struct PixelBufferDeleter {
    std::size_t size = 0;
    void operator()(std::uint8_t* data) const noexcept {
        releasePixelBuffer(data, size);
    }
};

using PixelBuffer = std::unique_ptr<std::uint8_t[], PixelBufferDeleter>;

// Zero-initialized, exclusively owned bytes. Failure returns a null buffer;
// never fall back to a caching heap for a failed large VM allocation.
[[nodiscard]] inline PixelBuffer allocatePixelBuffer(std::size_t size) noexcept {
    std::uint8_t* data = nullptr;
    if (size == 0 || size > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()))
        return PixelBuffer(nullptr, PixelBufferDeleter{size});
    if (size >= kMappedPixelBufferMinimum) {
#if defined(_WIN32)
        data = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (data != nullptr)
            liveMappedBytesCounter().fetch_add(size, std::memory_order_relaxed);
        return PixelBuffer(data, PixelBufferDeleter{size});
#elif defined(__APPLE__) || defined(__unix__)
        void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapping != MAP_FAILED) {
            data = static_cast<std::uint8_t*>(mapping);
            liveMappedBytesCounter().fetch_add(size, std::memory_order_relaxed);
        }
        return PixelBuffer(data, PixelBufferDeleter{size});
#endif
    }
    data = static_cast<std::uint8_t*>(std::calloc(size, 1));
    return PixelBuffer(data, PixelBufferDeleter{size});
}
} // namespace snow::memory
#endif
