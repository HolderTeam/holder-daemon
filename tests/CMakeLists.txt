find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  find_package(Catch2 REQUIRED)
endif()

if(TARGET Catch2::Catch2)
  include(Catch)
endif()

add_executable(lock_helper
  lock_helper.cpp
  ../src/core/LockFile.cpp
)
target_include_directories(lock_helper PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

add_executable(lockfile_tests
  LockFile_test.cpp
  ServerInfo_test.cpp
  Signal_test.cpp
  HttpHealth_test.cpp
  HttpListener_test.cpp
  ListenerAcceptError_test.cpp
  HttpAiMessages_test.cpp
  HttpAiThreads_test.cpp
  HttpAiRouterConfig_test.cpp
  HttpAiProviderCatalog_test.cpp
  HttpAiProviderCredentials_test.cpp
  HttpAiMessageLinks_test.cpp
  HttpAiRuns_test.cpp
  HttpAiRunEvents_test.cpp
  HttpAiCapture_test.cpp
  HttpAiComplete_test.cpp
  HttpResources_test.cpp
  HttpTrash_test.cpp
  HttpRebuild_test.cpp
  HttpFirstRun_test.cpp
  HttpOpenFlow_test.cpp
  HttpProjects_test.cpp
  HttpDocs_test.cpp
  HttpProjectGitFailure_test.cpp
  HttpBadJson_test.cpp
  HttpAuth_test.cpp
  HttpModels_test.cpp
  HttpCloudProviders_test.cpp
  HttpRunner_test.cpp
  HttpCards_test.cpp
  HttpLinks_test.cpp
  HttpSearch_test.cpp
  HttpSearchDisabled_test.cpp
  HttpSearchFlow_test.cpp
  HttpSearchAiFlow_test.cpp
  Router_test.cpp
  Cli_test.cpp
  ProjectRepo_test.cpp
  CardRepo_test.cpp
  LinkRepo_test.cpp
  ResourceRepo_test.cpp
  AiThreadRepo_test.cpp
  AiMessageRepo_test.cpp
  AiRunRepo_test.cpp
  AiRouterConfigRepo_test.cpp
  AiProviderCredentialRepo_test.cpp
  Migrations_test.cpp
  Cascade_test.cpp
  CardPaths_test.cpp
  CardFrontMatter_test.cpp
  AiMessagePaths_test.cpp
  AiMessageFrontMatter_test.cpp
  ProjectPaths_test.cpp
  CardStore_test.cpp
  CardStoreFsFailure_test.cpp
  FtsIndexer_test.cpp
  GitRepo_test.cpp
  FsAdapterFailure_test.cpp
  GitAdapterFailure_test.cpp
  Reindexer_test.cpp
  ../src/core/LockFile.cpp
  ../src/core/Paths.cpp
  ../src/core/ServerInfo.cpp
  ../src/core/Signal.cpp
  ../src/core/Fs.cpp
  ../src/core/CardPaths.cpp
  ../src/core/AiMessagePaths.cpp
  ../src/core/CardFrontMatter.cpp
  ../src/core/AiMessageFrontMatter.cpp
  ../src/core/ProjectPaths.cpp
  ../src/index/FtsIndexer.cpp
  ../src/index/Reindexer.cpp
  ../src/llm/LocalModelRunner.cpp
  ../src/api/HttpServer.cpp
  ../src/api/Listener.cpp
  ../src/api/routes/ai/AiProviderRoutes.cpp
  ../src/api/routes/AuthenticatedRoutes.cpp
  ../src/api/routes/ai/AiDispatch.cpp
  ../src/api/routes/ai/AiMessageRoutes.cpp
  ../src/api/routes/AiResourceRoutes.cpp
  ../src/api/routes/ai/AiRunRoutes.cpp
  ../src/api/routes/ai/AiRunnerRoutes.cpp
  ../src/api/routes/ai/AiStatusRoutes.cpp
  ../src/api/routes/ai/AiThreadRoutes.cpp
  ../src/api/routes/CardRoutes.cpp
  ../src/api/routes/ProjectRoutes.cpp
  ../src/api/routes/RebuildRoutes.cpp
  ../src/api/routes/SearchRoutes.cpp
  ../src/api/routes/TrashRoutes.cpp
  ../src/api/routes/StaticRoutes.cpp
  ../src/api/support/PathDiscovery.cpp
  ../src/api/support/CloudConfig.cpp
  ../src/api/support/CloudClient.cpp
  ../src/api/support/CloudQuota.cpp
  ../src/api/support/LocalModelRouting.cpp
  ../src/api/support/RunEventStore.cpp
  ../src/api/Session.cpp
  ../src/api/Router.cpp
  ../src/git/GitRepo.cpp
  ../src/git/GitOps.cpp
  ../src/store/ProjectRepo.cpp
  ../src/store/CardRepo.cpp
  ../src/store/LinkRepo.cpp
  ../src/store/ResourceRepo.cpp
  ../src/store/AiThreadRepo.cpp
  ../src/store/AiMessageRepo.cpp
  ../src/store/AiRunRepo.cpp
  ../src/store/AiRouterConfigRepo.cpp
  ../src/store/AiProviderCredentialRepo.cpp
  ../src/store/Rebuilder.cpp
  ../src/store/Migrations.cpp
  ../src/store/CardStore.cpp
  ../src/store/Tx.cpp
  ../src/store/Db.cpp
)
target_include_directories(lockfile_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

if(TARGET Catch2::Catch2WithMain)
  target_link_libraries(lockfile_tests PRIVATE Catch2::Catch2WithMain)
else()
  target_sources(lockfile_tests PRIVATE catch_main.cpp)
  target_link_libraries(lockfile_tests PRIVATE Catch2::Catch2)
endif()

target_compile_definitions(lockfile_tests PRIVATE
  LOCK_HELPER_PATH="$<TARGET_FILE:lock_helper>"
  CARD_SERVER_VERSION="${PROJECT_VERSION}"
  SCHEMA_SQL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../schema/schema.sql"
  HOLDER_BIN_PATH="$<TARGET_FILE:holder>"
)

target_link_libraries(lockfile_tests PRIVATE
  Boost::system
  Boost::filesystem
  OpenSSL::SSL
  OpenSSL::Crypto
  PkgConfig::SQLITE3
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  XdgUtils::BaseDir
  yaml-cpp
  caste
)

if(TARGET PkgConfig::LIBGIT2)
  target_link_libraries(lockfile_tests PRIVATE PkgConfig::LIBGIT2)
endif()

if(COMMAND catch_discover_tests)
  catch_discover_tests(lockfile_tests)
else()
  add_test(NAME lockfile_tests COMMAND lockfile_tests)
endif()
