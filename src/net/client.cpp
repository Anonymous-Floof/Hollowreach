#include "net/client.h"

#include "ui/settings.h"

#include <algorithm>

#include "audio/sfx.h"
#include "core/log.h"
#include "game/blockentities.h"
#include "game/inventory.h"
#include "game/items.h"
#include "game/player.h"
#include "render/sky.h"
#include "save/format.h"
#include "world/blocks.h"
#include "world/world.h"

namespace hr::net {
namespace {

// A guest's own pose. 20 Hz, matched to the host's snapshot rate: sending more
// often than the host relays only adds a sample the host throws away, and less
// often makes the guest the coarsest link in the chain the ghosts buffer for.
constexpr double kPosePeriod = 1.0 / 20.0;
constexpr double kStatePeriod = 10.0;        // inventory and player, for the host's save
constexpr double kConnectTimeout = 8.0;

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
void Client::send(MsgType type, const T& msg, Channel channel) {
  if (host_ == kNoPeer) return;
  ByteWriter w;
  begin(w, type);
  encode(w, msg);
  transport_.send(host_, channel, w.data());
}

void Client::sendEmpty(MsgType type, Channel channel) {
  if (host_ == kNoPeer) return;
  ByteWriter w;
  begin(w, type);
  transport_.send(host_, channel, w.data());
}

bool Client::start(const std::string& address, std::uint16_t port, const std::string& playerId,
                   const std::string& name, SessionHooks hooks, std::string* error) {
  if (!transport_.connect(address, port, error)) return false;
  hooks_ = std::move(hooks);
  playerId_ = playerId;
  name_ = name;
  state_ = State::Connecting;
  failure_.clear();
  roster_.clear();
  ghosts_.clear();
  host_ = transport_.peers().empty() ? kNoPeer : transport_.peers().front();
  connectTimer_ = 0;
  poseTimer_ = stateTimer_ = 0;
  poseSeq_ = lastSnapSeq_ = 0;
  haveSnapSeq_ = false;
  return true;
}

void Client::attachGame(const GameRefs& game) {
  game_ = game;
  ghosts_.attach(game_.entities);
}

void Client::stop(bool sayGoodbye) {
  if (sayGoodbye && transport_.active() && state_ == State::Playing) {
    // The host keeps this guest's progress, so the last state goes out with the
    // goodbye rather than being lost to whatever the last periodic send held.
    sendPlayerState();
    sendEmpty(MsgType::Bye);
  }
  transport_.close();
  ghosts_.clear();
  roster_.clear();
  host_ = kNoPeer;
  state_ = State::Idle;
}

std::uint32_t Client::pingMs() const {
  return host_ == kNoPeer ? 0 : transport_.roundTripMs(host_);
}

void Client::update(double dt, double now) {
  if (!transport_.active()) return;

  transport_.poll(events_);
  for (TransportEvent& e : events_) {
    switch (e.kind) {
      case TransportEvent::Kind::Connected:
        host_ = e.peer;
        state_ = State::Handshaking;
        log::info("net: connected, sending hello as \"%s\"", name_.c_str());
        sendHello();
        break;
      case TransportEvent::Kind::Disconnected: {
        const std::string why = failure_;
        state_ = failure_.empty() ? State::Idle : State::Failed;
        host_ = kNoPeer;
        if (hooks_.onDisconnected) hooks_.onDisconnected(why);
        return;
      }
      case TransportEvent::Kind::Message:
        onMessage(e.data.data(), e.data.size(), now);
        break;
    }
  }

  if (state_ == State::Connecting || state_ == State::Handshaking) {
    connectTimer_ += dt;
    if (connectTimer_ > kConnectTimeout) {
      failure_ = state_ == State::Connecting ? "No answer from that address"
                                             : "The host did not send a world";
      state_ = State::Failed;
      transport_.close();
      host_ = kNoPeer;
      log::warn("net: %s", failure_.c_str());
      if (hooks_.onDisconnected) hooks_.onDisconnected(failure_);
      return;
    }
  }

  if (state_ != State::Playing) return;

  ghosts_.update(now);

  poseTimer_ += dt;
  if (poseTimer_ >= kPosePeriod) {
    poseTimer_ = 0;
    sendPose();
  }
  stateTimer_ += dt;
  if (stateTimer_ >= kStatePeriod) {
    stateTimer_ = 0;
    sendPlayerState();
  }
}

void Client::sendHello() {
  HelloMsg m;
  m.version = kNetVersion;
  m.name = name_;
  m.playerId = playerId_;
  send(MsgType::Hello, m);
}

void Client::sendPose() {
  if (!game_.player) return;
  PoseMsg m;
  m.pos = game_.player->pos();
  m.vel = game_.player->velocity();
  m.yaw = game_.player->yaw();
  m.pitch = game_.player->pitch();
  m.health = game_.player->health();
  m.flags = static_cast<std::uint8_t>((game_.player->swimming() ? 1 : 0) |
                                      (game_.player->sneaking() ? 2 : 0) |
                                      (game_.player->flying() ? 4 : 0) |
                                      (game_.player->sprinting() ? 8 : 0));
  m.seq = ++poseSeq_;
  send(MsgType::Pose, m, Channel::Fast);
}

void Client::sendPlayerState() {
  if (!game_.player || !game_.inventory) return;
  PlayerStateMsg m;
  const game::PlayerState s = game_.player->state();
  m.pos = s.pos;
  m.yaw = s.yaw;
  m.pitch = s.pitch;
  m.health = s.health;
  m.hunger = s.hunger;
  m.saturation = s.saturation;
  m.flying = s.flying;
  for (const game::ItemStack& slot : game_.inventory->slots()) m.slots.push_back(toWire(slot));
  for (const game::ItemStack& slot : game_.inventory->armor()) m.armor.push_back(toWire(slot));
  m.selected = game_.inventory->selected();
  // Where this player wakes up. The fields have been on the wire since the first
  // multiplayer build and nothing ever filled them in, so the host stored a
  // hasSpawn of false for every guest it had ever met: a guest could bind a Soul
  // Anchor, and the moment they left and came back it was as if they never had.
  m.hasSpawn = spawn_.bound;
  m.spawn = spawn_.at;
  send(MsgType::PlayerState, m);
}

void Client::onMessage(const std::uint8_t* data, std::size_t size, double now) {
  const MsgType type = peekType(data, size);
  if (type == MsgType::None) return;
  // The world payload is the one message that is legitimately enormous: it is the
  // entire save, which the host bounds with kMaxWorldBytes rather than kMaxMessage.
  // Holding it to the 64 KB cap silently dropped every world bigger than that —
  // about a minute of play — and the guest then sat through the handshake timeout
  // reporting that the host had sent nothing, which is exactly what it looked like
  // from the outside. Every other type keeps the small cap, so a peer still cannot
  // use an ordinary message to make this end allocate megabytes.
  const std::size_t cap = type == MsgType::World ? kMaxWorldBytes + kMaxMessage : kMaxMessage;
  if (size > cap) return;
  ByteReader r(data, size);
  r.skip(1);

  switch (type) {
    case MsgType::Reject: {
      RejectMsg m;
      if (decode(r, m)) failure_ = m.reason;
      log::warn("net: host refused the connection: %s", failure_.c_str());
      break;
    }
    case MsgType::World: {
      WorldMsg m;
      if (!decode(r, m)) {
        failure_ = "The host sent a world this build cannot read";
        break;
      }
      save::WorldSave world;
      std::string error;
      if (!save::decode(m.save.data(), m.save.size(), world, &error)) {
        failure_ = "World rejected: " + error;
        log::error("net: %s", failure_.c_str());
        break;
      }
      // The host's world is not this player's to keep: the id is cleared so
      // nothing downstream can mistake it for a local save.
      world.meta.id.clear();
      if (hooks_.adoptWorld) hooks_.adoptWorld(world);
      state_ = State::Playing;
      sendEmpty(MsgType::Ready);
      log::info("net: joined, %zu bytes of world applied", m.save.size());
      break;
    }
    case MsgType::Snapshot: {
      SnapshotMsg m;
      if (!decode(r, m)) break;
      // Older than one already applied, or a duplicate of it. A snapshot is a
      // complete census — every entity that exists, and by omission every one
      // that does not — so applying a stale one does not merely repeat old news,
      // it un-picks-up items, un-kills mobs and walks everybody backwards. See
      // SnapshotMsg::seq.
      if (haveSnapSeq_ && !newerSeq(m.seq, lastSnapSeq_)) break;
      lastSnapSeq_ = m.seq;
      haveSnapSeq_ = true;
      // The host's own player is in here; drawing a ghost of ourselves would be a
      // second body standing where we are.
      m.players.erase(std::remove_if(m.players.begin(), m.players.end(),
                                     [this](const SnapPlayer& p) {
                                       return p.playerId == playerId_;
                                     }),
                      m.players.end());
      ghosts_.feedSnapshot(m, now);
      if (game_.sky) game_.sky->time = m.time;
      break;
    }
    case MsgType::Time: {
      TimeMsg m;
      if (!decode(r, m) || !game_.sky) break;
      game_.sky->time = m.time;
      // The host jumped the clock for a sleep everyone agreed to, so everyone who
      // agreed wakes rested. A guest's own tiredness counter is advancing locally
      // and has no other way to learn that the night it just skipped was slept.
      if (m.sleeping) game_.sky->markRested();
      break;
    }
    case MsgType::WorldSettings: {
      WorldSettingsMsg m;
      if (!decode(r, m)) break;
      // The host's world, the host's rules. Re-installed wholesale rather than
      // merged, so a setting the host turned back to its default arrives as the
      // default instead of lingering at whatever it was.
      ui::settings().beginWorld(m.values);
      ui::settings().setWorldLocked(true);
      break;
    }
    case MsgType::SleepState: {
      SleepStateMsg m;
      if (!decode(r, m)) break;
      sleepActive_ = m.active;
      sleepTarget_ = m.target;
      sleepProposer_ = m.proposer;
      break;
    }
    // Every one of these applies through applyRemoteEdit rather than setBlock, and
    // the difference is not cosmetic. A guest's world carries an edit sink that
    // sends whatever is written to the host for approval; writing the host's own
    // edits through it sent them straight back where they came from. The host then
    // accepted each one, relayed it to everyone, and the guest echoed it again —
    // a loop with nothing to damp it, because setBlock does not skip a write that
    // changes nothing. Within a second or two of the first edit anywhere in the
    // world that traffic drained the guest's edit budget, and from then on the
    // host answered the player's real edits with a denial: a placed block appeared
    // and vanished, and a broken one came back.
    case MsgType::Edits: {
      EditsMsg m;
      if (!decode(r, m) || !game_.world) break;
      const world::BlockRegistry& blocks = world::BlockRegistry::get();
      for (const EditMsg& e : m.list) {
        if (e.id >= blocks.count()) continue;
        game_.world->applyRemoteEdit(e.x, e.y, e.z, static_cast<world::BlockId>(e.id), e.meta);
      }
      break;
    }
    case MsgType::Edit:
    case MsgType::EditDeny: {
      EditMsg m;
      if (!decode(r, m) || !game_.world) break;
      const world::BlockRegistry& blocks = world::BlockRegistry::get();
      if (m.id >= blocks.count()) break;
      // A denial and an authoritative edit are the same operation: put the cell
      // back to what the host says it is. A denial especially must not go back out
      // — that is the rejected edit being sent for a second refusal.
      game_.world->applyRemoteEdit(m.x, m.y, m.z, static_cast<world::BlockId>(m.id), m.meta);
      break;
    }
    case MsgType::Teleport: {
      TeleportMsg m;
      if (decode(r, m) && game_.player) {
        game_.player->setPos(m.pos);
        game_.player->setVelocity({0, 0, 0});
      }
      break;
    }
    case MsgType::Give: {
      GiveMsg m;
      if (decode(r, m) && game_.inventory && game::getItem(m.key)) {
        const int left = game_.inventory->give(m.key, m.count, m.dura);
        if (left < m.count) audio::sfx::pickup();
        // A full bag. The host has already destroyed the drop on its side, so
        // anything that would not fit has to go back into the world or it simply
        // stops existing — which is how an award for a kill, or a stack the host
        // handed over when we walked onto it, quietly vanished.
        if (left > 0 && game_.player) {
          const Vec3 p = game_.player->pos();
          sendToss(Vec3{p.x, p.y + 0.6f, p.z}, Vec3{0, 0, 0}, m.key, left, m.dura);
        }
      }
      break;
    }
    case MsgType::Damage: {
      DamageMsg m;
      if (decode(r, m) && game_.player) {
        game::PlayerOptions options;
        options.defense =
            static_cast<float>(game_.inventory ? game_.inventory->totalDefense() : 0);
        game_.player->damage(m.amount, options);
        const Vec3 v = game_.player->velocity();
        game_.player->setVelocity({v.x + m.knockback.x, m.knockback.y, v.z + m.knockback.z});
      }
      break;
    }
    case MsgType::Sfx: {
      SfxMsg m;
      if (decode(r, m) && hooks_.playSfx) hooks_.playSfx(m.kind, m.pos);
      break;
    }
    case MsgType::PlayerJoin: {
      PlayerJoinMsg m;
      if (!decode(r, m)) break;
      // The first join the host sends is its own, which is how a guest learns who
      // is in charge without a field for it.
      if (hostPlayerId_.empty()) hostPlayerId_ = m.playerId;
      auto it = std::find_if(roster_.begin(), roster_.end(), [&m](const Member& x) {
        return x.playerId == m.playerId;
      });
      if (it != roster_.end()) {
        it->name = m.name;
      } else {
        roster_.push_back(Member{m.playerId, m.name});
      }
      ghosts_.addPlayer(m.playerId, m.name);
      if (hooks_.onRosterChange) hooks_.onRosterChange();
      break;
    }
    case MsgType::PlayerLeave: {
      PlayerLeaveMsg m;
      if (!decode(r, m)) break;
      roster_.erase(std::remove_if(roster_.begin(), roster_.end(),
                                   [&m](const Member& x) { return x.playerId == m.playerId; }),
                    roster_.end());
      ghosts_.removePlayer(m.playerId);
      if (hooks_.onRosterChange) hooks_.onRosterChange();
      break;
    }
    case MsgType::Notify: {
      NotifyMsg m;
      if (decode(r, m) && hooks_.notify) hooks_.notify(m.message);
      break;
    }
    case MsgType::Painting: {
      PaintingMsg m;
      if (!decode(r, m) || !game_.world) break;
      game::Painting art;
      art.rgb = std::move(m.rgb);
      game_.world->setPainting(m.x, m.y, m.z, art);
      break;
    }
    case MsgType::BeState: {
      BeStateMsg m;
      if (!decode(r, m) || !game_.world) break;
      const auto kind = static_cast<game::BlockEntityKind>(m.kind);
      game::BlockEntity* be = game_.world->getOrCreateBlockEntity(m.x, m.y, m.z, kind);
      if (!be) break;
      const auto fromWire = [](const WireSlot& s) {
        game::ItemStack out;
        if (s.key.empty() || s.count <= 0 || !game::getItem(s.key)) return out;
        out.key = s.key;
        out.count = s.count;
        out.dura = s.dura;
        return out;
      };
      if (kind == game::BlockEntityKind::Forge) {
        be->input = fromWire(m.input);
        be->fuel = fromWire(m.fuel);
        be->output = fromWire(m.output);
        be->fuelLeft = m.fuelLeft;
        be->fuelMax = m.fuelMax;
        be->progress = m.progress;
      } else if (kind == game::BlockEntityKind::Chest) {
        for (std::size_t i = 0; i < be->slots.size(); ++i) {
          be->slots[i] = i < m.slots.size() ? fromWire(m.slots[i]) : game::ItemStack{};
        }
      }
      break;
    }
    case MsgType::BeDeny: {
      BeDenyMsg m;
      if (decode(r, m) && hooks_.notify) hooks_.notify(m.reason);
      break;
    }
    case MsgType::BoatDeny:
      // The mount was refused. Nothing to undo — the guest asked and did not get
      // it, and its own player was never seated.
      break;
    case MsgType::Pong:
    case MsgType::Ping: break;
    case MsgType::Bye:
      failure_.clear();
      transport_.disconnect(host_);
      break;
    default: break;
  }
}

void Client::sendEdit(int x, int y, int z, std::uint16_t id, std::uint8_t meta) {
  if (state_ != State::Playing) return;
  EditMsg m;
  m.x = x;
  m.y = y;
  m.z = z;
  m.id = id;
  m.meta = meta;
  send(MsgType::Edit, m);
}

void Client::sendHit(int entityId, const std::string& held, bool crit) {
  if (state_ != State::Playing) return;
  HitMsg m;
  m.entityId = entityId;
  m.held = held;
  m.crit = crit;
  send(MsgType::Hit, m);
}

void Client::sendPlayerHit(const std::string& playerId, const std::string& held, bool crit) {
  if (state_ != State::Playing) return;
  PlayerHitMsg m;
  m.playerId = playerId;
  m.held = held;
  m.crit = crit;
  send(MsgType::PlayerHit, m);
}

void Client::sendToss(const Vec3& pos, const Vec3& dir, const std::string& key, int count,
                      int dura) {
  if (state_ != State::Playing) return;
  TossMsg m;
  m.pos = pos;
  m.dir = dir;
  m.key = key;
  m.count = count;
  m.dura = dura;
  send(MsgType::Toss, m);
}

void Client::sendBoatSpawn(const Vec3& pos) {
  if (state_ != State::Playing) return;
  BoatSpawnMsg m;
  m.pos = pos;
  send(MsgType::BoatSpawn, m);
}

void Client::sendBoatMount(int entityId, bool on) {
  if (state_ != State::Playing) return;
  BoatMountMsg m;
  m.entityId = entityId;
  m.on = on;
  send(MsgType::BoatMount, m);
}

void Client::sendSleep(bool on, float target) {
  if (state_ != State::Playing) return;
  SleepMsg m;
  m.on = on;
  m.target = target;
  send(MsgType::Sleep, m);
}

void Client::sendWarp() {
  if (state_ != State::Playing) return;
  sendEmpty(MsgType::Warp);
}

void Client::sendBlockEntityRequest(int x, int y, int z, std::uint8_t kind) {
  if (state_ != State::Playing) return;
  BeRequestMsg m;
  m.x = x;
  m.y = y;
  m.z = z;
  m.kind = kind;
  send(MsgType::BeRequest, m);
}

void Client::sendPainting(int x, int y, int z, const game::Painting& art) {
  if (state_ != State::Playing || art.blank()) return;
  PaintingMsg m;
  m.x = x;
  m.y = y;
  m.z = z;
  m.rgb = art.rgb;
  send(MsgType::Painting, m);
}

void Client::sendBlockEntityState(const BeStateMsg& state) {
  if (state_ != State::Playing) return;
  send(MsgType::BeState, state);
}

std::vector<RosterEntry> Client::roster() const {
  std::vector<RosterEntry> out;
  for (const Member& m : roster_) {
    RosterEntry e;
    e.playerId = m.playerId;
    e.name = m.name;
    e.host = m.playerId == hostPlayerId_;
    if (e.host) e.pingMs = pingMs();
    out.push_back(std::move(e));
  }
  RosterEntry self;
  self.playerId = playerId_;
  self.name = name_;
  self.self = true;
  self.pingMs = pingMs();
  out.push_back(std::move(self));
  return out;
}

}  // namespace hr::net
