// Finding a game to join, without a signalling server.
//
// The web build had no equivalent and needed none: WebRTC's copy-and-paste SDP
// exchange *was* the discovery mechanism, and it worked across the internet for
// free. ENet gives that up (see the accepted regressions), so this milestone owes
// two things back:
//
//   * **LAN discovery.** The host broadcasts a small beacon on a fixed UDP port
//     once a second; a guest opening the Join screen listens for them and lists
//     what it hears. Broadcast rather than probe-and-reply because it needs no
//     round trip, works when only one side can broadcast, and a datagram of forty
//     bytes a second is not worth optimising.
//
//   * **The invite code.** The plan is explicit that the code-shaped join string
//     survives the transport change, in the same `HRW1…HRW1` envelope, because the
//     invite-code UX and the whole Join panel are built around it. It now encodes
//     `host:port` plus the world's name instead of an SDP blob, which makes it
//     three lines long instead of thirty — and it is what a player sends to a
//     friend who is not on the same network and has forwarded a port.
//
// The socket work goes through ENet's own portable socket API rather than
// Winsock/BSD directly, so this file has no platform branches.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hr::net {

// Fixed, so a guest knows where to listen without being told.
inline constexpr std::uint16_t kDiscoveryPort = 25566;
inline constexpr std::uint16_t kDefaultGamePort = 25565;

struct Beacon {
  std::string address;   // where the beacon came from
  std::uint16_t port = kDefaultGamePort;
  std::string worldName;
  std::string hostName;
  std::uint8_t players = 0;
  std::uint8_t maxPlayers = 0;
  double ageSeconds = 0;  // since it was last heard
};

// "no socket". Spelled out rather than written inline as `~0u`, which was this
// file's first bug and a genuinely nasty one: `~0u` is a 32-bit 0xFFFFFFFF, and
// widening it into a 64-bit field gives 0x00000000FFFFFFFF — which is not equal to
// the 0xFFFFFFFFFFFFFFFF the comparison used. A never-opened advertiser therefore
// looked open, `stop()` destroyed a socket handle that was never real and released
// ENet's reference count to zero, and WSACleanup() invalidated every socket in the
// process — including the game host's, which then failed with WSAENOTSOCK on its
// very first poll and simply never heard anyone knock.
inline constexpr std::uint64_t kNoSocket = ~static_cast<std::uint64_t>(0);

// The host side: opens a broadcast socket and beacons on a timer.
class Advertiser {
 public:
  ~Advertiser();
  bool start(std::uint16_t gamePort, std::string worldName, std::string hostName);
  void stop();
  bool active() const { return socket_ != kNoSocket; }
  void setPlayers(int players, int maxPlayers);
  // Call every frame; sends at most one beacon a second.
  void update(double dt);

 private:
  void send();

  std::uint64_t socket_ = kNoSocket;  // an ENetSocket, or kNoSocket
  std::uint16_t gamePort_ = kDefaultGamePort;
  std::string worldName_;
  std::string hostName_;
  std::uint8_t players_ = 0;
  std::uint8_t maxPlayers_ = 0;
  double timer_ = 0.0;
};

// The guest side: listens and keeps a list of what it has heard recently.
class Listener {
 public:
  ~Listener();
  bool start();
  void stop();
  bool active() const { return socket_ != kNoSocket; }
  // Drains the socket and ages out anything not heard from in `forgetAfter`.
  void update(double dt, double forgetAfter = 6.0);
  const std::vector<Beacon>& found() const { return beacons_; }

 private:
  std::uint64_t socket_ = kNoSocket;
  std::vector<Beacon> beacons_;
};

// ---- invite codes -----------------------------------------------------------
// HRW1<base32>HRW1, where the payload is address, port and world name. Crockford's
// alphabet: no I, L, O or U, so a code read aloud or typed from a screenshot does
// not turn into a different code — or a word.

std::string makeInviteCode(const std::string& address, std::uint16_t port,
                           const std::string& worldName);
// Returns false for anything that is not one of ours. Tolerant of whitespace,
// case and a missing envelope, because this is pasted by hand.
bool parseInviteCode(const std::string& code, std::string& address, std::uint16_t& port,
                     std::string& worldName);

// The machine's own LAN address, for putting in an invite code. Empty if it cannot
// be determined, which is not an error — a code with an empty address still
// carries the port, and the Join panel lets an address be typed.
std::string localAddress();

}  // namespace hr::net
