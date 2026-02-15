cmake_minimum_required(VERSION 3.22)
project(card_server VERSION 0.1.0 LANGUAGES CXX)

# ----------------------------
# Build options
# ----------------------------
option(CARD_SERVER_ENABLE_MD4C "Enable markdown parsing via md4c" ON)
option(CARD_SERVER_ENABLE_LIBGIT2 "Enable git support via libgit2" ON)

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

# libgit2 via pkg-config
if(CARD_SERVER_ENABLE_LIBGIT2)
  pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
endif()

# markdown parsing via md4c (apt: libmd4c-dev)
if(CARD_SERVER_ENABLE_MD4C)
  pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c)
endif()

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
  src/core/Paths.cpp
  src/core/LockFile.cpp
  src/core/ServerInfo.cpp
  src/core/Signal.cpp
  src/core/Fs.cpp
  src/core/CardPaths.cpp
  src/core/CardFrontMatter.cpp
  src/core/AiMessagePaths.cpp
  src/core/AiMessageFrontMatter.cpp
  src/core/ProjectPaths.cpp
  src/index/FtsIndexer.cpp
  src/index/Reindexer.cpp
  src/llm/LocalModelRunner.cpp
  src/api/HttpServer.cpp
  src/api/Listener.cpp
  src/api/routes/AiProviderRoutes.cpp
  src/api/routes/AiMessageRoutes.cpp
  src/api/routes/AiResourceRoutes.cpp
  src/api/routes/AiRunRoutes.cpp
  src/api/routes/AiRunnerRoutes.cpp
  src/api/routes/AiStatusRoutes.cpp
  src/api/routes/AiThreadRoutes.cpp
  src/api/routes/CardRoutes.cpp
  src/api/routes/ProjectRoutes.cpp
  src/api/routes/TrashRoutes.cpp
  src/api/routes/StaticRoutes.cpp
  src/api/support/PathDiscovery.cpp
  src/api/support/CloudConfig.cpp
  src/api/support/CloudClient.cpp
  src/api/support/CloudQuota.cpp
  src/api/support/LocalModelRouting.cpp
  src/api/support/RunEventStore.cpp
  src/api/Session.cpp
  src/api/Router.cpp
  src/store/ProjectRepo.cpp
  src/store/CardRepo.cpp
  src/store/LinkRepo.cpp
  src/store/ResourceRepo.cpp
  src/store/AiThreadRepo.cpp
  src/store/AiMessageRepo.cpp
  src/store/AiRunRepo.cpp
  src/store/AiRouterConfigRepo.cpp
  src/store/AiProviderCredentialRepo.cpp
  src/store/Rebuilder.cpp
  src/store/CardStore.cpp
  src/store/Db.cpp
  src/store/Tx.cpp
  src/store/Migrations.cpp
  src/git/GitRepo.cpp
  src/git/GitOps.cpp
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
