#include "net/host.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "game/blockentities.h"
#include "game/entities/ai.h"
#include "game/entities/manager.h"
#include "game/entities/types.h"
#include "game/inventory.h"
#include "game/items.h"
#include "game/player.h"
#include "render/sky.h"
#include "ui/settings.h"
#include "save/format.h"
#include "world/blocks.h"
#include "world/world.h"

namespace hr::net {
namespace {

// Semantic limits, from js/net/host.js:31-35.
constexpr float kEditReach = 14.0f;  // generous: covers the leaf-decay halo
constexpr float kHitReach = 8.0f;    // 6 blocks of raycast plus interpolation slack
constexpr float kBeReach = 8.0f;
// Entity snapshots. 20 Hz, up from the 10 the web build used, because the ghost
// interpolation has to buffer a whole inter-arrival gap before it can draw
// anything — so the send rate, not the latency, is what sets the floor on how far
// in the past a remote player is drawn. Halving the gap halves that floor, and a
// snapshot is small: a few hundred bytes for a busy world on an unreliable
// channel, which is a few kilobytes a second.
constexpr double kSnapPeriod = 0.050;
constexpr double kTimePeriod = 2.0;     // authoritative clock
// How long the movement check stays suspended after a guest says it is about to
// jump. Generous on purpose: it is bounded by an explicit request that costs a
// token, so the worst a peer can buy with it is the freedom to move oddly for a
// second at a rate its own bucket already limits.
constexpr double kMoveGrace = 1.0;
// How close a guest has to be to collect a drop. game/entities/drop.cpp uses 1.8
// for the local player and this has to agree with it, or the same item is
// collectable by one of them and not the other from the same spot.
constexpr float kPickupRange = 1.8f;

float dist3(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

WireSlot toWire(const game::ItemStack& s) {
  WireSlot out;
  if (s.empty()) return out;
  out.key = s.key;
  out.count = s.count;
  out.dura = s.dura;
  return out;
}

}  // namespace

template <typename T>
void Host::sendTo(PeerId peer, MsgType type, const T& msg, Channel channel) {
  ByteWriter w;
  begin(w, type);
  encode(w, msg);
  transport_.send(peer, channel, w.data());
}

void Host::sendEmpty(PeerId peer, MsgType type, Channel channel) {
  ByteWriter w;
  begin(w, type);
  transport_.send(peer, channel, w.data());
}

template <typename T>
void Host::broadcast(MsgType type, const T& msg, PeerId except, Channel channel) {
  ByteWriter w;
  begin(w, type);
  encode(w, msg);
  for (const PeerState& st : peers_) {
    if (!st.active || st.peer == except) continue;
    transport_.send(st.peer, channel, w.data());
  }
}

bool Host::start(std::uint16_t port, const std::string& playerId, const std::string& name,
                 const GameRefs& game, SessionHooks hooks, std::string* error) {
  if (!transport_.listen(port, kMaxGuests, error)) return false;
  game_ = game;
  hooks_ = std::move(hooks);
  playerId_ = playerId;
  name_ = name;
  ghosts_.attach(game_.entities);
  ghosts_.clear();
  peers_.clear();
  pendingEdits_.clear();
  beLocks_.clear();
  sleepVotes_.clear();
  snapTimer_ = timeTimer_ = 0;
  return true;
}

void Host::stop() {
  if (transport_.active()) {
    ByteWriter w;
    begin(w, MsgType::Bye);
    transport_.broadcast(Channel::Reliable, w.data());
  }
  transport_.close();
  peers_.clear();
  ghosts_.clear();
  pendingEdits_.clear();
  beLocks_.clear();
}

Host::PeerState* Host::peerFor(PeerId id) {
  for (PeerState& st : peers_) {
    if (st.peer == id) return &st;
  }
  return nullptr;
}

Host::PeerState* Host::peerForPlayer(const std::string& playerId) {
  for (PeerState& st : peers_) {
    if (st.playerId == playerId) return &st;
  }
  return nullptr;
}

int Host::guestCount() const {
  int n = 0;
  for (const PeerState& st : peers_) {
    if (st.active) ++n;
  }
  return n;
}

void Host::drop(PeerState& st, const std::string& reason) {
  if (!reason.empty()) {
    RejectMsg m;
    m.reason = reason.substr(0, kMaxReason);
    sendTo(st.peer, MsgType::Reject, m);
  }
  transport_.disconnect(st.peer);
}

void Host::forget(PeerState& st) {
  const bool announce = st.active && !st.playerId.empty();
  const std::string who = st.name;
  const std::string pid = st.playerId;
  // Removed first, so the broadcast below cannot reach the peer that left.
  peers_.erase(peers_.begin() + (&st - peers_.data()));
  if (!announce) return;

  ghosts_.removePlayer(pid);
  // Their vote goes with them, and if they were the one who proposed the hour the
  // question closes — the rest were answering them, not the clock.
  sleepVotes_.erase(pid);
  if (sleepVotes_.empty()) sleepProposer_.clear();
  broadcastSleepState();
  PlayerLeaveMsg leave;
  leave.playerId = pid;
  broadcast(MsgType::PlayerLeave, leave);
  NotifyMsg note;
  note.message = who + " left the world";
  broadcast(MsgType::Notify, note);
  if (hooks_.notify) hooks_.notify(note.message);
  if (hooks_.onRosterChange) hooks_.onRosterChange();
}

void Host::update(double dt, double now) {
  if (!transport_.active() || !game_.world) return;

  transport_.poll(events_);
  for (TransportEvent& e : events_) {
    switch (e.kind) {
      case TransportEvent::Kind::Connected: {
        if (static_cast<int>(peers_.size()) >= kMaxGuests) {
          RejectMsg m;
          m.reason = "The world is full";
          ByteWriter w;
          begin(w, MsgType::Reject);
          encode(w, m);
          transport_.send(e.peer, Channel::Reliable, w.data());
          transport_.disconnect(e.peer);
          break;
        }
        PeerState st;
        st.peer = e.peer;
        peers_.push_back(std::move(st));
        log::info("net: peer %u connected from %s", e.peer,
                  transport_.addressOf(e.peer).c_str());
        break;
      }
      case TransportEvent::Kind::Disconnected: {
        if (PeerState* st = peerFor(e.peer)) forget(*st);
        break;
      }
      case TransportEvent::Kind::Message: {
        PeerState* st = peerFor(e.peer);
        if (st) onMessage(*st, e.data.data(), e.data.size(), now);
        break;
      }
    }
  }

  // Poses arrive continuously; the ghosts are what the host draws.
  ghosts_.update(now);

  // Every frame, not on a timer. The 90 ms one this replaces was batching for its
  // own sake: an edit already coalesces with everything else that happened in the
  // same frame into a single EditsMsg, so the timer bought no extra packing that
  // mattered and charged up to 90 ms for it — on a link whose round trip is about
  // one. That was the delay between one player breaking a block and the other
  // seeing it go, and it was ninety times the network.
  //
  // A burst still batches: a flood of water writes hundreds of cells in one tick
  // and they leave together, capped at the protocol's 512 per message.
  flushEdits();
  // Before the snapshot, so a drop that has just been handed to somebody is gone
  // from the census that goes out in the same breath rather than being drawn for
  // one more frame in a place it no longer is.
  collectDrops();
  snapTimer_ += dt;
  if (snapTimer_ >= kSnapPeriod) {
    snapTimer_ -= kSnapPeriod;
    sendSnapshot();
  }
  timeTimer_ += dt;
  if (timeTimer_ >= kTimePeriod) {
    timeTimer_ -= kTimePeriod;
    if (game_.sky) {
      TimeMsg m;
      m.time = game_.sky->time;
      m.sleeping = game_.sky->isSleeping();
      broadcast(MsgType::Time, m, kNoPeer, Channel::Fast);
    }
  }

  // Every guest gets tired at the same rate the host does, measured off the host's
  // own clock — so the answer to "may I propose a sleep" is the same however
  // laggy, paused or modified the asking client is.
  if (game_.sky) {
    const float hours = game_.sky->advanced() * render::Sky::kHoursPerDay;
    if (hours > 0.0f) {
      for (PeerState& p : peers_) {
        p.hoursAwake = std::min(p.hoursAwake + hours, render::Sky::kHoursPerDay);
      }
    }
  }
}

void Host::onMessage(PeerState& st, const std::uint8_t* data, std::size_t size, double now) {
  if (size > kMaxMessage) {
    drop(st, "Message too large");
    return;
  }
  const MsgType type = peekType(data, size);
  if (type == MsgType::None) return;

  ByteReader r(data, size);
  r.skip(1);  // the type tag

  // Anything before a valid hello is refused outright, so an unauthenticated peer
  // cannot reach a single game system.
  if (!st.greeted && type != MsgType::Hello) return;

  switch (type) {
    case MsgType::Hello: {
      HelloMsg m;
      if (!decode(r, m)) {
        drop(st, "Malformed handshake");
        return;
      }
      onHello(st, m);
      break;
    }
    case MsgType::Ready: onReady(st); break;
    // A wayshard. The guest is about to arrive somewhere the movement check would
    // refuse, so the next pose is compared against nothing rather than against
    // where they were standing underground. It grants exactly one exemption and it
    // still costs a token, so a peer that spams it hits its own rate limit rather
    // than gaining free teleports.
    case MsgType::Warp:
      if (st.active && st.misc.take(now)) {
        st.havePose = false;
        st.moveGraceUntil = now + kMoveGrace;
      }
      break;
    case MsgType::Pose: {
      PoseMsg m;
      if (decode(r, m)) onPose(st, m, now);
      break;
    }
    case MsgType::Edit: {
      EditMsg m;
      if (decode(r, m)) onEdit(st, m, now);
      break;
    }
    case MsgType::Hit: {
      HitMsg m;
      if (decode(r, m)) onHit(st, m, now);
      break;
    }
    case MsgType::PlayerHit: {
      PlayerHitMsg m;
      if (decode(r, m)) onPlayerHit(st, m, now);
      break;
    }
    case MsgType::BoatMount: {
      BoatMountMsg m;
      if (decode(r, m)) onBoatMount(st, m, now);
      break;
    }
    case MsgType::BoatSpawn: {
      BoatSpawnMsg m;
      if (decode(r, m)) onBoatSpawn(st, m, now);
      break;
    }
    case MsgType::Toss: {
      TossMsg m;
      if (decode(r, m)) onToss(st, m, now);
      break;
    }
    case MsgType::Sleep: {
      SleepMsg m;
      if (decode(r, m)) onSleep(st, m);
      break;
    }
    case MsgType::BeRequest: {
      BeRequestMsg m;
      if (decode(r, m)) onBeRequest(st, m, now);
      break;
    }
    case MsgType::Painting: {
      PaintingMsg m;
      if (!decode(r, m) || !game_.world) break;
      // The same reach the host applies to an edit, checked the same way. Hanging
      // a picture is an edit to the world in every way that matters, so it gets
      // the existing rule rather than a second one to keep in step.
      if (std::fabs(static_cast<float>(m.x) - st.lastPose.x) > kEditReach ||
          std::fabs(static_cast<float>(m.y) - st.lastPose.y) > kEditReach ||
          std::fabs(static_cast<float>(m.z) - st.lastPose.z) > kEditReach) {
        break;
      }
      if (game_.world->getBlock(m.x, m.y, m.z) != world::wk().canvas) break;
      game::Painting art;
      art.rgb = std::move(m.rgb);
      game_.world->setPainting(m.x, m.y, m.z, art);
      // Back to everyone including the sender, so the picture a guest sees is the
      // one the host actually stored rather than the one it hoped for.
      PaintingMsg out;
      out.x = m.x;
      out.y = m.y;
      out.z = m.z;
      out.rgb = game_.world->painting(m.x, m.y, m.z)->rgb;
      broadcast(MsgType::Painting, out);
      break;
    }
    case MsgType::BeState: {
      BeStateMsg m;
      if (decode(r, m)) onBeState(st, m, now);
      break;
    }
    case MsgType::PlayerState: {
      PlayerStateMsg m;
      if (decode(r, m)) onPlayerState(st, m);
      break;
    }
    case MsgType::Ping: {
      PingMsg m;
      if (decode(r, m) && st.misc.take(now)) sendTo(st.peer, MsgType::Pong, m, Channel::Fast);
      break;
    }
    case MsgType::Bye: {
      // The guest has already stopped listening, so the roster is updated now
      // rather than when ENet's own handshake eventually times out.
      const PeerId peer = st.peer;
      forget(st);
      transport_.disconnect(peer);
      return;
    }
    default: break;  // anything host-to-client arriving here is simply ignored
  }
}

void Host::onHello(PeerState& st, const HelloMsg& m) {
  if (st.greeted) return;
  if (m.version != kNetVersion) {
    drop(st, "Different game version");
    return;
  }
  if (peerForPlayer(m.playerId) != nullptr || m.playerId == playerId_) {
    drop(st, "Already connected");
    return;
  }
  st.greeted = true;
  st.playerId = m.playerId;
  st.name = cleanName(m.name);
  sendWorld(st);
}

void Host::sendWorld(PeerState& st) {
  if (!hooks_.buildSave) {
    drop(st, "The host has no world to share");
    return;
  }
  save::WorldSave data = hooks_.buildSave();
  // The payload carries the world once and *this* guest's own progress in the
  // player and inventory slots — so a returning friend lands back where they were
  // with what they had, rather than in the host's body (js/net/host.js:178).
  const auto stored = stored_.find(st.playerId);
  if (stored != stored_.end() && stored->second.valid) {
    const save::GuestSave& mine = [&] {
      static save::GuestSave scratch;
      scratch = save::GuestSave{};
      const std::vector<save::GuestSave> all = guestsForSave();
      for (const save::GuestSave& g : all) {
        if (g.playerId == st.playerId) scratch = g;
      }
      return scratch;
    }();
    if (!mine.playerId.empty()) {
      data.player = mine.player;
      data.inventory = mine.inventory;
      data.meta.hasSpawn = mine.hasSpawn;
      if (mine.hasSpawn) data.meta.spawn = mine.spawn;
    }
  }
  // A guest never receives anyone else's progress.
  data.guests.clear();
  // Nor the entities. Every mob, drop and boat in this world belongs to the host
  // and reaches a guest as a ghost in the snapshot stream, which is what makes it
  // the same creature for both of them. Shipping the list in the payload as well
  // had the guest build a full LOCAL copy of each one — ticked by its own AI,
  // wandering off on its own, invisible to the host, and impossible to kill,
  // because the thing being swung at was never the thing that existed. Every one
  // of them then sat beside the ghost of the real animal for the rest of the
  // session. Cleared here rather than only on the far side so that a guest on any
  // build is told the truth about whose world it is.
  data.entities.clear();
  WorldMsg m;
  m.save = save::encode(data);
  if (m.save.size() > kMaxWorldBytes) {
    drop(st, "World is too large to share");
    return;
  }
  log::info("net: sending %zu bytes of world to %s", m.save.size(), st.name.c_str());
  sendTo(st.peer, MsgType::World, m);
}

void Host::onReady(PeerState& st) {
  if (st.active || st.playerId.empty()) return;
  st.active = true;
  ghosts_.addPlayer(st.playerId, st.name);

  // Roster: tell the newcomer about everyone, and everyone about the newcomer.
  PlayerJoinMsg self;
  self.playerId = playerId_;
  self.name = name_;
  sendTo(st.peer, MsgType::PlayerJoin, self);
  for (const PeerState& other : peers_) {
    if (&other == &st || !other.active) continue;
    PlayerJoinMsg j;
    j.playerId = other.playerId;
    j.name = other.name;
    sendTo(st.peer, MsgType::PlayerJoin, j);
  }
  PlayerJoinMsg join;
  join.playerId = st.playerId;
  join.name = st.name;
  broadcast(MsgType::PlayerJoin, join, st.peer);

  NotifyMsg note;
  note.message = st.name + " joined the world";
  broadcast(MsgType::Notify, note, st.peer);
  if (hooks_.notify) hooks_.notify(note.message);
  if (hooks_.onRosterChange) hooks_.onRosterChange();
}

void Host::onPose(PeerState& st, const PoseMsg& m, double now) {
  if (!st.active || !st.pose.take(now)) return;

  // Out of order on the unsequenced channel, or a duplicate. Taking one would move
  // this guest backwards for everybody else and — worse — feed a stale position to
  // the movement check below, which then answers their real position with a
  // teleport.
  if (st.havePoseSeq && !newerSeq(m.seq, st.lastPoseSeq)) return;
  st.lastPoseSeq = m.seq;
  st.havePoseSeq = true;

  if (st.havePose && now >= st.moveGraceUntil) {
    // Movement sanity. Not an anti-cheat so much as a bound on what one message
    // can do: eighty blocks a second plus eight of slack is well past sprinting,
    // flying or a boat, and short of teleporting across the map.
    const double dt = std::clamp(now - st.lastPoseTime, 0.01, 2.0);
    const float allowed = static_cast<float>(80.0 * dt + 8.0);
    if (dist3(m.pos, st.lastPose) > allowed) {
      TeleportMsg tp;
      tp.pos = st.lastPose;
      sendTo(st.peer, MsgType::Teleport, tp);
      return;
    }
  }
  st.lastPose = m.pos;
  st.lastYaw = m.yaw;
  st.lastPitch = m.pitch;
  st.flags = m.flags;
  st.havePose = true;
  st.lastPoseTime = now;
  st.health = m.health;
  ghosts_.feedPlayerPose(st.playerId, st.name, m, now);
}

void Host::denyEdit(PeerState& st, const EditMsg& m) {
  EditMsg back = m;
  back.id = static_cast<std::uint16_t>(game_.world->getBlock(m.x, m.y, m.z));
  back.meta = static_cast<std::uint8_t>(game_.world->getMeta(m.x, m.y, m.z));
  sendTo(st.peer, MsgType::EditDeny, back);
}

void Host::onEdit(PeerState& st, const EditMsg& m, double now) {
  if (!st.active) return;
  if (!st.edit.take(now) || !st.havePose) {
    denyEdit(st, m);
    return;
  }
  if (std::fabs(static_cast<float>(m.x) - st.lastPose.x) > kEditReach ||
      std::fabs(static_cast<float>(m.y) - st.lastPose.y) > kEditReach ||
      std::fabs(static_cast<float>(m.z) - st.lastPose.z) > kEditReach) {
    denyEdit(st, m);
    return;
  }
  const world::BlockRegistry& blocks = world::BlockRegistry::get();
  if (m.id >= blocks.count()) {
    denyEdit(st, m);
    return;
  }

  // A station block being replaced spills its contents and evicts whoever had it
  // open, exactly as breaking one locally does.
  const world::BlockId previous = game_.world->getBlock(m.x, m.y, m.z);
  if (previous != m.id && blocks.def(previous).station != world::Station::None) {
    spillBlockEntity(m.x, m.y, m.z);
  }

  // applyRemoteEdit, so the host's own edit sink stays quiet: it feeds
  // pendingEdits_, and the explicit push below already puts this edit there. Going
  // through setBlock queued every guest edit twice and sent each one out to
  // everybody twice. Anything the edit sets off later — water finding a new level,
  // a support giving way — runs on a later tick outside this call and still
  // reaches the sink normally, which is what relays the consequences.
  game_.world->applyRemoteEdit(m.x, m.y, m.z, static_cast<world::BlockId>(m.id), m.meta);
  pendingEdits_.push_back(m);
}

void Host::broadcastPainting(int x, int y, int z, const game::Painting& art) {
  if (!running() || art.blank()) return;
  PaintingMsg m;
  m.x = x;
  m.y = y;
  m.z = z;
  m.rgb = art.rgb;
  broadcast(MsgType::Painting, m);
}

void Host::spillBlockEntity(int x, int y, int z) {
  const game::BlockEntityKey key = game::blockEntityKey(x, y, z);
  if (game::BlockEntity* be = game_.world->getBlockEntity(x, y, z)) {
    for (const game::ItemStack& s : game::entityContents(*be)) {
      if (!s.empty()) {
        game_.world->spawnDrop(x + 0.5f, y + 0.5f, z + 0.5f, s.key, s.count, s.dura);
      }
    }
    game_.world->removeBlockEntity(x, y, z);
  }
  auto lock = beLocks_.find(key);
  if (lock != beLocks_.end()) {
    if (PeerState* holder = peerForPlayer(lock->second)) {
      BeDenyMsg deny;
      deny.x = x;
      deny.y = y;
      deny.z = z;
      deny.reason = "Container was destroyed";
      sendTo(holder->peer, MsgType::BeDeny, deny);
    }
    beLocks_.erase(lock);
  }
}

void Host::onHit(PeerState& st, const HitMsg& m, double now) {
  if (!st.active || !st.hit.take(now) || !st.havePose || !game_.entities) return;
  game::Entity* target = game_.entities->byId(m.entityId);
  if (!target || target->ghost || target->dead) return;
  if (dist3(st.lastPose, target->pos) > kHitReach) return;
  // A ridden boat cannot be broken out from under its rider.
  if (target->type == game::EntityType::Boat && target->data.rider) return;

  const game::EntityDef* def = game::defOf(target->type);
  if (!def || !def->interact) return;

  // The attacker's held item stands in for their inventory. A key the item
  // registry does not know is dropped rather than trusted.
  const bool knownHeld = !m.held.empty() && game::getItem(m.held) != nullptr;
  game::Inventory scratch;
  if (knownHeld) scratch.give(m.held, 1);

  game::EntityContext ctx;
  ctx.world = game_.world;
  ctx.player = game_.player;
  ctx.inventory = &scratch;
  ctx.entities = game_.entities;
  ctx.sky = game_.sky;
  const bool wasDead = target->dead;

  // Whatever the hook knocks loose belongs to the guest that swung, not to the
  // host standing somewhere else — so drops are captured for the length of the
  // call and sent back as awards rather than becoming instant pickups here. The
  // web build did the same thing through a shimmed context (js/net/host.js:_shimCtx).
  std::vector<GiveMsg> awards;
  const world::World::DropSink previous = game_.world->dropSink();
  game_.world->setDropSink([&awards](float, float, float, const std::string& key, int count,
                                     int dura) {
    GiveMsg award;
    award.key = key;
    award.count = count;
    award.dura = dura;
    awards.push_back(std::move(award));
  });
  def->interact(*target, ctx, game::InteractButton::Left);
  game_.world->setDropSink(previous);

  // Anything the hook put straight into the attacker's hands — shears taking wool,
  // a bucket taking milk — is in the scratch inventory it was given.
  for (const game::ItemStack& slot : scratch.slots()) {
    if (slot.empty() || slot.key == m.held) continue;
    GiveMsg award;
    award.key = slot.key;
    award.count = slot.count;
    award.dura = slot.dura;
    awards.push_back(std::move(award));
  }
  for (const GiveMsg& award : awards) sendTo(st.peer, MsgType::Give, award);

  const Vec3 voice{target->pos.x, target->pos.y + target->h * 0.7f, target->pos.z};
  onLocalSfx("thwack", voice);
  const char* key = game::entityTypeKey(target->type);
  if (target->data.health > 0.0f || wasDead) {
    onLocalSfx(std::string(key) + "_hurt", voice);
  } else {
    onLocalSfx(std::string(key) + "_death", voice);
  }
}

void Host::onPlayerHit(PeerState& st, const PlayerHitMsg& m, double now) {
  if (!st.active || !st.hit.take(now) || !st.havePose) return;
  const bool knownHeld = !m.held.empty() && game::getItem(m.held) != nullptr;
  game::Inventory scratch;
  if (knownHeld) scratch.give(m.held, 1);
  // `attackDamage` takes the attacker only to check whether they are falling, for
  // the critical-hit bonus. A guest's fall is not simulated here, so the flag they
  // sent stands in — it can only ever add the same 50% a local crit would.
  float damage = game::attackDamage(scratch, nullptr);
  if (m.crit) damage *= 1.5f;

  const auto knockback = [&st](const Vec3& victim) {
    const float dx = victim.x - st.lastPose.x;
    const float dz = victim.z - st.lastPose.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    const float scale = len > 0.001f ? 4.0f / len : 0.0f;
    return Vec3{dx * scale, 3.0f, dz * scale};
  };

  if (m.playerId == playerId_) {
    if (!game_.player || dist3(st.lastPose, game_.player->pos()) > kHitReach) return;
    DamageMsg dmg;
    dmg.amount = damage;
    dmg.knockback = knockback(game_.player->pos());
    game::PlayerOptions options;
    options.defense = static_cast<float>(game_.inventory ? game_.inventory->totalDefense() : 0);
    game_.player->damage(damage, options);
    Vec3 v = game_.player->velocity();
    game_.player->setVelocity({v.x + dmg.knockback.x, dmg.knockback.y, v.z + dmg.knockback.z});
    return;
  }

  PeerState* victim = peerForPlayer(m.playerId);
  if (!victim || !victim->active || !victim->havePose) return;
  if (dist3(st.lastPose, victim->lastPose) > kHitReach) return;
  DamageMsg dmg;
  dmg.amount = damage;
  dmg.knockback = knockback(victim->lastPose);
  sendTo(victim->peer, MsgType::Damage, dmg);
}

void Host::onBoatMount(PeerState& st, const BoatMountMsg& m, double now) {
  if (!st.active || !st.misc.take(now) || !game_.entities) return;
  game::Entity* boat = game_.entities->byId(m.entityId);
  if (!boat || boat->type != game::EntityType::Boat || boat->dead) {
    BoatMountMsg deny;
    deny.entityId = m.entityId;
    sendTo(st.peer, MsgType::BoatDeny, deny);
    return;
  }
  if (m.on) {
    if (boat->data.rider || (st.havePose && dist3(st.lastPose, boat->pos) > kHitReach)) {
      BoatMountMsg deny;
      deny.entityId = m.entityId;
      sendTo(st.peer, MsgType::BoatDeny, deny);
      return;
    }
    boat->data.rider = true;
  } else {
    boat->data.rider = false;
  }
}

void Host::onBoatSpawn(PeerState& st, const BoatSpawnMsg& m, double now) {
  if (!st.active || !st.misc.take(now) || !st.havePose || !game_.entities) return;
  if (dist3(st.lastPose, m.pos) > kHitReach) return;
  game_.entities->spawn(game::EntityType::Boat,
                        Vec3{m.pos.x, m.pos.y + game::kBoatSeatY, m.pos.z});
}

void Host::onToss(PeerState& st, const TossMsg& m, double now) {
  if (!st.active || !st.toss.take(now) || !st.havePose || !game_.entities) return;
  if (dist3(st.lastPose, m.pos) > kHitReach) return;
  if (!game::getItem(m.key)) return;
  game_.entities->spawnTossed(m.pos, m.dir, m.key, m.count, m.dura);
}

// A vote arriving from a guest. The tiredness rule is checked HERE and only for
// the person who opens the question: whoever proposes an hour has to have earned
// it, and everyone after them is simply agreeing. That is the whole point of the
// rework — a group should not be held awake because one of them happens to have
// napped more recently than the others.
void Host::onSleep(PeerState& st, const SleepMsg& m) {
  if (!st.active) return;
  if (m.on) {
    if (sleepVotes_.empty()) {
      // Opening the question. The host owns the clock, so the host is also the one
      // that knows whether this player is tired — it has been counting their hours
      // since they joined.
      if (st.hoursAwake < render::Sky::kRestedHours) {
        NotifyMsg no;
        no.message = "You are not tired yet";
        sendTo(st.peer, MsgType::Notify, no);
        return;
      }
      sleepTarget_ = m.target;
      sleepProposer_ = st.name;
    }
    sleepVotes_.insert(st.playerId);
  } else {
    sleepVotes_.erase(st.playerId);
    if (sleepVotes_.empty()) sleepProposer_.clear();
  }
  tallySleep();
}

void Host::onLocalSleep(bool on, float target) {
  if (on) {
    if (sleepVotes_.empty()) {
      if (game_.sky && !game_.sky->tired()) {
        if (hooks_.notify) hooks_.notify("You are not tired yet");
        return;
      }
      sleepTarget_ = target;
      sleepProposer_ = name_;
    }
    sleepVotes_.insert(playerId_);
  } else {
    sleepVotes_.erase(playerId_);
    if (sleepVotes_.empty()) sleepProposer_.clear();
  }
  tallySleep();
}

void Host::broadcastWorldSettings() {
  if (!running()) return;
  WorldSettingsMsg m;
  m.values = ui::settings().worldValues();
  broadcast(MsgType::WorldSettings, m);
}

void Host::broadcastSleepState() {
  SleepStateMsg s;
  s.active = !sleepVotes_.empty();
  s.target = sleepTarget_;
  s.proposer = sleepProposer_;
  s.votes = static_cast<std::uint8_t>(std::min<std::size_t>(sleepVotes_.size(), 255));
  s.needed = static_cast<std::uint8_t>(std::min(guestCount() + 1, 255));
  broadcast(MsgType::SleepState, s);
}

void Host::tallySleep() {
  const std::size_t present = static_cast<std::size_t>(guestCount()) + 1;  // and the host
  broadcastSleepState();
  NotifyMsg note;
  if (sleepVotes_.size() < present) {
    note.message = std::to_string(sleepVotes_.size()) + "/" + std::to_string(present) +
                   " in bed \xC2\xB7 until " + render::Sky::clockStringAt(sleepTarget_);
    broadcast(MsgType::Notify, note);
    if (hooks_.notify) hooks_.notify(note.message);
    return;
  }
  // Unanimous. The clock is the host's, so it sweeps here and the guests are told;
  // they do not each fast-forward their own copy and drift apart doing it.
  const float span = game_.sky ? std::fmod(sleepTarget_ - game_.sky->time + 1.0f, 1.0f) : 0.0f;
  if (game_.sky) game_.sky->startSleep(sleepTarget_);
  // Everyone who was in bed wakes rested, which on this side means every peer:
  // the tally only fires when all of them voted.
  for (PeerState& p : peers_) p.hoursAwake = 0.0f;
  sleepVotes_.clear();
  sleepProposer_.clear();
  broadcastSleepState();
  note.message = "Everyone sleeps for " +
                 render::Sky::spanString(span <= 0.0005f ? 1.0f : span);
  broadcast(MsgType::Notify, note);
  if (hooks_.notify) hooks_.notify(note.message);
  if (game_.sky) {
    TimeMsg t;
    t.time = game_.sky->time;
    t.sleeping = true;
    broadcast(MsgType::Time, t);
  }
}

std::vector<save::GuestSave> Host::guestsForSave() const {
  std::vector<save::GuestSave> out;
  for (const auto& [pid, stored] : stored_) {
    if (!stored.valid) continue;
    save::GuestSave g;
    g.playerId = pid;
    g.name = stored.name;
    g.player.pos = stored.state.pos;
    g.player.yaw = stored.state.yaw;
    g.player.pitch = stored.state.pitch;
    g.player.health = stored.state.health;
    g.player.hunger = stored.state.hunger;
    g.player.saturation = stored.state.saturation;
    g.player.flying = stored.state.flying;
    for (std::size_t i = 0; i < stored.state.slots.size() && i < game::kInventorySlots; ++i) {
      const WireSlot& w = stored.state.slots[i];
      if (w.key.empty() || w.count <= 0) continue;
      g.inventory.slots()[i] = game::ItemStack{w.key, w.count, w.dura};
    }
    for (std::size_t i = 0; i < stored.state.armor.size() && i < game::kArmorSlots; ++i) {
      const WireSlot& w = stored.state.armor[i];
      if (w.key.empty() || w.count <= 0) continue;
      g.inventory.armor()[i] = game::ItemStack{w.key, w.count, w.dura};
    }
    g.inventory.setSelected(stored.state.selected);
    g.hasSpawn = stored.state.hasSpawn;
    g.spawn = stored.state.spawn;
    out.push_back(std::move(g));
  }
  // Sorted, so the save stays byte-identical between two writes of the same world.
  std::sort(out.begin(), out.end(), [](const save::GuestSave& a, const save::GuestSave& b) {
    return a.playerId < b.playerId;
  });
  return out;
}

void Host::restoreGuests(const std::vector<save::GuestSave>& guests) {
  for (const save::GuestSave& g : guests) {
    if (!validPlayerId(g.playerId)) continue;
    StoredGuest& stored = stored_[g.playerId];
    stored.name = g.name;
    stored.valid = true;
    stored.state.pos = g.player.pos;
    stored.state.yaw = g.player.yaw;
    stored.state.pitch = g.player.pitch;
    stored.state.health = g.player.health;
    stored.state.hunger = g.player.hunger;
    stored.state.saturation = g.player.saturation;
    stored.state.flying = g.player.flying;
    stored.state.slots.clear();
    for (const game::ItemStack& slot : g.inventory.slots()) {
      WireSlot w;
      if (!slot.empty()) {
        w.key = slot.key;
        w.count = slot.count;
        w.dura = slot.dura;
      }
      stored.state.slots.push_back(std::move(w));
    }
    stored.state.armor.clear();
    for (const game::ItemStack& slot : g.inventory.armor()) {
      WireSlot w;
      if (!slot.empty()) {
        w.key = slot.key;
        w.count = slot.count;
        w.dura = slot.dura;
      }
      stored.state.armor.push_back(std::move(w));
    }
    stored.state.selected = g.inventory.selected();
    stored.state.hasSpawn = g.hasSpawn;
    stored.state.spawn = g.spawn;
  }
}

void Host::onBeRequest(PeerState& st, const BeRequestMsg& m, double now) {
  if (!st.active || !st.be.take(now) || !st.havePose) return;
  const Vec3 at{m.x + 0.5f, m.y + 0.5f, m.z + 0.5f};
  if (dist3(st.lastPose, at) > kBeReach) return;

  const game::BlockEntityKey key = game::blockEntityKey(m.x, m.y, m.z);
  auto lock = beLocks_.find(key);
  if (lock != beLocks_.end() && lock->second != st.playerId) {
    BeDenyMsg deny;
    deny.x = m.x;
    deny.y = m.y;
    deny.z = m.z;
    deny.reason = "Someone else has that open";
    sendTo(st.peer, MsgType::BeDeny, deny);
    return;
  }
  game::BlockEntity* be = game_.world->getBlockEntity(m.x, m.y, m.z);
  if (!be) {
    BeDenyMsg deny;
    deny.x = m.x;
    deny.y = m.y;
    deny.z = m.z;
    deny.reason = "Nothing there";
    sendTo(st.peer, MsgType::BeDeny, deny);
    return;
  }
  beLocks_[key] = st.playerId;

  BeStateMsg state;
  state.x = m.x;
  state.y = m.y;
  state.z = m.z;
  state.kind = static_cast<std::uint8_t>(be->kind);
  state.input = toWire(be->input);
  state.fuel = toWire(be->fuel);
  state.output = toWire(be->output);
  state.fuelLeft = be->fuelLeft;
  state.fuelMax = be->fuelMax;
  state.progress = be->progress;
  for (const game::ItemStack& s : be->slots) state.slots.push_back(toWire(s));
  sendTo(st.peer, MsgType::BeState, state);
}

void Host::onBeState(PeerState& st, const BeStateMsg& m, double now) {
  if (!st.active || !st.be.take(now)) return;
  const game::BlockEntityKey key = game::blockEntityKey(m.x, m.y, m.z);
  auto lock = beLocks_.find(key);
  // Only the holder of the lock may write, which is the whole reason the lock
  // exists: two people stirring one forge would otherwise each overwrite the
  // other's idea of what is in it.
  if (lock == beLocks_.end() || lock->second != st.playerId) return;

  game::BlockEntity* be = game_.world->getBlockEntity(m.x, m.y, m.z);
  if (be) {
    const auto fromWire = [](const WireSlot& s) {
      game::ItemStack out;
      if (s.key.empty() || s.count <= 0 || !game::getItem(s.key)) return out;
      out.key = s.key;
      out.count = s.count;
      out.dura = s.dura;
      return out;
    };
    if (be->kind == game::BlockEntityKind::Forge) {
      be->input = fromWire(m.input);
      be->fuel = fromWire(m.fuel);
      be->output = fromWire(m.output);
    } else if (be->kind == game::BlockEntityKind::Chest) {
      for (std::size_t i = 0; i < be->slots.size(); ++i) {
        be->slots[i] = i < m.slots.size() ? fromWire(m.slots[i]) : game::ItemStack{};
      }
    }
  }
  if (m.final) beLocks_.erase(key);
}

void Host::onPlayerState(PeerState& st, const PlayerStateMsg& m) {
  if (!st.active) return;
  StoredGuest& stored = stored_[st.playerId];
  stored.name = st.name;
  stored.state = m;
  stored.valid = true;
}

void Host::onLocalEdit(int x, int y, int z, std::uint16_t id, std::uint8_t meta) {
  if (!transport_.active()) return;
  EditMsg m;
  m.x = x;
  m.y = y;
  m.z = z;
  m.id = id;
  m.meta = meta;
  pendingEdits_.push_back(m);
}

void Host::onLocalSfx(const std::string& kind, const Vec3& pos) {
  if (!transport_.active()) return;
  SfxMsg m;
  m.kind = kind.substr(0, 24);
  m.pos = pos;
  broadcast(MsgType::Sfx, m, kNoPeer, Channel::Fast);
  if (hooks_.playSfx) hooks_.playSfx(m.kind, pos);
}

void Host::collectDrops() {
  if (!game_.entities) return;
  for (game::Entity& e : game_.entities->all()) {
    if (e.dead || e.ghost || e.type != game::EntityType::Drop) continue;
    if (e.data.key.empty() || e.data.count <= 0) continue;
    // The same delay the local player waits out, so a thrown item cannot be
    // caught by the thrower before it has left their hand.
    if (e.age < e.data.pickupDelay) continue;

    for (PeerState& st : peers_) {
      if (!st.active || !st.havePose) continue;
      // Measured to the chest rather than the feet, matching drop.cpp: a drop
      // resting on the ground you are stood on is a block below your eyes and
      // most of a body away from your ankles.
      const Vec3 chest{st.lastPose.x, st.lastPose.y + 0.9f, st.lastPose.z};
      if (dist3(chest, e.pos) > kPickupRange) continue;

      GiveMsg give;
      give.key = e.data.key;
      give.count = e.data.count;
      give.dura = e.data.dura;
      sendTo(st.peer, MsgType::Give, give);
      // Handed over whole. What will not fit in the guest's bag comes straight
      // back as a toss, which is the same round trip a full inventory takes
      // locally and keeps the host the only place an item can exist. No sound
      // from here: the collector's own Give handler plays it, and a pickup is
      // heard by the person who picked it up.
      e.dead = true;
      break;
    }
  }
}

void Host::flushEdits() {
  if (pendingEdits_.empty()) return;
  EditsMsg batch;
  // 512 is the protocol's cap on one message; a bigger burst goes out over
  // consecutive flushes rather than being dropped.
  const std::size_t take = std::min<std::size_t>(pendingEdits_.size(), 512);
  batch.list.assign(pendingEdits_.begin(), pendingEdits_.begin() + take);
  pendingEdits_.erase(pendingEdits_.begin(), pendingEdits_.begin() + take);
  broadcast(MsgType::Edits, batch);
}

void Host::sendSnapshot() {
  if (!game_.entities || peers_.empty()) return;
  SnapshotMsg snap;
  snap.time = game_.sky ? game_.sky->time : 0.32f;
  snap.seq = ++snapSeq_;

  for (const game::Entity& e : game_.entities->all()) {
    if (e.dead || e.ghost) continue;
    if (snap.entities.size() >= 512) break;
    SnapEntity out;
    out.id = e.id;
    out.type = static_cast<std::uint8_t>(e.type);
    out.pos = e.pos;
    out.yaw = e.yaw;
    switch (e.type) {
      case game::EntityType::Drop:
        out.a = static_cast<float>(e.data.count);
        // Which item this is. Everything else in a snapshot is a number because
        // a sheep is entirely described by being a sheep; a drop is not, and
        // sending it without this left the receiver drawing the renderer's
        // last-resort cube for every item in the world.
        out.key = e.data.key.substr(0, kMaxItemKey);
        break;
      case game::EntityType::FallingBlock:
        // The block it looks like, for the same reason. The id it will put back
        // down travels in `dura` and is the host's business, not the guest's.
        out.key = e.data.key.substr(0, kMaxItemKey);
        break;
      case game::EntityType::Boat:
        out.a = e.data.rider ? 1.0f : 0.0f;
        break;
      default:
        out.a = e.data.health;
        out.b = e.data.hurtFlash;
        break;
    }
    snap.entities.push_back(std::move(out));
  }

  // The host's own player is in the snapshot like any other, so a guest draws it
  // through exactly the same path as another guest.
  if (game_.player) {
    SnapPlayer self;
    self.playerId = playerId_;
    self.pos = game_.player->pos();
    self.yaw = game_.player->yaw();
    self.pitch = game_.player->pitch();
    self.flags = static_cast<std::uint8_t>((game_.player->swimming() ? 1 : 0) |
                                           (game_.player->sneaking() ? 2 : 0) |
                                           (game_.player->flying() ? 4 : 0) |
                                           (game_.player->sprinting() ? 8 : 0));
    self.health = game_.player->health();
    snap.players.push_back(std::move(self));
  }
  for (const PeerState& st : peers_) {
    if (!st.active || !st.havePose) continue;
    SnapPlayer p;
    p.playerId = st.playerId;
    p.pos = st.lastPose;
    // Relayed the same way the position is. These were left at their defaults,
    // which the host never noticed because it draws guests from their poses
    // directly — but a guest only ever learns about another guest through this
    // snapshot, so with three people in a world the other two faced north and
    // never moved their heads.
    p.yaw = st.lastYaw;
    p.pitch = st.lastPitch;
    p.flags = st.flags;
    p.health = st.health;
    snap.players.push_back(std::move(p));
  }

  broadcast(MsgType::Snapshot, snap, kNoPeer, Channel::Fast);
}

std::vector<RosterEntry> Host::roster() const {
  std::vector<RosterEntry> out;
  RosterEntry self;
  self.playerId = playerId_;
  self.name = name_;
  self.self = true;
  self.host = true;
  out.push_back(std::move(self));
  for (const PeerState& st : peers_) {
    if (!st.active) continue;
    RosterEntry e;
    e.playerId = st.playerId;
    e.name = st.name;
    e.pingMs = transport_.roundTripMs(st.peer);
    out.push_back(std::move(e));
  }
  return out;
}

}  // namespace hr::net
