#pragma once

#include <doctest/doctest.h>

#include "harness/test_util.h"
#include "raftpp/core/error.h"
#include "raftpp/core/raft_core.h"
#include "raftpp/core/types.h"

#define DOCTEST_VALUE_PARAMETERIZED_DATA(data, data_container)                           \
    do {                                                                                 \
        size_t idx = 0;                                                                  \
        for (const auto& it : data_container) {                                          \
            DOCTEST_SUBCASE(                                                             \
                (std::string(#data_container "[") + std::to_string(idx++) + "]").c_str() \
            ) {                                                                          \
                data = it;                                                               \
            }                                                                            \
        }                                                                                \
    } while (0)

#define DOCTEST_VALUE_PARAMETERIZED_DATA_WITH_INDEX(data, data_container)                \
    do {                                                                                 \
        size_t idx = 0;                                                                  \
        for (const auto& it : data_container) {                                          \
            DOCTEST_SUBCASE(                                                             \
                (std::string(#data_container "[") + std::to_string(idx++) + "]").c_str() \
            ) {                                                                          \
                data = it;                                                               \
                data.test_index = idx;                                                   \
            }                                                                            \
        }                                                                                \
    } while (0)

namespace raftpp {

Snapshot NewSnapshot(uint64_t index, uint64_t term);

bool operator==(const Entry& e1, const Entry& e2);
bool operator==(const HardState& e1, const HardState& e2);

doctest::String toString(const std::vector<Entry>& entries);
doctest::String toString(const std::optional<std::vector<Entry>>& entries);
doctest::String toString(const RaftError& error);
doctest::String toString(const std::optional<uint64_t>& value);

}  // namespace raftpp
