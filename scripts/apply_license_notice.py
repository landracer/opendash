#!/usr/bin/env python3
# Licensed under Sovereign Individual License v1.0 — see LICENSE file
"""
apply_license_notice.py — insert (or remove) a short license notice at the top
of every source file in a repository.

Repo-agnostic and idempotent. Designed to be copied between projects: just run it
from the repo root, or pass --root. Re-running never duplicates a notice.

Notice style is chosen per file type:
    C / C++ / headers .......  /* <notice> */
    Python / CMake / Kconfig .  # <notice>   (inserted after shebang / coding line)
    Markdown ................  <!-- <notice> --> (inserted after YAML front-matter)

Safety:
  * Generated / vendored directories are skipped (build, managed_components,
    node_modules, virtualenvs, .git, ...).
  * Files that already carry a THIRD-PARTY license header (SPDX, Espressif, LVGL,
    MIT/BSD/Apache/GPL boilerplate) are skipped so you never relicense code that
    is not yours.

Usage:
    python3 apply_license_notice.py                 # apply to current repo
    python3 apply_license_notice.py --root /path    # apply to another repo
    python3 apply_license_notice.py --check         # dry-run, report only
    python3 apply_license_notice.py --revert        # remove notices added by this tool
    python3 apply_license_notice.py --notice "Licensed under My License v2 — see LICENSE"

Exit code is 0 on success, 1 if nothing matched.
"""
from __future__ import annotations

import argparse
import os
import sys

# ── Defaults (override via CLI) ──────────────────────────────────────────────
DEFAULT_NOTICE = "Licensed under Sovereign Individual License v1.0 — see LICENSE file"

# Marker substring used for idempotency + revert. Keep it stable across versions
# of the notice text so re-runs and reverts always match.
DEFAULT_MARKER = "Sovereign Individual License"

EXCLUDE_DIRS = {
    ".git", "build", "managed_components", "__pycache__", "node_modules",
    ".venv", "venv", "env", ".env", ".tox", ".mypy_cache", ".pytest_cache",
    "dist", ".idea", ".vscode", ".claude", ".cline",
}

C_EXT = {".c", ".h", ".cpp", ".hpp", ".cc", ".cxx", ".hh", ".ino"}
HASH_EXT = {".py"}
MD_EXT = {".md"}
BUILD_NAMES = {"CMakeLists.txt", "Kconfig", "Kconfig.projbuild"}

# If a file already contains any of these, treat it as third-party and skip.
THIRD_PARTY_MARKERS = (
    "SPDX-License-Identifier",
    "Espressif Systems",
    "Permission is hereby granted",       # MIT / ISC
    "Redistribution and use in source",   # BSD
    "Apache License",
    "GNU GENERAL PUBLIC LICENSE",
    "www.lvgl.io",
    "LVGL LLC",
)


def comment_for(kind: str, notice: str) -> str:
    if kind == "c":
        return f"/* {notice} */\n"
    if kind == "hash":
        return f"# {notice}\n"
    if kind == "md":
        return f"<!-- {notice} -->\n"
    raise ValueError(kind)


def kind_for(filename: str, enabled: set[str]) -> str | None:
    ext = os.path.splitext(filename)[1].lower()
    if "build" in enabled and filename in BUILD_NAMES:
        return "hash"
    if "c" in enabled and ext in C_EXT:
        return "c"
    if "py" in enabled and ext in HASH_EXT:
        return "hash"
    if "md" in enabled and ext in MD_EXT:
        return "md"
    return None


def insertion_index(kind: str, lines: list[str]) -> int:
    """Where to insert the notice so we don't break shebangs / front-matter."""
    if kind == "hash" and lines and lines[0].startswith("#!"):
        idx = 1
        if idx < len(lines) and lines[idx].startswith("#") and "coding" in lines[idx]:
            idx += 1
        return idx
    if kind == "md" and lines and lines[0].strip() == "---":
        for i in range(1, len(lines)):
            if lines[i].strip() == "---":
                return i + 1
    return 0


def iter_files(root: str, enabled: set[str]):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
        for fn in filenames:
            kind = kind_for(fn, enabled)
            if kind:
                yield os.path.join(dirpath, fn), kind


def read(path: str) -> str | None:
    try:
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
            return f.read()
    except (OSError, UnicodeDecodeError):
        return None


def write(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(text)


def run(args) -> int:
    root = os.path.abspath(args.root)
    notice = args.notice
    marker = args.marker
    enabled = set(args.types)
    notices_by_kind = {k: comment_for(k, notice) for k in ("c", "hash", "md")}

    stats = {"scanned": 0, "added": 0, "removed": 0,
             "skip_has": 0, "skip_thirdparty": 0}
    by_dir: dict[str, int] = {}

    for path, kind in iter_files(root, enabled):
        stats["scanned"] += 1
        content = read(path)
        if content is None:
            continue
        top = os.path.relpath(path, root).split(os.sep)[0]

        if args.revert:
            lines = content.splitlines(keepends=True)
            kept = [ln for ln in lines if marker not in ln]
            if len(kept) != len(lines):
                if not args.check:
                    write(path, "".join(kept))
                stats["removed"] += 1
                by_dir[top] = by_dir.get(top, 0) + 1
            continue

        if marker in content:
            stats["skip_has"] += 1
            continue
        if any(tp in content for tp in THIRD_PARTY_MARKERS):
            stats["skip_thirdparty"] += 1
            continue

        lines = content.splitlines(keepends=True)
        at = insertion_index(kind, lines)
        if not args.check:
            new_lines = lines[:at] + [notices_by_kind[kind]] + lines[at:]
            write(path, "".join(new_lines))
        stats["added"] += 1
        by_dir[top] = by_dir.get(top, 0) + 1

    verb = "Would " if args.check else ""
    action = "remove" if args.revert else "add"
    print("=== License notice ===")
    print(f"Root              : {root}")
    print(f"Notice            : {notice}")
    print(f"File types        : {','.join(sorted(enabled))}")
    print(f"Files scanned     : {stats['scanned']}")
    print(f"{verb}{action:7s} notice : {stats['removed'] if args.revert else stats['added']}")
    if not args.revert:
        print(f"Already had notice: {stats['skip_has']}")
        print(f"Skipped 3rd-party : {stats['skip_thirdparty']}")
    if by_dir:
        print("\nPer top-level dir:")
        for d in sorted(by_dir):
            print(f"  {d:42s} {by_dir[d]}")

    return 0 if stats["scanned"] else 1


def main() -> int:
    p = argparse.ArgumentParser(
        description="Insert or remove a license notice across a repo's source files.")
    p.add_argument("--root", default=".", help="Repo root to process (default: cwd).")
    p.add_argument("--notice", default=DEFAULT_NOTICE, help="Notice text to insert.")
    p.add_argument("--marker", default=DEFAULT_MARKER,
                   help="Stable substring used for idempotency and --revert matching.")
    p.add_argument("--types", nargs="+", default=["c", "py", "build", "md"],
                   choices=["c", "py", "build", "md"],
                   help="File-type groups to process.")
    p.add_argument("--check", action="store_true",
                   help="Dry run: report what would change, write nothing.")
    p.add_argument("--revert", action="store_true",
                   help="Remove any line containing --marker (undo previous runs).")
    return run(p.parse_args())


if __name__ == "__main__":
    sys.exit(main())
