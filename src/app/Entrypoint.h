#pragma once

namespace holder::app {

using RunDaemonFn = int (*)(int argc, char* argv[]);
using FatalReporterFn = void (*)(const char* message) noexcept;

int run_entrypoint(
    int argc,
    char* argv[],
    RunDaemonFn run_daemon,
    FatalReporterFn report_fatal_error
) noexcept;

} // namespace holder::app
