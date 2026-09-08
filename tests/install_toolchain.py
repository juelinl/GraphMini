"""Test platform selection without claiming a native Windows build."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import toolchain


class ToolchainTests(unittest.TestCase):
    def test_defaults(self):
        for system, pair in (("Darwin", ("clang", "clang++")),
                             ("Linux", ("gcc", "g++")), ("Windows", ("cl", "cl"))):
            self.assertEqual(toolchain.default_compilers(system)[:2], pair)

    def test_explicit_selection_ignores_environment(self):
        with patch("toolchain.platform.system", return_value="Linux"), \
             patch("toolchain.shutil.which", side_effect=lambda name: "/tools/" + name), \
             patch.dict("os.environ", {"CC": "clang", "CXX": "clang++"}):
            cc, cxx, generator, tag = toolchain.select_toolchain()
            self.assertEqual((cc, cxx, generator, tag),
                             ("/tools/gcc", "/tools/g++", "Ninja", "linux-gcc"))
            self.assertIn("-DCMAKE_CXX_COMPILER=/tools/g++", toolchain.cmake_toolchain_args(cc, cxx, generator))

    def test_missing_compiler(self):
        with patch("toolchain.shutil.which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "Cannot find"):
                toolchain.select_toolchain()

    def test_override_pair(self):
        with self.assertRaisesRegex(RuntimeError, "together"):
            toolchain.select_toolchain(cxx="clang++")

    def test_windows_requires_single_config(self):
        with patch("toolchain.platform.system", return_value="Windows"), \
             patch("toolchain.shutil.which", side_effect=lambda name: "/tools/" + name):
            self.assertEqual(toolchain.select_toolchain()[1], "/tools/cl")
            with self.assertRaisesRegex(RuntimeError, "single-config"):
                toolchain.select_toolchain(generator="Visual Studio 17 2022")

    def test_cache_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "CMakeCache.txt").write_text(
                "CMAKE_C_COMPILER:FILEPATH=/tools/gcc\n"
                "CMAKE_CXX_COMPILER:FILEPATH=/tools/g++\nCMAKE_GENERATOR:INTERNAL=Ninja\n")
            toolchain.check_build_cache(directory, "/tools/gcc", "/tools/g++", "Ninja")
            with self.assertRaisesRegex(RuntimeError, "fresh --build-dir"):
                toolchain.check_build_cache(directory, "/tools/clang", "/tools/clang++", "Ninja")
            with self.assertRaisesRegex(RuntimeError, "another generator"):
                toolchain.check_build_cache(directory, "/tools/gcc", "/tools/g++", "Unix Makefiles")


if __name__ == "__main__":
    unittest.main()
