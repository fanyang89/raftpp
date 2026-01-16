#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"
#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>

int main(const int argc, char** argv) {
    // Set default log level to WARN for tests to reduce noise
    // Use SPDLOG_LEVEL=debug or SPDLOG_LEVEL=info environment variable to enable verbose logging
    spdlog::set_level(spdlog::level::warn);
    spdlog::cfg::load_env_levels();

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int rc = context.run();
    if (context.shouldExit()) {
        return rc;
    }
    return 0;
}
