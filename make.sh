#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-default}"
BUILD_TYPE="${2:-RelWithDebInfo}"
CASTE_DIR="submodules/caste"
CASTE_COMMIT="f0728b046df27b9f8ff965a3fd4a5b94bcb65057"
CASTE_ARCHIVE_URL="https://github.com/zeth/caste/archive/${CASTE_COMMIT}.tar.gz"
CCACHE_BIN=""
CCACHE_STATE="unchecked"
CORE_CMAKE_ARGS=()
CORE_SDK_TOOLS_REF="249464d96a9c87f05b282e7a539ae524438ce6a5"

print_usage() {
  cat <<'EOF'
Usage:
  ./make.sh [command] [args...]
  ./make.sh [BuildType]

Commands:
  help, -h, --help              Show this help
  default                       Configure, build, run tests, then run holderd
  build [BuildType]             Configure and build holderd and holderctl
  core-update [ref] [BuildType]  Refresh the cached core selection and fetch its SDK
  test [BuildType]              Configure, build, and run the automated tests
  [BuildType]                   Run the default flow with this CMAKE_BUILD_TYPE
  coverage                      Build, run tests, and generate coverage reports
  warnings [BuildType]          Build holderd and holderctl with warnings as errors
  memcheck [test-regex]         Run Valgrind memcheck tests
  san [sanitizers] [BuildType]  Run sanitizer build and tests
  tidy                          Run clang-tidy through run-clang-tidy
  format                        Format C++ source and test files
  format-check                  Check C++ source and test formatting

Examples:
  ./make.sh
  ./make.sh Debug
  ./make.sh coverage
  ./make.sh warnings Debug
  ./make.sh san address,undefined
  HOLDER_SAN_DETECT_LEAKS=1 ./make.sh san

Environment:
  HOLDER_CCACHE                auto (default), 1 to require, or 0 to disable ccache
  HOLDER_CORE_SDK              Explicit SDK path (auto-fetched except on Fedora)
  HOLDER_CORE_REF              Resolve this tag/SHA instead of reusing the cached selection
  HOLDER_CORE_SDK_TOOL         Override the shared SDK selection tool
  HOLDER_USE_SYSTEM_CORE       1/ON to use the installed system core package
  HOLDER_CORE_SOURCE_DIR       Source override (Fedora defaults to ../holder-core)
  HOLDER_CTEST_TIMEOUT          Per-test timeout (memcheck defaults to 900 seconds)
  HOLDER_SAN_BUILD_DIR          Override the sanitizer build directory
  HOLDER_TSAN_SUPPRESSIONS      Optional explicit ThreadSanitizer suppression file
  HOLDER_CLANG_TIDY             Override clang-tidy (default clang-tidy-18)
  HOLDER_RUN_CLANG_TIDY         Override run-clang-tidy
  HOLDER_MEMCHECK_BUILD_TYPE    Override memcheck build type (SDK default RelWithDebInfo)
  HOLDER_SAN_DETECT_LEAKS       Set to 1 to enable ASan leak detection
EOF
}

prepare_ccache() {
  local setting="${HOLDER_CCACHE:-auto}"
  local required="false"

  if [ "${CCACHE_STATE}" != "unchecked" ]; then
    return
  fi

  case "${setting}" in
    auto)
      ;;
    1|on|true)
      required="true"
      ;;
    0|off|false)
      CCACHE_STATE="disabled"
      echo "ccache: disabled by HOLDER_CCACHE=${setting}"
      return
      ;;
    *)
      echo "Invalid HOLDER_CCACHE value: ${setting} (expected auto, 1, or 0)." >&2
      exit 2
      ;;
  esac

  if CCACHE_BIN="$(command -v ccache 2>/dev/null)"; then
    CCACHE_STATE="enabled"
    echo "ccache: enabled (${CCACHE_BIN})"
    "${CCACHE_BIN}" --show-stats
    return
  fi

  CCACHE_BIN=""
  CCACHE_STATE="disabled"
  if [ "${required}" = "true" ]; then
    echo "Missing dependency: ccache is required by HOLDER_CCACHE=${setting}." >&2
    exit 1
  fi
  echo "ccache: not found; building without a compiler cache." >&2
  echo "Install ccache with your package manager (see README.md for dependencies)." >&2
}

download_sdk_tool() {
  local destination="$1" url="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$destination"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$destination" "$url"
  else
    echo "Missing dependency: curl or wget is required to fetch the core SDK tool." >&2
    return 1
  fi
}

core_sdk_tool() {
  if [ -n "${HOLDER_CORE_SDK_TOOL:-}" ]; then
    printf '%s\n' "$HOLDER_CORE_SDK_TOOL"
    return
  fi
  if [ -f ../holder-core/scripts/core-sdk.py ]; then
    printf '%s\n' ../holder-core/scripts/core-sdk.py
    return
  fi
  local tools_dir=".core-sdk/tools/${CORE_SDK_TOOLS_REF}" file
  mkdir -p "$tools_dir" || return
  for file in core-sdk.py sdk_dependencies.py; do
    if [ ! -f "$tools_dir/$file" ]; then
      echo "Fetching core SDK tool: $file" >&2
      download_sdk_tool "$tools_dir/$file.tmp" \
        "https://raw.githubusercontent.com/HolderTeam/holder-core/${CORE_SDK_TOOLS_REF}/scripts/$file" || return
      mv "$tools_dir/$file.tmp" "$tools_dir/$file" || return
    fi
  done
  printf '%s\n' "$tools_dir/core-sdk.py"
}

fedora_source_default() {
  [ -z "${HOLDER_CORE_SOURCE_DIR:-}" ] && [ -z "${HOLDER_CORE_SDK:-}" ] || return 1
  case "${HOLDER_USE_SYSTEM_CORE:-OFF}" in
    0|OFF|off|FALSE|false|NO|no|"") ;;
    *) return 1 ;;
  esac
  [ "$(uname -s)" = Linux ] && [ -r /etc/os-release ] || return 1
  local distro_id
  distro_id="$(awk -F= '$1 == "ID" {gsub(/"/, "", $2); print $2}' /etc/os-release)"
  [ "$distro_id" = fedora ]
}

prepare_core() {
  local build_type="$1" tool python_bin selection="out/core-selection.json"
  if fedora_source_default; then
    if [ ! -f ../holder-core/CMakeLists.txt ]; then
      echo "Fedora requires a native core build; clone holder-core beside holder-daemon." >&2
      echo "Alternatively set HOLDER_CORE_SOURCE_DIR or HOLDER_CORE_SDK to a native build." >&2
      return 1
    fi
    HOLDER_CORE_SOURCE_DIR="$(cd ../holder-core && pwd)"
    export HOLDER_CORE_SOURCE_DIR
    echo "core: Fedora default uses the sibling checkout (native dependency ABI)"
  fi
  CORE_CMAKE_ARGS=("-DHOLDER_CORE_SOURCE_DIR=${HOLDER_CORE_SOURCE_DIR:-}"
                   "-DHOLDER_CORE_SDK=${HOLDER_CORE_SDK:-}" "-DHOLDER_USE_SYSTEM_CORE=OFF")
  if [ -n "${HOLDER_CORE_SOURCE_DIR:-}" ]; then
    case "${HOLDER_USE_SYSTEM_CORE:-OFF}" in
      1|ON|on|TRUE|true|YES|yes)
        echo "Select one core mode: HOLDER_CORE_SOURCE_DIR or HOLDER_USE_SYSTEM_CORE." >&2
        return 2 ;;
    esac
    echo "core: explicit source override ($HOLDER_CORE_SOURCE_DIR)"
    return
  fi
  case "${HOLDER_USE_SYSTEM_CORE:-OFF}" in
    1|ON|on|TRUE|true|YES|yes)
      CORE_CMAKE_ARGS=("-DHOLDER_CORE_SOURCE_DIR=" "-DHOLDER_CORE_SDK=" "-DHOLDER_USE_SYSTEM_CORE=ON")
      echo "core: installed system package"
      return
      ;;
    0|OFF|off|FALSE|false|NO|no|"") ;;
    *) echo "Invalid HOLDER_USE_SYSTEM_CORE value: $HOLDER_USE_SYSTEM_CORE" >&2; return 2 ;;
  esac
  if [ -n "${HOLDER_CORE_SDK:-}" ]; then
    echo "core: explicit SDK ($HOLDER_CORE_SDK)"
    return
  fi
  case "$build_type" in
    Release|RelWithDebInfo) ;;
    *)
      echo "Published core SDKs support RelWithDebInfo and Release, not $build_type." >&2
      echo "For $build_type, set HOLDER_CORE_SOURCE_DIR to an explicit core checkout." >&2
      return 2
      ;;
  esac
  if command -v python3 >/dev/null 2>&1; then
    python_bin=python3
  elif command -v python >/dev/null 2>&1; then
    python_bin=python
  else
    echo "Missing dependency: Python 3 is required to resolve the core SDK." >&2
    return 1
  fi
  tool="$(core_sdk_tool)"
  if [ ! -f "$selection" ] || [ -n "${HOLDER_CORE_REF:-}" ]; then
    echo "core: resolving ${HOLDER_CORE_REF:-latest-green}"
    "$python_bin" "$tool" resolve --core-ref "${HOLDER_CORE_REF:-latest-green}" --output "$selection"
  else
    echo "core: reusing $selection (./make.sh core-update refreshes it)"
  fi
  echo "core: fetching/verifying $build_type SDK"
  local sdk_path
  sdk_path="$("$python_bin" "$tool" fetch --selection "$selection" --cache .core-sdk --build-type "$build_type")"
  sdk_path="${sdk_path//$'\r'/}"
  CORE_CMAKE_ARGS=("-DHOLDER_CORE_SOURCE_DIR=" "-DHOLDER_CORE_SDK=$sdk_path" "-DHOLDER_USE_SYSTEM_CORE=OFF")
}

development_build_type() {
  if [ -n "${HOLDER_CORE_SOURCE_DIR:-}" ] || fedora_source_default; then printf '%s\n' Debug
  else printf '%s\n' RelWithDebInfo
  fi
}

cmake_configure() {
  local build_type=RelWithDebInfo arg
  for arg in "$@"; do
    case "$arg" in -DCMAKE_BUILD_TYPE=*) build_type="${arg#*=}" ;; esac
  done
  prepare_core "$build_type"
  prepare_ccache
  if [ "${CCACHE_STATE}" = "enabled" ]; then
    cmake "$@" "${CORE_CMAKE_ARGS[@]}" -DCMAKE_CXX_COMPILER_LAUNCHER="${CCACHE_BIN}"
  else
    # Clear a launcher cached by an earlier invocation when caching is now
    # explicitly disabled or ccache is no longer installed.
    cmake "$@" "${CORE_CMAKE_ARGS[@]}" -DCMAKE_CXX_COMPILER_LAUNCHER=
  fi
}

cmake_build() {
  cmake --build "$@"
  if [ "${CCACHE_STATE}" = "enabled" ]; then
    echo "ccache statistics after build:"
    "${CCACHE_BIN}" --show-stats
  fi
}

jobs() {
  if [ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]; then
    printf '%s\n' "${CMAKE_BUILD_PARALLEL_LEVEL}"
  elif [ -n "${NUMBER_OF_PROCESSORS:-}" ]; then
    printf '%s\n' "${NUMBER_OF_PROCESSORS}"
  elif command -v nproc >/dev/null 2>&1; then
    nproc
  else
    getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1
  fi
}

is_windows_shell() {
  case "${OS:-}:$(uname -s 2>/dev/null || true)" in
    Windows_NT:*|*:MINGW*|*:MSYS*|*:CYGWIN*) return 0 ;;
    *) return 1 ;;
  esac
}

case "${MODE}" in
  help|-h|--help)
    print_usage
    exit 0
    ;;
esac

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

if [ -f ".gitmodules" ] && grep -q "submodules/caste" ".gitmodules"; then
  if [ "${is_git_repo}" = "true" ]; then
    git submodule update --init --recursive -- submodules/caste
  fi
fi

if [ ! -f "${CASTE_DIR}/CMakeLists.txt" ]; then
  if [ "${is_git_repo}" = "true" ]; then
    echo "Missing dependency: ${CASTE_DIR}" >&2
    echo "Run: git submodule update --init --recursive -- submodules/caste" >&2
    exit 1
  fi

  echo "No git metadata detected. Downloading pinned caste dependency..." >&2
  download_caste_archive
fi

if [ ! -f "${CASTE_DIR}/CMakeLists.txt" ]; then
  echo "Failed to prepare dependency at ${CASTE_DIR}" >&2
  exit 1
fi

build_all() {
  cmake_configure -S . -B build -G Ninja -DCMAKE_BUILD_TYPE="${1}"
  JOBS="$(jobs)"
  cmake_build build -- -j "${JOBS}"
}

build_standard() {
  local build_type="${1:-RelWithDebInfo}"

  if is_windows_shell; then
    if [ -z "${VCPKG_ROOT:-}" ]; then
      echo "VCPKG_ROOT must name the Windows vcpkg installation before running ./make.sh build." >&2
      exit 1
    fi
    local preset="windows-sdk-tests"
    if [ -n "${HOLDER_CORE_SOURCE_DIR:-}" ]; then preset="windows-vcpkg-debug"; fi
    cmake_configure --preset "$preset" -DCMAKE_BUILD_TYPE="$build_type"
    cmake --build --preset "$preset" --parallel "$(jobs)"
    return
  fi

  build_all "${build_type}"
}

test_standard() {
  local build_type="${1:-RelWithDebInfo}"

  if is_windows_shell; then
    if [ -z "${VCPKG_ROOT:-}" ]; then
      echo "VCPKG_ROOT must name the Windows vcpkg installation before running ./make.sh test." >&2
      exit 1
    fi
    local preset="windows-sdk-tests"
    if [ -n "${HOLDER_CORE_SOURCE_DIR:-}" ]; then preset="windows-vcpkg-tests-debug"; fi
    cmake_configure --preset "$preset" -DCMAKE_BUILD_TYPE="$build_type"
    cmake --build --preset "$preset" --parallel "$(jobs)"
    ctest --preset "$preset"
    return
  fi

  build_all "${build_type}"
  test_build build
}

run_standard() {
  if is_windows_shell; then
    local preset=windows-sdk-tests
    if [ -n "${HOLDER_CORE_SOURCE_DIR:-}" ]; then preset=windows-vcpkg-tests-debug; fi
    "./out/build/$preset/holderd.exe"
  else
    ./build/holderd
  fi
}

warnings_all() {
  local build_dir="build-warnings"
  local build_type="${1:-$(development_build_type)}"

  cmake_configure -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DHOLDER_WARNINGS_AS_ERRORS=ON

  JOBS="$(jobs)"
  cmake_build "${build_dir}" --target holderd holderctl -- -j "${JOBS}"
}

memcheck_all() {
  local build_dir="build-memcheck"
  local build_type="${HOLDER_MEMCHECK_BUILD_TYPE:-$(development_build_type)}"
  local test_regex="${1:-}"
  local valgrind_bin
  local valgrind_options
  local suppression_file
  local memcheck_skip_regex

  if ! valgrind_bin="$(command -v valgrind)"; then
    echo "Missing dependency: valgrind is required for ./make.sh memcheck." >&2
    exit 1
  fi

  valgrind_options="--leak-check=full --show-leak-kinds=definite,possible --errors-for-leak-kinds=definite,possible --track-origins=yes"
  suppression_file="${PWD}/tools/valgrind/holder.supp"
  memcheck_skip_regex="Slow background route does not block foreground route|Slow background route does not block save lane route|Multiple configured runners do not block card save path under background saturation|Queued save request jumps ahead of queued non-save work at dispatch time"

  cmake_configure -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DMEMORYCHECK_COMMAND="${valgrind_bin}" \
    -DMEMORYCHECK_COMMAND_OPTIONS="${valgrind_options}" \
    -DMEMORYCHECK_SUPPRESSIONS_FILE="${suppression_file}"

  JOBS="$(jobs)"
  cmake_build "${build_dir}" -- -j "${JOBS}"

  if [ -n "${test_regex}" ]; then
    ctest --test-dir "${build_dir}" \
      -T memcheck \
      --output-on-failure \
      --timeout "${HOLDER_CTEST_TIMEOUT:-900}" \
      -E "${memcheck_skip_regex}" \
      -R "${test_regex}"
  else
    ctest --test-dir "${build_dir}" \
      -T memcheck \
      --output-on-failure \
      --timeout "${HOLDER_CTEST_TIMEOUT:-900}" \
      -E "${memcheck_skip_regex}"
  fi
}

test_build() {
  local build_dir="${1:?}"
  local mode="${2:-split}"
  local test_timeout="${HOLDER_CTEST_TIMEOUT:-30}"

  if [ "${mode}" = "single" ]; then
    ctest --test-dir "${build_dir}" --output-on-failure --timeout "${HOLDER_CTEST_TIMEOUT:-300}"
    return
  fi

  ctest --test-dir "${build_dir}" --output-on-failure -j 8 --timeout "${test_timeout}"
}

san_all() {
  local build_dir="${HOLDER_SAN_BUILD_DIR:-build-san}"
  local sanitizers="${1:-address}"
  local build_type="${2:-$(development_build_type)}"
  local san_flags="-fsanitize=${sanitizers} -fno-omit-frame-pointer -O1 -g"
  local detect_leaks="${HOLDER_SAN_DETECT_LEAKS:-0}"
  local catch_discovery="ON"
  local test_mode="split"
  local tsan_use_setarch="OFF"
  local tsan_options="halt_on_error=1:second_deadlock_stack=1"
  if [ -n "${HOLDER_TSAN_SUPPRESSIONS:-}" ]; then
    tsan_options="${tsan_options}:suppressions=${HOLDER_TSAN_SUPPRESSIONS}"
  fi

  case ",${sanitizers}," in
  *",thread,"*)
    catch_discovery="OFF"
    test_mode="single"
    tsan_use_setarch="ON"
    ;;
  esac

  cmake_configure -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DCMAKE_CXX_FLAGS="${san_flags}" \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=${sanitizers}" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=${sanitizers}" \
    -DHOLDER_CATCH_DISCOVER_TESTS="${catch_discovery}" \
    -DHOLDER_TSAN_USE_SETARCH="${tsan_use_setarch}"

  JOBS="$(jobs)"
  ASAN_OPTIONS="detect_leaks=${detect_leaks}:halt_on_error=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    cmake_build "${build_dir}" -- -j "${JOBS}"

  ASAN_OPTIONS="detect_leaks=${detect_leaks}:halt_on_error=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    TSAN_OPTIONS="${tsan_options}" \
    test_build "${build_dir}" "${test_mode}"
}

coverage_all() {
  local build_dir="build-coverage"
  local report_dir="${build_dir}/coverage"
  local info_base="${build_dir}/coverage-base.info"
  local info_tests="${build_dir}/coverage-tests.info"
  local info_total="${build_dir}/coverage.info"
  local coverage_json="${report_dir}/coverage.json"
  local gcov_executable="gcov"
  if command -v gcov-13 >/dev/null 2>&1; then
    gcov_executable="gcov-13"
  fi

  cmake_configure -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="$(development_build_type)" \
    -DCMAKE_CXX_FLAGS="--coverage -O0 -g" \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O0 -g"

  JOBS="$(jobs)"
  cmake_build "${build_dir}" -- -j "${JOBS}"

  lcov --directory "${build_dir}" --zerocounters
  lcov --capture --initial --directory "${build_dir}" --output-file "${info_base}" \
    --ignore-errors gcov,gcov \
    --rc geninfo_unexecuted_blocks=1
  ctest --test-dir "${build_dir}" --output-on-failure
  lcov --capture --directory "${build_dir}" --output-file "${info_tests}" \
    --ignore-errors gcov,gcov \
    --rc geninfo_unexecuted_blocks=1
  lcov --add-tracefile "${info_base}" --add-tracefile "${info_tests}" --output-file "${info_total}"
  lcov --remove "${info_total}" \
    '/usr/*' \
    '*/submodules/*' \
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
      --exclude 'submodules/' \
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
  local tidy_bin="${HOLDER_CLANG_TIDY:-clang-tidy-18}"
  local tidy_runner="${HOLDER_RUN_CLANG_TIDY:-run-clang-tidy}"
  local gcc_version gcc_major
  local tidy_extra_args=""

  source_regex="^${PWD}/(src|tests)/.*\\.(cpp|cc|cxx|h|hpp)$"

  for executable in "${tidy_bin}" "${tidy_runner}"; do
    if ! command -v "${executable}" >/dev/null 2>&1; then
      echo "Missing dependency: ${executable}; see README.md for static analysis tools." >&2
      exit 1
    fi
  done
  if command -v g++ >/dev/null 2>&1; then
    gcc_version="$(g++ -dumpfullversion -dumpversion)"
    gcc_major="${gcc_version%%.*}"
    if [ -d "/usr/include/c++/${gcc_major}" ]; then
      tidy_extra_args="${tidy_extra_args} -extra-arg=-isystem/usr/include/c++/${gcc_major}"
    fi
    if [ -d "/usr/include/x86_64-linux-gnu/c++/${gcc_major}" ]; then
      tidy_extra_args="${tidy_extra_args} -extra-arg=-isystem/usr/include/x86_64-linux-gnu/c++/${gcc_major}"
    fi
  fi

  cmake_configure -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="$(development_build_type)" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

  "${tidy_runner}" \
    -clang-tidy-binary "${tidy_bin}" \
    -p "${build_dir}" \
    -quiet \
    ${tidy_extra_args} \
    "${source_regex}"
}

format_files() {
  local format_bin="clang-format-18"
  local mode="${1:?}"
  local file_list
  local status

  if ! command -v "${format_bin}" >/dev/null 2>&1; then
    echo "Missing dependency: clang-format-18 is required for ./make.sh format and format-check." >&2
    echo "Install clang-format-18 and ensure it is available on PATH." >&2
    exit 1
  fi

  case "${mode}" in
    write)
      format_args="-i"
      ;;
    check)
      format_args="--dry-run --Werror"
      ;;
    *)
      echo "Unknown format mode: ${mode}" >&2
      exit 1
      ;;
  esac

  file_list="$(mktemp)"
  if command -v rg >/dev/null 2>&1; then
    rg --files -0 src tests -g '*.cpp' -g '*.cc' -g '*.cxx' -g '*.h' -g '*.hpp' >"${file_list}" || true
  else
    find src tests \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.h' -o -name '*.hpp' \) -print0 >"${file_list}"
  fi

  if [ ! -s "${file_list}" ]; then
    rm -f "${file_list}"
    echo "No C++ files found to format." >&2
    return
  fi

  if xargs -0 "${format_bin}" ${format_args} <"${file_list}"; then
    status=0
  else
    status=$?
  fi
  rm -f "${file_list}"
  return "${status}"
}

case "${MODE}" in
  default)
    test_standard "RelWithDebInfo"
    run_standard
    ;;
  core-update)
    if [ -n "${HOLDER_CORE_SOURCE_DIR:-}" ] || fedora_source_default; then
      echo "core-update refreshes SDKs; update a source checkout separately with Git." >&2
      exit 2
    fi
    HOLDER_CORE_REF="${2:-latest-green}" prepare_core "${3:-RelWithDebInfo}"
    ;;
  build)
    build_standard "${2:-RelWithDebInfo}"
    ;;
  test)
    test_standard "${2:-RelWithDebInfo}"
    ;;
  perf-privacy)
    echo 'Core performance tests live in holder-core. Run build/tests/holder_core_tests "CardStore encrypted project perf profile (manual)" there.' >&2
    exit 2
    ;;
  coverage)
    coverage_all
    ;;
  warnings)
    warnings_all "${2:-$(development_build_type)}"
    ;;
  memcheck)
    memcheck_all "${2:-}"
    ;;
  san)
    san_all "${2:-address}" "${3:-$(development_build_type)}"
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
    test_standard "${MODE}"
    run_standard
    ;;
esac
