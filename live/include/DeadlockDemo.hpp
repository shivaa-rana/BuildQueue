#pragma once

#include "DeadlockDetector.hpp"
#include "Checksum.hpp"

#include <array>
#include <filesystem>

namespace bq {
struct RecoveryCase {
    const char* directory;
    const char* title;
};

inline constexpr std::array<RecoveryCase, 5> recoveryCases{{
    {"01-printer-and-scanner", "Printer and scanner"},
    {"02-input-and-output-buffers", "Input and output buffers"},
    {"03-inventory-and-orders", "Inventory and orders"},
    {"04-accounts-and-ledger", "Accounts and ledger"},
    {"05-backup-and-index", "Backup and index"},
}};

struct RecoveryDemoResult {
    DeadlockReport detection;
    int victim = 0;
    int completed = 0;
    bool recovered = false;
    std::array<std::thread::id, 2> workers;
    std::array<ChecksumResult, 2> checksums;
};

RecoveryDemoResult runLockedChecksumPair(const std::filesystem::path& firstFile,
                                         const std::filesystem::path& secondFile);
}
