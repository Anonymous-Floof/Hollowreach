#!/usr/bin/env python3
"""Verify this port against the original JavaScript it was ported from.

    python tools/compare_golden.py            # everything
    python tools/compare_golden.py --only gen # one group

Runs the reference implementation and this build side by side and diffs the
results. This is the gate for any change under src/world, src/resource or
src/core/prng.

The reference is NOT in this repository. Hollowreach was forked out of the web
version's repo and the js/ tree stayed with the archive, so a checkout of
https://github.com/Anonymous-Floof/Hollow-Reach has to exist somewhere and
HOLLOWREACH_JS has to point at its js/ directory. tools/jsref.mjs does the
looking and says so plainly when it comes up empty.

Four groups, ordered by how much each narrows a failure:

  gen    PRNG, noise permutation tables, field samples, heights, biomes, whole
         generated chunks. Exact — any difference is a bug.
         A prng failure means nothing else can match; suspect imul/ushr.
         A noise failure means the Fisher-Yates shuffle; every later value is
         then unrelated rather than merely close.
         A fields failure with matching tables means float contraction.
         A worldgen failure with matching fields means integer semantics
         (floorDiv is the usual culprit).

  atlas  Per-tile hashes of every procedural texture. Exact. The atlas *layouts*
         differ by design, so tiles are compared individually rather than as one
         image.

  recipes  The crafting table, the smelting table, the derived fuel value of
         every item, and a fixed set of grids run through the matcher. Exact.
         Most of the table is generated from the block registry, so one wrong
         loop silently drops a whole family of recipes.

  mesh   Chunk geometry, ambient occlusion, smooth lighting, water surfaces.
         Positions are compared on a 1/16-block grid; see mesh_golden.mjs for why.
         One chunk of the fixture is expected to differ: three vertices sit one
         float32 ULP apart because std::sin and Math.sin disagree on a couple of
         the plant-jitter angles. That is reported as a known difference, not a
         failure. Use --strict to fail on it anyway.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

# Chunks whose only difference is the documented one-ULP transcendental drift.
KNOWN_MESH_DIFFS = {"mesh(3918175327, v1, 7, -3)"}


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


def compare(name: str, expected: dict[str, str], got: dict[str, str],
            known: set[str], strict: bool) -> tuple[int, int, int]:
    """Returns (checked, failures, known-differences)."""
    failures: list[str] = []
    known_hits = 0
    for label, want in expected.items():
        if label not in got:
            failures.append(f"MISSING in native: {label}")
            continue
        if got[label] == want:
            continue
        if label in known and not strict:
            known_hits += 1
            continue
        failures.append(f"{label}\n      js  = {want}\n      c++ = {got[label]}")

    extra = sorted(set(got) - set(expected))
    for label in extra:
        failures.append(f"EXTRA in native: {label}")

    status = "OK" if not failures else "FAIL"
    note = f", {known_hits} known difference(s)" if known_hits else ""
    print(f"  [{status}] {name}: {len(expected)} values{note}")
    for f in failures[:6]:
        print(f"    {f}")
    if len(failures) > 6:
        print(f"    ... and {len(failures) - 6} more")
    return len(expected), len(failures), known_hits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", help="path to the native build")
    ap.add_argument("--only", choices=["gen", "atlas", "recipes", "mesh"],
                    help="run one group")
    ap.add_argument("--strict", action="store_true",
                    help="fail on known transcendental differences too")
    args = ap.parse_args()

    if not shutil.which("node"):
        sys.exit("error: node is not on PATH; it runs the reference implementation in js/")
    exe = find_exe(args.exe)
    print(f"reference: js/   candidate: {exe.name} "
          f"({exe.parent.parent.name})\n")

    out_dir = REPO / "build" / "golden"
    out_dir.mkdir(parents=True, exist_ok=True)

    groups = [args.only] if args.only else ["gen", "atlas", "recipes", "mesh"]
    total_checked = total_failed = total_known = 0

    for group in groups:
        if group == "gen":
            js = run(["node", str(HERE / "gen_golden.mjs")], "the JS generator dump")
            # Sections are listed explicitly: with none given the native dumper
            # emits every section it knows, including mesh, which belongs to its own
            # group and would show up here as unmatched extra lines.
            cpp = run([str(exe), "--dump-golden", "-", "--sections",
                       "prng,noise,fields,worldgen,chunks"],
                      "the native generator dump")
        elif group == "atlas":
            js = run(["node", str(HERE / "atlas_golden.mjs")], "the JS atlas dump")
            png = out_dir / "cpp-atlas.png"
            run([str(exe), "--dump-atlas", str(png)], "the native atlas dump")
            cpp = (png.with_suffix(".png.txt")).read_text(encoding="utf-8")
        elif group == "recipes":
            js = run(["node", str(HERE / "recipes_golden.mjs")], "the JS recipe dump")
            cpp = run([str(exe), "--dump-recipes", "-"], "the native recipe dump")
        else:
            js = run(["node", str(HERE / "mesh_golden.mjs")], "the JS mesh dump")
            cpp = run([str(exe), "--dump-golden", "-", "--sections", "mesh"],
                      "the native mesh dump")

        (out_dir / f"js-{group}.txt").write_text(js, encoding="utf-8")
        (out_dir / f"cpp-{group}.txt").write_text(cpp, encoding="utf-8")

        known = KNOWN_MESH_DIFFS if group == "mesh" else set()
        checked, failed, known_hits = compare(group, keyed(js), keyed(cpp), known,
                                              args.strict)
        total_checked += checked
        total_failed += failed
        total_known += known_hits

    print()
    if total_failed:
        print(f"{total_failed} difference(s) across {total_checked} values.")
        print(f"full dumps: {out_dir}")
        return 1
    suffix = f" ({total_known} known transcendental difference(s))" if total_known else ""
    # ASCII on purpose: this runs in cmd.exe, whose code page mangles anything else.
    print(f"MATCH - {total_checked} values reproduce the JavaScript exactly{suffix}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
