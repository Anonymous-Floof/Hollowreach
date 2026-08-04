#include "net/discovery.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <enet/enet.h>

#include "core/bytes.h"
#include "core/log.h"
#include "net/protocol.h"
#include "net/transport.h"

namespace hr::net {
namespace {

constexpr char kBeaconMagic[4] = {'H', 'R', 'B', '1'};
// A guest asking "is anyone hosting". Same port, different magic, so an old build
// simply ignores it rather than mistaking it for a malformed beacon.
constexpr char kQueryMagic[4] = {'H', 'R', 'Q', '1'};
constexpr double kBeaconPeriod = 1.0;
constexpr double kQueryPeriod = 2.0;
// Adapters change while the game runs: a VPN connects, a cable goes in.
constexpr double kInterfacePeriod = 5.0;
constexpr std::size_t kMaxDatagram = 512;

ENetSocket handle(std::uint64_t s) { return static_cast<ENetSocket>(s); }

// Sends one datagram to every subnet the machine is on, and to the limited
// broadcast as well.
//
// Both, deliberately. The directed broadcasts are the ones that reliably leave by
// the right interface, and 255.255.255.255 is kept because a few setups drop
// directed broadcast at the switch while passing the limited one. Sending two
// datagrams a second to cover both is not a cost worth optimising.
void blast(ENetSocket socket, const std::vector<Interface>& interfaces,
           const std::vector<std::uint8_t>& data, std::uint16_t port) {
  ENetBuffer buffer;
  buffer.data = const_cast<std::uint8_t*>(data.data());
  buffer.dataLength = data.size();

  for (const Interface& iface : interfaces) {
    ENetAddress target;
    target.host = iface.broadcast;
    target.port = port;
    enet_socket_send(socket, &target, &buffer, 1);
  }
  ENetAddress limited;
  limited.host = ENET_HOST_BROADCAST;
  limited.port = port;
  enet_socket_send(socket, &limited, &buffer, 1);
}

void logInterfaces(const char* who, const std::vector<Interface>& interfaces) {
  if (interfaces.empty()) {
    log::warn("net: %s found no broadcastable network; falling back to 255.255.255.255", who);
    return;
  }
  // Logged because when discovery does not work this is the first thing anybody
  // needs to see, and it is otherwise invisible. A machine beaconing only into a
  // Hyper-V switch looks identical to one with a firewall problem.
  for (const Interface& iface : interfaces) {
    log::info("net: %s on %s (%s -> %s)", who, iface.name.c_str(), iface.addressText.c_str(),
              iface.broadcastText.c_str());
  }
}

// Crockford base32: no I, L, O or U, so a code read off a screen cannot turn into
// a different code — or into a word.
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int valueOf(char c) {
  const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  for (int i = 0; i < 32; ++i) {
    if (kAlphabet[i] == upper) return i;
  }
  // The letters the alphabet leaves out are the ones people substitute, so they
  // are accepted on the way in and mapped to what they were meant to be.
  switch (upper) {
    case 'I':
    case 'L': return 1;
    case 'O': return 0;
    case 'U': return 27;  // V
    default: return -1;
  }
}

std::string base32Encode(const std::vector<std::uint8_t>& data) {
  std::string out;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const std::uint8_t byte : data) {
    buffer = (buffer << 8) | byte;
    bits += 8;
    while (bits >= 5) {
      out.push_back(kAlphabet[(buffer >> (bits - 5)) & 31]);
      bits -= 5;
    }
  }
  if (bits > 0) out.push_back(kAlphabet[(buffer << (5 - bits)) & 31]);
  return out;
}

bool base32Decode(const std::string& text, std::vector<std::uint8_t>& out) {
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char c : text) {
    if (c == ' ' || c == '-' || c == '\n' || c == '\r' || c == '\t') continue;
    const int v = valueOf(c);
    if (v < 0) return false;
    buffer = (buffer << 5) | static_cast<std::uint32_t>(v);
    bits += 5;
    if (bits >= 8) {
      out.push_back(static_cast<std::uint8_t>((buffer >> (bits - 8)) & 0xFF));
      bits -= 8;
    }
  }
  return true;
}

}  // namespace

// ---- advertiser -------------------------------------------------------------

Advertiser::~Advertiser() { stop(); }

bool Advertiser::start(std::uint16_t gamePort, std::string worldName, std::string hostName) {
  stop();
  if (!enetAcquire()) return false;
  const ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) {
    enetRelease();
    log::warn("net: could not open the discovery socket; LAN advertising is off");
    return false;
  }
  enet_socket_set_option(s, ENET_SOCKOPT_BROADCAST, 1);
  enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
  enet_socket_set_option(s, ENET_SOCKOPT_REUSEADDR, 1);

  // Bound so that guests' queries can be heard. Failing to bind is not fatal —
  // something else may hold the port — and the beacon still goes out, so the host
  // is merely findable one way instead of two.
  ENetAddress bind;
  bind.host = ENET_HOST_ANY;
  bind.port = kDiscoveryPort;
  bound_ = enet_socket_bind(s, &bind) == 0;
  if (!bound_) {
    log::warn("net: the discovery port is taken; this world beacons but cannot answer queries");
  }

  socket_ = static_cast<std::uint64_t>(s);
  gamePort_ = gamePort;
  worldName_ = std::move(worldName);
  hostName_ = std::move(hostName);
  timer_ = kBeaconPeriod;  // beacon immediately, so Join finds it at once
  interfaceTimer_ = 0.0;
  refreshInterfaces(true);
  return true;
}

void Advertiser::refreshInterfaces(bool logThem) {
  interfaces_ = localInterfaces();
  if (logThem) logInterfaces("advertising", interfaces_);
}

void Advertiser::stop() {
  if (socket_ == kNoSocket) return;
  enet_socket_destroy(handle(socket_));
  socket_ = kNoSocket;
  enetRelease();
}

void Advertiser::setPlayers(int players, int maxPlayers) {
  players_ = static_cast<std::uint8_t>(std::clamp(players, 0, 255));
  maxPlayers_ = static_cast<std::uint8_t>(std::clamp(maxPlayers, 0, 255));
}

void Advertiser::update(double dt) {
  if (socket_ == kNoSocket) return;

  // Answered first and every frame, so a guest that has just opened Join sees the
  // world appear as fast as the wire allows rather than on the next beacon.
  answerQueries();

  interfaceTimer_ += dt;
  if (interfaceTimer_ >= kInterfacePeriod) {
    interfaceTimer_ = 0.0;
    refreshInterfaces(false);
  }

  timer_ += dt;
  if (timer_ < kBeaconPeriod) return;
  timer_ = 0.0;
  beacon();
}

std::vector<std::uint8_t> Advertiser::payload() const {
  ByteWriter w;
  w.bytes(kBeaconMagic, sizeof kBeaconMagic);
  w.u16(kNetVersion);
  w.u16(gamePort_);
  w.u8(players_);
  w.u8(maxPlayers_);
  w.str(worldName_);
  w.str(hostName_);
  if (w.size() > kMaxDatagram) return {};
  return w.data();
}

void Advertiser::beacon() {
  const std::vector<std::uint8_t> data = payload();
  if (data.empty()) return;
  blast(handle(socket_), interfaces_, data, kDiscoveryPort);
}

void Advertiser::answerQueries() {
  if (!bound_) return;
  std::uint8_t scratch[kMaxDatagram];
  for (int guard = 0; guard < 16; ++guard) {
    ENetAddress from;
    ENetBuffer buffer;
    buffer.data = scratch;
    buffer.dataLength = sizeof scratch;
    const int got = enet_socket_receive(handle(socket_), &from, &buffer, 1);
    if (got <= 0) break;
    if (got < static_cast<int>(sizeof kQueryMagic)) continue;
    // Our own beacons come back on this socket too; only a query is answered, or
    // two hosts on one network would answer each other forever.
    if (std::memcmp(scratch, kQueryMagic, sizeof kQueryMagic) != 0) continue;

    const std::vector<std::uint8_t> data = payload();
    if (data.empty()) continue;
    // Straight back to whoever asked. A unicast reply reaches places a broadcast
    // from here would not, which is the entire reason the query exists.
    ENetBuffer out;
    out.data = const_cast<std::uint8_t*>(data.data());
    out.dataLength = data.size();
    enet_socket_send(handle(socket_), &from, &out, 1);
  }
}

// ---- listener ---------------------------------------------------------------

Listener::~Listener() { stop(); }

bool Listener::start() {
  stop();
  problem_.clear();
  if (!enetAcquire()) {
    problem_ = "Networking is unavailable on this machine.";
    return false;
  }
  const ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) {
    enetRelease();
    problem_ = "Could not open a network socket.";
    return false;
  }
  // Both flags matter for the case that gets tested most: a host and a guest on
  // the same machine, which means two sockets on one port.
  enet_socket_set_option(s, ENET_SOCKOPT_REUSEADDR, 1);
  enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
  enet_socket_set_option(s, ENET_SOCKOPT_BROADCAST, 1);

  ENetAddress bind;
  bind.host = ENET_HOST_ANY;
  bind.port = kDiscoveryPort;
  if (enet_socket_bind(s, &bind) != 0) {
    enet_socket_destroy(s);
    enetRelease();
    log::warn("net: could not bind the discovery port; LAN games will not be listed");
    problem_ = "Could not listen for games on this network. Another program may be "
               "using port " + std::to_string(kDiscoveryPort) + ".";
    return false;
  }
  socket_ = static_cast<std::uint64_t>(s);
  beacons_.clear();
  queryTimer_ = kQueryPeriod;  // ask at once rather than after a wait
  interfaceTimer_ = 0.0;
  refreshInterfaces(true);
  return true;
}

void Listener::refreshInterfaces(bool logThem) {
  interfaces_ = localInterfaces();
  if (logThem) logInterfaces("looking for games", interfaces_);
}

void Listener::query() {
  ByteWriter w;
  w.bytes(kQueryMagic, sizeof kQueryMagic);
  w.u16(kNetVersion);
  blast(handle(socket_), interfaces_, w.data(), kDiscoveryPort);
}

void Listener::stop() {
  if (socket_ == kNoSocket) return;
  enet_socket_destroy(handle(socket_));
  socket_ = kNoSocket;
  beacons_.clear();
  enetRelease();
}

void Listener::update(double dt, double forgetAfter) {
  for (Beacon& b : beacons_) b.ageSeconds += dt;

  if (socket_ != kNoSocket) {
    interfaceTimer_ += dt;
    if (interfaceTimer_ >= kInterfacePeriod) {
      interfaceTimer_ = 0.0;
      refreshInterfaces(false);
    }
    queryTimer_ += dt;
    if (queryTimer_ >= kQueryPeriod) {
      queryTimer_ = 0.0;
      query();
    }

    std::uint8_t scratch[kMaxDatagram];
    for (int guard = 0; guard < 32; ++guard) {
      ENetAddress from;
      ENetBuffer buffer;
      buffer.data = scratch;
      buffer.dataLength = sizeof scratch;
      const int got = enet_socket_receive(handle(socket_), &from, &buffer, 1);
      if (got <= 0) break;

      ByteReader r(scratch, static_cast<std::size_t>(got));
      char magic[4] = {};
      if (!r.bytes(magic, sizeof magic) || std::memcmp(magic, kBeaconMagic, 4) != 0) continue;
      if (r.u16() != kNetVersion) continue;  // a build we cannot talk to

      Beacon b;
      b.port = r.u16();
      b.players = r.u8();
      b.maxPlayers = r.u8();
      b.worldName = r.str();
      b.hostName = r.str();
      if (!r.ok() || b.worldName.size() > kMaxWorldName || b.hostName.size() > kMaxName) {
        continue;
      }
      b.worldName = cleanName(b.worldName);
      b.hostName = cleanName(b.hostName);

      char text[64] = {};
      if (enet_address_get_host_ip(&from, text, sizeof text) != 0) continue;
      b.address = text;
      b.ageSeconds = 0;

      auto it = std::find_if(beacons_.begin(), beacons_.end(), [&b](const Beacon& x) {
        return x.address == b.address && x.port == b.port;
      });
      if (it != beacons_.end()) {
        *it = std::move(b);
      } else {
        beacons_.push_back(std::move(b));
      }
    }
  }

  beacons_.erase(std::remove_if(beacons_.begin(), beacons_.end(),
                                [forgetAfter](const Beacon& b) {
                                  return b.ageSeconds > forgetAfter;
                                }),
                 beacons_.end());
}

// ---- invite codes -----------------------------------------------------------

std::string makeInviteCode(const std::string& address, std::uint16_t port,
                           const std::string& worldName) {
  ByteWriter w;
  w.u8(1);  // envelope version, independent of the wire protocol's
  w.u16(port);
  w.str(address.size() > 64 ? address.substr(0, 64) : address);
  w.str(worldName.size() > kMaxWorldName ? worldName.substr(0, kMaxWorldName) : worldName);
  return "HRW1" + base32Encode(w.data()) + "HRW1";
}

bool parseInviteCode(const std::string& code, std::string& address, std::uint16_t& port,
                     std::string& worldName) {
  // Strip whitespace and the envelope. Pasted by hand, so a missing or lowercase
  // envelope is tolerated rather than refused.
  std::string body;
  for (const char c : code) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      body.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }
  const std::string envelope = "HRW1";
  if (body.rfind(envelope, 0) == 0) body = body.substr(envelope.size());
  if (body.size() >= envelope.size() &&
      body.compare(body.size() - envelope.size(), envelope.size(), envelope) == 0) {
    body = body.substr(0, body.size() - envelope.size());
  }
  if (body.empty()) return false;

  std::vector<std::uint8_t> payload;
  if (!base32Decode(body, payload)) return false;

  ByteReader r(payload.data(), payload.size());
  if (r.u8() != 1) return false;
  port = r.u16();
  address = r.str();
  worldName = r.str();
  if (!r.ok() || port == 0 || address.size() > 64 || worldName.size() > kMaxWorldName) {
    return false;
  }
  // The address has to be usable as a hostname, and this is the one field an
  // attacker controls in a string a player pastes from a chat window.
  for (const char c : address) {
    const bool okChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':';
    if (!okChar) return false;
  }
  return true;
}

std::string localAddress() {
  if (!enetAcquire()) return {};
  // There is no portable "what is my LAN address" call, and the usual trick is
  // this one: a datagram socket connected to an off-machine address picks a route
  // and therefore an interface, without sending anything.
  const ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  std::string out;
  if (s != ENET_SOCKET_NULL) {
    ENetAddress probe;
    probe.port = 53;
    if (enet_address_set_host_ip(&probe, "1.1.1.1") == 0 &&
        enet_socket_connect(s, &probe) == 0) {
      ENetAddress mine;
      if (enet_socket_get_address(s, &mine) == 0) {
        char text[64] = {};
        if (enet_address_get_host_ip(&mine, text, sizeof text) == 0) out = text;
      }
    }
    enet_socket_destroy(s);
  }
  enetRelease();
  if (out == "0.0.0.0") out.clear();
  return out;
}

}  // namespace hr::net
