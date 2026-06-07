#pragma once

#include "model/Project.h"

#include <optional>
#include <string>

namespace holder::card {
class CardStore;
} // namespace holder::card

namespace holder::platform {
class Db;
} // namespace holder::platform

namespace holder::app {

std::string generate_uuid_v4();

std::optional<holder::model::Project> ensure_default_home_project(holder::platform::Db& db);

void ensure_default_welcome_card(
    holder::card::CardStore& card_store,
    const holder::model::Project& home
);

} // namespace holder::app
