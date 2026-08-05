// The authoritative host, ported from js/net/host.js.
//
// TRUST MODEL, unchanged: guests are untrusted. Every message has already passed
// the protocol's shape and range validator; this layer adds the semantics — a
// version gate at the handshake, movement speed checks, reach checks on edits and
// combat, per-action token buckets, and block and item whitelists. A guest that
// fails a check gets a correction — a rollback or a teleport — not a disconnect
// and never a crash.
//
// What the host shares: the world being played, and player poses. Nothing else.
// The world payload is `save::WorldSave`, which is exactly what would be written
// to disk, so there is no second serialisation to keep honest.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net/ghosts.h"
#include "net/protocol.h"
#include "net/session.h"
#include "net/transport.h"
#include "save/format.h"

namespace hr::net {

inline constexpr int kMaxGuests = 7;

class Host {
 public:
  bool start(std::uint16_t port, const std::string& playerId, const std::string& name,
             const GameRefs& game, SessionHooks hooks, std::string* error);
  void stop();
  bool running() const { return transport_.active(); }
  // Tells every guest what now hangs at a position. Called when the host hangs one
  // itself; a guest's request is answered from the message handler.
  void broadcastPainting(int x, int y, int z, const game::Painting& art);

  void update(double dt, double now);

  // --- things the local player does that guests must be told about ------------
  void onLocalEdit(int x, int y, int z, std::uint16_t id, std::uint8_t meta);
  void onLocalSfx(const std::string& kind, const Vec3& pos);
  // The host's own vote. Everyone in the world has to want it before the night
  // is skipped, which is the whole point of a vote rather than a button.
  // The host's own vote. `target` is only read from the first voter — whoever
  // opens the question owns the hour, and everyone after them is answering it.
  // The world's own rules, pushed to every guest. Called by App when the host
  // changes one; a guest that joins later gets them inside the world payload
  // instead, so this is only ever the mid-session update.
  void broadcastWorldSettings();

  void onLocalSleep(bool on, float target);

  // The proposal currently on the table, or -1 when nobody has asked. App reads
  // these to decide whether a bed opens a chooser or a yes/no.
  float proposedSleep() const { return sleepVotes_.empty() ? -1.0f : sleepTarget_; }
  const std::string& proposer() const { return sleepProposer_; }

  // Guest progress to write into the save, and progress read back out of one.
  std::vector<save::GuestSave> guestsForSave() const;
  void restoreGuests(const std::vector<save::GuestSave>& guests);

  std::vector<RosterEntry> roster() const;
  const Ghosts& ghosts() const { return ghosts_; }
  Ghosts& ghosts() { return ghosts_; }
  std::uint16_t port() const { return transport_.port(); }
  int guestCount() const;

  // Guest progress, keyed by player id, for the host's save. The web build called
  // this remotePlayersStore and wrote it into the save file; the native save has
  // no section for it yet, so it lives for the session and is documented as the
  // one field M11 still owes M9.
  struct StoredGuest {
    std::string name;
    PlayerStateMsg state;
    bool valid = false;
  };
  const std::unordered_map<std::string, StoredGuest>& storedGuests() const { return stored_; }

 private:
  struct PeerState {
    PeerId peer = kNoPeer;
    std::string playerId;
    std::string name;
    bool active = false;   // has applied the world snapshot
    bool greeted = false;  // has sent a valid hello
    Vec3 lastPose;
    // Kept beside the position because a snapshot has to carry them on to the
    // other guests: this is the only record the host has of which way a guest is
    // facing, and without it everyone but the host saw them staring north.
    float lastYaw = 0;
    float lastPitch = 0;
    std::uint8_t flags = 0;
    bool havePose = false;
    double lastPoseTime = 0;
    // The highest pose sequence seen. Poses ride the unsequenced channel, so an
    // old one can overtake a new one; taking it would move this guest backwards
    // for everybody else and — much worse — feed a stale position to the movement
    // check, which then answers the guest's real position with a teleport.
    std::uint32_t lastPoseSeq = 0;
    bool havePoseSeq = false;
    float health = 20.0f;
    // Until when the movement check is suspended for this guest, on the host's
    // clock. A wayshard and a respawn both move a body further in one step than
    // any speed allows, and both say so first — but "first" is not a thing the
    // network guarantees: the warning goes reliably on channel 0 and the pose that
    // needs it goes unsequenced on channel 1, and the two channels do not wait for
    // each other. A window rather than a single exemption, so the pose that
    // arrives just before its own warning is still covered.
    double moveGraceUntil = -1.0;
    // Game hours since this guest last slept, counted by the host off its own
    // clock. Held here rather than trusted from the guest for the same reason
    // every other limit is checked on this side, and runtime-only: a guest who
    // rejoins arrives freshly rested and has to earn the next night again.
    float hoursAwake = 0.0f;
    // Per-action rate limits, from js/net/host.js:102.
    Bucket edit{40, 120};
    Bucket hit{5, 10};
    Bucket toss{8, 20};
    Bucket be{6, 12};
    Bucket pose{40, 80};
    Bucket misc{10, 20};
  };

  PeerState* peerFor(PeerId id);
  PeerState* peerForPlayer(const std::string& playerId);
  void drop(PeerState& st, const std::string& reason);
  // Removes a peer and tells everyone else. Called both when ENet reports the
  // connection gone and when the guest says goodbye — a goodbye is the guest
  // telling us it has already stopped listening, so waiting for ENet's own
  // handshake to time out would leave a ghost in the roster for seconds.
  // Invalidates `st`.
  void forget(PeerState& st);

  void onMessage(PeerState& st, const std::uint8_t* data, std::size_t size, double now);
  void onHello(PeerState& st, const HelloMsg& m);
  void onReady(PeerState& st);
  void onPose(PeerState& st, const PoseMsg& m, double now);
  void onEdit(PeerState& st, const EditMsg& m, double now);
  void onHit(PeerState& st, const HitMsg& m, double now);
  void onPlayerHit(PeerState& st, const PlayerHitMsg& m, double now);
  void onBoatMount(PeerState& st, const BoatMountMsg& m, double now);
  void onBoatSpawn(PeerState& st, const BoatSpawnMsg& m, double now);
  void onToss(PeerState& st, const TossMsg& m, double now);
  void onSleep(PeerState& st, const SleepMsg& m);
  void onBeRequest(PeerState& st, const BeRequestMsg& m, double now);
  void onBeState(PeerState& st, const BeStateMsg& m, double now);
  void onPlayerState(PeerState& st, const PlayerStateMsg& m);

  void denyEdit(PeerState& st, const EditMsg& m);
  void sendWorld(PeerState& st);
  void sendSnapshot();
  void flushEdits();
  void spillBlockEntity(int x, int y, int z);
  // Walks the drops and hands each one to whichever guest is stood on it. The
  // host's own player collects through the entity tick like it always has; a
  // guest's body is not simulated here, so there is nothing on that side for the
  // drop to notice, and until this existed a guest could not pick up so much as
  // its own death scatter.
  void collectDrops();

  template <typename T>
  void sendTo(PeerId peer, MsgType type, const T& msg, Channel channel = Channel::Reliable);
  void sendEmpty(PeerId peer, MsgType type, Channel channel = Channel::Reliable);
  template <typename T>
  void broadcast(MsgType type, const T& msg, PeerId except = kNoPeer,
                 Channel channel = Channel::Reliable);

  Transport transport_;
  GameRefs game_;
  SessionHooks hooks_;
  Ghosts ghosts_;

  std::string playerId_;
  std::string name_;
  std::vector<PeerState> peers_;
  std::vector<TransportEvent> events_;

  // Edits are batched: a player mining a tree produces a burst, and one packet
  // per cell is a packet per millisecond.
  std::vector<EditMsg> pendingEdits_;
  double snapTimer_ = 0;
  double timeTimer_ = 0;
  // Stamped on each snapshot so a guest can tell a new one from one that took the
  // scenic route. Starts at 1, because 0 is what an unset field decodes to.
  std::uint32_t snapSeq_ = 0;

  // Containers a guest has open, so two people cannot stir the same forge.
  std::unordered_map<std::uint64_t, std::string> beLocks_;
  std::unordered_map<std::string, StoredGuest> stored_;
  // Who currently wants to sleep, by player id, including the host — and the hour
  // the first of them asked for, which is what the rest are voting on. Cleared
  // together, so "is there a proposal" is just "is anyone voting".
  std::unordered_set<std::string> sleepVotes_;
  float sleepTarget_ = 0.27f;
  std::string sleepProposer_;
  void tallySleep();
  void broadcastSleepState();
};

}  // namespace hr::net
