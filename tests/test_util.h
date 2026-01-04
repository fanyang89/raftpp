#pragma once

#include <doctest/doctest.h>

#include "raftpp/raftpp.pb.h"

#define DOCTEST_VALUE_PARAMETERIZED_DATA(data, data_container)                                        \
    do {                                                                                              \
        size_t i = 0;                                                                                 \
        for (const auto& it : data_container) {                                                       \
            DOCTEST_SUBCASE((std::string(#data_container "[") + std::to_string(i++) + "]").c_str()) { \
                data = it;                                                                            \
            }                                                                                         \
        }                                                                                             \
    } while (0)

namespace raftpp {

Entry NewEntry(uint64_t index, uint64_t term);
bool operator==(const Entry& e1, const Entry& e2);
bool operator==(const Snapshot& e1, const Snapshot& e2);

}  // namespace raftpp
