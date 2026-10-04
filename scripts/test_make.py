"""Exercise make.sh's SDK selection and default flow without compiling or running a Home."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPOSITORY = Path(__file__).resolve().parents[1]

SDK_TOOL = '''import json, os, sys
from pathlib import Path
args = sys.argv[1:]
with open(os.environ["MAKE_TEST_LOG"], "a") as log:
    log.write(json.dumps(["sdk", *args]) + "\\n")
if args[0] == "resolve":
    output = Path(args[args.index("--output") + 1])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"ref": args[args.index("--core-ref") + 1]}))
else:
    print(os.environ["MAKE_TEST_SDK"])
'''

FAKE_COMMAND = '''import json, os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
if name == "git":
    sys.exit(1)
if name == "awk":
    print("fedora" if os.environ.get("MAKE_TEST_FEDORA") else "ubuntu")
    sys.exit(0)
if name == "uname":
    print("MINGW64_NT" if os.environ.get("MAKE_TEST_WINDOWS") else "Linux")
    sys.exit(0)
with open(os.environ["MAKE_TEST_LOG"], "a") as log:
    log.write(json.dumps([name, *args]) + "\\n")
if name == "curl":
    if os.environ.get("MAKE_TEST_DOWNLOAD_FAIL"):
        sys.exit(7)
    destination = Path(args[args.index("-o") + 1])
    destination.write_text(Path(os.environ["MAKE_TEST_TOOL"]).read_text()
        if "core-sdk.py" in args[-3] else "# SDK helper dependency\\n")
if name == "cmake" and ("-S" in args or "--preset" in args):
    if "--preset" in args:
        build = Path("out/build") / args[args.index("--preset") + 1]
        executable = build / "holderd.exe"
    else:
        build = Path(args[args.index("-B") + 1])
        executable = build / "holderd"
    build.mkdir(parents=True, exist_ok=True)
    executable.write_text(Path(os.environ["MAKE_TEST_COMMAND"]).read_text())
    executable.chmod(0o755)
'''


class MakeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="holder make test ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "daemon"
        self.repo.mkdir()
        shutil.copy2(REPOSITORY / "make.sh", self.repo / "make.sh")
        caste = self.repo / "submodules/caste"
        caste.mkdir(parents=True)
        (caste / "CMakeLists.txt").write_text("# ready")
        self.tool = self.root / "holder-core/scripts/core-sdk.py"
        self.tool.parent.mkdir(parents=True)
        self.tool.write_text(SDK_TOOL)
        (self.tool.parent.parent / "CMakeLists.txt").write_text("# native core fixture")
        self.fixture_tool = self.root / "fixture-sdk.py"
        self.fixture_tool.write_text(SDK_TOOL)
        commands = self.root / "bin"
        commands.mkdir()
        command = f"#!{sys.executable}\n" + FAKE_COMMAND
        self.command_file = self.root / "command.py"
        self.command_file.write_text(command)
        for name in ("git", "uname", "awk", "cmake", "ctest", "curl", "valgrind", "lcov",
                     "genhtml", "gcovr", "clang-tidy-18", "run-clang-tidy"):
            path = commands / name
            path.write_text(command)
            path.chmod(0o755)
        self.log = self.root / "commands.jsonl"
        self.sdk = self.root / "verified sdk"
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(("HOLDER_", "MAKE_TEST_"))}
        self.env.update(PATH=str(commands) + os.pathsep + os.environ["PATH"],
                        HOLDER_CCACHE="0", CMAKE_BUILD_PARALLEL_LEVEL="2",
                        MAKE_TEST_LOG=str(self.log), MAKE_TEST_SDK=str(self.sdk),
                        MAKE_TEST_TOOL=str(self.fixture_tool),
                        MAKE_TEST_COMMAND=str(self.command_file))
        self.env.pop("OS", None)

    def run_make(self, *args, **environment):
        return subprocess.run(["bash", "./make.sh", *args], cwd=self.repo,
                              env=self.env | environment, text=True, capture_output=True)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def require_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def configure_call(self):
        return next(call for call in self.calls() if call[0] == "cmake" and "--build" not in call)

    def test_no_arguments_resolves_builds_tests_and_runs(self):
        self.require_success(self.run_make())
        self.assertEqual([call[0] for call in self.calls()],
                         ["sdk", "sdk", "cmake", "cmake", "ctest", "holderd"])
        configure = self.configure_call()
        self.assertIn("-DHOLDER_CORE_SDK=" + str(self.sdk), configure)
        self.assertIn("-DHOLDER_CORE_SOURCE_DIR=", configure)
        self.assertIn("-DCMAKE_BUILD_TYPE=RelWithDebInfo", configure)
        self.assertEqual(self.calls()[3][-1], "2")

    def test_cached_selection_reused_and_release_fetch_matches(self):
        self.require_success(self.run_make("build"))
        self.log.unlink()
        self.require_success(self.run_make("build", "Release"))
        self.assertEqual(self.calls()[0][1], "fetch")
        self.assertIn("Release", self.calls()[0])
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", self.configure_call())
        self.assertFalse(any(call[:2] == ["sdk", "resolve"] for call in self.calls()))

    def test_explicit_pin_and_core_update_refresh_selection(self):
        self.require_success(self.run_make("build", HOLDER_CORE_REF="v1.2.3"))
        self.assertIn("v1.2.3", self.calls()[0])
        self.log.unlink()
        self.require_success(self.run_make("core-update", "latest-green", "Release"))
        self.assertIn("latest-green", self.calls()[0])
        self.assertIn("Release", self.calls()[1])
        self.assertEqual(len(self.calls()), 2)

    def test_explicit_sdk_source_and_system_modes_skip_download(self):
        for environment, expected in (
            ({"HOLDER_CORE_SDK": str(self.sdk)}, "-DHOLDER_CORE_SDK=" + str(self.sdk)),
            ({"HOLDER_CORE_SOURCE_DIR": "../core source"}, "-DHOLDER_CORE_SOURCE_DIR=../core source"),
            ({"HOLDER_USE_SYSTEM_CORE": "ON"}, "-DHOLDER_USE_SYSTEM_CORE=ON"),
        ):
            with self.subTest(environment=environment):
                self.log.unlink(missing_ok=True)
                self.require_success(self.run_make("build", **environment))
                self.assertIn(expected, self.configure_call())
                self.assertFalse(any(call[0] in {"sdk", "curl"} for call in self.calls()))
        self.assertNotEqual(self.run_make("build", HOLDER_CORE_SOURCE_DIR="../core",
                                         HOLDER_USE_SYSTEM_CORE="ON").returncode, 0)

    def test_debug_requires_explicit_override_and_source_debug_works(self):
        result = self.run_make("build", "Debug")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("HOLDER_CORE_SOURCE_DIR", result.stderr)
        self.assertEqual(self.calls(), [])
        self.require_success(self.run_make("test", "Debug", HOLDER_CORE_SOURCE_DIR="../holder-core"))
        self.assertIn("-DCMAKE_BUILD_TYPE=Debug", self.configure_call())

    def test_diagnostic_commands_use_supported_sdk_configuration(self):
        for args in (("warnings",), ("san",), ("coverage",), ("memcheck",), ("tidy",)):
            with self.subTest(args=args):
                self.log.unlink(missing_ok=True)
                self.require_success(self.run_make(*args))
                fetch = next(call for call in self.calls() if call[:2] == ["sdk", "fetch"])
                self.assertIn("RelWithDebInfo", fetch)
                self.assertIn("-DCMAKE_BUILD_TYPE=RelWithDebInfo", self.configure_call())

    def test_standalone_checkout_bootstraps_shared_tool_once(self):
        shutil.rmtree(self.tool.parent.parent)
        self.require_success(self.run_make("build"))
        downloads = [call for call in self.calls() if call[0] == "curl"]
        self.assertEqual(len(downloads), 2)
        self.assertTrue(all("249464d96a9c87f05b282e7a539ae524438ce6a5" in call[-3] for call in downloads))
        self.log.unlink()
        self.require_success(self.run_make("build"))
        self.assertFalse(any(call[0] == "curl" for call in self.calls()))

    def test_bootstrap_download_failure_stops_before_configure(self):
        shutil.rmtree(self.tool.parent.parent)
        self.assertNotEqual(self.run_make("build", MAKE_TEST_DOWNLOAD_FAIL="1").returncode, 0)
        self.assertFalse(any(call[0] in {"cmake", "sdk"} for call in self.calls()))

    def test_windows_default_and_release_use_sdk_preset(self):
        windows = {"MAKE_TEST_WINDOWS": "1", "VCPKG_ROOT": str(self.root / "vcpkg")}
        self.require_success(self.run_make(**windows))
        self.assertEqual(self.calls()[-1][0], "holderd.exe")
        self.assertIn("windows-sdk-tests", self.configure_call())
        self.log.unlink()
        self.require_success(self.run_make("build", "Release", **windows))
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", self.configure_call())

    def test_fedora_automatically_uses_sibling_source(self):
        self.require_success(self.run_make(MAKE_TEST_FEDORA="1"))
        configure = self.configure_call()
        self.assertIn("-DHOLDER_CORE_SOURCE_DIR=" + str(self.tool.parent.parent), configure)
        self.assertIn("-DHOLDER_CORE_SDK=", configure)
        self.assertEqual([call[0] for call in self.calls()], ["cmake", "cmake", "ctest", "holderd"])
        self.log.unlink()
        self.require_success(self.run_make("warnings", MAKE_TEST_FEDORA="1"))
        self.assertIn("-DCMAKE_BUILD_TYPE=Debug", self.configure_call())

    def test_fedora_explicit_sdk_and_system_override_auto_source(self):
        for environment, expected in (
            ({"HOLDER_CORE_SDK": str(self.sdk)}, "-DHOLDER_CORE_SDK=" + str(self.sdk)),
            ({"HOLDER_USE_SYSTEM_CORE": "ON"}, "-DHOLDER_USE_SYSTEM_CORE=ON"),
        ):
            with self.subTest(environment=environment):
                self.log.unlink(missing_ok=True)
                self.require_success(self.run_make("build", MAKE_TEST_FEDORA="1", **environment))
                self.assertIn(expected, self.configure_call())
                self.assertIn("-DHOLDER_CORE_SOURCE_DIR=", self.configure_call())

    def test_fedora_without_sibling_fails_before_fetching_incompatible_sdk(self):
        shutil.rmtree(self.tool.parent.parent)
        result = self.run_make("build", MAKE_TEST_FEDORA="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("clone holder-core beside holder-daemon", result.stderr)
        self.assertEqual(self.calls(), [])

    def test_help_does_not_prepare_dependencies(self):
        shutil.rmtree(self.repo / "submodules")
        self.require_success(self.run_make("help"))
        self.assertEqual(self.calls(), [])


if __name__ == "__main__":
    unittest.main()
