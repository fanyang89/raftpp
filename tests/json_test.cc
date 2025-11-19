#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "raftpp/majority.h"
#include "raftpp/primitives.h"

using namespace raftpp;

TEST(JSONTest, Set) {
    Set<int> s;
    s.emplace(1);
    s.emplace(2);
    s.emplace(3);
    const nlohmann::json j = s;
    const Set<int> s2 = j;
    EXPECT_EQ(s, s2);
}

TEST(JSONTest, Map) {
    Map<int, std::string> s;
    s.emplace(1, "a");
    s.emplace(2, "b");
    s.emplace(3, "c");
    const nlohmann::json j = s;
    const Map<int, std::string> s2 = j;
    EXPECT_EQ(s, s2);
}

TEST(JSONTest, MajorityConfig) {
    MajorityConfig c;
    c.mutable_voters().emplace(1);
    c.mutable_voters().emplace(2);
    c.mutable_voters().emplace(3);
    const nlohmann::json j = c;
    const Set<uint64_t> s = j["voters"];
    Set<uint64_t> s2{1, 2, 3};
    EXPECT_EQ(s, s2);
}
