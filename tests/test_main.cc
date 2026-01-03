#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

int main(const int argc, char** argv) {
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int rc = context.run();
    if (context.shouldExit()) {
        return rc;
    }
    return 0;
}
