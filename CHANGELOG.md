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
- **Chat.** **T** to talk, **`/`** to run a command. It draws over the world and
  does not pause it — the reason you are typing is usually something you are
  looking at. Lines fade after ten seconds when the box is shut; **PageUp** reads
  back through them when it is open. Joins and leaves land in the log as well as
  in a toast, so "when did Ada leave" is a question you can answer.

- **Typed commands**, in single player as well as multiplayer. Twenty-seven of
  them: `/give` `/tp` `/time` `/gamemode` `/summon` `/heal` `/clear` `/kill`
  `/locate` `/spawn` `/seed` `/list` `/msg` `/me` `/say` `/save` `/help`, and the
  administrative half below.

  **`/set` reaches the whole settings screen from the chat box** — `/set fov 90`,
  `/set monsters false`, `/set renderDistance 10`. It refuses exactly what the
  settings screen refuses, including the rule that a world created Survival can
  never become Creative.

- **Autocomplete that does not need you to remember anything.** Start typing and
  a list appears, matched fuzzily rather than by prefix: `sto` finds `greystone`,
  `rd` finds `renderDistance`, `pkst` finds `pick_stone`. **↑/↓** move through it,
  **Tab** takes one, **Enter** sends. Nothing is highlighted until you press ↓, so
  Enter on a command you already know sends it instead of replacing your last word
  with the machine's guess. With no list up, ↑/↓ walk back through what you sent.

  Arguments complete from what they actually accept: a player argument offers
  whoever is in the world, `/give` the item registry, `/set` the settings schema,
  and the value after `/set` whatever that particular setting takes.

- **Permissions, and a host who can hand them out.** Four levels — *anyone*,
  *trusted*, *operator*, *owner*. The host is always owner; `/op ada trusted`
  promotes a friend, `/deop` takes it back. Nobody can grant a level above their
  own, and nobody can kick, ban or demote somebody who outranks them.

- **`/kick`, `/ban`, `/pardon`, `/banlist` and `/whitelist`**, kept in
  `data/access.json` beside your settings rather than inside a world — being an
  operator is a fact about a person, not about a place, and it should not
  evaporate because you made a new world. Hand-editable. A ban is typed against a
  name and starts covering that person's id the first time they connect under it,
  so it survives them renaming themselves. `/whitelist on` refuses everybody not
  named, with operators on the list by virtue of being operators so that turning
  it on cannot lock out the people trusted to turn it off.

  This is also the groundwork for the dedicated server: it is the same file, the
  same levels and the same commands a server with no window will need.

- **`--command <line>`**, repeatable, runs a command once the world is up. Every
  chat line also goes to `data/hollowreach.log`. Between them a command can be
  checked without a keyboard, and somebody running a world for other people can
  see what was said in it.

### Changed
- The multiplayer protocol is now version 11, so **both machines need this
  build.** An older guest would receive no chat and never be told its permission
  level — its own completion popup would offer it every command in the game while
  the host refused each one with a message it could not receive either. Silence
  that looks like a bug in the host.

### Fixed
- **A guest leaving now says so.** Quitting the game never sent a goodbye, so
  everyone else watched a body stand there until the connection timed out —
  seconds of a player who had already closed the window. The host had handled a
  goodbye since multiplayer landed; nothing was ever sending one.
- **Leaves are recorded.** Somebody leaving reached a two-second toast and nothing
  else, so the one event a host most wants a record of left no trace. Joins and
  leaves are both in the chat log now — and the host can see them, which it could
  not before: a broadcast reaches every guest and never the person running the
  world.

## [2.11.0] - 2026-08-10

### Added
- **Sound packs.** Your own sounds can replace the game's, and the layout is
  Minecraft's: a folder in `data/resourcepacks/` holding `pack.mcmeta` and
  `assets/<namespace>/sounds/`. The event names are Minecraft's too —
  `block.stone.break`, `entity.cow.hurt`, `ui.button.click` — so a Minecraft
  sound pack largely works as-is rather than needing to be repackaged. `.ogg`
  and `.wav` both play.

  Anything a pack leaves out keeps the sound the game synthesises for it, so a
  pack that replaces four sounds replaces four sounds and nothing else changes.
  Sounds this game has and Minecraft does not fall through to the nearest thing a
  Minecraft pack would have: ores use the stone set, leaves use grass, trapdoors
  use doors, the inventory tick uses the button click.

- **A Resource Packs screen**, on the main menu and under Settings › Audio.
  Turn packs on and off, and drag the order about when you have several — the top
  of the list wins. It shows what each pack supplies, what it is currently
  replacing, and what is wrong with one that is not working, because a pack that
  silently does nothing is the worst thing that can happen to somebody making one.

- **A sound pack comes with the game.** `FilmCowSFX`, built from the FilmCow
  Royalty Free SFX library, replaces 68 of the 78 sounds — real recordings for
  footsteps, breaking, doors, chests, water and the player. It ships **switched
  off**: a fresh install should still sound like the game. Turn it on in Resource
  Packs. Sheep, pigs and cows are deliberately left out, because the library has
  no farm animals and the synthesised bleat, oink and moo are written for them —
  which is also a demonstration of what a partial pack does.

- **A pack you can fill in.** **Create Example Pack** writes the whole folder
  tree, one empty folder per sound, with an `EVENTS.txt` listing every event and
  the file that replaces it, and a README. Making a pack is then dropping files in
  — no `sounds.json` needed, since a file at the path the event name spells out
  (`block.stone.break` → `sounds/block/stone/break.ogg`) is picked up on its own.
  Name four of them `break1`..`break4` and they become random variants.

  `sounds.json` still works, in Minecraft's format, when you want per-sound
  `volume`, `pitch` and `weight`.

### Fixed
- **The game would not start if it was installed in a deeply nested folder.**
  Scanning a resource pack walks its folders, and the standard library call that
  does it throws once a path passes Windows' 260-character limit — from inside a
  loop that looks incapable of throwing, with nothing to catch it. The result was
  a launch that died instantly and silently. Packs are scanned during startup, so
  this arrived with them. Deep paths now degrade instead: whatever is reachable
  loads, and the Resource Packs screen says the paths are too long rather than
  claiming the files are missing.
- **Most real Minecraft sound packs only replaced a handful of sounds.** A pack's
  own `sounds.json` lists only the events whose *definition* it changes; to change
  how something sounds, the usual and far simpler thing is to drop a replacement
  `.ogg` at the path Minecraft already uses and ship no entry at all — because
  Minecraft's own `sounds.json`, inside the game jar, still supplies the mapping.
  Hollowreach had no equivalent, so those files were invisible: one tested pack
  defines 140 events but ships 662 files, and only 21 of them landed. The game now
  carries that default table — `dig/stone1` for a stone break, `step/grass1` for a
  footstep, `mob/cow/say1` for a cow, `random/pop` for a pickup — and the same pack
  now fills 69 of 78 events, farm animals included.
- **settings.json could be written corrupt, losing every setting in it.** A row
  that only performs an action — "Locate Nearest Dungeon" — was being saved as if
  it held a value, and since it holds none the file came out with a key, a colon
  and nothing after it. That is not valid JSON, so the next launch failed to read
  the file and quietly fell back to defaults: field of view, volumes, sensitivity,
  all of it. Present since 2.9.0 and easy to miss, because the file still looks
  almost right if you open it.

### Notes for pack authors
- Packs are **folders, not `.zip` files** for now — unzip a downloaded one into
  `data/resourcepacks/`.
- Clips are mixed down to **mono** (the 3D panner needs one channel) and must be
  under 30 seconds. No `.mp3` or `.flac`; the game says so by name if you try.
- `"replace"` defaults to **true** here and to false in Minecraft, so that the
  pack you put on top actually wins instead of having its sounds shuffled in with
  the pack below. With one pack installed there is no difference; set
  `"replace": false` for Minecraft's behaviour.
- **Textures in a pack are detected and counted but not applied yet.** The screen
  says so on the row rather than pretending the pack is fully loaded.

## [2.10.1] - 2026-08-08

> **Multiplayer needs both machines on this version.** A world saved while flight
> was allowed still says so, and a 2.10.0 guest would read that and go on flying
> around a survival world its host no longer grants flight in. Same key, different
> meaning — so the two refuse each other at the join screen. **Saves are fine in
> both directions.**

### Changed
- **Flight is gone from survival.** It was a leftover from the web build and the
  one "cheat" that changes how the whole game is played rather than adding a tool
  — and a double-tap of Space is easy enough to do by accident while jumping that
  people met it without meaning to. It lives in creative now, as its own switch.
  The Cheats tab is empty in a survival world and simply does not appear; it stays
  wired up for whatever goes in it next.

### Fixed
- **Double-tap to fly needs an actual double tap.** Any two presses toggled it,
  however far apart — and sometimes one press did, depending on how long the last
  frame happened to be. The tap was being read once per physics step rather than
  once per frame, so on a busy frame the second step saw the same press, measured
  the gap from itself as zero, and called it a double tap. One tap now means one
  jump, whatever the frame rate is doing. The press that completes a double tap is
  also spent rather than starting the next one, so three quick presses no longer
  land you back where you began.
- **Instant break breaks one block at a time.** It was taking a block per frame
  while the button was down, so the shortest click took four or five out of a wall.
  It now clears about seven a second: instant on the block you aimed at, and slow
  enough to steer.

## [2.10.0] - 2026-08-08

> **Multiplayer needs both machines on this version.** Creative gains a rule an
> older build has never heard of, and world rules travel as text — so a 2.9.0
> guest would drop it and play by its own defaults with neither end told. The two
> refuse each other at the join screen. **Saves are fine in both directions.**

### Added
- **A real creative menu.** In a creative world the recipe book becomes an item
  picker: every item in the game, once each, with the same tabs and search, and a
  click takes a stack. It goes back to being a recipe book the moment creative is
  switched off.
- **Break blocks instantly**, as its own creative switch. Blocks come out on the
  first frame and drop nothing — the point is clearing space to build in, and
  anyone who wanted the block can take one from the menu. The "you need a stone
  pickaxe" refusal goes quiet with it, because being told that about a block you
  just removed anyway is noise.

### Fixed
- **The debug tools actually appear.** They were reachable only from inside a
  world, defaulted off in every world separately, and vanished along with the
  whole Debug tab — which is what "the light view and mob paths do nothing" meant.
  They were filed as one of the world's rules, and they are not: none of them
  changes a single thing about the game, only how it is drawn for the one person
  looking. They now sit in their own tab, work from the main menu, and follow you
  between worlds. Each overlay switches on its own.
- **A world's rules apply the moment you enter it**, rather than on the first
  frame you spend actually playing. Opening a world straight into a screen left
  flight, invulnerability and the creative menu itself wrong for as long as you
  stood in that menu.
- **Creative no longer flies whether you asked or not.** It forced flight on while
  the Allow Flight switch sat there reading Off, which is worse than having no
  switch. Every creative rule is now exactly what its own row says. The one
  exception is walking through walls, which has to fly: a body that never collides
  never lands.
- **Walking through walls is the right way round.** Solid rock came out
  see-through and open caves came out filled in — the exact opposite of what every
  other game does, and of what is useful. Every surface underground is the wall of
  some opening, and the fix is which side of it you are looking at: walls facing
  you are the near side of somewhere you could fly into and are lit; walls facing
  away are the far side of the mass behind you and are pushed down into it.

## [2.9.0] - 2026-08-08

> **Your existing worlds keep the ground they have until you say otherwise.**
> Hollowreach does not store terrain. It keeps everything you have built, mined
> and placed, and regrows the landscape from the world's seed every time you open
> it — so a world remembers which version of the generator made it, and only ever
> uses that one. Dungeons therefore do not appear in a world made before this
> release until you update it, which the world list now offers to do, keeping a
> copy of the world as it is first. Worlds you make from now on have them
> immediately. **Saves still load in both directions.**
>
> **Multiplayer needs both machines on this version.** A world's rules travel as
> text, so an older build does not fail to read the new ones — it drops them and
> plays by its own defaults instead. In a creative world that guest would take
> fall damage, drown, and be unable to fly while everybody else was invulnerable,
> with neither end told they disagreed. So the two refuse each other at the join
> screen.

### Added
- **Dungeons — the first real structure.** Brick chambers buried well
  underground, joined by corridors, with an Evil Altar standing in the largest
  room and a chest in each of the others. Each one bores a tunnel out to the
  nearest real cave, so you find them by following a cave system down rather than
  by mining blindly and hoping — and the altar is still breeding monsters when you
  arrive. They are completely unlit, and that is the point rather than an
  oversight: an altar only spawns anything in pitch dark, so the moment you light
  the room properly you have turned the dungeon off. Deciding when to do that is
  the fight.
- **Loot, and the tables behind it.** Dungeon chests hold something worth the
  walk — fuel, ore, food, and now and then a tool a tier above whatever got you
  down there. The chest in the altar's own room draws from a better table, and is
  the only place aetherite turns up without mining for it. What a given chest
  holds is decided by the world and the spot it stands on, so it is the same for
  everyone playing that world, it does not change if you leave and come back, and
  a chest you emptied stays empty.
- **Worlds tell you which generator made them.** Any world in the list built by
  an older one is marked, with an Update button beside it. Updating explains what
  it is about to do in plain words, takes a copy of the world first, and only then
  moves it — so the version you have been playing is always still there under its
  own name. You can also take a copy and change nothing, or go on playing the
  world exactly as it is. Nothing is forced, and nothing stops working if you
  never touch it.
- **Creative worlds.** Chosen when you make a world, and only then: a world made
  Creative can be switched between creative and survival whenever you like, and a
  world made Survival stays survival forever. In creative you fly, nothing can hurt
  you, you can walk through the ground — and while you are inside it you can see
  where you are going instead of a black screen — and clicking a recipe's result in
  the book hands you one. The book only lists things that have a recipe, so raw ore
  and mob drops are not reachable this way yet.
- **Debug tools.** A switch in Cheats reveals a Debug tab with a set of overlays:
  surface light as a colour ramp, block light and skylight separately, the ambient
  occlusion and sun shadow terms that were previously only reachable from the
  command line, mob paths drawn as you watch them walk, chunk borders, and entity
  hitboxes. A partial path — one the planner gave up on — is drawn in a different
  colour from a complete one, because "walking into a wall" and "never had a route"
  look identical from outside.
- **Locate the nearest dungeon**, in a creative world: it prints the coordinates and
  pins them on the Atlas.

### Changed
- **Sparkstone glows red**, which is the colour it looks. It glowed cyan over a red
  tile, because the texture colour and the light colour are two different numbers
  and only one of them had ever been chosen.
- **Diamond ore glows**, faintly and blue. Dimmer than sparkstone on purpose: it is
  the rarest thing down there and a lamp would give away every vein from across a
  cavern.

### Fixed
- **Nothing is deleted without asking.** A world, a screenshot and an Atlas waypoint
  all vanished on the first click. All three now ask, and name the thing they are
  about to remove rather than saying "are you sure" about nothing in particular.


## [2.8.0] - 2026-08-05

> **Multiplayer needs both machines on this version.** Boats now belong to
> whoever is sitting in them, which is a different answer to the same question an
> older build asks — so the two are refused at the join screen rather than allowed
> to disagree about it. **Saves are fine in both directions.**

### Fixed
- **A guest no longer loses the world after half an hour.** Mobs would stop dead,
  the host would stop moving, and nothing brought them back — while blocks kept
  working perfectly, which is what made it so hard to place. Position updates go
  on a channel that is meant to drop packets rather than resend them, but any
  update too big for a single message was quietly being sent the careful way
  instead: acknowledged, retransmitted and kept in order. Twenty of those a second
  is more than the connection can carry, and once it fell behind it stayed behind.
  A busy world crossed that size in about forty entities, so every session got
  there eventually.
- **The world a guest is shown is the world around them.** The host used to send
  every entity it owned to everybody, oldest first, and stop at five hundred. A
  dropped item in an unloaded chunk never ages away, so a long session built up a
  queue of frozen items that used up the whole message before it reached the animal
  standing next to you — and anything left out of the message is understood to have
  died. That is the other half of mobs vanishing, and it got worse the longer you
  played. Each person is now told about what is near them, nearest first.
- **A chest a guest puts down is a real chest.** It reached the host as a block but
  never as a container, so opening it was refused with "Nothing there", everything
  put in it was quietly discarded, and breaking it dropped nothing. The guest's own
  screen showed the contents the whole time — it was reading its own copy — which
  is why it looked like a chest that worked and then stopped. Chests already
  standing in a world this happened to are repaired the next time somebody opens
  one, rather than being left broken forever.
- **A container the host destroys stops existing for the guest too.** It used to be
  left behind at that position, invisible and still holding things, and hand them
  to the next block placed there.
- **Boats work for a guest.** They were asked for by an id no host could match, so
  every attempt to get in one was refused. A boat is now steered by whoever is
  sitting in it and everyone else is shown where it went — including the host, who
  previously would have been dragged into the seat and made to drive.
- **A chest is not locked shut by somebody who has left.** If a guest disconnected
  with one open, it stayed reserved for them forever and told everybody else that
  somebody else had it open.
- **A container screen a guest is not allowed closes** instead of letting them
  spend a minute sorting something the host will never keep.
- **Opening a chest no longer grabs half a stack on the way in.** Right clicking a
  container opened it and, in the same instant, split whatever stack sat under the
  pointer — so you arrived at the chest already holding something you never picked
  up. The pointer is hidden while you are playing but it does not move, so the
  click went to wherever you had last left it in a menu, which is why it always
  seemed to pick the same slot. Screens now take a moment to become clickable, and
  a button you were already holding when one opened has to be let go before it
  counts. Present since the first inventory screen.
- **A world stops collecting dropped items forever.** Items left in ground you have
  walked away from wait for you rather than ageing away, which is on purpose and has
  not changed — but nothing ever bounded the pile, and it was saved with the world,
  so every session began with the last one's leftovers and added to it. That growing
  pile is what made the problem above outlive quitting and rejoining. A world now
  keeps at most a thousand of them and forgets the very oldest first. You would have
  to abandon a thousand stacks in places you never return to before it takes
  anything, and walking back for your things still works however long you take.

## [2.7.0] - 2026-08-05

> **Multiplayer needs both machines on this version.** A guest is now told what a
> dropped item actually is, and both ends stamp their position updates so a packet
> that arrives late can be recognised as late — neither of which an older build can
> read. **Saves are fine in both directions.**

### Added
- **Everyone shows up on the Atlas.** Other players in the world are drawn on the
  full map and the corner minimap as a coloured arrow pointing the way they are
  facing, with their name beside it on the big map. Off the edge of the minimap
  they clamp to the rim, the way waypoints do, so the arrow still points you at
  them. Each person keeps the same colour for everybody, in every session.

### Fixed
- **The world no longer stops when somebody opens a screen.** Opening the
  inventory, the map or the pause menu used to halt the entire simulation — which
  is right when you are on your own and the game is waiting for you, and quite
  wrong when there are other people in it. The host glancing in a chest froze every
  mob mid-stride, hung every dropped item in the air, stopped every forge and left
  the guests looking at a photograph. In a shared world nothing pauses now: mobs
  move, items fall, furnaces smelt, the sun goes on setting, and your own body
  keeps falling and drowning while you are reading a menu. Single player pauses
  exactly as it always did.
- **Dropped items look like what they are.** Every item lying on the ground in
  somebody else's world was drawn as the same anonymous grey cube — ore, tools,
  bread, all identical — because a guest was told where a drop was and how many
  of it there were, and never what it was.
- **Guests can pick things up.** Anything the host owned — your own death scatter
  most of all — could be walked over all day and never collected, because only the
  host was running the code that picks a drop up. Standing on a drop now collects
  it whoever you are. Mined blocks also stop flying across the world into the
  host's pockets: an item that vacuums up from any distance is a kindness in a
  world of one and a theft in a world of two.
- **No more animals only one player can see.** A joining guest was handed the
  host's list of mobs and quietly built its own private copy of every one, which
  then wandered off under its own AI beside the ghost of the real animal — visible
  to nobody else, and impossible to kill, because the thing you were swinging at
  had never existed for anyone but you. They accumulated for as long as the session
  ran, which is most of why things drifted further apart the longer you played.
- **Respawning takes you to your spawn point.** A guest who died more than about a
  dozen blocks from where they should wake up was dragged straight back to the spot
  they had just died on, standing in their own dropped things. The host was reading
  the respawn as a speed hack and undoing it.
- **A Soul Anchor a guest binds is remembered.** The host had never been told about
  it, so a guest's bound spawn lasted exactly until they left the world.
- **Players stop appearing at world spawn.** For the moment between learning that
  somebody had joined and learning where they were, a body was drawn at the world
  origin — buried at bedrock, near the spot most worlds call home. Nobody is drawn
  now until there is somewhere to draw them.
- **Late packets are recognised as late.** Positions travel on a channel that does
  not promise to keep them in order, and an update that overtook a newer one was
  being believed: bodies snapped backwards, picked-up items reappeared, and mobs
  that had just spawned blinked out. Both ends now number what they send and ignore
  anything they have already passed.

## [2.6.0] - 2026-08-04

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

[Latest]: https://github.com/Anonymous-Floof/Hollowreach/compare/v2.11.0...HEAD
[2.11.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.11.0
[2.10.1]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.10.1
[2.10.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.10.0
[2.9.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.9.0
[2.8.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.8.0
[2.7.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.7.0
[2.6.0]: https://github.com/Anonymous-Floof/Hollowreach/releases/tag/v2.6.0
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
