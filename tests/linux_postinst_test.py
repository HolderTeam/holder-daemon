import os
from pathlib import Path
import subprocess
import tempfile
import unittest


POSTINST = Path(__file__).resolve().parents[1] / "packaging/linux/debian/holder-daemon.postinst"


class HolderDaemonPostinstTests(unittest.TestCase):
    def test_configure_starts_active_user_services_except_in_staged_root(self):
        with tempfile.TemporaryDirectory(prefix="holder-postinst-test-") as directory:
            root = Path(directory)
            helper = root / "deb-systemd-invoke"
            helper.write_text("#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$HOLDER_TEST_CALLS\"\n")
            helper.chmod(0o755)
            calls = root / "calls"
            env = os.environ.copy()
            env.update({"PATH": f"{root}:{env['PATH']}", "HOLDER_TEST_CALLS": str(calls)})
            env.pop("DPKG_ROOT", None)

            subprocess.run([str(POSTINST), "configure", ""], env=env, check=True)
            self.assertEqual(calls.read_text(), "--user start holder-daemon.service\n")

            subprocess.run([str(POSTINST), "configure", "0.2.0"], env=env, check=True)
            self.assertEqual(calls.read_text(), "--user start holder-daemon.service\n" * 2)

            calls.unlink()
            subprocess.run([str(POSTINST), "abort-upgrade", ""], env=env, check=True)
            self.assertFalse(calls.exists())

            env["DPKG_ROOT"] = "/staged-root"
            subprocess.run([str(POSTINST), "configure", ""], env=env, check=True)
            self.assertFalse(calls.exists())


if __name__ == "__main__":
    unittest.main()
