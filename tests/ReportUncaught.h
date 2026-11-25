#pragma once
#include <cstdio>
#include <cstdlib>
#include <exception>

// QtTest catches a test's exception with catch (...), logs only "Caught unhandled exception" and rethrows.
// Run with -nocrashhandler and this reports what the exception said before aborting (CI diagnostics).
namespace sav::test {
inline const bool reportsUncaughtExceptions = [] {
    std::set_terminate([] {
        if (const auto current = std::current_exception()) {
            try { std::rethrow_exception(current); }
            catch (const std::exception& e) { std::fprintf(stderr, "Uncaught exception: %s\n", e.what()); }
            catch (...) { std::fprintf(stderr, "Uncaught exception of a non-standard type\n"); }
        } else std::fprintf(stderr, "Terminated without an active exception\n");
        std::fflush(stderr);
        std::abort();
    });
    return true;
}();
}
