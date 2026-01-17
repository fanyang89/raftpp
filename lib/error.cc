#include "raftpp/error.h"

namespace {

struct ToStringVisitor {
    std::string operator()(const raftpp::StorageErrorCode ec) const {
        return fmt::format("storage error: {}", format_as(ec));
    }

    std::string operator()(raftpp::StorageErrorOther ec) const {
        return fmt::format("other storage error: {}", ec.message);
    }

    std::string operator()(const raftpp::RaftErrorCode ec) const {
        return fmt::format("raft error: {}", format_as(ec));
    }

    std::string operator()(raftpp::InvalidConfigError ec) const {
        return fmt::format("invalid config error: {}", ec.message);
    }

    std::string operator()(raftpp::ConfChangeError ec) const {
        return fmt::format("conf change error: {}", ec.message);
    }

    std::string operator()(raftpp::FatalError ec) const {
        return fmt::format("fatal error: {}", ec.message);
    }
};

}  // namespace

namespace raftpp {

bool StorageErrorOther::operator==(const StorageErrorOther&) const = default;

bool InvalidConfigError::operator==(const InvalidConfigError&) const = default;

RaftError ConfChangeError::ToError() const {
    return RaftError{*this};
}

bool ConfChangeError::operator==(const ConfChangeError&) const = default;

RaftError FatalError::ToError() const {
    return RaftError{*this};
}

bool FatalError::operator==(const FatalError&) const = default;

RaftError InvalidConfigError::ToError() const {
    return RaftError{*this};
}

bool RaftError::operator==(const RaftError& other) const {
    return inner_ == other.inner_;
}

std::string RaftError::ToString() const {
    return std::visit(ToStringVisitor{}, inner_);
}

}  // namespace raftpp
