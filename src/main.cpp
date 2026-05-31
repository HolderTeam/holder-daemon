#include "app/DaemonApp.h"
#include "app/FatalError.h"

#include <exception>

int main(int argc, char* argv[]) {
  try {
    return holder::app::run_daemon(argc, argv);
  } catch (const std::exception& ex) { // LCOV_EXCL_LINE
    holder::app::report_fatal_error(ex.what()); // LCOV_EXCL_LINE
    return 1; // LCOV_EXCL_LINE
  } catch (...) { // LCOV_EXCL_LINE
    holder::app::report_fatal_error("unknown fatal error"); // LCOV_EXCL_LINE
    return 1; // LCOV_EXCL_LINE
  }
}
