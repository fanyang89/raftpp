#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace raftpp {

class Inflights {
  public:
    explicit Inflights(size_t capacity);

    void SetCapacity(size_t incoming_capacity);
    void Add(uint64_t inflight);
    [[nodiscard]] bool Full() const;
    [[nodiscard]] size_t Count() const;
    [[nodiscard]] size_t BufferSize() const;
    [[nodiscard]] bool buffer_is_allocated() const;
    void Reset();
    void FreeTo(uint64_t to);
    void FreeFirstOne();

  protected:
    size_t start_;
    size_t count_;
    std::vector<uint64_t> buffer_;
    size_t capacity_;
    std::optional<size_t> incoming_capacity_;
};

}  // namespace raftpp
