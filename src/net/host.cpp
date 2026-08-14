#include "net/host.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

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

// The one place both halves are visible. Raise kMaxBlockEntityKind when a kind is
// added, or decode refuses every state message about it and the new station silently
// does nothing over the network — which is exactly what happened to all three
// kitchens in the 2.14.0 draft.
static_assert(static_cast<std::uint8_t>(game::BlockEntityKind::Pot) == kMaxBlockEntityKind,
              "a new BlockEntityKind needs kMaxBlockEntityKind raised to match");

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
// How many bytes of one snapshot may go out, so that it arrives as one datagram.
//
// ENet fragments above mtu - sizeof(ENetProtocolHeader) - sizeof(ENetProtocolSendFragment),
// which is 1364 bytes at the default 1392 MTU. Fragmenting is no longer the
// disaster it was — see the flag in transport.cpp — but an unreliable fragment set
// is delivered only if every piece arrives, so a snapshot in five pieces is five
// times as likely to be thrown away whole. Staying inside one datagram is what
// makes a dropped snapshot cost one frame of smoothing instead of a stutter, and
// this is comfortably under so a peer that negotiates a smaller MTU still fits.
constexpr std::size_t kSnapBudget = 1200;
// Past this an entity is not a guest's business. The byte budget is the real
// limit; this is the relevance filter in front of it, so the sort below has a
// small list to work on rather than every entity in the world.
constexpr float kSnapRange = 128.0f;

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
  out.tint = s.tint;
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
  // A boat cannot stay occupied by somebody who has gone home. It would be
  // unbreakable, unmountable and, because the host does not simulate a hull
  // somebody else is steering, motionless forever.
  releaseBoats(pid);
  // Their locks go with them. A guest that closes a container politely releases it
  // with a final BeState; one that crashed, timed out, or was still standing in a
  // chest when the power went out did not — and a lock is keyed by position, not by
  // session, so it outlived them. Every later opener, including the host, was then
  // told "Someone else has that open" about a chest belonging to nobody, forever.
  for (auto it = beLocks_.begin(); it != beLocks_.end();) {
    if (it->second == pid) {
      it = beLocks_.erase(it);
    } else {
      ++it;
    }
  }
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
  // A leave was reaching the toast and nothing else, so the one event a host most
  // wants a record of — who was here and when they went — left no trace at all.
  announceChat(note.message);
  if (hooks_.onRosterChange) hooks_.onRosterChange();
}

void Host::update(double dt, double now) {
  // Cached so the command helpers below can grant a movement grace window. They
  // are called from App in response to a typed line rather than from the update
  // loop, so they have no clock of their own — and a grace that had to be a single
  // exemption instead of a window would be defeated by exactly the reordering the
  // window exists for. See PeerState::moveGraceUntil.
  now_ = now;
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
  // Both before the snapshot, so what goes out is where things are now rather than
  // where they were a frame ago: a drop that has just been handed to somebody is
  // gone from the census in the same breath, and a boat somebody else is steering
  // is at the position their latest pose puts it.
  collectDrops();
  syncRiddenBoats();
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
    case MsgType::Chat: {
      ChatMsg m;
      if (!decode(r, m) || !st.active) break;
      // The rate limit is checked before the line is looked at, so a guest cannot
      // buy extra tokens by sending something that turns out to be invalid. A
      // refused line is dropped silently rather than answered: telling a flooder
      // it is being throttled costs the same bandwidth as the flood.
      if (!st.chat.take(now)) break;
      const std::string text = cleanChat(m.text);
      if (text.empty()) break;
      // Straight up to the session, which owns the command registry and the
      // permission table. Host deliberately cannot tell a command from a sentence.
      if (hooks_.onChatLine) hooks_.onChatLine(st.playerId, text);
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
  const std::string name = cleanName(m.name);
  // Bans and the whitelist, asked of the session rather than answered here: the
  // Host owns no list of people, and a transport that decided who was welcome
  // would be a second place for that decision to live. Checked before the world is
  // built, so refusing somebody costs nothing.
  if (hooks_.mayJoin) {
    std::string reason;
    if (!hooks_.mayJoin(m.playerId, name, reason)) {
      drop(st, reason.empty() ? "You may not join this world" : reason);
      return;
    }
  }
  st.greeted = true;
  st.playerId = m.playerId;
  st.name = name;
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

  // Everybody's level, to the newcomer. Its completion popup filters on its own,
  // and /list shows everyone's, so both have to arrive before the first thing is
  // typed. The host's own goes too: a guest that did not know the host was the
  // owner would show them as an ordinary player in the list.
  if (hooks_.levelOf) {
    PermissionMsg mine;
    mine.playerId = playerId_;
    mine.level = hooks_.levelOf(playerId_, name_);
    sendTo(st.peer, MsgType::Permission, mine);
    // `st` is already active by this point, so this covers the newcomer too — which
    // is what tells it its OWN level, the one thing it cannot work out for itself.
    for (const PeerState& other : peers_) {
      if (!other.active || other.playerId.empty()) continue;
      PermissionMsg p;
      p.playerId = other.playerId;
      p.level = hooks_.levelOf(other.playerId, other.name);
      sendTo(st.peer, MsgType::Permission, p);
      // And the newcomer's own level to everybody else, so their rosters agree.
      if (&other == &st) broadcast(MsgType::Permission, p, st.peer);
    }
  }

  NotifyMsg note;
  note.message = st.name + " joined the world";
  broadcast(MsgType::Notify, note, st.peer);
  if (hooks_.notify) hooks_.notify(note.message);
  // The same event in the chat log, where it stays. A toast is gone in two
  // seconds, and "when did Ada leave" is a question a log can answer.
  announceChat(note.message);
  if (hooks_.onRosterChange) hooks_.onRosterChange();
}

void Host::announceChat(const std::string& text) {
  broadcastChatLine(1 /*ui::Chat::Kind::System*/, {}, text);
  // And to the host's own box. The host is not one of `peers_`, so a broadcast
  // reaches everybody except the person running the world — which is how the
  // guests came to have a record of who joined and the host did not.
  if (hooks_.onChatShow) hooks_.onChatShow(1, {}, text);
}

void Host::sendChatLine(const std::string& playerId, std::uint8_t kind, const std::string& from,
                        const std::string& text) {
  PeerState* st = peerForPlayer(playerId);
  if (!st || !st->active) return;
  ChatLineMsg m;
  m.kind = kind;
  m.from = from;
  m.text = text;
  sendTo(st->peer, MsgType::ChatLine, m);
}

void Host::broadcastChatLine(std::uint8_t kind, const std::string& from,
                             const std::string& text) {
  ChatLineMsg m;
  m.kind = kind;
  m.from = from;
  m.text = text;
  broadcast(MsgType::ChatLine, m);
}

void Host::broadcastPermission(const std::string& playerId, std::uint8_t level) {
  if (!validPlayerId(playerId)) return;
  PermissionMsg m;
  m.playerId = playerId;
  m.level = level;
  broadcast(MsgType::Permission, m);
}

bool Host::kick(const std::string& playerId, const std::string& reason) {
  PeerState* st = peerForPlayer(playerId);
  if (!st) return false;
  drop(*st, reason.empty() ? "You were removed from this world" : reason);
  return true;
}

bool Host::teleportPlayer(const std::string& playerId, const Vec3& to) {
  PeerState* st = peerForPlayer(playerId);
  if (!st || !st->active) return false;
  TeleportMsg m;
  m.pos = to;
  sendTo(st->peer, MsgType::Teleport, m);
  // The same grace a wayshard gets, and for the same reason: the guest is about to
  // move further in one step than any speed allows, and the movement check would
  // otherwise answer its own teleport with a teleport back. Set here rather than
  // waiting for the guest to say so, because we are the one who moved it.
  st->havePose = false;
  st->moveGraceUntil = now_ + kMoveGrace;
  st->lastPose = to;
  return true;
}

bool Host::givePlayer(const std::string& playerId, const std::string& key, int count, int dura) {
  PeerState* st = peerForPlayer(playerId);
  if (!st || !st->active) return false;
  GiveMsg m;
  m.key = key;
  m.count = count;
  m.dura = dura;
  sendTo(st->peer, MsgType::Give, m);
  return true;
}

bool Host::setPlayerState(const std::string& playerId, float health, bool clearInventory) {
  PeerState* st = peerForPlayer(playerId);
  if (!st || !st->active) return false;
  SetStateMsg m;
  m.health = health;
  m.clearInventory = clearInventory;
  sendTo(st->peer, MsgType::SetState, m);
  return true;
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
  // kUntinted on the wire as -1. The two spellings of "no dye" have to be kept
  // apart here rather than further in: a guest receiving 0xFFFFFF would install an
  // explicit white tint on a cell that has none, and that entry would then be
  // written into the save forever.
  {
    const std::uint32_t t = game_.world->tintAt(m.x, m.y, m.z);
    back.tint = t == world::World::kUntinted ? -1 : static_cast<std::int32_t>(t);
  }
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
  game_.world->applyRemoteEdit(m.x, m.y, m.z, static_cast<world::BlockId>(m.id), m.meta,
                               m.tint);

  // A station carries state from the moment it exists, and that state belongs to
  // the host. Interact::tryPlace does exactly this for a block the local player
  // puts down; nothing did it for a block that arrived over the wire, so a chest a
  // guest placed existed on the host as a block and nowhere as a container.
  //
  // Everything downstream then failed in a way that pointed at the wrong thing.
  // onBeRequest found nothing and answered "Nothing there"; because the request
  // failed no lock was taken, so onBeState quietly discarded every write the guest
  // sent when it closed the screen; and when the chest was broken spillBlockEntity
  // had nothing to spill. Meanwhile the guest's own screen worked perfectly,
  // because it was reading the copy in its own world the whole time — which is why
  // this looked like a container that worked and then stopped rather than one that
  // was never really there.
  const game::BlockEntityKind kind =
      game::entityKindFor(world::blocks().def(static_cast<world::BlockId>(m.id)).key);
  if (kind != game::BlockEntityKind::None) {
    game_.world->getOrCreateBlockEntity(m.x, m.y, m.z, kind);
  }

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
        // s.tint, not the default. A dyed thing shut inside a chest that is then
        // broken comes back out of it the colour it went in.
        game_.world->spawnDrop(x + 0.5f, y + 0.5f, z + 0.5f, s.key, s.count, s.dura, s.tint);
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
                                     int dura, std::int32_t tint) {
    GiveMsg award;
    award.key = key;
    award.count = count;
    award.dura = dura;
    award.tint = tint;
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
    boat->data.remoteRider = true;
    boatRiders_[boat->id] = st.playerId;
  } else {
    // Only the person in it may get out of it. Without this a guest could stand
    // anybody else up out of their boat by asking, from anywhere in the world —
    // the reach check above is on mounting, and there is no reach at which
    // ejecting somebody else is reasonable.
    const auto seat = boatRiders_.find(boat->id);
    if (seat != boatRiders_.end()) {
      if (seat->second != st.playerId) return;
      boatRiders_.erase(seat);
    }
    boat->data.rider = false;
    boat->data.remoteRider = false;
  }
}

// Where the boats somebody else is steering have got to.
//
// A ridden boat is simulated by whoever is in it, for the same reason their body
// is: it moves where they move and only their keyboard turns it. So the host does
// not run its update hook (boat.cpp returns early on remoteRider) and instead
// reads its position back out of the one thing the rider already sends twenty
// times a second — their pose. The seat offset is the whole conversion, which is
// why this needs no message of its own and no trust the pose did not already have.
void Host::syncRiddenBoats() {
  if (!game_.entities || boatRiders_.empty()) return;
  for (game::Entity& e : game_.entities->all()) {
    if (e.dead || e.ghost || e.type != game::EntityType::Boat || !e.data.remoteRider) continue;
    const auto seat = boatRiders_.find(e.id);
    if (seat == boatRiders_.end()) continue;
    const PeerState* st = peerForPlayer(seat->second);
    if (!st || !st->active || !st->havePose) continue;
    e.pos = Vec3{st->lastPose.x, st->lastPose.y - game::kBoatSeatY, st->lastPose.z};
    // Zeroed rather than left alone: the manager still sweeps this entity every
    // tick, and a stale velocity would drag the hull away from the seat it is
    // being placed under before the next pose put it back.
    e.vel = Vec3{};
    e.yaw = st->lastYaw;
  }
}

void Host::releaseBoats(const std::string& playerId) {
  if (!game_.entities) return;
  for (auto it = boatRiders_.begin(); it != boatRiders_.end();) {
    if (it->second != playerId) {
      ++it;
      continue;
    }
    if (game::Entity* boat = game_.entities->byId(it->first)) {
      boat->data.rider = false;
      boat->data.remoteRider = false;
    }
    it = boatRiders_.erase(it);
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
  game_.entities->spawnTossed(m.pos, m.dir, m.key, m.count, m.dura, m.tint);
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
        w.tint = slot.tint;
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
  // The block decides, not the guest and not what happens to be in the map. A
  // station block with no container behind it is a container that has not been
  // made yet, which is exactly how the local path treats one — tryPlace and this
  // both go through getOrCreate for the same reason.
  //
  // It also repairs a world rather than only stopping the damage. Every chest a
  // guest placed before onEdit learned to make one is sitting in somebody's save
  // right now as a block with nothing behind it, and would go on answering
  // "Nothing there" forever however many times they reloaded. Opening it makes it
  // real. The allocation is not something a peer can abuse into anything: it
  // happens only where the world already holds a station block, so the ceiling is
  // the number of stations in the world.
  const game::BlockEntityKind kind =
      game::entityKindFor(world::blocks().def(game_.world->getBlock(m.x, m.y, m.z)).key);
  game::BlockEntity* be = kind == game::BlockEntityKind::None
                              ? nullptr
                              : game_.world->getOrCreateBlockEntity(m.x, m.y, m.z, kind);
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
  state.container = toWire(be->container);
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
      out.tint = s.tint;
      return out;
    };
    const auto copySlots = [&] {
      for (std::size_t i = 0; i < be->slots.size(); ++i) {
        be->slots[i] = i < m.slots.size() ? fromWire(m.slots[i]) : game::ItemStack{};
      }
    };
    if (be->kind == game::BlockEntityKind::Forge) {
      be->input = fromWire(m.input);
      be->fuel = fromWire(m.fuel);
      be->output = fromWire(m.output);
    } else if (be->kind == game::BlockEntityKind::Chest) {
      copySlots();
    } else if (game::isKitchen(be->kind)) {
      // This branch did not exist. The three kitchens fell off the end of the if and
      // the host wrote back NOTHING, so a guest who loaded a cooking pot with six
      // vegetables and a bowl and closed the window handed them all to the void: gone
      // from the guest's inventory, never once written on the host, and wiped from
      // the guest's own copy the next time the host re-sent the real state.
      be->input = fromWire(m.input);
      be->fuel = fromWire(m.fuel);
      be->output = fromWire(m.output);
      be->container = fromWire(m.container);
      copySlots();
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

void Host::onLocalEdit(int x, int y, int z, std::uint16_t id, std::uint8_t meta, int tint) {
  if (!transport_.active()) return;
  EditMsg m;
  m.x = x;
  m.y = y;
  m.z = z;
  m.id = id;
  m.meta = meta;
  m.tint = tint;
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
      give.tint = e.data.tint;
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
  const float now = game_.sky ? game_.sky->time : 0.32f;
  // One number for the whole round. Each peer still sees it strictly increasing,
  // which is all newerSeq asks of it, and a shared counter means two guests cannot
  // be told contradictory things about which snapshot is the newer one.
  const std::uint32_t seq = ++snapSeq_;

  // Everyone in the world, built once: the player list is the same for all of them
  // and is never the part that has to be cut. Seven guests plus the host is a
  // couple of hundred bytes, and a player dropped from a snapshot is a person who
  // vanishes — which is exactly the failure this whole pass exists to stop.
  std::vector<SnapPlayer> everyone;
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
    everyone.push_back(std::move(self));
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
    everyone.push_back(std::move(p));
  }
  std::size_t playerBytes = 0;
  for (const SnapPlayer& p : everyone) playerBytes += wireSize(p);

  // Every entity that could be sent to anyone, encoded once. Which of them each
  // guest is actually told about is decided below and differs per guest; what an
  // entity looks like does not.
  std::vector<SnapEntity> pool;
  pool.reserve(game_.entities->all().size());
  for (const game::Entity& e : game_.entities->all()) {
    if (e.dead || e.ghost) continue;
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
        // And what colour it is, for the same reason: a dyed thing lying on the
        // ground is not described by its key alone.
        out.tint = e.data.tint;
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
    pool.push_back(std::move(out));
  }

  // Where each guest is standing, so "near me" means near them. One that has not
  // sent a pose yet is still arriving; the host's own position is the best guess
  // available and is where they are about to appear anyway.
  const Vec3 fallback = game_.player ? game_.player->pos() : Vec3{};

  std::vector<std::pair<float, std::size_t>> near;
  for (const PeerState& st : peers_) {
    if (!st.active) continue;
    const Vec3 eye = st.havePose ? st.lastPose : fallback;

    near.clear();
    for (std::size_t i = 0; i < pool.size(); ++i) {
      const float d = dist3(eye, pool[i].pos);
      if (d > kSnapRange) continue;
      near.emplace_back(d, i);
    }
    // Nearest first, so what survives the budget is what this guest can see, reach
    // and be hurt by. The old census took whatever came first in the entity
    // vector, which is oldest first — and a drop in an unloaded chunk never ages,
    // so a long session accumulated a queue of frozen items that filled the
    // allowance and left the mob standing next to you out of the message
    // altogether. A guest treats anything a snapshot does not mention as gone, so
    // it did not merely fail to update that mob: it deleted it.
    std::sort(near.begin(), near.end(),
              [](const std::pair<float, std::size_t>& a,
                 const std::pair<float, std::size_t>& b) { return a.first < b.first; });

    SnapshotMsg snap;
    snap.time = now;
    snap.seq = seq;
    snap.players = everyone;
    std::size_t used = snapshotOverhead() + playerBytes;
    for (const auto& [d, i] : near) {
      const std::size_t cost = wireSize(pool[i]);
      if (used + cost > kSnapBudget) break;
      used += cost;
      snap.entities.push_back(pool[i]);
      // The protocol's own ceiling, which the budget reaches long before in any
      // world worth the check. Kept so the encoder and the decoder cannot disagree.
      if (snap.entities.size() >= 512) break;
    }
    sendTo(st.peer, MsgType::Snapshot, snap, Channel::Fast);
  }
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
