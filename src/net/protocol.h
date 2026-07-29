// Multiplayer wire protocol: message types, binary encoding, strict validation.
//
// SECURITY MODEL, carried over from js/net/protocol.js unchanged in substance:
// everything that arrives from a peer is hostile until proven otherwise, because a
// "friend" may be running a modified client. What changes is the mechanism.
//
// The web build sent JSON and had to defend against the shape of JSON: unknown
// fields smuggling `__proto__` into game objects, prototype-named keys in a player
// map, a rebuilt-field-by-field decode so nothing was ever passed through. A binary
// format has none of those problems by construction — there are no keys, only a
// fixed sequence of fields — so the defence collapses to two rules:
//
//   1. **Fail closed on truncation.** ByteReader latches on the first read past the
//      end and every later read returns zero, so a decoder runs straight through a
//      short or hostile packet and is checked once at the end.
//   2. **Range-check every field.** Every number that reaches the game is bounded
//      here; anything outside rejects the whole message. The checks are written as
//      "is it provably in range", so a NaN — which compares false against every
//      bound — is rejected rather than accepted by a missing `!`.
//
// The host additionally applies *semantic* validation (reach, rate limits, block
// whitelists) in host.cpp. This file only guarantees shape and range.
//
// One structural simplification the port earns: **the world snapshot is the save
// format**. The web build chunked a JSON blob into 64 KB parts with its own
// reassembly path; here the host encodes a `save::WorldSave` and ENet fragments the
// reliable packet for us. That is why the plan put this milestone strictly after
// saves.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/bytes.h"
#include "core/mat4.h"

namespace hr::net {

// Bumped whenever the wire layout changes. A peer that does not match is rejected
// at the handshake with a reason, rather than being allowed to misparse.
inline constexpr std::uint16_t kNetVersion = 1;

// Hard caps.
inline constexpr std::size_t kMaxMessage = 64 * 1024;
inline constexpr std::size_t kMaxWorldBytes = 24u * 1024u * 1024u;
inline constexpr std::size_t kMaxName = 20;
inline constexpr std::size_t kMaxWorldName = 28;
inline constexpr std::size_t kMaxReason = 80;
inline constexpr std::size_t kMaxPlayerId = 48;
inline constexpr std::size_t kMaxItemKey = 40;

// Sanity bounds, from js/net/protocol.js:45.
inline constexpr float kMaxCoord = 2.0e6f;
inline constexpr float kMaxVel = 2.0e3f;

enum class MsgType : std::uint8_t {
  None = 0,
  // handshake
  Hello,      // c->h
  Reject,     // h->c
  World,      // h->c: the whole save payload
  Ready,      // c->h: snapshot applied
  // continuous state
  Pose,       // c->h @15Hz
  Snapshot,   // h->c @10Hz
  Time,       // h->c authoritative clock
  Ping,       // both
  Pong,       // both
  // world edits
  Edit,       // both
  Edits,      // h->c batched
  EditDeny,   // h->c rollback
  // actions
  Hit,        // c->h attack a host-owned entity
  PlayerHit,  // c->h PvP
  BoatMount,  // c->h
  BoatDeny,   // h->c
  BoatSpawn,  // c->h
  Warp,       // c->h
  Teleport,   // h->c forced position
  Toss,       // c->h spawn a tossed drop
  Give,       // h->c pickup award
  Damage,     // h->c
  Sleep,      // c->h sleep vote
  Sfx,        // h->c positional one-shot
  // block entities
  BeRequest,  // c->h
  BeState,    // both
  BeDeny,     // h->c
  // lifecycle
  PlayerState,  // c->h periodic, for the host's save
  Bye,          // both
  PlayerJoin,   // h->c roster add
  PlayerLeave,  // h->c roster remove
  Notify,       // h->c toast
  Count,
};

const char* typeName(MsgType type);

// ---- messages ---------------------------------------------------------------
// All plain data. Each has an encode() that writes its body and a decode() that
// reads and validates it; the one-byte type tag is written by the caller through
// `begin()` so a dispatcher can peek at it without knowing any body layout.

struct HelloMsg {
  std::uint16_t version = kNetVersion;
  std::string name;
  std::string playerId;
};

struct RejectMsg {
  std::string reason;
};

struct WorldMsg {
  std::vector<std::uint8_t> save;  // save::encode() output
};

struct PoseMsg {
  Vec3 pos;
  Vec3 vel;
  float yaw = 0, pitch = 0;
  float health = 20.0f;
  // bit 0 swimming, 1 sneaking, 2 flying, 3 sprinting.
  std::uint8_t flags = 0;
};

// One entity in a snapshot. `a` and `b` are per-type extras, exactly as the web
// build packed them: a drop carries its stack count, a mob its health and hurt
// flash, a boat its rider flag.
struct SnapEntity {
  std::int32_t id = 0;
  std::uint8_t type = 0;
  Vec3 pos;
  float yaw = 0;
  float a = 0, b = 0;
};

struct SnapPlayer {
  std::string playerId;
  Vec3 pos;
  float yaw = 0, pitch = 0;
  std::uint8_t flags = 0;
  float health = 20.0f;
  bool hurt = false;
};

struct SnapshotMsg {
  float time = 0.32f;
  std::vector<SnapEntity> entities;
  std::vector<SnapPlayer> players;
};

struct TimeMsg {
  float time = 0.32f;
  bool sleeping = false;
};

struct PingMsg {
  std::uint64_t stamp = 0;
};

struct EditMsg {
  std::int32_t x = 0, y = 0, z = 0;
  std::uint16_t id = 0;
  std::uint8_t meta = 0;
};

struct EditsMsg {
  std::vector<EditMsg> list;
};

struct HitMsg {
  std::int32_t entityId = 0;
  std::string held;
  bool crit = false;
};

struct PlayerHitMsg {
  std::string playerId;
  std::string held;
  bool crit = false;
};

struct BoatMountMsg {
  std::int32_t entityId = 0;
  bool on = false;
};

struct BoatSpawnMsg {
  Vec3 pos;
};

struct TeleportMsg {
  Vec3 pos;
};

struct TossMsg {
  Vec3 pos;
  Vec3 dir;
  std::string key;
  std::int32_t count = 1;
  std::int32_t dura = -1;
};

struct GiveMsg {
  std::string key;
  std::int32_t count = 1;
  std::int32_t dura = -1;
};

struct DamageMsg {
  float amount = 0;
  Vec3 knockback;
};

struct SleepMsg {
  bool on = false;
};

struct SfxMsg {
  std::string kind;
  Vec3 pos;
};

// A slot as it travels the wire. Empty key means an empty slot.
struct WireSlot {
  std::string key;
  std::int32_t count = 0;
  std::int32_t dura = -1;
};

struct BeStateMsg {
  std::int32_t x = 0, y = 0, z = 0;
  std::uint8_t kind = 0;  // matches game::BlockEntityKind
  // Forge.
  WireSlot input, fuel, output;
  float fuelLeft = 0, fuelMax = 0, progress = 0;
  // Chest.
  std::vector<WireSlot> slots;
  bool final = false;  // close and unlock
};

struct BeRequestMsg {
  std::int32_t x = 0, y = 0, z = 0;
  std::uint8_t kind = 0;
};

struct BeDenyMsg {
  std::int32_t x = 0, y = 0, z = 0;
  std::string reason;
};

struct PlayerStateMsg {
  Vec3 pos;
  float yaw = 0, pitch = 0, health = 20, hunger = 20, saturation = 5;
  bool flying = false;
  std::vector<WireSlot> slots;
  std::vector<WireSlot> armor;
  std::int32_t selected = 0;
  bool hasSpawn = false;
  Vec3 spawn;
};

struct PlayerJoinMsg {
  std::string playerId;
  std::string name;
};

struct PlayerLeaveMsg {
  std::string playerId;
};

struct NotifyMsg {
  std::string message;
};

// ---- framing ----------------------------------------------------------------

// Starts a message: writes the one-byte type tag.
inline void begin(ByteWriter& w, MsgType type) { w.u8(static_cast<std::uint8_t>(type)); }
// The type of a raw packet, or None if it is empty or names nothing.
MsgType peekType(const std::uint8_t* data, std::size_t size);

// ---- encode / decode --------------------------------------------------------
// Each decode() takes a reader positioned just past the type tag and returns false
// for a truncated or out-of-range message. On false the output is not to be used.

void encode(ByteWriter& w, const HelloMsg& m);
bool decode(ByteReader& r, HelloMsg& m);
void encode(ByteWriter& w, const RejectMsg& m);
bool decode(ByteReader& r, RejectMsg& m);
void encode(ByteWriter& w, const WorldMsg& m);
bool decode(ByteReader& r, WorldMsg& m);
void encode(ByteWriter& w, const PoseMsg& m);
bool decode(ByteReader& r, PoseMsg& m);
void encode(ByteWriter& w, const SnapshotMsg& m);
bool decode(ByteReader& r, SnapshotMsg& m);
void encode(ByteWriter& w, const TimeMsg& m);
bool decode(ByteReader& r, TimeMsg& m);
void encode(ByteWriter& w, const PingMsg& m);
bool decode(ByteReader& r, PingMsg& m);
void encode(ByteWriter& w, const EditMsg& m);
bool decode(ByteReader& r, EditMsg& m);
void encode(ByteWriter& w, const EditsMsg& m);
bool decode(ByteReader& r, EditsMsg& m);
void encode(ByteWriter& w, const HitMsg& m);
bool decode(ByteReader& r, HitMsg& m);
void encode(ByteWriter& w, const PlayerHitMsg& m);
bool decode(ByteReader& r, PlayerHitMsg& m);
void encode(ByteWriter& w, const BoatMountMsg& m);
bool decode(ByteReader& r, BoatMountMsg& m);
void encode(ByteWriter& w, const BoatSpawnMsg& m);
bool decode(ByteReader& r, BoatSpawnMsg& m);
void encode(ByteWriter& w, const TeleportMsg& m);
bool decode(ByteReader& r, TeleportMsg& m);
void encode(ByteWriter& w, const TossMsg& m);
bool decode(ByteReader& r, TossMsg& m);
void encode(ByteWriter& w, const GiveMsg& m);
bool decode(ByteReader& r, GiveMsg& m);
void encode(ByteWriter& w, const DamageMsg& m);
bool decode(ByteReader& r, DamageMsg& m);
void encode(ByteWriter& w, const SleepMsg& m);
bool decode(ByteReader& r, SleepMsg& m);
void encode(ByteWriter& w, const SfxMsg& m);
bool decode(ByteReader& r, SfxMsg& m);
void encode(ByteWriter& w, const BeRequestMsg& m);
bool decode(ByteReader& r, BeRequestMsg& m);
void encode(ByteWriter& w, const BeStateMsg& m);
bool decode(ByteReader& r, BeStateMsg& m);
void encode(ByteWriter& w, const BeDenyMsg& m);
bool decode(ByteReader& r, BeDenyMsg& m);
void encode(ByteWriter& w, const PlayerStateMsg& m);
bool decode(ByteReader& r, PlayerStateMsg& m);
void encode(ByteWriter& w, const PlayerJoinMsg& m);
bool decode(ByteReader& r, PlayerJoinMsg& m);
void encode(ByteWriter& w, const PlayerLeaveMsg& m);
bool decode(ByteReader& r, PlayerLeaveMsg& m);
void encode(ByteWriter& w, const NotifyMsg& m);
bool decode(ByteReader& r, NotifyMsg& m);

// A player id is used as a map key and shown on a nameplate, so it is restricted
// the same way the web build restricted it — minus the prototype-name exclusions,
// which were a property of JavaScript objects and have no analogue here.
bool validPlayerId(const std::string& id);
// A fresh random one. Persisted in settings so a returning guest lands back in the
// host's saved slot for them.
std::string makePlayerId();
// Strips control characters and clamps the length. Never empty.
std::string cleanName(const std::string& raw);

// A per-peer token bucket, ported from js/net/protocol.js:265. `now` is the
// caller's monotonic clock in seconds, so the host can drive every bucket from the
// one timestamp it already has.
class Bucket {
 public:
  Bucket() = default;
  Bucket(double rate, double burst) : rate_(rate), burst_(burst), level_(burst) {}
  bool take(double now, double n = 1.0);

 private:
  double rate_ = 1.0;
  double burst_ = 1.0;
  double level_ = 1.0;
  double last_ = -1.0;
};

}  // namespace hr::net
