#pragma once

#include <optional>

#include <doctest/doctest.h>

#include "raftpp/raftpp.pb.h"

#define DOCTEST_VALUE_PARAMETERIZED_DATA(data, data_container)                                        \
    if (auto i = 0; true) {                                                                           \
        for (auto it = data_container.begin(); it != data_container.end(); ++it) {                    \
            DOCTEST_SUBCASE((std::string(#data_container "[") + std::to_string(i++) + "]").c_str()) { \
                data = *it;                                                                           \
            }                                                                                         \
        }                                                                                             \
    }

namespace raftpp {

Entry NewEntry(uint64_t index, uint64_t term);

}  // namespace raftpp
