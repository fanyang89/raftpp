#include "raftor/wal/record.h"

#include "raftor/wal/crc32c.h"

namespace raftpp::wal {

void RecordBuilder::SetPayload(std::span<const uint8_t> payload) {
    payload_.assign(payload.begin(), payload.end());
}

void RecordBuilder::SetPayload(const std::string& payload) {
    payload_.assign(payload.begin(), payload.end());
}

std::vector<uint8_t> RecordBuilder::Build() const {
    RecordHeader header;
    header.type = static_cast<uint8_t>(type_);
    header.length = static_cast<uint32_t>(payload_.size());
    header.padding = RecordHeader::CalculatePadding(header.length);

    // Calculate total size
    size_t total_size = sizeof(RecordHeader) + payload_.size() + header.padding;
    std::vector<uint8_t> result(total_size, 0);

    // Compute CRC over: type, flags, reserved, length, padding, payload
    // (everything except the crc field itself)
    CRC32C crc;
    crc.Update(&header.type, sizeof(header) - offsetof(RecordHeader, type));
    crc.Update(payload_.data(), payload_.size());
    header.crc = crc.Finalize();

    // Serialize header
    std::memcpy(result.data(), &header, sizeof(RecordHeader));

    // Copy payload
    if (!payload_.empty()) {
        std::memcpy(result.data() + sizeof(RecordHeader), payload_.data(), payload_.size());
    }

    // Padding is already zeroed

    return result;
}

RecordParser::RecordParser(std::span<const uint8_t> data) {
    if (data.size() < sizeof(RecordHeader)) {
        return;
    }

    std::memcpy(&header_, data.data(), sizeof(RecordHeader));

    size_t expected_size = sizeof(RecordHeader) + header_.length + header_.padding;
    if (data.size() < expected_size) {
        return;
    }

    // Verify CRC
    CRC32C crc;
    crc.Update(&header_.type, sizeof(header_) - offsetof(RecordHeader, type));
    crc.Update(data.data() + sizeof(RecordHeader), header_.length);

    if (crc.Finalize() != header_.crc) {
        return;
    }

    payload_ = data.subspan(sizeof(RecordHeader), header_.length);
    valid_ = true;
}

}  // namespace raftpp::wal
