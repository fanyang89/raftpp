#include <doctest/doctest.h>
#include <spdlog/spdlog.h>

#include "raftpp/majority.h"
#include "raftpp/primitives.h"

using namespace raftpp;

TEST_SUITE_BEGIN("JSON");

TEST_CASE("Set<K>") {
    Set<int> s;
    s.emplace(1);
    s.emplace(2);
    s.emplace(3);
    const nlohmann::json j = s;
    const Set<int> s2 = j;
    CHECK_EQ(s, s2);
}

TEST_CASE("Map<K, V>") {
    Map<int, std::string> s;
    s.emplace(1, "a");
    s.emplace(2, "b");
    s.emplace(3, "c");
    const nlohmann::json j = s;
    const Map<int, std::string> s2 = j;
    CHECK_EQ(s, s2);
}

TEST_CASE("MajorityConfig") {
    MajorityConfig c;
    c.mutable_voters().emplace(1);
    c.mutable_voters().emplace(2);
    c.mutable_voters().emplace(3);
    const nlohmann::json j = c;
    const Set<uint64_t> s = j["voters"];
    CHECK_EQ(s, Set<uint64_t>{1, 2, 3});
}

TEST_SUITE_END();
