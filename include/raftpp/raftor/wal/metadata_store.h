#pragma once

#include <filesystem>

#include "raftpp/core/error.h"
#include "raftpp/core/raftpp.pb.h"

namespace raftpp::raftor::wal {

// Metadata stored in the metadata file
struct WALMetadata {
    HardState hard_state;
    ConfState conf_state;
    uint64_t first_index = 1;
    uint64_t snapshot_index = 0;
    uint64_t snapshot_term = 0;
};

// Handles atomic persistence of WAL metadata (HardState, ConfState, indices)
// Uses rename-based atomic update to ensure crash safety
class MetadataStore {
  public:
    explicit MetadataStore(const std::filesystem::path& dir);

    // Initialize the metadata store
    // Creates the metadata file if it doesn't exist
    [[nodiscard]] Result<void> Initialize();

    // Load metadata from disk
    // Returns default metadata if file doesn't exist
    [[nodiscard]] Result<WALMetadata> Load();

    // Save metadata atomically using rename
    [[nodiscard]] Result<void> Save(const WALMetadata& meta);

    // Check if metadata file exists
    [[nodiscard]] bool Exists() const;

  private:
    // Atomic write implementation
    [[nodiscard]] Result<void> AtomicWrite(const std::vector<uint8_t>& data);

    // Serialize metadata to bytes
    [[nodiscard]] std::vector<uint8_t> Serialize(const WALMetadata& meta) const;

    // Deserialize metadata from bytes
    [[nodiscard]] Result<WALMetadata> Deserialize(const std::vector<uint8_t>& data) const;

    std::filesystem::path path_;      // metadata file path
    std::filesystem::path tmp_path_;  // temporary file path for atomic write
};

}  // namespace raftpp::raftor::wal
