#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-default}"
BUILD_TYPE="${2:-RelWithDebInfo}"
CASTE_DIR="third_party/caste"
CASTE_COMMIT="4bd5d90075f1f2d26127fe0ba27fedd7bd450da9"
CASTE_ARCHIVE_URL="https://github.com/zeth/caste/archive/${CASTE_COMMIT}.tar.gz"

download_caste_archive() {
  local tmp_dir archive_path
  tmp_dir="$(mktemp -d)"
  archive_path="${tmp_dir}/caste.tar.gz"

  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "${CASTE_ARCHIVE_URL}" -o "${archive_path}"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "${archive_path}" "${CASTE_ARCHIVE_URL}"
  else
    echo "Missing dependency: curl or wget is required to download caste." >&2
    echo "Install curl/wget, or clone with git so submodules can be initialized." >&2
    rm -rf "${tmp_dir}"
    exit 1
  fi

  rm -rf "${CASTE_DIR}"
  mkdir -p "${CASTE_DIR}"
  tar -xzf "${archive_path}" --strip-components=1 -C "${CASTE_DIR}"
  rm -rf "${tmp_dir}"
}

is_git_repo=false
if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
  is_git_repo=true
fi

if [[ -f ".gitmodules" ]] && grep -q "third_party/caste" ".gitmodules"; then
  if [[ "${is_git_repo}" == "true" ]]; then
    git submodule update --init --recursive -- third_party/caste
  fi
fi

if [[ ! -f "${CASTE_DIR}/CMakeLists.txt" ]]; then
  if [[ "${is_git_repo}" == "true" ]]; then
    echo "Missing dependency: ${CASTE_DIR}" >&2
    echo "Run: git submodule update --init --recursive -- third_party/caste" >&2
    exit 1
  fi

  echo "No git metadata detected. Downloading pinned caste dependency..." >&2
  download_caste_archive
fi

if [[ ! -f "${CASTE_DIR}/CMakeLists.txt" ]]; then
  echo "Failed to prepare dependency at ${CASTE_DIR}" >&2
  exit 1
fi

build_all() {
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE="${1}"
  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  else
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
  fi
  cmake --build build -- -j "${JOBS}"
}

coverage_all() {
  local build_dir="build-coverage"
  local report_dir="${build_dir}/coverage"
  local info_base="${build_dir}/coverage-base.info"
  local info_tests="${build_dir}/coverage-tests.info"
  local info_total="${build_dir}/coverage.info"
  local -a lcov_capture_flags=(
    --ignore-errors gcov,gcov
    --rc geninfo_unexecuted_blocks=1
  )

  cmake -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="--coverage -O0 -g"

  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  else
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
  fi
  cmake --build "${build_dir}" -- -j "${JOBS}"

  lcov --directory "${build_dir}" --zerocounters
  lcov --capture --initial --directory "${build_dir}" --output-file "${info_base}" "${lcov_capture_flags[@]}"
  ctest --test-dir "${build_dir}" --output-on-failure
  lcov --capture --directory "${build_dir}" --output-file "${info_tests}" "${lcov_capture_flags[@]}"
  lcov --add-tracefile "${info_base}" --add-tracefile "${info_tests}" --output-file "${info_total}"
  lcov --remove "${info_total}" \
    '/usr/*' \
    '*/third_party/*' \
    '*/tests/*' \
    '*/CMakeFiles/*/CompilerIdCXX/*' \
    --output-file "${info_total}"
  genhtml "${info_total}" --output-directory "${report_dir}" --title "holder backend coverage"
  echo "Coverage report: ${report_dir}/index.html"
}

case "${MODE}" in
  default)
    build_all "RelWithDebInfo"
    SERIAL_TEST_REGEX="HTTP /health returns ok with valid token|HTTP /health reports db_ok false when DB is closed|Listener serves card nudge and ai status routes without regression|Listener worker-owned DB handles support concurrent mixed request load"
    ctest --test-dir build --output-on-failure -j 8 --timeout 30 -E "${SERIAL_TEST_REGEX}"
    ctest --test-dir build --output-on-failure --timeout 30 -R "${SERIAL_TEST_REGEX}"
    ./build/holderd
    ;;
  perf-privacy)
    build_all "${BUILD_TYPE}"
    ./build/tests/lockfile_tests "CardStore encrypted project perf profile (manual)"
    ;;
  coverage)
    coverage_all
    ;;
  *)
    # Backward-compatible: treat first arg as build type in default flow.
    build_all "${MODE}"
    ctest --test-dir build --output-on-failure
    ./build/holderd
    ;;
esac
