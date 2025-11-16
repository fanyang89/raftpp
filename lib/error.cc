#include "raftpp/error.h"

namespace raftpp {

RaftError InvalidConfigError::ToError() const {
    return {*this};
}

RaftError::RaftError(const StorageErrorCode ec) :
    type_(ErrorCodeType::Storage), ec_({.storage_ec = ec}) {}

RaftError::RaftError(const RaftErrorCode ec) :
    type_(ErrorCodeType::Raft), ec_({.raft_ec = ec}) {}

RaftError::RaftError(const InvalidConfigError& ec) :
    type_(ErrorCodeType::InvalidConfig), ec_({.config_ec = ec}) {}

}
