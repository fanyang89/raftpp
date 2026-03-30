#pragma once

#include <variant>

#include <nonstd/expected.hpp>
#include <spdlog/fmt/fmt.h>

#include "raftpp/core/assert.h"

namespace raftpp {

enum class StorageErrorCode {
    // Core storage errors
    /// The storage was compacted and not accessible
    Compacted,
    /// The log is not available.
    Unavailable,
    /// The log is being fetched.
    LogTemporarilyUnavailable,
    /// The snapshot is out of date.
    SnapshotOutOfDate,
    /// The snapshot is being created.
    SnapshotTemporarilyUnavailable,

    // Metadata errors
    /// Metadata file is smaller than minimum required size
    MetadataFileTooSmall,
    /// Metadata header validation failed
    InvalidMetadataHeader,
    /// Metadata CRC checksum mismatch
    MetadataCrcMismatch,
    /// Failed to parse HardState from metadata
    HardStateParseError,
    /// Failed to parse ConfState from metadata
    ConfStateParseError,

    // Segment errors
    /// Current active segment not found in segment manager
    CurrentSegmentNotFound,
    /// Attempted operation on a segment that is not open
    SegmentNotOpen,
    /// Segment header validation failed
    InvalidSegmentHeader,

    // io_uring errors
    /// io_uring support not built into this binary
    IoUringNotBuilt,
    /// io_uring is not supported on this platform
    IoUringNotLinux,
    /// io_uring initialization failed
    IoUringInitFailed,
    /// io_uring probe missing required operation
    IoUringProbeMissingOp,

    // WAL errors
    /// WAL entry record is corrupted (CRC mismatch)
    CorruptEntryRecord,
    /// Failed to parse entry from WAL record
    EntryParseError,

    // RaftLog errors
    /// Got zero entries when slice expected non-empty result
    ZeroEntriesInSlice,
};

constexpr std::string_view format_as(StorageErrorCode ec) {
    switch (ec) {
        case StorageErrorCode::Compacted:
            return "Compacted";
        case StorageErrorCode::Unavailable:
            return "Unavailable";
        case StorageErrorCode::LogTemporarilyUnavailable:
            return "LogTemporarilyUnavailable";
        case StorageErrorCode::SnapshotOutOfDate:
            return "SnapshotOutOfDate";
        case StorageErrorCode::SnapshotTemporarilyUnavailable:
            return "SnapshotTemporarilyUnavailable";
        case StorageErrorCode::MetadataFileTooSmall:
            return "MetadataFileTooSmall";
        case StorageErrorCode::InvalidMetadataHeader:
            return "InvalidMetadataHeader";
        case StorageErrorCode::MetadataCrcMismatch:
            return "MetadataCrcMismatch";
        case StorageErrorCode::HardStateParseError:
            return "HardStateParseError";
        case StorageErrorCode::ConfStateParseError:
            return "ConfStateParseError";
        case StorageErrorCode::CurrentSegmentNotFound:
            return "CurrentSegmentNotFound";
        case StorageErrorCode::SegmentNotOpen:
            return "SegmentNotOpen";
        case StorageErrorCode::InvalidSegmentHeader:
            return "InvalidSegmentHeader";
        case StorageErrorCode::IoUringNotBuilt:
            return "IoUringNotBuilt";
        case StorageErrorCode::IoUringNotLinux:
            return "IoUringNotLinux";
        case StorageErrorCode::IoUringInitFailed:
            return "IoUringInitFailed";
        case StorageErrorCode::IoUringProbeMissingOp:
            return "IoUringProbeMissingOp";
        case StorageErrorCode::CorruptEntryRecord:
            return "CorruptEntryRecord";
        case StorageErrorCode::EntryParseError:
            return "EntryParseError";
        case StorageErrorCode::ZeroEntriesInSlice:
            return "ZeroEntriesInSlice";
    }
    return "Unknown";
}

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
    /// Raftor already started.
    AlreadyStarted,
    /// Raftor shutting down.
    ShuttingDown,
    /// Lost leadership while proposals pending.
    LostLeadership,
    /// Storage type mismatch.
    IncompatibleStorage,
    /// Failed to parse conf change.
    ConfChangeParseError,
};

constexpr std::string_view format_as(RaftErrorCode ec) {
    switch (ec) {
        case RaftErrorCode::StepLocalMsg:
            return "StepLocalMsg";
        case RaftErrorCode::StepPeerNotFound:
            return "StepPeerNotFound";
        case RaftErrorCode::ProposalDropped:
            return "ProposalDropped";
        case RaftErrorCode::RequestSnapshotDropped:
            return "RequestSnapshotDropped";
        case RaftErrorCode::AlreadyStarted:
            return "AlreadyStarted";
        case RaftErrorCode::ShuttingDown:
            return "ShuttingDown";
        case RaftErrorCode::LostLeadership:
            return "LostLeadership";
        case RaftErrorCode::IncompatibleStorage:
            return "IncompatibleStorage";
        case RaftErrorCode::ConfChangeParseError:
            return "ConfChangeParseError";
    }
    return "Unknown";
}

enum class RpcErrorCode {
    /// Missing port in address
    AddressPortMissing,
    /// Invalid port format in address
    AddressPortInvalid,
    /// Port number out of valid range
    AddressPortOutOfRange,
    /// TCP/UDP bind failed
    BindFailed,
    /// TCP listen failed
    ListenFailed,
    /// UDP bind failed
    UdpBindFailed,
    /// UDP receive start failed
    UdpRecvStartFailed,
    /// Connection was closed
    ConnectionClosed,
    /// Invalid magic number in frame header
    InvalidMagic,
    /// Failed to parse RpcHeader
    HeaderParseFailed,
    /// Failed to parse RpcHandshake
    HandshakeParseFailed,
    /// Failed to parse message payload
    PayloadParseFailed,
    /// KCP handshake packet too short
    HandshakeTooShort,
    /// Invalid KCP handshake magic
    HandshakeInvalidMagic,
    /// Handshake buffer too small
    HandshakeBufferTooSmall,
    /// Message exceeds maximum allowed size
    MessageTooLarge,
    /// Operation timed out
    Timeout,
};

constexpr std::string_view format_as(RpcErrorCode ec) {
    switch (ec) {
        case RpcErrorCode::AddressPortMissing:
            return "AddressPortMissing";
        case RpcErrorCode::AddressPortInvalid:
            return "AddressPortInvalid";
        case RpcErrorCode::AddressPortOutOfRange:
            return "AddressPortOutOfRange";
        case RpcErrorCode::BindFailed:
            return "BindFailed";
        case RpcErrorCode::ListenFailed:
            return "ListenFailed";
        case RpcErrorCode::UdpBindFailed:
            return "UdpBindFailed";
        case RpcErrorCode::UdpRecvStartFailed:
            return "UdpRecvStartFailed";
        case RpcErrorCode::ConnectionClosed:
            return "ConnectionClosed";
        case RpcErrorCode::InvalidMagic:
            return "InvalidMagic";
        case RpcErrorCode::HeaderParseFailed:
            return "HeaderParseFailed";
        case RpcErrorCode::HandshakeParseFailed:
            return "HandshakeParseFailed";
        case RpcErrorCode::PayloadParseFailed:
            return "PayloadParseFailed";
        case RpcErrorCode::HandshakeTooShort:
            return "HandshakeTooShort";
        case RpcErrorCode::HandshakeInvalidMagic:
            return "HandshakeInvalidMagic";
        case RpcErrorCode::HandshakeBufferTooSmall:
            return "HandshakeBufferTooSmall";
        case RpcErrorCode::MessageTooLarge:
            return "MessageTooLarge";
        case RpcErrorCode::Timeout:
            return "Timeout";
    }
    return "Unknown";
}

enum class ConfigErrorCode {
    /// Node ID is invalid (kInvalidId)
    InvalidNodeId,
    /// Heartbeat tick must be greater than 0
    HeartbeatTickTooSmall,
    /// Election tick must be greater than heartbeat tick
    ElectionTickTooSmall,
    /// Max inflight messages must be greater than 0
    MaxInflightMessagesTooSmall,
    /// LeaseBased read-only option requires check_quorum to be true
    LeaseBasedReadRequiresCheckQuorum,
    /// Empty listen address
    ListenAddressEmpty,
    /// Empty data directory
    DataDirectoryEmpty,
    /// Node ID must be included in initial_peers list
    NodeIdNotInInitialPeers,
    /// RDMA configuration is invalid
    RdmaConfigInvalid,
    /// RDMA transport is not enabled at build time
    RdmaNotEnabled,
};

constexpr std::string_view format_as(ConfigErrorCode ec) {
    switch (ec) {
        case ConfigErrorCode::InvalidNodeId:
            return "InvalidNodeId";
        case ConfigErrorCode::HeartbeatTickTooSmall:
            return "HeartbeatTickTooSmall";
        case ConfigErrorCode::ElectionTickTooSmall:
            return "ElectionTickTooSmall";
        case ConfigErrorCode::MaxInflightMessagesTooSmall:
            return "MaxInflightMessagesTooSmall";
        case ConfigErrorCode::LeaseBasedReadRequiresCheckQuorum:
            return "LeaseBasedReadRequiresCheckQuorum";
        case ConfigErrorCode::ListenAddressEmpty:
            return "ListenAddressEmpty";
        case ConfigErrorCode::DataDirectoryEmpty:
            return "DataDirectoryEmpty";
        case ConfigErrorCode::NodeIdNotInInitialPeers:
            return "NodeIdNotInInitialPeers";
        case ConfigErrorCode::RdmaConfigInvalid:
            return "RdmaConfigInvalid";
        case ConfigErrorCode::RdmaNotEnabled:
            return "RdmaNotEnabled";
    }
    return "Unknown";
}

enum class ConfChangeErrorCode {
    /// learners_next must be empty when not in joint config
    LearnersNextMustBeEmpty,
    /// auto_leave must be false when not in joint config
    AutoLeaveMustBeFalse,
    /// Cannot enter joint config when already in joint config
    ConfigAlreadyJoint,
    /// Cannot make a zero-voter config joint
    ZeroVoterConfigJoint,
    /// Cannot leave a non-joint config
    LeaveNonJointConfig,
    /// Removed all voters from config
    RemovedAllVoters,
    /// Cannot apply simple config change while in joint config
    CannotApplySimpleInJointConfig,
    /// Multiple voters changed without entering joint config
    MultipleVotersChangedWithoutJoint,
};

constexpr std::string_view format_as(ConfChangeErrorCode ec) {
    switch (ec) {
        case ConfChangeErrorCode::LearnersNextMustBeEmpty:
            return "LearnersNextMustBeEmpty";
        case ConfChangeErrorCode::AutoLeaveMustBeFalse:
            return "AutoLeaveMustBeFalse";
        case ConfChangeErrorCode::ConfigAlreadyJoint:
            return "ConfigAlreadyJoint";
        case ConfChangeErrorCode::ZeroVoterConfigJoint:
            return "ZeroVoterConfigJoint";
        case ConfChangeErrorCode::LeaveNonJointConfig:
            return "LeaveNonJointConfig";
        case ConfChangeErrorCode::RemovedAllVoters:
            return "RemovedAllVoters";
        case ConfChangeErrorCode::CannotApplySimpleInJointConfig:
            return "CannotApplySimpleInJointConfig";
        case ConfChangeErrorCode::MultipleVotersChangedWithoutJoint:
            return "MultipleVotersChangedWithoutJoint";
    }
    return "Unknown";
}

class RaftError;

struct InvalidConfigError {
    std::string message;

    explicit InvalidConfigError(std::string msg) : message(std::move(msg)) {}

    [[nodiscard]] RaftError ToError() const;
    bool operator==(const InvalidConfigError&) const;
};

struct ConfChangeError {
    std::string message;

    explicit ConfChangeError(std::string msg) : message(std::move(msg)) {}

    [[nodiscard]] RaftError ToError() const;
    bool operator==(const ConfChangeError&) const;
};

struct FatalError {
    std::string message;

    explicit FatalError(std::string msg) : message(std::move(msg)) {}

    [[nodiscard]] RaftError ToError() const;
    bool operator==(const FatalError&) const;
};

using RaftErrorInner = std::variant<
    StorageErrorCode, StorageErrorOther, RaftErrorCode, InvalidConfigError, ConfChangeError,
    FatalError, RpcErrorCode, ConfigErrorCode, ConfChangeErrorCode>;

// RaftError is the universal error type in this lib
class RaftError {
  public:
    // Default special member functions
    RaftError(const RaftError&) = default;
    RaftError(RaftError&&) = default;
    RaftError& operator=(const RaftError&) = default;
    RaftError& operator=(RaftError&&) = default;

    // Forwarding constructor for RaftErrorInner types (excludes RaftError itself)
    template <typename T, typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, RaftError>>>
    explicit RaftError(T&& arg) : inner_(std::forward<T>(arg)) {}

    template <typename T>
    [[nodiscard]] operator nonstd::expected<T, RaftError>() const;

    template <typename T>
    bool Is() const;

    template <typename T>
    bool Is(const T& ec) const;

    template <typename T>
    bool operator==(const T& ec) const;

    bool operator==(const RaftError& other) const;

    std::string ToString() const;

  private:
    RaftErrorInner inner_;
};

template <typename T>
RaftError::operator nonstd::expected<T, RaftError>() const {
    return nonstd::make_unexpected(*this);
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
using Result = nonstd::expected<R, E>;

template <class T, class E>
[[nodiscard]] constexpr T Unwrap(const nonstd::expected<T, E>& ex) {
    if (ex) {
        return *ex;
    }
    if constexpr (std::is_same_v<E, RaftError>) {
        const auto& err = ex.error().ToString();
        PANIC("Unwrap", err);
    } else {
        const auto& err = ex.error();
        PANIC("Unwrap", err);
    }
}

template <class T, class E>
[[nodiscard]] constexpr T UnwrapOr(const nonstd::expected<T, E>& ex, const T& value) {
    if (ex) {
        return *ex;
    }
    return value;
}

}  // namespace raftpp
