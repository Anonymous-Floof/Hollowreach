#include "net/ghosts.h"

#include <algorithm>
#include <cmath>

namespace hr::net {
namespace {

constexpr float kPi = 3.14159265358979f;

// Shortest way round the circle, so a remote player turning past north does not
// spin the long way to catch up.
float lerpAngle(float a, float b, float t) {
  float d = b - a;
  while (d > kPi) d -= 2.0f * kPi;
  while (d < -kPi) d += 2.0f * kPi;
  return a + d * t;
}

Vec3 lerpVec(const Vec3& a, const Vec3& b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

}  // namespace

void Ghosts::Track::push(const Vec3& p, float y, float pit, double now) {
  if (!primed) {
    prevPos = pos = p;
    prevYaw = yaw = y;
    prevPitch = pitch = pit;
    prevTime = time = now;
    primed = true;
    return;
  }
  // Out of order or duplicated: the older sample tells us nothing we do not
  // already have, and accepting it would drag the ghost backwards.
  if (now <= time) return;
  prevPos = pos;
  prevYaw = yaw;
  prevPitch = pitch;
  prevTime = time;
  pos = p;
  yaw = y;
  pitch = pit;
  time = now;
}

void Ghosts::Track::sample(double at, Vec3& outPos, float& outYaw, float& outPitch) const {
  if (!primed) {
    outPos = pos;
    outYaw = yaw;
    outPitch = pitch;
    return;
  }
  const double span = time - prevTime;
  // A single sample, or two that arrived together: nothing to interpolate along.
  if (span <= 1e-6) {
    outPos = pos;
    outYaw = yaw;
    outPitch = pitch;
    return;
  }
  // Clamped rather than extrapolated. Extrapolating a player who stopped sending
  // walks them through a wall; freezing them where they were is wrong for a
  // sixth of a second and right afterwards.
  const double u = std::clamp((at - prevTime) / span, 0.0, 1.0);
  const float t = static_cast<float>(u);
  outPos = lerpVec(prevPos, pos, t);
  outYaw = lerpAngle(prevYaw, yaw, t);
  outPitch = prevPitch + (pitch - prevPitch) * t;
}

void Ghosts::clear() {
  players_.clear();
  entityGhosts_.clear();
  nameplates_.clear();
  nextNetId_ = kPlayerNetBase;
}

int Ghosts::netIdFor(const std::string& playerId) {
  auto it = players_.find(playerId);
  if (it != players_.end() && it->second.netId != 0) return it->second.netId;
  return nextNetId_++;
}

void Ghosts::addPlayer(const std::string& playerId, const std::string& name) {
  PlayerGhost& g = players_[playerId];
  g.name = name;
  if (g.netId == 0) g.netId = nextNetId_++;
}

void Ghosts::removePlayer(const std::string& playerId) {
  auto it = players_.find(playerId);
  if (it == players_.end()) return;
  if (entities_) {
    for (game::Entity& e : entities_->all()) {
      if (e.ghost && e.netId == it->second.netId) e.dead = true;
    }
  }
  players_.erase(it);
}

void Ghosts::feedPlayerPose(const std::string& playerId, const std::string& name,
                            const PoseMsg& pose, double now) {
  PlayerGhost& g = players_[playerId];
  if (g.netId == 0) g.netId = netIdFor(playerId);
  if (!name.empty()) g.name = name;
  g.health = pose.health;
  g.flags = pose.flags;
  g.track.push(pose.pos, pose.yaw, pose.pitch, now);
}

void Ghosts::feedSnapshot(const SnapshotMsg& snap, double now) {
  for (const SnapPlayer& p : snap.players) {
    PlayerGhost& g = players_[p.playerId];
    if (g.netId == 0) g.netId = nextNetId_++;
    g.health = p.health;
    g.flags = p.flags;
    g.hurt = p.hurt;
    g.track.push(p.pos, p.yaw, p.pitch, now);
  }

  for (auto& [id, ghost] : entityGhosts_) ghost.seen = false;
  for (const SnapEntity& e : snap.entities) {
    EntityGhost& g = entityGhosts_[e.id];
    g.type = e.type;
    g.a = e.a;
    g.b = e.b;
    g.seen = true;
    g.track.push(e.pos, e.yaw, 0.0f, now);
  }
  // Anything the host did not mention is gone — it died, despawned or was picked
  // up. Dropping it here is what keeps a guest's world from filling with the
  // corpses of entities that no longer exist.
  for (auto it = entityGhosts_.begin(); it != entityGhosts_.end();) {
    if (it->second.seen) {
      ++it;
      continue;
    }
    if (entities_) {
      for (game::Entity& e : entities_->all()) {
        if (e.ghost && e.netId == it->first) e.dead = true;
      }
    }
    it = entityGhosts_.erase(it);
  }
}

bool Ghosts::playerPos(const std::string& playerId, Vec3& out) const {
  auto it = players_.find(playerId);
  if (it == players_.end() || !it->second.track.primed) return false;
  out = it->second.track.pos;
  return true;
}

void Ghosts::update(double now) {
  nameplates_.clear();
  if (!entities_) return;
  const double at = now - kInterpDelay;

  const auto findGhost = [this](int netId) -> game::Entity* {
    for (game::Entity& e : entities_->all()) {
      if (e.ghost && !e.dead && e.netId == netId) return &e;
    }
    return nullptr;
  };

  for (auto& [pid, g] : players_) {
    Vec3 pos;
    float yaw = 0, pitch = 0;
    g.track.sample(at, pos, yaw, pitch);

    game::Entity* body = findGhost(g.netId);
    if (!body) {
      body = entities_->spawnGhost(g.netId, game::EntityType::RemotePlayer, pos);
      if (!body) continue;
    }
    body->pos = pos;
    // The one place a *player's* yaw becomes an *entity's*, and the two do not
    // mean the same thing. Every model in the game is built facing +z and every
    // mob derives its yaw from where it is walking, but a player's yaw 0 looks
    // down -z (core/mat4.cpp lookDir) — half a turn apart, which is why two people
    // standing face to face each saw the other's back. The boat hit this first and
    // answered it by flipping its hull (render/entityrenderer.cpp:332); a body has
    // a front and a back that have to agree with its limbs, so the conversion
    // belongs here instead, where a pose crosses into the entity that draws it.
    // Only the heading turns: a rotation about y leaves up and down alone, so the
    // head's pitch bone still tips the face the way the remote player is looking.
    body->yaw = yaw + kPi;
    body->pitch = pitch;
    body->data.health = g.health;
    body->data.hurtFlash = g.hurt ? 0.25f : 0.0f;

    Nameplate plate;
    plate.playerId = pid;
    plate.name = g.name;
    plate.pos = pos;
    plate.health = g.health;
    nameplates_.push_back(std::move(plate));
  }

  for (auto& [id, g] : entityGhosts_) {
    Vec3 pos;
    float yaw = 0, pitch = 0;
    g.track.sample(at, pos, yaw, pitch);

    game::Entity* body = findGhost(id);
    if (!body) {
      const auto type = static_cast<game::EntityType>(g.type);
      if (type == game::EntityType::None || type >= game::EntityType::Count) continue;
      body = entities_->spawnGhost(id, type, pos);
      if (!body) continue;
    }
    body->pos = pos;
    body->yaw = yaw;
    // The two per-type extras, unpacked the way the host packed them: a drop's
    // stack count, a mob's health and hurt flash, a boat's rider flag.
    switch (body->type) {
      case game::EntityType::Drop:
        body->data.count = static_cast<int>(g.a);
        break;
      case game::EntityType::Boat:
        body->data.rider = g.a > 0.5f;
        break;
      default:
        body->data.health = g.a;
        body->data.hurtFlash = g.b;
        break;
    }
  }
}

}  // namespace hr::net
