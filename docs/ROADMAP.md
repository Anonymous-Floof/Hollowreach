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

Current protocol version is `kNetVersion = 7` (`src/net/protocol.h`). Guests are
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

### An oversized packet on the fast channel is not a fast packet

The single worst bug this project has had, and none of it was in this project.

`Transport::send` asks for `ENET_PACKET_FLAG_UNSEQUENCED` on channel 1. ENet
decides what to do with a packet too big for one datagram in `enet_peer_send`
(`peer.c:136`), and the test it uses is

```c
(packet->flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT))
    == ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT
```

**`UNSEQUENCED` is not in that test.** It fails, and the `else` branch sends
`SEND_FRAGMENT | FLAG_ACKNOWLEDGE` — reliable, ordered, acknowledged,
retransmitted. The call returns 0 like any other. Nothing anywhere says a word.

The threshold is `mtu - sizeof(ENetProtocolHeader) - sizeof(ENetProtocolSendFragment)`,
1364 bytes at the default 1392 MTU and as little as 548 if a peer negotiates
`ENET_PROTOCOL_MINIMUM_MTU`. A snapshot entity costs about 33 bytes, so a world
with forty things in it is over it and stays over it. Twenty multi-fragment
reliable packets a second is more than the reliable window can carry:
`reliableDataInTransit` climbs past `(packetThrottle * windowSize) / 32`, the send
stalls at `protocol.c:1472`, and the next snapshot is already queued behind the
one that did not go. It does not recover. Channel 0 has its own window, which is
why the world went on being edited while everything alive in it stood still —
mobs and the other players froze in the same instant, because both travel in the
snapshot.

Two things guard it now and both are wanted:

- **The flag.** `Channel::Fast` sends `UNSEQUENCED | UNRELIABLE_FRAGMENT`, so
  something that does exceed a datagram degrades to "a lost fragment costs one
  snapshot" instead of wedging the channel.
- **The budget.** `Host::sendSnapshot` fills a message to `kSnapBudget` bytes,
  measured with `net::wireSize`, so it does not fragment in the first place. There
  is a self-test that the budget's arithmetic and the encoder agree, because
  nothing else in the game would ever notice them drifting apart.

**Anything new on the fast channel must be small.** Not "usually small" — bounded.

### A snapshot is what is near you, not what exists

`sendSnapshot` is per-peer: entities within `kSnapRange` of that guest, sorted
nearest first, cut at the byte budget. The count cap of 512 is still there and is
now unreachable in any world worth the check.

The old version was a census in entity-vector order, which is oldest first. That
matters more than it sounds, because `EntityManager::tick` skips a chunk that is
not loaded *before* `age += dt` — so a drop in an unloaded chunk never despawns.
A long session accumulates them, they are the oldest things in the vector, and
they fill the message before it reaches the mob standing next to the player. A
guest treats anything a snapshot does not mention as dead, so those mobs were not
merely stale: they were deleted and respawned, over and over.

The frozen-drop accumulation is still there and is now harmless to the network.
**It is also the reason the symptom was permanent**, and that is worth stating
plainly because it is not obvious: `App::buildSave` writes the entity list into
the save, so the pile survives quitting. A guest leaving and rejoining gets a new
ENet host, peer and channel set — new windows, new `reliableDataInTransit` — and
would have recovered if the broken state lived in the connection. It did not. The
world came back with the pile still in it, the first snapshot of the new session
was already over the datagram, and it re-wedged in seconds. The host restarting
did not help either, for the same reason and off the same file.

So a bug whose *mechanism* is entirely per-connection presented as one nothing
could clear, because its *trigger* is persisted. Worth remembering the next time
"restarting does not fix it" is used to rule something out.

The pile itself is now capped at `EntityManager::kMaxDrops` (1024), oldest id
first. A **count and not a clock**, deliberately: a clock would eventually take
things a player is still walking back for, and that the freeze keeps your
belongings however long you take is the entire point of it. A cap cannot take
anything anyone is coming back for, because reaching it means a thousand stacks
abandoned in chunks never revisited. The precedent is `kHostileDespawn`, whose
comment describes the identical failure for mobs.

The ageing rule itself is untouched, and should stay untouched — it matches what
players expect from this genre, and the leak was never the freeze, it was that
nothing counted.

### The rider owns the boat

A boat is the one entity a guest may simulate, and it is the same exception the
player already is: it goes where they go and only their keyboard turns it. So the
guest ticks the hull it is sitting in (`EntityManager::tick`), `Ghosts::update`
leaves that one body alone, and the host reads the boat's position back out of the
rider's pose (`Host::syncRiddenBoats`) — no new message, and no trust the pose did
not already have.

`EntityData::remoteRider` is what keeps the two apart: `rider` means occupied,
`remoteRider` means *the occupant is not the player on this machine*. Without it
`boatUpdate` steers from `ctx.input` and seats `ctx.player`, which are always
whoever is playing here — so a host whose guest took a boat was pulled into it
from wherever they were standing. There is a self-test for exactly that.

### The fast channel is unsequenced, and always was

`Channel::Fast` is `ENET_PACKET_FLAG_UNSEQUENCED`. Poses and snapshots therefore
arrive in arbitrary order, and a `Ghosts::Track` stamps every sample with its
*arrival* time — so a late packet is not ignored, it is believed. Both ends now
carry a counter and use `net::newerSeq` to discard anything already passed. Any
new message on that channel needs the same treatment or it inherits the same bug.

---

## Structures, and what the first one settled

Dungeons landed at `kGenVersion = 5`. Three things about that are worth keeping.

**A structure is an object, and fields do not describe objects.** Caves and
ravines are thresholds on noise: continuous across a chunk border because both
sides ask the same question at the same coordinate, with nothing anywhere that
knows a cave exists. A dungeon has a centre, a room count and a chest in a
particular corner. The pattern that works for it is the one trees already used —
the **margin scan**: every chunk independently re-derives anything whose footprint
could reach it, stamps the whole thing, and discards the writes outside its own
bounds. No chunk writes to another; no chunk depends on another.

What changed is *what* gets scanned. A tree is one column, so trees scan columns.
Scanning columns for something thirty blocks wide would be thousands of hashes a
chunk, so dungeons sit on a **96-block lattice** and a chunk tests the handful of
cells that could reach it. The jitter and footprint are deliberately sized so a
dungeon is always contained inside its own lattice cell (16 + 48 + 31 < 96) —
widen the footprint past that and chunks on the far side stop seeing the half that
reaches them.

**Nothing needed storing.** `generate` writes voxels and nothing else, and
`GenResult` carries only a `ChunkData`, so there is nowhere to leave a note saying
"chest here". There did not need to be: placement is a pure function of the seed
and the coordinate, so `dungeonChestsIn` simply asks again as the chunk lands.
Re-derivation beats a side channel whenever the thing is deterministic — it costs
a few hashes and cannot go stale.

**Chunks regenerate, so anything that fills a container must be idempotent.**
`unloadFar` erases chunks outright and walking back regenerates them, which fires
the loot sink again for a chest the player emptied an hour ago. What stops a
refill is that `blockEntities_` is *not* pruned on unload and is saved with the
world — so an existing entity is a reliable record that this chest has been met.
Guests fill nothing at all; contents are the host's, and arrive on request.

### The loot tables are keyed by name, on purpose

Recipes are addressed by index and blocks by registration order, and both carry
warnings about what that costs — inserting a recipe renumbers a hundred and
nineteen others and the golden diff becomes unreadable. Both are stuck with it: a
block id is in every save, and a recipe's position is its match priority.

A loot table has neither excuse, so `LootBook` is a map from name to table and the
dump is sorted by name. A new table can go anywhere in the list. **Prefer this
whenever a new registry appears** — index-keying is a cost to be justified, not a
default.

### Bumping kGenVersion is safe, and the gate proves it rather than promising it

Every `ver` comparison in the generator is `>=` or `<`, never `==`, so `if (ver >=
5)` is invisible to a v4 world. When 5 landed, the golden diff was **49 new, 0
changed, 0 missing**, all 49 carrying v5 — which is what makes moving an existing
world forward safe enough to offer in the menu at all.

Note the layered guard: the version is checked at the call site, in
`stampDungeons`, and in `dungeonPlan`. Sabotaging any one of them changes nothing,
which is good defence and a trap when verifying — a sabotage that "passes" may
simply not have taken effect. Check that a sabotage actually altered behaviour
before concluding a check is vacuous.

## The settings gate, and where a rule belongs

Two small additions to `ui/settings.h` carry the whole of the cheats and debug
work, and both are worth knowing about before adding anything near them.

**`SettingDef::gate`** names a flag that must be on before the row exists at all —
hidden, uneditable, absent. One switch therefore reveals a family, and a category
whose every row is gated off is not an empty tab but no tab. (`requires` would have
read better and is a C++20 keyword.)

A gate may name a **virtual flag**, which is a fact about the session that no row
can set: `worldIsCreative` is written by App from the save and is the entire reason
a survival world can never become a creative one. If it were a row, the host could
flip it, and the choice made when the world was created would mean nothing.

**Where a rule lives is a real decision, not a formality.** The split that came out
of this one is worth reusing:

- *Permission* is the world's, and therefore world-scoped, replicated, and the
  host's to set — `debugTools`, `creativeMode`, `noHealth`, `noClip`.
- *Preference* is the player's, and therefore Global and never sent — which
  overlay you like looking at follows you between worlds and is nobody else's
  business. It also keeps them out of `WorldSettingsMsg`, which caps at 64 pairs.

`SettingType::Action` is a row that stores nothing and fires the change callback.
"Locate the nearest dungeon" is something you do once, and the schema had no way
to say that.

### A note on PlayerOptions

Every rule about the local body belongs on `PlayerOptions`, and every consumer must
be handed the real one. `mobs.cpp` used to build its own carrying only the armour
figure, which was harmless while every field it dropped was cosmetic and stopped
being harmless the moment one of them was `invulnerable` — a zombie was the last
thing in the world that could kill a player nothing else could touch. If a hook
needs the options, give it `ctx.playerOptions`; do not construct one.

## Commands, and what the dedicated server will need from them

`src/cmd/` is deliberately a module with no UI and no network in it. That is not
tidiness: the dedicated server has no window and no settings screen, so **every
operational act has to be expressible as a line of text or it cannot be done at
all**, and a command layer that reached into the interface could not be reused by
a process that has none.

Three shapes to keep when extending it:

**Everything a command may touch is named.** `cmd::Context` holds the five live
objects and a `cmd::Hooks` for the side effects — the same argument as
`net/session.h`. "What a command can reach" should be a list you can read rather
than a property of what happened to be in scope.

**Anything affecting somebody else goes through a hook.** The host does not
simulate a guest's body, and in single player there is nobody else to affect; a
hook is the only shape honest in both cases. `App::makeCommandHooks` is where the
"is this me, or is this somebody over the wire" branch lives, once, rather than
in twenty command bodies.

**`Command::level` is a floor, not the whole answer.** `/set` is `Anyone` because
your own field of view is your own business, and demands `Operator` once the
setting turns out to be world-scoped. The floor is what the completion popup
filters on, so a command with an argument-dependent bar is offered and then
argues.

### What is already server-shaped

- `data/access.json` (`cmd/access.h`) is per **installation**, not per world,
  because being an operator is a fact about a person. A server that swaps worlds
  keeps its operators. One table with three flags rather than Minecraft's three
  files, so "who is Ada" has one answer.
- `SessionHooks::mayJoin` is the handshake gate. `Host` owns no list of people and
  asks; a dedicated server answers the same question from the same file.
- `Context::console` marks a line with no body behind it. It is what should make
  `~` refuse rather than silently mean the world origin — today only `--command`
  sets it, and the check that actually fires is `Participant::hasPos`.
- `/stop` sets `pendingStop_` rather than closing the world in place. It is
  reached from inside the host's own message loop, which is walking a peer list
  that closing the world would empty.

### What it still needs

- A console reader on stdin, feeding `cmd::run` with `console = true` and
  `Level::Owner`. Nothing else in the layer needs to change for it.
- A headless main loop: no window, no renderer, no interface. `App` currently
  owns all three, so this is where the split has to happen.
- The permission table is broadcast as `MsgType::Permission` and held by App in
  `netLevels_`. A server would hold the authoritative copy in `Access` and never
  need the map.

## The interface, and the two palettes that were not chosen

The UI was redesigned on 2026-08-11. The part worth recording is not what it
looks like but the shape underneath it, because that shape is what a resource
pack gets to reach.

**Tokens are two-tier, and that is the whole design.** A small *palette* — the
dozen-odd colours a theme actually decides — and ~70 *roles* derived from it in
C++. A pack that overrides the palette recolours the entire interface
coherently, which is what makes a ten-line theme file possible; a pack that
wants one specific widget different overrides that one role. The alternative
was a flat table of 70 names, which sounds simpler and means every pack author
has to get 70 colours to agree with each other by hand.

Names are for authors, indices are for frames. A token is written
`"panel.bg"` in JSON and resolved **once at load** into a dense array, so
drawing costs an array index rather than a hash lookup. The enum and the name
table come from one X-macro for the single reason that two hand-maintained
lists of 80 names will drift, and the drift is silent — a role nobody can
address any more still compiles and still draws.

**The palette that was chosen is Lantern**: warm amber on a near-black warm
brown-grey, the interface as seen by lamplight underground. It has one known
cost, and it was known before it was chosen — lantern amber sits close to both
the health red and the hunger amber of the HUD pips, which are the two things on
screen that must stay readable at a glance. Those pips had to be reworked rather
than left to collide. If a future change moves the accent, check them again.

### Two things deliberately NOT built, and why

**A separate skin / StyleRecipe layer.** The original plan had a third tier
between the tokens and the widget builders: a table of recipes per widget per
state, so a pack could restyle "every primary button". It was dropped once the
token table existed, because the tokens are already role-per-widget-per-state —
`button.primary.fill.hover` is exactly what a recipe row would have held. A skin
table would have been a second indirection over the same data with no capability
the first one lacked. If it comes back, it should come back for a reason the
tokens genuinely cannot express, not for tidiness.

**Free-form anchored nodes in the layout engine.** The plan was to give `Doc`
absolute positioning so the HUD, toasts and chat could stop being hand-positioned
floats and a pack could move them. The HUD was redesigned by hand instead, which
means there is currently **no consumer** for such a feature — and building layout
infrastructure against a guessed use rather than a real one is how engines end up
with two layout systems that disagree.

Much of what it was for is already covered: `hotbar.slot`, `hotbar.bottom`,
`hotbar.gap`, `pip.size`, `pip.gap`, `minimap.size`, `minimap.inset`, `toast.top`
and `toast.right` are all theme scalars, so a pack can already resize and shift
most of the HUD. What is missing is moving a widget to a *different corner*, and
that is the thing to build anchors for when somebody actually wants it.

### Two candidate UI Style Packs

Both were designed alongside Lantern on 2026-08-11 and both were liked; they
lost on one choice, not on merit. They are recorded here in full so that
building either as a resource pack is a matter of transcribing a palette rather
than re-deriving one. Each is a **palette override only** — no role overrides,
no sprites — which is also the honest test of whether the two-tier design works:
if either needs more than its palette to look right, the derivations are wrong.

**Verdant Reforged** — what the pre-redesign interface was reaching for. The old
UI paired a pastel green accent with blue-grey panels, and the ground fighting
the accent is exactly why it read as washed out. This keeps the green identity
and gives it a green ground to sit on.

| token | value | |
|---|---|---|
| `bg` | `#0f1411` | dark green-black |
| `panel` | `#18211b` | |
| `panel2` | `#222e26` | |
| `edge` | `#070a08` | |
| `accent` | `#6ee787` | saturated, not pastel |
| `accent.dark` | `#2f7a45` | |
| `text` | `#e9f2ea` | |
| `muted` | `#93a598` | |
| `danger` | `#e0604e` | |

**Slate & Bone** — near-monochrome warm greys with a bone off-white accent, on
one rule: **colour is reserved for meaning**. Nothing in the chrome is saturated,
so item icons, ore glows, rarity tints and the health and hunger pips become the
only coloured things on screen and need no help to stand out. It is the quietest
of the three and the one that ages best, and it is the natural pairing for anyone
who finds Lantern too warm to read for long sessions.

| token | value | |
|---|---|---|
| `bg` | `#15161a` | |
| `panel` | `#20222a` | |
| `panel2` | `#2b2e38` | |
| `edge` | `#0a0b0e` | |
| `accent` | `#d8d2c4` | bone |
| `accent.dark` | `#8d8778` | |
| `text` | `#eceae4` | |
| `muted` | `#9a978e` | |
| `danger` | `#d9584a` | the only saturated chrome colour |

A third direction was considered and dropped before it was offered: **Aetherite**,
a cyan-teal accent taken from the ore's own texture hex on deep indigo-black. It
is worth remembering only because it is the palette whose accent sits *furthest*
from health red and hunger amber, so it is the one to reach for if the pip
readability problem above ever proves unfixable rather than merely awkward.

## Farming, and the two indexes that cost no save format

Three decisions here are worth keeping, because each one replaced something that
looked obvious and was wrong.

**Growth stage lives in cell metadata, not in a block per stage.** Eighteen crops at
four stages is seventy-two block ids, and because every block automatically becomes
a placeable item it would also have been seventy-two items. `shapes.h` documents
Cross metadata as 0 standing / 1-4 wall-mounted, read as `meta & 7`, and a plant is
always 0 — so the upper five bits were already free on exactly the blocks a crop is.
`World::setMeta` writes metadata into the persisted edit map, so growth survives a
save, a chunk unload and a regeneration **without one byte of new save format**.

**The planted-crop index is derived, not saved.** `CropSim` needs to know where the
crops are, and `blockupdate.h` is right that walking the world to find them is dead
on arrival. But the set does not need storing either: a crop is always a player
edit, and every player edit is in `edits_` — so `World::indexCrops()` rebuilds the
index from the edit map on load, and `setEdits` calls it rather than trusting five
call sites to remember. A derived index cannot go stale against the thing it is
derived from, which is the same argument that made dungeon chests re-derived.

The corollary is the good half of the design: **wild stands are not in the index**,
because they are generator output rather than edits. So the sweep never spends a
roll on scenery — and that is precisely why worldgen has to stamp a wild stand at
its ripe stage. A stand that generated unripe would stay unripe for the life of the
world, because nothing would ever visit it.

**Growth has no timer anywhere.** Each sweep, each crop rolls against a chance.
Minecraft's model, and it means nothing is stored per crop, nothing drifts while the
game is paused, and no save has to describe a half-grown plant.

### Three bugs this shook out, all of them the same shape

Each was a case of a new branch being placed after an existing early return, and
none of them would have failed a compile or a test — they simply made content
unreachable.

- The crop roll in `pickFoliage` originally sat **after** the `sand` and `snowturf`
  branches, both of which `return` outright. The desert list (melon, chili, maize)
  and the snow list were painted, registered, and could not be found by anybody.
- Then it keyed on the ground being sand rather than the biome being Desert, so
  every beach on the map sprouted grapes and tomatoes.
- `--find-crop` exists because of these. A wild stand is far too sparse to find by
  flying around and looking, so "is this generating at all" had no answer. It
  mirrors `--find-dungeon` and reports the densest 16x16 region rather than the
  nearest cell, because the nearest cell is always a lone straggler and frames a
  screenshot on one plant.

### Then nine more of the same shape, and the gate that now catches them

The three above were found by looking. Nine were not, and shipped in the 2.14.0
draft — the Stove could not be crafted at all, which is how this was reported.

Every one was **authored content that no player could reach**, and every one looked
perfectly correct in the table it was written in. They came in four mechanisms:

- **Two identical crafting patterns, the earlier one winning.** `matchGrid` returns
  the first recipe a grid satisfies. The Stove was a ring of cobbled and so was the
  forge; the Bowl was a hull of planks and so was the boat. The bowl one was worse
  than the reported bug: every pot meal needs a bowl, so the entire Cooking Pot was
  unusable and nobody had noticed.
- **A recipe wanting more ingredients than its station has slots.** The stove had one
  input and five of its thirteen recipes name two or three different things. Not a
  matching bug at all, and invisible unless you count a recipe's keys against the
  station's capacity.
- **A general tag recipe tying with a specific one.** `matchCooking` ranked by total
  ingredient count, so "any three vegetables" tied with "two pumpkin and a garlic",
  and ties keep table order. Pumpkin Soup, Tomato Soup and Garden Salad existed in
  the table, in the handbook, and nowhere else. Demand now weighs **specificity above
  quantity** — a concrete key counts double a tag.
- **A validator that was right when it was written.** `BeRequestMsg::decode` and
  `BeStateMsg::decode` both ended in `m.kind <= 2`, correct when Forge and Chest were
  the only kinds. The three kitchens are 3, 4 and 5, so **every network message about
  a kitchen was refused and dropped in silence**: a guest's station opened empty, ate
  whatever was put in it, and showed a cook that never advanced. There was no error
  anywhere, and the check that should have caught it asserted no *denial* had arrived
  — which a dropped message also satisfies. `kMaxBlockEntityKind` now holds the bound
  and a `static_assert` in host.cpp ties it to the enum.

**`testRecipesReachable` and `testCookingReachable` are the general form.** They lay
out every recipe's own ingredients and demand the matcher hand back *that* recipe, by
pointer — comparing output keys lets a shadowed duplicate pass under its twin's name.
Between them they found all nine, including the five nobody had reported.

The reason no earlier test caught any of this: **every recipe test asserts a recipe it
names**, and an unreachable recipe is exactly the one nobody thought to name. A test
per recipe would not have been written; a test over the whole table costs forty lines.
Reach for that shape whenever a registry is append-only and matching is ordered.

### A note on fuzzy completion, and content pressure

`/give sto` no longer offers `greystone`. Nothing broke: `fuzzyScore` pays +14 for a
match on a word boundary and +2 for one buried mid-word, so every `<tool>_stone`
outranks `greystone` outright, and the six hoes pushed it past the ten-row cap. The
crowding was always there and one more tool tipped it over.

Worth knowing because it will happen again: **every tool family added is ten more
rows competing for a ten-row list**, and the base material always loses. If it
becomes a real annoyance the fix is in the scorer — prefer a candidate with fewer
underscores when the query has none — not in the content.

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
