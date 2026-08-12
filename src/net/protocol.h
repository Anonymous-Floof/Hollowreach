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
#include "game/painting.h"

namespace hr::net {

// Bumped whenever the wire layout changes. A peer that does not match is rejected
// at the handshake with a reason, rather than being allowed to misparse.
// 2: paintings. A new message type, which an older peer would drop on the floor —
// so a guest on 1 would hang a picture nobody else could see and never see anyone
// else's. Bumped rather than left tolerant, because a clear "different version"
// at the join screen is a much better answer than a world that silently disagrees
// with itself about what is on the walls.
// 3: the guest half of the protocol was wrong in three ways that the wire format
// cannot express. A 2.2.0 guest drops any world bigger than one message, echoes
// every edit the host sends it back as a request of its own, and draws every body
// half a turn out — and it does all of that after a handshake that succeeds. The
// layout is unchanged, so this bump buys nothing technical; it buys a refusal at
// the join screen instead of a session that looks connected and does not work.
// 4: sleeping. A sleep vote now carries the hour it is voting for, and the host
// broadcasts whose proposal is on the table — a genuine layout change on one
// message and one new type, so a 2.3.0 peer would misread the vote it was sent and
// silently drop the state it needs to answer.
// 5: the world's own rules. Difficulty and cheats now belong to the world rather
// than the installation, so the host sends them and a guest obeys them. A new
// message type, which an older peer would take for one it knows by that tag -- and
// far worse than the mismatch, a 2.5.x guest would go on applying its OWN monster
// and flight settings in somebody else's world without either of them being told.
// 6: what a guest is told about the world it is standing in. Two layout changes,
// both of them things the old wire could not say: a snapshot entity now names the
// item it IS, because a drop that arrived as a position and a count was drawn by
// every guest as a featureless grey cube; and a pose and a snapshot each carry a
// sequence number, because the fast channel is unsequenced and a reordered packet
// was being accepted as the newest one. A 2.6.0 peer would misread both from the
// first packet it received.
// 7: who owns a boat, and how much of the world a guest is told about at once.
// The layout is unchanged and that is exactly why this has to be refused rather
// than tolerated -- the same bytes now mean something different. A BoatMount used
// to be asked for with the guest's own ghost id, which no host could ever match,
// so every mount was refused and boats simply did not work for a guest; asking
// with the host's id is what fixes it, and a 2.7.0 host answering that request
// would mount the boat and then drag its OWN player into the seat and steer the
// thing with its own keyboard. A silent version match would turn "boats do not
// work" into "the host is abducted by anyone who touches a boat".
// 8: the creative and debug rules. The layout is unchanged again, and again that
// is the reason: world rules travel as key/value text, so an older guest does not
// fail to parse the new ones -- it drops them and falls back to the defaults it
// was compiled with. In a creative world that guest takes fall damage, drowns and
// cannot fly while everybody else is invulnerable, and neither end is told they
// disagree. Exactly the shape of version 5, which was bumped for exactly this.
// 9: creative gains instant break, and the debug tools stop being one of the
// world's rules at all. Same reasoning as 8 and as 5 before it -- a rule an older
// guest has never heard of is dropped rather than refused, and it plays a
// different game without either end being told.
// 10: flight belongs to creative worlds now. The same key means something else --
// a 2.10.0 guest reads flight=true out of a world whose host no longer grants it
// and flies around a survival world on its own authority. Version 7's case
// exactly: the layout is unchanged and that is the reason to refuse rather than
// tolerate it.
// 11: chat, and who is allowed to do what. Three new message types, so an older
// peer drops them on the floor — and dropping THESE is worse than dropping most,
// because two of the three are how the two ends stay honest with each other. A
// 2.11.0 guest would type into a chat box nobody receives and see nothing anybody
// says, which is merely broken; but it would also never be told its permission
// level, so its own completion popup would offer it every command in the game and
// the host would refuse each one with a message the guest cannot receive either.
// Silence that looks like a bug in the host.
// 12: the Farming update. BeStateMsg carries a `kind` byte that matches
// game::BlockEntityKind, and this build added three values to that enum — so a
// version-11 guest opening a cooking pot would read it as whatever kind now sits at
// its number and show a chest's worth of nonsense. The wire LAYOUT is unchanged,
// which is exactly why this has to be refused rather than tolerated: the same bytes
// now mean something different. Same reasoning as versions 3, 7 and 10.
inline constexpr std::uint16_t kNetVersion = 12;

// Hard caps.
inline constexpr std::size_t kMaxMessage = 64 * 1024;
inline constexpr std::size_t kMaxWorldBytes = 24u * 1024u * 1024u;
inline constexpr std::size_t kMaxName = 20;
inline constexpr std::size_t kMaxWorldName = 28;
inline constexpr std::size_t kMaxReason = 80;
inline constexpr std::size_t kMaxPlayerId = 48;
inline constexpr std::size_t kMaxItemKey = 40;
// One chat line. Long enough to say anything worth saying and short enough that
// one guest cannot spend everybody's screen; the host also rate-limits, because a
// bound on one message is not a bound on how many arrive.
inline constexpr std::size_t kMaxChat = 256;

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
  // paintings
  Painting,   // both: c->h asks to hang one, h->c tells everyone what hangs there
  // lifecycle
  PlayerState,  // c->h periodic, for the host's save
  Bye,          // both
  PlayerJoin,   // h->c roster add
  PlayerLeave,  // h->c roster remove
  Notify,       // h->c toast
  // Who, if anyone, is currently waiting for the rest of us to come to bed, and
  // what hour they asked for. Appended rather than slotted in beside Sleep: the
  // tag is a byte on the wire and every value after an insertion would shift.
  SleepState,   // h->c
  WorldSettings,  // h->c: the world's own difficulty and cheat rules
  // Chat, and commands, which travel the same way: a guest sends a LINE and the
  // host decides what it was. There is deliberately no separate "command" message
  // — a guest that could name a command directly would be choosing which code path
  // runs on the host, and the whole trust model here is that it chooses nothing.
  Chat,        // c->h: the raw line, slash and all
  ChatLine,    // h->c: a line to show, already formatted and already addressed
  Permission,  // h->c: what level somebody holds
  SetState,    // h->c: an operator reached into this player's body or bag
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
  // Counts up from the sender, one per pose. See SnapshotMsg::seq — a pose travels
  // the same unsequenced channel and needs the same defence.
  std::uint32_t seq = 0;
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
  // What this entity IS, for the two types whose appearance is not implied by
  // their type: a dropped item and a falling block are both "whatever key names
  // them", and without it the receiver has a position, a count, and no idea what
  // to draw. Empty for everything else, which costs the one length byte.
  //
  // The key rather than an index into the item registry. An index is 2 bytes
  // against ~12 and was the first instinct, but it would make the wire depend on
  // registration order — a new item added anywhere but the end would silently
  // redraw every drop in every session between two builds that both think they
  // agree. A string cannot be wrong that way.
  std::string key;
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
  // Counts up from the host, one per snapshot, and the receiver discards anything
  // it has already passed.
  //
  // The fast channel is ENET_PACKET_FLAG_UNSEQUENCED, which means exactly what it
  // says: packets arrive in whatever order the network hands them over. A ghost
  // stamps each sample with the time it ARRIVED, so an old snapshot overtaking a
  // new one was not merely ignored — it was accepted as the newest thing known and
  // dragged every body back to where they had been. Worse, the entity list is a
  // full census and anything missing from it is treated as gone, so a stale
  // snapshot resurrected items that had already been picked up and killed ones
  // that had just been spawned. That is the flicker, and it is why it got worse
  // the longer a session ran: more entities, more to disagree about.
  std::uint32_t seq = 0;
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

// A sleep vote. `target` is the hour on the 0..1 clock the sender would like to
// wake at; only the first voter's is used, because the rest are agreeing to that
// hour rather than proposing their own.
struct SleepMsg {
  bool on = false;
  float target = 0.27f;
};

// The host's running tally, so a bed opened by anyone else shows the hour already
// on the table instead of offering a second one nobody voted for.
struct SleepStateMsg {
  bool active = false;
  float target = 0.27f;
  std::string proposer;  // display name, for "Ada wants to sleep"
  std::uint8_t votes = 0;
  std::uint8_t needed = 0;
};

// The world's own rules, pushed whenever the host changes one.
//
// A guest already receives these with the world when it joins, inside the save
// payload; this is what keeps them in step when the host reaches for the settings
// screen mid-game. Key/value text, exactly as the save carries it, so the schema
// can grow without this message learning about it.
struct WorldSettingsMsg {
  std::vector<std::pair<std::string, std::string>> values;
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

// The highest game::BlockEntityKind a state message may name. This read `2` — Forge
// and Chest — and the Farming update added three kinds above it, so decode REFUSED
// every message about a cutting board, a stove or a cooking pot and dropped it in
// silence. A guest's kitchen was empty on opening, ate whatever was put in it, and
// showed a cook that never progressed, all without one line of error anywhere.
//
// It lives here rather than being read out of `game/` because net does not depend on
// game; host.cpp sees both and static_asserts that they still agree, so the next kind
// added cannot repeat this quietly.
inline constexpr std::uint8_t kMaxBlockEntityKind = 5;

struct BeStateMsg {
  std::int32_t x = 0, y = 0, z = 0;
  std::uint8_t kind = 0;  // matches game::BlockEntityKind
  // Forge.
  WireSlot input, fuel, output;
  float fuelLeft = 0, fuelMax = 0, progress = 0;
  // Chest, and the pot's and stove's ingredient trays.
  std::vector<WireSlot> slots;
  // The pot's bowl. It had no field here at all, so a guest's bowl never reached the
  // host — and since Host::onBeState wrote back nothing but forges and chests, a
  // guest who filled a cooking pot and closed it lost the lot.
  WireSlot container;
  bool final = false;  // close and unlock
};

struct BeRequestMsg {
  std::int32_t x = 0, y = 0, z = 0;
  std::uint8_t kind = 0;
};

// A picture and where it hangs. The pixels themselves travel, which is the whole
// point — the receiver has never seen the sender's screenshots folder. Fixed size,
// so there is no dimension on the wire for a hostile peer to lie about: a body
// that is not exactly kPaintingBytes long is rejected outright.
struct PaintingMsg {
  std::int32_t x = 0, y = 0, z = 0;
  std::vector<std::uint8_t> rgb;
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

// What a guest typed. Nothing about it is interpreted on the sending end — not
// even whether it starts with a slash — because the host has to make that decision
// anyway and two ends deciding it separately is two ends that can disagree.
struct ChatMsg {
  std::string text;
};

// A line to show, and how to show it. `kind` matches ui::Chat::Kind; it travels as
// a byte and an unknown value is clamped to plain speech rather than rejected, so
// a newer host adding a colour does not disconnect an older guest over it.
struct ChatLineMsg {
  std::uint8_t kind = 0;
  // The display name of whoever said it, or empty for the world itself. Separate
  // from the text so the receiver can style it and so a guest cannot forge a line
  // that looks like it came from somebody else — the host fills this in.
  std::string from;
  std::string text;
};

// What level somebody holds. Sent for everybody as a guest joins and broadcast
// whenever one changes.
//
// A guest is told its OWN level because its completion popup filters on it, and
// told everybody else's because /list shows them. Neither is authority: the host
// checks again on every command, and this message only decides what a guest's
// screen says.
struct PermissionMsg {
  std::string playerId;
  std::uint8_t level = 0;
};

// Somebody with the authority to do it has changed this player's state without
// asking. Both halves in one message because they are the same act, and because a
// guest that could receive one without the other is a guest whose /clear also
// healed it.
//
// Only ever sent as the result of a command the host has already checked. There is
// no request form: a guest asking to have its own health set would be asking the
// host to trust it, which is the one thing this protocol never does.
struct SetStateMsg {
  // Below zero leaves health alone, which is what /clear wants.
  float health = -1.0f;
  bool clearInventory = false;
};

// Whether `seq` is newer than the newest already seen, for the counters carried by
// the messages that travel the unsequenced channel.
//
// The subtraction is the whole trick: an unsigned difference cast to signed is
// positive exactly when `seq` is ahead of `last` within half the range, which
// makes the comparison survive the counter wrapping. A plain `seq > last` would
// reject every packet for the rest of a session once it did.
//
// One function rather than the same three lines in the host and the client,
// because a rule about what to throw away that the two ends disagree about is
// worse than no rule at all.
inline bool newerSeq(std::uint32_t seq, std::uint32_t last) {
  return static_cast<std::int32_t>(seq - last) > 0;
}

// What one entry costs a SnapshotMsg on the wire, and what an empty one costs.
//
// Here rather than at the caller because they have to agree with encode() exactly,
// and the only way to keep that true is to define them next to it: a field added
// above without a matching change here would make the host's budget optimistic and
// push the snapshot back over the datagram it is being kept inside.
std::size_t snapshotOverhead();
std::size_t wireSize(const SnapEntity& e);
std::size_t wireSize(const SnapPlayer& p);

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
void encode(ByteWriter& w, const SleepStateMsg& m);
bool decode(ByteReader& r, SleepStateMsg& m);
void encode(ByteWriter& w, const WorldSettingsMsg& m);
bool decode(ByteReader& r, WorldSettingsMsg& m);
void encode(ByteWriter& w, const SfxMsg& m);
bool decode(ByteReader& r, SfxMsg& m);
void encode(ByteWriter& w, const BeRequestMsg& m);
bool decode(ByteReader& r, BeRequestMsg& m);
void encode(ByteWriter& w, const BeStateMsg& m);
bool decode(ByteReader& r, BeStateMsg& m);
void encode(ByteWriter& w, const PaintingMsg& m);
bool decode(ByteReader& r, PaintingMsg& m);
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
void encode(ByteWriter& w, const ChatMsg& m);
bool decode(ByteReader& r, ChatMsg& m);
void encode(ByteWriter& w, const ChatLineMsg& m);
bool decode(ByteReader& r, ChatLineMsg& m);
void encode(ByteWriter& w, const PermissionMsg& m);
bool decode(ByteReader& r, PermissionMsg& m);
void encode(ByteWriter& w, const SetStateMsg& m);
bool decode(ByteReader& r, SetStateMsg& m);

// Strips control characters and trims. Used on every chat line before it reaches a
// screen, because a display name is already cleaned and the text beside it is not:
// a newline in a chat line would let one guest draw as many rows as it liked, and a
// carriage return would let it overwrite the row above.
//
// Here rather than in the chat overlay so the host cleans what it relays and the
// guest cleans what it receives — a hostile peer is on the other side of both.
std::string cleanChat(const std::string& raw);

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
