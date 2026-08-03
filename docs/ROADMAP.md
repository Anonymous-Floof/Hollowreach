# Roadmap: the engine work, and what 3.0 is for

Written 2026-08-03, immediately after 2.4.0 was drafted. This document exists to
survive a context reset — it assumes the reader knows nothing about the
conversation that produced it, so it repeats measurements rather than referring
to them.

Two horizons:

- **2.5.0** — engine work with no new gameplay. Four items, in a deliberate
  order, each one making the next safe or cheap.
- **3.0.0** — the deliberate break: entity rewrite, the last of the web-build
  shapes, and multiplayer that does not need a forwarded port.

---

## Standing decisions

**Existing saves do not have to load.** Decided 2026-08-03. This is the single
biggest constraint being lifted and most of the plan below depends on it. It
means `kSaveVersion` can change freely, `save/migrate.h` can be emptied rather
than extended, and chunk storage can change shape without a conversion path. It
does **not** mean the save format stops being versioned — a 3.0 build should
still *refuse* a 2.x world with a clear message rather than crash on it or, far
worse, load it wrong.

**The golden gate is being re-anchored, not kept.** See item 2. The project has
diverged from the archived web build by design and matching it is no longer a
goal.

**Releases are drafted, never auto-published.** The maintainer publishes by hand.

---

## The order, and why it is this order

```
1. Incremental lighting   — self-contained, already covered by tests, pays immediately
2. Re-anchor the gate     — cheap, and it is what makes 3 and 4 safe to attempt
3. Chunk storage          — the big one; wants 2 in place first
4. Worldgen throughput    — wants 2 in place first, and is the least important
```

Doing 3 or 4 before 2 means changing the two systems whose output is bulk
generated data with **no way to prove the output did not change**. That is the
whole argument for the ordering.

---

## 1. Incremental lighting

**Where:** `src/world/lighting.cpp`, `src/world/lighting.h`, and the dirty
tracking in `src/world/world.cpp` (`installResults`, `scanAndSubmit`,
`dirtyLight`).

**Current design.** `computeLight` rebuilds a whole chunk from zero on every
relight: it zeroes both channels, refills skylight by dropping a column from
the top, seeds the four border planes from whatever the neighbours are *currently
storing*, then floods. That makes lighting a fixed-point iteration over the
loaded chunks, which is why 2.4.0 had to add a neighbour-notification step to
drive it (`neighbourWouldChange` in `world.cpp`).

**Why replace it.** Rebuild-from-zero costs 32768 cells × 2 channels for a single
torch. Measured in 2.4.0: the notification machinery that makes the iteration
converge costs **about 1 second on a render-distance-12 world load** (3.85 s →
4.85 s, measured against the shipped 2.3.0 binary, repeated, same data
directory). Steady-state frames are unaffected — the gap is the same at 400 and
1600 frames.

**What to build.** The standard two-BFS scheme:

- *Adding* a light: push the changed cell, flood outward while `level - 1 >
  current`. Cost is bounded by the light radius (15), not by the chunk.
- *Removing* a light: a removal BFS that clears cells whose value came from the
  removed source, collecting the boundary of cells that did not, then re-add
  from that boundary.
- Skylight column changes when a block is placed or broken: re-drop the affected
  column only, then add/remove from the cells that changed.

The BFS must be free to cross chunk borders — which is the point, and which is
what lets most of `installResults`'s notification code be **deleted**, including
`neighbourWouldChange` and the `lightInFlight` guard beside it.

**Traps, all of which cost time in 2.4.0:**

- `seedBorders` refuses to seed an opaque cell. Any predicate that asks "would
  the neighbour change" must skip opaque cells or it never terminates — two
  chunks will offer each other light a wall can never accept, forever. This hung
  the self-test once already.
- A neighbour whose light job is *in flight* has `lit == false` but is **not** in
  the same state as one that has never been lit: it is computing from an older
  snapshot. Treating those two as one case produced a race that changed the world
  hash between 0 and 8 workers about once in twenty-five builds, and it shipped
  into a draft artifact before being caught. Whatever replaces this must keep
  that distinction or make it structurally impossible.
- The flood's flat-index trick (`+1` x, `+16` z, `+256` y) is only valid because
  `CX == CZ == 16`. It is documented at the top of `lighting.cpp`. Item 3 changes
  the storage under it.

**Tests that already exist and must keep passing:** `testLighting()` in
`src/dev/selftest.cpp` — a torch one cell from a chunk border must light 13
across the seam, removing it must unlight the far side, and a sealed room
straddling a seam must be dark all the way across. Plus the threading
determinism block in `testThreading()`, which now does thirteen 8-worker builds
and compares every world hash against the inline build.

---

## 2. Re-anchor the golden gate

**Where:** `tools/compare_golden.py`, `src/dev/golden.cpp`, `tools/jsref.mjs`.

**Current design.** `compare_golden.py` dumps 610 values from the native build
(`--golden <path>`) and diffs them against the archived WebGL2 build, whose `js/`
directory must be pointed at by `HOLLOWREACH_JS`. It reports 10 known documented
differences: 3 atlas, 6 recipes, 1 mesh.

**Why change it.** Two reasons, the second decisive:

- The exception list only grows. Every deliberate divergence costs a permanent
  entry to keep alive a comparison that is no longer a project goal.
- **The gen dump covers versions 1 and 2 only.** Three loops in `golden.cpp` read
  `for (int ver = 1; ver <= 2; ++ver)`. Worlds generate at **v4**. So the check
  guarding the worst regression available — terrain shape shifting between
  releases — structurally cannot cover the version anyone plays, because v3 and
  v4 do not exist in the JS to compare against. The JS anchor is what prevents
  the gate covering the thing that matters most.

**What to build.** Same script shape, same 610 values, reference becomes **the
previous release's dump** instead of the web build:

- Commit `tools/golden/v2.4.0.txt` (or similar) per release. 610 values is a
  small, diffable text file.
- Extend the gen loops to cover the current `kGenVersion`.
- Drop the 10 JS exceptions; replace with a per-release "expected changes" list
  that is **cleared each version** rather than accumulating.
- Drop the `HOLLOWREACH_JS` dependency entirely.
- Report-and-acknowledge, never demand equality. A gate that fires every release
  gets ignored, and legitimate changes will fire it.

It covers generated tables and terrain, **not rendering**. 2.4.0's lighting fix
legitimately changed pixels in every frame and a content dump would neither
catch nor care.

---

## 3. Chunk storage

**Where:** `src/world/chunk.h` is the definition; the consumers are
`lighting.cpp`, `mesher.cpp`, `worldgen.cpp`, `world.cpp`, `save/format.cpp` and
the net world transfer.

**Current design.** Five flat arrays over all 32768 cells of a 16×192×16 chunk:

| array | type | bytes |
|---|---|---|
| `voxels` | `uint16` (`BlockId`) | 64 KB |
| `meta` | `uint8` | 32 KB |
| `skylight` | `uint8` | 32 KB |
| `blocklight` | `uint8` | 32 KB |
| | | **160 KB per chunk, always** |

| Render distance | Chunks | Resident |
|---|---|---|
| 8 | 289 | 45 MB |
| 12 | 625 | **98 MB** |
| 16 | 1089 | 170 MB |

Paid in full whether the chunk is a cave system or 190 cells of empty sky over
one layer of grass, which most of them are.

**What to build.** Sectioned storage with a per-section palette — 16×16×16
sections, 12 per chunk, each holding a small palette of block ids and 2–4 bits
per cell, with an all-air section costing approximately nothing. Real terrain
typically wins 8–15×.

**What the player gets.** Render distance 16 becomes affordable, which is the
most visible change available to this project. Everything downstream gets faster
for free because the lighting BFS and the mesher walk these arrays constantly
and the working set collapses.

**Notes:**

- The copy-on-write clone in `World::mutableData` (`world.cpp:95`) currently
  copies 160 KB. It is already well mitigated — the `use_count() > 1` test means a
  run of edits in one frame pays for exactly one copy — but with palettes it
  stops mattering at all.
- `BlockId` is `uint16` and there are 125 registered blocks. A palette index of
  4–8 bits is plenty per section.
- **Block ids and recipe indices are handed out in registration order.** Anything
  new must be registered LAST or every existing world's blocks shift. This is a
  standing invariant of the project, not specific to this work.
- Save compatibility is not required (see standing decisions), so the on-disk
  chunk representation can change with the in-memory one.

---

## 4. Worldgen throughput

**Where:** `src/world/worldgen.cpp`, `src/world/noise.cpp`.

Noise is scalar and per-column. Generation already runs on workers, so this is
throughput rather than architecture — SIMD across a column or a row of columns
would plausibly give 2–4×, visible as smoother streaming when moving fast.

Least important of the four, and genuinely unsafe until item 2 lands: touching
the noise without a v4 baseline risks shifting terrain under every existing
world with nothing to catch it. `kGenVersion` gates worldgen behaviour and every
comparison uses `>=` / `<`, never `==`, so a new version inherits the previous
one's behaviour.

---

## 3.0.0 — the deliberate break

Slated together because they are all compatibility breaks and should cost the
player one upgrade, not three.

### Entity system

`src/game/entities/entity.h:61` — `EntityData` is one struct carrying the union
of every entity's fields. A dropped stone carries a `PathFollower`, a flee angle,
a stuck watchdog and a `despawn` timer; a zombie carries `pickupDelay` and `bob`.
Plus a `std::string key` per entity. This is a JavaScript object literal ported
across, where unset properties were free.

Worth saying plainly: **this costs the player nothing today.** Entity counts are
small. It is a correctness-and-clarity change, not a performance one, and it
should be sold as such rather than dressed up.

The shape to move to is per-type storage with a common header — `id`, `type`,
`pos`, `vel`, `yaw`, `onGround`, `dead` — and type-specific payloads held
separately, which also makes the save encoding per-type instead of one fat
record.

### Residual web-build shapes

Reviewed and deliberately **not** on this list: the UI `Doc` / `Style` / tween
system in `src/ui/`. It is unmistakably a browser reimplementation — a node tree
rebuilt every frame, tweens keyed by `(tag, index)` so they survive the rebuild,
CSS `ease` transcribed by hand. It also works, is finished, and rewriting it buys
the player nothing. It has not been profiled; profile before believing it costs
frame time.

### Multiplayer without a forwarded port

The current transport is ENet over UDP (`src/net/transport.*`), LAN-first. The
browser build got NAT traversal free from WebRTC; UDP does not, so playing with
someone outside the network needs **25565/udp** forwarded on the host's router.
`README.md` documents this as a known limit and the transport layer already has
an explicit seam for a relay or hole-punch backend.

This needs real design work before any code — at minimum: whether to run a relay
(who pays for it, and it is a service to operate, not a feature to ship), or
attempt hole-punching with a rendezvous server and accept that symmetric NATs
will fail, or both with a fallback. The invite-code envelope (`HRW1…HRW1`,
Crockford base32) is already the natural place to carry a rendezvous token.

Current protocol version is `kNetVersion = 4` (`src/net/protocol.h`). Guests are
untrusted and the host range-checks, reach-checks and rate-limits everything;
whatever transport replaces this must not lose that.

---

## Things a future session will otherwise rediscover the hard way

- `build.bat` must be invoked from PowerShell as `& cmd.exe /c ".\build.bat"`. It
  hangs through the Bash tool.
- Python is `py -3`. Plain `python`/`python3` is not on PATH.
- **Do not pass `--data-dir` with a possibly-empty shell variable.** It will
  swallow the next argument, and a run that gets `--data-dir --world` creates a
  directory literally called `--world` holding a log with this machine's absolute
  asset path in it. That happened on 2026-08-03 and was committed and pushed
  before anyone noticed. `.gitignore` now covers `--*/` and `data/`.
- **Perf: pair, never sweep.** Running the nine-cell table in sequence reads about
  1.6× the interleaved pairing by the last cell, because the GPU clocks down over
  the run. And an old binary unzipped elsewhere brings its own `data/`, hence its
  own resolution — which silently compares 1280×720 against 1920×1080 and doubles
  every GPU pass. Both traps are documented in `README.md` next to the table.
- The pre-ship checks are manual and there is no CI. `docs/RELEASING.md` says so.
- `dumpbin /dependents` is not on PATH on this machine, so the import check in
  `RELEASING.md` has been skipped for the last three releases.
