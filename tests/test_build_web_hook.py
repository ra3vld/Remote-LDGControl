"""Host build preflight, including Windows with a stale VS Code PATH."""
import importlib.util
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "build_web.py"
SPEC = importlib.util.spec_from_file_location("build_web_hook", SCRIPT)
hook = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hook)


class FakeEnvironment:
    def __init__(self, project):
        self.project = project
        self.replaced = {}

    def subst(self, value):
        return str(self.project if value == "$PROJECT_DIR" else self.project / "build")

    def Replace(self, **values):
        self.replaced.update(values)


class BuildWebHookTests(unittest.TestCase):
    def test_path_takes_precedence(self):
        with patch.object(hook.shutil, "which", return_value="/custom/bin/node"):
            self.assertEqual(hook.find_node(), "/custom/bin/node")

    def test_windows_installer_and_nvm_paths_without_path_entry(self):
        with tempfile.TemporaryDirectory(prefix="Node installation ") as directory:
            root = Path(directory)
            for variable, suffix in [
                ("ProgramW6432", "nodejs"), ("ProgramFiles", "nodejs"),
                ("ProgramFiles(x86)", "nodejs"),
                ("LOCALAPPDATA", "Programs/nodejs"), ("LOCALAPPDATA", "nodejs"),
                ("NVM_SYMLINK", ""),
            ]:
                with self.subTest(variable=variable, suffix=suffix):
                    executable = root / suffix / "node.exe"
                    executable.parent.mkdir(parents=True, exist_ok=True)
                    executable.touch()
                    with patch.object(hook.shutil, "which", return_value=None), \
                         patch.object(hook.sys, "platform", "win32"), \
                         patch.dict(os.environ, {variable: directory}, clear=True):
                        self.assertEqual(hook.find_node(), str(executable))
                    executable.unlink()

    def test_missing_node_explains_install_and_restart(self):
        with patch.object(hook.shutil, "which", return_value=None), \
             patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(RuntimeError, "Install Node.js LTS.*restart VS Code.*npm ci"):
                hook.find_node()

    def test_old_node_does_not_start_generation(self):
        env = FakeEnvironment(Path("project"))
        with patch.object(hook, "find_node", return_value="node"), \
             patch.object(hook.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "v16.20.2\n")) as run:
            with self.assertRaisesRegex(RuntimeError, "18\\+ is required"):
                hook.build(env)
        self.assertEqual(run.call_count, 1)
        self.assertEqual(env.replaced, {})

    def test_missing_dependencies_explain_npm_ci(self):
        env = FakeEnvironment(Path("project"))
        with patch.object(hook, "find_node", return_value="node"), \
             patch.object(hook.subprocess, "run", side_effect=[
                 subprocess.CompletedProcess([], 0, "v24.19.0\n"),
                 subprocess.CalledProcessError(1, "node"),
             ]):
            with self.assertRaisesRegex(RuntimeError, "Web build dependencies are missing.*npm ci"):
                hook.build(env)
        self.assertEqual(env.replaced, {})

    def test_absolute_executable_and_project_paths_are_separate_arguments(self):
        env = FakeEnvironment(Path("project with spaces"))
        node = "Program Files/nodejs/node.exe"
        with patch.object(hook, "find_node", return_value=node), \
             patch.object(hook.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "v24.19.0\n")) as run:
            hook.build(env)
        args, kwargs = run.call_args
        self.assertEqual(args[0], [node, str(env.project / "scripts/build_web.cjs"),
                                   "--out", str(env.project / "build/web-assets")])
        self.assertEqual(kwargs, {"cwd": env.project, "check": True})
        self.assertEqual(env.replaced, {"PROJECT_DATA_DIR": str(env.project / "build/web-assets")})

    def test_scons_entry_reports_missing_node_without_traceback(self):
        with patch.object(hook.shutil, "which", return_value=None), \
             patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(SystemExit, r"\[build_web\].*Node.js 18\+"):
                runpy.run_path(str(SCRIPT), init_globals={
                    "Import": lambda name: None, "env": FakeEnvironment(Path("project")),
                })


if __name__ == "__main__":
    unittest.main()
