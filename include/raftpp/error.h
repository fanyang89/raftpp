#pragma once

#include <expected>
#include <variant>

#include <libassert/assert.hpp>
#include <spdlog/fmt/fmt.h>

namespace raftpp {

enum class StorageErrorCode {
    /// The storage was compacted and not accessible
    Compacted,
    /// The log is not available.
    Unavailable,
    /// The log is being fetched.
    LogTemporarilyUnavailable,
    /// The snapshot is out of date.
    SnapshotOutOfDate,
    /// The snapshot is being created.
    SnapshotTemporarilyUnavailable
};

enum class RaftErrorCode {
    /// Raft cannot step the local message.
    StepLocalMsg,
    /// The raft peer is not found and thus cannot step.
    StepPeerNotFound,
    /// The proposal of changes was dropped.
    ProposalDropped,
    /// The request snapshot is dropped.
    RequestSnapshotDropped,
};

class RaftError;

struct InvalidConfigError {
    std::string message;
    [[nodiscard]] RaftError ToError() const;
};

struct ConfChangeError {
    std::string message;
    [[nodiscard]] RaftError ToError() const;
};

using RaftErrorInner = std::variant<StorageErrorCode, RaftErrorCode, InvalidConfigError, ConfChangeError>;

// RaftError is the universal error type in this lib
class RaftError : public RaftErrorInner {
  public:
    template <typename T>
    operator std::expected<T, RaftError>() const;

    template <typename T>
    bool Is(const T& ec) const;

    bool IsStorageError() const;
    StorageErrorCode AsStorageError() const;
};

template <typename T>
RaftError::operator std::expected<T, RaftError>() const {
    return std::unexpected(*this);
}

template <typename T>
bool RaftError::Is(const T& ec) const {
    if constexpr (std::is_same_v<T, StorageErrorCode>) {
        return std::get<StorageErrorCode>(*this) == ec;
    } else if constexpr (std::is_same_v<T, RaftErrorCode>) {
        return std::get<RaftErrorCode>(*this) == ec;
    } else if constexpr (std::is_same_v<T, InvalidConfigError>) {
        return std::get<InvalidConfigError>(*this) == ec;
    } else if constexpr (std::is_same_v<T, ConfChangeError>) {
        return std::get<ConfChangeError>(*this) == ec;
    } else {
        static_assert(!std::is_same_v<T, T>, "unexpected type");
        return false;
    }
}

template <typename R, typename E = RaftError>
using Result = std::expected<R, E>;

template <class T, class E>
constexpr T Unwrap(std::expected<T, E> ex) {
    if (ex.has_value()) {
        return ex.value();
    }
    PANIC("Unwrap error");
}

template <class T, class E>
constexpr T UnwrapOr(std::expected<T, E> ex, T value) {
    if (ex.has_value()) {
        return ex.value();
    }
    return value;
}

}  // namespace raftpp

template <>
struct fmt::formatter<raftpp::InvalidConfigError> {
    static format_context::iterator format(const raftpp::InvalidConfigError& value, const format_context& ctx);
};
