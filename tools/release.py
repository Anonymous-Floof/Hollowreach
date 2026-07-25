"""Release tooling for Hollowreach. Stdlib only, same Python 3 the game needs.

    python tools/release.py bump <major|minor|patch>
        Move the [Latest] changelog entries under a new version heading
        (dated today) and write the new number into js/version.js.

    python tools/release.py package
        Build dist/Hollowreach-vX.Y.Z.zip — the full game plus the Windows
        (run.bat) and Linux/macOS (run.sh) launchers, everything under a
        Hollowreach-vX.Y.Z/ folder so it unzips tidily — and
        dist/RELEASE_NOTES-vX.Y.Z.md, the release body generated from
        CHANGELOG.md plus standard download/run instructions.

    python tools/release.py publish [--draft]
        package, then create the GitHub release (tag vX.Y.Z, zip attached,
        notes as the body) via the `gh` CLI. --draft leaves it unpublished so
        it can be reviewed on github.com first.

Typical flow when shipping:  bump minor  ->  review + commit + push  ->  publish.
See docs/RELEASING.md for the full walkthrough.
"""

import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_FILE = os.path.join(ROOT, "js", "version.js")
CHANGELOG = os.path.join(ROOT, "CHANGELOG.md")
DIST = os.path.join(ROOT, "dist")

# What ships in the zip: the game, its server, both launchers, and the docs a
# player might actually open. Dev-only folders (tools/, screenshots/, worlds/,
# .git/ ...) stay out.
SHIP_FILES = ["index.html", "server.py", "run.bat", "run.sh",
              "README.md", "LICENSE", "CHANGELOG.md"]
SHIP_DIRS = ["css", "js"]


def fail(msg):
    print("error: " + msg)
    sys.exit(1)


# ---- version ----------------------------------------------------------------

VERSION_RE = re.compile(r'^export const GAME_VERSION = "(\d+)\.(\d+)\.(\d+)";', re.M)


def read_version():
    with open(VERSION_FILE, encoding="utf-8") as f:
        src = f.read()
    m = VERSION_RE.search(src)
    if not m:
        fail("could not find GAME_VERSION in js/version.js")
    return ".".join(m.groups()), src


def write_version(new, src):
    src = VERSION_RE.sub('export const GAME_VERSION = "%s";' % new, src, count=1)
    with open(VERSION_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)


# ---- changelog --------------------------------------------------------------

def changelog_section(version):
    """Return the body text under '## [version]' (up to the next '## [')."""
    with open(CHANGELOG, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"^## \[%s\][^\n]*\n(.*?)(?=^## \[|^\[|\Z)" % re.escape(version),
                  text, re.M | re.S)
    return m.group(1).strip() if m else None


def cmd_bump(part):
    cur, src = read_version()
    major, minor, patch = map(int, cur.split("."))
    if part == "major":
        new = "%d.0.0" % (major + 1)
    elif part == "minor":
        new = "%d.%d.0" % (major, minor + 1)
    else:
        new = "%d.%d.%d" % (major, minor, patch + 1)

    with open(CHANGELOG, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"^## \[Latest\][^\n]*\n(.*?)(?=^## \[|^\[|\Z)", text, re.M | re.S)
    if not m:
        fail("no [Latest] section in CHANGELOG.md")
    body = m.group(1).strip()
    if not body:
        fail("the [Latest] section of CHANGELOG.md is empty — write the "
             "release notes there first, then bump")

    today = datetime.date.today().isoformat()
    replacement = "## [Latest]\n\n## [%s] - %s\n\n%s\n\n" % (new, today, body)
    text = text[:m.start()] + replacement + text[m.end():]
    # keep the compare/tag link footer up to date if it exists
    text = text.replace("/compare/v%s...HEAD" % cur, "/compare/v%s...HEAD" % new)
    m2 = re.search(r"^\[Latest\]: (.*)/compare/", text, re.M)
    if m2 and ("\n[%s]: " % new) not in text:
        text = re.sub(r"^(\[Latest\]: [^\n]*\n)",
                      r"\g<1>[%s]: %s/releases/tag/v%s\n" % (new, m2.group(1), new),
                      text, count=1, flags=re.M)
    with open(CHANGELOG, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    write_version(new, src)
    print("bumped %s -> %s" % (cur, new))
    print("CHANGELOG.md: [Latest] entries moved under [%s] - %s" % (new, today))
    print("next: review both files, commit, then  python tools/release.py publish")


def _git_dirty():
    try:
        out = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                             capture_output=True, text=True, timeout=15)
        return bool(out.stdout.strip())
    except Exception:
        return False


# ---- package ----------------------------------------------------------------

def cmd_package():
    version, _ = read_version()
    notes = changelog_section(version)
    if notes is None:
        fail("CHANGELOG.md has no '## [%s]' section — run "
             "'python tools/release.py bump ...' (or add one by hand) first" % version)
    if not notes:
        fail("the '## [%s]' section of CHANGELOG.md is empty" % version)
    if _git_dirty():
        print("warning: uncommitted changes in the working tree — the zip is "
              "built from the files on disk, not from a git ref")

    name = "Hollowreach-v" + version
    os.makedirs(DIST, exist_ok=True)
    zip_path = os.path.join(DIST, name + ".zip")
    if os.path.exists(zip_path):
        os.remove(zip_path)

    def add(zf, abs_path, rel):
        arc = name + "/" + rel.replace(os.sep, "/")
        if rel == "run.sh":
            # LF endings (a CRLF shebang is "bad interpreter") + unix exec bit,
            # regardless of what the Windows checkout did to the file
            with open(abs_path, "rb") as f:
                data = f.read().replace(b"\r\n", b"\n")
            info = zipfile.ZipInfo(arc, date_time=time.localtime()[:6])
            info.create_system = 3          # "unix", or extractors ignore the mode bits
            info.external_attr = 0o100755 << 16
            zf.writestr(info, data, zipfile.ZIP_DEFLATED)
        elif rel == "run.bat":
            with open(abs_path, "rb") as f:
                data = f.read()
            data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            zf.writestr(name + "/run.bat", data, zipfile.ZIP_DEFLATED)
        else:
            zf.write(abs_path, arc, zipfile.ZIP_DEFLATED)

    count = 0
    with zipfile.ZipFile(zip_path, "w") as zf:
        for rel in SHIP_FILES:
            p = os.path.join(ROOT, rel)
            if not os.path.exists(p):
                fail("missing file: " + rel)
            add(zf, p, rel)
            count += 1
        for d in SHIP_DIRS:
            for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, d)):
                dirnames.sort()
                for fn in sorted(filenames):
                    p = os.path.join(dirpath, fn)
                    add(zf, p, os.path.relpath(p, ROOT))
                    count += 1

    notes_path = os.path.join(DIST, "RELEASE_NOTES-v%s.md" % version)
    with open(notes_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(RELEASE_NOTES_TEMPLATE.format(version=version, notes=notes,
                                              zip_name=name + ".zip"))

    size = os.path.getsize(zip_path) / (1024 * 1024)
    print("packaged %d files -> %s (%.1f MB)" % (count, os.path.relpath(zip_path, ROOT), size))
    print("release notes    -> %s" % os.path.relpath(notes_path, ROOT))
    return zip_path, notes_path, version


RELEASE_NOTES_TEMPLATE = """\
{notes}

---

## How to play

1. Download **`{zip_name}`** below and unzip it anywhere.
2. Start the game:
   - **Windows** — double-click `run.bat`
   - **Linux / macOS** — run `./run.sh` (from a terminal, or a double-click if
     your file manager runs scripts)
3. Your browser opens the game automatically.

The only requirements are **Python 3** (the launcher tells you where to get it
if it's missing) and a browser with **WebGL2** — recent Chrome, Edge, or
Firefox. Nothing is installed and nothing leaves your machine; a tiny local
server just hosts the files for your own browser.

**Multiplayer note:** everyone needs the same game version (v{version}).
"""


# ---- publish ----------------------------------------------------------------

def cmd_publish(draft):
    if shutil.which("gh") is None:
        fail("the GitHub CLI (`gh`) is not installed or not on PATH — install "
             "it from https://cli.github.com/ and run `gh auth login`, or "
             "create the release by hand (see docs/RELEASING.md)")
    zip_path, notes_path, version = cmd_package()
    tag = "v" + version

    existing = subprocess.run(["gh", "release", "view", tag], cwd=ROOT,
                              capture_output=True, text=True)
    if existing.returncode == 0:
        fail("release %s already exists on GitHub — bump the version first, or "
             "delete the old release (gh release delete %s)" % (tag, tag))

    cmd = ["gh", "release", "create", tag, zip_path,
           "--title", "Hollowreach %s" % tag,
           "--notes-file", notes_path]
    if draft:
        cmd.append("--draft")
    print("running:", " ".join(cmd))
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        fail("gh release create failed (see output above)")
    print("release %s %s" % (tag, "drafted — review and publish it on github.com"
                             if draft else "published"))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bump", help="advance GAME_VERSION and date the changelog")
    b.add_argument("part", choices=["major", "minor", "patch"])
    sub.add_parser("package", help="build the release zip + notes into dist/")
    p = sub.add_parser("publish", help="package, then create the GitHub release via gh")
    p.add_argument("--draft", action="store_true", help="create as a draft release")
    args = ap.parse_args()

    if args.cmd == "bump":
        cmd_bump(args.part)
    elif args.cmd == "package":
        cmd_package()
    else:
        cmd_publish(args.draft)


if __name__ == "__main__":
    main()
