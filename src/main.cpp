#include "app/DaemonApp.h"
#include "app/Entrypoint.h"
#include "app/FatalError.h"

int main(int argc, char* argv[]) {
  return holder::app::run_entrypoint(
      argc,
      argv,
      holder::app::run_daemon,
      holder::app::report_fatal_error
  );
}
