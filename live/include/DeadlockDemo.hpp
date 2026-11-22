#pragma once

#include "DeadlockDetector.hpp"
#include "Checksum.hpp"

#include <array>
#include <filesystem>

namespace bq {
struct RecoveryCase {
    const char* filename;
    const char* title;
};

inline constexpr std::array<RecoveryCase, 10> recoveryCases{{
    {"01-printer-scanner.txt", "Printer and scanner"},
    {"02-input-output-buffers.txt", "Input and output buffers"},
    {"03-inventory-orders.txt", "Inventory and orders"},
    {"04-accounts-ledger.txt", "Accounts and ledger"},
    {"05-backup-index.txt", "Backup and index"},
    {"06-cache-journal.txt", "Cache and journal"},
    {"07-compiler-linker.txt", "Compiler and linker"},
    {"08-reader-writer.txt", "Reader and writer (3 jobs)"},
    {"09-catalog-metadata.txt", "Catalog and metadata (4 jobs)"},
    {"10-network-disk.txt", "Network and disk (8 jobs)"},
}};

struct RecoveryDemoResult {
    DeadlockReport detection;
    int victim = 0;
    int completed = 0;
    bool recovered = false;
    std::vector<std::thread::id> workers;
    std::vector<ChecksumResult> checksums;
    bool timedOut = false;
    bool stopped = false;
};

using CycleObserver = std::function<void(const RecoveryDemoResult&)>;
RecoveryDemoResult runLockedChecksumFile(
    const std::filesystem::path& file, std::chrono::milliseconds waitForTimeout = {},
    const std::atomic<bool>* stopped = nullptr, const CycleObserver& onCycle = {});
}
