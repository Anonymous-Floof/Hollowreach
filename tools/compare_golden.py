#!/usr/bin/env python3
"""Check this build's generated content against the last release's.

    py -3 tools/compare_golden.py              # everything
    py -3 tools/compare_golden.py --only gen   # one group
    py -3 tools/compare_golden.py --accept     # adopt this build as the baseline

Dumps every table the game generates -- terrain, textures, recipes, chunk
geometry -- and diffs it against the committed baseline in tools/golden/. It is
the gate for any change under src/world, src/resource or src/core/prng.

WHAT THIS COMPARES AGAINST, AND WHY IT CHANGED
----------------------------------------------
Until 2.5.0 the reference was the archived WebGL2 build this project was forked
out of: an external checkout, found through HOLLOWREACH_JS, diffed value for
value. That went, for two reasons.

The exception list only grew. Every deliberate divergence -- and divergence is
the point of a fork -- cost a permanent entry to keep alive a comparison that
had stopped being a goal.

And it could not see the thing most worth guarding. The generator dump ran
`for ver = 1..2` because versions 3 and 4 do not exist in the JavaScript to
compare against, while worlds generate at 4. So the check standing between a
release and the worst regression available -- terrain shape shifting under every
existing save -- structurally could not cover the version anyone plays. The
anchor was not merely redundant, it was in the way.

The baseline is now this project's own previous release. tools/*.mjs are kept as
the record of what the port was verified against for 1.x through 2.4.0; they no
longer run here, and the dump has since grown labels they cannot produce.

HOW A DIFFERENCE IS MEANT TO BE HANDLED
---------------------------------------
Not every difference is a bug -- most releases change something on purpose, and
a gate that fires every release gets ignored, which is worse than no gate. So
differences are reported in full either way, and the exit code asks only whether
each one was declared:

  * expected it?  add the label to tools/golden/expected.txt with a reason.
  * did not?      that is the gate firing. It is the regression.

At release time, `--accept` rewrites the baselines from the current build and
empties expected.txt, so the list never accumulates the way the old one did.
docs/RELEASING.md says where in the sequence that goes.

Four groups, ordered by how much each narrows a failure:

  gen    PRNG, noise permutation tables, field samples, heights, biomes, whole
         generated chunks, at every generator version the build still knows.
         A prng failure means nothing else can match; suspect imul/ushr.
         A noise failure means the Fisher-Yates shuffle; every later value is
         then unrelated rather than merely close.
         A fields failure with matching tables means float contraction.
         A worldgen failure with matching fields means integer semantics
         (floorDiv is the usual culprit).

  atlas  Per-tile hashes of every procedural texture.

  recipes  The crafting table, the smelting table, the derived fuel value of
         every item, and a fixed set of grids run through the matcher. Most of
         the table is generated from the block registry, so one wrong loop
         silently drops a whole family of recipes.

  mesh   Chunk geometry, ambient occlusion, smooth lighting, water surfaces.
         Positions are compared on a 1/16-block grid; see mesh_golden.mjs.

This covers generated tables and terrain. It does NOT cover rendering: 2.4.0's
lighting fix legitimately changed pixels in every frame and a content dump would
neither catch nor care.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from fnmatch import fnmatch
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
BASELINE = HERE / "golden"
EXPECTED = BASELINE / "expected.txt"

GROUPS = ("gen", "atlas", "recipes", "mesh")

EXPECTED_HEADER = (
    "# Labels this working tree changes on purpose, one per line, with a reason\n"
    "# after a '#'. fnmatch patterns, so `chunk(*, v4, *)` covers a family.\n"
    "# compare_golden.py reports every difference either way; this file only\n"
    "# decides which ones fail the gate.\n"
    "#\n"
    "# Emptied by --accept at release time, so it can never accumulate the way the\n"
    "# JS exception list it replaced did.\n"
)


def find_exe(explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if not p.is_file():
            sys.exit(f"error: {p} does not exist")
        return p
    candidates = [
        REPO / "build" / cfg / "bin" / name
        for cfg in ("RelWithDebInfo", "Release", "Debug")
        for name in ("Hollowreach.exe", "Hollowreach")
    ]
    found = [p for p in candidates if p.is_file()]
    if not found:
        sys.exit("error: no built executable found; run build.bat first")
    # Newest wins, so switching configurations cannot silently compare a stale binary.
    return max(found, key=lambda p: p.stat().st_mtime)


def run(cmd: list[str], what: str) -> str:
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.exit(f"error: {what} failed")
    return proc.stdout


def dump(exe: Path, group: str, scratch: Path) -> str:
    """Runs the build's own dumper for one group."""
    if group == "gen":
        # Sections are listed explicitly: with none given the dumper emits every
        # section it knows, including mesh, which belongs to its own group.
        return run([str(exe), "--dump-golden", "-", "--sections",
                    "prng,noise,fields,worldgen,chunks"], "the generator dump")
    if group == "mesh":
        return run([str(exe), "--dump-golden", "-", "--sections", "mesh"], "the mesh dump")
    if group == "recipes":
        return run([str(exe), "--dump-recipes", "-"], "the recipe dump")
    png = scratch / "atlas.png"
    run([str(exe), "--dump-atlas", str(png)], "the atlas dump")
    return png.with_suffix(".png.txt").read_text(encoding="utf-8")


def keyed(text: str) -> dict[str, str]:
    """Parses `label = value` and `label(args) rest` lines into a dict."""
    out: dict[str, str] = {}
    for line in text.splitlines():
        line = line.rstrip("\r")
        if line.startswith("#") or not line.strip():
            continue
        if " = " in line:
            label, value = line.split(" = ", 1)
            out[label] = value
        else:
            m = re.match(r"^(\w+\([^)]*\))\s+(.*)$", line)
            if m:
                out[m.group(1)] = m.group(2)
    return out


def read_expected() -> list[str]:
    """Labels this working tree is knowingly changing, as fnmatch patterns.

    Patterns rather than plain labels because one decision usually moves a whole
    family at once -- widening the dump by a generator version adds a line per
    seed per chunk -- and a hundred literals would be neither readable nor
    reviewable. `chunk(*, v4, *)` says the thing the author actually meant.
    """
    if not EXPECTED.is_file():
        return []
    out: list[str] = []
    for line in EXPECTED.read_text(encoding="utf-8").splitlines():
        label = line.partition("#")[0].strip()
        if label:
            out.append(label)
    return out


def compare(name: str, base: dict[str, str], got: dict[str, str],
            expected: list[str]) -> tuple[int, list[str], int]:
    """Returns (values checked, undeclared differences, declared ones)."""
    undeclared: list[str] = []
    declared = 0

    def record(label: str, detail: str) -> None:
        nonlocal declared
        if any(fnmatch(label, pattern) for pattern in expected):
            declared += 1
        else:
            undeclared.append(detail)

    for label, want in base.items():
        if label not in got:
            record(label, f"GONE from this build: {label}")
        elif got[label] != want:
            record(label, f"{label}\n      baseline = {want}\n      this     = {got[label]}")

    for label in sorted(set(got) - set(base)):
        record(label, f"NEW in this build: {label}")

    status = "OK" if not undeclared else "FAIL"
    note = f", {declared} declared change(s)" if declared else ""
    print(f"  [{status}] {name}: {len(base)} values{note}")
    for f in undeclared[:6]:
        print(f"    {f}")
    if len(undeclared) > 6:
        print(f"    ... and {len(undeclared) - 6} more")
    return len(base), undeclared, declared


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", help="path to the built executable")
    ap.add_argument("--only", choices=GROUPS, help="run one group")
    ap.add_argument("--accept", action="store_true",
                    help="rewrite the baselines from this build and clear expected.txt")
    args = ap.parse_args()

    exe = find_exe(args.exe)
    scratch = REPO / "build" / "golden"
    scratch.mkdir(parents=True, exist_ok=True)
    BASELINE.mkdir(parents=True, exist_ok=True)
    groups = [args.only] if args.only else list(GROUPS)

    if args.accept:
        for group in groups:
            (BASELINE / f"{group}.txt").write_text(dump(exe, group, scratch), encoding="utf-8")
            print(f"  baseline updated: tools/golden/{group}.txt")
        # Only a full run may clear the declarations: emptying them after checking
        # one group would silently bless the three that were not looked at.
        if not args.only:
            EXPECTED.write_text(EXPECTED_HEADER, encoding="utf-8")
            print("  expected.txt cleared")
        return 0

    print(f"baseline: tools/golden/   candidate: {exe.name} ({exe.parent.parent.name})\n")
    expected = read_expected()
    total_checked = total_declared = total_undeclared = 0

    for group in groups:
        path = BASELINE / f"{group}.txt"
        if not path.is_file():
            sys.exit(f"error: no baseline at {path}; run with --accept to create one")
        got = dump(exe, group, scratch)
        (scratch / f"this-{group}.txt").write_text(got, encoding="utf-8")
        checked, undeclared, declared = compare(
            group, keyed(path.read_text(encoding="utf-8")), keyed(got), expected)
        total_checked += checked
        total_undeclared += len(undeclared)
        total_declared += declared

    print()
    if total_undeclared:
        print(f"{total_undeclared} undeclared difference(s) across {total_checked} values.")
        print(f"full dumps: {scratch}")
        print("If they are intended, add the labels to tools/golden/expected.txt with a reason.")
        return 1
    # ASCII on purpose: this runs in cmd.exe, whose code page mangles anything else.
    suffix = f" ({total_declared} declared change(s))" if total_declared else ""
    print(f"MATCH - {total_checked} values match the baseline{suffix}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
