# Releasing Hollowreach

How a version becomes a public GitHub release. The whole thing is three
commands, and `tools/release.py` (stdlib-only Python, no dependency on the
build) does the fiddly parts.

## Version numbers

One public version, the `VERSION` in the `project()` call in
[CMakeLists.txt](../CMakeLists.txt) — the only place it is written down. CMake
passes it to the game as `HR_VERSION`, so the build cannot disagree with the
changelog about which release this is. It shows in the menu footer, gets
stamped into saves (diagnostics only), and appears in the multiplayer
version-mismatch message.

Semantic versioning, released as tag `vMAJOR.MINOR.PATCH`:

| bump  | when                                                        | example |
|-------|-------------------------------------------------------------|---------|
| major | big milestone or a compatibility break                      | v2.0.0  |
| minor | a content/feature update (new mobs, new systems…)           | v1.1.0  |
| patch | fixes and tuning only                                       | v1.0.1  |

The *internal* format versions are separate and only move when their format
actually changes — don't touch them for a release:

- `kSaveVersion` (src/save/format.h) — bump **with a migration** in
  src/save/migrate.cpp when a section's layout changes. A *new* section needs
  neither: unknown tags are skipped.
- `kGenVersion` (src/world/worldgen.h) — bump when terrain generation changes
  shape, so old worlds keep their exact terrain.
- `kNetVersion` (src/net/protocol.h) — bump when the multiplayer protocol
  changes. Multiplayer pairs on the save and net versions, so a patch release
  that touches neither stays compatible with the previous release.

## Naming conventions

- git tag: `v1.2.0`
- release title: `Hollowreach v1.2.0`
- asset: `Hollowreach-v1.2.0-Windows.zip`, `Hollowreach-v1.2.0-Linux.zip`, … —
  **one zip per platform**, because the payload is a compiled executable. Each
  unzips to a folder of its own name holding `Hollowreach`, `README.md`,
  `LICENSE` and `CHANGELOG.md`.

## The release flow

**0. While developing** — describe changes in the `[Latest]` section of
[CHANGELOG.md](../CHANGELOG.md), written for players (grouped `### Added` /
`### Changed` / `### Fixed` headings work well). These lines become the release
notes verbatim, so this is the "outline changes and additions" step.

**1. Bump.**

```
python tools/release.py bump minor        # or major / patch
```

Moves the `[Latest]` entries under a dated `## [1.2.0]` heading and writes the
new number into the `project()` call. Refuses to run if `[Latest]` is empty.
Review the two changed files, then commit and push them (`Release v1.2.0` is a
fine message).

**2. Publish.**

```
python tools/release.py publish           # add --draft to review it first
```

Builds the game, has CPack produce `dist/Hollowreach-v1.2.0-<Platform>.zip`,
writes `dist/RELEASE_NOTES-v1.2.0.md` (changelog section + standard
how-to-play instructions), then creates the GitHub release with the right
tag/title/asset via the `gh` CLI. Use `--draft` if you want to eyeball it on
github.com before it goes live.

`package` alone does everything except touch GitHub — useful for testing the
zip locally. `--no-build` on either one skips straight to the packaging and
uses whatever is already in `dist/`.

**3. Other platforms.** A release is per-platform and this script packages
whichever machine it runs on. To ship more than one, publish as a draft from
the first, then on each other machine:

```
python tools/release.py package
gh release upload v1.2.0 dist/Hollowreach-v1.2.0-Linux.zip
```

…and publish the draft when they are all attached.

### No `gh`? Manual fallback

1. `python tools/release.py package`
2. `git tag v1.2.0 && git push origin v1.2.0`
3. On GitHub: Releases → *Draft a new release* → choose tag `v1.2.0`, title
   `Hollowreach v1.2.0`, paste `dist/RELEASE_NOTES-v1.2.0.md` as the body,
   attach the zips, publish.

## What's in the zip (and what isn't)

Ships: `Hollowreach` (the executable), `README.md`, `LICENSE`, `CHANGELOG.md`.
That is the whole list, and it is defined in
[cmake/package.cmake](../cmake/package.cmake) rather than in the release
script, so `build.bat package` and `release.py package` cannot disagree about
what a release contains.

There is no assets folder because `HOLLOWREACH_EMBED_ASSETS` bakes the shaders
into the executable, and no runtime DLLs because **the MSVC runtime is linked
statically** (`CMAKE_MSVC_RUNTIME_LIBRARY` in CMakeLists.txt). That second one
is not an optimisation: the default dynamic runtime makes the game depend on
`VCRUNTIME140.dll` and `MSVCP140.dll`, which ship with the Visual C++
Redistributable rather than with Windows, so a player who has never installed
Visual Studio gets a missing-DLL dialog instead of a game. If you ever change
it, check the built executable's imports before shipping:

```
python tools/release.py package
# then, on the unzipped exe — there should be no VCRUNTIME/MSVCP entries:
dumpbin /dependents Hollowreach.exe
```

Excluded: `data/` (player data, which the game creates on first run anyway),
`src/`, `tools/`, `docs/`, `screenshots/`, `.git/`, and everything else
dev-only.

## Before you ship

Neither of these is automated, and both have caught real problems:

```
build\RelWithDebInfo\bin\Hollowreach.exe --selftest
python tools/compare_golden.py
```

The self-test needs no window and takes seconds. The golden comparison needs a
checkout of the archived web build — see the note at the top of
`tools/compare_golden.py` — and is what proves a worldgen, atlas, recipe or
mesher change did not quietly alter the game.
