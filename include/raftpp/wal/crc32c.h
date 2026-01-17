#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace raftpp::wal {

// CRC32C (Castagnoli) polynomial: 0x1EDC6F41
// Software implementation optimized with lookup table

class CRC32C {
  public:
    CRC32C() : crc_(0xFFFFFFFF) {}

    explicit CRC32C(uint32_t initial) : crc_(initial ^ 0xFFFFFFFF) {}

    void Update(const void* data, size_t len);
    void Update(std::span<const uint8_t> data);

    // Finalize and return the CRC value
    [[nodiscard]] uint32_t Finalize() const { return crc_ ^ 0xFFFFFFFF; }

    // Reset to initial state
    void Reset() { crc_ = 0xFFFFFFFF; }

    // Convenience static methods
    [[nodiscard]] static uint32_t Compute(const void* data, size_t len);
    [[nodiscard]] static uint32_t Compute(std::span<const uint8_t> data);

    // Extend an existing CRC with more data
    [[nodiscard]] static uint32_t Extend(uint32_t crc, const void* data, size_t len);

  private:
    uint32_t crc_;
    static const uint32_t kTable[256];
};

}  // namespace raftpp::wal
