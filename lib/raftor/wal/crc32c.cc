#include "raftor/wal/crc32c.h"

#include <crc32c/crc32c.h>

namespace raftpp::wal {

CRC32C::CRC32C() : crc_(0) {}

void CRC32C::Update(const void* data, const size_t len) {
    crc_ = crc32c::Extend(crc_, static_cast<const uint8_t*>(data), len);
}

void CRC32C::Update(const std::span<const uint8_t> data) {
    crc_ = crc32c::Extend(crc_, data.data(), data.size());
}

uint32_t CRC32C::Finalize() const {
    return crc_;
}

void CRC32C::Reset() {
    crc_ = 0;
}

uint32_t CRC32C::Compute(const void* data, const size_t len) {
    CRC32C c;
    c.Update(data, len);
    return c.Finalize();
}

uint32_t CRC32C::Compute(const std::span<const uint8_t> data) {
    CRC32C c;
    c.Update(data);
    return c.Finalize();
}

}  // namespace raftpp::wal
