#pragma once

#include <doctest/doctest.h>

#define DOCTEST_VALUE_PARAMETERIZED_DATA(data, data_container)                                        \
    if (auto i = 0; true) {                                                                           \
        for (auto it = data_container.begin(); it != data_container.end(); ++it) {                    \
            DOCTEST_SUBCASE((std::string(#data_container "[") + std::to_string(i++) + "]").c_str()) { \
                data = *it;                                                                           \
            }                                                                                         \
        }                                                                                             \
    }

namespace raftpp {}
