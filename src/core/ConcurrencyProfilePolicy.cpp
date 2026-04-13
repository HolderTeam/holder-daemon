#include "core/ConcurrencyProfilePolicy.h"

namespace holder::core {

holder::api::ConcurrencyProfile concurrency_profile_for_caste(Caste caste) {
  holder::api::ConcurrencyProfile profile;
  if (caste == Caste::Mini) {
    profile.io_threads = 1;
    profile.general_workers = 2;
  } else if (caste == Caste::User) {
    profile.io_threads = 1;
    profile.general_workers = 3;
  } else if (dev_or_above.contains(caste)) {
    profile.io_threads = 2;
    profile.general_workers = 4;
  }
  return profile;
}

} // namespace holder::core
