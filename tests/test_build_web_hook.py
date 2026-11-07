"""Host tool bootstrap and Windows builds without manual terminal commands."""
from contextlib import contextmanager
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/build_web.py"
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
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="Web build with spaces ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "package.json").write_text('{"name":"example"}')
        (self.root / "package-lock.json").write_text('{"lockfileVersion":3}')
        self.stamp = self.root / ".pio/web-tools/dependencies.sha256"

    def test_path_takes_precedence(self):
        with patch.object(hook.shutil, "which", return_value="/custom/bin/node"):
            self.assertEqual(hook.find_node(), "/custom/bin/node")

    def test_windows_installer_and_nvm_paths_without_path_entry(self):
        for variable, suffix in [("ProgramW6432", "nodejs"), ("ProgramFiles", "nodejs"),
                                 ("ProgramFiles(x86)", "nodejs"), ("LOCALAPPDATA", "Programs/nodejs"),
                                 ("LOCALAPPDATA", "nodejs"), ("NVM_SYMLINK", "")]:
            with self.subTest(variable=variable, suffix=suffix):
                executable = self.root / suffix / "node.exe"
                executable.parent.mkdir(parents=True, exist_ok=True)
                executable.touch()
                with patch.object(hook.shutil, "which", return_value=None), \
                     patch.object(hook.sys, "platform", "win32"), \
                     patch.dict(os.environ, {variable: str(self.root)}, clear=True):
                    self.assertEqual(hook.find_node(), str(executable))
                executable.unlink()

    def test_missing_node_on_other_platform_explains_install(self):
        with patch.object(hook.shutil, "which", return_value=None), \
             patch.object(hook.sys, "platform", "linux"):
            with self.assertRaisesRegex(RuntimeError, "Install Node.js LTS"):
                hook.ensure_tools(self.root)

    def test_old_node_is_rejected(self):
        with patch.object(hook.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "v16.20.2\n")):
            with self.assertRaisesRegex(RuntimeError, r"18\+ is required"):
                hook.node_version("node", self.root)

    def test_windows_downloads_when_node_is_missing(self):
        with patch.object(hook, "find_node", side_effect=RuntimeError("missing")), \
             patch.object(hook.sys, "platform", "win32"), \
             patch.object(hook, "install_windows_node", return_value="cached node.exe") as install, \
             patch.object(hook, "node_version", return_value="v24.19.0"), \
             patch.object(hook, "find_npm_cli", return_value="npm-cli.js"):
            self.assertEqual(hook.ensure_tools(self.root), ("cached node.exe", "v24.19.0", "npm-cli.js"))
            install.assert_called_once_with(self.root)

    def test_windows_replaces_unsupported_node_with_local_runtime(self):
        with patch.object(hook, "find_node", return_value="old node.exe"), \
             patch.object(hook.sys, "platform", "win32"), \
             patch.object(hook, "node_version", side_effect=[RuntimeError("old"), "v24.19.0"]), \
             patch.object(hook, "install_windows_node", return_value="cached node.exe"), \
             patch.object(hook, "find_npm_cli", return_value="npm-cli.js"):
            self.assertEqual(hook.ensure_tools(self.root)[0], "cached node.exe")

    def test_npm_js_entry_is_found_beside_node_without_path(self):
        node = self.root / "node.exe"
        npm = self.root / "node_modules/npm/bin/npm-cli.js"
        npm.parent.mkdir(parents=True)
        npm.touch()
        with patch.object(hook.shutil, "which", return_value=None):
            self.assertEqual(hook.find_npm_cli(str(node)), str(npm))

    def test_npm_system_symlink_resolves_to_js(self):
        npm = self.root / "npm-cli.js"
        npm.touch()
        with patch.object(hook.shutil, "which", return_value=str(npm)):
            self.assertEqual(hook.find_npm_cli(str(self.root / "bin/node")), str(npm))

    def archive(self, arch="x64", extra=None, include_npm=True):
        prefix = "node-v%s-win-%s" % (hook.NODE_VERSION, arch)
        content = io.BytesIO()
        with zipfile.ZipFile(content, "w") as package:
            package.writestr(prefix + "/node.exe", b"node executable fixture")
            if include_npm:
                package.writestr(prefix + "/node_modules/npm/bin/npm-cli.js", b"npm fixture")
            if extra:
                package.writestr(extra, b"unsafe")
        return content.getvalue()

    @contextmanager
    def download_fixture(self, content, arch="x64", digest=None):
        with patch.object(hook.platform, "machine", return_value="ARM64" if arch == "arm64" else "AMD64"), \
             patch.dict(hook.NODE_WINDOWS_SHA256, {arch: digest or hashlib.sha256(content).hexdigest()}), \
             patch.object(hook.urllib.request, "urlopen", return_value=io.BytesIO(content)) as request:
            yield request

    def test_verified_download_is_cached_and_path_is_local(self):
        for arch in ["x64", "arm64"]:
            with self.subTest(arch=arch), self.download_fixture(self.archive(arch), arch) as request:
                executable = hook.install_windows_node(self.root)
                self.assertTrue(Path(executable).is_relative_to(self.root / ".pio/web-tools"))
                self.assertEqual(Path(executable).read_bytes(), b"node executable fixture")
                self.assertEqual(hook.install_windows_node(self.root), executable)
                request.assert_called_once()
                self.assertEqual(request.call_args.args[0],
                                 "https://nodejs.org/download/release/v%s/node-v%s-win-%s.zip" %
                                 (hook.NODE_VERSION, hook.NODE_VERSION, arch))

    def test_digest_mismatch_does_not_install(self):
        with self.download_fixture(self.archive(), digest="0" * 64):
            with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
                hook.install_windows_node(self.root)
        self.assertFalse(any(self.root.rglob("node.exe")))
        self.assertFalse(any(self.root.rglob("node-setup-*")))

    def test_archive_traversal_is_rejected(self):
        for member in ["../escape", "/absolute", "node-v24.19.0-win-x64/../../escape",
                       "C:/escape", "node-v24.19.0-win-x64/..\\escape"]:
            with self.subTest(member=member), self.download_fixture(self.archive(extra=member)):
                with self.assertRaisesRegex(RuntimeError, "Unsafe Node.js archive"):
                    hook.install_windows_node(self.root)
        self.assertFalse((self.root / "escape").exists())

    def test_incomplete_archive_is_not_cached(self):
        with self.download_fixture(self.archive(include_npm=False)):
            with self.assertRaisesRegex(RuntimeError, "missing node.exe or npm"):
                hook.install_windows_node(self.root)
        self.assertFalse(any(self.root.rglob(".ldg-archive-sha256")))

    def test_download_error_can_be_retried(self):
        with patch.object(hook.platform, "machine", return_value="AMD64"), \
             patch.object(hook.urllib.request, "urlopen", side_effect=OSError("offline")):
            with self.assertRaisesRegex(RuntimeError, "Check internet access and retry Build"):
                hook.install_windows_node(self.root)
        self.assertFalse(any(self.root.rglob("node-setup-*")))

    def test_first_build_installs_locked_dependencies_and_second_reuses_them(self):
        with patch.object(hook.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            hook.ensure_dependencies("node", "v24.19.0", "npm-cli.js", self.root)
            self.assertEqual(run.call_args_list[0].args[0],
                             ["node", "npm-cli.js", "ci", "--include=dev", "--ignore-scripts", "--no-audit", "--no-fund"])
            self.assertTrue(self.stamp.is_file())
            run.reset_mock()
            hook.ensure_dependencies("node", "v24.19.0", "npm-cli.js", self.root)
            self.assertEqual(run.call_count, 1)
            self.assertEqual(run.call_args.args[0][1], "-e")

    def test_changed_lock_or_manifest_reinstalls(self):
        with patch.object(hook.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            hook.ensure_dependencies("node", "v24.19.0", "npm-cli.js", self.root)
            for name in ["package-lock.json", "package.json"]:
                (self.root / name).write_text('{"changed":true}')
                run.reset_mock()
                hook.ensure_dependencies("node", "v24.19.0", "npm-cli.js", self.root)
                self.assertEqual(run.call_count, 2)
                self.assertEqual(run.call_args_list[0].args[0][2], "ci")

    def test_removed_modules_trigger_reinstall(self):
        with patch.object(hook.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
            hook.ensure_dependencies("node", "v24.19.0", "npm-cli.js", self.root)
        with patch.object(hook.subprocess, "run", side_effect=[subprocess.CalledProcessError(1, "node"),
                          subprocess.CompletedProcess([], 0), subprocess.CompletedProcess([], 0)]) as run:
            hook.ensure_dependencies("node", "v24.19.0", "npm-cli.js", self.root)
            self.assertEqual(run.call_count, 3)

    def test_failed_npm_install_leaves_no_success_stamp(self):
        with patch.object(hook.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "npm")):
            with self.assertRaisesRegex(RuntimeError, "Automatic npm dependency setup failed"):
                hook.ensure_dependencies("node", "v24.19.0", "npm-cli.js", self.root)
        self.assertFalse(self.stamp.exists())

    def test_absolute_paths_are_separate_arguments_and_build_selects_generated_assets(self):
        env = FakeEnvironment(self.root)
        with patch.object(hook, "ensure_tools", return_value=("Program Files/node.exe", "v24.19.0", "npm-cli.js")), \
             patch.object(hook, "ensure_dependencies") as prepare, \
             patch.object(hook.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            hook.build(env)
        prepare.assert_called_once_with("Program Files/node.exe", "v24.19.0", "npm-cli.js", self.root)
        self.assertEqual(run.call_args.args[0], ["Program Files/node.exe", str(self.root / "scripts/build_web.cjs"),
                                              "--out", str(self.root / "build/web-assets")])
        self.assertEqual(run.call_args.kwargs, {"cwd": self.root, "check": True})
        self.assertEqual(env.replaced, {"PROJECT_DATA_DIR": str(self.root / "build/web-assets")})

    def test_setup_lock_is_released_after_error(self):
        with self.assertRaises(ValueError):
            with hook.bootstrap_lock(self.root):
                raise ValueError("cancelled")
        with hook.bootstrap_lock(self.root):
            pass

    def test_scons_entry_reports_setup_error_without_traceback(self):
        with patch.object(hook.shutil, "which", return_value=None), \
             patch.object(hook.sys, "platform", "linux"):
            with self.assertRaisesRegex(SystemExit, r"\[build_web\].*Node.js 18\+"):
                runpy.run_path(str(SCRIPT), init_globals={"Import": lambda name: None,
                                                        "env": FakeEnvironment(self.root)})


if __name__ == "__main__":
    unittest.main()
