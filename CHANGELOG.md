# Changelog

All notable changes to Hollowreach are recorded here, newest first.

The format follows [Keep a Changelog](https://keepachangelog.com/) and the
version numbers follow [Semantic Versioning](https://semver.org/):
**MAJOR** for big milestones or compatibility breaks, **MINOR** for content and
feature updates, **PATCH** for fixes and tuning.

`tools/release.py` reads this file: whatever sits under a version's heading
becomes the GitHub release notes for that version, so write entries for
players, not for the git log. Day-to-day changes go under **[Latest]**;
`python tools/release.py bump <major|minor|patch>` moves them under a new
version heading when it's time to ship.

## [Latest]

### Added
- **Blocks hold each other up now.** Torches, plants, mushrooms and pebbles need
  solid ground beneath them and break into their drop when it goes — mine the
  dirt under a torch and the torch comes with it.
- **Sand falls.** Dig it out from underneath and it comes down as a real falling
  block, stacking wherever it lands, so tunnelling through a dune collapses it.
- **Water washes things away.** A flood takes out the torches and greebles it
  flows into. Water sitting quietly beside a shoreline leaves it alone — only
  water that actually moves into a cell destroys what was there.
- **A red damage vignette.** A hit you did not see coming no longer goes
  unnoticed because you were looking somewhere else.
- **Number keys in the inventory.** Hover a slot, press 1–9, and it swaps with
  that hotbar slot.
- **V-Sync, a frame rate limit and a borderless window**, all in Settings →
  Graphics. The limit is exact — 30, 60, 120 and 144 all land on the number.
- **A Copy Invite Code button** while hosting, replacing "copy it from the log".

### Changed
- **New worlds pick somewhere sensible to start.** Spawn was a fixed point
  whatever happened to be there, and since roughly half of any seed is ocean,
  half of all new worlds began treading water on a beach. It now finds dry,
  reasonably flat ground as near that point as it can. Existing worlds keep the
  spawn they were saved with, and no terrain has changed.
- **The Atlas map opens with M**, not N.
- **Clouds have tops.** They were paying for a ray-marched sky and looking like a
  painted one: the shape was the same at every height. They now bulge and dome
  and erode, with much stronger self-shadowing, which is what a volumetric cloud
  is for.
- **Shadow edges are soft instead of stepped**, from a rotated 12-tap disc rather
  than a fixed 3×3 grid — which had been showing its stair-steps more clearly on
  Ultra, not less.
- **God rays are easier to see.**
- **Pickaxes, axes and hoes are held nearer vertical**, so they strike with the
  point or the edge rather than the flat of the head. Shovels are unchanged,
  because a shovel really does dig face-first.
- **Recipes that take any wood say so.** The ingredient showed as "Oak Planks",
  which is exactly how somebody in a pine forest concludes they cannot make a
  pickaxe.

### Fixed
- **The recipe book's Back button** closed everything and freed the mouse instead
  of returning to the crafting screen you opened it from.
- **Menu backgrounds.** Setting one from the Gallery worked until you left the
  Gallery, and was forgotten entirely on restart. Three separate faults: it was
  never saved, the main menu painted an opaque gradient over it, and the Settings
  screen did the same.
- Toggling fullscreen no longer silently turns V-Sync back on.

## [2.0.0] - 2026-08-01

### Changed
- **Hollowreach is now a native application.** The whole game has been rewritten
  in C++ and ships as a single executable — no browser, no local server, no
  Python. Download, unzip, run. It looks and plays the same on purpose: the same
  renderer, the same world generator, the same gameplay numbers, verified
  against the old build value by value, so a seed makes the terrain it always
  made.
- **Multiplayer is LAN-first.** Games on your network announce themselves and
  appear in a list, so joining a friend is usually one click instead of pasting
  two codes back and forth. Invite codes still work and look the same. Playing
  with someone outside your network now needs a forwarded port (25565/udp),
  which is the one thing the old browser build got for free.
- **Worlds are saved in a compact binary format** in `data/worlds/`, written
  atomically so a crash mid-save cannot cost you the world, and refused with a
  reason rather than loaded as nonsense if a file is damaged. Worlds from the
  browser build do not load; the archived version still plays them.
- **The world is a hundred blocks deeper.** It now stands 192 blocks tall with
  the sea at y=100, where it was 128 with the sea at 46 — and every one of those
  new blocks is *underground*. The surface is unchanged: same coastlines, same
  mountains, same trees in the same places, just with far more beneath them.
  Worlds made before this update keep the world they were generated with and do
  not shift.
- **Ore is something you dig for again.** Rarer ores now sit in depth bands well
  below anything the surface can reach. Ravines used to bottom out at y=8–19
  while diamond spanned y=3–16, so a canyon near spawn was a delivery service and
  it was possible to find every ore in the game in one of them inside ten
  minutes. Ravines now stop at y=62 and the four rare ores end at y=55 or lower.
  Caves still expose ore, and still should — spelunking is meant to pay.
- **Wooden tools can be made from any wood.** The recipe asked for oak planks
  specifically, so spawning in a pine, birch, dusk or palm forest meant you could
  not craft your first pickaxe at all — while a bed, which already accepted any
  wood, worked fine.
- **Fewer animals.** A meadow used to fill up with nineteen head of livestock;
  it now holds about a quarter of that.
- **Flight is off by default** — a double-tap of Space was too easy to trigger by
  accident. Turn it back on under Settings → Gameplay.

### Added
- **Shift-drag and Q-drag in the inventory.** Hold shift and drag across slots to
  move every one you touch; hold **Q** and drag to throw them on the floor. Q over
  a single slot drops that stack. Each slot acts once per gesture.
- **A Recipes button** on the inventory and every crafting screen, and a Back
  button in the book itself. The recipe book keybind moved from **R to H**.
- **Menu background from a screenshot.** **Set BG** on any capture in the Gallery.
- **Fullscreen** in Settings, and **Quit** on the main menu. Alt+Enter toggles
  fullscreen too, and the game remembers which you chose.
- **F1 hides the interface** — hotbar, hearts, crosshair, minimap and the block
  outline — for a clean screenshot.
- **Interface scale** and **raw mouse input** settings, neither of which the
  browser build needed.
- **Render Resolution** in Settings. Draws the world at a fraction of the window
  and scales it up; the interface stays sharp either way. Picking a quality
  preset sets it, and you can then move it yourself.
- **A screenshot gallery**, reachable from the main or pause menu — view, delete
  or reveal your captures, which are ordinary PNGs in `data/screenshots/`.
- **Export and import worlds.** `data/exports/` is both outbox and inbox; an
  imported world always arrives as a new copy, so importing the same file twice
  gives you two worlds rather than overwriting one.
- **Nameplates** over other players, fading with distance, with a health bar
  underneath when someone is hurt.
- **Beds work in single player.** They previously did nothing at all. With
  company, sleeping is a vote — the night passes when everyone is in bed, and the
  host announces the tally.

### Removed
- **The panorama menu background and its setting.** The rotating cube-map behind
  the menu is not implemented in the native build; set a screenshot as your
  background instead (Gallery → **Set BG**).
- **Playing with someone outside your network without port forwarding.** The
  browser build got NAT traversal for free from WebRTC; a native UDP transport
  does not. LAN play needs no setup at all, and the invite-code flow is unchanged.

### Fixed
- **The frame rate.** On the test machine (Radeon RX 5700 XT, 1920×1080),
  standing on the surface at render distance 12 on High, the game ran at 17 fps;
  it now runs at 378, and Ultra at 342. The cause was a single wrong flag on the
  chunk mesh buffers that told the graphics driver to keep them in system memory,
  so the card was fetching every triangle of the world back across the bus every
  frame instead of reading its own. Render distance now costs almost nothing —
  4, 8 and 12 are within a millisecond of each other — and the gap between the
  Low and Ultra presets is about the same. The README has the full table.
- **The outermost ring of chunks had no terrain in it,** and the ring just inside
  it was drawn too dark. Chunks on the edge of the loaded area were waiting on
  neighbours that were never going to be generated, so they were never built at
  all. The world now generates one ring further out than it draws.
- **Boats are ridden facing forward.** The hull was modelled bow-to-+z while a
  ridden boat is handed the player's yaw, whose zero faces -z - so you sat facing
  the stern. The browser did this too.
- **The controls table said M opened the map.** It is N, and the "minimap toggle"
  row was not a key at all; it is a setting.
- **You can die.** Fall damage, drowning and starvation all emptied the hearts
  and then did nothing at all. Dying now drops what you were carrying where you
  fell, pins a death waypoint on the Atlas, and wakes you at your Soul Anchor.
- **Boats can be placed** and **wayshards work.** Both did nothing at all when
  used — the click was simply swallowed.
- Block icons sit centred in their inventory slots instead of two pixels up and
  to the left.
- Graphics quality and render distance are no longer quietly reset to their
  defaults every time the game starts.

## [1.2.0] - 2026-07-25

### Added
- The held item is animated: it swings when you mine, hit, place or use
  something (holding a mine keeps it swinging), sways with your walk cycle,
  rises into frame when you switch hotbar slots, and drifts gently at rest.
- Held items are lit by where you are standing, so they dim at night and go
  almost dark in an unlit cave instead of glowing at full brightness.

### Fixed
- Held blocks no longer lose faces — a held cube's top face could be painted
  over by its own bottom face, and shaped blocks like stairs showed through
  themselves. The viewmodel now depth-tests properly.
- Held items keep their place on screen at any window shape. On narrow or tall
  windows they used to slide off the right edge.

### Changed
- Tools, swords, blocks and panels (doors, trapdoors, ladders) each have their
  own hand pose, so a sword is held hilt-outward and blade-up, a pickaxe by its
  handle, and a door turned to show its face instead of edge-on.
- Held items sit lower in the corner and read at a consistent size.

## [1.1.0] - 2026-07-19

### Added
- Dropped items are now full 3D models: blocks appear as their real shape
  (stairs are stair-shaped, slabs are slab-shaped) and non-block items like
  tools and food are extruded from their pixel-art sprite.
- The held viewmodel uses the same 3D models — swords, pickaxes, and every
  other item now show their actual sprite shape in your hand instead of a
  tinted cube.
- Item drop shadows are sprite-shaped: a dropped sword casts a sword-shaped
  shadow instead of a solid rectangle.

### Changed
- Inventory icons for shaped blocks (stairs, slabs, doors, trapdoors, beds)
  now show the block's actual shape in isometric view instead of a full cube.
- Sword pixel art widened to three texels so the blade reads cleanly at the
  larger held/dropped model scale.

## [1.0.0] - 2026-07-19

First public release. Hollowreach is a voxel sandbox that runs entirely in the
browser — no engine, no libraries, no build step — served by a tiny local
Python script. Highlights of everything on board at 1.0:

### World
- Procedural infinite terrain with biomes, caves, ravines, ores (including
  Gloamite and Verdanite), trees, and 12 kinds of foliage and flowers.
- Terrain generation is versioned: old worlds keep their exact shape when the
  generator improves.
- Flowing water with source/spread/recede mechanics, currents that push you,
  and swimming/drowning.
- Day/night cycle with ray-traced sky, sun, moon, and stars; volumetric clouds
  with moving cloud shadows.

### Gameplay
- Mining, building, crafting (workbench + forge with fuel and smelting), chests,
  doors, ladders, beds with sleep-to-morning, and a recipe book (R).
- Survival systems: health, hunger, eating, armour, fall damage, drowning.
- Mobs: sheep (wool), pigs (pork), cows (milk), and zombies with real
  line-of-sight, memory, and A* pathfinding.
- Boats, the Atlas world map with fog-of-war and waypoints (M / minimap N),
  and the Wayshard warp item.
- File-backed world saves with versioned migrations — old saves keep working.

### Multiplayer
- Peer-to-peer co-op straight between browsers: copy-paste invite codes, no
  account, no game server. Optional TURN relay support for strict routers.
- Host-authoritative with client-side prediction: movement, building, combat,
  containers, sleeping, and mobs all sync.

### Graphics
- Deferred renderer (WebGL2): smooth lighting, coloured point lights, cast
  shadows, SSAO, god rays, and screen-space water reflections.
- Leaf sway, water ripple, walking camera bob, and quality presets from Low to
  Ultra in a tabbed settings menu.

### Audio
- Fully synthesised sound — every effect is generated in the browser with the
  Web Audio API: footsteps by surface, mob voices, breaking/placing by
  material, wind, birds, crickets, cave ambience, and underwater muffling.

### Platforms
- Windows (`run.bat`) and Linux/macOS (`run.sh`) launchers; the only
  requirements are Python 3 and a WebGL2 browser.

[Latest]: https://github.com/Anonymous-Floof/Hollowreach/compare/v2.0.0...HEAD
[2.0.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.0.0
[1.2.0]: https://github.com/Anonymous-Floof/Hollow-Reach/releases/tag/v1.2.0
[1.1.0]: https://github.com/Anonymous-Floof/Hollow-Reach/releases/tag/v1.1.0
[1.0.0]: https://github.com/Anonymous-Floof/Hollow-Reach/releases/tag/v1.0.0
