#include "app/Entrypoint.h"

#include <exception>

namespace holder::app {

int run_entrypoint(
    int argc,
    char* argv[],
    RunDaemonFn run_daemon,
    FatalReporterFn report_fatal_error
) noexcept {
  try {
    return run_daemon(argc, argv);
  } catch (const std::exception& ex) {
    report_fatal_error(ex.what());
    return 1;
  } catch (...) {
    report_fatal_error("unknown fatal error");
    return 1;
  }
}

} // namespace holder::app
