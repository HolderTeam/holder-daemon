#pragma once

#include <cstddef>

namespace holder::api {

struct ConcurrencyProfile {
  std::size_t io_threads = 1;
  std::size_t ingress_workers = 1;
  std::size_t save_workers = 1;
  std::size_t general_workers = 3;
  std::size_t writer_workers = 1;
};

} // namespace holder::api
