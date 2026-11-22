#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "BufferPool.hpp"

namespace bq {
inline constexpr std::uint64_t defaultMemoryMiB = 64;
inline constexpr std::uint64_t bytesPerMiB = 1024 * 1024;
inline constexpr std::size_t checksumQuantum = bytesPerMiB;

struct FileInfo {
    std::string name;
    std::uintmax_t size;
};

struct ChecksumResult {
    int pid = -1;
    int exitCode = -1;
    std::uint32_t checksum = 0;
    std::uintmax_t bytes = 0;
    std::uint64_t addressSpaceLimitBytes = 0;
    std::optional<std::uint64_t> peakRssKiB;
    std::string error;
};

struct ChecksumProgress {
    std::uintmax_t offset = 0, size = 0, device = 0, inode = 0;
    std::int64_t modifiedSeconds = 0, modifiedNanoseconds = 0;
    std::uint32_t crc = 0;
    bool initialized = false;
};

class Checksum {
    std::filesystem::path root_;
    int directory_;
    mutable BufferPool pool_;
    static ChecksumResult runInput(int descriptor, std::chrono::milliseconds timeout,
                                   std::uint64_t addressSpaceBytes, BufferPool& pool,
                                   const std::atomic<bool>* stopped = nullptr);
    static std::optional<ChecksumResult> sliceInput(int descriptor, ChecksumProgress&, BufferPool&);

public:
    explicit Checksum(const std::filesystem::path& folder);
    ~Checksum();
    Checksum(const Checksum&) = delete;
    Checksum& operator=(const Checksum&) = delete;

    std::vector<FileInfo> files() const;
    std::optional<std::uintmax_t> size(const std::string& name) const;
    static std::optional<std::uintmax_t> pathSize(const std::filesystem::path& path);
    static bool validName(const std::string& name);
    std::optional<ChecksumResult> slice(const std::string&, ChecksumProgress&) const;
    static std::optional<ChecksumResult> slicePath(const std::filesystem::path&, ChecksumProgress&,
                                                   BufferPool&);
    ChecksumResult run(const std::string& name,
                       std::chrono::milliseconds timeout = std::chrono::minutes(2),
                       std::uint64_t addressSpaceBytes = defaultMemoryMiB * bytesPerMiB) const;
    static ChecksumResult runPath(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout = std::chrono::minutes(2),
        std::uint64_t addressSpaceBytes = defaultMemoryMiB * bytesPerMiB,
        BufferPool* pool = nullptr, const std::atomic<bool>* stopped = nullptr);
};
}
