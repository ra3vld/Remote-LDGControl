"""PlatformIO pre-build hook: apply local unified diffs to library deps.

Uses only Python's standard library, including on Windows: no patch.exe, Git,
PATH changes, or extra pip packages are needed. Context must match exactly
(apart from LF/CRLF line endings); ambiguous or incompatible patches fail the
build. A reverse check makes repeated builds idempotent.

Patch filenames: <libname>-<description>.patch.
Wired in by `extra_scripts = pre:scripts/apply_patches.py` in platformio.ini.
"""

import re
from pathlib import Path, PurePosixPath


class PatchMismatch(ValueError):
    """The dependency does not match the patch's context."""


def parse_patch(patchfile):
    """Read the file-edit unified diff format used by patches/ (strip -p1)."""
    lines = Path(patchfile).read_bytes().replace(b"\r\n", b"\n").splitlines(True)
    files = []
    index = 0
    while index < len(lines):
        if not lines[index].startswith(b"--- a/"):
            raise ValueError("expected --- a/<path> file header")
        old = lines[index][6:].split(b"\t", 1)[0].rstrip(b"\n")
        index += 1
        if index >= len(lines) or not lines[index].startswith(b"+++ b/"):
            raise ValueError("expected +++ b/<path> file header")
        new = lines[index][6:].split(b"\t", 1)[0].rstrip(b"\n")
        path = PurePosixPath(old.decode("utf-8"))
        if (old != new or path.is_absolute() or not path.parts
                or ".." in path.parts or b"\\" in old or b":" in old):
            raise ValueError("only edits to existing relative file paths are supported")
        index += 1
        hunks = []
        while index < len(lines) and lines[index].startswith(b"@@ "):
            header = re.fullmatch(
                rb"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@[^\n]*\n",
                lines[index],
            )
            if header is None:
                raise ValueError("invalid unified diff hunk header")
            old_count = int(header[2]) if header[2] is not None else 1
            new_count = int(header[4]) if header[4] is not None else 1
            before, after = [], []
            index += 1
            while len(before) < old_count or len(after) < new_count:
                if index >= len(lines):
                    raise ValueError("truncated unified diff hunk")
                line = lines[index]
                if line[:1] not in (b" ", b"-", b"+") or not line.endswith(b"\n"):
                    raise ValueError("unsupported or malformed unified diff line")
                if line[:1] in (b" ", b"-"):
                    before.append(line[1:])
                if line[:1] in (b" ", b"+"):
                    after.append(line[1:])
                index += 1
            if len(before) != old_count or len(after) != new_count:
                raise ValueError("unified diff hunk line counts do not match")
            if not before or not after:
                raise ValueError("hunks must include context on both sides")
            hunks.append((before, after))
        if not hunks:
            raise ValueError("file patch has no hunks")
        files.append((path, hunks))
    if not files:
        raise ValueError("empty patch")
    return files


def patched_content(content, hunks, reverse=False):
    """Apply exact, uniquely located hunks in memory, preserving line endings."""
    lines = content.splitlines(True)
    cursor = 0
    for before, after in hunks:
        if reverse:
            before, after = after, before
        normalized = [line.replace(b"\r\n", b"\n") for line in lines]
        matches = [
            start for start in range(cursor, len(lines) - len(before) + 1)
            if normalized[start:start + len(before)] == before
        ]
        if len(matches) != 1:
            raise PatchMismatch(f"expected one matching context, found {len(matches)}")
        start = matches[0]
        # Use the source hunk's newline style for context and added lines.
        original = lines[start:start + len(before)]
        ending = b"\r\n" if original[0].endswith(b"\r\n") else b"\n"
        replacement = [line[:-1] + ending for line in after]
        lines[start:start + len(before)] = replacement
        cursor = start + len(replacement)
    return b"".join(lines)


def apply_patch(patchfile, libdir):
    """Validate every file before writing; reject partial or incompatible patches."""
    files = parse_patch(patchfile)
    root = Path(libdir).resolve()
    originals = []
    for relative, hunks in files:
        target = root.joinpath(*relative.parts).resolve()
        if root not in target.parents:
            raise ValueError(f"patch target escapes library directory: {relative}")
        originals.append((target, target.read_bytes(), hunks))
    try:
        updates = [(target, patched_content(content, hunks))
                   for target, content, hunks in originals]
    except PatchMismatch as error:
        try:
            for _, content, hunks in originals:
                patched_content(content, hunks, reverse=True)
        except PatchMismatch:
            raise RuntimeError(
                f"[apply_patches] could not apply {Path(patchfile).name}: {error}. "
                "Dependency sources differ from the expected version or are "
                "partially patched."
            ) from error
        print(f"[apply_patches] skipping {Path(patchfile).name} (already applied)")
        return
    for target, content in updates:
        target.write_bytes(content)
    print(f"[apply_patches] applying {Path(patchfile).name}")


def main(env):
    patches_dir = Path(env["PROJECT_DIR"]) / "patches"
    libdeps_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"]
    for patchfile in sorted(patches_dir.glob("*.patch")):
        libname = patchfile.name.split("-", 1)[0]
        libdir = libdeps_dir / libname
        if not libdir.is_dir():
            print(f"[apply_patches] target lib dir not found for {patchfile} "
                  f"(looked at {libdir}); skipping — PlatformIO may not have "
                  "installed it yet")
            continue
        apply_patch(patchfile, libdir)


if "Import" in globals():
    Import("env")  # type: ignore  # provided by PlatformIO SCons
    main(env)  # type: ignore
