#include "llm/RunnerModelRef.h"

#include "llm/RunnerRegistry.h"

namespace holder::llm {
namespace {

constexpr const char* kSeparator = "::";

} // namespace

std::string make_runner_model_ref(const std::string& runner_id, const std::string& model_name) {
  return runner_id + kSeparator + model_name;
}

std::optional<RunnerModelRef> parse_runner_model_ref(const std::string& ref) {
  const auto pos = ref.find(kSeparator);
  if (pos == std::string::npos || pos == 0 || pos + 2 >= ref.size()) {
    return std::nullopt;
  }
  return RunnerModelRef{
      .runner_id = ref.substr(0, pos),
      .model_name = ref.substr(pos + 2),
  };
}

std::string normalize_local_runner_model_ref(const std::string& ref) {
  if (ref.empty()) {
    return ref;
  }
  if (parse_runner_model_ref(ref).has_value()) {
    return ref;
  }
  return make_runner_model_ref(RunnerRegistry::kAutoLocalRunnerId, ref);
}

std::optional<std::string> local_model_name_from_ref(
    const std::optional<std::string>& ref,
    const std::string& expected_runner_id
) {
  if (!ref.has_value() || ref->empty()) {
    return std::nullopt;
  }
  const auto parsed = parse_runner_model_ref(ref.value());
  if (!parsed.has_value()) {
    return ref;
  }
  if (parsed->runner_id != expected_runner_id) {
    return std::nullopt;
  }
  return parsed->model_name;
}

std::optional<std::string> runner_id_from_ref(const std::optional<std::string>& ref) {
  if (!ref.has_value() || ref->empty()) {
    return std::nullopt;
  }
  const auto parsed = parse_runner_model_ref(ref.value());
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  return parsed->runner_id;
}

std::optional<ResolvedRunnerModel> resolve_configured_runner_model(
    const std::optional<std::string>& ref,
    RunnerRegistry* runner_registry
) {
  if (!ref.has_value() || ref->empty() || runner_registry == nullptr) {
    return std::nullopt;
  }
  const auto normalized = normalize_local_runner_model_ref(ref.value());
  const auto parsed = parse_runner_model_ref(normalized);
  if (!parsed.has_value()) {
    return std::nullopt; // LCOV_EXCL_LINE
  }
  auto* runner = runner_registry->get_client(parsed->runner_id);
  if (runner == nullptr) {
    return std::nullopt;
  }
  return ResolvedRunnerModel{
      .runner_id = parsed->runner_id,
      .model_name = parsed->model_name,
      .runner = runner,
  };
}

} // namespace holder::llm
