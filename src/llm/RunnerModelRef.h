#pragma once

#include <optional>
#include <string>

namespace holder::llm {

struct RunnerModelRef {
  std::string runner_id;
  std::string model_name;
};

std::string make_runner_model_ref(const std::string& runner_id, const std::string& model_name);
std::optional<RunnerModelRef> parse_runner_model_ref(const std::string& ref);
std::string normalize_local_runner_model_ref(const std::string& ref);
std::optional<std::string> local_model_name_from_ref(
    const std::optional<std::string>& ref,
    const std::string& expected_runner_id);
std::optional<std::string> runner_id_from_ref(const std::optional<std::string>& ref);

} // namespace holder::llm
