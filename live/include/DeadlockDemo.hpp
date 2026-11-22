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
    {"08-reader-writer.txt", "Reader and writer"},
    {"09-catalog-metadata.txt", "Catalog and metadata"},
    {"10-network-disk.txt", "Network and disk"},
}};

struct RecoveryDemoResult {
    DeadlockReport detection;
    int victim = 0;
    int completed = 0;
    bool recovered = false;
    std::array<std::thread::id, 2> workers;
    std::array<ChecksumResult, 2> checksums;
};

RecoveryDemoResult runLockedChecksumFile(const std::filesystem::path& file);
}
