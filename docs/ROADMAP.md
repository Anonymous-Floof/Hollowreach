# Roadmap: the engine work, and what 3.0 is for

Written 2026-08-03 after 2.4.0 was drafted; rewritten 2026-08-04 when the first
three items landed. This document exists to survive a context reset — it assumes
the reader knows nothing about the conversation that produced it, so it repeats
measurements rather than referring to them.

Two horizons:

- **2.5.0** — engine work with no new gameplay. Three of its four items are
  done; what remains is worldgen throughput.
- **3.0.0** — the deliberate break: entity rewrite, the last of the web-build
  shapes, and multiplayer that does not need a forwarded port.

---

## Standing decisions

**Existing saves do not have to load.** Decided 2026-08-03. It means
`kSaveVersion` can change freely, `save/migrate.h` can be emptied rather than
extended, and chunk storage can change shape without a conversion path. It does
**not** mean the save format stops being versioned — a 3.0 build should still
*refuse* a 2.x world with a clear message rather than crash on it or, far worse,
load it wrong.

*(As it turned out, banded chunk storage needed none of this: only player edits
are persisted, cell by cell, and that encoding never changed. The licence is
still granted and still unused.)*

**Releases are drafted, never auto-published.** The maintainer publishes by hand.

---

## Two corrections to this document's own numbers

Both were written here on 2026-08-03 and both were wrong. They are recorded
rather than quietly fixed, because each was wrong in a way worth not repeating.

**A chunk was 240 KB, not 160 KB.** `kCellsPerChunk` is 16 × 192 × 16 = **49152**,
not the 32768 this file and `chunk.h`'s own comment claimed — 32768 is the figure
from before the world height went to 192 at gen v3, and it survived the change in
two comments. So the flat cost was 49152 × 5 = 240 KB, and the resident totals
were 1.5× larger than stated: **135 MB at render distance 12** and **228 MB at
16**, not 98 MB and 170 MB. The case for the storage work was stronger than the
argument made for it.

**Lighting did not cost "about 1 second on a render-distance-12 world load".**
That came from timing whole runs of `--frames 400`, which are **vsync-bound** —
2.3.0, 2.4.0 and the current build all take 3.9–4.0 s for those 400 frames
regardless of how much streaming work they do, so the measurement could not see
the thing it was quoted for. Which is the trap this very file documents under
"perf: pair, never sweep", written on the same day.

Measure main-thread streaming instead: run with `--no-vsync --perf` and read the
`streaming on the main thread over N frames` line. Over a distance-12 load:

| | total main-thread streaming, 1200 frames | worst frame |
|---|---|---|
| 2.3.0 (seams broken) | 313 ms | 7.5 ms |
| 2.4.0 (correct, iterative) | 372 ms | 10.7 ms |
| 2.5.0-dev (incremental + banded) | **235 ms** | 10.0 ms |

So correctness cost 59 ms in 2.4.0, not a second, and it is now well below the
build that did not even have it.

---

## Done

### 1. Incremental lighting — landed

`computeLight` still exists and is still a rebuild from zero, but a chunk now
gets **exactly one** of them, on a worker, when it has just been generated.
Everything after that is incremental: an add/remove BFS over world coordinates,
free to cross chunk borders, in `src/world/lightengine.cpp`.

Two properties the old scheme had to be argued into now fall out of the design.
The add pass is monotone, so it converges on the unique least assignment where
every passable cell is at least its brightest neighbour minus one — the same
answer whatever order the seeds arrive in. And a full pass can only ever land
*below* that, since it uses real sources and real neighbour values and invents
nothing, so a stale snapshot is no longer a race: it produces light that is
merely too low, and `seedSeams` raises it when the job lands. The `lightInFlight`
guard and `neighbourWouldChange` were **deleted rather than fixed**.

`Chunk::needsLight` is set once and never set again. If a second full relight is
ever reintroduced, it will land on top of incremental light and undo it — the
comment on that field says so.

The check that matters is `worstLightDrift` in `selftest.cpp`: it rebuilds every
lit chunk from zero, repeatedly, until the whole loaded set stops changing, and
reports the worst cell that disagrees with the live world. It runs over an
edited arena and a freshly streamed world. **Nothing else in the suite would
notice this drifting** — the golden mesh dump calls `computeLight` itself, so it
compares the rebuild against the rebuild.

### 2. The golden gate — re-anchored

`tools/compare_golden.py` no longer needs `HOLLOWREACH_JS` or a checkout of the
archived web build. The baseline is `tools/golden/{gen,atlas,recipes,mesh}.txt`,
committed, dumped from the previous release.

The gen loops run to `kGenVersion` instead of stopping at 2, and a `chunkFull`
hash covers all 192 layers beside the old 128-layer one. Coverage: 614 values
plus 140 labels the old anchor made impossible.

Differences are reported in full and fail only when undeclared:
`tools/golden/expected.txt` holds fnmatch patterns for what the working tree
changes on purpose. `--accept` re-baselines and empties it, and `RELEASING.md`
puts that **after** the release is cut — the declarations are one version's worth
of intent and are then thrown away, which is what stops the list accumulating the
way the JS exception list did.

The re-anchoring was verified, not asserted: the committed baseline came from the
**shipped 2.4.0 binary**, and the current build reproduces all 217 gen labels,
158 atlas tiles, 209 recipes and 30 meshes without one value moving.

### 3. Chunk storage — landed as bands

Each of the four per-cell arrays is stored as twelve 16-tall **bands**, each
either one repeated value or a dense 4096-entry buffer. Settled, same seed and
data directory:

| Render distance | Chunks | Flat | Banded | |
|---|---|---|---|---|
| 12 | 577 | 135.2 MB | **40.0 MB** | 3.38× |
| 16 | 973 | 228.0 MB | **66.9 MB** | 3.41× |

The split is free rather than a re-indexing, because `localIdx` runs y slowest:
16 y-layers of 256 cells is a contiguous run of 4096 flat indices, so a band is
`i >> 12` and an offset inside it is `i & 4095`.

**No palette and no bit packing**, deliberately. After the uniform bands cost
nothing, what is left is genuinely mixed, and a palette over its four or five
block ids would save a few more kilobytes in exchange for an indirection in the
mesher's and the lighting flood's innermost loops. Measure before believing that
trade — `ChunkData::bytes()` and the `chunk storage:` line in the `--perf` report
are there for exactly that.

`computeLight` is the exception: it expands a chunk into `thread_local` flat
buffers, works there, and packs back with `loadFrom`.

Called **bands** because `mesher.h` already has sections (32 tall, for draw
culling, and not aligned with these), and because the game has slab blocks.

---

## Still to do in 2.5.0

### 4. Worldgen throughput

**Where:** `src/world/worldgen.cpp`, `src/world/noise.cpp`.

Noise is scalar and per-column. Generation already runs on workers, so this is
throughput rather than architecture — SIMD across a column or a row of columns
would plausibly give 2–4×, visible as smoother streaming when moving fast.

The least important of the four, and the reason it was sequenced last: touching
the noise risks shifting terrain under every existing world. That is now a much
smaller risk than it was, because the gate covers `chunk` and `chunkFull` hashes
at **every** generator version including the one worlds actually use — which was
the entire argument for doing item 2 before this.

`kGenVersion` gates worldgen behaviour and every comparison uses `>=` / `<`,
never `==`, so a new version inherits the previous one's behaviour.

**Measure first.** Nobody has profiled generation since the job system landed. It
is on workers, and the numbers above say main-thread streaming is 235 ms over a
whole distance-12 load — if generation is not the thing a player waits on, this
item should be dropped rather than done.

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

Current protocol version is `kNetVersion = 6` (`src/net/protocol.h`). Guests are
untrusted and the host range-checks, reach-checks and rate-limits everything;
whatever transport replaces this must not lose that.

### The ownership rule, now that it is actually enforced

**The host owns every entity in the world. A guest owns none.** That was always
the design and for several releases it was only half true — the world payload
shipped the host's entity list, and `startWorld` loads whatever it is given, so
every guest built a private local copy of every mob and ticked it with its own
AI. `sendWorld` now clears the list and `adoptRemoteWorld` clears it again.

Two consequences worth keeping in mind before touching this area:

- **A guest's `EntityManager` holds nothing but ghosts, and ghosts are never
  ticked.** Anything that has to happen *to* an entity happens on the host. That
  is why picking a drop up is a host-side pass (`Host::collectDrops`) rather than
  something the guest does locally, and why a guest's mined item goes straight
  into its inventory instead of becoming a drop entity.
- **`EntityContext::sharedWorld` marks the rules that quietly assume one player.**
  Today that is the instant-drop vacuum, which has no distance check at all; there
  will be others, and this is where they go.

### The fast channel is unsequenced, and always was

`Channel::Fast` is `ENET_PACKET_FLAG_UNSEQUENCED`. Poses and snapshots therefore
arrive in arbitrary order, and a `Ghosts::Track` stamps every sample with its
*arrival* time — so a late packet is not ignored, it is believed. Both ends now
carry a counter and use `net::newerSeq` to discard anything already passed. Any
new message on that channel needs the same treatment or it inherits the same bug.

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
- **Perf: pair, never sweep — and never time a vsync-bound run.** Running the
  nine-cell quality table in sequence reads about 1.6× the interleaved pairing by
  the last cell, because the GPU clocks down over the run. An old binary unzipped
  elsewhere brings its own `data/`, hence its own resolution, which silently
  compares 1280×720 against 1920×1080. And wall-clock over `--frames N` measures
  the display's refresh rate, not the game — pass `--no-vsync` and read `--perf`.
  All three are documented in `README.md` beside the table; the third one was
  learned by getting it wrong twice.
- A test that has never failed has proved nothing. Both engine changes here were
  verified by deliberately reverting them and confirming the new checks caught it
  — removing `seedSeams` fails the drift check at 0 against 12, weakening the
  remove pass fails it at 14 against 0 and takes eight older checks with it.
- The pre-ship checks are manual and there is no CI. `docs/RELEASING.md` says so.
- `dumpbin /dependents` is not on PATH on this machine, so the import check in
  `RELEASING.md` has been skipped for the last three releases.
