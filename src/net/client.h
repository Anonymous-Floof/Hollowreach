// The guest half, ported from js/net/client.js.
//
// A guest simulates nothing that the host owns. Its own player moves locally —
// anything else would put a round trip between pressing W and moving — and every
// edit it makes is applied at once and rolled back if the host disagrees. Mobs,
// drops, boats, time and other players all arrive as snapshots and are drawn as
// ghosts (net/ghosts.h).
//
// The three things that follow from that, and that the rest of the game has to
// respect: the guest's world is never saved (it belongs to the host), its water
// simulation does not run (the host's edits are what it will be told about), and
// its entity manager holds nothing but ghosts.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/ghosts.h"
#include "net/protocol.h"
#include "net/session.h"
#include "net/transport.h"

namespace hr::net {

class Client {
 public:
  enum class State : std::uint8_t {
    Idle,
    Connecting,   // socket opened, waiting for ENet
    Handshaking,  // connected, hello sent, waiting for the world
    Playing,
    Failed,
  };

  bool start(const std::string& address, std::uint16_t port, const std::string& playerId,
             const std::string& name, SessionHooks hooks, std::string* error);
  // Called once the world payload has been adopted and the game refs are live.
  void attachGame(const GameRefs& game);
  void stop(bool sayGoodbye = true);

  bool running() const { return transport_.active(); }
  State state() const { return state_; }
  const std::string& failure() const { return failure_; }
  std::uint32_t pingMs() const;

  void update(double dt, double now);

  // --- things the local player does that the host must be told about ----------
  // A typed line, exactly as typed. Whether it is a sentence or a command is the
  // host's decision, not ours — see MsgType::Chat.
  void sendChat(const std::string& line);
  void sendEdit(int x, int y, int z, std::uint16_t id, std::uint8_t meta, int tint);
  void sendHit(int entityId, const std::string& held, bool crit);
  void sendPlayerHit(const std::string& playerId, const std::string& held, bool crit);
  // Puts an item into the world through the host, which owns every entity in it.
  void sendToss(const Vec3& pos, const Vec3& dir, const std::string& key, int count, int dura);
  void sendBoatSpawn(const Vec3& pos);
  void sendBoatMount(int entityId, bool on);
  // `target` is the hour on the 0..1 clock this player would wake at. The host
  // only reads it from whoever asks first; after that it is a yes to their hour.
  void sendSleep(bool on, float target);

  // The proposal the host says is on the table, or -1 when nobody has asked. A bed
  // opened while this is set shows the proposal rather than a fresh chooser.
  float proposedSleep() const { return sleepActive_ ? sleepTarget_ : -1.0f; }
  const std::string& proposer() const { return sleepProposer_; }
  // This player is about to move further in one step than any speed allows, and
  // it is legitimate. The host suspends its movement check for a moment rather
  // than answering the jump with a teleport back.
  //
  // Two things need it. A wayshard, which throws the player straight up: without
  // this the host reads it as a speed hack and puts them back underground, which
  // looks exactly like the item failing. And a respawn, which is the same jump
  // over a longer distance — anyone who died more than about twelve blocks from
  // their spawn point was dragged straight back to the spot they had just died
  // on, standing in their own dropped things, wondering what respawning was for.
  void sendWarp();
  // Ask the host to hang a picture. Nothing is applied locally: the host answers
  // with what it stored, which is what everyone else will see too.
  void sendPainting(int x, int y, int z, const game::Painting& art);
  void sendBlockEntityRequest(int x, int y, int z, std::uint8_t kind);
  void sendBlockEntityState(const BeStateMsg& state);
  // Where this player has bound their spawn, so the host can keep it with the
  // rest of their progress. Told rather than read: binding a Soul Anchor is a
  // game event and App is where it happens.
  void setSpawn(bool bound, const Vec3& at) {
    spawn_.bound = bound;
    spawn_.at = at;
  }

  std::vector<RosterEntry> roster() const;
  const Ghosts& ghosts() const { return ghosts_; }
  Ghosts& ghosts() { return ghosts_; }

 private:
  void onMessage(const std::uint8_t* data, std::size_t size, double now);
  void sendHello();
  void sendPose();
  void sendPlayerState();
  template <typename T>
  void send(MsgType type, const T& msg, Channel channel = Channel::Reliable);
  void sendEmpty(MsgType type, Channel channel = Channel::Reliable);

  Transport transport_;
  GameRefs game_;
  SessionHooks hooks_;
  Ghosts ghosts_;

  State state_ = State::Idle;
  std::string failure_;
  std::string playerId_;
  std::string name_;
  PeerId host_ = kNoPeer;

  std::vector<TransportEvent> events_;
  struct Member {
    std::string playerId;
    std::string name;
  };
  std::vector<Member> roster_;
  std::string hostPlayerId_;

  // Mirrors the host's sleep tally. Purely for the interface — the decision is
  // never taken here.
  bool sleepActive_ = false;
  float sleepTarget_ = 0.27f;
  std::string sleepProposer_;

  double poseTimer_ = 0;
  double stateTimer_ = 0;
  double connectTimer_ = 0;
  // Our own pose counter, and the newest snapshot the host has managed to get
  // here. Both exist because the fast channel does not promise order; see
  // SnapshotMsg::seq.
  std::uint32_t poseSeq_ = 0;
  std::uint32_t lastSnapSeq_ = 0;
  bool haveSnapSeq_ = false;

  struct BoundSpawn {
    bool bound = false;
    Vec3 at;
  };
  BoundSpawn spawn_;
};

}  // namespace hr::net
