# Releasing Hollowreach

How a version becomes a public GitHub release. The whole thing is three
commands, and `tools/release.py` (stdlib-only Python, the same Python the game
already needs) does the fiddly parts.

## Version numbers

One public version, `GAME_VERSION` in [js/version.js](../js/version.js) — the
only place it's written down. It shows in the menu footer, gets stamped into
saves (`gameVersion`, diagnostics only), and appears in the multiplayer
version-mismatch message.

Semantic versioning, released as tag `vMAJOR.MINOR.PATCH`:

| bump  | when                                                        | example |
|-------|-------------------------------------------------------------|---------|
| major | big milestone or a compatibility break                      | v2.0.0  |
| minor | a content/feature update (new mobs, new systems…)           | v1.1.0  |
| patch | fixes and tuning only                                       | v1.0.1  |

The *internal* format versions are separate and only move when their format
actually changes — don't touch them for a release:

- `SAVE_VERSION` (js/save/serialize.js) — bump **with a migration** in
  js/save/migrate.js when the save shape changes.
- `GEN_VERSION` (js/world/worldgen.js) — bump when terrain generation changes
  shape, so old worlds keep their exact terrain.
- `NET_VERSION` (js/net/protocol.js) — bump when the multiplayer protocol
  changes. Multiplayer pairs on `SAVE_VERSION.NET_VERSION`, so a patch release
  that touches neither stays compatible with the previous release.

## Naming conventions

- git tag: `v1.2.0`
- release title: `Hollowreach v1.2.0`
- asset: `Hollowreach-v1.2.0.zip` — **one zip for all platforms**; it unzips
  to a `Hollowreach-v1.2.0/` folder containing `run.bat` (Windows) and
  `run.sh` (Linux/macOS, exec bit already set inside the zip).

## The release flow

**0. While developing** — describe changes in the `[Latest]` section of
[CHANGELOG.md](../CHANGELOG.md), written for players (grouped `### Added` /
`### Changed` / `### Fixed` headings work well). These lines become the release
notes verbatim, so this is the "outline changes and additions" step.

**1. Bump.**

```
python tools/release.py bump minor        # or major / patch
```

Moves the `[Latest]` entries under a dated `## [1.2.0]` heading and writes
the new number into js/version.js. Refuses to run if `[Latest]` is empty.
Review the two changed files, then commit and push them
(`Release v1.2.0` is a fine message).

**2. Publish.**

```
python tools/release.py publish           # add --draft to review it first
```

Builds `dist/Hollowreach-v1.2.0.zip` and `dist/RELEASE_NOTES-v1.2.0.md`
(changelog section + standard how-to-play instructions), then creates the
GitHub release with the right tag/title/asset via the `gh` CLI. Use `--draft`
if you want to eyeball it on github.com before it goes live.

`package` alone builds the zip + notes without touching GitHub — useful for
testing the zip locally (unzip it somewhere clean and double-click the
launcher) or for uploading by hand.

### No `gh`? Manual fallback

1. `python tools/release.py package`
2. `git tag v1.2.0 && git push origin v1.2.0`
3. On GitHub: Releases → *Draft a new release* → choose tag `v1.2.0`, title
   `Hollowreach v1.2.0`, paste `dist/RELEASE_NOTES-v1.2.0.md` as the body,
   attach `dist/Hollowreach-v1.2.0.zip`, publish.

## What's in the zip (and what isn't)

Ships: `index.html`, `css/`, `js/`, `server.py`, both launchers, `README.md`,
`LICENSE`, `CHANGELOG.md`. Excluded: `worlds/` (player data), `screenshots/`,
`tools/`, `docs/`, `.git/`, and everything else dev-only. `run.sh` is
normalised to LF line endings and gets its unix executable bit set *inside the
zip*, so it stays double-click-runnable even though the zip is built on
Windows.
