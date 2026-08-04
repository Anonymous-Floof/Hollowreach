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

> **Multiplayer needs both machines on this version.** A world's difficulty and
> cheat rules are now the host's to set, and an older build has no idea they
> exist — it would quietly go on applying its own monster and flight settings in
> somebody else's world, with nobody told. So the two refuse each other at the
> join screen instead. Your **saves are fine in both directions**: a world made
> here opens in 2.5.x under the default rules.

### Added
- **Some settings now belong to the world, not to you.** The settings screen has
  two new tabs, **Difficulty** (fall damage, hunger, monster spawning) and
  **Cheats** (flight), and everything on them is saved inside the world rather
  than on your machine. Turn monsters off in a creative build and your survival
  world is untouched; share the world and its rules go with it. They only appear
  while a world is open, because on the main menu there is no world whose rules
  could be shown.
  **In multiplayer the host owns them.** A guest can see what they are playing
  under but cannot change it, and when the host changes something everyone is
  told at once. Everything else — graphics, controls, auto step, volume — is still
  yours and still follows you from world to world.

### Changed
- **Q drops one item, not the whole stack.** Pressing Q over a slot used to bin
  everything in it. It now drops a single item, **Shift+Q** drops the lot, and
  **holding Q** keeps dropping — faster the longer you hold, so a full stack takes
  about three seconds instead of sixty-four presses. Dragging Q across slots still
  works and now takes one from each. In the world Q is unchanged at one item, with
  Ctrl+Q for the stack and the same hold-to-repeat.

### Fixed
- **High Step no longer stops you walking up stairs.** Turning the setting on made
  indoor staircases *harder* to climb than leaving it off: the game checked whether
  you would fit at the full height of the step rather than at the height you were
  actually stepping to, so it demanded half a block more headroom than the step
  needed and any staircase with a ceiling over it refused. A flight that the
  default step climbed all six blocks of stalled after one and a half with High
  Step enabled.

## [2.5.1] - 2026-08-04

> Plays with 2.5.0. The multiplayer protocol did not change, so a 2.5.1 machine
> and a 2.5.0 one can still share a world — though only the 2.5.1 side gets the
> discovery fix, so host from that one if a world will not appear.

### Added
- **`--net-doctor`.** Run `Hollowreach.exe --net-doctor` from a terminal and it
  says why this machine might not be found by another: which networks a game here
  would be announced on (and which are skipped, and why), whether the two ports
  are free, whether the machine can hear its own announcement, and the exact
  program path Windows firewall rules are keyed on. Run it on both machines and
  compare — if the protocol numbers differ, the two builds simply cannot see each
  other, which nothing else in the game will ever tell you.

### Fixed
- **Worlds on your network actually show up now.** A world announced itself with
  a single broadcast, and a broadcast only leaves your machine by *one* network
  card — so on any PC with Hyper-V, WSL, VirtualBox or a VPN installed, the
  announcement could sail off into a virtual network with nobody on it. Nothing
  reported an error. That is why hosting from one machine worked while hosting
  from the other did not, and why typing the address in by hand often worked when
  the list stayed empty. A world is now announced on every network your machine
  is actually on, and the Join screen also *asks* rather than only listening, so
  a world usually appears the moment you open it.
  If a world still does not appear, the other cause is Windows Firewall, which
  silently drops incoming announcements for a program it has never been asked
  about — allow Hollowreach through for Private networks on the hosting machine.
  Run with `--verbose` and the log now names every network it is announcing on.

### Changed
- **Playing on the same network stops feeling like playing across an ocean.**
  Other players and mobs were drawn a fixed sixth of a second in the past, on top
  of a position that was sent ten times a second and relayed ten times a second —
  about a quarter of a second of delay on a connection whose real round trip is
  roughly one millisecond. None of those numbers looked at the connection. The
  game now measures how fast updates are actually arriving and holds back only as
  much as it needs, positions go out twice as often, and a block someone else
  breaks reaches you on the next frame instead of up to a tenth of a second later.
  On a LAN that is roughly a two-and-a-half times cut in visible lag; on a poor
  connection it still buffers as much as it did, because now it can tell the
  difference.

## [2.5.0] - 2026-08-04

### Changed
- **Torches light up instantly, and go out instantly.** Placing or breaking a
  block used to rebuild the lighting of the nine chunks around it from scratch,
  then take several frames to settle — which is why light sometimes crawled
  across a chunk border a step at a time, and why a torch you had already broken
  could keep glowing through a seam until something else nudged it. Light now
  spreads outward from the change itself and stops when it runs out, so it
  arrives and leaves in the same frame you swung.
- **A loaded world takes about a third of the memory it used to.** Chunks no
  longer pay full price for the enormous amount of nothing most of them contain:
  empty sky, unlit rock, and the ninety-odd layers of stone that are all the same
  block. At render distance 12 that is 135 MB down to 40 MB, and at 16 it is
  228 MB down to 67 MB — which is what makes the longest view distances
  comfortable on an ordinary machine rather than merely possible.
- **Chunk streaming got faster as well as lighter.** The work the main thread
  does while a world loads in is down by a third against 2.4.0, and below where
  it was in 2.3.0 — before any of the seam fixes existed at all.
  While measuring that, the note in 2.4.0 below saying its lighting fix cost
  "about a second longer" to load a world turned out to be wrong: it came from
  timing runs that were limited by the monitor's refresh rate rather than by the
  game, so it was not measuring the change at all. 2.4.0 cost about 59
  milliseconds, spread across a whole world load, and nobody could have seen it.

## [2.4.0] - 2026-08-03

### Added
- **The Time Wheel.** A bed no longer sleeps the moment you touch it. It opens a
  24-hour dial, painted with the day it describes — blue through the night, pale
  gold through the middle of the day, warm at the two crossings — with the hour it
  is now marked on it. Closing it again costs nothing, which makes a bed the
  nearest thing to a clock in the game and worth opening even when you have no
  intention of sleeping. Drag the handle to the hour you want to wake at and
  confirm, and you sleep until then: a nap through the worst of a night is now a
  different decision from sleeping the whole of it.
- **Evil Altar.** A dark, caged block with something burning inside it. While you
  are near one it breeds zombies into the space around it — in daylight as readily
  as at night — up to a small crowd, and then waits. It obeys the same darkness
  rule as everything else, so lighting the room is what shuts it off, the same way
  you would disarm one in Minecraft rather than digging it out. There is **no
  recipe** and mining it destroys it: it exists for the dungeons that will place
  it, and until then `--give evil_altar` is the only way to hold one.

### Changed
- **You have to have earned a night's sleep.** A bed will not take you until **8
  game hours** after the last time you slept, and the wheel says how long is left
  when it will not. Sleeping was a button that deleted any night you did not fancy;
  now it is something you do about once a day. In multiplayer only the person who
  *opens* a bed has to be tired — everyone else is voting on their hour, not
  proposing one of their own, so a group is never held awake because one of them
  napped more recently than the rest. Whoever asks first picks the time, and the
  rest see their name and their hour on their own bed.

- **Monsters spawn in the dark, not after dark.** The rule was the clock: past
  dusk, on the surface, anywhere solid. It is now light level zero and nothing
  else. On open ground that works out the same — the sky goes dark and they come
  — but a cave, a mineshaft or a room you have roofed over is pitch dark at noon
  too, and now spawns them at noon. Digging without a torch in your hand has
  become a decision. Lighting a space still stops them completely, because the one
  rule is the one rule: inside a torch's radius nothing spawns, underground or out.
- **Zombies that get a long way from you are removed.** Previously the sun did
  this job — every zombie burned away at dawn, so the two-at-a-time limit cleared
  itself daily. Cave spawns never see the sun, and a mob in ground you have walked
  away from is frozen rather than deleted, so without this the limit would have
  filled once with monsters nobody would ever meet again and the nights would have
  gone quiet for good. Animals are untouched: one you walked home stays where you
  put it.

### Fixed
- **Light now crosses a chunk border.** Placing a torch within a block or two of
  an invisible chunk line lit your side of it and stopped dead at the line — and
  taking that torch away again left the *far* side still glowing, lit by a torch
  that no longer existed. Sealed rooms that straddled a line kept a strip of
  daylight along the seam. Lighting is worked out one chunk at a time, each reading
  its neighbours, which only settles if a chunk that changes tells the neighbours it
  changed for — and nothing ever did. It does now, and only when it has something
  they would actually take, so the common case still costs nothing. Loading a world
  at render distance 12 takes about a second longer for it; running frames are
  unaffected.

## [2.3.0] - 2026-08-02

### Changed
- **Crouching does something you can see.** Holding Shift now lowers your head —
  the same drop Minecraft uses — so it is obvious the key is doing anything at all,
  and it keeps you on the block you are standing on: walk at a ledge while crouched
  and you lean out over the edge instead of stepping off it. Diagonally, only the
  direction that would drop you is refused, so you slide along the rim rather than
  stopping dead.
- **Caves are no longer drowned.** Underground water used to be one line drawn
  across the whole world, with everything carved below it filled in. That line was
  set when the world was less than half as deep, and when the world grew it came
  along — until it sat at y=66, which is very nearly the entire underground and
  *every one* of the four ores worth going to find. Mining the only tier that
  rewards mining meant mining underwater. Water now collects in lakes instead,
  filling a cave from its floor up to its own level, so the deeps are mostly dry
  and water is something you come across rather than something you swim through.
  Measured on one seed, the deep ore band went from 100% flooded to 9%.
  **New worlds only** — an existing world keeps the terrain it was made with, so a
  drowned cave stays drowned in the world you found it in.
- **Water stops where it should.** Poured out on flat ground it spread seven blocks
  and stopped, as it always has. Poured off any height it spread at full strength
  from every level of the fall at once, and each cell it reached fed the next — so
  it did not stop: one bucket off a twenty-block wall wet ninety-six thousand
  cells. Water now falls before it spreads. One bucket, one column, and seven
  blocks out from wherever it lands.

### Fixed
- **Joining a world that has actually been played.** A guest silently threw away
  any world larger than 64 KB — about a minute of play — and then sat there until
  the connection timed out, reporting that the host had never sent one. A brand new
  world squeaked under the limit, so whether multiplayer worked at all came down to
  how much the host had built. Worlds up to 24 MB now transfer.
- **Building as a guest.** A guest applied the host's edits back through the very
  path it uses to *ask* for one, so everything the host changed was immediately
  requested back, granted, relayed and requested again. The loop ran until the host
  stopped trusting the guest — at which point the blocks being refused were the
  ones the player had actually placed, appearing for an instant and vanishing.
- **Which way everybody is facing.** Player bodies were drawn half a turn out, so
  two people looking straight at each other each saw the other's back. Guests were
  also never told which way *other guests* were facing, which left everyone but the
  host frozen looking north in a world with three or more players.
- **Falling sand no longer throws you through walls.** Sand landing on you put a
  solid block where you were standing, and the physics answer to a body inside a
  block is to move it to the nearest way out — which can be on the far side of a
  wall, the same way closing a door on yourself used to. Sand now waits on top of
  you and lands the moment you step aside.

### Multiplayer
- **The network protocol moved to version 3**, so this release and 2.2.0 do not
  play together and say so at the join screen. Nothing about the wire format
  changed — the reason is that all three multiplayer faults above are in the code
  that *reads* it. A 2.2.0 guest would still throw away a world it could not fit in
  one message, still bounce every edit the host sent it straight back, and still
  draw everybody facing the wrong way, and it would do all of that after a
  handshake that looked fine. The same rule 2.2.0 applied to 2.1.1: a clear refusal
  beats a session that quietly does not work.
- **Terrain generation moved to version 4** for the underground water change. That
  is not a compatibility break — the version travels inside each world, so an older
  world keeps generating exactly the terrain it always has, wherever it is opened,
  and a guest builds the host's world to the host's version. Worlds and saves are
  unaffected in both directions.

## [2.2.0] - 2026-08-02

### Added
- **Paintings.** Craft one from 8 planks, wool and azurite, hang it on a wall, and
  right-click it to put any screenshot you have taken on display. The picture lives
  *in* the painting rather than pointing at the file, which is what makes the rest
  of it work: delete the screenshot and the painting is unchanged, export the world
  and your art goes with it, and a friend who joins sees exactly what you hung even
  though they have never seen your captures folder. Pictures are scaled to 128
  square on the way in — 48 KB each, and still sharper than anything else in the
  world.
- **A built-in updater.** **Check for Updates** on the main menu fetches the latest
  public release and installs it over your copy, in three deliberate clicks —
  check, download, install — with the version shown before anything is fetched.
  It adds and overwrites only what was in the release zip, so `data/`, resource
  packs and anything else you keep beside the game are untouched. Windows only for
  now, and it never checks or installs on its own.

### Changed
- **Clicking a recipe in the book lays it out for you.** If the book was opened
  from a crafting screen and you have the ingredients, the grid fills itself —
  no more remembering shapes. Short of something and it leaves the grid alone
  rather than half-filling it.
- **Charcoal takes any log**, not just oak. It read "Oak Log" and meant it: the
  smelting table matched by name, so a forge in a pine forest refused every log
  fed into it while the book said logs make charcoal. It now says "Any Logs" and
  does.
- **The Atlas recipe shows its three paper as three slots**, which is what it
  actually wants. One chip reading "paper x3" said the opposite, and stacking
  three paper into one square is not the recipe.

### Fixed
- **Paintings no longer shimmer.** The picture sat exactly on the plane of its own
  frame, so which of the two a pixel showed came down to depth rounding and a
  painting flickered between the photograph and blank canvas as you moved. It is
  now lifted two millimetres proud of the frame.
- **A painting can be given a new picture.** It kept showing the first one you
  chose, and would only take another after being broken and put back — because
  the uploaded texture was cached by position, and a position is not a picture:
  choosing a second screenshot leaves the painting exactly where it was, so the
  cache saw nothing to do. It now tracks which picture it is holding.
- **The held tool no longer swings when you close an inventory.** Every
  right-click started a swing, including the ones that only open a screen — and
  opening a screen pauses the world, so the swing had nowhere to play. It sat
  queued behind the pause and ran when the screen closed, several seconds after
  anything was clicked. Opening a chest, a workbench, a bed, a soul anchor or a
  painting no longer swings at all, and a swing interrupted by a screen is
  dropped rather than left frozen mid-arc.
- **Field of View survives a restart.** It always saved and the slider always showed
  the right number; the camera was the one thing never told, because startup set the
  projection back to the default seventy a few lines after applying your settings.
  Every other setting in the schema was checked for the same fault and none had it.
- **Tools swing the way they now look.** 2.1.1 turned them to lead with the edge but
  left the swing arc alone, so they still dug sideways. The arc was being added into
  each item's own pose angles, which means the axis it turned about moved whenever a
  pose did — the moment a tool was turned a quarter, "swing down" became "spin in
  place". The animation is now applied outside the pose, so the arm comes down
  whatever the hand is holding. For a pickaxe that is 7× more downward travel than
  sideways, where before it was nearly 2× the other way.

### Multiplayer
- **The network protocol moved to version 2**, so 2.2.0 games do not accept 2.1.1
  ones and say so at the join screen. Paintings need a message that 2.1.1 has no
  handler for, and a lobby where half the players silently cannot see the art on the
  walls is worse than a clear refusal. Worlds and saves are unaffected — a 2.1.1
  save loads here and a save from here loads there.

## [2.1.1] - 2026-08-01

### Added
- **Pick block on middle click.** Aim at a block and click the wheel to bring one
  to hand: it selects the stack if it is already on your bar, and otherwise pulls
  it out of your pack into a free slot, swapping only if the bar is full. It
  cannot conjure blocks — there is no creative mode here — so it finds one you
  already own.

### Fixed
- **Wall torches stopped falling off walls.** The support rules that arrived in
  2.1.0 only ever looked at the cell below, and a torch mounted on a wall has
  nothing below it by definition, so every one of them broke the moment anything
  disturbed it. A block now asks whichever cell actually holds it up.
- **Walking up stairs no longer jolts the camera.** An auto-step moved you the
  full step height in a single frame and left gravity to settle you onto the
  tread, which took a few frames of not being on the ground — long enough to stop
  the walk cycle and start it again. Once per stair, that read as jitter. You now
  land on the tread directly and the eye eases up over about a tenth of a second.
- **Spawn avoids ravines, and dying takes you back to where the world started
  you.** Two separate faults with the same symptom, which together were a death
  loop: the spawn search only consulted the surface heightmap, and a ravine is
  carved *after* that heightmap out of ground it still reports as solid — so a
  canyon was invisible to it and you could be placed on the lip of a thirty-block
  drop. Meanwhile respawning ignored the chosen spawn entirely and sent you to the
  world origin, which is exactly the spot the search exists to move you off. Over
  twenty-four test seeds the old rule left seven starts beside a drop that could
  kill outright, the worst of them fifty-seven blocks; there are now none.
- **Tools and swords lead with their edge.** They were still swinging the flat of
  the head into the block. A held item is its sprite extruded a texel, and the
  flat leads exactly while that face still points down the view axis — so the
  turn needed to be a full quarter, not the half-measure of the last attempt. The
  art stays readable because the hand sits far enough to the right that side-on to
  the swing is nowhere near side-on to the eye. The shovel is unchanged: it digs
  with its blade face, and always did.
- **The map's own key legend says M**, which is what the map has opened with since
  2.1.0 rebound it; and the recipe book's Back button and the multiplayer status
  line no longer show a stray `Â` where a separator dot belongs.
- **The README's performance table is honest again.** It quoted numbers about a
  third faster than this build measures, from a camera nobody wrote down — and
  since roughly half a frame is the cloud march, where you stand changes the
  answer more than the quality preset does. Every cell is re-measured, and the
  exact command that produces it is printed underneath so it can be checked. The
  renderer itself is untouched in this release: the old and new builds measure
  the same frame, pass for pass, from the same spot.

## [2.1.0] - 2026-08-01

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

[Latest]: https://github.com/Anonymous-Floof/Hollowreach/compare/v2.5.1...HEAD
[2.5.1]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.5.1
[2.5.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.5.0
[2.4.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.4.0
[2.3.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.3.0
[2.2.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.2.0
[2.1.1]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.1.1
[2.1.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.1.0
[2.0.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.0.0
[1.2.0]: https://github.com/Anonymous-Floof/Hollow-Reach/releases/tag/v1.2.0
[1.1.0]: https://github.com/Anonymous-Floof/Hollow-Reach/releases/tag/v1.1.0
[1.0.0]: https://github.com/Anonymous-Floof/Hollow-Reach/releases/tag/v1.0.0
