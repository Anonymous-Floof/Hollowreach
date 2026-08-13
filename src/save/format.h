// The world save format, version 1.
//
// The web build wrote JSON (js/save/serialize.js). This is binary, because the
// decision was taken up front that existing `worlds/*.json` are not loaded — there
// is no old format to stay compatible with, and the one thing a save is mostly made
// of, the per-cell edit list, is 5 bytes here against roughly 30 characters there.
//
// Two properties are load-bearing and worth stating before the layout:
//
// **Blocks are stored by their string key, never by numeric id.** A world holds
// the palette it was written with; loading maps each key back through the block
// registry. Reordering, inserting or removing a block between builds therefore
// cannot silently turn every chest in a world into a furnace. Unknown keys become
// air, which is what js/save/serialize.js:63's `BLOCK[blockKey] ?? AIR` did.
//
// **Nothing is trusted.** ByteReader fails closed on the first read past the end,
// counts are checked against the bytes actually remaining, sections carry their own
// length so a corrupt one cannot swallow the rest of the file, and the payload
// carries a CRC. A truncated, garbage or hostile file produces a decode failure and
// a message, never a crash and never a half-loaded world.
//
// ---- layout ----------------------------------------------------------------
//
//   header    "HRWORLD\0"  u16 version  u16 flags  u32 payloadBytes  u32 payloadCrc
//   payload   a sequence of sections, each:  u32 tag  u32 length  bytes[length]
//
// Sections are self-delimiting and dispatched by tag, so a section can be added,
// reordered or (with a migration) dropped without touching the reader's structure;
// an unknown tag is skipped with a warning rather than failing the load. What that
// does NOT survive is a *changed* section layout, which is what the version field
// is for — see save/migrate.h.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/mat4.h"
#include "game/blockentities.h"
#include "game/entities/manager.h"
#include "game/inventory.h"
#include "game/player.h"
#include "world/chunk.h"
#include "world/world.h"

namespace hr::save {

// Bumped whenever a section's layout changes. Adding a whole new section does not
// need a bump, because an old reader skips what it does not know.
// 2: the Dye update. ItemStack gained a colour, and every stack in the file goes
// through writeStack/readStack — so this is a layout change inside sections that
// already exist, which is the one thing a new tag cannot work around.
inline constexpr std::uint16_t kSaveVersion = 2;

// The oldest version this build can still read. Migrations cover everything from
// here up to kSaveVersion.
inline constexpr std::uint16_t kMinReadableVersion = 1;

struct WorldMeta {
  std::string id;    // "w" + base36 timestamp + random, as in js/save/storage.js:14
  std::string name;  // what the player typed
  std::uint32_t seed = 0;
  std::int32_t genVersion = 2;
  // Unix seconds. The web build used Date.now() milliseconds; seconds are plenty
  // for "saved 3m ago" and keep the field readable in a hex dump.
  std::int64_t createdAt = 0;
  std::int64_t savedAt = 0;
  // Which build last wrote this file. Diagnostics only — never gated on, exactly as
  // js/save/serialize.js:26 says.
  std::string gameVersion;
  float time = 0.32f;  // the sky clock

  // The player-set spawn point (a bound Soul Anchor). Absent means "derive the
  // default from the terrain", which is why it is optional rather than zeroed.
  bool hasSpawn = false;
  Vec3 spawn;
};

struct WaypointSave {
  float x = 0, y = 64, z = 0;
  std::string name;
  std::uint32_t color = 0;  // packed RGBA, high byte red
  bool death = false;
};

// One guest's progress in this world, kept by the host so a friend rejoining
// picks up where they left off. The web build carried this as `remotePlayers` in
// its save (js/save/serialize.js:47); it is a new section here, which is why it
// needed no version bump.
struct GuestSave {
  std::string playerId;
  std::string name;
  game::PlayerState player;
  game::Inventory inventory;
  bool hasSpawn = false;
  Vec3 spawn;
};

struct WorldSave {
  WorldMeta meta;
  game::PlayerState player;
  game::Inventory inventory;
  world::World::EditMap edits;
  std::vector<world::ChunkKey> explored;
  std::unordered_map<game::BlockEntityKey, game::BlockEntity> blockEntities;
  // A section of its own, added after the format was frozen. That is the whole
  // reason paintings are not block entities: a NEW tag costs no version bump,
  // because an older build skips one it does not recognise and loads the rest of
  // the world — where changing the layout of the block-entity section would have
  // made every existing save unreadable without a migration.
  std::unordered_map<game::BlockEntityKey, game::Painting> paintings;
  // Dyed cells, position -> 0xRRGGBB. Also its own section, and for the plainest
  // version of the same reason: an older build skips the tag and opens the world
  // with its dyed blocks back at neutral, which is a world it can still play.
  world::World::TintMap tints;
  // The colours the player pressed "save in this world" on. A separate list from
  // the tints above, and separate on purpose: those are colours that ARE somewhere
  // in the world, these are colours somebody wants to reach for again. Deleting a
  // dyed wall must not take the swatch with it.
  std::vector<std::uint32_t> paletteFavourites;
  std::vector<game::EntitySave> entities;
  std::vector<WaypointSave> waypoints;
  std::vector<GuestSave> guests;
  // Game hours the player has been awake, for the bed's tiredness gate. Its own
  // section, so a world written by an older build simply loads at zero — well
  // rested, and unable to sleep for the first eight hours, which is the harmless
  // reading of "we do not know".
  float hoursAwake = 0.0f;
  // The world's own rules: difficulty and cheats, as key/value text pairs. Its own
  // section for the same reason the two above are — a new tag costs nothing and an
  // older build skips it, loading a world whose rules are simply the defaults.
  //
  // Text rather than a struct of typed fields on purpose. The settings schema is a
  // table that grows, and a struct here would mean this file had to learn every row
  // added to it; a pair list means a new world-scoped setting is one schema row and
  // nothing else. Unknown keys are dropped on load, so a world written by a newer
  // build opens in an older one with the settings it still understands.
  std::vector<std::pair<std::string, std::string>> worldSettings;
  // What this world was made as, decided once and never again. A world created
  // Creative may switch between creative and survival freely; one created Survival
  // never can, which is what makes the choice on the New World screen mean
  // something. Its own section, so no version bump — see kTagWorldMode.
  bool createdCreative = false;
};

// Encodes to bytes. Deterministic: every map is written in sorted key order, so
// saving the same world twice produces byte-identical files. That is what makes
// "save, reload, save again, compare" a real round-trip test rather than a
// field-by-field one that can miss whatever it forgot to compare.
// Copies everything a World contributes to a save.
//
// Extracted out of App::saveWorld so it can be TESTED. The list is easy to add to
// and easy to forget, and forgetting one line is completely silent: `tints` was
// missing from it for most of the Dye update, so every dyed world saved its blocks
// and none of their colours, and nothing in the suite could see it — the save tests
// build a WorldSave by hand and never go near the assembly. Sabotage said so out
// loud: deleting the line changed no test.
void captureWorld(const world::World& world, WorldSave& out);

std::vector<std::uint8_t> encode(const WorldSave& save);

// Decodes, running any migrations needed. Returns false and fills `error` on a bad
// magic, an unreadable version, a length or CRC mismatch, or a truncated section.
bool decode(const std::uint8_t* data, std::size_t size, WorldSave& out, std::string* error);

// Just the header and the META section, for the world list — a listing of forty
// worlds should not decode forty edit maps.
bool decodeMeta(const std::uint8_t* data, std::size_t size, WorldMeta& out, std::string* error);

// The version a file claims, or 0 if it is not one of ours. Used by the migration
// scaffold and by --save-info.
std::uint16_t peekVersion(const std::uint8_t* data, std::size_t size);

}  // namespace hr::save
