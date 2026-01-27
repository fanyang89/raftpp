#define DOCTEST_CONFIG_IMPLEMENT
#include "raftpp/logging.h"

#include "doctest/doctest.h"

int main(const int argc, char** argv) {
    raftpp::logging::ConfigureFromEnv(raftpp::logging::LogLevel::kWarn);

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
