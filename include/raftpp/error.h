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

struct StorageErrorOther {
    std::string message;
    bool operator==(const StorageErrorOther&) const;
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
    bool operator==(const InvalidConfigError&) const;
};

struct ConfChangeError {
    std::string message;
    [[nodiscard]] RaftError ToError() const;
    bool operator==(const ConfChangeError&) const;
};

struct FatalError {
    std::string message;
    [[nodiscard]] RaftError ToError() const;
    bool operator==(const FatalError&) const;
};

using RaftErrorInner = std::variant<
    StorageErrorCode, StorageErrorOther, RaftErrorCode, InvalidConfigError, ConfChangeError,
    FatalError>;

// RaftError is the universal error type in this lib
class RaftError {
  public:
    template <typename... Args>
    explicit RaftError(Args&&... args);

    template <typename T>
    [[nodiscard]] operator std::expected<T, RaftError>() const;

    template <typename T>
    bool Is() const;

    template <typename T>
    bool Is(const T& ec) const;

    template <typename T>
    bool operator==(const T& ec) const;

    bool operator==(const RaftError& other) const;

    std::string ToString() const;

    void Unwrap() const;

  private:
    RaftErrorInner inner_;
};

template <typename... Args>
RaftError::RaftError(Args&&... args) : inner_(std::forward<Args>(args)...) {}

template <typename T>
RaftError::operator std::expected<T, RaftError>() const {
    return std::unexpected(*this);
}

template <typename T>
bool RaftError::Is() const {
    return std::holds_alternative<T>(inner_);
}

template <typename T>
bool RaftError::Is(const T& ec) const {
    if (!Is<T>()) {
        return false;
    }
    return std::get<T>(inner_) == ec;
}

template <typename T>
bool RaftError::operator==(const T& ec) const {
    return Is(ec);
}

template <typename R, typename E = RaftError>
using Result = std::expected<R, E>;

template <class T, class E>
[[nodiscard]] constexpr T Unwrap(const std::expected<T, E>& ex) {
    if (ex.has_value()) {
        return ex.value();
    }
    const auto& err = ex.error();
    PANIC("Unwrap", err);
}

template <class T, class E>
[[nodiscard]] constexpr T UnwrapOr(const std::expected<T, E>& ex, const T& value) {
    if (ex.has_value()) {
        return ex.value();
    }
    return value;
}

}  // namespace raftpp
