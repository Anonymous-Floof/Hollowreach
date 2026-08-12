#include "save/format.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "core/bytes.h"
#include "core/log.h"
#include "save/migrate.h"
#include "world/blocks.h"

namespace hr::save {
namespace {

constexpr char kMagic[8] = {'H', 'R', 'W', 'O', 'R', 'L', 'D', '\0'};
constexpr std::size_t kHeaderBytes = 8 + 2 + 2 + 4 + 4;

constexpr std::uint32_t makeTag(char a, char b, char c, char d) {
  return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8) |
         (static_cast<std::uint32_t>(c) << 16) | (static_cast<std::uint32_t>(d) << 24);
}

constexpr std::uint32_t kTagMeta = makeTag('M', 'E', 'T', 'A');
constexpr std::uint32_t kTagPlayer = makeTag('P', 'L', 'Y', 'R');
constexpr std::uint32_t kTagInventory = makeTag('I', 'N', 'V', 'T');
constexpr std::uint32_t kTagPalette = makeTag('P', 'A', 'L', 'T');
constexpr std::uint32_t kTagEdits = makeTag('E', 'D', 'I', 'T');
constexpr std::uint32_t kTagBlockEntities = makeTag('B', 'E', 'N', 'T');
constexpr std::uint32_t kTagEntities = makeTag('E', 'N', 'T', 'S');
constexpr std::uint32_t kTagExplored = makeTag('E', 'X', 'P', 'L');
constexpr std::uint32_t kTagWaypoints = makeTag('W', 'A', 'Y', 'P');
constexpr std::uint32_t kTagGuests = makeTag('R', 'P', 'L', 'R');
constexpr std::uint32_t kTagPaintings = makeTag('P', 'A', 'N', 'T');
// How long the player has been awake. A section of its own for the same reason
// paintings got one: it belongs with the player, and the player section's layout
// is frozen without a version bump and a migration. An older build skips this tag
// and loads a world whose owner is simply well rested.
constexpr std::uint32_t kTagRest = makeTag('R', 'E', 'S', 'T');
// The world's own difficulty and cheat settings. A new tag, so an older build
// skips it and opens the world under the defaults.
constexpr std::uint32_t kTagWorldSettings = makeTag('W', 'S', 'E', 'T');
// What the world was CREATED as, which is not the same question as what mode it is
// being played in right now. The current mode is an ordinary world setting the host
// can flip; this is the thing that decides whether they are allowed to, and nothing
// can change it after the world is made.
//
// Its own tag rather than a WorldMeta field, which would have frozen the META
// layout and cost a save version bump and a migration. An older build skips it, and
// a save written before this existed has no such section — which reads as Survival,
// the honest answer to "we do not know" and the right one for every world that
// already exists.
constexpr std::uint32_t kTagWorldMode = makeTag('W', 'M', 'O', 'D');
// The kitchen stations, in a section of their own rather than alongside the forges
// and chests in BENT.
//
// THE REASON IS A REAL TRAP, not tidiness. decodeBlockEntities below hits a `kind`
// it does not recognise and calls fail(), because entity payloads carry no length
// and there is no way to step over one. Putting a cooking pot in that section would
// therefore be a LAYOUT change: an older build would lose every entity sorted after
// the first pot — chests included — rather than merely not seeing the pot.
//
// So: a new tag, which an older build skips whole, AND a length prefix on every
// entry from the start. The length is what makes this section permanent — every
// future block-entity kind can live here without a version bump, because a reader
// that does not know a kind can seek past it and carry on. BENT could not do that
// and now never has to.
constexpr std::uint32_t kTagStations = makeTag('S', 'T', 'A', 'T');

// ---- shared field encoders -------------------------------------------------

void writeVec3(ByteWriter& w, const Vec3& v) {
  w.f32(v.x);
  w.f32(v.y);
  w.f32(v.z);
}
Vec3 readVec3(ByteReader& r) {
  Vec3 v;
  v.x = r.f32();
  v.y = r.f32();
  v.z = r.f32();
  return v;
}

// An item stack. An empty slot is an empty key, which is how ItemStack already
// spells it — the web build used a null and had to test for it on both sides.
void writeStack(ByteWriter& w, const game::ItemStack& s) {
  if (s.empty()) {
    w.str("");
    w.i32(0);
    w.i32(-1);
    return;
  }
  w.str(s.key);
  w.i32(s.count);
  w.i32(s.dura);
}
game::ItemStack readStack(ByteReader& r) {
  game::ItemStack s;
  s.key = r.str();
  s.count = r.i32();
  s.dura = r.i32();
  if (s.key.empty() || s.count <= 0) s.clear();
  return s;
}

void writeFsm(ByteWriter& w, const std::vector<game::FsmRecord>& records) {
  w.u16(static_cast<std::uint16_t>(std::min<std::size_t>(records.size(), 0xFFFFu)));
  for (const game::FsmRecord& rec : records) {
    w.str(rec.slot);
    w.str(rec.state);
    w.f32(rec.timeIn);
    const auto& values = rec.bb.values();
    w.u16(static_cast<std::uint16_t>(std::min<std::size_t>(values.size(), 0xFFFFu)));
    for (const auto& [name, value] : values) {
      w.str(name);
      w.f32(value);
    }
  }
}
void readFsm(ByteReader& r, std::vector<game::FsmRecord>& out) {
  const std::size_t count = r.u16();
  // Six bytes is the smallest a record can be (two empty strings, a float, a
  // count), so a claim larger than the bytes left is corruption.
  if (count * 6 > r.remaining()) {
    r.fail();
    return;
  }
  out.reserve(count);
  for (std::size_t i = 0; i < count && r.ok(); ++i) {
    game::FsmRecord rec;
    rec.slot = r.str();
    rec.state = r.str();
    rec.timeIn = r.f32();
    const std::size_t values = r.u16();
    if (values * 6 > r.remaining()) {
      r.fail();
      return;
    }
    for (std::size_t k = 0; k < values && r.ok(); ++k) {
      const std::string name = r.str();
      rec.bb.set(name, r.f32());
    }
    out.push_back(std::move(rec));
  }
}

// ---- sections --------------------------------------------------------------

void encodeMeta(ByteWriter& w, const WorldMeta& m) {
  w.str(m.id);
  w.str(m.name);
  w.u32(m.seed);
  w.i32(m.genVersion);
  w.i64(m.createdAt);
  w.i64(m.savedAt);
  w.str(m.gameVersion);
  w.f32(m.time);
  w.boolean(m.hasSpawn);
  writeVec3(w, m.spawn);
}

void decodeMeta(ByteReader& r, WorldMeta& m) {
  m.id = r.str();
  m.name = r.str();
  m.seed = r.u32();
  m.genVersion = r.i32();
  m.createdAt = r.i64();
  m.savedAt = r.i64();
  m.gameVersion = r.str();
  m.time = r.f32();
  m.hasSpawn = r.boolean();
  m.spawn = readVec3(r);
  if (!(m.time >= 0.0f && m.time <= 1.0f)) m.time = 0.32f;
}

void encodePlayer(ByteWriter& w, const game::PlayerState& p) {
  writeVec3(w, p.pos);
  w.f32(p.yaw);
  w.f32(p.pitch);
  w.f32(p.health);
  w.f32(p.hunger);
  w.f32(p.saturation);
  w.boolean(p.flying);
}

void decodePlayer(ByteReader& r, game::PlayerState& p) {
  p.pos = readVec3(r);
  p.yaw = r.f32();
  p.pitch = r.f32();
  p.health = r.f32();
  p.hunger = r.f32();
  p.saturation = r.f32();
  p.flying = r.boolean();
}

void encodeInventory(ByteWriter& w, const game::Inventory& inv) {
  // The slot counts are written rather than assumed, so a build that widens the
  // inventory reads an old save as a short one instead of misparsing it.
  w.u16(static_cast<std::uint16_t>(game::kInventorySlots));
  for (const game::ItemStack& s : inv.slots()) writeStack(w, s);
  w.u16(static_cast<std::uint16_t>(game::kArmorSlots));
  for (const game::ItemStack& s : inv.armor()) writeStack(w, s);
  w.u16(static_cast<std::uint16_t>(inv.selected()));
}

void decodeInventory(ByteReader& r, game::Inventory& inv) {
  const std::size_t slots = r.u16();
  if (slots * 10 > r.remaining()) {
    r.fail();
    return;
  }
  for (std::size_t i = 0; i < slots && r.ok(); ++i) {
    const game::ItemStack s = readStack(r);
    if (i < game::kInventorySlots) inv.slots()[i] = s;
  }
  const std::size_t armor = r.u16();
  if (armor * 10 > r.remaining()) {
    r.fail();
    return;
  }
  for (std::size_t i = 0; i < armor && r.ok(); ++i) {
    const game::ItemStack s = readStack(r);
    if (i < game::kArmorSlots) inv.armor()[i] = s;
  }
  inv.setSelected(static_cast<int>(r.u16()));
}

// The block palette. Built while the edits are encoded, so it holds exactly the
// keys a world's edits use — usually a couple of dozen, against the ~190 blocks in
// the registry.
class Palette {
 public:
  std::uint16_t intern(world::BlockId id) {
    auto it = byId_.find(id);
    if (it != byId_.end()) return it->second;
    const auto index = static_cast<std::uint16_t>(keys_.size());
    keys_.push_back(world::BlockRegistry::get().def(id).key);
    byId_.emplace(id, index);
    return index;
  }
  const std::vector<std::string>& keys() const { return keys_; }

 private:
  std::vector<std::string> keys_;
  std::unordered_map<world::BlockId, std::uint16_t> byId_;
};

void encodeEdits(ByteWriter& w, const world::World::EditMap& edits, Palette& palette) {
  // Sorted, so two saves of the same world are byte-identical. An unordered_map's
  // iteration order is unspecified and in practice moves with the allocator.
  std::vector<world::ChunkKey> chunkKeys;
  chunkKeys.reserve(edits.size());
  for (const auto& [key, cells] : edits) {
    if (!cells.empty()) chunkKeys.push_back(key);
  }
  std::sort(chunkKeys.begin(), chunkKeys.end());

  w.u32(static_cast<std::uint32_t>(chunkKeys.size()));
  std::vector<int> cellIndices;
  for (const world::ChunkKey key : chunkKeys) {
    const auto& cells = edits.at(key);
    w.i32(world::keyCx(key));
    w.i32(world::keyCz(key));
    w.u32(static_cast<std::uint32_t>(cells.size()));

    cellIndices.clear();
    cellIndices.reserve(cells.size());
    for (const auto& [index, packed] : cells) cellIndices.push_back(index);
    std::sort(cellIndices.begin(), cellIndices.end());

    for (const int index : cellIndices) {
      const std::uint32_t packed = cells.at(index);
      w.u16(static_cast<std::uint16_t>(index));
      w.u16(palette.intern(static_cast<world::BlockId>(packed & 0xFFFFu)));
      w.u8(static_cast<std::uint8_t>((packed >> 16) & 0xFFu));
    }
  }
}

void decodeEdits(ByteReader& r, const std::vector<world::BlockId>& palette,
                 world::World::EditMap& out) {
  const std::uint32_t chunkCount = r.readCount();
  for (std::uint32_t c = 0; c < chunkCount && r.ok(); ++c) {
    const int cx = r.i32();
    const int cz = r.i32();
    const std::uint32_t cellCount = r.u32();
    // Five bytes a cell, so anything larger than the bytes left is a lie.
    if (!r.ok() || cellCount > r.remaining() / 5) {
      r.fail();
      return;
    }
    auto& cells = out[world::chunkKey(cx, cz)];
    cells.reserve(cellCount);
    for (std::uint32_t i = 0; i < cellCount; ++i) {
      const std::uint16_t index = r.u16();
      const std::uint16_t paletteIndex = r.u16();
      const std::uint8_t meta = r.u8();
      if (index >= world::kCellsPerChunk) continue;  // out of range: drop the cell
      const world::BlockId id =
          paletteIndex < palette.size() ? palette[paletteIndex] : world::kAir;
      cells[index] = static_cast<std::uint32_t>(id) | (static_cast<std::uint32_t>(meta) << 16);
    }
  }
}

void encodeBlockEntities(
    ByteWriter& w, const std::unordered_map<game::BlockEntityKey, game::BlockEntity>& map) {
  std::vector<game::BlockEntityKey> keys;
  keys.reserve(map.size());
  for (const auto& [key, be] : map) {
    // Kitchens are deliberately NOT written here. This section cannot describe a
    // kind an older reader does not know — see kTagStations above — so letting one
    // through would corrupt every entity after it for anyone on an older build.
    if (be.kind != game::BlockEntityKind::None && !game::isKitchen(be.kind)) {
      keys.push_back(key);
    }
  }
  std::sort(keys.begin(), keys.end());

  w.u32(static_cast<std::uint32_t>(keys.size()));
  for (const game::BlockEntityKey key : keys) {
    const game::BlockEntity& be = map.at(key);
    int x = 0, y = 0, z = 0;
    game::unpackBlockEntityKey(key, x, y, z);
    w.i32(x);
    w.i32(y);
    w.i32(z);
    w.u8(static_cast<std::uint8_t>(be.kind));
    if (be.kind == game::BlockEntityKind::Forge) {
      writeStack(w, be.input);
      writeStack(w, be.fuel);
      writeStack(w, be.output);
      w.f32(be.fuelLeft);
      w.f32(be.fuelMax);
      w.f32(be.progress);
    } else {
      w.u16(static_cast<std::uint16_t>(be.slots.size()));
      for (const game::ItemStack& s : be.slots) writeStack(w, s);
    }
  }
}

// The kitchen stations. Every entry carries its own byte length, which is the whole
// reason this section exists and the one thing BENT cannot do.
void encodeStations(
    ByteWriter& w, const std::unordered_map<game::BlockEntityKey, game::BlockEntity>& map) {
  std::vector<game::BlockEntityKey> keys;
  for (const auto& [key, be] : map) {
    if (game::isKitchen(be.kind)) keys.push_back(key);
  }
  // Sorted for the same reason BENT sorts: two saves of the same world have to come
  // out byte-identical, and an unordered_map promises no order between runs.
  std::sort(keys.begin(), keys.end());

  w.u32(static_cast<std::uint32_t>(keys.size()));
  for (const game::BlockEntityKey key : keys) {
    const game::BlockEntity& be = map.at(key);
    int x = 0, y = 0, z = 0;
    game::unpackBlockEntityKey(key, x, y, z);

    ByteWriter body;
    body.u8(static_cast<std::uint8_t>(be.kind));
    writeStack(body, be.input);
    writeStack(body, be.fuel);
    writeStack(body, be.output);
    writeStack(body, be.container);
    body.f32(be.fuelLeft);
    body.f32(be.fuelMax);
    body.f32(be.progress);
    body.u16(static_cast<std::uint16_t>(be.slots.size()));
    for (const game::ItemStack& s : be.slots) writeStack(body, s);

    w.i32(x);
    w.i32(y);
    w.i32(z);
    w.u32(static_cast<std::uint32_t>(body.size()));
    w.bytes(body.data().data(), body.size());
  }
}

void decodeStations(ByteReader& r,
                    std::unordered_map<game::BlockEntityKey, game::BlockEntity>& into) {
  const std::uint32_t count = r.u32();
  if (!r.ok() || count > 65536u) {
    r.fail();
    return;
  }
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    const std::int32_t x = r.i32(), y = r.i32(), z = r.i32();
    const std::uint32_t length = r.u32();
    if (!r.ok()) return;
    const std::size_t end = r.position() + length;

    game::BlockEntity be;
    be.kind = static_cast<game::BlockEntityKind>(r.u8());
    if (game::isKitchen(be.kind)) {
      be.input = readStack(r);
      be.fuel = readStack(r);
      be.output = readStack(r);
      be.container = readStack(r);
      be.fuelLeft = r.f32();
      be.fuelMax = r.f32();
      be.progress = r.f32();
      const std::uint16_t n = r.u16();
      if (n <= game::kPotSlots) {
        be.slots.resize(n);
        for (game::ItemStack& s : be.slots) s = readStack(r);
      }
      if (r.ok()) into[game::blockEntityKey(x, y, z)] = std::move(be);
    }
    // Known or not, land exactly on the end of this entry. THIS is the line that
    // makes the section future-proof: a kind this build has never heard of costs a
    // seek rather than the rest of the world's stations.
    if (r.position() < end) r.skip(end - r.position());
  }
}

void encodePaintings(ByteWriter& w,
                     const std::unordered_map<game::BlockEntityKey, game::Painting>& map) {
  std::vector<game::BlockEntityKey> keys;
  keys.reserve(map.size());
  for (const auto& [key, art] : map) {
    if (!art.blank()) keys.push_back(key);  // a blank canvas is the absence of one
  }
  std::sort(keys.begin(), keys.end());

  w.u32(static_cast<std::uint32_t>(keys.size()));
  for (const game::BlockEntityKey key : keys) {
    const game::Painting& art = map.at(key);
    int x = 0, y = 0, z = 0;
    game::unpackBlockEntityKey(key, x, y, z);
    w.i32(x);
    w.i32(y);
    w.i32(z);
    w.str(art.source);
    // Raw pixels at a fixed size, so there is no per-painting dimension to
    // validate and a truncated file cannot claim a gigantic one.
    for (std::size_t i = 0; i < game::kPaintingBytes; ++i) w.u8(art.rgb[i]);
  }
}

void decodePaintings(ByteReader& r,
                     std::unordered_map<game::BlockEntityKey, game::Painting>& out) {
  const std::uint32_t count = r.readCount();
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    const int x = r.i32();
    const int y = r.i32();
    const int z = r.i32();
    game::Painting art;
    art.source = r.str();
    // The one thing readCount cannot cover: each entry is 48 KB, so a plausible
    // count over a short file would still reserve hundreds of megabytes one
    // painting at a time before the reader ran dry.
    if (!r.ok() || r.remaining() < game::kPaintingBytes) {
      r.fail();
      return;
    }
    art.rgb.resize(game::kPaintingBytes);
    for (std::size_t k = 0; k < game::kPaintingBytes; ++k) art.rgb[k] = r.u8();
    if (r.ok()) out[game::blockEntityKey(x, y, z)] = std::move(art);
  }
}

void decodeBlockEntities(ByteReader& r,
                         std::unordered_map<game::BlockEntityKey, game::BlockEntity>& out) {
  const std::uint32_t count = r.readCount();
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    const int x = r.i32();
    const int y = r.i32();
    const int z = r.i32();
    const auto kind = static_cast<game::BlockEntityKind>(r.u8());
    if (!r.ok()) return;

    if (kind == game::BlockEntityKind::Forge) {
      game::BlockEntity be = game::makeForge();
      be.input = readStack(r);
      be.fuel = readStack(r);
      be.output = readStack(r);
      be.fuelLeft = r.f32();
      be.fuelMax = r.f32();
      be.progress = r.f32();
      if (r.ok()) out[game::blockEntityKey(x, y, z)] = std::move(be);
    } else if (kind == game::BlockEntityKind::Chest) {
      game::BlockEntity be = game::makeChest();
      const std::size_t slots = r.u16();
      if (slots * 10 > r.remaining()) {
        r.fail();
        return;
      }
      for (std::size_t k = 0; k < slots && r.ok(); ++k) {
        const game::ItemStack s = readStack(r);
        if (k < be.slots.size()) be.slots[k] = s;
      }
      if (r.ok()) out[game::blockEntityKey(x, y, z)] = std::move(be);
    } else {
      // A kind this build does not know. There is no way to skip past it, because
      // its payload length is not encoded, so the section stops here rather than
      // reading the next entity out of the middle of this one.
      r.fail();
      return;
    }
  }
}

void encodeEntities(ByteWriter& w, const std::vector<game::EntitySave>& entities) {
  w.u32(static_cast<std::uint32_t>(entities.size()));
  for (const game::EntitySave& e : entities) {
    // By key, not by enum index — for exactly the reason blocks are: inserting a
    // type into the middle of EntityType must not turn every cow into a zombie.
    w.str(game::entityTypeKey(e.type));
    writeVec3(w, e.pos);
    writeVec3(w, e.vel);
    w.f32(e.yaw);
    w.str(e.key);
    w.i32(e.count);
    w.i32(e.dura);
    w.f32(e.despawn);
    w.boolean(e.instant);
    w.f32(e.health);
    writeFsm(w, e.fsm);
  }
}

void decodeEntities(ByteReader& r, std::vector<game::EntitySave>& out) {
  const std::uint32_t count = r.readCount();
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    game::EntitySave e;
    const std::string typeKey = r.str();
    e.type = game::entityTypeFromKey(typeKey);
    e.pos = readVec3(r);
    e.vel = readVec3(r);
    e.yaw = r.f32();
    e.key = r.str();
    e.count = r.i32();
    e.dura = r.i32();
    e.despawn = r.f32();
    e.instant = r.boolean();
    e.health = r.f32();
    readFsm(r, e.fsm);
    // An unknown type still had to be read through — the fields are fixed-layout —
    // but it is not kept. EntityManager::load would drop it anyway; dropping it
    // here means a listing or a diff does not show a phantom.
    if (r.ok() && e.type != game::EntityType::None) out.push_back(std::move(e));
  }
}

void encodeExplored(ByteWriter& w, std::vector<world::ChunkKey> keys) {
  std::sort(keys.begin(), keys.end());
  w.u32(static_cast<std::uint32_t>(keys.size()));
  for (const world::ChunkKey key : keys) {
    w.i32(world::keyCx(key));
    w.i32(world::keyCz(key));
  }
}

void decodeExplored(ByteReader& r, std::vector<world::ChunkKey>& out) {
  const std::uint32_t count = r.readCount();
  if (count > r.remaining() / 8) {
    r.fail();
    return;
  }
  out.reserve(count);
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    const int cx = r.i32();
    const int cz = r.i32();
    out.push_back(world::chunkKey(cx, cz));
  }
}

void encodeWaypoints(ByteWriter& w, const std::vector<WaypointSave>& points) {
  w.u32(static_cast<std::uint32_t>(points.size()));
  for (const WaypointSave& p : points) {
    w.f32(p.x);
    w.f32(p.y);
    w.f32(p.z);
    w.str(p.name);
    w.u32(p.color);
    w.boolean(p.death);
  }
}

void decodeWaypoints(ByteReader& r, std::vector<WaypointSave>& out) {
  const std::uint32_t count = r.readCount();
  if (count > r.remaining() / 19) {
    r.fail();
    return;
  }
  out.reserve(count);
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    WaypointSave p;
    p.x = r.f32();
    p.y = r.f32();
    p.z = r.f32();
    p.name = r.str();
    p.color = r.u32();
    p.death = r.boolean();
    if (r.ok()) out.push_back(std::move(p));
  }
}

void encodeGuests(ByteWriter& w, const std::vector<GuestSave>& guests) {
  w.u32(static_cast<std::uint32_t>(guests.size()));
  for (const GuestSave& g : guests) {
    w.str(g.playerId);
    w.str(g.name);
    encodePlayer(w, g.player);
    encodeInventory(w, g.inventory);
    w.boolean(g.hasSpawn);
    writeVec3(w, g.spawn);
  }
}

void decodeGuests(ByteReader& r, std::vector<GuestSave>& out) {
  const std::uint32_t count = r.readCount();
  if (count > 64) {
    r.fail();
    return;
  }
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    GuestSave g;
    g.playerId = r.str();
    g.name = r.str();
    decodePlayer(r, g.player);
    decodeInventory(r, g.inventory);
    g.hasSpawn = r.boolean();
    g.spawn = readVec3(r);
    if (r.ok()) out.push_back(std::move(g));
  }
}

// ---- framing ---------------------------------------------------------------

void appendSection(ByteWriter& out, std::uint32_t tag, const ByteWriter& body) {
  out.u32(tag);
  out.u32(static_cast<std::uint32_t>(body.size()));
  out.bytes(body.data().data(), body.size());
}

// Reads and validates the header, returning a reader positioned at the payload.
bool openPayload(const std::uint8_t* data, std::size_t size, std::uint16_t& version,
                 ByteReader& payload, std::string* error) {
  const auto fail = [&](const char* message) {
    if (error) *error = message;
    return false;
  };
  if (!data || size < kHeaderBytes) return fail("not a world file (too short)");

  char magic[8] = {};
  ByteReader r(data, size);
  if (!r.bytes(magic, sizeof magic) || std::memcmp(magic, kMagic, sizeof kMagic) != 0) {
    return fail("not a world file (bad magic)");
  }
  version = r.u16();
  r.u16();  // header flags, reserved
  const std::uint32_t payloadBytes = r.u32();
  const std::uint32_t payloadCrc = r.u32();
  if (!r.ok()) return fail("truncated header");

  if (version < kMinReadableVersion) return fail("save is too old for this build");
  if (version > kSaveVersion) return fail("save was written by a newer build");
  if (payloadBytes != size - kHeaderBytes) return fail("truncated or padded save file");
  if (crc32(data + kHeaderBytes, payloadBytes) != payloadCrc) return fail("save file is corrupt");

  payload = ByteReader(data + kHeaderBytes, payloadBytes);
  return true;
}

}  // namespace

std::vector<std::uint8_t> encode(const WorldSave& save) {
  // Edits are encoded first because doing so is what fills the palette, and the
  // palette section has to precede them in the file so a single-pass decoder has it
  // in hand when the edits arrive.
  Palette palette;
  ByteWriter edits;
  encodeEdits(edits, save.edits, palette);

  ByteWriter paletteBody;
  paletteBody.u16(static_cast<std::uint16_t>(palette.keys().size()));
  for (const std::string& key : palette.keys()) paletteBody.str(key);

  ByteWriter meta;
  encodeMeta(meta, save.meta);
  ByteWriter player;
  encodePlayer(player, save.player);
  ByteWriter inventory;
  encodeInventory(inventory, save.inventory);
  ByteWriter blockEntities;
  encodeBlockEntities(blockEntities, save.blockEntities);
  ByteWriter stations;
  encodeStations(stations, save.blockEntities);
  ByteWriter paintings;
  encodePaintings(paintings, save.paintings);
  ByteWriter entities;
  encodeEntities(entities, save.entities);
  ByteWriter explored;
  encodeExplored(explored, save.explored);
  ByteWriter waypoints;
  encodeWaypoints(waypoints, save.waypoints);
  ByteWriter guests;
  encodeGuests(guests, save.guests);
  ByteWriter rest;
  rest.f32(save.hoursAwake);
  ByteWriter worldSettings;
  worldSettings.u16(static_cast<std::uint16_t>(save.worldSettings.size()));
  for (const auto& [key, value] : save.worldSettings) {
    worldSettings.str(key);
    worldSettings.str(value);
  }

  ByteWriter mode;
  mode.boolean(save.createdCreative);

  ByteWriter payload;
  appendSection(payload, kTagMeta, meta);
  appendSection(payload, kTagPlayer, player);
  appendSection(payload, kTagInventory, inventory);
  appendSection(payload, kTagPalette, paletteBody);
  appendSection(payload, kTagEdits, edits);
  appendSection(payload, kTagBlockEntities, blockEntities);
  appendSection(payload, kTagStations, stations);
  appendSection(payload, kTagEntities, entities);
  appendSection(payload, kTagExplored, explored);
  appendSection(payload, kTagWaypoints, waypoints);
  appendSection(payload, kTagGuests, guests);
  appendSection(payload, kTagPaintings, paintings);
  appendSection(payload, kTagRest, rest);
  appendSection(payload, kTagWorldSettings, worldSettings);
  appendSection(payload, kTagWorldMode, mode);

  ByteWriter out;
  out.bytes(kMagic, sizeof kMagic);
  out.u16(kSaveVersion);
  out.u16(0);
  out.u32(static_cast<std::uint32_t>(payload.size()));
  out.u32(crc32(payload.data().data(), payload.size()));
  out.bytes(payload.data().data(), payload.size());
  return std::move(out.data());
}

bool decode(const std::uint8_t* data, std::size_t size, WorldSave& out, std::string* error) {
  std::uint16_t version = 0;
  ByteReader payload(nullptr, 0);
  if (!openPayload(data, size, version, payload, error)) return false;

  // Block ids for the palette's keys, resolved once. An id of air for a key this
  // build no longer has matches the web build's `BLOCK[blockKey] ?? AIR`.
  std::vector<world::BlockId> palette;
  bool sawPalette = false;

  while (payload.remaining() >= 8) {
    const std::uint32_t tag = payload.u32();
    const std::uint32_t length = payload.u32();
    if (!payload.ok() || length > payload.remaining()) {
      if (error) *error = "truncated section";
      return false;
    }
    ByteReader body = payload.sub(length);

    switch (tag) {
      case kTagMeta: decodeMeta(body, out.meta); break;
      case kTagPlayer: decodePlayer(body, out.player); break;
      case kTagInventory: decodeInventory(body, out.inventory); break;
      case kTagPalette: {
        const std::size_t count = body.u16();
        if (count * 2 > body.remaining()) {
          body.fail();
          break;
        }
        const world::BlockRegistry& blocks = world::BlockRegistry::get();
        palette.reserve(count);
        for (std::size_t i = 0; i < count && body.ok(); ++i) {
          palette.push_back(blocks.idOf(body.str()));
        }
        sawPalette = true;
        break;
      }
      case kTagEdits:
        if (!sawPalette) {
          if (error) *error = "edits before palette";
          return false;
        }
        decodeEdits(body, palette, out.edits);
        break;
      case kTagBlockEntities: decodeBlockEntities(body, out.blockEntities); break;
      // Same map: the stations rejoin the forges and chests once both sections are
      // read, so nothing downstream has to know they were stored apart.
      case kTagStations: decodeStations(body, out.blockEntities); break;
      case kTagPaintings: decodePaintings(body, out.paintings); break;
      case kTagEntities: decodeEntities(body, out.entities); break;
      case kTagExplored: decodeExplored(body, out.explored); break;
      case kTagWaypoints: decodeWaypoints(body, out.waypoints); break;
      case kTagGuests: decodeGuests(body, out.guests); break;
      case kTagRest: out.hoursAwake = body.f32(); break;
      case kTagWorldMode: out.createdCreative = body.boolean(); break;
      case kTagWorldSettings: {
        const int n = body.u16();
        for (int i = 0; i < n && body.ok(); ++i) {
          std::string key = body.str();
          std::string value = body.str();
          if (!body.ok()) break;
          out.worldSettings.emplace_back(std::move(key), std::move(value));
        }
        break;
      }
      default:
        // A section this build does not know. Its length is right there, so it is
        // skipped rather than fatal — which is what lets a section be added without
        // a version bump.
        log::warn("save: skipping unknown section 0x%08X (%u bytes)", tag, length);
        break;
    }
    if (!body.ok()) {
      if (error) *error = "corrupt section";
      return false;
    }
  }
  if (payload.remaining() != 0 || !payload.ok()) {
    if (error) *error = "trailing bytes after the last section";
    return false;
  }

  migrate(out, version);
  return true;
}

bool decodeMeta(const std::uint8_t* data, std::size_t size, WorldMeta& out, std::string* error) {
  std::uint16_t version = 0;
  ByteReader payload(nullptr, 0);
  if (!openPayload(data, size, version, payload, error)) return false;

  while (payload.remaining() >= 8) {
    const std::uint32_t tag = payload.u32();
    const std::uint32_t length = payload.u32();
    if (!payload.ok() || length > payload.remaining()) {
      if (error) *error = "truncated section";
      return false;
    }
    ByteReader body = payload.sub(length);
    if (tag != kTagMeta) continue;
    decodeMeta(body, out);
    if (!body.ok()) {
      if (error) *error = "corrupt meta section";
      return false;
    }
    return true;
  }
  if (error) *error = "save has no meta section";
  return false;
}

std::uint16_t peekVersion(const std::uint8_t* data, std::size_t size) {
  if (!data || size < kHeaderBytes) return 0;
  if (std::memcmp(data, kMagic, sizeof kMagic) != 0) return 0;
  return static_cast<std::uint16_t>(data[8] | (data[9] << 8));
}

}  // namespace hr::save
