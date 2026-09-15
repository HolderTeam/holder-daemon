#include "http_test_helpers.h"
#include "TestCommand.h"

#include "model/Location.h"
#include "identity/Uuid.h"
#include "resource/AssetEnvelope.h"
#include "resource/LocationRepo.h"
#include "resource/ResourceStore.h"

#include <fstream>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

std::string read_export_bytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write_export_bytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  REQUIRE(file.is_open());
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  file.close();
  REQUIRE(file.good());
}

std::string quote_export_path(const std::filesystem::path& path) {
  return "\"" + path.string() + "\"";
}

} // namespace

TEST_CASE("holderctl resource export streams verified assets and isolates binary stdout", "[holderctl][export]") {
  namespace http = boost::beast::http;
  const auto dir = holder::test::make_temp_dir();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (dir / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (dir / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (dir / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "export-project", (dir / "project").string());
  holder::test::create_project(db, "foreign-project", (dir / "foreign-project").string());
  holder::project::ProjectRepo(db).update_name("export-project", "Home", 2);
  const auto project = *holder::project::ProjectRepo(db).get("export-project");
  const auto objects = dir / "objects";
  std::filesystem::create_directories(objects);
  holder::model::Location location;
  location.location_id = "export-location";
  location.project_id = project.project_id;
  location.name = "Exports";
  location.provider = "local_directory";
  holder::resource::LocationRepo(db).put(location);

  std::string binary;
  for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
  std::string large(9 * 1024 * 1024 + 7, '\0');
  for (std::size_t i = 0; i < large.size(); ++i) large[i] = static_cast<char>(i % 256);
  auto make_bundle = [&](const std::string& resource_id, const std::vector<std::string>& contents) {
    holder::model::ResourceBundle bundle;
    bundle.resource.resource_id = resource_id;
    bundle.resource.project_id = project.project_id;
    bundle.resource.type = "file";
    bundle.resource.label = "Export fixture";
    for (const auto& bytes : contents) {
      holder::model::Asset asset;
      asset.asset_id = holder::identity::uuid_v4();
      asset.resource_id = resource_id;
      asset.original_filename = "payload.bin";
      asset.media_type = "application/octet-stream";
      const auto source = dir / (asset.asset_id + ".source");
      write_export_bytes(source, bytes);
      const auto staged = holder::resource::stage_asset_file(source, objects / asset.asset_id,
          project, resource_id, asset.asset_id);
      asset.byte_size = staged.plaintext.byte_size;
      asset.plaintext_sha256 = staged.plaintext.sha256;
      holder::model::Placement placement;
      placement.placement_id = holder::identity::uuid_v4();
      placement.asset_id = asset.asset_id;
      placement.location_id = location.location_id;
      placement.object_key = asset.asset_id;
      placement.encoding = staged.encoding;
      placement.stored_byte_size = staged.stored.byte_size;
      placement.stored_sha256 = staged.stored.sha256;
      asset.placements.push_back(placement);
      bundle.assets.push_back(asset);
    }
    holder::resource::ResourceStore(db).put(bundle);
    return bundle;
  };
  const auto single = make_bundle(holder::identity::uuid_v4(), {binary});
  const auto empty = make_bundle(holder::identity::uuid_v4(), {""});
  const auto big = make_bundle(holder::identity::uuid_v4(), {large});
  const auto multiple = make_bundle(holder::identity::uuid_v4(), {binary, ""});
  const auto external = make_bundle(holder::identity::uuid_v4(), {});
  auto foreign = external;
  foreign.resource.resource_id = holder::identity::uuid_v4();
  foreign.resource.project_id = "foreign-project";
  holder::resource::ResourceStore(db).put(foreign);

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore cards(db, &fts);
  const std::string token = "export-token";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &cards, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try { bound = server.start(); }
  catch (const std::exception& error) { SKIP("Local HTTP socket unavailable: " + std::string(error.what())); }
  holder::core::SignalHandler signals;
  holder::test::HttpServerThreadGuard thread(server, signals);
  REQUIRE(holder::test::wait_for_http_listener(bound.bind, bound.port));
  holder::test::http_json_request(bound.bind, bound.port, token, http::verb::put,
      "/locations/" + location.location_id + "/binding",
      {{"values", {{"root_path", objects.string()}}}, {"preview", "Exports"}}, http::status::ok);

  const auto info = dir / "data" / "holder" / "server" / "holder.json";
  std::filesystem::create_directories(info.parent_path());
#ifdef _WIN32
  const int pid = _getpid();
#else
  const int pid = getpid();
#endif
  write_export_bytes(info, nlohmann::json({{"pid", pid}, {"bind", bound.bind},
      {"port", bound.port}, {"auth_token", token}}).dump());
#ifndef _WIN32
  std::filesystem::permissions(info.parent_path(), std::filesystem::perms::owner_all);
  std::filesystem::permissions(info, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
  const auto stdout_file = dir / "stdout.bin";
  const auto stderr_file = dir / "stderr.txt";
  const auto output = dir / "export with spaces.bin";
  const std::string command = quote_export_path(HOLDER_CTL_PATH) + " resource export ";
  auto run = [&](const std::string& arguments, int expected = 0) {
    REQUIRE(holder::test::run_system_command(command + arguments + " > " + quote_export_path(stdout_file) +
        " 2> " + quote_export_path(stderr_file)) == expected);
    return read_export_bytes(stdout_file);
  };
  auto run_error = [&](const std::string& arguments, const std::string& code) {
    REQUIRE(run(arguments + " --json", 1).empty());
    const auto error = nlohmann::json::parse(read_export_bytes(stderr_file));
    REQUIRE(error["ok"] == false);
    REQUIRE(error["error"]["code"] == code);
    return error;
  };
  const auto resource_id = single.resource.resource_id;
  const auto asset_id = single.assets[0].asset_id;
  REQUIRE(run(resource_id) == binary);
  REQUIRE(read_export_bytes(stderr_file).empty());
  REQUIRE(run(resource_id + " " + asset_id + " --output -") == binary);
  REQUIRE(read_export_bytes(stderr_file).empty());
  write_export_bytes(output, "original");
  REQUIRE(run(resource_id + " --output " + quote_export_path(output)).empty());
  REQUIRE(read_export_bytes(output) == binary);
  REQUIRE(read_export_bytes(stderr_file).find("Exported resource:") == 0);
  const auto result = nlohmann::json::parse(run(resource_id + " --output " + quote_export_path(output) + " --json"));
  REQUIRE(read_export_bytes(stderr_file).empty());
  REQUIRE(result["ok"] == true);
  REQUIRE(result["data"]["resource_id"] == resource_id);
  REQUIRE(result["data"]["asset_id"] == asset_id);
  REQUIRE(result["data"]["byte_size"] == binary.size());
  REQUIRE(result["data"]["content_type"] == "application/octet-stream");
  REQUIRE(result["data"]["filename"] == "payload.bin");
  REQUIRE(result["data"]["changed"] == true);
  REQUIRE(run(empty.resource.resource_id).empty());
  REQUIRE(read_export_bytes(stderr_file).empty());
  REQUIRE(run(empty.resource.resource_id + " --output " + quote_export_path(output)).empty());
  REQUIRE(read_export_bytes(output).empty());
  REQUIRE(run(big.resource.resource_id) == large);
  REQUIRE(read_export_bytes(stderr_file).empty());
  const auto large_result = nlohmann::json::parse(run(big.resource.resource_id + " --output " + quote_export_path(output) + " --json"));
  REQUIRE(large_result["data"]["byte_size"] == large.size());
  REQUIRE(read_export_bytes(output) == large);
  const auto ambiguous = run_error(multiple.resource.resource_id + " --output " + quote_export_path(output), "ambiguous_asset");
  REQUIRE(ambiguous["error"]["details"]["candidates"].size() == 2);
  REQUIRE(run(multiple.resource.resource_id + " " + multiple.assets[0].asset_id) == binary);
  REQUIRE(run(multiple.resource.resource_id + " " + multiple.assets[1].asset_id).empty());
  run_error(external.resource.resource_id + " --output " + quote_export_path(output), "no_exportable_asset");
  run_error(resource_id + " " + multiple.assets[0].asset_id + " --output " + quote_export_path(output), "asset_not_found");
  run_error("missing --output " + quote_export_path(output), "not_found");
  run_error(foreign.resource.resource_id + " --output " + quote_export_path(output), "cross_project_resource_forbidden");
  run_error(resource_id, "invalid_arguments");
  run_error(resource_id + " --output -", "invalid_arguments");
  run_error(resource_id + " --output", "invalid_arguments");
  // Retrieval errors are structured and cannot replace an existing output or leak JSON into bytes.
  write_export_bytes(output, "original");
  std::filesystem::remove(objects / single.assets[0].placements[0].object_key);
  REQUIRE(run(resource_id + " --output " + quote_export_path(output), 1).empty());
  REQUIRE(read_export_bytes(output) == "original");
  REQUIRE(run(resource_id, 1).empty());
  REQUIRE_FALSE(read_export_bytes(stderr_file).empty());
  run_error(resource_id + " --output " + quote_export_path(output), "storage_unavailable");
  REQUIRE(read_export_bytes(output) == "original");
  write_export_bytes(objects / big.assets[0].placements[0].object_key, "damaged stored asset");
  // The existing core envelope verifier reports an unclassified runtime error as bad_request.
  const auto damaged = run_error(big.resource.resource_id + " --output " + quote_export_path(output), "bad_request");
  REQUIRE(damaged["error"]["message"].get<std::string>().find("integrity check failed") != std::string::npos);
  REQUIRE(read_export_bytes(output) == "original");
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    REQUIRE(entry.path().filename().string().find(".holderctl-export-") != 0);
  }
}
