#define DOCTEST_CONFIG_IMPLEMENT
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>

#include "doctest/doctest.h"

int main(const int argc, char** argv) {
    spdlog::set_level(spdlog::level::warn);
    spdlog::cfg::load_env_levels();

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
