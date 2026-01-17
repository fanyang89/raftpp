#pragma once

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

namespace raftpp {

constexpr uint64_t INVALID_INDEX = 0;
constexpr uint64_t INVALID_ID = 0;

template <typename K, typename V, typename... Args>
using Map = absl::flat_hash_map<K, V, Args...>;

template <typename K, typename... Args>
using Set = absl::flat_hash_set<K, Args...>;

}  // namespace raftpp
