> [!NOTE]
> - This project was built with 99% AI assistance (Claude Opus & Fable) under human oversight. 🤖
> - Expect there to be bugs and balancing issues

# Hollowreach — a vibecoded voxel sandbox

A 3D first-person Minecraft-like, built from scratch in C++: its **own OpenGL
voxel engine, procedural world with real biomes, procedural textures and item
models, deferred lighting pipeline, synthesised sound engine, mob AI, an
in-game world atlas, and host-authoritative multiplayer**. Mine, smelt, craft,
tier up tools/armour, build, chart the map, survive the night, and play with a
friend over the LAN, all in **persistent, shareable worlds**.

It is a native application: one executable, no runtime, no installer, and
nothing that phones home.

> [!NOTE]
> **This repository is a fork.** Hollowreach began life as a dependency-free
> WebGL2 game in ~18k lines of ES modules, and this is a full C++ port of it —
> the same renderer, the same world generator, the same gameplay constants,
> rewritten. The original is archived at
> [Anonymous-Floof/Hollow-Reach](https://github.com/Anonymous-Floof/Hollow-Reach)
> and its history is the first seventeen commits here. The port is verified
> *against* that code: `tools/compare_golden.py` runs both implementations and
> diffs 610 values, so the terrain a seed produces here is the terrain it
> produced in the browser.

<table>
<tr>
<td width="50%"><img src="screenshots/dawn.png" width="100%" alt="Sunrise over the sea, with dawn fog on the water and layered cloud"><br><sub>Dawn over the sea — the sun low through layered cloud, with the morning fog that burns off as it climbs.</sub></td>
<td width="50%"><img src="screenshots/shore.png" width="100%" alt="A papyrus shoreline reflected in still water under a bright sky"><br><sub>Real-time water reflections mirror the shoreline, the papyrus and the sky.</sub></td>
</tr>
<tr>
<td><img src="screenshots/forest.png" width="100%" alt="A forested landscape seen from the air, with a ravine and a distant beach"><br><sub>Biomes run to the horizon: forest, meadow, beach and a ravine cutting through.</sub></td>
<td><img src="screenshots/ravine.png" width="100%" alt="A deep ravine splitting a forest, under volumetric clouds"><br><sub>Ravines crack the surface open — and volumetric clouds cast moving shadows on the ground.</sub></td>
</tr>
<tr>
<td><img src="screenshots/night.png" width="100%" alt="A moonlit palm shoreline at night with drifting cloud"><br><sub>Moonlight over a palm shore, with drifting cloud and a sky full of stars.</sub></td>
<td><img src="screenshots/grassland.png" width="100%" alt="Grass terraces and trees beside a beach, with a cave mouth in the hillside"><br><sub>A cave mouth in the hillside. They widen into proper caverns the deeper you go.</sub></td>
</tr>
<tr>
<td><img src="screenshots/inventory.png" width="100%" alt="The inventory screen over the world, with item icons and a held pickaxe"><br><sub>Every icon and held model is generated at runtime — no image files ship with the game.</sub></td>
<td><img src="screenshots/menu.png" width="100%" alt="The Hollowreach main menu"><br><sub>The interface is its own 2D layer: no toolkit, no dependency, drawn in one batched pass.</sub></td>
</tr>
</table>

## Running it

Download a release, unzip it anywhere, and run **`Hollowreach.exe`**. There is
nothing to install: the assets are baked into the executable and everything
links statically apart from your graphics driver. The game creates a `data/`
folder beside itself on first run and keeps your worlds, screenshots, exports
and settings there, so the whole thing stays portable — move the folder to a
USB stick and it moves with you.

Requirements: a GPU and driver supporting **OpenGL 3.3** (anything from about
2010 onward). Nothing else.

### Updating

**Check for Updates** on the main menu fetches the latest public release and
installs it over your copy. It takes three clicks and never fewer: the first
asks GitHub what the latest version is, the second downloads it, the third
installs and restarts the game. Nothing happens on its own — the game does not
check on startup, does not check in the background, and does not install
anything you have not seen the version number of first.

It only ever adds and overwrites the files that were in the release zip.
`data/`, resource packs, and anything else you have put beside the executable
are left exactly as they were — not by an exclusion list that somebody has to
remember to update, but because nothing in the process deletes anything.

Windows only for now, since the release zip is per-platform and so is replacing
a running executable; the button is not shown elsewhere. There is also
`--check-update` if you would rather ask from a terminal.

### Building it

One command, from a clean checkout, with no environment set up by hand:

- **Windows** — **`build.bat`** finds Visual Studio through `vswhere`, sets up
  the MSVC environment, and uses the CMake and Ninja that ship inside VS. You
  need **Visual Studio 2022** with the C++ workload; you do not need CMake or
  Ninja on `PATH`.
- **Linux / macOS** — **`./build.sh`**, with CMake ≥ 3.21, Ninja and a C++20
  compiler.

The build fetches GLFW and ENet itself; everything else is vendored in
`third_party/`. The result is `build/RelWithDebInfo/bin/Hollowreach`.

`Hollowreach --help` lists the harness flags the port was verified with —
`--screenshot`, `--at`, `--time`, `--seed`, `--screen`, `--threads`,
`--selftest` and the rest. `--selftest` runs 385 assertions with no window at
all and is the fastest way to know a change did not break something.

## Controls

| | |
|---|---|
| Move | WASD |
| Look | Mouse |
| Jump / swim up | Space |
| Sprint (1.3×) | Hold Left Ctrl while moving |
| Fly (toggle) | Double-tap Space |
| Sneak (slow) | Left Shift |
| Break block / attack | Left mouse (hold) |
| Place / use station / open door / sleep / **eat** | Right mouse |
| Pick block (bring one you own to hand) | Middle mouse |
| Choose a painting's picture | Right mouse on a hung painting |
| Drop item | Q (one) · Ctrl+Q (whole stack) |
| Climb ladder | Walk into it + W/Space (Shift = down) |
| Select hotbar | 1–9 / scroll |
| Inventory | E |
| Move a slot to the hotbar | Hover it and press 1–9 (swaps) |
| Move many at once | Hold Shift and drag across slots |
| Drop many at once | Hold Q and drag across slots |
| Recipe book | H (or the **Recipes** button on any crafting screen) |
| Atlas map (needs an **Atlas**) | M |
| Screenshot | F2 |
| Hide the interface | F1 |
| Fullscreen | Alt+Enter |
| Pause | Esc |
| Debug overlay | F3 |

**Settings** (pause or main menu) are grouped into Graphics / Controls /
Gameplay / Audio tabs — render distance and quality preset,
shadow/reflection/cloud/AO toggles, render resolution, fullscreen, borderless
window, V-Sync, a frame rate cap, mouse sensitivity and raw-input, interface
scale, fall-damage/hunger/monster/flight toggles, and volume sliders.
Every one applies live, with no restart. The in-game **About** screen (main
menu) has a quick feature rundown if you want the highlight reel instead of
reading this whole file, and **Quit** is at the bottom of that same menu — it
saves the open world on the way out.

**Screenshots (F2):** captures land in an in-game gallery (Gallery button on
the main or pause menu) where you can view them, delete them, or reveal them in
your file manager. They are ordinary PNGs in `data/screenshots/`.

**Performance.** Milliseconds per frame at 1920×1080 on a Radeon RX 5700 XT:

| Render distance | Low | High | Ultra |
|---|---|---|---|
| 4  | 1.11 (901 fps) | 2.88 (347 fps) | 3.61 (277 fps) |
| 8  | 1.48 (676 fps) | 3.33 (301 fps) | 4.02 (249 fps) |
| 12 | 2.20 (455 fps) | 3.98 (251 fps) | 4.71 (212 fps) |

Every cell comes from one command, so you can reproduce or dispute it:

```
Hollowreach --world --seed 3918175327 --at 0,0,0,0,0 --freeze --time 0.5 \
  --quality ultra --render-distance 12 --no-vsync --perf --frames 2500
```

A fixed camera and a pinned midday sun, because both matter more than the
settings do — how much sky is on screen sets what the cloud march costs, and
that pass is about half the frame. An earlier version of this table quoted
numbers a third lower with no camera attached to them, which made it impossible
to tell a real change from a different place to stand.

Repeated runs settle within about 5%, with the occasional much faster outlier
when the GPU happens to be clocked up for the whole window. Do not read a
difference that size between releases as a change to the renderer; check the
per-pass breakdown before believing one.

Neither lever costs much. Render distance is nearly free past 8,
because fog bounds what is actually visible long before the loaded radius does,
and the whole spread from Low to Ultra at distance 12 is about 2.6 ms. If you do
need to claw some back, **Render Resolution** is the biggest single control —
it draws the world at a fraction of the window and scales it up, leaving the
interface sharp — and turning individual effects off (God Rays, Water
Reflections, Cast Shadows, Volumetric Clouds) removes a whole pass each.

If you want to know where a frame actually goes, `--perf` prints a per-pass GPU
and CPU breakdown while you play and a summary when you quit.

**Bed** — craft from 3 planks + 3 wool (from a sheep), place it (it lays out two cells, pillow
always at the head whichever way you face), then right-click at night to
fast-forward to morning. Sleeping advances the actual game clock, so
time-of-day mechanics (like grass spreading) move forward while you sleep. **Boat** —
craft from 5 planks, right-click to set it on water (or ground); right-click it
to ride (look where you want to go, W/S throttle, A/D strafe), **Shift** to
dismount, and left-click an empty boat to pick it back up.

**Painting** — craft from 8 planks around 1 wool and 1 azurite, hang it on any
wall, then right-click it and choose one of your own screenshots to display.
The picture is stored *in the painting*, not as a link to the file: it survives
deleting the screenshot, it travels with an exported world, and a friend who
joins your game sees what you hung even though they have never seen your
captures folder. It is scaled to 128 square on the way in, which is 48 KB per
painting and sharper than anything else in the world.

**Inventory (Mouse-Tweaks style):** Left-click picks up / places a stack,
right-click takes half / places one. **Shift-click** instantly moves a stack to
the other container (inventory ↔ chest/forge, hotbar ↔ storage) — and **hold
shift and drag** across slots to move every one you touch, which empties a
rucksack into a chest in a single gesture. **Q** over a slot throws that stack on
the floor, and **holding Q while dragging** throws all of them. Hold **left** and
drag across slots to split a held stack evenly; hold **right** and drag to drop
one per slot. **Scroll** on a slot to nudge single items across. Hover any item
for its name and stats.

**Biomes:** the overworld is split by temperature and moisture into
**meadows**, dense **forests**, pale **birch groves**, sandy **deserts** and
**snowfields**, with **palm trees** on warm beaches and **papyrus** reeds
growing along shorelines. Mountain ranges rise well above the old hills,
**ravines** crack the surface open, caves widen into proper caverns the deeper
you go, and the deepest passages are **flooded** — bring a way back up.

**Buckets & water:** craft a **bucket** from three iron ingots, right-click
still water to scoop a source, and right-click again to pour it back out
anywhere — the placed water flows for real, so you can move springs, fill
moats, or carve waterfalls. An empty bucket used on a **cow** fills with milk.

**The Atlas:** craft it from **3 paper + 1 leather + 1 azurite** (paper comes
from shoreline **papyrus**) and carry it to unlock cartography. **M** opens a
fullscreen top-down map — every block is one pixel in its true colour, with
relief shading and depth-tinted water — that only charts ground you've
actually explored (the rest stays fogged). Click to drop **waypoints** (rename,
recolour or delete them in the side panel); they show as floating markers in
the world with live distances. Dying pins an automatic **death waypoint** where
you fell (toggle in Settings), and a corner **minimap** (toggle in Settings)
keeps your surroundings and waypoint headings in view. Lose the Atlas — say, by
dying with it — and the map goes with it until you get it back.

**Soul Anchor & Wayshard:** the **Soul Anchor** — one of every ore ringing a
sparkstone — is a placeable block; right-click it to attune your **spawn
point**, and you'll wake there when you die (breaking it unbinds you). The
**Wayshard** (gloamite + sparkstone) is a one-use escape rope: use it deep
underground and it warps you straight up to the surface. Two new deep ores
power them: violet, faintly glowing **Gloamite** and mossy **Verdanite**.

In water you swim: you sink slowly, hold **Space** to rise (and swim into a
shore to climb out), or **Left Shift** to dive. Soft blocks (grass, dirt, wood,
sand) drop when mined by hand — only stone, ores and other hard blocks require
the right tool tier — and a tool only mines faster for the block class it's
*meant* for. Chop all of a tree's logs and its leaves decay on their own.
**Grass creeps**: exposed dirt that's lit and next to grass slowly turns to
grass over in-game days (so a dug-out patch heals over, and beds let you watch it).

**Animals:** wild **sheep**, **pigs** and **cows** wander the grass in
daylight. Left-click to attack (a sword hits hardest, and striking **while
falling lands a critical hit** for 1.5× damage). A sheep drops a block of
**white wool** (which, with planks, crafts a **bed**); a pig drops a **Raw
Porkchop**; a cow drops **Raw Beef** and sometimes **Leather** — cook the meats
in the forge for much better food, and right-click a cow with an empty bucket
to **milk** it. All of them climb hills and steer clear of water, so they stay
on dry land instead of drowning.

**Monsters:** **zombies** rise on solid ground after dark. They need genuine
line of sight to notice you — no seeing through walls — and once they spot you
they path around obstacles to reach you, remembering roughly where you last
were for a few seconds if you break their sight, clawing for damage when they
close in (armour softens the blow). They burn away in direct sunlight, so
they're a night-time threat — hole up or fight back. A slain zombie drops
**rotten flesh** (edible, but a gamble — it might feed you a point or sicken
you for two). Like the animals they climb 1-block ledges and won't wade into
the sea. (Turn them off in Settings.)

**Survival:** a **hunger** bar (next to your hearts) slowly drains as you live and
act — sprinting and swimming burn it faster. Eat to refill it; when it empties you
**starve** and lose health until you eat. You only regenerate health while
well-fed. Hold your breath underwater: a row of **bubbles** counts down once your
head is submerged, and when they run out you start to **drown**. (Hunger can be
toggled off in Settings; breath/drowning is always on.) Taking a hit now also
**wears your armour** — it soaks damage and loses durability for it — and
flashes a **red vignette** around the edge of the screen, so a hit you did not
see coming does not go unnoticed while you are looking somewhere else. Run out
of hearts and you **die**: what you were carrying drops where you fell, a death
waypoint is pinned on the Atlas, and you wake at your Soul Anchor.

**Things hold each other up.** Torches, plants, mushrooms and pebbles need solid
ground beneath them, and break into their drop when it goes — mine the dirt under
a torch and the torch comes with it. **Sand falls**: dig it out from underneath
and it drops as a real falling block, stacking up wherever it lands, which is
what makes a dune collapse when you tunnel through it. And water **washes away**
what it flows into: a flooded torch or tuft of grass is gone, though water
sitting quietly beside a shoreline leaves it alone. All of it is event-driven —
nothing is checked until something near it changes — so a world holding tens of
thousands of plants costs nothing until you disturb one.

**Atmosphere:** the world breathes a little. A real **sun** and **moon** arc
across the sky (the sun rises in the east), the night fills with a sparse,
twinkling **star field**, **volumetric clouds** drift overhead and cast moving
shadows on the ground, and **dawn rolls in thick fog** that burns off as the
morning brightens. The sun **casts real shadows**, **water reflects** its
surroundings and gives a soft underwater view when you're submerged, and
ambient occlusion + god-rays add depth in caves and under canopies (all
toggleable in Settings if you'd rather have the frame rate back). **Leaves
sway** and the **water surface ripples** with a gentle noisy motion, the camera
does a soft **head-bob** as you walk (more when you sprint) with your held item
swaying along, and mobs — and other players, in multiplayer — walk with real
**limb animation** instead of sliding. All of it is driven on the GPU so it
costs almost nothing.

**Building blocks:** **stairs**, **slabs** and **vertical slabs** can be cut
from *any* wood, sandstone or stone — including the polished and brick
sub-variants — so every material has a matching step and half-block. Stairs and
slabs read where you aim: click a block's top for a bottom slab / right-way-up
stair, its underside (or the upper half of a side) for a **top slab / upside-down
stair**. A slab crafts into a **vertical slab** (and back) for thin walls. Each
**wood type makes its own doors, trapdoors, stairs and slabs**, and any wood's
planks work for sticks, the workbench, chests, beds and boats (even mixed).
Ladders, trapdoors and doors place facing you; doors and trapdoors toggle on
right-click (and politely refuse to close on anyone standing in the frame).
Beyond Stone and Oak there are two more stone families (**Umberstone**,
**Slatestone**, each with polished + brick forms) and four more woods
(**Pine**, **Walnut**, **Birch**, **Palm**) that generate naturally — stone in
underground blobs, the woods as their own trees in their own biomes. **Torches** angle correctly when set on a
wall, and show as a flat sprite in hand and when dropped. **Chests** store 27
stacks; **forges keep smelting with the UI closed** and both keep their contents
until you mine them. Out of Coal? **Smelt logs into Charcoal** — it burns and
crafts torches just like Coal. **Anything wooden burns as forge fuel** — logs,
planks (any wood type), wooden tools, chests, boats, even torches — and the burn
time is read straight from the item's recipe, so it scales with how much wood
went in (no per-item bookkeeping).

**Recipe book (press H):** categorised tabs (Building / Tools / Armour /
Materials / Smelting), a fuzzy **search** box, and grouped cards — near-identical
recipes (every stairs material, every pickaxe tier, a torch's two fuels) collapse
into one card you cycle with the **‹ ›** arrows. Hover any ingredient or result
for the same detailed tooltip the inventory shows (tool tier, mining speed,
durability, armour defense, fuel time).

**Click a recipe** and it lays itself out in the crafting grid you opened the
book from, taking the ingredients out of your bag — so you never have to
remember a shape. It is all or nothing: if you are short of something the grid
is left exactly as it was, rather than half-filled and looking ready. A tag
ingredient ("any planks") picks the wood you have *least* of, which spends the
odd single birch plank before it opens the stack of sixty oak. Clicking a second
recipe returns the first to your bag rather than refusing, and the variant on
show is the one you get — so cycle to the tier or the wood you want first.

## The gameplay loop

1. Punch **Oak** trees → logs → **planks** → **sticks** → a **Workbench**.
2. Mine stone with a wood pick for **Cobblestone** → build a **Forge**.
3. Mine ores; smelt **Raw Copper / Iron / Gold** into ingots at the Forge
   (fuel: Coal, logs, planks).
4. Climb tool & armour tiers: Wooden → Stone → Copper → Iron → Gold (fast,
   fragile) → **Diamond**. Each tier unlocks the next ore (e.g. only an Iron+
   pick harvests Diamond).
5. Craft **Torches** to light caves; build with planks, bricks, polished stone,
   sandstone and glass.
6. Gather papyrus and leather for an **Atlas**, mine every ore for a **Soul
   Anchor** to move your spawn, and keep a **Wayshard** in your pocket for the
   trip back up.

Worlds are saved as `.hrw` files in **`data/worlds/`** beside the executable —
a compact binary format: a checksummed header over a list of tag-dispatched
sections, so a new section costs no version bump and an unknown one is skipped
rather than fatal. Writes are atomic (written to a temporary file and renamed),
so losing power mid-save costs the save and not the world. A truncated, corrupt
or hostile file is rejected with a reason instead of loading as nonsense, and
`Hollowreach --save-info <id>` will tell you which reason.

There is an autosave every fifteen minutes and another on a clean shutdown, so
closing the window does not cost you the session. To share a world, **Pause →
Export World** writes it to `data/exports/`, and the world-select screen imports
anything sitting in that same folder — one folder is both outbox and inbox,
which is a smaller cost than a native file dialog for two buttons.
`--export-world` and `--import-world` take real paths for anything scripted.

## Multiplayer

Up to eight people on the same network can play together, over UDP, with no
server to run and nothing to sign up for.

- **Host:** Pause → **Open to LAN**. The button then reads *Stop Hosting* and
  shows an invite code and how many guests are connected.
- **Join:** from the main menu, pick **Join a Friend**. Games on your network
  announce themselves and appear in the list, so usually there is nothing to
  type at all — click one. The field below takes an invite code or a plain
  `address` / `address:port` if you would rather.

The invite code is the same `HRW1…HRW1` envelope the browser build used, so it
still survives being lowercased, broken up by a chat client, or read out loud —
it is Crockford base32, with the letters that look like digits left out.

**Known limit:** this is LAN-first. The browser build got NAT traversal for free
from WebRTC; UDP does not, so playing with someone outside your network needs a
forwarded port (default **25565/udp**) on the host's router. The transport layer
has an explicit seam for a relay or hole-punch backend if that ever changes.

The host's world is authoritative — they simulate mobs, water, forges and
time; guests generate the same terrain locally from the shared seed and stay
in sync via live edits and periodic snapshots. Your own movement, mining,
building, crafting and inventory apply instantly on your end regardless of
ping; only seeing *someone else's* edits, combat, and container access wait on
the connection, so play stays responsive even at high latency. Guests can do
everything the host can: place, ride and break **boats** (riding is predicted
client-side, so it feels instant), **milk cows**, use **Wayshards**, land
falling **critical hits**, and attune a **Soul Anchor** — a guest's spawn
point, inventory and position are all saved inside the host's world and
restored if they reconnect. While connected as a guest, the world-list
"Export World" button becomes **Leave World** instead, since it's the host's
save, not yours.

Guests are untrusted, and the host says so with more than good manners: every
message is range-checked before it reaches the game, edits and combat are reach-
checked, movement is speed-checked, and each action has its own token bucket per
peer. A guest that fails a check gets a correction — a rollback or a teleport —
not a disconnect and never a crash.

Known limits (for now): both players need the same version of the game, and the
native build cannot play with the browser one — different transport, different
wire format.

## Architecture (built to be extended)

Roughly 40k lines of C++20 under `src/`, one directory per concern. The layout
deliberately mirrors the web build's `js/` folder for folder, so any file in the
archived repository maps to an obvious file here.

- `core/` — GL loading, shaders, matrix math (`mat4` ported rather than
  replaced with GLM, to keep the same conventions), seeded RNG, input, the
  worker pool (`jobs`), and `bytes` — a **fail-closed** binary reader that
  latches on the first read past the end, which is what lets both the save
  loader and the network protocol be checked once instead of field by field.
- `platform/` — the GLFW window, pointer capture, clipboard, and `paths`, which
  decides where `data/` lives.
- `resource/` — `identifier` + `packstack` (an ordered provider list), the
  procedural `painters`, and the `atlas` builder.
- `world/` — `blocks` (the master data table), `noise`, `worldgen`
  (biomes/terrain/caves/ravines/ores/trees — **versioned**, so old worlds keep
  generating as they did), `chunk`, `mesher`, `lighting`, `shapes`, `water`
  (the flowing-water automaton), `world` (chunk streaming).
- `render/` — `renderer` + `gbuffer` (deferred lighting: shadows, SSAO,
  god-rays, water reflections), `sky`, `chunkmesh`, `entityrenderer`,
  `itemmodel`/`itemmesh`/`viewmodel`, `iconatlas` (a CPU rasteriser for the
  isometric inventory icons).
- `game/` — `player`, `physics` (shared swept-AABB collision), `raycast`,
  `interact`, `items`, `inventory`, `recipes`, `crafting`, `blockentities`,
  and `entities/` (the framework: `registry`, `manager`, one def per kind, a
  shared movement brain in `ai`, and an `ai/` subfolder for pathing, perception
  and state machines).
- `net/` — `protocol` (binary messages, every number range-checked),
  `transport` (ENet, addressing peers by integer id rather than `ENetPeer*`,
  because ENet recycles that pointer), `host`/`client`, `ghosts` (150 ms
  interpolation), `discovery` (LAN beacons and the invite-code codec).
- `audio/` — `dsp` (band-limited oscillators, cookbook biquads, envelopes, a
  soft-knee compressor), `engine` (the bus graph on miniaudio), then `sfx`,
  `ambience` and `director`. Every generator and filter is ours: the recipes
  were tuned against Chrome's Web Audio nodes, so miniaudio is only a device
  abstraction.
- `ui/` — a custom immediate-mode 2D layer with two-pass layout, a font atlas
  and a tween store, then the screens: `menu`, `hud`, `inventoryui`,
  `recipebook`, `settingsui`, `notify`, `gallery`, `map` (the Atlas).
- `save/` — `format` (the binary encoder, deterministic so a round-trip test can
  diff every byte), `storage`, `migrate`, `transfer`, `gallery`.
- `dev/` — `selftest` and the golden-vector dumpers behind `--dump-golden`.

**Assets** (`assets/shaders/`) are baked into the executable by
`cmake/embed_assets.cmake`, with an on-disk override in Debug so a shader edit
hot-reloads without a rebuild.

**Entities:** a small, data-driven framework (`game/entities/`) mirroring the
block/item/recipe tables. An `EntityManager` owns instances; each dispatches
lifecycle hooks (`update`, `interact`, `load`) to its type definition, and
shares the player's collision (`game/physics.h`). Player and entities are both
just a `Body{pos, hw, h}`. The **item drop** was the first entity: mined blocks
and spilled container contents pop out as drops, vacuumed into your inventory
the moment there's room (so mining feels instant) and only lingering physically
when it's full. The **boat** is the first *rideable*: it floats on water via a
buoyancy spring, carries the player in its seat, steers toward your look
direction, and breaks back into an item. **Sheep, pigs, cows and zombies** use
the same hooks plus the shared `ai` brain for hill-climbing and water-avoidance;
the zombie additionally uses true line-of-sight and budgeted A* pathfinding
(`entities/senses`, `path`) instead of aggroing blindly through walls. In
multiplayer a sixth type, `remote_player`, mirrors other players as a
locally-simulated "ghost" driven by network snapshots instead of physics. Any
entity with a walk cycle animates through a small GPU bone system, driven purely
by how its position changes frame to frame, so it works identically for local
mobs and networked ones with zero extra sync.

Per-type state lives in a **flat struct** rather than a variant. That looks
wasteful next to a variant until you count what it buys: the tick loop touches
these fields every frame for every mob, the serializer can whitelist per type
with no reflection, and the whole thing stays trivially copyable — which is what
lets a network snapshot be taken cheaply.

**Threading:** generation, lighting and meshing all run on **one** pool with
three priority lanes, so a remesh burst cannot starve generation. There is **no
lock in the chunk pipeline**: chunk data is copy-on-write behind a
`shared_ptr`, and `use_count() > 1` is the entire test — only the main thread
hands a snapshot to a job, so the count can only ever fall behind our back and
cloning when we needn't is harmless. Staleness is the dirty flag and nothing
else: a job clears it at submit, anything that invalidates the chunk sets it
again, and a result returning to a dirty chunk is stale by definition.

An **unstarted pool runs every job inline on the calling thread**. That is not a
fallback — it is what makes `--threads 0` a one-flag switch, keeps every
headless tool working, and means the threaded and single-threaded builds run the
*same* pipeline rather than two that can drift. `--selftest` asserts that 0, 1,
4 and 8 workers produce byte-identical worlds.

### Common extension points

- **New block:** add one entry to `src/world/blocks.cpp` and a matching painter
  in `src/resource/painters.cpp`. Saves stay valid because blocks are stored by
  stable string key, not numeric id.
- **New recipe:** add a row to `src/game/recipes.cpp`. It appears in the in-game
  Recipe Book (press **R**) automatically — the book is generated from the data.
- **New entity** (mob, boat, projectile…): add a definition under
  `src/game/entities/` and register it in `registry.cpp` — give it a size, set
  `physics`, and implement the hooks you need. For perception or pathing, reach
  for `senses.h` / `path.h` rather than rolling your own.
- **New setting:** add a row to the schema in `src/ui/settings.cpp` — the
  settings screen and the persistence pick it up with no edit to either.
- **Save format change:** a *new section* costs nothing — pick a four-character
  tag and write it; unknown tags are skipped. A *changed* section layout bumps
  `kSaveVersion` in `src/save/format.h` and needs a migration in
  `src/save/migrate.cpp`. Both halves are required: a version-guarded read so
  the old file parses, and a migration so the loaded world is correct rather
  than merely parsed.
- **Verifying a worldgen change:** `python tools/compare_golden.py`. It needs a
  checkout of the archived web repo — see the note at the top of that file.
- **Shipping a release:** the public version is `project(... VERSION x.y.z)` in
  `CMakeLists.txt`; changes are outlined in `CHANGELOG.md` and
  `tools/release.py` bumps, packages and publishes the GitHub release — see
  [docs/RELEASING.md](docs/RELEASING.md).

### Built with a resource pack loader in mind

None of it is implemented, but several things are shaped for it rather than
against it, because retrofitting them would mean rewriting the atlas, the
mesher and the item pipeline: textures are addressed by a namespaced identifier
through a variable layer (`#side`) with model `parent` chaining; the atlas
builder handles variable tile resolution, gutters and per-tile mipmaps and keeps
its CPU pixels; the mesher emits a generic quad list from an MC-shaped
`BlockModel` in 0..16 space; the vertex format carries a real per-vertex tint
channel; items go through one `ItemModel` with display transforms instead of
being smeared across four files; and entity meshes have genuine UVs rather than
colour smuggled through the UV slot.

## Deliberately deferred (foundation already in place)

More mob types & deeper combat variety (sheep, pigs, cows and zombies are in,
and the zombie already has real line-of-sight and pathfinding), farming/crops
(hunger, eating, milk and cooking are in — planting and growing isn't), greedy
meshing, fully smooth (non-voxel) global lighting, resource packs (the seven
abstractions above are in place and inert), and future uses for the newest
ores — **Gloamite** is earmarked for more teleport/void tech beyond the
Wayshard, and **Verdanite** for growth and alchemy once farming lands.

### What the port does not carry over

- **WAN multiplayer without a forwarded port.** WebRTC traversed NAT for free;
  UDP does not. LAN discovery and the invite-code UX survive, and there is a
  seam for a relay backend.
- **Cross-play with the browser build**, and **worlds saved by it** — the save
  format is binary and starts fresh. The archived repo still plays those.
- **The menu panorama** (F8 in the web build). Instead of the rotating cube map,
  the menu takes a still picture: **Set BG** on any capture in the Gallery makes
  it the backdrop, and clearing it falls back to the stylesheet's own gradient.
  The panorama toggle is gone from Settings — with no panorama to toggle it
  switched between the fallback gradient and the same fallback gradient.
- **Pixel-identical inventory icons.** All 67 sprite icons and every
  cross-rendered block icon are bit-exact against the browser; the 120
  cube/shape icons differ only along the one-pixel antialiased silhouette, where
  the browser's rasteriser blends edge coverage its own way. They are also
  deliberately centred here, where the browser's sat two pixels up and left.
- **The audio compressor**, which is a documented approximation of Chrome's
  rather than a reproduction — within 0.2 dB across the range the game uses.
