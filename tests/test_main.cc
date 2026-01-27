#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"
#include "raftpp/logging.h"

int main(const int argc, char** argv) {
    // Set default log level to WARN for tests to reduce noise.
    // Use SPDLOG_LEVEL=debug or SPDLOG_LEVEL=info environment variable to enable verbose logging.
    raftpp::logging::ConfigureFromEnv(raftpp::logging::LogLevel::kWarn);

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int rc = context.run();
    if (context.shouldExit()) {
        return rc;
    }
    return 0;
}
