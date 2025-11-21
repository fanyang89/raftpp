#include <gtest/gtest.h>

#include "data_driven_test.h"
#include "raftpp/conf_changer.h"

using namespace raftpp;

class ConfChangeDataDrivenTest : public testing::TestWithParam<TestData> {};
