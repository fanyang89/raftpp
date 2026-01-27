#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "codec.h"

namespace raftpp::raftor::rpc {

/// RDMA transport tuning parameters (rdma-core / RC).
struct RdmaConfig {
    /// Number of pre-posted receive buffers per connection.
    size_t recv_buffer_count = 256;

    /// Max number of in-flight sends per connection.
    size_t send_buffer_count = 256;

    /// Size of each registered buffer (bytes).
    /// Must be >= max_size_per_message + message overhead + RPC framing overhead.
    size_t buffer_size = 1024 * 1024 + Codec::MessageOverhead() + Codec::FrameOverhead();

    /// Completion queue depth per connection.
    size_t cq_depth = 1024;

    /// Queue pair depth per connection.
    size_t qp_depth = 1024;

    /// Inline data threshold (bytes), 0 disables inline.
    size_t max_inline_data = 0;

    /// IB port index (default 1).
    uint8_t ib_port = 1;

    /// GID index for RoCE (0 by default).
    uint8_t gid_index = 0;

    /// Optional device name override (empty means auto-select).
    std::string device_name;
};

}  // namespace raftpp::raftor::rpc
