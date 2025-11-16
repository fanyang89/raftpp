#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include "raftpp/logging.h"

int main(const int argc, char** argv) {
    raftpp::RegisterLogSink();

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int rc = context.run();
    if (context.shouldExit()) {
        return rc;
    }
    return rc;
}
