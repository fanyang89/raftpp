#include <doctest/doctest.h>
#include <spdlog/spdlog.h>

#include "raftpp/majority_conf.h"
#include "raftpp/primitives.h"

using namespace raftpp;

TEST_CASE("JSONTest.Set") {
    Set<int> s;
    s.emplace(1);
    s.emplace(2);
    s.emplace(3);
    const nlohmann::json j = s;
    const Set<int> s2 = j;
    CHECK_EQ(s, s2);
}

TEST_CASE("JSONTest.Map") {
    Map<int, std::string> s;
    s.emplace(1, "a");
    s.emplace(2, "b");
    s.emplace(3, "c");
    const nlohmann::json j = s;
    const Map<int, std::string> s2 = j;
    CHECK_EQ(s, s2);
}

TEST_CASE("JSONTest.MajorityConfig") {
    MajorityConfig c;
    c.emplace(1);
    c.emplace(2);
    c.emplace(3);
    const nlohmann::json j = c;
    const Set<uint64_t> s = j["voters"];
    Set<uint64_t> s2{1, 2, 3};
    CHECK_EQ(s, s2);
}
