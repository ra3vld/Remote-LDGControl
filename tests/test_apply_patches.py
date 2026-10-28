"""Host regression tests: python -m unittest discover -s tests -p 'test_*.py'."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "apply_patches.py"
SPEC = importlib.util.spec_from_file_location("apply_patches", SCRIPT)
hook = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hook)

DIFF = b"""--- a/src/example.cpp
+++ b/src/example.cpp
@@ -1,3 +1,4 @@
 begin
-old
+new
+added
 end
"""


class ApplyPatchesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="patch tests ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.lib = self.root / "libdeps" / "windows" / "example"
        (self.lib / "src").mkdir(parents=True)
        self.target = self.lib / "src" / "example.cpp"
        self.target.write_bytes(b"begin\nold\nend\n")
        (self.root / "patches").mkdir()
        self.diff = self.root / "patches" / "example-fix.patch"
        self.diff.write_bytes(DIFF)
        self.output = io.StringIO()
        redirect = contextlib.redirect_stdout(self.output)
        redirect.__enter__()
        self.addCleanup(redirect.__exit__, None, None, None)

    def apply(self):
        hook.apply_patch(self.diff, self.lib)

    def test_apply_and_repeat_without_external_tools(self):
        with patch.dict(os.environ, {"PATH": ""}):
            self.apply()
            self.assertEqual(self.target.read_bytes(), b"begin\nnew\nadded\nend\n")
            self.apply()
        self.assertIn("already applied", self.output.getvalue())

    def test_windows_line_endings_and_patch_with_crlf(self):
        self.target.write_bytes(b"begin\r\nold\r\nend\r\n")
        self.diff.write_bytes(DIFF.replace(b"\n", b"\r\n"))
        self.apply()
        self.apply()
        self.assertEqual(self.target.read_bytes(), b"begin\r\nnew\r\nadded\r\nend\r\n")

    def test_context_can_move_after_other_patches(self):
        self.target.write_bytes(b"unrelated\nbegin\nold\nend\n")
        self.apply()
        self.assertEqual(self.target.read_bytes(), b"unrelated\nbegin\nnew\nadded\nend\n")

    def test_incompatible_dependency_fails_without_writing(self):
        content = b"begin\nupstream changed\nend\n"
        self.target.write_bytes(content)
        with self.assertRaisesRegex(RuntimeError, "could not apply"):
            self.apply()
        self.assertEqual(self.target.read_bytes(), content)

    def test_ambiguous_context_is_rejected(self):
        content = self.target.read_bytes() * 2
        self.target.write_bytes(content)
        with self.assertRaisesRegex(RuntimeError, "found 2"):
            self.apply()
        self.assertEqual(self.target.read_bytes(), content)

    def test_later_file_failure_leaves_first_file_untouched(self):
        other = self.lib / "src" / "other.cpp"
        other.write_bytes(b"upstream changed\n")
        self.diff.write_bytes(DIFF + DIFF.replace(b"example.cpp", b"other.cpp"))
        with self.assertRaises(RuntimeError):
            self.apply()
        self.assertEqual(self.target.read_bytes(), b"begin\nold\nend\n")
        self.assertEqual(other.read_bytes(), b"upstream changed\n")

    def test_later_hunk_failure_leaves_file_untouched(self):
        self.diff.write_bytes(DIFF + b"@@ -10 +11 @@\n-missing\n+replacement\n")
        with self.assertRaises(RuntimeError):
            self.apply()
        self.assertEqual(self.target.read_bytes(), b"begin\nold\nend\n")

    def test_multiple_hunks_are_idempotent(self):
        self.target.write_bytes(b"begin\nold\nend\nuntouched\nlast\n")
        self.diff.write_bytes(DIFF + b"@@ -5 +6 @@\n-last\n+final\n")
        self.apply()
        self.apply()
        self.assertEqual(self.target.read_bytes(), b"begin\nnew\nadded\nend\nuntouched\nfinal\n")

    def test_partially_applied_patch_is_rejected(self):
        self.target.write_bytes(b"begin\nnew\nadded\nend\nuntouched\nlast\n")
        original = self.target.read_bytes()
        self.diff.write_bytes(DIFF + b"@@ -5 +6 @@\n-last\n+final\n")
        with self.assertRaises(RuntimeError):
            self.apply()
        self.assertEqual(self.target.read_bytes(), original)

    def test_malformed_and_unsafe_patches_are_rejected(self):
        for content in (
            b"", DIFF.replace(b"-1,3", b"-1,30"),
            DIFF.replace(b"example.cpp", b"../../outside.cpp"),
            DIFF.replace(b"src/example.cpp", b"C:/outside.cpp"),
            DIFF.replace(b"src/example.cpp", b"/outside.cpp"),
            DIFF.replace(b"src/example.cpp", b"..\\outside.cpp"),
        ):
            with self.subTest(diff=content):
                self.diff.write_bytes(content)
                with self.assertRaises(ValueError):
                    self.apply()
                self.assertEqual(self.target.read_bytes(), b"begin\nold\nend\n")

    def test_scons_hook_runs_without_path(self):
        class Env(dict):
            def subst(inner, value):
                self.assertEqual(value, "$PROJECT_LIBDEPS_DIR")
                return str(self.root / "libdeps")

        env = Env(PROJECT_DIR=str(self.root), PIOENV="windows")
        with patch.dict(os.environ, {"PATH": ""}):
            for _ in range(2):
                runpy.run_path(str(SCRIPT), init_globals={"Import": lambda name: None, "env": env})
        self.assertEqual(self.target.read_bytes(), b"begin\nnew\nadded\nend\n")


if __name__ == "__main__":
    unittest.main()
