// ENet transport: the replacement for js/net/transport.js and js/net/signal.js.
//
// The web build ran WebRTC data channels over a copy-and-paste SDP exchange, and
// two of its channels — "rel" (reliable, ordered) and "fast" (unreliable) — map
// exactly onto ENet's channel 0 and channel 1, which is why the rewrite kept the
// same split rather than inventing one. What is lost with WebRTC is NAT traversal;
// what is gained is that a LAN game needs no signalling server, no ICE, and no
// copy-and-paste at all.
//
// This layer knows nothing about the game. It owns a socket, turns ENet's event
// loop into a vector of events, and addresses peers by a small integer that stays
// valid for the life of a connection — a raw ENetPeer* would be reused for the
// next peer to connect into the same slot, and a message routed to a recycled
// pointer is exactly the kind of bug that only shows up with two people playing.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hr::net {

// Channel 0 is reliable and ordered: edits, handshakes, inventory, anything whose
// loss would desynchronise the world. Channel 1 is unreliable: poses and
// snapshots, where the next one supersedes the last and waiting for a resend is
// worse than dropping it.
enum class Channel : std::uint8_t {
  Reliable = 0,
  Fast = 1,
  Count = 2,
};

using PeerId = std::uint32_t;
inline constexpr PeerId kNoPeer = 0;

struct TransportEvent {
  enum class Kind : std::uint8_t { Connected, Disconnected, Message };
  Kind kind = Kind::Message;
  PeerId peer = kNoPeer;
  std::vector<std::uint8_t> data;
};

class Transport {
 public:
  Transport() = default;
  ~Transport();
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  // Host: bind a port and accept up to `maxPeers` guests.
  bool listen(std::uint16_t port, int maxPeers, std::string* error);
  // Guest: connect out. Returns once the attempt is under way — a Connected event
  // arrives from poll() when it succeeds, and nothing arrives if it does not, so
  // the caller times it out.
  bool connect(const std::string& address, std::uint16_t port, std::string* error);

  void close();
  bool active() const { return host_ != nullptr; }
  bool isServer() const { return server_; }
  // The port actually bound, which matters when the caller asked for 0.
  std::uint16_t port() const { return port_; }

  // Pumps ENet. `timeoutMs` of 0 returns immediately, which is what a game loop
  // wants; a non-zero value blocks and is only useful in a test.
  void poll(std::vector<TransportEvent>& out, std::uint32_t timeoutMs = 0);

  void send(PeerId peer, Channel channel, const std::vector<std::uint8_t>& bytes);
  void broadcast(Channel channel, const std::vector<std::uint8_t>& bytes);
  // Excludes one peer, for relaying a guest's action to everyone else.
  void broadcastExcept(PeerId except, Channel channel, const std::vector<std::uint8_t>& bytes);
  // A polite disconnect: the peer is told, and a Disconnected event follows.
  void disconnect(PeerId peer);

  std::size_t peerCount() const { return peers_.size(); }
  std::vector<PeerId> peers() const;
  // Round-trip time in milliseconds as ENet measures it, or 0 for an unknown peer.
  std::uint32_t roundTripMs(PeerId peer) const;
  const std::string& addressOf(PeerId peer) const;

 private:
  struct PeerSlot {
    PeerId id = kNoPeer;
    void* peer = nullptr;  // ENetPeer*
    std::string address;
  };

  PeerSlot* slotFor(PeerId id);
  const PeerSlot* slotFor(PeerId id) const;
  PeerSlot* slotForPeer(void* peer);

  void* host_ = nullptr;  // ENetHost*
  bool server_ = false;
  std::uint16_t port_ = 0;
  std::vector<PeerSlot> peers_;
  PeerId nextPeerId_ = 1;
  bool loggedServiceError_ = false;
};

// ENet's global init/deinit, reference counted so the transport and the LAN
// discovery socket can both hold it without either owning it.
bool enetAcquire();
void enetRelease();

}  // namespace hr::net
