cmake_minimum_required(VERSION 3.22)
project(card_server VERSION 0.1.0 LANGUAGES CXX)

# ----------------------------
# Build options
# ----------------------------
option(CARD_SERVER_ENABLE_MD4C "Enable markdown parsing via md4c" ON)
option(CARD_SERVER_ENABLE_LIBGIT2 "Enable git support via libgit2" ON)
option(HOLDER_ENABLE_ENCRYPTED_GIT "Enable encrypted_git feature paths" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sensible defaults for a service-like binary
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

# ----------------------------
# Dependencies
# ----------------------------
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)

# Boost: Beast is header-only; Asio lives under Boost.
# You still generally want Boost::system for networking.
# Boost.Process v2 usage requires Boost 1.80+.
# For ease of testing, we require 1.83+
find_package(Boost 1.83.0 REQUIRED COMPONENTS system filesystem)

# JSON + logging (apt-friendly)
# Ubuntu packages:
#   nlohmann-json3-dev
#   libspdlog-dev (often brings libfmt-dev)
find_package(nlohmann_json 3.6.0 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(spdlog REQUIRED)

# XDG base dirs helper (apt: xdg-utils-cxx-dev)
find_package(XdgUtils CONFIG REQUIRED COMPONENTS BaseDir)

# sqlite3 via pkg-config (robust on Linux)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

# libgit2 via pkg-config
if(CARD_SERVER_ENABLE_LIBGIT2)
  pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
endif()

# markdown parsing via md4c (apt: libmd4c-dev)
if(CARD_SERVER_ENABLE_MD4C)
  pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c)
endif()

# optional keyring integration (apt: libsecret-1-dev)
pkg_check_modules(LIBSECRET IMPORTED_TARGET libsecret-1)

# Holder depends on caste for hardware capability classification.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/caste/CMakeLists.txt")
  message(FATAL_ERROR
    "Missing dependency at third_party/caste. "
    "Run: git submodule update --init --recursive -- third_party/caste")
endif()
set(CASTE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CASTE_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(CASTE_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/caste)

# ----------------------------
# Target
# ----------------------------
add_executable(holder
  src/main.cpp
  src/platform/Paths.cpp
  src/platform/LockFile.cpp
  src/platform/ServerInfo.cpp
  src/platform/Signal.cpp
  src/platform/Fs.cpp
  src/card/CardPaths.cpp
  src/card/CardFrontMatter.cpp
  src/ai/AiMessagePaths.cpp
  src/ai/AiMessageFrontMatter.cpp
  src/project/ProjectPaths.cpp
  src/index/FtsIndexer.cpp
  src/index/Reindexer.cpp
  src/llm/LocalModelRunner.cpp
  src/api/HttpServer.cpp
  src/api/Listener.cpp
  src/api/routes/ai/AiProviderRoutes.cpp
  src/api/routes/ai/providers/AiProviderCatalogRoutes.cpp
  src/api/routes/ai/providers/AiProviderCredentialRoutes.cpp
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
  src/api/routes/ai/status/AiRouterConfigRoutes.cpp
  src/api/routes/ai/AiThreadRoutes.cpp
  src/api/routes/ai/threads/AiThreadCollectionRoutes.cpp
  src/api/routes/ai/threads/AiThreadItemRoutes.cpp
  src/api/routes/CardRoutes.cpp
  src/api/routes/ProjectRoutes.cpp
  src/api/routes/RebuildRoutes.cpp
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
  src/project/ProjectRepo.cpp
  src/project/ProjectSyncRepo.cpp
  src/card/CardRepo.cpp
  src/card/LinkRepo.cpp
  src/store/ResourceRepo.cpp
  src/ai/AiThreadRepo.cpp
  src/ai/AiMessageRepo.cpp
  src/ai/AiRunRepo.cpp
  src/ai/AiRouterConfigRepo.cpp
  src/ai/AiProviderCredentialRepo.cpp
  src/ai/AiProviderSettingRepo.cpp
  src/project/Rebuilder.cpp
  src/card/CardStore.cpp
  src/platform/Db.cpp
  src/platform/Tx.cpp
  src/platform/Migrations.cpp
  src/git/GitRepo.cpp
  src/git/GitOps.cpp
  src/git/RepoSyncMetrics.cpp
  src/privacy/ProjectPrivacy.cpp
  src/privacy/CryptoService.cpp
  src/sync/ProjectSyncPolicy.cpp
  src/sync/ProjectSyncWorker.cpp
  # add more sources as you create them, e.g.:
  # src/api/router.cpp
  # src/store/sqlite_store.cpp
  # src/git/git_store.cpp
  # src/index/fts.cpp
  # src/markdown/parse_links.cpp
)

target_include_directories(holder PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(holder PRIVATE
  CARD_SERVER_VERSION="${PROJECT_VERSION}"
)
if(HOLDER_ENABLE_ENCRYPTED_GIT)
  target_compile_definitions(holder PRIVATE
    HOLDER_ENABLE_ENCRYPTED_GIT=1
  )
else()
  target_compile_definitions(holder PRIVATE HOLDER_ENABLE_ENCRYPTED_GIT=0)
endif()

# Warnings (GNU/Clang)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(holder PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wno-unknown-pragmas
  )
endif()

# Link libs
target_link_libraries(holder PRIVATE
  Boost::system
  Boost::filesystem
  OpenSSL::SSL
  OpenSSL::Crypto
  PkgConfig::SQLITE3
  PkgConfig::SODIUM
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  XdgUtils::BaseDir
  yaml-cpp
  caste
)

# Feature-linked deps
if(CARD_SERVER_ENABLE_LIBGIT2)
  target_link_libraries(holder PRIVATE PkgConfig::LIBGIT2)
  target_compile_definitions(holder PRIVATE CARD_SERVER_HAVE_LIBGIT2=1)
else()
  target_compile_definitions(holder PRIVATE CARD_SERVER_HAVE_LIBGIT2=0)
endif()

if(CARD_SERVER_ENABLE_MD4C)
  target_link_libraries(holder PRIVATE PkgConfig::MD4C)
  target_compile_definitions(holder PRIVATE CARD_SERVER_HAVE_MD4C=1)
else()
  target_compile_definitions(holder PRIVATE CARD_SERVER_HAVE_MD4C=0)
endif()

if(LIBSECRET_FOUND)
  target_link_libraries(holder PRIVATE PkgConfig::LIBSECRET)
  target_compile_definitions(holder PRIVATE HOLDER_HAVE_LIBSECRET=1)
else()
  target_compile_definitions(holder PRIVATE HOLDER_HAVE_LIBSECRET=0)
endif()

# ----------------------------
# Install (optional for later packaging)
# ----------------------------
install(TARGETS holder RUNTIME DESTINATION bin)

# ----------------------------
# Tests
# ----------------------------
include(CTest)
if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()
