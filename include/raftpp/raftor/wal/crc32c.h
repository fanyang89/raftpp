#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace raftpp::raftor::wal {

class CRC32C {
  public:
    CRC32C();

    void Update(const void* data, size_t len);
    void Update(std::span<const uint8_t> data);

    // Finalize and return the CRC value
    [[nodiscard]] uint32_t Finalize() const;

    // Reset to initial state
    void Reset();

    // Convenience static methods
    [[nodiscard]] static uint32_t Compute(const void* data, size_t len);
    [[nodiscard]] static uint32_t Compute(std::span<const uint8_t> data);

  private:
    uint32_t crc_;
};

}  // namespace raftpp::raftor::wal
