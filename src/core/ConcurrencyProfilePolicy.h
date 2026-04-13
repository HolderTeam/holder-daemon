#pragma once

#include "api/ConcurrencyProfile.h"
#include "caste.hpp"

namespace holder::core {

holder::api::ConcurrencyProfile concurrency_profile_for_caste(Caste caste);

} // namespace holder::core
