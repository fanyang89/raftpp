#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace raftpp {

constexpr uint64_t kInvalidIndex = 0;
constexpr uint64_t kInvalidId = 0;

template <typename K, typename V, typename... Args>
class Map : public std::unordered_map<K, V, Args...> {
  public:
    using std::unordered_map<K, V, Args...>::unordered_map;

    [[nodiscard]] bool contains(const K& key) const { return this->find(key) != this->end(); }

    [[nodiscard]] bool Contains(const K& key) const { return this->find(key) != this->end(); }
};

template <typename K, typename... Args>
class Set : public std::unordered_set<K, Args...> {
  public:
    using std::unordered_set<K, Args...>::unordered_set;

    [[nodiscard]] bool contains(const K& key) const { return this->find(key) != this->end(); }

    [[nodiscard]] bool Contains(const K& key) const { return this->find(key) != this->end(); }
};

}  // namespace raftpp
