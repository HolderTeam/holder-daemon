#if __has_include(<catch2/catch_session.hpp>)
#include <catch2/catch_session.hpp>

int main(int argc, char* argv[]) {
  return Catch::Session().run(argc, argv);
}
#else
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#endif
