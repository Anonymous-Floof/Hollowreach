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

[Latest]: https://github.com/Anonymous-Floof/Hollow-Reach/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/Anonymous-Floof/Hollow-Reach/releases/tag/v1.1.0
[1.0.0]: https://github.com/Anonymous-Floof/Hollow-Reach/releases/tag/v1.0.0
