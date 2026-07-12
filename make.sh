#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-default}"
BUILD_TYPE="${2:-RelWithDebInfo}"
CASTE_DIR="third_party/caste"
CASTE_COMMIT="f0728b046df27b9f8ff965a3fd4a5b94bcb65057"
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

warnings_all() {
  local build_dir="build-warnings"
  local build_type="${1:-Debug}"

  cmake -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DHOLDER_WARNINGS_AS_ERRORS=ON

  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  else
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
  fi
  cmake --build "${build_dir}" --target holderd holderctl -- -j "${JOBS}"
}

memcheck_all() {
  local build_dir="build-memcheck"
  local build_type="${HOLDER_MEMCHECK_BUILD_TYPE:-Debug}"
  local test_regex="${1:-}"
  local valgrind_bin
  local valgrind_options
  local suppression_file
  local memcheck_skip_regex
  local -a ctest_args

  if ! valgrind_bin="$(command -v valgrind)"; then
    echo "Missing dependency: valgrind is required for ./make.sh memcheck." >&2
    exit 1
  fi

  valgrind_options="--leak-check=full --show-leak-kinds=definite,possible --errors-for-leak-kinds=definite,possible --track-origins=yes"
  suppression_file="${PWD}/tools/valgrind/holder.supp"
  memcheck_skip_regex="Slow background route does not block foreground route|Slow background route does not block save lane route|Multiple configured runners do not block card save path under background saturation|Queued save request jumps ahead of queued non-save work at dispatch time"

  cmake -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DMEMORYCHECK_COMMAND="${valgrind_bin}" \
    -DMEMORYCHECK_COMMAND_OPTIONS="${valgrind_options}" \
    -DMEMORYCHECK_SUPPRESSIONS_FILE="${suppression_file}"

  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  else
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
  fi
  cmake --build "${build_dir}" -- -j "${JOBS}"

  ctest_args=(
    --test-dir "${build_dir}"
    -T memcheck
    --output-on-failure
    --timeout 300
    -E "${memcheck_skip_regex}"
  )
  if [[ -n "${test_regex}" ]]; then
    ctest_args+=(-R "${test_regex}")
  fi
  ctest "${ctest_args[@]}"
}

test_build() {
  local build_dir="${1:?}"
  local mode="${2:-split}"
  local serial_test_regex

  if [[ "${mode}" == "single" ]]; then
    ctest --test-dir "${build_dir}" --output-on-failure --timeout 300
    return
  fi

  serial_test_regex="HTTP /health returns ok with valid token|HTTP /health reports db_ok false when DB is closed|Listener serves card nudge and ai status routes without regression|Listener worker-owned DB handles support concurrent mixed request load"
  ctest --test-dir "${build_dir}" --output-on-failure -j 8 --timeout 30 -E "${serial_test_regex}"
  ctest --test-dir "${build_dir}" --output-on-failure --timeout 30 -R "${serial_test_regex}"
}

san_all() {
  local build_dir="build-san"
  local sanitizers="${1:-address}"
  local build_type="${2:-Debug}"
  local san_flags="-fsanitize=${sanitizers} -fno-omit-frame-pointer -O1 -g"
  local detect_leaks="${HOLDER_SAN_DETECT_LEAKS:-0}"
  local catch_discovery="ON"
  local test_mode="split"
  local tsan_use_setarch="OFF"

  if [[ ",${sanitizers}," == *",thread,"* ]]; then
    catch_discovery="OFF"
    test_mode="single"
    tsan_use_setarch="ON"
  fi

  cmake -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DCMAKE_CXX_FLAGS="${san_flags}" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=${sanitizers}" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=${sanitizers}" \
    -DHOLDER_CATCH_DISCOVER_TESTS="${catch_discovery}" \
    -DHOLDER_TSAN_USE_SETARCH="${tsan_use_setarch}"

  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  else
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
  fi
  ASAN_OPTIONS="detect_leaks=${detect_leaks}:halt_on_error=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    cmake --build "${build_dir}" -- -j "${JOBS}"

  ASAN_OPTIONS="detect_leaks=${detect_leaks}:halt_on_error=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" \
    test_build "${build_dir}" "${test_mode}"
}

coverage_all() {
  local build_dir="build-coverage"
  local report_dir="${build_dir}/coverage"
  local info_base="${build_dir}/coverage-base.info"
  local info_tests="${build_dir}/coverage-tests.info"
  local info_total="${build_dir}/coverage.info"
  local coverage_json="${report_dir}/coverage.json"
  local -a lcov_capture_flags=(
    --ignore-errors gcov,gcov
    --rc geninfo_unexecuted_blocks=1
  )
  local gcov_executable="gcov"
  if command -v gcov-13 >/dev/null 2>&1; then
    gcov_executable="gcov-13"
  fi

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
  if command -v gcovr >/dev/null 2>&1; then
    gcovr \
      --root . \
      --object-directory "${build_dir}" \
      --filter 'src/' \
      --exclude 'tests/' \
      --exclude 'third_party/' \
      --gcov-executable "${gcov_executable}" \
      --gcov-ignore-errors all \
      --exclude-pattern-prefix LCOV \
      --exclude-unreachable-branches \
      --exclude-throw-branches \
      --exclude-function-lines \
      --json-pretty \
      --output "${coverage_json}"
    echo "Coverage JSON:   ${coverage_json}"
  else
    echo "Coverage JSON:   skipped (gcovr not found)" >&2
  fi
  echo "Coverage report: ${report_dir}/index.html"
}

tidy_all() {
  local build_dir="build-tidy"
  local source_regex
  local tidy_bin="clang-tidy"
  local gcc_version gcc_major
  local -a tidy_extra_args=()

  source_regex="^${PWD}/(src|tests)/.*\\.(cpp|cc|cxx|h|hpp)$"

  if command -v clang-tidy-18 >/dev/null 2>&1; then
    tidy_bin="clang-tidy-18"
  fi
  if command -v g++ >/dev/null 2>&1; then
    gcc_version="$(g++ -dumpfullversion -dumpversion)"
    gcc_major="${gcc_version%%.*}"
    if [[ -d "/usr/include/c++/${gcc_major}" ]]; then
      tidy_extra_args+=("-extra-arg=-isystem/usr/include/c++/${gcc_major}")
    fi
    if [[ -d "/usr/include/x86_64-linux-gnu/c++/${gcc_major}" ]]; then
      tidy_extra_args+=("-extra-arg=-isystem/usr/include/x86_64-linux-gnu/c++/${gcc_major}")
    fi
  fi

  cmake -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

  run-clang-tidy \
    -clang-tidy-binary "${tidy_bin}" \
    -p "${build_dir}" \
    -quiet \
    "${tidy_extra_args[@]}" \
    "${source_regex}"
}

format_files() {
  local format_bin="clang-format"
  local mode="${1:?}"
  local -a format_args=()
  local -a files=()

  if command -v clang-format-18 >/dev/null 2>&1; then
    format_bin="clang-format-18"
  fi

  case "${mode}" in
    write)
      format_args=(-i)
      ;;
    check)
      format_args=(--dry-run --Werror)
      ;;
    *)
      echo "Unknown format mode: ${mode}" >&2
      exit 1
      ;;
  esac

  if command -v rg >/dev/null 2>&1; then
    while IFS= read -r -d '' file; do
      files+=("${file}")
    done < <(rg --files -0 src tests -g '*.cpp' -g '*.cc' -g '*.cxx' -g '*.h' -g '*.hpp')
  else
    while IFS= read -r -d '' file; do
      files+=("${file}")
    done < <(find src tests \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.h' -o -name '*.hpp' \) -print0)
  fi

  if [[ "${#files[@]}" -eq 0 ]]; then
    echo "No C++ files found to format." >&2
    return
  fi

  "${format_bin}" "${format_args[@]}" "${files[@]}"
}

case "${MODE}" in
  default)
    build_all "RelWithDebInfo"
    test_build build
    ./build/holderd
    ;;
  perf-privacy)
    build_all "${BUILD_TYPE}"
    ./build/tests/lockfile_tests "CardStore encrypted project perf profile (manual)"
    ;;
  coverage)
    coverage_all
    ;;
  warnings)
    warnings_all "${2:-Debug}"
    ;;
  memcheck)
    memcheck_all "${2:-}"
    ;;
  san)
    san_all "${2:-address}" "${3:-Debug}"
    ;;
  tidy)
    tidy_all
    ;;
  format)
    format_files write
    ;;
  format-check)
    format_files check
    ;;
  *)
    # Backward-compatible: treat first arg as build type in default flow.
    build_all "${MODE}"
    ctest --test-dir build --output-on-failure
    ./build/holderd
    ;;
esac
