cmake_minimum_required(VERSION 3.22)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" HOLDER_DAEMON_VERSION)
string(STRIP "${HOLDER_DAEMON_VERSION}" HOLDER_DAEMON_VERSION)
if(HOLDER_DAEMON_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?(\\+[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$")
  # CMake's project(VERSION) accepts only numeric components. Keep the full
  # SemVer for the daemon's reported version and expose its numeric core to
  # CMake's project version machinery.
  set(HOLDER_DAEMON_CMAKE_VERSION
      "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
else()
  message(FATAL_ERROR
    "VERSION must contain a semantic version like 0.1.0 or 2.0.0-dev")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/API_VERSION" HOLDER_DAEMON_API_VERSION)
string(STRIP "${HOLDER_DAEMON_API_VERSION}" HOLDER_DAEMON_API_VERSION)
if(NOT HOLDER_DAEMON_API_VERSION MATCHES "^[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "API_VERSION must contain a compatibility version like 0.1")
endif()

project(card_server VERSION "${HOLDER_DAEMON_CMAKE_VERSION}" LANGUAGES CXX)
include(GNUInstallDirs)

# ----------------------------
# Build options
# ----------------------------
option(CARD_SERVER_ENABLE_MD4C "Enable markdown parsing via md4c" ON)
option(CARD_SERVER_ENABLE_LIBGIT2 "Enable git support via libgit2" ON)
option(HOLDER_ENABLE_ENCRYPTED_GIT "Enable encrypted_git feature paths" ON)
option(HOLDER_USE_SYSTEM_CASTE "Use packaged libcaste instead of third_party/caste" OFF)
option(HOLDER_CATCH_DISCOVER_TESTS "Discover Catch2 tests as individual CTest tests" ON)
option(HOLDER_TSAN_USE_SETARCH "Run tests through setarch -R for GCC ThreadSanitizer" OFF)
option(HOLDER_WARNINGS_AS_ERRORS "Treat Holder target compiler warnings as errors" OFF)
set(HOLDER_CORE_SOURCE_DIR "" CACHE PATH "Path to holder-core source tree")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
  add_compile_definitions(
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
  )
endif()

# Sensible defaults for a service-like binary
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

# ----------------------------
# Dependencies
# ----------------------------
if(WIN32)
  set(HOLDER_USE_PKG_CONFIG_DEPS OFF)
else()
  set(HOLDER_USE_PKG_CONFIG_DEPS ON)
endif()

if(HOLDER_USE_PKG_CONFIG_DEPS)
  find_package(PkgConfig REQUIRED)
endif()
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Boost: Beast/Asio/System are header-only for our usage; Filesystem still
# needs a compiled component. Boost.Process is header-only on Noble but is a
# split compiled component on newer Ubuntu, so link it when its config exists.
find_package(Boost 1.83.0 CONFIG REQUIRED COMPONENTS filesystem)
find_package(boost_process CONFIG QUIET)

# JSON + logging (apt-friendly)
# Ubuntu packages:
#   nlohmann-json3-dev
#   libspdlog-dev (often brings libfmt-dev)
find_package(nlohmann_json 3.6.0 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(spdlog REQUIRED)
if(TARGET yaml-cpp::yaml-cpp)
  set(HOLDER_YAML_CPP_TARGET yaml-cpp::yaml-cpp)
else()
  set(HOLDER_YAML_CPP_TARGET yaml-cpp)
endif()

# sqlite3 + libsodium
if(WIN32)
  find_package(unofficial-sqlite3 CONFIG QUIET)
  if(TARGET unofficial::sqlite3::sqlite3)
    set(HOLDER_SQLITE3_TARGET unofficial::sqlite3::sqlite3)
  else()
    find_package(SQLite3 REQUIRED)
    set(HOLDER_SQLITE3_TARGET SQLite::SQLite3)
  endif()

  find_package(unofficial-sodium CONFIG REQUIRED)
  if(TARGET unofficial-sodium::sodium)
    set(HOLDER_SODIUM_TARGET unofficial-sodium::sodium)
  else()
    message(FATAL_ERROR "unofficial-sodium package did not provide unofficial-sodium::sodium")
  endif()
else()
  pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
  set(HOLDER_SQLITE3_TARGET PkgConfig::SQLITE3)
  set(HOLDER_SODIUM_TARGET PkgConfig::SODIUM)
endif()

# libgit2
if(CARD_SERVER_ENABLE_LIBGIT2)
  if(WIN32)
    find_package(unofficial-libgit2 CONFIG QUIET)
    find_package(libgit2 CONFIG QUIET)
    if(TARGET unofficial::libgit2::libgit2)
      set(HOLDER_LIBGIT2_TARGET unofficial::libgit2::libgit2)
    elseif(TARGET libgit2::libgit2package)
      set(HOLDER_LIBGIT2_TARGET libgit2::libgit2package)
    elseif(TARGET libgit2::libgit2)
      set(HOLDER_LIBGIT2_TARGET libgit2::libgit2)
    elseif(TARGET libgit2package)
      set(HOLDER_LIBGIT2_TARGET libgit2package)
    elseif(TARGET libgit2)
      set(HOLDER_LIBGIT2_TARGET libgit2)
    else()
      message(FATAL_ERROR "Could not find a usable libgit2 CMake target")
    endif()
  else()
    pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
    set(HOLDER_LIBGIT2_TARGET PkgConfig::LIBGIT2)
  endif()
endif()

# markdown parsing via md4c (apt: libmd4c-dev)
if(CARD_SERVER_ENABLE_MD4C)
  if(WIN32)
    find_package(unofficial-md4c CONFIG QUIET)
    find_package(md4c CONFIG QUIET)
    if(TARGET md4c::md4c)
      set(HOLDER_MD4C_TARGET md4c::md4c)
    elseif(TARGET unofficial::md4c::md4c)
      set(HOLDER_MD4C_TARGET unofficial::md4c::md4c)
    elseif(TARGET unofficial-md4c::md4c)
      set(HOLDER_MD4C_TARGET unofficial-md4c::md4c)
    elseif(TARGET md4c)
      set(HOLDER_MD4C_TARGET md4c)
    else()
      message(FATAL_ERROR "Could not find a usable md4c CMake target")
    endif()
  else()
    pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c)
    set(HOLDER_MD4C_TARGET PkgConfig::MD4C)
  endif()
endif()

# optional keyring integration (apt: libsecret-1-dev)
if(HOLDER_USE_PKG_CONFIG_DEPS AND NOT APPLE)
  pkg_check_modules(LIBSECRET IMPORTED_TARGET libsecret-1)
else()
  set(LIBSECRET_FOUND 0)
endif()
if(APPLE)
  find_library(HOLDER_SECURITY_FRAMEWORK Security)
  find_library(HOLDER_COREFOUNDATION_FRAMEWORK CoreFoundation)
  if(HOLDER_SECURITY_FRAMEWORK AND HOLDER_COREFOUNDATION_FRAMEWORK)
    set(HOLDER_HAVE_MACOS_KEYCHAIN 1)
  else()
    message(WARNING "macOS Keychain frameworks not found; using non-keyring secret fallback")
    set(HOLDER_HAVE_MACOS_KEYCHAIN 0)
  endif()
else()
  set(HOLDER_HAVE_MACOS_KEYCHAIN 0)
endif()
if(WIN32)
  set(HOLDER_HAVE_WINDOWS_CREDENTIAL_MANAGER 1)
else()
  set(HOLDER_HAVE_WINDOWS_CREDENTIAL_MANAGER 0)
endif()

# Holder depends on caste for hardware capability classification.
if(HOLDER_USE_SYSTEM_CASTE)
  find_package(caste CONFIG REQUIRED)
  set(HOLDER_CASTE_TARGET caste::caste)
else()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/caste/CMakeLists.txt")
    message(FATAL_ERROR
      "Missing dependency at third_party/caste. "
      "Run: git submodule update --init --recursive -- third_party/caste")
  endif()
  set(CASTE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CASTE_BUILD_CLI OFF CACHE BOOL "" FORCE)
  set(CASTE_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
  if(WIN32 OR APPLE)
    set(CASTE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  endif()
  add_subdirectory(third_party/caste)
  set(HOLDER_CASTE_TARGET caste)
endif()

if(NOT HOLDER_CORE_SOURCE_DIR)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/holder-core/CMakeLists.txt")
    set(HOLDER_CORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/holder-core")
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../holder-core/CMakeLists.txt")
    get_filename_component(HOLDER_CORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../holder-core" ABSOLUTE)
  else()
    message(FATAL_ERROR
      "Missing holder-core. Set HOLDER_CORE_SOURCE_DIR or place holder-core next to holder-daemon.")
  endif()
endif()
add_subdirectory("${HOLDER_CORE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/holder-core")

# config/WELCOME.md is authored in holder-core (shared with other libholder
# consumers); keep this source-tree copy in sync so find_welcome_markdown()'s
# existing search paths and install rule need no changes. Re-copied whenever
# CMake reconfigures, which it does automatically if the holder-core copy changes.
configure_file(
  "${HOLDER_CORE_SOURCE_DIR}/resources/WELCOME.md"
  "${CMAKE_CURRENT_SOURCE_DIR}/config/WELCOME.md"
  COPYONLY
)

# schema/schema.sql is authored in holder-core; same rationale as WELCOME.md above.
configure_file(
  "${HOLDER_CORE_SOURCE_DIR}/schema/schema.sql"
  "${CMAKE_CURRENT_SOURCE_DIR}/schema/schema.sql"
  COPYONLY
)

# ----------------------------
# Target
# ----------------------------
add_executable(holderd
  src/main.cpp
  src/app/DaemonApp.cpp
  src/app/Bootstrap.cpp
  src/app/Entrypoint.cpp
  src/app/FatalError.cpp
  src/platform/BaseDir.cpp
  src/platform/InstalledDataPath.cpp
  src/platform/Paths.cpp
  src/platform/LockFile.cpp
  src/platform/ServerInfo.cpp
  src/platform/Signal.cpp
  src/core/SerialExecutor.cpp
  src/core/ConcurrencyProfilePolicy.cpp
  src/ai/AiNudgeRepo.cpp
  src/ai/NudgeService.cpp
  src/ai/AiLocalModelConfigRepo.cpp
  src/llm/LocalModelRunner.cpp
  src/llm/LocalRunnerClient.cpp
  src/llm/ExecutorRunnerClient.cpp
  src/llm/RunnerModelRef.cpp
  src/llm/RunnerRegistry.cpp
  src/api/HttpServer.cpp
  src/api/Listener.cpp
  src/api/routes/ai/AiProviderRoutes.cpp
  src/api/routes/ai/providers/AiProviderCatalogRoutes.cpp
  src/api/routes/ai/providers/AiProviderCredentialRoutes.cpp
  src/api/routes/ai/AiNudgeRoutes.cpp
  src/api/routes/AuthenticatedRoutes.cpp
  src/api/routes/ai/AiDispatch.cpp
  src/api/routes/ai/AiMessageRoutes.cpp
  src/api/routes/ai/messages/AiMessageCaptureRoutes.cpp
  src/api/routes/ai/messages/AiMessageCrudRoutes.cpp
  src/api/routes/ai/messages/AiMessageLinkRoutes.cpp
  src/api/routes/AiResourceRoutes.cpp
  src/api/routes/ai/runs/AiRunPostRoute.cpp
  src/api/routes/ai/runs/AiRunQueryRoutes.cpp
  src/api/routes/ai/AiRunRoutes.cpp
  src/api/routes/ai/AiRunnerRoutes.cpp
  src/api/routes/ai/runner/AiRunnerPullEventRoutes.cpp
  src/api/routes/ai/runner/AiRunnerPullRoutes.cpp
  src/api/routes/ai/AiStatusRoutes.cpp
  src/api/routes/ai/status/AiCapabilitiesRoutes.cpp
  src/api/routes/ai/status/AiRuntimeStatusRoutes.cpp
  src/api/routes/ai/status/AiLocalModelConfigRoutes.cpp
  src/api/routes/ai/AiThreadRoutes.cpp
  src/api/routes/ai/threads/AiThreadCollectionRoutes.cpp
  src/api/routes/ai/threads/AiThreadItemRoutes.cpp
  src/api/routes/CardRoutes.cpp
  src/api/routes/ProjectRoutes.cpp
  src/api/routes/RebuildRoutes.cpp
  src/api/routes/ReindexRoutes.cpp
  src/api/routes/SearchRoutes.cpp
  src/api/routes/TrashRoutes.cpp
  src/api/routes/StaticRoutes.cpp
  src/api/support/Health.cpp
  src/api/support/PathDiscovery.cpp
  src/api/support/CloudConfig.cpp
  src/api/support/CloudClient.cpp
  src/api/support/CloudQuota.cpp
  src/api/support/ThreadCompaction.cpp
  src/api/support/LocalModelRouting.cpp
  src/api/support/RunEventStore.cpp
  src/api/Session.cpp
  src/api/Router.cpp
  src/git/ExecutorGitOps.cpp
  src/sync/ProjectSyncWorker.cpp
  # add more sources as you create them, e.g.:
  # src/api/router.cpp
  # src/store/sqlite_store.cpp
  # src/git/git_store.cpp
  # src/index/fts.cpp
  # src/markdown/parse_links.cpp
)

target_include_directories(holderd PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(holderd PRIVATE
  CARD_SERVER_API_VERSION="${HOLDER_DAEMON_API_VERSION}"
  CARD_SERVER_VERSION="${HOLDER_DAEMON_VERSION}"
  HOLDER_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/holder-daemon"
)
if(HOLDER_ENABLE_ENCRYPTED_GIT)
  target_compile_definitions(holderd PRIVATE
    HOLDER_ENABLE_ENCRYPTED_GIT=1
  )
else()
  target_compile_definitions(holderd PRIVATE HOLDER_ENABLE_ENCRYPTED_GIT=0)
endif()

# Warnings (GNU/Clang)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(holderd PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wno-unknown-pragmas
  )
  if(HOLDER_WARNINGS_AS_ERRORS)
    target_compile_options(holderd PRIVATE -Werror)
  endif()
endif()

# Link libs
target_link_libraries(holderd PRIVATE
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
  ${HOLDER_CASTE_TARGET}
)
if(TARGET Boost::system)
  target_link_libraries(holderd PRIVATE Boost::system)
endif()
if(TARGET Boost::process)
  target_link_libraries(holderd PRIVATE Boost::process)
endif()

# Feature-linked deps
if(CARD_SERVER_ENABLE_LIBGIT2)
  target_link_libraries(holderd PRIVATE ${HOLDER_LIBGIT2_TARGET})
  target_compile_definitions(holderd PRIVATE CARD_SERVER_HAVE_LIBGIT2=1)
else()
  target_compile_definitions(holderd PRIVATE CARD_SERVER_HAVE_LIBGIT2=0)
endif()

if(CARD_SERVER_ENABLE_MD4C)
  target_link_libraries(holderd PRIVATE ${HOLDER_MD4C_TARGET})
  target_compile_definitions(holderd PRIVATE CARD_SERVER_HAVE_MD4C=1)
else()
  target_compile_definitions(holderd PRIVATE CARD_SERVER_HAVE_MD4C=0)
endif()

if(LIBSECRET_FOUND)
  target_link_libraries(holderd PRIVATE PkgConfig::LIBSECRET)
  target_compile_definitions(holderd PRIVATE HOLDER_HAVE_LIBSECRET=1)
else()
  target_compile_definitions(holderd PRIVATE HOLDER_HAVE_LIBSECRET=0)
endif()
target_compile_definitions(holderd PRIVATE
  HOLDER_HAVE_MACOS_KEYCHAIN=${HOLDER_HAVE_MACOS_KEYCHAIN}
  HOLDER_HAVE_WINDOWS_CREDENTIAL_MANAGER=${HOLDER_HAVE_WINDOWS_CREDENTIAL_MANAGER}
)
if(HOLDER_HAVE_MACOS_KEYCHAIN)
  target_link_libraries(holderd PRIVATE
    "${HOLDER_SECURITY_FRAMEWORK}"
    "${HOLDER_COREFOUNDATION_FRAMEWORK}"
  )
endif()
if(HOLDER_HAVE_WINDOWS_CREDENTIAL_MANAGER)
  target_link_libraries(holderd PRIVATE Advapi32)
endif()

if(WIN32)
  add_custom_target(holder_runtime_data
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "${CMAKE_CURRENT_BINARY_DIR}/schema"
      "${CMAKE_CURRENT_BINARY_DIR}/config"
      "${CMAKE_CURRENT_BINARY_DIR}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/schema/schema.sql"
      "${CMAKE_CURRENT_BINARY_DIR}/schema/schema.sql"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/config/WELCOME.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/ai_catalog.yaml"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/git_providers.yaml"
      "${CMAKE_CURRENT_BINARY_DIR}/config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/openapi.yaml"
      "${CMAKE_CURRENT_BINARY_DIR}/openapi.yaml"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/swagger-ui"
      "${CMAKE_CURRENT_BINARY_DIR}/assets/swagger-ui"
    VERBATIM
  )
  add_dependencies(holderd holder_runtime_data)
endif()

add_executable(holderctl
  src/cli/holderctl.cpp
  src/cli/commands/Common.cpp
  src/cli/commands/DaemonCommands.cpp
  src/cli/commands/Support.cpp
  src/cli/commands/card.cpp
  src/cli/commands/links.cpp
  src/cli/commands/project.cpp
  src/cli/commands/recovery.cpp
  src/cli/commands/reindex.cpp
  src/cli/commands/resource.cpp
  src/cli/commands/trash.cpp
  src/cli/commands/SystemCommands.cpp
  src/platform/BaseDir.cpp
  src/platform/Paths.cpp
)
target_include_directories(holderctl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(holderctl PRIVATE
  CARD_SERVER_VERSION="${HOLDER_DAEMON_VERSION}"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(holderctl PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wno-unknown-pragmas
  )
  if(HOLDER_WARNINGS_AS_ERRORS)
    target_compile_options(holderctl PRIVATE -Werror)
  endif()
endif()
target_link_libraries(holderctl PRIVATE
  Holder::Core
  Boost::filesystem
  Threads::Threads
  nlohmann_json::nlohmann_json
)
if(TARGET Boost::process)
  target_link_libraries(holderctl PRIVATE Boost::process)
endif()
if(WIN32)
  target_link_libraries(holderctl PRIVATE Shell32)
endif()

# ----------------------------
# Install
# ----------------------------
install(TARGETS holderd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS holderctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES schema/schema.sql DESTINATION ${CMAKE_INSTALL_DATADIR}/holder-daemon/schema)
install(FILES
  config/WELCOME.md
  config/ai_catalog.yaml
  config/git_providers.yaml
  DESTINATION ${CMAKE_INSTALL_DATADIR}/holder-daemon/config
)
install(FILES openapi.yaml DESTINATION ${CMAKE_INSTALL_DATADIR}/holder-daemon)
install(DIRECTORY assets/swagger-ui DESTINATION ${CMAKE_INSTALL_DATADIR}/holder-daemon/assets)

# ----------------------------
# Tests
# ----------------------------
if(WIN32 AND NOT DEFINED BUILD_TESTING)
  set(BUILD_TESTING OFF CACHE BOOL "Build tests")
endif()
include(CTest)
if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()
