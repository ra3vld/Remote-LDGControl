"""PlatformIO pre-build hook: apply local patches to managed library deps.

Why this exists:
  We need a NULL-guard fix in esp32_idf5_https_server's HTTPSConnection.cpp
  (see patches/esp32_idf5_https_server-null-guards.patch and the wedge story
  in tests/wedge_repro.sh). The library is installed under .pio/libdeps/, so
  edits there get blown away whenever PlatformIO refreshes deps. This script
  re-applies the patches on every build so the fix survives clean checkouts,
  CI, dep updates, etc.

How it works:
  - For each .patch under patches/, look up the target library directory
    under .pio/libdeps/<env>/<libname>/ (filename prefix before "-" is the
    lib name).
  - Try to apply via `patch -p1 --dry-run` first; if it succeeds, apply for
    real. If the dry-run reports "already applied", skip.
  - Find GNU patch on PATH or inside Git for Windows, without changing PATH.
  - Fail the build if a patch can't be applied (better to fail loudly than
    silently ship un-patched code).

Wired in by `extra_scripts = pre:scripts/apply_patches.py` in platformio.ini.
"""

import glob
import os
import shutil
import subprocess
import sys

Import("env")  # type: ignore  # provided by PlatformIO SCons

PROJECT_DIR    = env["PROJECT_DIR"]
LIBDEPS_DIR    = env.subst("$PROJECT_LIBDEPS_DIR")  # .pio/libdeps
ENV_NAME       = env["PIOENV"]
PATCHES_DIR    = os.path.join(PROJECT_DIR, "patches")


def windows_git_dirs():
    """Find Git installations, including portable and non-default locations."""
    git = shutil.which("git")
    if git:
        # Git exposes cmd/git.exe, bin/git.exe, or mingw64/bin/git.exe.
        parent = os.path.dirname(os.path.dirname(os.path.abspath(git)))
        yield parent
        yield os.path.dirname(parent)

    import winreg

    for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
            try:
                with winreg.OpenKey(hive, r"SOFTWARE\GitForWindows", 0,
                                    winreg.KEY_READ | view) as key:
                    install_dir, _ = winreg.QueryValueEx(key, "InstallPath")
                if isinstance(install_dir, str) and install_dir:
                    yield install_dir
            except OSError:
                continue

    for variable in ("ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"):
        directory = os.environ.get(variable)
        if directory:
            yield os.path.join(directory, "Git")
    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        yield os.path.join(local_app_data, "Programs", "Git")


def find_patch():
    """Return the executable's absolute path or an actionable build error."""
    executable = shutil.which("patch")
    if executable:
        return os.path.abspath(executable)
    if sys.platform == "win32":
        for directory in windows_git_dirs():
            executable = os.path.join(directory, "usr", "bin", "patch.exe")
            if os.path.isfile(executable):
                return os.path.abspath(executable)
        raise RuntimeError(
            "[apply_patches] GNU patch was not found. Install the full Git for "
            "Windows distribution (https://gitforwindows.org/), which includes "
            "usr\\bin\\patch.exe, and restart VS Code. MinGit is not sufficient."
        )
    raise RuntimeError(
        "[apply_patches] GNU patch was not found. Install GNU patch and make "
        "the patch command available on PATH."
    )


def lib_dir_for(patchfile):
    """Return the absolute path to the library dir this patch targets.

    Patch filename convention: <lib-name>-<short-description>.patch.
    E.g. esp32_idf5_https_server-null-guards.patch → lib dir
    .pio/libdeps/<env>/esp32_idf5_https_server.
    """
    name = os.path.basename(patchfile)
    libname = name.split("-", 1)[0]
    return os.path.join(LIBDEPS_DIR, ENV_NAME, libname)


def apply_patch(patchfile, libdir, patch_executable):
    """Apply one patch idempotently. Raises CalledProcessError on hard failure."""
    rel = os.path.relpath(patchfile, PROJECT_DIR)
    # Never prompt in an IDE build or automatically undo an existing patch.
    command = [patch_executable, "--batch", "--forward", "-p1"]

    # Dry-run forward to see if it applies cleanly.
    dry = subprocess.run(
        command + ["--dry-run", "-i", patchfile],
        cwd=libdir, capture_output=True, text=True, errors="replace",
    )
    if dry.returncode == 0:
        print(f"[apply_patches] applying {rel}")
        subprocess.run(
            command + ["-i", patchfile],
            cwd=libdir, check=True,
        )
        return

    # Dry-run reverse: if it would *un*-apply cleanly, the patch is already in place.
    rev = subprocess.run(
        command + ["-R", "--dry-run", "-i", patchfile],
        cwd=libdir, capture_output=True, text=True, errors="replace",
    )
    if rev.returncode == 0:
        print(f"[apply_patches] skipping {rel} (already applied)")
        return

    # Neither direction works — patch is broken or the target moved.
    print(f"[apply_patches] FAILED to apply {rel}:\n{dry.stdout}\n{dry.stderr}")
    raise RuntimeError(f"could not apply {rel}")


def main():
    if not os.path.isdir(PATCHES_DIR):
        return  # no patches to apply
    patch_executable = None
    for patchfile in sorted(glob.glob(os.path.join(PATCHES_DIR, "*.patch"))):
        libdir = lib_dir_for(patchfile)
        if not os.path.isdir(libdir):
            print(f"[apply_patches] target lib dir not found for {patchfile} "
                  f"(looked at {libdir}); skipping — PlatformIO may not have "
                  f"installed it yet")
            continue
        if patch_executable is None:
            patch_executable = find_patch()
        apply_patch(patchfile, libdir, patch_executable)


main()