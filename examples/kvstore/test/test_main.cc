#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"
#include "raftpp/logging.h"

int main(const int argc, char** argv) {
    raftpp::logging::ConfigureFromEnv(raftpp::logging::LogLevel::kWarn);

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
