#pragma once

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

namespace raftpp {

constexpr uint64_t INVALID_INDEX = 0;
constexpr uint64_t INVALID_ID = 0;

template <typename K, typename V, typename... Args>
using Map = absl::flat_hash_map<K, V, Args...>;

template <typename K, typename... Args>
using Set = absl::flat_hash_set<K, Args...>;

}  // namespace raftpp

NLOHMANN_JSON_NAMESPACE_BEGIN

template <typename V>
struct adl_serializer<raftpp::Set<V>> {
    static void to_json(json& j, const raftpp::Set<V>& m) {
        j = json::array();
        for (const auto& v : m) {
            j.push_back(v);
        }
    }

    static void from_json(const json& j, raftpp::Set<V>& m) {
        if (!j.is_array()) {
            throw json::type_error::create(302, fmt::format("invalid type {}", j.type_name()), &j);
        }
        m.clear();
        for (const auto& elem : j) {
            m.insert(elem.get<V>());
        }
    }
};

template <typename K, typename V>
struct adl_serializer<raftpp::Map<K, V>> {
    static void to_json(json& j, const raftpp::Map<K, V>& m) {
        j = json::object();
        for (const auto& [k, v] : m) {
            const auto key = fmt::format("{}", k);
            j[key] = v;
        }
    }

    static void from_json(const json& j, raftpp::Map<K, V>& m) {
        if (!j.is_object()) {
            throw json::type_error::create(302, fmt::format("invalid type {}", j.type_name()), &j);
        }
        m.clear();
        for (const auto& [k, v] : j.items()) {
            K key = json::parse(k).get<K>();
            V value = v.get<V>();
            m[key] = value;
        }
    }
};

NLOHMANN_JSON_NAMESPACE_END
