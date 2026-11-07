"""Generate minified, precompressed LittleFS assets on the build host."""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def find_node():
    node = shutil.which("node")
    if node:
        return node
    # VS Code can retain the PATH from before the Node.js installer ran.
    if sys.platform == "win32":
        directories = []
        for name in ("ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"):
            if os.environ.get(name):
                directories.append(Path(os.environ[name]) / "nodejs")
        if os.environ.get("LOCALAPPDATA"):
            local = Path(os.environ["LOCALAPPDATA"])
            directories.extend((local / "Programs" / "nodejs", local / "nodejs"))
        if os.environ.get("NVM_SYMLINK"):
            directories.append(Path(os.environ["NVM_SYMLINK"]))
        for directory in directories:
            candidate = directory / "node.exe"
            if candidate.is_file():
                return str(candidate)
    raise RuntimeError(
        "Node.js 18+ was not found. Install Node.js LTS from "
        "https://nodejs.org/en/download, restart VS Code, then run "
        "'npm ci --ignore-scripts --no-audit --no-fund' in the project root "
        "(use npm.cmd in Windows PowerShell)."
    )


def build(env):
    project = Path(env.subst("$PROJECT_DIR"))
    destination = Path(env.subst("$BUILD_DIR")) / "web-assets"
    node = find_node()
    try:
        version = subprocess.run(
            [node, "--version"], cwd=project, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError("Cannot run Node.js at %s: %s" % (node, error)) from error
    match = re.match(r"^v(\d+)\.", version)
    if not match or int(match.group(1)) < 18:
        raise RuntimeError("Node.js 18+ is required; found %s at %s." % (version, node))
    try:
        subprocess.run(
            [node, "-e", "for (const name of ['acorn','postcss','terser']) require.resolve(name)"],
            cwd=project, check=True, capture_output=True, text=True,
        )
    except subprocess.CalledProcessError as error:
        raise RuntimeError(
            "Web build dependencies are missing. Run "
            "'npm ci --ignore-scripts --no-audit --no-fund' in %s "
            "(use npm.cmd in Windows PowerShell)." % project
        ) from error
    print("[build_web] Node.js %s: %s" % (version, node))
    subprocess.run(
        [node, str(project / "scripts" / "build_web.cjs"), "--out", str(destination)],
        cwd=project, check=True,
    )
    env.Replace(PROJECT_DATA_DIR=str(destination))


if "Import" in globals():
    Import("env")
    try:
        build(env)
    except RuntimeError as error:
        raise SystemExit("[build_web] " + str(error)) from None
