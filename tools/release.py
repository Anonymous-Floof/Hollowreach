"""Release tooling for Hollowreach. Stdlib only, and no dependency on the build.

    python tools/release.py bump <major|minor|patch>
        Move the [Latest] changelog entries under a new version heading
        (dated today) and write the new number into CMakeLists.txt.

    python tools/release.py package [--no-build]
        Build the game and produce dist/Hollowreach-vX.Y.Z-<Platform>.zip
        (the executable plus the docs a player might open, under one folder so
        it unzips tidily) and dist/RELEASE_NOTES-vX.Y.Z.md, the release body
        generated from CHANGELOG.md plus download and run instructions.

        The zip itself is CPack's, configured in cmake/package.cmake — one
        definition of what ships, used whether it is built from here or by
        hand with `build.bat package`. This script drives it and does the
        parts CPack has no opinion about: checking the changelog first,
        naming and writing the notes, and tidying the staging directory.

    python tools/release.py publish [--draft]
        package, then create the GitHub release (tag vX.Y.Z, zip attached,
        notes as the body) via the `gh` CLI. --draft leaves it unpublished so
        it can be reviewed on github.com first.

A release is per-platform: this produces the zip for whatever machine it runs
on. Shipping Windows and Linux means running `package` on each and attaching
both, which is what `publish --draft` plus `gh release upload` is for.

Typical flow when shipping:  bump minor  ->  review + commit + push  ->  publish.
See docs/RELEASING.md for the full walkthrough.
"""

import argparse
import datetime
import glob
import os
import platform
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_FILE = os.path.join(ROOT, "CMakeLists.txt")
CHANGELOG = os.path.join(ROOT, "CHANGELOG.md")
DIST = os.path.join(ROOT, "dist")


def fail(msg):
    print("error: " + msg)
    sys.exit(1)


# ---- version ----------------------------------------------------------------
#
# The version lives in the CMake project() call and reaches the game as the
# HR_VERSION define, so there is exactly one place it is written down and the
# build cannot disagree with the changelog about which release this is.

VERSION_RE = re.compile(r"^(project\(Hollowreach\b[^)]*?\bVERSION )(\d+)\.(\d+)\.(\d+)",
                        re.M | re.S)


def read_version():
    with open(VERSION_FILE, encoding="utf-8") as f:
        src = f.read()
    m = VERSION_RE.search(src)
    if not m:
        fail("could not find the VERSION in the project() call in CMakeLists.txt")
    return ".".join(m.groups()[1:]), src


def write_version(new, src):
    src = VERSION_RE.sub(lambda m: m.group(1) + new, src, count=1)
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


# ---- pre-ship checks ---------------------------------------------------------

def _preflight():
    """The two checks docs/RELEASING.md asks for, run rather than remembered.

    There is no CI on this project, so until now both were a line in a document
    and a habit. The self-test takes seconds; the golden gate takes rather longer
    but is the only thing standing between a release and terrain shifting under
    every existing save. Neither belongs on the honour system at the one moment
    they matter most.

    Deliberately NOT run by `package`: that is also how a zip gets rebuilt for a
    second platform, and failing that on a check the first platform already
    passed would be noise. This runs from `publish`, which happens once."""
    exe = None
    for cfg in ("RelWithDebInfo", "Release", "Debug"):
        for name in ("Hollowreach.exe", "Hollowreach"):
            p = os.path.join(ROOT, "build", cfg, "bin", name)
            if os.path.isfile(p):
                if exe is None or os.path.getmtime(p) > os.path.getmtime(exe):
                    exe = p
    if exe is None:
        fail("no built executable to check — run build.bat first")

    print("pre-ship check 1/2: --selftest")
    r = subprocess.run([exe, "--selftest"], cwd=ROOT, capture_output=True, text=True)
    tail = "\n".join(r.stdout.strip().splitlines()[-3:])
    if r.returncode != 0:
        print(tail)
        fail("the self-test failed — fix it before shipping, or pass --skip-checks "
             "if you genuinely mean to ship this")
    print("  " + tail.strip())

    print("pre-ship check 2/2: golden gate")
    gate = os.path.join(ROOT, "tools", "compare_golden.py")
    r = subprocess.run([sys.executable, gate], cwd=ROOT, capture_output=True, text=True)
    print("\n".join("  " + l for l in r.stdout.strip().splitlines()[-6:]))
    if r.returncode != 0:
        fail("the golden gate found undeclared differences — either they are a "
             "regression, or they belong in tools/golden/expected.txt with a reason")


def _rebaseline_reminder(tag):
    """What has to happen AFTER the release exists, and nowhere else.

    The gate compares against the previous release. So the moment this one is cut,
    the committed baseline is a version out of date and every subsequent run
    reports the whole release as a difference. --accept re-anchors it and empties
    expected.txt, which is what stops the declarations accumulating across
    versions the way the old JS exception list did.

    A reminder rather than an automatic step, because it rewrites five committed
    files and wants to be one deliberate commit with a human's name on it."""
    print("")
    print("=" * 72)
    print("  %s is cut. One step left, and it is not optional:" % tag)
    print("")
    print("      py -3 tools/compare_golden.py --accept")
    print("      git add tools/golden && git commit -m \"Re-baseline the golden gate on %s\"" % tag)
    print("")
    print("  That re-anchors tools/golden/ on this release and empties")
    print("  expected.txt. Skip it and the next run reports all of %s as a" % tag)
    print("  difference, and the declarations start piling up across versions.")
    print("=" * 72)


# ---- package ----------------------------------------------------------------

def _build_and_pack():
    """Runs the platform build script's `package` action, which builds the game
    and then hands off to CPack. Going through the script rather than calling
    cmake directly is the point: on Windows neither CMake nor Ninja nor the MSVC
    toolchain is normally on PATH, and build.bat is what knows how to find all
    three.

    Built as Release, not the RelWithDebInfo a plain `build.bat` gives you, and
    the reason is about what ends up in a binary handed to strangers rather than
    about speed. RelWithDebInfo compiles with /Zi, which writes the absolute path
    of the .pdb into the executable, and it is not the configuration
    HR_SOURCE_ASSET_DIR is suppressed in — so a RelWithDebInfo zip carries the
    builder's username and directory layout, twice."""
    if os.name == "nt":
        cmd = [os.path.join(ROOT, "build.bat"), "release", "package"]
    else:
        cmd = [os.path.join(ROOT, "build.sh"), "release", "package"]
        if not os.access(cmd[0], os.X_OK):
            cmd = ["sh"] + cmd
    print("building:", " ".join(cmd))
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        fail("the build failed (see output above)")


def _find_zip(version):
    """CPack names the archive Hollowreach-vX.Y.Z-<CMAKE_SYSTEM_NAME>.zip, and the
    system name is whatever CMake decided rather than anything Python knows, so
    the file is found rather than predicted."""
    found = sorted(glob.glob(os.path.join(DIST, "Hollowreach-v%s-*.zip" % version)))
    if not found:
        fail("no dist/Hollowreach-v%s-*.zip was produced — did the packaging step "
             "run? Try 'build.bat package' on its own to see why not." % version)
    if len(found) > 1:
        # Two platforms' zips can legitimately sit here at once if they were
        # copied in for one release, but only one was just built.
        found.sort(key=os.path.getmtime)
    return found[-1]


def cmd_package(no_build=False):
    version, _ = read_version()
    # Checked BEFORE building, because a missing changelog section is a
    # three-second fix and a full build is not.
    notes = changelog_section(version)
    if notes is None:
        fail("CHANGELOG.md has no '## [%s]' section — run "
             "'python tools/release.py bump ...' (or add one by hand) first" % version)
    if not notes:
        fail("the '## [%s]' section of CHANGELOG.md is empty" % version)
    if _git_dirty():
        print("warning: uncommitted changes in the working tree — the zip is "
              "built from the files on disk, not from a git ref")

    os.makedirs(DIST, exist_ok=True)
    if no_build:
        print("skipping the build (--no-build)")
    else:
        _build_and_pack()

    zip_path = _find_zip(version)
    # CPack's staging tree. Harmless, ignored by git, and confusing to find in a
    # folder that is otherwise exactly what gets uploaded.
    shutil.rmtree(os.path.join(DIST, "_CPack_Packages"), ignore_errors=True)

    notes_path = os.path.join(DIST, "RELEASE_NOTES-v%s.md" % version)
    with open(notes_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(RELEASE_NOTES_TEMPLATE.format(version=version, notes=notes,
                                              zip_name=os.path.basename(zip_path)))

    size = os.path.getsize(zip_path) / (1024 * 1024)
    print("packaged -> %s (%.1f MB)" % (os.path.relpath(zip_path, ROOT), size))
    print("notes    -> %s" % os.path.relpath(notes_path, ROOT))
    return zip_path, notes_path, version


RELEASE_NOTES_TEMPLATE = """\
{notes}

---

## How to play

1. Download the zip for your system below and unzip it anywhere.
2. Run **`Hollowreach`**.

That is the whole installation. There is no runtime to install and no installer
to run: the assets are baked into the executable and everything links statically
apart from your graphics driver. The game creates a `data/` folder next to
itself for your worlds, screenshots and settings, so it stays portable — move
the folder and everything moves with it.

The only requirement is a GPU and driver supporting **OpenGL 3.3**, which is
anything from roughly 2010 onward.

**Multiplayer note:** everyone needs the same game version (v{version}), and
this is LAN play — hosting for someone outside your network needs port
25565/udp forwarded to the host.
"""


# ---- publish ----------------------------------------------------------------

def cmd_publish(draft, no_build=False, skip_checks=False):
    if shutil.which("gh") is None:
        fail("the GitHub CLI (`gh`) is not installed or not on PATH — install "
             "it from https://cli.github.com/ and run `gh auth login`, or "
             "create the release by hand (see docs/RELEASING.md)")
    # Before the build, so a failure costs seconds rather than a full package.
    if skip_checks:
        print("skipping the pre-ship checks at your request")
    else:
        _preflight()
    zip_path, notes_path, version = cmd_package(no_build)
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
    _rebaseline_reminder(tag)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bump", help="advance the project version and date the changelog")
    b.add_argument("part", choices=["major", "minor", "patch"])
    k = sub.add_parser("package", help="build the release zip + notes into dist/")
    k.add_argument("--no-build", action="store_true",
                   help="use the zip already in dist/ instead of rebuilding")
    p = sub.add_parser("publish", help="package, then create the GitHub release via gh")
    p.add_argument("--draft", action="store_true", help="create as a draft release")
    p.add_argument("--no-build", action="store_true",
                   help="use the zip already in dist/ instead of rebuilding")
    p.add_argument("--skip-checks", action="store_true",
                   help="do not run --selftest and the golden gate first")
    args = ap.parse_args()

    if args.cmd == "bump":
        cmd_bump(args.part)
    elif args.cmd == "package":
        cmd_package(args.no_build)
    else:
        cmd_publish(args.draft, args.no_build, args.skip_checks)


if __name__ == "__main__":
    main()
