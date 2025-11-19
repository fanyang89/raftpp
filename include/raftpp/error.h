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

// ReSharper disable CppNonExplicitConvertingConstructor,CppNonExplicitConversionOperator

// RaftError is the universal error type in this lib
class RaftError {
  public:
    RaftError(StorageErrorCode ec);
    RaftError(RaftErrorCode ec);
    RaftError(const InvalidConfigError& ec);
    RaftError(const ConfChangeError& ec);

    template <typename T>
    operator std::expected<T, RaftError>() const;

  private:
    std::variant<StorageErrorCode, RaftErrorCode, InvalidConfigError, ConfChangeError> err_;
};

// ReSharper restore CppNonExplicitConvertingConstructor,CppNonExplicitConversionOperator

template <typename T>
RaftError::operator std::expected<T, RaftError>() const {
    return std::unexpected(*this);
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
