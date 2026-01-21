#pragma once

#include <optional>
#include <string>

namespace kvstore {

enum class Op { Put, Get, Del };

struct Command {
    Op op;
    std::string key;
    std::optional<std::string> value;
};

struct Response {
    bool success;
    std::optional<std::string> value;
    std::optional<std::string> error;
};

class IKVStore {
  public:
    virtual ~IKVStore() = default;
    virtual std::optional<std::string> Get(const std::string& key) = 0;
    virtual bool Put(const std::string& key, const std::string& value) = 0;
    virtual bool Del(const std::string& key) = 0;
};

}  // namespace kvstore
