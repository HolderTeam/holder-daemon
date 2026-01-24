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
  HttpServer_test.cpp
  Router_test.cpp
  ProjectRepo_test.cpp
  CardRepo_test.cpp
  LinkRepo_test.cpp
  ResourceRepo_test.cpp
  AiThreadRepo_test.cpp
  AiMessageRepo_test.cpp
  ../src/core/LockFile.cpp
  ../src/core/ServerInfo.cpp
  ../src/core/Signal.cpp
  ../src/api/HttpServer.cpp
  ../src/api/Listener.cpp
  ../src/api/Session.cpp
  ../src/api/Router.cpp
  ../src/store/ProjectRepo.cpp
  ../src/store/CardRepo.cpp
  ../src/store/LinkRepo.cpp
  ../src/store/ResourceRepo.cpp
  ../src/store/AiThreadRepo.cpp
  ../src/store/AiMessageRepo.cpp
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
)

target_link_libraries(lockfile_tests PRIVATE
  Boost::system
  PkgConfig::SQLITE3
  nlohmann_json::nlohmann_json
  spdlog::spdlog
)

if(COMMAND catch_discover_tests)
  catch_discover_tests(lockfile_tests)
else()
  add_test(NAME lockfile_tests COMMAND lockfile_tests)
endif()
