// Asking the router to let the outside world in, for hosting to somebody who is not
// on your network.
//
// WHAT THIS ACTUALLY DOES, because it is worth being blunt about in the header and
// not only in the manual: it asks the router to forward one UDP port from the
// public internet to this machine. While it is open, anything on the internet can
// send datagrams to the game's socket — not just the friends the address was given
// to. That is not a flaw in the implementation, it is what a port forward IS. The
// mitigations are that it is off by default, that it is one UDP port and not a
// range, that it carries a finite lease so a crash cannot leave it open forever,
// and that it is deleted on the way out.
//
// Two protocols, tried in order, because between them they cover most consumer
// routers and neither needs a library:
//
//   NAT-PMP  (RFC 6886) — 12 bytes to the default gateway on UDP 5351. Simple and
//            fast when it is there. Needs the gateway's address, which is why it
//            is second on platforms where that is awkward to find.
//   UPnP IGD — an SSDP multicast search, an HTTP GET of the device description,
//            then a SOAP POST. Far more code, and far more routers.
//
// BOTH ask the GATEWAY for the external address rather than asking a "what is my
// IP" website. That is deliberate: the website version would send this player's
// address to a third party in order to tell them their own address, which is a
// strange thing to do inside a feature about not leaking it.
//
// Everything here that can be tested without a router is a free function below —
// the byte layouts, the XML and header parsing, and the address classifier. The
// socket work is the part that needs a real network, and it is deliberately the
// thin part.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hr::net {

// How a mapping attempt ended. `Trying` is the state while the worker is out.
enum class MapState : std::uint8_t {
  Off,       // never asked
  Trying,    // the worker is talking to the router
  Open,      // the port is forwarded and externalAddress is worth sharing
  Failed,    // no router answered, or it refused
  Unusable,  // a mapping was made, but the address behind it cannot be reached
};

// What the outside world would see. `address` is what the GATEWAY says, which on a
// double-NAT or a carrier-NAT connection is not the internet's view — hence
// MapState::Unusable, which is a mapping that exists and cannot help.
struct MapResult {
  MapState state = MapState::Off;
  std::string address;       // dotted quad as the gateway reported it
  std::uint16_t port = 0;    // external port, normally the same as the internal one
  std::string method;        // "NAT-PMP" or "UPnP IGD", for the panel and the doctor
  std::string detail;        // why it failed, or why the address is unusable
  std::uint32_t leaseSeconds = 0;
};

// Where an address can be reached from. The classifier is the reason a player is
// told "your provider is using carrier NAT" instead of being handed an address that
// will never work and left to wonder why nobody can join.
enum class Reach : std::uint8_t {
  Public,      // routable, worth sharing
  PrivateLan,  // RFC1918 — a mapping was made on an inner router of two
  CarrierNat,  // RFC6598 100.64/10 — the provider's NAT, and nothing here can open it
  Loopback,
  Invalid,
};

Reach classify(const std::string& dotted);
const char* reachExplanation(Reach reach);

// The router this machine would ask, as a dotted quad, or empty when none was
// found. Exposed for --net-doctor: "which router did it even talk to" is the first
// question when a forward does not happen, and the answer being empty is itself the
// finding — that is how a missing GAA_FLAG_INCLUDE_GATEWAYS was caught.
std::string defaultGatewayText();

// The first few routers between here and the internet, by walking the TTL up.
//
// This is the question UPnP cannot answer and that decides whether ANY of this can
// work. One private hop is an ordinary home: your router, then the internet. Two is
// double NAT, and a forward has to be made on both. Three or more, or a hop in
// 100.64/10, is the provider doing its own NAT — and then there is no public
// address belonging to this connection at all, so no setting on any router in the
// house can help.
//
// Diagnostic only, and Windows only: it uses the ICMP helper in iphlpapi, which
// needs no elevation. Elsewhere it returns nothing and the doctor says so rather
// than guessing.
struct Hop {
  std::string address;
  Reach reach = Reach::Invalid;
};
std::vector<Hop> firstHops(int maxHops = 8);

// What the hop list means, as one sentence, or empty when it looks ordinary.
std::string hopVerdict(const std::vector<Hop>& hops);

// --- NAT-PMP, as bytes -------------------------------------------------------
//
// Pure, so the layouts can be asserted without a gateway to talk to.

std::vector<std::uint8_t> natpmpAddressRequest();
std::vector<std::uint8_t> natpmpMapRequest(std::uint16_t internalPort,
                                           std::uint16_t externalPort,
                                           std::uint32_t lifetimeSeconds);
// Both return false with `error` set on a short, malformed, or non-zero-result
// packet. `resultCode` is the gateway's own, which is worth reporting: 2 is "not
// authorised", which is a router with UPnP/NAT-PMP deliberately switched off, and
// that is a different conversation from "no router answered".
bool natpmpParseAddress(const std::uint8_t* data, std::size_t size, std::string& address,
                        std::string& error);
bool natpmpParseMap(const std::uint8_t* data, std::size_t size, std::uint16_t& externalPort,
                    std::uint32_t& lifetime, std::string& error);

// --- UPnP IGD, as text -------------------------------------------------------

// The LOCATION header out of an SSDP reply. Case-insensitive: routers disagree
// about capitalisation and several send `Location:`.
bool ssdpLocation(const std::string& reply, std::string& url);
// Splits http://host:port/path into its parts. Returns false on anything that is
// not plain http — an https device description is not something to chase.
bool splitUrl(const std::string& url, std::string& host, std::uint16_t& port,
              std::string& path);
// The control URL and service type of the first WAN connection service in a device
// description. Both WANIPConnection and WANPPPConnection are accepted; a router
// with a modem behind it often only offers the latter.
bool upnpControlUrl(const std::string& deviceXml, std::string& controlPath,
                    std::string& serviceType);
// The text of one XML element, with no namespace assumptions — routers prefix
// these inconsistently and a strict parse would refuse working hardware.
bool xmlValue(const std::string& xml, const std::string& tag, std::string& out);

// --- the mapper --------------------------------------------------------------
//
// One outstanding attempt at a time. Every call is non-blocking: the socket work
// happens on a worker thread, and the frame only ever asks whether it has finished.
class PortMapper {
 public:
  PortMapper() = default;
  ~PortMapper();
  PortMapper(const PortMapper&) = delete;
  PortMapper& operator=(const PortMapper&) = delete;

  // Starts an attempt. Harmless to call when one is already running or a mapping is
  // already open — it is ignored rather than stacking.
  void begin(std::uint16_t port);
  // The current state, safe to call every frame.
  MapResult result() const;
  bool busy() const { return state_.load() == MapState::Trying; }

  // Gives the port back. Blocking and short, because it runs while the game is
  // shutting down and the alternative is a detached thread racing the process
  // exiting — which is how a port stays open after the game has gone.
  //
  // Safe to call when nothing is open, and safe to call twice.
  void release();

  // Renews a lease that is running out. Called from the host's update; does nothing
  // until the lease is more than half gone, so it is cheap to call every frame.
  void tick(double dt);

 private:
  void join();

  std::thread worker_;
  std::atomic<MapState> state_ {MapState::Off};
  // Written by the worker, read by the frame, and only ever after state_ leaves
  // Trying — which the atomic's release/acquire ordering is what makes safe.
  MapResult pending_;
  std::uint16_t port_ = 0;
  double sinceRenew_ = 0.0;
  bool opened_ = false;
};

}  // namespace hr::net
