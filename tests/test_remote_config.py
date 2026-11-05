"""Host-only tests for the controller's optional fixed-IP configuration.

Uses the real configuration header, not ESP-IDF or a connected device.
Run: python -m unittest discover -s tests -v
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "xiaozhi-esp32-main3" / "main"


class RemoteConfigurationTests(unittest.TestCase):
    def check_config(self, private_config, expected_with_saved, expected_empty):
        compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("a C++17 compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            shutil.copyfile(MAIN / "lucky_cat_remote_config.h", directory / "lucky_cat_remote_config.h")
            if private_config is not None:
                (directory / "lucky_cat_private_config.h").write_text(private_config)
            (directory / "main.cc").write_text(
                '#include "lucky_cat_remote_config.h"\n'
                '#include <iostream>\n'
                'int main() {\n'
                '    std::cout << lucky_cat_remote_config::ResolveUrl("ws://old-host:9999/device") << "\\n";\n'
                '    std::cout << lucky_cat_remote_config::ResolveUrl("") << "\\n";\n'
                '}\n'
            )
            executable = directory / "remote-config-test"
            subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                str(directory / "main.cc"), "-o", str(executable),
            ], check=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout, f"{expected_with_saved}\n{expected_empty}\n")

    def test_missing_private_file_preserves_existing_fallback(self):
        self.check_config(None, "ws://old-host:9999/device", "")

    def test_private_ip_overrides_saved_url(self):
        self.check_config('#define ZHIMU_LUCKY_CAT_SERVER_IP "192.0.2.10"\n',
                          "ws://192.0.2.10:8080/device", "ws://192.0.2.10:8080/device")

    def test_changed_ip_uses_new_target(self):
        self.check_config('#define ZHIMU_LUCKY_CAT_SERVER_IP "192.0.2.20"\n',
                          "ws://192.0.2.20:8080/device", "ws://192.0.2.20:8080/device")

    def test_empty_private_ip_disables_remote_even_with_saved_url(self):
        self.check_config('#define ZHIMU_LUCKY_CAT_SERVER_IP ""\n', "", "")

    def test_private_file_without_ip_preserves_existing_fallback(self):
        self.check_config("#pragma once\n", "ws://old-host:9999/device", "")

    def test_unmodified_example_disables_remote(self):
        self.check_config((MAIN / "lucky_cat_private_config.example.h").read_text(), "", "")


if __name__ == "__main__":
    unittest.main()
