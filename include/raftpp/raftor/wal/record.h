#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace raftpp::raftor::wal {

// Magic numbers for file format identification
constexpr uint32_t kSegmentMagic = 0x57414C31;   // "WAL1"
constexpr uint32_t kMetadataMagic = 0x4D455441;  // "META"
constexpr uint32_t kFormatVersion = 1;

// Record types stored in WAL
enum class RecordType : uint8_t {
    Entry = 1,       // Single raft Entry
    EntryBatch = 2,  // Multiple entries in one record
    HardState = 3,   // Hard state update
};

// Segment file header (32 bytes, aligned)
struct SegmentHeader {
    uint32_t magic = kSegmentMagic;
    uint32_t version = kFormatVersion;
    uint64_t segment_id = 0;
    uint64_t first_index = 0;
    uint64_t reserved = 0;

    [[nodiscard]] bool IsValid() const {
        return magic == kSegmentMagic && version == kFormatVersion;
    }

    void Serialize(std::span<uint8_t, 32> out) const {
        std::memcpy(out.data(), this, sizeof(*this));
    }

    static SegmentHeader Deserialize(std::span<const uint8_t, 32> in) {
        SegmentHeader h;
        std::memcpy(&h, in.data(), sizeof(h));
        return h;
    }
};

static_assert(sizeof(SegmentHeader) == 32);

// Record header (16 bytes, aligned)
struct RecordHeader {
    uint32_t crc = 0;       // CRC32C of (type + reserved + length + payload)
    uint8_t type = 0;       // RecordType
    uint8_t flags = 0;      // Reserved for future use (compression, etc.)
    uint16_t reserved = 0;  // Padding
    uint32_t length = 0;    // Payload length
    uint32_t padding = 0;   // Padding bytes for 8-byte alignment

    void Serialize(std::span<uint8_t, 16> out) const {
        std::memcpy(out.data(), this, sizeof(*this));
    }

    static RecordHeader Deserialize(std::span<const uint8_t, 16> in) {
        RecordHeader h;
        std::memcpy(&h, in.data(), sizeof(h));
        return h;
    }

    // Calculate padding needed to align record to 8 bytes
    [[nodiscard]] static uint32_t CalculatePadding(uint32_t length) {
        constexpr uint32_t alignment = 8;
        uint32_t total = sizeof(RecordHeader) + length;
        uint32_t remainder = total % alignment;
        return remainder == 0 ? 0 : alignment - remainder;
    }

    // Total size of the record including header, payload, and padding
    [[nodiscard]] uint32_t TotalSize() const { return sizeof(RecordHeader) + length + padding; }
};

static_assert(sizeof(RecordHeader) == 16);

// Metadata file header
struct MetadataHeader {
    uint32_t magic = kMetadataMagic;
    uint32_t version = kFormatVersion;
    uint32_t crc = 0;  // CRC of everything after this field
    uint32_t reserved = 0;

    [[nodiscard]] bool IsValid() const {
        return magic == kMetadataMagic && version == kFormatVersion;
    }
};

static_assert(sizeof(MetadataHeader) == 16);

// Metadata content (follows MetadataHeader)
struct MetadataContent {
    uint64_t first_index = 0;
    uint64_t snapshot_index = 0;
    uint64_t snapshot_term = 0;
    // Followed by serialized HardState and ConfState
};

static_assert(sizeof(MetadataContent) == 24);

// Helper to build a record with CRC
class RecordBuilder {
  public:
    RecordBuilder() = default;

    void SetType(RecordType type) { type_ = type; }

    void SetPayload(std::span<const uint8_t> payload);
    void SetPayload(const std::string& payload);

    // Build the complete record with CRC
    [[nodiscard]] std::vector<uint8_t> Build() const;

  private:
    RecordType type_ = RecordType::Entry;
    std::vector<uint8_t> payload_;
};

// Helper to parse records
class RecordParser {
  public:
    explicit RecordParser(std::span<const uint8_t> data);

    [[nodiscard]] bool IsValid() const { return valid_; }

    [[nodiscard]] RecordHeader Header() const { return header_; }

    [[nodiscard]] std::span<const uint8_t> Payload() const { return payload_; }

    [[nodiscard]] RecordType Type() const { return static_cast<RecordType>(header_.type); }

  private:
    RecordHeader header_;
    std::span<const uint8_t> payload_;
    bool valid_ = false;
};

}  // namespace raftpp::raftor::wal
