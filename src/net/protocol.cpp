#include "net/protocol.h"

#include <algorithm>
#include <random>

namespace hr::net {
namespace {

// Written as "provably in range" rather than "not out of range", so a NaN — which
// compares false against every bound — is rejected instead of slipping through a
// missing negation. Every numeric field in this file goes through one of these.
bool okF(float v, float lo, float hi) { return v >= lo && v <= hi; }
bool okPos(const Vec3& v) {
  return okF(v.x, -kMaxCoord, kMaxCoord) && okF(v.y, -kMaxCoord, kMaxCoord) &&
         okF(v.z, -kMaxCoord, kMaxCoord);
}
bool okVel(const Vec3& v) {
  return okF(v.x, -kMaxVel, kMaxVel) && okF(v.y, -kMaxVel, kMaxVel) &&
         okF(v.z, -kMaxVel, kMaxVel);
}
bool okStr(const std::string& s, std::size_t max) { return s.size() <= max; }

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

void writeSlot(ByteWriter& w, const WireSlot& s) {
  w.str(s.key);
  w.i32(s.count);
  w.i32(s.dura);
}
bool readSlot(ByteReader& r, WireSlot& s) {
  s.key = r.str();
  s.count = r.i32();
  s.dura = r.i32();
  if (!okStr(s.key, kMaxItemKey)) return false;
  if (s.count < 0 || s.count > 999) return false;
  if (s.dura < -1 || s.dura > 99999) return false;
  if (s.key.empty()) s.count = 0;
  return true;
}

// A count that has to be believable before anything is reserved for it: nothing
// encodes in less than a byte, so a claim past the end of the packet is a lie.
bool readList(ByteReader& r, std::uint32_t max, std::uint32_t& out) {
  out = r.u32();
  if (!r.ok() || out > max || out > r.remaining()) {
    r.fail();
    return false;
  }
  return true;
}

}  // namespace

const char* typeName(MsgType type) {
  switch (type) {
    case MsgType::Hello: return "hello";
    case MsgType::Reject: return "reject";
    case MsgType::World: return "world";
    case MsgType::Ready: return "ready";
    case MsgType::Pose: return "pose";
    case MsgType::Snapshot: return "snap";
    case MsgType::Time: return "time";
    case MsgType::Ping: return "ping";
    case MsgType::Pong: return "pong";
    case MsgType::Edit: return "edit";
    case MsgType::Edits: return "edits";
    case MsgType::EditDeny: return "editDeny";
    case MsgType::Hit: return "hit";
    case MsgType::PlayerHit: return "phit";
    case MsgType::BoatMount: return "bmount";
    case MsgType::BoatDeny: return "bdeny";
    case MsgType::BoatSpawn: return "bspawn";
    case MsgType::Warp: return "warp";
    case MsgType::Teleport: return "tp";
    case MsgType::Toss: return "toss";
    case MsgType::Give: return "give";
    case MsgType::Damage: return "dmg";
    case MsgType::Sleep: return "sleep";
    case MsgType::Sfx: return "sfx";
    case MsgType::BeRequest: return "beReq";
    case MsgType::BeState: return "beState";
    case MsgType::BeDeny: return "beDeny";
    case MsgType::Painting: return "painting";
    case MsgType::PlayerState: return "pstate";
    case MsgType::Bye: return "bye";
    case MsgType::PlayerJoin: return "pjoin";
    case MsgType::PlayerLeave: return "pleave";
    case MsgType::Notify: return "notify";
    case MsgType::SleepState: return "sleepstate";
    default: return "?";
  }
}

MsgType peekType(const std::uint8_t* data, std::size_t size) {
  if (!data || size == 0) return MsgType::None;
  const std::uint8_t raw = data[0];
  if (raw == 0 || raw >= static_cast<std::uint8_t>(MsgType::Count)) return MsgType::None;
  return static_cast<MsgType>(raw);
}

// ---- handshake --------------------------------------------------------------

void encode(ByteWriter& w, const HelloMsg& m) {
  w.u16(m.version);
  w.str(m.name);
  w.str(m.playerId);
}
bool decode(ByteReader& r, HelloMsg& m) {
  m.version = r.u16();
  m.name = r.str();
  m.playerId = r.str();
  return r.ok() && okStr(m.name, kMaxName) && validPlayerId(m.playerId);
}

void encode(ByteWriter& w, const RejectMsg& m) { w.str(m.reason); }
bool decode(ByteReader& r, RejectMsg& m) {
  m.reason = r.str();
  return r.ok() && okStr(m.reason, kMaxReason);
}

void encode(ByteWriter& w, const WorldMsg& m) {
  w.u32(static_cast<std::uint32_t>(m.save.size()));
  w.bytes(m.save.data(), m.save.size());
}
bool decode(ByteReader& r, WorldMsg& m) {
  const std::uint32_t n = r.u32();
  if (!r.ok() || n > kMaxWorldBytes || n > r.remaining()) return false;
  m.save.resize(n);
  return r.bytes(m.save.data(), n);
}

// ---- continuous state -------------------------------------------------------

void encode(ByteWriter& w, const PoseMsg& m) {
  writeVec3(w, m.pos);
  writeVec3(w, m.vel);
  w.f32(m.yaw);
  w.f32(m.pitch);
  w.f32(m.health);
  w.u8(m.flags);
  w.u32(m.seq);
}
bool decode(ByteReader& r, PoseMsg& m) {
  m.pos = readVec3(r);
  m.vel = readVec3(r);
  m.yaw = r.f32();
  m.pitch = r.f32();
  m.health = r.f32();
  m.flags = r.u8();
  m.seq = r.u32();
  return r.ok() && okPos(m.pos) && okVel(m.vel) && okF(m.yaw, -64, 64) &&
         okF(m.pitch, -4, 4) && okF(m.health, 0, 40) && m.flags <= 15;
}

// The type tag, the clock, the sequence, and the two list lengths.
std::size_t snapshotOverhead() { return 1 + 4 + 4 + 4 + 4; }

std::size_t wireSize(const SnapEntity& e) {
  //     id   type  pos  yaw   a    b   key length + bytes
  return 4 + 1 + 12 + 4 + 4 + 4 + 2 + e.key.size();
}

std::size_t wireSize(const SnapPlayer& p) {
  //     id length + bytes    pos  yaw  pitch flags health hurt
  return 2 + p.playerId.size() + 12 + 4 + 4 + 1 + 4 + 1;
}

void encode(ByteWriter& w, const SnapshotMsg& m) {
  w.f32(m.time);
  w.u32(m.seq);
  w.u32(static_cast<std::uint32_t>(m.entities.size()));
  for (const SnapEntity& e : m.entities) {
    w.i32(e.id);
    w.u8(e.type);
    writeVec3(w, e.pos);
    w.f32(e.yaw);
    w.f32(e.a);
    w.f32(e.b);
    w.str(e.key);
  }
  w.u32(static_cast<std::uint32_t>(m.players.size()));
  for (const SnapPlayer& p : m.players) {
    w.str(p.playerId);
    writeVec3(w, p.pos);
    w.f32(p.yaw);
    w.f32(p.pitch);
    w.u8(p.flags);
    w.f32(p.health);
    w.boolean(p.hurt);
  }
}
bool decode(ByteReader& r, SnapshotMsg& m) {
  m.time = r.f32();
  m.seq = r.u32();
  if (!r.ok() || !okF(m.time, 0, 1)) return false;

  std::uint32_t n = 0;
  if (!readList(r, 512, n)) return false;
  m.entities.resize(n);
  for (SnapEntity& e : m.entities) {
    e.id = r.i32();
    e.type = r.u8();
    e.pos = readVec3(r);
    e.yaw = r.f32();
    e.a = r.f32();
    e.b = r.f32();
    e.key = r.str();
    // An empty key is the ordinary case — only a drop and a falling block have one
    // — so it is length that is checked here, not presence. Whether the key names
    // anything this build knows is the receiver's business: an unknown item draws
    // as the fallback cube rather than rejecting a whole snapshot of good entities.
    if (!okPos(e.pos) || !okF(e.yaw, -64, 64) || !okF(e.a, -kMaxCoord, kMaxCoord) ||
        !okF(e.b, -kMaxCoord, kMaxCoord) || !okStr(e.key, kMaxItemKey)) {
      return false;
    }
  }

  if (!readList(r, 32, n)) return false;
  m.players.resize(n);
  for (SnapPlayer& p : m.players) {
    p.playerId = r.str();
    p.pos = readVec3(r);
    p.yaw = r.f32();
    p.pitch = r.f32();
    p.flags = r.u8();
    p.health = r.f32();
    p.hurt = r.boolean();
    if (!validPlayerId(p.playerId) || !okPos(p.pos) || !okF(p.yaw, -64, 64) ||
        !okF(p.pitch, -4, 4) || p.flags > 15 || !okF(p.health, 0, 40)) {
      return false;
    }
  }
  return r.ok();
}

void encode(ByteWriter& w, const TimeMsg& m) {
  w.f32(m.time);
  w.boolean(m.sleeping);
}
bool decode(ByteReader& r, TimeMsg& m) {
  m.time = r.f32();
  m.sleeping = r.boolean();
  return r.ok() && okF(m.time, 0, 1);
}

void encode(ByteWriter& w, const PingMsg& m) { w.u64(m.stamp); }
bool decode(ByteReader& r, PingMsg& m) {
  m.stamp = r.u64();
  return r.ok();
}

// ---- edits ------------------------------------------------------------------

void encode(ByteWriter& w, const EditMsg& m) {
  w.i32(m.x);
  w.i32(m.y);
  w.i32(m.z);
  w.u16(m.id);
  w.u8(m.meta);
}
bool decode(ByteReader& r, EditMsg& m) {
  m.x = r.i32();
  m.y = r.i32();
  m.z = r.i32();
  m.id = r.u16();
  m.meta = r.u8();
  const auto coord = static_cast<std::int32_t>(kMaxCoord);
  return r.ok() && m.x >= -coord && m.x <= coord && m.z >= -coord && m.z <= coord &&
         m.y >= 0 && m.y < 512;
}

void encode(ByteWriter& w, const EditsMsg& m) {
  w.u32(static_cast<std::uint32_t>(m.list.size()));
  for (const EditMsg& e : m.list) encode(w, e);
}
bool decode(ByteReader& r, EditsMsg& m) {
  std::uint32_t n = 0;
  if (!readList(r, 512, n)) return false;
  m.list.resize(n);
  for (EditMsg& e : m.list) {
    if (!decode(r, e)) return false;
  }
  return r.ok();
}

// ---- actions ----------------------------------------------------------------

void encode(ByteWriter& w, const HitMsg& m) {
  w.i32(m.entityId);
  w.str(m.held);
  w.boolean(m.crit);
}
bool decode(ByteReader& r, HitMsg& m) {
  m.entityId = r.i32();
  m.held = r.str();
  m.crit = r.boolean();
  return r.ok() && okStr(m.held, kMaxItemKey);
}

void encode(ByteWriter& w, const PlayerHitMsg& m) {
  w.str(m.playerId);
  w.str(m.held);
  w.boolean(m.crit);
}
bool decode(ByteReader& r, PlayerHitMsg& m) {
  m.playerId = r.str();
  m.held = r.str();
  m.crit = r.boolean();
  return r.ok() && validPlayerId(m.playerId) && okStr(m.held, kMaxItemKey);
}

void encode(ByteWriter& w, const BoatMountMsg& m) {
  w.i32(m.entityId);
  w.boolean(m.on);
}
bool decode(ByteReader& r, BoatMountMsg& m) {
  m.entityId = r.i32();
  m.on = r.boolean();
  return r.ok();
}

void encode(ByteWriter& w, const BoatSpawnMsg& m) { writeVec3(w, m.pos); }
bool decode(ByteReader& r, BoatSpawnMsg& m) {
  m.pos = readVec3(r);
  return r.ok() && okPos(m.pos);
}

void encode(ByteWriter& w, const TeleportMsg& m) { writeVec3(w, m.pos); }
bool decode(ByteReader& r, TeleportMsg& m) {
  m.pos = readVec3(r);
  return r.ok() && okPos(m.pos);
}

void encode(ByteWriter& w, const TossMsg& m) {
  writeVec3(w, m.pos);
  writeVec3(w, m.dir);
  w.str(m.key);
  w.i32(m.count);
  w.i32(m.dura);
}
bool decode(ByteReader& r, TossMsg& m) {
  m.pos = readVec3(r);
  m.dir = readVec3(r);
  m.key = r.str();
  m.count = r.i32();
  m.dura = r.i32();
  return r.ok() && okPos(m.pos) && okF(m.dir.x, -2, 2) && okF(m.dir.y, -2, 2) &&
         okF(m.dir.z, -2, 2) && okStr(m.key, kMaxItemKey) && !m.key.empty() &&
         m.count >= 1 && m.count <= 999 && m.dura >= -1 && m.dura <= 99999;
}

void encode(ByteWriter& w, const GiveMsg& m) {
  w.str(m.key);
  w.i32(m.count);
  w.i32(m.dura);
}
bool decode(ByteReader& r, GiveMsg& m) {
  m.key = r.str();
  m.count = r.i32();
  m.dura = r.i32();
  return r.ok() && okStr(m.key, kMaxItemKey) && !m.key.empty() && m.count >= 1 &&
         m.count <= 999 && m.dura >= -1 && m.dura <= 99999;
}

void encode(ByteWriter& w, const DamageMsg& m) {
  w.f32(m.amount);
  writeVec3(w, m.knockback);
}
bool decode(ByteReader& r, DamageMsg& m) {
  m.amount = r.f32();
  m.knockback = readVec3(r);
  return r.ok() && okF(m.amount, 0, 100) && okF(m.knockback.x, -40, 40) &&
         okF(m.knockback.y, -40, 40) && okF(m.knockback.z, -40, 40);
}

void encode(ByteWriter& w, const SleepMsg& m) {
  w.boolean(m.on);
  w.f32(m.target);
}
bool decode(ByteReader& r, SleepMsg& m) {
  m.on = r.boolean();
  m.target = r.f32();
  return r.ok() && okF(m.target, 0, 1);
}

void encode(ByteWriter& w, const SleepStateMsg& m) {
  w.boolean(m.active);
  w.f32(m.target);
  w.str(m.proposer);
  w.u8(m.votes);
  w.u8(m.needed);
}
bool decode(ByteReader& r, SleepStateMsg& m) {
  m.active = r.boolean();
  m.target = r.f32();
  m.proposer = r.str();
  m.votes = r.u8();
  m.needed = r.u8();
  return r.ok() && okF(m.target, 0, 1) && m.proposer.size() <= kMaxName;
}

void encode(ByteWriter& w, const WorldSettingsMsg& m) {
  // Capped like every other list on the wire: a guest must not be able to make
  // the host allocate by claiming a huge count, and the schema has a handful.
  const std::size_t n = std::min<std::size_t>(m.values.size(), 64);
  w.u16(static_cast<std::uint16_t>(n));
  for (std::size_t i = 0; i < n; ++i) {
    w.str(m.values[i].first);
    w.str(m.values[i].second);
  }
}
bool decode(ByteReader& r, WorldSettingsMsg& m) {
  const int n = r.u16();
  if (n > 64) return false;
  for (int i = 0; i < n && r.ok(); ++i) {
    std::string key = r.str();
    std::string value = r.str();
    if (!r.ok()) break;
    m.values.emplace_back(std::move(key), std::move(value));
  }
  return r.ok();
}

void encode(ByteWriter& w, const SfxMsg& m) {
  w.str(m.kind);
  writeVec3(w, m.pos);
}
bool decode(ByteReader& r, SfxMsg& m) {
  m.kind = r.str();
  m.pos = readVec3(r);
  return r.ok() && okStr(m.kind, 24) && okPos(m.pos);
}

// ---- block entities ---------------------------------------------------------

void encode(ByteWriter& w, const PaintingMsg& m) {
  w.i32(m.x);
  w.i32(m.y);
  w.i32(m.z);
  for (std::size_t i = 0; i < game::kPaintingBytes; ++i) {
    w.u8(i < m.rgb.size() ? m.rgb[i] : 0);
  }
}
bool decode(ByteReader& r, PaintingMsg& m) {
  m.x = r.i32();
  m.y = r.i32();
  m.z = r.i32();
  // Exactly this many bytes or nothing. There is no length field to trust, so a
  // short packet fails here rather than leaving a half-filled picture.
  if (!r.ok() || r.remaining() < game::kPaintingBytes) return false;
  m.rgb.resize(game::kPaintingBytes);
  for (std::size_t i = 0; i < game::kPaintingBytes; ++i) m.rgb[i] = r.u8();
  const auto coord = static_cast<std::int32_t>(kMaxCoord);
  return r.ok() && m.x >= -coord && m.x <= coord && m.z >= -coord && m.z <= coord &&
         m.y >= 0 && m.y < 512;
}

void encode(ByteWriter& w, const BeRequestMsg& m) {
  w.i32(m.x);
  w.i32(m.y);
  w.i32(m.z);
  w.u8(m.kind);
}
bool decode(ByteReader& r, BeRequestMsg& m) {
  m.x = r.i32();
  m.y = r.i32();
  m.z = r.i32();
  m.kind = r.u8();
  const auto coord = static_cast<std::int32_t>(kMaxCoord);
  return r.ok() && m.x >= -coord && m.x <= coord && m.z >= -coord && m.z <= coord &&
         m.y >= 0 && m.y < 512 && m.kind <= 2;
}

void encode(ByteWriter& w, const BeStateMsg& m) {
  w.i32(m.x);
  w.i32(m.y);
  w.i32(m.z);
  w.u8(m.kind);
  writeSlot(w, m.input);
  writeSlot(w, m.fuel);
  writeSlot(w, m.output);
  w.f32(m.fuelLeft);
  w.f32(m.fuelMax);
  w.f32(m.progress);
  w.u16(static_cast<std::uint16_t>(m.slots.size()));
  for (const WireSlot& s : m.slots) writeSlot(w, s);
  w.boolean(m.final);
}
bool decode(ByteReader& r, BeStateMsg& m) {
  m.x = r.i32();
  m.y = r.i32();
  m.z = r.i32();
  m.kind = r.u8();
  if (!readSlot(r, m.input) || !readSlot(r, m.fuel) || !readSlot(r, m.output)) return false;
  m.fuelLeft = r.f32();
  m.fuelMax = r.f32();
  m.progress = r.f32();
  const std::size_t n = r.u16();
  if (!r.ok() || n > 54 || n * 10 > r.remaining()) return false;
  m.slots.resize(n);
  for (WireSlot& s : m.slots) {
    if (!readSlot(r, s)) return false;
  }
  m.final = r.boolean();
  const auto coord = static_cast<std::int32_t>(kMaxCoord);
  return r.ok() && m.x >= -coord && m.x <= coord && m.z >= -coord && m.z <= coord &&
         m.y >= 0 && m.y < 512 && m.kind <= 2 && okF(m.fuelLeft, 0, 1e6f) &&
         okF(m.fuelMax, 0, 1e6f) && okF(m.progress, 0, 1e6f);
}

void encode(ByteWriter& w, const BeDenyMsg& m) {
  w.i32(m.x);
  w.i32(m.y);
  w.i32(m.z);
  w.str(m.reason);
}
bool decode(ByteReader& r, BeDenyMsg& m) {
  m.x = r.i32();
  m.y = r.i32();
  m.z = r.i32();
  m.reason = r.str();
  return r.ok() && okStr(m.reason, kMaxReason);
}

// ---- lifecycle --------------------------------------------------------------

void encode(ByteWriter& w, const PlayerStateMsg& m) {
  writeVec3(w, m.pos);
  w.f32(m.yaw);
  w.f32(m.pitch);
  w.f32(m.health);
  w.f32(m.hunger);
  w.f32(m.saturation);
  w.boolean(m.flying);
  w.u16(static_cast<std::uint16_t>(m.slots.size()));
  for (const WireSlot& s : m.slots) writeSlot(w, s);
  w.u16(static_cast<std::uint16_t>(m.armor.size()));
  for (const WireSlot& s : m.armor) writeSlot(w, s);
  w.i32(m.selected);
  w.boolean(m.hasSpawn);
  writeVec3(w, m.spawn);
}
bool decode(ByteReader& r, PlayerStateMsg& m) {
  m.pos = readVec3(r);
  m.yaw = r.f32();
  m.pitch = r.f32();
  m.health = r.f32();
  m.hunger = r.f32();
  m.saturation = r.f32();
  m.flying = r.boolean();

  std::size_t n = r.u16();
  if (!r.ok() || n > 40 || n * 10 > r.remaining()) return false;
  m.slots.resize(n);
  for (WireSlot& s : m.slots) {
    if (!readSlot(r, s)) return false;
  }
  n = r.u16();
  if (!r.ok() || n > 4 || n * 10 > r.remaining()) return false;
  m.armor.resize(n);
  for (WireSlot& s : m.armor) {
    if (!readSlot(r, s)) return false;
  }
  m.selected = r.i32();
  m.hasSpawn = r.boolean();
  m.spawn = readVec3(r);
  return r.ok() && okPos(m.pos) && okF(m.yaw, -64, 64) && okF(m.pitch, -4, 4) &&
         okF(m.health, 0, 40) && okF(m.hunger, 0, 40) && okF(m.saturation, 0, 40) &&
         m.selected >= 0 && m.selected <= 8 && okPos(m.spawn);
}

void encode(ByteWriter& w, const PlayerJoinMsg& m) {
  w.str(m.playerId);
  w.str(m.name);
}
bool decode(ByteReader& r, PlayerJoinMsg& m) {
  m.playerId = r.str();
  m.name = r.str();
  return r.ok() && validPlayerId(m.playerId) && okStr(m.name, kMaxName);
}

void encode(ByteWriter& w, const PlayerLeaveMsg& m) { w.str(m.playerId); }
bool decode(ByteReader& r, PlayerLeaveMsg& m) {
  m.playerId = r.str();
  return r.ok() && validPlayerId(m.playerId);
}

void encode(ByteWriter& w, const NotifyMsg& m) { w.str(m.message); }
bool decode(ByteReader& r, NotifyMsg& m) {
  m.message = r.str();
  return r.ok() && okStr(m.message, 120);
}

// ---- identity ---------------------------------------------------------------

bool validPlayerId(const std::string& id) {
  if (id.size() < 4 || id.size() > kMaxPlayerId) return false;
  for (const char c : id) {
    const bool okChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!okChar) return false;
  }
  return true;
}

std::string makePlayerId() {
  static constexpr char kAlphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
  static std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> pick(0, 31);
  std::string out = "p";
  for (int i = 0; i < 12; ++i) out.push_back(kAlphabet[pick(rng)]);
  return out;
}

std::string cleanName(const std::string& raw) {
  std::string out;
  for (const unsigned char c : raw) {
    // Control characters out, everything printable in — including UTF-8
    // continuation bytes, which are all >= 0x80 and must survive intact.
    if (c >= 32 && c != 127) out.push_back(static_cast<char>(c));
    if (out.size() >= kMaxName) break;
  }
  // Trim, since a name of spaces is a blank nameplate.
  const auto first = out.find_first_not_of(' ');
  const auto last = out.find_last_not_of(' ');
  if (first == std::string::npos) return "Player";
  out = out.substr(first, last - first + 1);
  return out.empty() ? "Player" : out;
}

bool Bucket::take(double now, double n) {
  if (last_ < 0.0) last_ = now;
  level_ = std::min(burst_, level_ + (now - last_) * rate_);
  last_ = now;
  if (level_ < n) return false;
  level_ -= n;
  return true;
}

}  // namespace hr::net
