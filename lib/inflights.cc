#include "raftpp/inflights.h"

#include <libassert/assert.hpp>

namespace raftpp {

Inflights::Inflights(const size_t capacity) : start_(0), count_(0), capacity_(capacity) {
    // Don't pre-allocate buffer - it will be allocated on first Add()
    // This matches raft-rs behavior where buffer is lazily allocated
}

void Inflights::SetCapacity(size_t incoming_capacity) {
    if (capacity_ == incoming_capacity) {
        incoming_capacity_ = std::nullopt;
        return;
    }

    if (capacity_ < incoming_capacity) {
        if (start_ + count_ <= capacity_) {
            if (buffer_.capacity() > 0) {
                buffer_.reserve(incoming_capacity);
            }
        } else {
            DEBUG_ASSERT(capacity_ == buffer_.size());
            std::vector<uint64_t> buffer;
            buffer.reserve(incoming_capacity);
            buffer.insert(buffer.end(), buffer_.begin() + start_, buffer_.end());
            buffer.insert(buffer.end(), buffer_.begin(), buffer_.begin() + count_ - (capacity_ - start_));
            buffer_ = std::move(buffer);
            start_ = 0;
        }
        capacity_ = incoming_capacity;
        incoming_capacity_ = std::nullopt;
        return;
    }

    // incoming_capacity_ > incoming_capacity
    if (count_ == 0) {
        capacity_ = incoming_capacity;
        incoming_capacity_ = std::nullopt;
        start_ = 0;
        if (buffer_.capacity() > 0) {
            std::vector<uint64_t> buffer;
            buffer.reserve(incoming_capacity);
            buffer_ = std::move(buffer);
        }
    } else {
        incoming_capacity_ = incoming_capacity;
    }
}

void Inflights::Reset() {
    count_ = 0;
    start_ = 0;
    buffer_ = std::vector<uint64_t>();

    if (incoming_capacity_.has_value()) {
        capacity_ = *incoming_capacity_;
        incoming_capacity_ = std::nullopt;
    }
}

void Inflights::FreeTo(const uint64_t to) {
    if (count_ == 0 || to < buffer_[start_]) {
        // out of the left side of the window
        return;
    }

    size_t i = 0;
    size_t idx = start_;
    while (i < count_) {
        if (to < buffer_[idx]) {
            // found the first large inflight
            break;
        }

        // increase index and maybe rotate
        idx += 1;
        if (idx >= capacity_) {
            idx -= capacity_;
        }

        i += 1;
    }

    // free i inflights and set new start index
    count_ -= i;
    start_ = idx;

    if (count_ == 0) {
        if (incoming_capacity_) {
            const auto incoming_cap = *incoming_capacity_;
            start_ = 0;
            capacity_ = incoming_cap;
            std::vector<uint64_t> buf;
            buf.reserve(capacity_);
            buffer_ = std::move(buf);
            incoming_capacity_ = std::nullopt;
        }
    }
}

void Inflights::FreeFirstOne() {
    if (count_ > 0) {
        const auto start = buffer_[start_];
        FreeTo(start);
    }
}

void Inflights::Add(const uint64_t inflight) {
    if (Full()) {
        PANIC("inflights full");
    }

    if (buffer_.capacity() == 0) {
        DEBUG_ASSERT(count_ == 0);
        DEBUG_ASSERT(start_ == 0);
        DEBUG_ASSERT(!incoming_capacity_.has_value());
        std::vector<uint64_t> buf;
        buf.reserve(capacity_);
        buffer_ = std::move(buf);
    }

    auto next = start_ + count_;
    if (next >= capacity_) {
        next -= capacity_;
    }
    ASSERT(next <= buffer_.size());
    if (next == buffer_.size()) {
        buffer_.emplace_back(inflight);
    } else {
        buffer_[next] = inflight;
    }
    count_ += 1;
}

bool Inflights::Full() const {
    if (count_ == capacity_) {
        return true;
    }
    if (incoming_capacity_.has_value() && count_ >= *incoming_capacity_) {
        return true;
    }
    return false;
}

size_t Inflights::Count() const {
    return count_;
}

size_t Inflights::BufferSize() const {
    return buffer_.capacity();
}

bool Inflights::buffer_is_allocated() const {
    return buffer_.capacity() > 0;
}

}  // namespace raftpp
