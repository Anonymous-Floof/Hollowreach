#!/usr/bin/env python3
"""Refuse a commit that would put this machine into the repository.

Run as a pre-commit hook (see .githooks/pre-commit). It reads what is actually
STAGED — not the working tree — and fails the commit if it finds either of the
two things that have gone wrong here before:

  * a build artefact nobody meant to add. Both real incidents came from
    `git add -A` immediately after running a tool that had quietly written
    something: a `--world/` directory created by a shell variable that expanded
    to nothing, and a `__pycache__/*.pyc` created by importing release.py to
    test it.
  * this machine's absolute paths or username inside a file. That is the part
    that actually matters, and it is not always visible: Python bytecode embeds
    the absolute path of the source it was compiled from, so a 21 KB binary
    nobody would think to open carried a home directory in it.

Deliberately derived from the environment rather than hardcoding a name, so it
protects whoever is committing rather than one particular person.

Override with `git commit --no-verify` if you are certain. Usually the right
answer is to unstage the file and add it to .gitignore instead.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

# Paths that should never be committed from this project. Matched against the
# staged path with '/' separators.
BAD_PATHS = [
    (re.compile(r"(^|/)__pycache__/"), "Python bytecode cache"),
    (re.compile(r"\.pyc$"), "compiled Python (embeds the source's absolute path)"),
    (re.compile(r"\.(pdb|ilk|obj|o|exe|dll|so|dylib)$"), "build output"),
    (re.compile(r"^dist/.*\.zip$"), "release archive"),
    (re.compile(r"^data/"), "player data the game creates at runtime"),
    (re.compile(r"^--"), "a directory named from a shell argument that expanded to nothing"),
    (re.compile(r"(^|/)\.DS_Store$"), "Finder metadata"),
]

# Generic account names that would match half the world's file paths.
TOO_GENERIC = {"user", "users", "admin", "administrator", "root", "guest", "owner",
               "build", "runner", "default"}


def staged_files() -> list[str]:
    out = subprocess.run(["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"],
                         capture_output=True, text=True)
    return [l.strip() for l in out.stdout.splitlines() if l.strip()]


def staged_bytes(path: str) -> bytes:
    out = subprocess.run(["git", "show", ":" + path], capture_output=True)
    return out.stdout


def needles() -> list[tuple[bytes, str]]:
    """What counts as this machine leaking into a file."""
    found: list[tuple[bytes, str]] = []
    user = (os.environ.get("USERNAME") or os.environ.get("USER") or "").strip()
    if len(user) >= 4 and user.lower() not in TOO_GENERIC:
        found.append((user.encode(), "your account name (%s)" % user))
    home = os.path.expanduser("~")
    if home and home not in ("/", "") and len(home) > 6:
        found.append((home.encode(), "your home directory"))
        found.append((home.replace("\\", "/").encode(), "your home directory"))
    return found


def main() -> int:
    files = staged_files()
    if not files:
        return 0

    problems: list[str] = []
    patterns = needles()

    for path in files:
        for rx, why in BAD_PATHS:
            if rx.search(path):
                problems.append("  %s\n      %s — this should be in .gitignore" % (path, why))
                break
        else:
            blob = staged_bytes(path)
            for needle, why in patterns:
                # Both encodings: Windows tools and compiled artefacts write
                # UTF-16, and a .pyc that reads clean as ASCII may not be.
                for enc, label in ((needle, ""), (needle.decode().encode("utf-16-le"), " (UTF-16)")):
                    at = blob.find(enc)
                    if at < 0:
                        continue
                    excerpt = blob[max(0, at - 30):at + 60]
                    excerpt = re.sub(rb"[^\x20-\x7e]", b".", excerpt).decode("ascii")
                    problems.append("  %s\n      contains %s%s: ...%s..."
                                    % (path, why, label, excerpt))
                    break
                else:
                    continue
                break

    if not problems:
        return 0

    sys.stderr.write("\ncommit refused: this would put your machine in the repository\n\n")
    sys.stderr.write("\n".join(problems) + "\n\n")
    sys.stderr.write("Unstage it and add it to .gitignore. If you are certain it belongs,\n"
                     "commit again with --no-verify.\n\n")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
