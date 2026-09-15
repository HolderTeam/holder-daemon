#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

YAML::Node load_openapi() { return YAML::LoadFile(OPENAPI_YAML_PATH); }

YAML::Node parameter_named(const YAML::Node& operation, const std::string& name) {
  for (const auto& parameter : operation["parameters"]) {
    if (parameter["name"].as<std::string>() == name) return parameter;
  }
  return {};
}

std::vector<std::string> required_properties(const YAML::Node& schema) {
  std::vector<std::string> required;
  for (const auto& property : schema["required"]) {
    required.push_back(property.as<std::string>());
  }
  std::sort(required.begin(), required.end());
  return required;
}

void require_json_response_ref(
    const YAML::Node& operation,
    const std::string& status,
    const std::string& schema_name
) {
  const auto response = operation["responses"][status];
  REQUIRE(response.IsDefined());
  REQUIRE(
      response["content"]["application/json"]["schema"]["$ref"].as<std::string>() ==
      "#/components/schemas/" + schema_name
  );
}

} // namespace

TEST_CASE("OpenAPI contracts project remote mutation results", "[openapi][sync][remote]") {
  const auto document = load_openapi();
  const auto operation = document["paths"]["/projects/{project_id}"]["patch"];
  CHECK(parameter_named(operation, "project_id")["required"].as<bool>());
  CHECK(
      operation["requestBody"]["content"]["application/json"]["schema"]["$ref"].as<std::string>() ==
      "#/components/schemas/ProjectUpdateRequest"
  );
  require_json_response_ref(operation, "200", "ProjectUpdateResponse");
  for (const auto* status : {"400", "401", "404"}) {
    require_json_response_ref(operation, status, "ErrorResponse");
  }
  const auto schemas = document["components"]["schemas"];
  CHECK(schemas["ProjectUpdateRequest"]["properties"]["git_remote_url"]["nullable"].as<bool>());
  const auto data = schemas["ProjectUpdateResponse"]["properties"]["data"];
  CHECK(required_properties(data) == std::vector<std::string>{"project_id"});
  CHECK(data["properties"]["git_remote_changed"]["type"].as<std::string>() == "boolean");
  const auto description = data["properties"]["git_remote_changed"]["description"].as<std::string>(
  );
  CHECK(description.find("Present when git_remote_url is supplied") != std::string::npos);
  CHECK(description.find("same value returns false") != std::string::npos);
}

TEST_CASE(
    "OpenAPI contracts binary asset content metadata and structured failures",
    "[openapi][resources][export]"
) {
  const auto document = load_openapi();
  const auto op = document["paths"]["/resources/{resource_id}/assets/{asset_id}/content"]["get"];
  CHECK(parameter_named(op, "resource_id")["required"].as<bool>());
  CHECK(parameter_named(op, "asset_id")["required"].as<bool>());
  CHECK(op["responses"]["200"]["headers"]["Content-Type"]["schema"]["type"].as<std::string>() == "string");
  CHECK(op["responses"]["200"]["headers"]["Content-Disposition"]["schema"]["type"].as<std::string>() == "string");
  CHECK(op["responses"]["200"]["content"]["application/octet-stream"]["schema"]["format"].as<std::string>() == "binary");
  for (const auto& code : {"400", "401", "404", "409", "422", "502", "503", "507"}) {
    require_json_response_ref(op, code, "ErrorResponse");
  }
}

TEST_CASE(
    "OpenAPI contracts card resource attachments and paginated listing",
    "[openapi][resources][attachments]"
) {
  const auto document = load_openapi();
  const auto list = document["paths"]["/resources"]["get"];
  CHECK(parameter_named(list, "project_id")["required"].as<bool>());
  CHECK_FALSE(parameter_named(list, "card_id")["required"].as<bool>());
  CHECK(parameter_named(list, "limit")["schema"]["maximum"].as<int>() == 1000);
  CHECK(parameter_named(list, "limit")["schema"]["default"].as<int>() == 100);
  CHECK(parameter_named(list, "offset")["schema"]["minimum"].as<int>() == 0);
  require_json_response_ref(list, "200", "ResourceListResponse");
  for (const auto& status : {"400", "401", "404", "422"}) require_json_response_ref(list, status, "ErrorResponse");
  const auto schemas = document["components"]["schemas"];
  CHECK(schemas["ResourceListResponse"]["properties"]["next_offset"]["nullable"].as<bool>());
  CHECK(required_properties(schemas["ResourceAttachmentResponse"]["properties"]["data"]) ==
      std::vector<std::string>{"card_id", "changed", "outcome", "resource_id"});
  for (const auto& method : {"post", "delete"}) {
    const auto op = document["paths"]["/cards/{card_id}/resources"][method];
    CHECK(parameter_named(op, "card_id")["required"].as<bool>());
    CHECK(required_properties(op["requestBody"]["content"]["application/json"]["schema"]) ==
        std::vector<std::string>{"project_id", "resource_id"});
    require_json_response_ref(op, "200", "ResourceAttachmentResponse");
    for (const auto& status : {"400", "401", "404", "422"}) require_json_response_ref(op, status, "ErrorResponse");
  }
}

TEST_CASE(
    "OpenAPI contracts project tags and exact card tag filtering",
    "[openapi][holderctl-foundation][tags]"
) {
  const auto document = load_openapi();
  const auto schemas = document["components"]["schemas"];

  const auto list_tags = document["paths"]["/projects/{project_id}/tags"]["get"];
  REQUIRE(list_tags.IsDefined());
  const auto project_id = parameter_named(list_tags, "project_id");
  REQUIRE(project_id.IsDefined());
  CHECK(project_id["in"].as<std::string>() == "path");
  CHECK(project_id["required"].as<bool>());
  require_json_response_ref(list_tags, "200", "ProjectTagListResponse");
  require_json_response_ref(list_tags, "404", "ErrorResponse");

  CHECK(
      required_properties(schemas["ProjectTag"]) == std::vector<std::string>{"card_count", "tag"}
  );
  CHECK(
      schemas["ProjectTagListResponse"]["properties"]["data"]["items"]["$ref"].as<std::string>() ==
      "#/components/schemas/ProjectTag"
  );

  const auto list_cards = document["paths"]["/cards"]["get"];
  const auto tag = parameter_named(list_cards, "tag");
  REQUIRE(tag.IsDefined());
  CHECK(tag["in"].as<std::string>() == "query");
  CHECK_FALSE(tag["required"].as<bool>());
  CHECK(tag["schema"]["type"].as<std::string>() == "string");
  CHECK(tag["description"].as<std::string>().find("Exact normalized tag") != std::string::npos);
  require_json_response_ref(list_cards, "200", "CardListResponse");
}

TEST_CASE(
    "OpenAPI contracts card-history revision references, snapshots, and restore",
    "[openapi][holderctl-history][history]"
) {
  const auto document = load_openapi();
  const auto schemas = document["components"]["schemas"];
  const auto revision = schemas["RevisionReference"];
  REQUIRE(revision.IsDefined());
  CHECK(revision["minLength"].as<int>() == 8);
  CHECK(revision["maxLength"].as<int>() == 40);
  CHECK(revision["pattern"].as<std::string>() == "^[0-9A-Fa-f]{8,40}$");

  const auto compare =
      document["paths"]["/projects/{project_id}/history/cards/{card_id}/compare"]["get"];
  REQUIRE(compare.IsDefined());
  CHECK(compare["description"].as<std::string>().find("first parent") != std::string::npos);
  for (const auto& name : {"from", "to"}) {
    const auto parameter = parameter_named(compare, name);
    REQUIRE(parameter.IsDefined());
    CHECK(
        parameter["schema"]["$ref"].as<std::string>() == "#/components/schemas/RevisionReference"
    );
  }
  CHECK(
      parameter_named(compare, "from")["description"].as<std::string>().find("since mode") !=
      std::string::npos
  );
  CHECK(
      parameter_named(compare, "mode")["description"].as<std::string>().find("first parent") !=
      std::string::npos
  );
  for (const auto& status : {"400", "404", "409", "413", "503"}) {
    require_json_response_ref(compare, status, "ErrorResponse");
  }

  const auto snapshot =
      document["paths"]["/projects/{project_id}/history/cards/{card_id}/snapshot"]["get"];
  REQUIRE(snapshot.IsDefined());
  for (const auto& name : {"project_id", "card_id", "oid"}) {
    const auto parameter = parameter_named(snapshot, name);
    REQUIRE(parameter.IsDefined());
    CHECK(parameter["required"].as<bool>());
  }
  CHECK(
      parameter_named(snapshot, "oid")["schema"]["$ref"].as<std::string>() ==
      "#/components/schemas/RevisionReference"
  );
  require_json_response_ref(snapshot, "200", "CardHistorySnapshotResponse");
  for (const auto& status : {"400", "401", "404", "409", "413", "503"}) {
    require_json_response_ref(snapshot, status, "ErrorResponse");
  }

  const auto restore =
      document["paths"]["/projects/{project_id}/history/cards/{card_id}/restore"]["post"];
  REQUIRE(restore.IsDefined());

  for (const auto& name : {"project_id", "card_id", "oid"}) {
    const auto parameter = parameter_named(restore, name);
    REQUIRE(parameter.IsDefined());
    CHECK(parameter["required"].as<bool>());
  }
  const auto oid = parameter_named(restore, "oid");
  CHECK(oid["in"].as<std::string>() == "query");
  CHECK(oid["schema"]["$ref"].as<std::string>() == "#/components/schemas/RevisionReference");

  require_json_response_ref(restore, "200", "CardHistoryRestoreResponse");
  for (const auto& status : {"400", "401", "404", "409", "503"}) {
    require_json_response_ref(restore, status, "ErrorResponse");
  }

  const auto response = schemas["CardHistoryRestoreResponse"];
  CHECK(required_properties(response) == std::vector<std::string>{"data", "ok"});
  CHECK(
      required_properties(response["properties"]["data"]) ==
      std::vector<std::string>{
          "card_id",
          "deleted_at",
          "restored_from_oid",
          "result_oid",
          "title",
          "updated_at"
      }
  );

  const auto snapshot_response = schemas["CardHistorySnapshotResponse"];
  CHECK(required_properties(snapshot_response) == std::vector<std::string>{"data", "ok"});
  CHECK(
      required_properties(snapshot_response["properties"]["data"]) ==
      std::vector<std::string>{"card_id", "snapshot"}
  );
}

TEST_CASE("OpenAPI contracts live-card tag mutations", "[openapi][holderctl-tags][tags]") {
  const auto document = load_openapi();
  const auto schemas = document["components"]["schemas"];
  const auto path = document["paths"]["/cards/{card_id}/tags"];

  for (const auto& method : {"post", "delete"}) {
    const auto operation = path[method];
    REQUIRE(operation.IsDefined());
    const auto card_id = parameter_named(operation, "card_id");
    REQUIRE(card_id.IsDefined());
    CHECK(card_id["in"].as<std::string>() == "path");
    CHECK(card_id["required"].as<bool>());
    CHECK(
        operation["requestBody"]["content"]["application/json"]["schema"]["$ref"].as<std::string>(
        ) == "#/components/schemas/CardTagMutationRequest"
    );
    require_json_response_ref(operation, "200", "CardTagMutationResponse");
    for (const auto& status : {"400", "401", "404", "422"}) {
      require_json_response_ref(operation, status, "ErrorResponse");
    }
  }

  CHECK(
      required_properties(schemas["CardTagMutationRequest"]) ==
      std::vector<std::string>{"project_id", "tag"}
  );
  CHECK(
      required_properties(schemas["CardTagMutationResult"]) ==
      std::vector<std::string>{"card_id", "changed", "outcome", "tag"}
  );
  const auto outcomes = schemas["CardTagMutationResult"]["properties"]["outcome"]["enum"];
  std::vector<std::string> documented_outcomes;
  for (const auto& outcome : outcomes)
    documented_outcomes.push_back(outcome.as<std::string>());
  std::sort(documented_outcomes.begin(), documented_outcomes.end());
  std::vector<std::string> expected{
      "added",
      "already_present",
      "not_present",
      "present_outside_editable_tag_line",
      "removed",
  };
  std::sort(expected.begin(), expected.end());
  CHECK(documented_outcomes == expected);
}

TEST_CASE(
    "OpenAPI contracts milestones and inclusive project calendar ranges",
    "[openapi][holderctl-milestones][milestones][calendar]"
) {
  const auto document = load_openapi();
  const auto schemas = document["components"]["schemas"];
  const auto milestone = schemas["Milestone"];
  const auto create = schemas["MilestoneCreateRequest"];

  REQUIRE(milestone.IsDefined());
  CHECK(milestone["properties"]["milestone_id"]["format"].as<std::string>() == "uuid");
  CHECK(
      milestone["properties"]["milestone_id"]["description"].as<std::string>().find(
          "never abbreviated"
      ) != std::string::npos
  );
  CHECK(
      milestone["properties"]["all_day"]["description"].as<std::string>().find("local calendar-date"
      ) != std::string::npos
  );
  CHECK(required_properties(create) == std::vector<std::string>{"start_at"});
  CHECK(create["properties"]["kind"].IsDefined());
  CHECK(create["properties"]["description"].IsDefined());
  CHECK_FALSE(create["properties"]["title"].IsDefined());
  CHECK(
      create["properties"]["end_at"]["description"].as<std::string>().find("Inclusive") !=
      std::string::npos
  );

  const auto card_milestones = document["paths"]["/cards/{card_id}/milestones"];
  REQUIRE(card_milestones["get"].IsDefined());
  REQUIRE(card_milestones["post"].IsDefined());
  require_json_response_ref(card_milestones["get"], "200", "MilestoneListResponse");
  require_json_response_ref(card_milestones["post"], "201", "MilestoneResponse");
  CHECK(
      card_milestones["post"]["description"].as<std::string>().find("no independent title") !=
      std::string::npos
  );

  const auto remove = document["paths"]["/cards/{card_id}/milestones/{milestone_id}"]["delete"];
  REQUIRE(remove.IsDefined());
  require_json_response_ref(remove, "200", "MilestoneRemoveResponse");
  CHECK(
      required_properties(schemas["MilestoneRemoveResult"]) ==
      std::vector<std::string>{"card_id", "milestone_id", "removed"}
  );

  const auto update = document["paths"]["/cards/{card_id}/milestones/{milestone_id}"]["patch"];
  REQUIRE(update.IsDefined());
  CHECK(
      update["requestBody"]["content"]["application/json"]["schema"]["$ref"].as<std::string>() ==
      "#/components/schemas/MilestoneUpdateRequest"
  );
  require_json_response_ref(update, "200", "MilestoneResponse");
  require_json_response_ref(update, "400", "ErrorResponse");
  require_json_response_ref(update, "404", "ErrorResponse");
  const auto milestone_id = parameter_named(update, "milestone_id");
  REQUIRE(milestone_id.IsDefined());
  CHECK(milestone_id["schema"]["format"].as<std::string>() == "uuid");
  CHECK(
      milestone_id["description"].as<std::string>().find("never abbreviated") !=
      std::string::npos
  );

  const auto milestone_update = schemas["MilestoneUpdateRequest"];
  REQUIRE(milestone_update.IsDefined());
  CHECK(milestone_update["minProperties"].as<int>() == 1);
  CHECK_FALSE(milestone_update["additionalProperties"].as<bool>());
  CHECK_FALSE(milestone_update["required"].IsDefined());
  CHECK_FALSE(milestone_update["properties"]["title"].IsDefined());
  for (const auto& name : {"start_at", "end_at", "all_day", "kind", "description"}) {
    CHECK(milestone_update["properties"][name].IsDefined());
  }
  CHECK(milestone_update["properties"]["end_at"]["nullable"].as<bool>());
  CHECK(milestone_update["properties"]["kind"]["nullable"].as<bool>());
  CHECK(milestone_update["properties"]["description"]["nullable"].as<bool>());

  const auto calendar = document["paths"]["/calendar"]["get"];
  REQUIRE(calendar.IsDefined());
  CHECK(
      calendar["description"].as<std::string>().find("both from and to are inclusive") !=
      std::string::npos
  );
  for (const auto& name : {"from", "to"}) {
    const auto parameter = parameter_named(calendar, name);
    REQUIRE(parameter.IsDefined());
    CHECK(parameter["required"].as<bool>());
    CHECK(parameter["schema"]["type"].as<std::string>() == "integer");
    CHECK(parameter["description"].as<std::string>().find("Inclusive") != std::string::npos);
  }
  require_json_response_ref(calendar, "200", "ProjectCalendarResponse");
  const auto calendar_data = schemas["ProjectCalendarResponse"]["properties"]["data"];
  CHECK(
      required_properties(calendar_data) ==
      std::vector<
          std::string>{"created_cards", "from", "milestones", "project_id", "to", "updated_cards"}
  );
}

TEST_CASE("OpenAPI contracts project Git sync status", "[openapi][holderctl-foundation][sync]") {
  const auto document = load_openapi();
  const auto schemas = document["components"]["schemas"];
  const auto sync_status = document["paths"]["/projects/{project_id}/git/sync-status"]["get"];
  REQUIRE(sync_status.IsDefined());

  const auto project_id = parameter_named(sync_status, "project_id");
  REQUIRE(project_id.IsDefined());
  CHECK(project_id["in"].as<std::string>() == "path");
  CHECK(project_id["required"].as<bool>());
  require_json_response_ref(sync_status, "200", "ProjectGitSyncStatusResponse");
  for (const auto& status : {"400", "401", "404"}) {
    require_json_response_ref(sync_status, status, "ErrorResponse");
  }

  const auto response_data = schemas["ProjectGitSyncStatusResponse"]["properties"]["data"];
  CHECK(required_properties(response_data) == std::vector<std::string>{"project_id", "sync"});
  CHECK(
      response_data["properties"]["sync"]["$ref"].as<std::string>() ==
      "#/components/schemas/ProjectSync"
  );

  auto sync_required = required_properties(schemas["ProjectSync"]);
  std::vector<std::string> expected{
      "last_commit_at",
      "last_pull_at",
      "last_pull_status",
      "last_push_at",
      "last_push_status",
      "last_sync_error",
      "last_sync_error_at",
      "next_pull_retry_at",
      "next_retry_at",
      "pull_retry_count",
      "retry_count",
      "uncommitted_changes_count",
      "unpushed_commits_count",
      "updated_at",
  };
  std::sort(expected.begin(), expected.end());
  CHECK(sync_required == expected);
  CHECK(
      schemas["ProjectSync"]["properties"]["pull_retry_count"]["type"].as<std::string>() ==
      "integer"
  );
  CHECK(schemas["ProjectSync"]["properties"]["next_pull_retry_at"]["nullable"].as<bool>());
}
