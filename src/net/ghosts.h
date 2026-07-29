// Remote bodies: other players, and (on a guest) every entity the host owns.
//
// Ported from js/net/ghosts.js. A ghost is an entity the local simulation never
// touches — `Entity::ghost` is checked by the manager's tick and by every AI hook
// — so its position comes entirely from what arrives over the wire.
//
// The one idea worth stating: **render 150 ms in the past**. Snapshots arrive
// about ten times a second, and drawing the newest one directly means a remote
// player teleports 100 ms forward every time one lands. Holding two samples and
// interpolating between them at `now - 150 ms` costs a sixth of a second of
// latency and buys motion that is smooth at any packet rate, including one where
// packets arrive late or out of order. The web build chose the same number.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/mat4.h"
#include "game/entities/manager.h"
#include "net/protocol.h"

namespace hr::net {

// How far behind the newest sample a ghost is drawn.
inline constexpr double kInterpDelay = 0.150;

// Player ghosts are numbered from here up; entity ghosts keep the host's own
// entity id, which counts from 1. Without the offset the two share one namespace,
// and the host's first mob has the same ghost id as the first remote player — so
// the mob's update finds the player's body, writes into that, and the mob is never
// created at all. It looked exactly like "mobs do not replicate" and was nothing of
// the sort.
inline constexpr int kPlayerNetBase = 1'000'000;

class Ghosts {
 public:
  void attach(game::EntityManager* entities) { entities_ = entities; }
  void clear();

  // A guest applying the host's snapshot: entities and players both.
  void feedSnapshot(const SnapshotMsg& snap, double now);
  // A host recording one guest's pose, so its own ghost list can draw them.
  void feedPlayerPose(const std::string& playerId, const std::string& name, const PoseMsg& pose,
                      double now);

  void addPlayer(const std::string& playerId, const std::string& name);
  void removePlayer(const std::string& playerId);

  // Interpolates every ghost and writes it into the entity manager.
  void update(double now);

  // For nameplates: every remote player currently known, with where it is drawn.
  struct Nameplate {
    std::string playerId;
    std::string name;
    Vec3 pos;
    float health = 20.0f;
  };
  const std::vector<Nameplate>& nameplates() const { return nameplates_; }

  std::size_t playerCount() const { return players_.size(); }
  bool hasPlayer(const std::string& playerId) const {
    return players_.find(playerId) != players_.end();
  }
  // Where a remote player was last seen, for the host's reach checks.
  bool playerPos(const std::string& playerId, Vec3& out) const;

 private:
  // Two samples and the times they arrived. Anything older is of no use, and
  // anything newer has not happened yet.
  struct Track {
    Vec3 prevPos, pos;
    float prevYaw = 0, yaw = 0;
    float prevPitch = 0, pitch = 0;
    double prevTime = 0, time = 0;
    bool primed = false;

    void push(const Vec3& p, float y, float pit, double now);
    void sample(double at, Vec3& pos, float& yaw, float& pitch) const;
  };

  struct PlayerGhost {
    std::string name;
    Track track;
    float health = 20.0f;
    std::uint8_t flags = 0;
    bool hurt = false;
    int netId = 0;
  };

  struct EntityGhost {
    Track track;
    std::uint8_t type = 0;
    float a = 0, b = 0;
    bool seen = false;
  };

  // Ghost ids are the network's, not the manager's. A player's is derived from a
  // running counter rather than from its id string, because the entity layer keys
  // on an int.
  int netIdFor(const std::string& playerId);

  game::EntityManager* entities_ = nullptr;
  std::unordered_map<std::string, PlayerGhost> players_;
  std::unordered_map<std::int32_t, EntityGhost> entityGhosts_;
  std::vector<Nameplate> nameplates_;
  int nextNetId_ = kPlayerNetBase;
};

}  // namespace hr::net
