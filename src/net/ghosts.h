// Remote bodies: other players, and (on a guest) every entity the host owns.
//
// Ported from js/net/ghosts.js. A ghost is an entity the local simulation never
// touches — `Entity::ghost` is checked by the manager's tick and by every AI hook
// — so its position comes entirely from what arrives over the wire.
//
// The one idea worth stating: **render slightly in the past**. Drawing the newest
// sample directly means a remote player teleports forward every time one lands,
// so a ghost holds two samples and interpolates between them at a time a little
// behind now. That costs latency and buys motion that is smooth at any packet
// rate, including one where packets arrive late or out of order.
//
// How far behind is MEASURED, not a constant. It used to be a flat 150 ms,
// inherited from the web build along with its 10 Hz snapshots; on a LAN where the
// round trip is about a millisecond that was the single largest source of
// apparent lag in the game, and no part of it looked at the connection.
//
// What the buffer has to cover is the gap until the NEXT sample arrives — if the
// render time runs past the newest sample there is nothing to interpolate toward
// and the ghost freezes. So the estimate is the smoothed inter-arrival gap plus a
// margin for how much that gap varies. Note what is deliberately NOT in it:
// latency. Samples are stamped with their local ARRIVAL time, so a slow link
// shifts the whole timeline uniformly and is already paid for; adding the round
// trip on top would charge for it twice.
//
// Estimated per track rather than globally, because the streams genuinely differ:
// a guest interpolates the host's snapshot rate, while a host interpolates each
// guest's pose rate separately, and with two guests those interleave.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/mat4.h"
#include "game/entities/manager.h"
#include "net/protocol.h"

namespace hr::net {

// Bounds on how far behind the newest sample a ghost is drawn. The floor is what
// a 30 Hz stream with no jitter would need and still leaves room for a dropped
// packet; the ceiling stops a link that has briefly fallen apart from putting
// everybody a second in the past and leaving them there.
inline constexpr double kMinInterpDelay = 0.045;
inline constexpr double kMaxInterpDelay = 0.300;
// What an unprimed track assumes before it has seen two samples.
inline constexpr double kStartInterpDelay = 0.100;

// Player ghosts are numbered from here up; entity ghosts keep the host's own
// entity id, which counts from 1. Without the offset the two share one namespace,
// and the host's first mob has the same ghost id as the first remote player — so
// the mob's update finds the player's body, writes into that, and the mob is never
// created at all. It looked exactly like "mobs do not replicate" and was nothing of
// the sort.
inline constexpr int kPlayerNetBase = 1'000'000;

class Ghosts {
 public:
  // Two samples, the times they arrived, and what this stream's pacing looks
  // like. Public because the delay estimate is the interesting behaviour in this
  // file and the only way to test it without two machines and a stopwatch.
  struct Track {
    Vec3 prevPos, pos;
    float prevYaw = 0, yaw = 0;
    float prevPitch = 0, pitch = 0;
    double prevTime = 0, time = 0;
    bool primed = false;
    // Smoothed inter-arrival gap and how much it varies, both in seconds, updated
    // on every push. Exponential averages rather than a window: one number each,
    // no history to keep, and they settle within a second of play.
    double gap = kStartInterpDelay;
    double jitter = 0.0;

    void push(const Vec3& p, float y, float pit, double now);
    void sample(double at, Vec3& pos, float& yaw, float& pitch) const;
    // How far behind now this track should be drawn.
    double delay() const;
  };

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
  //
  // `localMount` is the entity id of whatever the local player is sitting in, or 0.
  // That one body is simulated here rather than mirrored — see EntityManager::tick
  // — so writing an interpolated position into it would drag the rider back to
  // where the host last saw them, twenty times a second, which is a boat that
  // shudders on the spot and never leaves the shore.
  void update(double now, int localMount = 0);

  // For nameplates: every remote player currently known, with where it is drawn.
  struct Nameplate {
    std::string playerId;
    std::string name;
    Vec3 pos;
    float health = 20.0f;
    // Which way they are facing, in the player convention (0 looks down -z), for
    // anything that wants to draw a heading rather than a dot. The Atlas does.
    float yaw = 0.0f;
  };
  const std::vector<Nameplate>& nameplates() const { return nameplates_; }

  std::size_t playerCount() const { return players_.size(); }
  bool hasPlayer(const std::string& playerId) const {
    return players_.find(playerId) != players_.end();
  }
  // Where a remote player was last seen, for the host's reach checks.
  bool playerPos(const std::string& playerId, Vec3& out) const;

 private:
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
    std::string key;
    bool seen = false;
  };

  // netId -> index into the entity manager's vector, rebuilt once per update.
  //
  // This was a linear scan per ghost, inside a loop over every ghost, so the cost
  // of drawing other people grew as the square of how much was going on. At the
  // protocol's cap of 512 entities that is a quarter of a million comparisons
  // every frame, on the main thread, and it arrived exactly when a session had
  // been running long enough to have that much in it.
  std::unordered_map<int, std::size_t> index_;
  void rebuildIndex();
  game::Entity* bodyFor(int netId);

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
