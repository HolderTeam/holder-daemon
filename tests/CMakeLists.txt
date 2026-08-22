find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  find_package(Catch2 REQUIRED)
endif()

if(TARGET Catch2::Catch2)
  include(Catch)
endif()

add_executable(lock_helper
  lock_helper.cpp
  ../src/platform/LockFile.cpp
)
target_include_directories(lock_helper PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(lock_helper PRIVATE
  Boost::filesystem
)

add_executable(holder_daemon_tests
  LockFile_test.cpp
  ServerInfo_test.cpp
  Signal_test.cpp
  SerialExecutor_test.cpp
  ConcurrencyProfilePolicy_test.cpp
  ExecutorGitOps_test.cpp
  ExecutorRunnerClient_test.cpp
  RunnerModelRef_test.cpp
  HttpHealth_test.cpp
  HttpListener_test.cpp
  ListenerAcceptError_test.cpp
  HttpAiMessages_test.cpp
  AiDispatch_test.cpp
  AiResourceRoutes_test.cpp
  AiMessageRoutes_test.cpp
  AiMessageCrudRoutes_test.cpp
  HttpAiThreads_test.cpp
  AiThreadRoutes_test.cpp
  AiThreadCollectionRoutes_test.cpp
  AiThreadItemRoutes_test.cpp
  HttpAiLocalModelConfig_test.cpp
  HttpAiProviderCatalog_test.cpp
  HttpAiProviderCredentials_test.cpp
  AiProviderRoutes_test.cpp
  AiNudgeRoutes_test.cpp
  NudgeService_test.cpp
  AiRuntimeStatusRoutes_test.cpp
  HttpAiMessageLinks_test.cpp
  HttpAiRuns_test.cpp
  AiRunRoutes_test.cpp
  AiRunQueryRoutes_test.cpp
  HttpAiRunEvents_test.cpp
  HttpAiRunnerPullEvents_test.cpp
  HttpAiRunnerPullRoutes_test.cpp
  AiRunnerRoutes_test.cpp
  HttpAiCapture_test.cpp
  AiMessageCaptureRoutes_test.cpp
  HttpAiComplete_test.cpp
  HttpResources_test.cpp
  HttpTrash_test.cpp
  TrashRoutes_test.cpp
  HttpRebuild_test.cpp
  RebuildRoutes_test.cpp
  ReindexRoutes_test.cpp
  ProjectRoutes_test.cpp
  HttpFirstRun_test.cpp
  HttpOpenFlow_test.cpp
  HttpProjects_test.cpp
  HttpProjectRecoveryToken_test.cpp
  HttpProjectEncryptionCheck_test.cpp
  HttpProjectGitFailure_test.cpp
  HttpProjectGitTestRemote_test.cpp
  HttpProjectGitPush_test.cpp
  HttpBadJson_test.cpp
  HttpAuth_test.cpp
  StaticRoutes_test.cpp
  HttpRunner_test.cpp
  LocalModelRunner_test.cpp
  PathDiscovery_test.cpp
  Session_test.cpp
  CloudQuota_test.cpp
  CloudConfig_test.cpp
  CloudClient_test.cpp
  LocalModelRouting_test.cpp
  ThreadCompaction_test.cpp
  HttpCards_test.cpp
  CardRoutes_test.cpp
  HttpLinks_test.cpp
  HttpSearch_test.cpp
  SearchRoutes_test.cpp
  HttpSearchDisabled_test.cpp
  HttpSearchFlow_test.cpp
  HttpSearchAiFlow_test.cpp
  Entrypoint_test.cpp
  FatalError_test.cpp
  Router_test.cpp
  Cli_test.cpp
  HolderCtl_test.cpp
  ProjectSyncWorker_test.cpp
  AiLocalModelConfigRepo_test.cpp
  RunnerRegistry_test.cpp
  Paths_test.cpp
  ../src/platform/LockFile.cpp
  ../src/app/Entrypoint.cpp
  ../src/app/FatalError.cpp
  ../src/platform/BaseDir.cpp
  ../src/platform/Paths.cpp
  ../src/platform/ServerInfo.cpp
  ../src/platform/Signal.cpp
  ../src/core/SerialExecutor.cpp
  ../src/core/ConcurrencyProfilePolicy.cpp
  ../src/ai/AiNudgeRepo.cpp
  ../src/ai/NudgeService.cpp
  ../src/ai/AiLocalModelConfigRepo.cpp
  ../src/llm/LocalModelRunner.cpp
  ../src/llm/LocalRunnerClient.cpp
  ../src/llm/ExecutorRunnerClient.cpp
  ../src/llm/RunnerModelRef.cpp
  ../src/llm/RunnerRegistry.cpp
  ../src/api/HttpServer.cpp
  ../src/api/Listener.cpp
  ../src/api/routes/ai/AiProviderRoutes.cpp
  ../src/api/routes/ai/providers/AiProviderCatalogRoutes.cpp
  ../src/api/routes/ai/providers/AiProviderCredentialRoutes.cpp
  ../src/api/routes/ai/AiNudgeRoutes.cpp
  ../src/api/routes/AuthenticatedRoutes.cpp
  ../src/api/routes/ai/AiDispatch.cpp
  ../src/api/routes/ai/AiMessageRoutes.cpp
  ../src/api/routes/ai/messages/AiMessageCaptureRoutes.cpp
  ../src/api/routes/ai/messages/AiMessageCrudRoutes.cpp
  ../src/api/routes/ai/messages/AiMessageLinkRoutes.cpp
  ../src/api/routes/AiResourceRoutes.cpp
  ../src/api/routes/ai/runs/AiRunPostRoute.cpp
  ../src/api/routes/ai/runs/AiRunQueryRoutes.cpp
  ../src/api/routes/ai/AiRunRoutes.cpp
  ../src/api/routes/ai/AiRunnerRoutes.cpp
  ../src/api/routes/ai/runner/AiRunnerPullEventRoutes.cpp
  ../src/api/routes/ai/runner/AiRunnerPullRoutes.cpp
  ../src/api/routes/ai/AiStatusRoutes.cpp
  ../src/api/routes/ai/status/AiCapabilitiesRoutes.cpp
  ../src/api/routes/ai/status/AiRuntimeStatusRoutes.cpp
  ../src/api/routes/ai/status/AiLocalModelConfigRoutes.cpp
  ../src/api/routes/ai/AiThreadRoutes.cpp
  ../src/api/routes/ai/threads/AiThreadCollectionRoutes.cpp
  ../src/api/routes/ai/threads/AiThreadItemRoutes.cpp
  ../src/api/routes/CardRoutes.cpp
  ../src/api/routes/ProjectRoutes.cpp
  ../src/api/routes/RebuildRoutes.cpp
  ../src/api/routes/ReindexRoutes.cpp
  ../src/api/routes/SearchRoutes.cpp
  ../src/api/routes/TrashRoutes.cpp
  ../src/api/routes/StaticRoutes.cpp
  ../src/api/support/Health.cpp
  ../src/api/support/PathDiscovery.cpp
  ../src/platform/InstalledDataPath.cpp
  ../src/api/support/CloudConfig.cpp
  ../src/api/support/CloudClient.cpp
  ../src/api/support/CloudQuota.cpp
  ../src/api/support/ThreadCompaction.cpp
  ../src/api/support/LocalModelRouting.cpp
  ../src/api/support/RunEventStore.cpp
  ../src/api/Session.cpp
  ../src/api/Router.cpp
  ../src/git/ExecutorGitOps.cpp
  ../src/sync/ProjectSyncWorker.cpp
  ../src/app/Bootstrap.cpp
)
target_include_directories(holder_daemon_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
if(MSVC)
  target_compile_options(holder_daemon_tests PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/WinEnvCompat.h")
endif()

if(TARGET Catch2::Catch2)
  target_sources(holder_daemon_tests PRIVATE catch_main.cpp)
  target_link_libraries(holder_daemon_tests PRIVATE Catch2::Catch2)
elseif(TARGET Catch2::Catch2WithMain)
  target_link_libraries(holder_daemon_tests PRIVATE Catch2::Catch2WithMain)
endif()

target_compile_definitions(holder_daemon_tests PRIVATE
  LOCK_HELPER_PATH="$<TARGET_FILE:lock_helper>"
  CARD_SERVER_API_VERSION="${HOLDER_DAEMON_API_VERSION}"
  CARD_SERVER_VERSION="${HOLDER_DAEMON_VERSION}"
  SCHEMA_SQL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../schema/schema.sql"
  HOLDER_BIN_PATH="$<TARGET_FILE:holderd>"
  HOLDER_CTL_PATH="$<TARGET_FILE:holderctl>"
)
add_dependencies(holder_daemon_tests lock_helper holderd holderctl)

if(HOLDER_ENABLE_ENCRYPTED_GIT)
  target_compile_definitions(holder_daemon_tests PRIVATE
    HOLDER_ENABLE_ENCRYPTED_GIT=1
  )
else()
  target_compile_definitions(holder_daemon_tests PRIVATE HOLDER_ENABLE_ENCRYPTED_GIT=0)
endif()

target_link_libraries(holder_daemon_tests PRIVATE
  Holder::Core
  Boost::filesystem
  OpenSSL::SSL
  OpenSSL::Crypto
  ${HOLDER_SQLITE3_TARGET}
  ${HOLDER_SODIUM_TARGET}
  Threads::Threads
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  ${HOLDER_YAML_CPP_TARGET}
  caste
)
if(TARGET Boost::system)
  target_link_libraries(holder_daemon_tests PRIVATE Boost::system)
endif()
if(TARGET Boost::process)
  target_link_libraries(holder_daemon_tests PRIVATE Boost::process)
endif()

if(CARD_SERVER_ENABLE_LIBGIT2)
  target_link_libraries(holder_daemon_tests PRIVATE ${HOLDER_LIBGIT2_TARGET})
  target_compile_definitions(holder_daemon_tests PRIVATE CARD_SERVER_HAVE_LIBGIT2=1)
else()
  target_compile_definitions(holder_daemon_tests PRIVATE CARD_SERVER_HAVE_LIBGIT2=0)
endif()

if(TARGET PkgConfig::LIBSECRET)
  target_link_libraries(holder_daemon_tests PRIVATE PkgConfig::LIBSECRET)
  target_compile_definitions(holder_daemon_tests PRIVATE HOLDER_HAVE_LIBSECRET=1)
else()
  target_compile_definitions(holder_daemon_tests PRIVATE HOLDER_HAVE_LIBSECRET=0)
endif()
target_compile_definitions(holder_daemon_tests PRIVATE
  HOLDER_HAVE_MACOS_KEYCHAIN=${HOLDER_HAVE_MACOS_KEYCHAIN}
  HOLDER_HAVE_WINDOWS_CREDENTIAL_MANAGER=${HOLDER_HAVE_WINDOWS_CREDENTIAL_MANAGER}
)
if(HOLDER_HAVE_MACOS_KEYCHAIN)
  target_link_libraries(holder_daemon_tests PRIVATE
    "${HOLDER_SECURITY_FRAMEWORK}"
    "${HOLDER_COREFOUNDATION_FRAMEWORK}"
  )
endif()
if(HOLDER_HAVE_WINDOWS_CREDENTIAL_MANAGER)
  target_link_libraries(holder_daemon_tests PRIVATE Advapi32)
endif()

set(HOLDER_TEST_ENVIRONMENT
  "HOLDER_TEST_KEYSTORE_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-keystore"
)

if(HOLDER_CATCH_DISCOVER_TESTS AND COMMAND catch_discover_tests)
  catch_discover_tests(holder_daemon_tests
    PROPERTIES
      ENVIRONMENT "${HOLDER_TEST_ENVIRONMENT}"
      SKIP_RETURN_CODE 4
  )
elseif(HOLDER_TSAN_USE_SETARCH)
  find_program(SETARCH_EXECUTABLE setarch)
  if(SETARCH_EXECUTABLE)
    add_test(NAME holder_daemon_tests COMMAND ${SETARCH_EXECUTABLE} x86_64 -R $<TARGET_FILE:holder_daemon_tests>)
  else()
    message(WARNING "setarch not found; TSan test may fail before running tests")
    add_test(NAME holder_daemon_tests COMMAND holder_daemon_tests)
  endif()
  set_tests_properties(holder_daemon_tests PROPERTIES ENVIRONMENT "${HOLDER_TEST_ENVIRONMENT}")
else()
  add_test(NAME holder_daemon_tests COMMAND holder_daemon_tests)
  set_tests_properties(holder_daemon_tests PROPERTIES ENVIRONMENT "${HOLDER_TEST_ENVIRONMENT}")
endif()
