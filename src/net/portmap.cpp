#include "net/portmap.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <enet/enet.h>

#include "core/log.h"
#include "net/discovery.h"
#include "net/interfaces.h"
#include "net/transport.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#else
#include <cstdio>
#endif

namespace hr::net {
namespace {

// One hour. Long enough that nobody watches it expire mid-session, short enough
// that a crashed game stops being a hole in the router by teatime. The renew in
// tick() is what keeps a long session alive.
constexpr std::uint32_t kLeaseSeconds = 3600;
constexpr std::uint16_t kNatPmpPort = 5351;
constexpr int kNatPmpTimeoutMs = 700;
constexpr int kSsdpTimeoutMs = 1500;
constexpr int kHttpTimeoutMs = 2500;

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool parseDotted(const std::string& text, std::uint32_t& out) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  char extra = 0;
  if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  out = (a << 24) | (b << 16) | (c << 8) | d;
  return true;
}

std::string dotted(std::uint32_t host) {
  char text[24];
  std::snprintf(text, sizeof text, "%u.%u.%u.%u", (host >> 24) & 0xFF, (host >> 16) & 0xFF,
                (host >> 8) & 0xFF, host & 0xFF);
  return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

Reach classify(const std::string& text) {
  std::uint32_t host = 0;
  if (!parseDotted(text, host)) return Reach::Invalid;
  const std::uint32_t a = (host >> 24) & 0xFF;
  const std::uint32_t b = (host >> 16) & 0xFF;
  if (a == 127) return Reach::Loopback;
  if (a == 0 || a >= 240) return Reach::Invalid;
  // RFC 6598, and the reason this whole enum exists. A provider that hands out
  // 100.64.x.x is doing its own NAT in front of the customer's router, and no
  // amount of asking the customer's router will open a path through it.
  if (a == 100 && b >= 64 && b <= 127) return Reach::CarrierNat;
  if (a == 10) return Reach::PrivateLan;
  if (a == 192 && b == 168) return Reach::PrivateLan;
  if (a == 172 && b >= 16 && b <= 31) return Reach::PrivateLan;
  if (a == 169 && b == 254) return Reach::Invalid;  // link-local: no DHCP answered
  return Reach::Public;
}

const char* reachExplanation(Reach reach) {
  switch (reach) {
    case Reach::Public: return "";
    case Reach::PrivateLan:
      return "The router opened the port, but the address behind it is still a private "
             "one — there is a second router or modem in front of this one, and it "
             "would need the same port opened by hand.";
    case Reach::CarrierNat:
      return "Your internet provider is using carrier-grade NAT, so this connection "
             "has no address of its own to share. No setting here can change that; "
             "ask them for a public address, or use a relay such as Tailscale.";
    case Reach::Loopback: return "The router reported a loopback address, which cannot be right.";
    case Reach::Invalid: return "The router reported an address that is not usable.";
  }
  return "";
}

// ---------------------------------------------------------------------------
// NAT-PMP, as bytes
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> natpmpAddressRequest() { return {0, 0}; }

std::vector<std::uint8_t> natpmpMapRequest(std::uint16_t internalPort,
                                           std::uint16_t externalPort,
                                           std::uint32_t lifetimeSeconds) {
  // version 0, opcode 1 (map UDP), two reserved bytes, then the ports and lifetime
  // big-endian. RFC 6886 section 3.3.
  std::vector<std::uint8_t> out;
  out.reserve(12);
  out.push_back(0);
  out.push_back(1);
  out.push_back(0);
  out.push_back(0);
  out.push_back(static_cast<std::uint8_t>(internalPort >> 8));
  out.push_back(static_cast<std::uint8_t>(internalPort & 0xFF));
  out.push_back(static_cast<std::uint8_t>(externalPort >> 8));
  out.push_back(static_cast<std::uint8_t>(externalPort & 0xFF));
  out.push_back(static_cast<std::uint8_t>((lifetimeSeconds >> 24) & 0xFF));
  out.push_back(static_cast<std::uint8_t>((lifetimeSeconds >> 16) & 0xFF));
  out.push_back(static_cast<std::uint8_t>((lifetimeSeconds >> 8) & 0xFF));
  out.push_back(static_cast<std::uint8_t>(lifetimeSeconds & 0xFF));
  return out;
}

namespace {

// The two bytes every reply starts with, plus the result code. Shared because
// getting the opcode check wrong in one of the two parsers would mean accepting a
// mapping reply as an address reply and reading the lifetime as an IP.
bool natpmpHeader(const std::uint8_t* data, std::size_t size, std::uint8_t wantOpcode,
                  std::string& error) {
  if (size < 8) {
    error = "the router's reply was too short";
    return false;
  }
  if (data[0] != 0) {
    error = "the router answered with a version this build does not speak";
    return false;
  }
  if (data[1] != wantOpcode) {
    error = "the router answered a different request";
    return false;
  }
  const std::uint16_t result = static_cast<std::uint16_t>((data[2] << 8) | data[3]);
  if (result == 0) return true;
  switch (result) {
    case 2: error = "the router refused: port forwarding is switched off on it"; break;
    case 3: error = "the router says its own internet connection is down"; break;
    case 4: error = "the router is out of forwarding entries"; break;
    case 5: error = "the router does not allow this kind of forward"; break;
    default: error = "the router refused, code " + std::to_string(result); break;
  }
  return false;
}

}  // namespace

bool natpmpParseAddress(const std::uint8_t* data, std::size_t size, std::string& address,
                        std::string& error) {
  if (!natpmpHeader(data, size, 128, error)) return false;
  if (size < 12) {
    error = "the router's address reply was too short";
    return false;
  }
  const std::uint32_t host = (static_cast<std::uint32_t>(data[8]) << 24) |
                             (static_cast<std::uint32_t>(data[9]) << 16) |
                             (static_cast<std::uint32_t>(data[10]) << 8) |
                             static_cast<std::uint32_t>(data[11]);
  address = dotted(host);
  return true;
}

bool natpmpParseMap(const std::uint8_t* data, std::size_t size, std::uint16_t& externalPort,
                    std::uint32_t& lifetime, std::string& error) {
  if (!natpmpHeader(data, size, 129, error)) return false;
  if (size < 16) {
    error = "the router's mapping reply was too short";
    return false;
  }
  externalPort = static_cast<std::uint16_t>((data[10] << 8) | data[11]);
  lifetime = (static_cast<std::uint32_t>(data[12]) << 24) |
             (static_cast<std::uint32_t>(data[13]) << 16) |
             (static_cast<std::uint32_t>(data[14]) << 8) |
             static_cast<std::uint32_t>(data[15]);
  if (externalPort == 0) {
    error = "the router agreed and then gave port zero";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// UPnP IGD, as text
// ---------------------------------------------------------------------------

bool ssdpLocation(const std::string& reply, std::string& url) {
  const std::string haystack = lower(reply);
  std::size_t at = haystack.find("location:");
  if (at == std::string::npos) return false;
  at += 9;
  const std::size_t end = reply.find_first_of("\r\n", at);
  url = trim(reply.substr(at, end == std::string::npos ? std::string::npos : end - at));
  return !url.empty();
}

bool splitUrl(const std::string& url, std::string& host, std::uint16_t& port,
              std::string& path) {
  const std::string prefix = "http://";
  if (lower(url).rfind(prefix, 0) != 0) return false;
  const std::string rest = url.substr(prefix.size());
  const std::size_t slash = rest.find('/');
  const std::string authority = rest.substr(0, slash);
  path = slash == std::string::npos ? "/" : rest.substr(slash);
  const std::size_t colon = authority.find(':');
  if (colon == std::string::npos) {
    host = authority;
    port = 80;
  } else {
    host = authority.substr(0, colon);
    const int n = std::atoi(authority.substr(colon + 1).c_str());
    if (n <= 0 || n > 65535) return false;
    port = static_cast<std::uint16_t>(n);
  }
  return !host.empty();
}

bool upnpControlUrl(const std::string& deviceXml, std::string& controlPath,
                    std::string& serviceType) {
  // Walked rather than parsed. A device description is a nest of <service> blocks
  // and the one wanted is identified by its serviceType, so the job is to find that
  // string and then the controlURL that belongs to the SAME block — which is why
  // this searches forward from the match rather than searching the whole document
  // for a controlURL and hoping it is the right one.
  static const char* kWanted[] = {"WANIPConnection:", "WANPPPConnection:"};
  for (const char* want : kWanted) {
    std::size_t at = deviceXml.find(want);
    while (at != std::string::npos) {
      // Back up to the start of the serviceType element to read the whole URN.
      const std::size_t typeOpen = deviceXml.rfind('>', at);
      const std::size_t typeClose = deviceXml.find('<', at);
      if (typeOpen == std::string::npos || typeClose == std::string::npos) break;
      const std::string type = trim(deviceXml.substr(typeOpen + 1, typeClose - typeOpen - 1));

      const std::size_t control = deviceXml.find("controlURL", typeClose);
      if (control != std::string::npos) {
        const std::size_t open = deviceXml.find('>', control);
        const std::size_t close = deviceXml.find('<', open == std::string::npos ? control : open);
        if (open != std::string::npos && close != std::string::npos) {
          controlPath = trim(deviceXml.substr(open + 1, close - open - 1));
          serviceType = type;
          if (!controlPath.empty() && !serviceType.empty()) return true;
        }
      }
      at = deviceXml.find(want, at + 1);
    }
  }
  return false;
}

bool xmlValue(const std::string& xml, const std::string& tag, std::string& out) {
  // Namespace-insensitive on purpose: routers send <NewExternalIPAddress>,
  // <m:NewExternalIPAddress> and <u:NewExternalIPAddress>, and refusing two of
  // those would mean refusing working hardware over punctuation.
  std::size_t at = xml.find("<" + tag);
  while (at != std::string::npos) {
    const char before = at == 0 ? '\0' : xml[at - 1];
    (void)before;
    const std::size_t open = xml.find('>', at);
    if (open == std::string::npos) return false;
    const std::size_t close = xml.find("</", open);
    if (close == std::string::npos) return false;
    out = trim(xml.substr(open + 1, close - open - 1));
    return true;
  }
  // Try again allowing a namespace prefix: find ":tag" instead.
  at = xml.find(":" + tag);
  if (at == std::string::npos) return false;
  const std::size_t open = xml.find('>', at);
  if (open == std::string::npos) return false;
  const std::size_t close = xml.find("</", open);
  if (close == std::string::npos) return false;
  out = trim(xml.substr(open + 1, close - open - 1));
  return true;
}

// ---------------------------------------------------------------------------
// The socket half
// ---------------------------------------------------------------------------

namespace {

// The default gateway, for NAT-PMP. UPnP does not need this — it multicasts — which
// is why a platform without an implementation here still gets a mapping.
std::uint32_t defaultGateway() {
#if defined(_WIN32)
  ULONG size = 16 * 1024;
  std::vector<std::uint8_t> buffer(size);
  auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  // GAA_FLAG_INCLUDE_GATEWAYS is not optional here, and leaving it out does not
  // fail: FirstGatewayAddress is simply left null on every adapter, so this
  // reported "no default gateway to ask" on a machine sitting on a working
  // network. --net-doctor is what caught it.
  const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                      GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
  ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, first, &size);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    buffer.assign(size, 0);
    first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    rc = GetAdaptersAddresses(AF_INET, flags, nullptr, first, &size);
  }
  if (rc != NO_ERROR) return 0;
  for (IP_ADAPTER_ADDRESSES* a = first; a != nullptr; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (IP_ADAPTER_GATEWAY_ADDRESS* g = a->FirstGatewayAddress; g != nullptr; g = g->Next) {
      if (g->Address.lpSockaddr == nullptr) continue;
      if (g->Address.lpSockaddr->sa_family != AF_INET) continue;
      const auto* in = reinterpret_cast<const sockaddr_in*>(g->Address.lpSockaddr);
      const std::uint32_t host = ntohl(in->sin_addr.s_addr);
      if (host != 0) return host;
    }
  }
  return 0;
#elif defined(__linux__)
  // /proc/net/route, whose second column is the destination and third the gateway,
  // both little-endian hex. The default route is the one with destination 0.
  std::FILE* f = std::fopen("/proc/net/route", "r");
  if (f == nullptr) return 0;
  char line[512];
  std::uint32_t gateway = 0;
  bool first = true;
  while (std::fgets(line, sizeof line, f) != nullptr) {
    if (first) {
      first = false;
      continue;  // header
    }
    char iface[64] = {};
    unsigned long dest = 0, gw = 0;
    if (std::sscanf(line, "%63s %lx %lx", iface, &dest, &gw) == 3 && dest == 0 && gw != 0) {
      gateway = ntohl(static_cast<std::uint32_t>(gw));
      break;
    }
  }
  std::fclose(f);
  return gateway;
#else
  return 0;
#endif
}

}  // namespace

std::string defaultGatewayText() {
  const std::uint32_t gateway = defaultGateway();
  return gateway == 0 ? std::string() : dotted(gateway);
}

std::vector<Hop> firstHops(int maxHops) {
  std::vector<Hop> out;
#if defined(_WIN32)
  // 1.1.1.1 purely as something to aim at. Nothing is sent to it: every probe here
  // expires in the middle and is answered by a router along the way, which is the
  // only thing being asked for.
  const HANDLE icmp = IcmpCreateFile();
  if (icmp == INVALID_HANDLE_VALUE) return out;
  const IPAddr target = htonl(0x01010101);
  unsigned char payload[32] = {};
  std::vector<unsigned char> reply(sizeof(ICMP_ECHO_REPLY) + sizeof payload + 64);

  for (int ttl = 1; ttl <= maxHops; ++ttl) {
    IP_OPTION_INFORMATION options = {};
    options.Ttl = static_cast<UCHAR>(ttl);
    const DWORD got = IcmpSendEcho(icmp, target, payload, sizeof payload, &options,
                                   reply.data(), static_cast<DWORD>(reply.size()), 1500);
    if (got == 0) break;  // nothing came back at this distance; stop rather than guess
    const auto* echo = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply.data());
    Hop hop;
    hop.address = dotted(ntohl(echo->Address));
    hop.reach = classify(hop.address);
    out.push_back(hop);
    // The first public hop is the internet, and everything past it is somebody
    // else's business.
    if (hop.reach == Reach::Public) break;
  }
  IcmpCloseHandle(icmp);
#else
  (void)maxHops;
#endif
  return out;
}

std::string hopVerdict(const std::vector<Hop>& hops) {
  if (hops.empty()) return {};
  int privateHops = 0;
  bool carrier = false;
  bool sawPublic = false;
  for (const Hop& hop : hops) {
    if (hop.reach == Reach::Public) {
      sawPublic = true;
      break;
    }
    if (hop.reach == Reach::CarrierNat) carrier = true;
    if (hop.reach == Reach::PrivateLan || hop.reach == Reach::CarrierNat) ++privateHops;
  }
  // 100.64/10 is the provider saying so in as many words. More than two private
  // hops says the same thing less directly: a house has one or two routers, and
  // the ones past that belong to the provider's own network.
  if (carrier || privateHops >= 3) {
    return "Your internet provider is doing its own NAT: there are private hops "
           "beyond your own routers. This connection has no public address, so no "
           "port forwarding on any router in the house can make it reachable. A "
           "relay such as Tailscale is the way to play with distant friends here.";
  }
  if (privateHops == 2) {
    return "There are two routers between this machine and the internet. A forward "
           "has to be made on BOTH — the outer one pointing at the inner one's WAN "
           "address — and the game can only ever ask the nearer of the two.";
  }
  if (!sawPublic) {
    return "The internet was not reached within the hops checked, which usually "
           "means more than one router in front of this machine.";
  }
  return {};
}

namespace {

// One request, one reply, on a datagram socket. Used by both NAT-PMP and SSDP.
bool exchange(ENetSocket socket, const ENetAddress& to, const std::vector<std::uint8_t>& out,
              int timeoutMs, std::vector<std::uint8_t>& reply) {
  ENetBuffer send;
  send.data = const_cast<std::uint8_t*>(out.data());
  send.dataLength = out.size();
  if (enet_socket_send(socket, &to, &send, 1) < 0) return false;

  // enet's wait takes the condition by pointer and writes back what actually
  // happened, so a timeout is "returned zero and cleared the flag" rather than a
  // non-zero return. Checking only the return value would treat every timeout as a
  // successful wait and then block in receive.
  enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
  if (enet_socket_wait(socket, &condition, static_cast<enet_uint32>(timeoutMs)) != 0) {
    return false;
  }
  if ((condition & ENET_SOCKET_WAIT_RECEIVE) == 0) return false;
  reply.assign(2048, 0);
  ENetAddress from;
  ENetBuffer in;
  in.data = reply.data();
  in.dataLength = reply.size();
  const int got = enet_socket_receive(socket, &from, &in, 1);
  if (got <= 0) return false;
  reply.resize(static_cast<std::size_t>(got));
  return true;
}

// A whole HTTP exchange over a stream socket. Small and blocking, on the worker.
bool http(const std::string& host, std::uint16_t port, const std::string& request,
          std::string& response) {
  ENetAddress address;
  if (enet_address_set_host_ip(&address, host.c_str()) != 0) return false;
  address.port = port;
  const ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
  if (s == ENET_SOCKET_NULL) return false;
  bool ok = false;
  if (enet_socket_connect(s, &address) == 0) {
    ENetBuffer out;
    out.data = const_cast<char*>(request.data());
    out.dataLength = request.size();
    if (enet_socket_send(s, nullptr, &out, 1) >= 0) {
      response.clear();
      char chunk[2048];
      for (int i = 0; i < 32; ++i) {
        enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
        if (enet_socket_wait(s, &condition, kHttpTimeoutMs) != 0) break;
        if ((condition & ENET_SOCKET_WAIT_RECEIVE) == 0) break;  // timed out: no more coming
        ENetBuffer in;
        in.data = chunk;
        in.dataLength = sizeof chunk;
        const int got = enet_socket_receive(s, nullptr, &in, 1);
        if (got <= 0) break;
        response.append(chunk, static_cast<std::size_t>(got));
        if (response.size() > 256 * 1024) break;  // a device description is not this big
      }
      ok = !response.empty();
    }
  }
  enet_socket_destroy(s);
  return ok;
}

// --- NAT-PMP -----------------------------------------------------------------

bool tryNatPmp(std::uint16_t port, MapResult& out) {
  const std::uint32_t gateway = defaultGateway();
  if (gateway == 0) {
    out.detail = "no default gateway to ask";
    return false;
  }
  const ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) return false;

  ENetAddress to;
  to.host = htonl(gateway);
  to.port = kNatPmpPort;

  std::vector<std::uint8_t> reply;
  bool ok = false;
  if (exchange(s, to, natpmpAddressRequest(), kNatPmpTimeoutMs, reply)) {
    std::string error;
    if (natpmpParseAddress(reply.data(), reply.size(), out.address, error)) {
      std::uint16_t external = 0;
      std::uint32_t lease = 0;
      if (exchange(s, to, natpmpMapRequest(port, port, kLeaseSeconds), kNatPmpTimeoutMs,
                   reply) &&
          natpmpParseMap(reply.data(), reply.size(), external, lease, error)) {
        out.port = external;
        out.leaseSeconds = lease;
        out.method = "NAT-PMP";
        ok = true;
      } else {
        out.detail = error;
      }
    } else {
      out.detail = error;
    }
  } else {
    out.detail = "no answer from the router on NAT-PMP";
  }
  enet_socket_destroy(s);
  return ok;
}

// --- UPnP --------------------------------------------------------------------

std::string soapEnvelope(const std::string& serviceType, const std::string& action,
                         const std::string& body) {
  return "<?xml version=\"1.0\"?>"
         "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
         "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
         "<u:" + action + " xmlns:u=\"" + serviceType + "\">" + body +
         "</u:" + action + "></s:Body></s:Envelope>";
}

bool soapCall(const std::string& host, std::uint16_t port, const std::string& path,
              const std::string& serviceType, const std::string& action,
              const std::string& body, std::string& response) {
  const std::string envelope = soapEnvelope(serviceType, action, body);
  const std::string request =
      "POST " + path + " HTTP/1.1\r\n"
      "HOST: " + host + ":" + std::to_string(port) + "\r\n"
      "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
      "SOAPACTION: \"" + serviceType + "#" + action + "\"\r\n"
      "CONTENT-LENGTH: " + std::to_string(envelope.size()) + "\r\n"
      "CONNECTION: close\r\n\r\n" + envelope;
  return http(host, port, request, response);
}

// Finds the gateway's device description and the control URL inside it.
// Routers disagree about which search they will answer, so ask several ways.
// Observed on the router this was developed against: it answers NONE of these,
// while a television on the same network answers `ssdp:all` and `upnp:rootdevice`
// happily — which is exactly why the device-type search alone is not enough to
// conclude anything, and why every reply is checked for a WAN service rather than
// trusted because it arrived.
const char* const kSearchTypes[] = {
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
    "upnp:rootdevice",
    "ssdp:all",
};

std::string searchText(const char* serviceType) {
  return std::string(
             "M-SEARCH * HTTP/1.1\r\n"
             "HOST: 239.255.255.250:1900\r\n"
             "MAN: \"ssdp:discover\"\r\n"
             "MX: 2\r\n"
             "ST: ") +
         serviceType + "\r\n\r\n";
}

// One search out of ONE interface, and every reply it draws, appended to `out`.
//
// `bindTo` is why this function exists. A datagram to a multicast group from an
// unbound socket leaves by exactly one interface, chosen by the routing table —
// and on a developer machine that is as likely to be a Hyper-V switch, a VPN or
// WSL as the network the router is on. The M-SEARCH is then delivered perfectly
// into a network with no router in it, send() succeeds, and nothing answers.
//
// net/interfaces.h documents this at length for BROADCAST, which is where it was
// first met. It is the same trap for multicast, and this code walked straight
// into it: a machine with a working, UPnP-enabled router reported "no UPnP
// gateway answered" because the search went out through a virtual switch.
// Binding the source address is what pins the outgoing interface.
void searchFrom(std::uint32_t bindTo, const char* serviceType,
                std::vector<std::string>& out) {
  const ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) return;
  enet_socket_set_option(s, ENET_SOCKOPT_BROADCAST, 1);
  enet_socket_set_option(s, ENET_SOCKOPT_REUSEADDR, 1);
  if (bindTo != 0) {
    ENetAddress bind;
    bind.host = bindTo;
    bind.port = 0;
    if (enet_socket_bind(s, &bind) != 0) {
      enet_socket_destroy(s);
      return;
    }
  }

  ENetAddress to;
  if (enet_address_set_host_ip(&to, "239.255.255.250") != 0) {
    enet_socket_destroy(s);
    return;
  }
  to.port = 1900;

  const std::string request = searchText(serviceType);
  ENetBuffer send;
  send.data = const_cast<char*>(request.data());
  send.dataLength = request.size();
  if (enet_socket_send(s, &to, &send, 1) < 0) {
    enet_socket_destroy(s);
    return;
  }

  // EVERY reply, not the first. A house has more UPnP devices than a router —
  // televisions, printers, media servers — and taking whichever answered fastest
  // is how the search "succeeds" and then finds no WAN service on a smart TV.
  for (int i = 0; i < 12; ++i) {
    enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
    if (enet_socket_wait(s, &condition, kSsdpTimeoutMs) != 0) break;
    if ((condition & ENET_SOCKET_WAIT_RECEIVE) == 0) break;
    char buffer[2048];
    ENetAddress from;
    ENetBuffer in;
    in.data = buffer;
    in.dataLength = sizeof buffer;
    const int got = enet_socket_receive(s, &from, &in, 1);
    if (got <= 0) break;
    std::string location;
    if (ssdpLocation(std::string(buffer, static_cast<std::size_t>(got)), location)) {
      out.push_back(location);
    }
  }
  enet_socket_destroy(s);
}

// `devicesSeen` is how many UPnP devices answered at all, which is the difference
// between two failures that look identical and are not: a network where nothing
// speaks UPnP, and a network where a television answers cheerfully and the router
// says nothing. The second is the one where the checkbox in the router's settings
// is ticked and the player is entitled to be confused.
bool discoverIgd(std::string& host, std::uint16_t& port, std::string& controlPath,
                 std::string& serviceType, int& devicesSeen) {
  std::vector<std::string> locations;
  // Every interface the machine has a real network on, then an unbound search as
  // well — a machine whose interface list comes back empty still gets one try.
  // Stops at the first search type that produces anything, so the usual case of a
  // router answering the device search costs one round trip rather than five.
  for (const char* serviceType : kSearchTypes) {
    for (const Interface& iface : localInterfaces()) {
      searchFrom(iface.address, serviceType, locations);
    }
    searchFrom(0, serviceType, locations);
    if (!locations.empty()) break;
  }

  std::sort(locations.begin(), locations.end());
  locations.erase(std::unique(locations.begin(), locations.end()), locations.end());
  devicesSeen = static_cast<int>(locations.size());

  // Each candidate is fetched and read until one of them turns out to have a WAN
  // connection service. The gateway is not necessarily the first to answer.
  for (const std::string& location : locations) {
    std::string descPath;
    if (!splitUrl(location, host, port, descPath)) continue;
    const std::string request = "GET " + descPath + " HTTP/1.1\r\nHOST: " + host + ":" +
                                std::to_string(port) + "\r\nCONNECTION: close\r\n\r\n";
    std::string description;
    if (!http(host, port, request, description)) continue;
    if (upnpControlUrl(description, controlPath, serviceType)) return true;
  }
  return false;
}

bool tryUpnp(std::uint16_t port, MapResult& out) {
  std::string host, controlPath, serviceType;
  std::uint16_t httpPort = 0;
  int devicesSeen = 0;
  if (!discoverIgd(host, httpPort, controlPath, serviceType, devicesSeen)) {
    out.detail = devicesSeen == 0
                     ? "no UPnP device answered on this network at all"
                     : std::to_string(devicesSeen) +
                           " UPnP device(s) answered, but none of them was a router that "
                           "forwards ports — if your router's UPnP setting is on, it is "
                           "not replying";
    return false;
  }
  // A relative controlURL is the common case; an absolute one happens too.
  if (!controlPath.empty() && controlPath[0] != '/') {
    std::string h2, p2;
    std::uint16_t port2 = 0;
    if (splitUrl(controlPath, h2, port2, p2)) {
      host = h2;
      httpPort = port2;
      controlPath = p2;
    } else {
      controlPath = "/" + controlPath;
    }
  }

  const std::string self = localAddress();
  if (self.empty()) {
    out.detail = "could not work out this machine's address to forward to";
    return false;
  }

  const std::string body =
      "<NewRemoteHost></NewRemoteHost>"
      "<NewExternalPort>" + std::to_string(port) + "</NewExternalPort>"
      "<NewProtocol>UDP</NewProtocol>"
      "<NewInternalPort>" + std::to_string(port) + "</NewInternalPort>"
      "<NewInternalClient>" + self + "</NewInternalClient>"
      "<NewEnabled>1</NewEnabled>"
      "<NewPortMappingDescription>Hollowreach</NewPortMappingDescription>"
      "<NewLeaseDuration>" + std::to_string(kLeaseSeconds) + "</NewLeaseDuration>";

  std::string response;
  if (!soapCall(host, httpPort, controlPath, serviceType, "AddPortMapping", body, response)) {
    out.detail = "the router did not answer the forwarding request";
    return false;
  }
  if (response.find(" 200 ") == std::string::npos) {
    std::string code;
    if (xmlValue(response, "errorCode", code)) {
      // 725 is the one worth naming: the router only does permanent leases, so the
      // whole request is refused over the lease duration rather than the mapping.
      out.detail = code == "725" ? "the router refuses timed forwards (error 725)"
                                 : "the router refused, error " + code;
    } else {
      out.detail = "the router refused the forwarding request";
    }
    return false;
  }

  std::string external;
  if (soapCall(host, httpPort, controlPath, serviceType, "GetExternalIPAddress", "",
               response) &&
      xmlValue(response, "NewExternalIPAddress", external) && !external.empty()) {
    out.address = external;
  }
  out.port = port;
  out.leaseSeconds = kLeaseSeconds;
  out.method = "UPnP IGD";
  return true;
}

void deleteUpnp(std::uint16_t port) {
  std::string host, controlPath, serviceType;
  std::uint16_t httpPort = 0;
  int devicesSeen = 0;
  if (!discoverIgd(host, httpPort, controlPath, serviceType, devicesSeen)) return;
  if (!controlPath.empty() && controlPath[0] != '/') controlPath = "/" + controlPath;
  const std::string body =
      "<NewRemoteHost></NewRemoteHost>"
      "<NewExternalPort>" + std::to_string(port) + "</NewExternalPort>"
      "<NewProtocol>UDP</NewProtocol>";
  std::string response;
  soapCall(host, httpPort, controlPath, serviceType, "DeletePortMapping", body, response);
}

void deleteNatPmp(std::uint16_t port) {
  const std::uint32_t gateway = defaultGateway();
  if (gateway == 0) return;
  const ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) return;
  ENetAddress to;
  to.host = htonl(gateway);
  to.port = kNatPmpPort;
  // RFC 6886: a lifetime of zero deletes.
  std::vector<std::uint8_t> reply;
  exchange(s, to, natpmpMapRequest(port, 0, 0), kNatPmpTimeoutMs, reply);
  enet_socket_destroy(s);
}

}  // namespace

// ---------------------------------------------------------------------------
// PortMapper
// ---------------------------------------------------------------------------

PortMapper::~PortMapper() {
  release();
  join();
}

void PortMapper::join() {
  if (worker_.joinable()) worker_.join();
}

void PortMapper::begin(std::uint16_t port) {
  if (state_.load() == MapState::Trying || opened_) return;
  join();
  port_ = port;
  state_.store(MapState::Trying);
  pending_ = MapResult{};
  pending_.state = MapState::Trying;

  worker_ = std::thread([this, port] {
    MapResult found;
    if (!enetAcquire()) {
      found.state = MapState::Failed;
      found.detail = "networking is unavailable";
      pending_ = found;
      state_.store(MapState::Failed);
      return;
    }
    // NAT-PMP first because it answers in a fraction of a second when it is there;
    // UPnP takes seconds even when it works.
    bool ok = tryNatPmp(port, found);
    if (!ok) {
      const std::string natpmpWhy = found.detail;
      MapResult viaUpnp;
      if (tryUpnp(port, viaUpnp)) {
        found = viaUpnp;
        ok = true;
      } else {
        // BOTH reasons, joined. The first version kept whichever string was
        // longer, which is not a reason to prefer one diagnosis over another —
        // and it duly reported the NAT-PMP failure while saying nothing about
        // UPnP, which is the one that would have worked on most routers.
        found.detail = natpmpWhy.empty() ? viaUpnp.detail
                       : viaUpnp.detail.empty()
                           ? natpmpWhy
                           : "NAT-PMP: " + natpmpWhy + "; UPnP: " + viaUpnp.detail;
      }
    }
    enetRelease();

    if (!ok) {
      found.state = MapState::Failed;
      if (found.detail.empty()) found.detail = "no router here answered";
    } else {
      const Reach reach = classify(found.address);
      if (reach == Reach::Public) {
        found.state = MapState::Open;
      } else {
        // The port IS open. It simply cannot be reached, and saying "open" here
        // would send somebody off to share an address that can never work.
        found.state = MapState::Unusable;
        found.detail = reachExplanation(reach);
      }
    }
    pending_ = found;
    state_.store(found.state);
  });
}

MapResult PortMapper::result() const {
  const MapState state = state_.load();
  if (state == MapState::Trying) {
    MapResult out;
    out.state = MapState::Trying;
    return out;
  }
  if (state == MapState::Off) return MapResult{};
  return pending_;
}

void PortMapper::tick(double dt) {
  if (!opened_) {
    // The worker finishing is what turns an attempt into a mapping worth renewing.
    const MapState state = state_.load();
    if (state == MapState::Open || state == MapState::Unusable) {
      join();
      opened_ = true;
      sinceRenew_ = 0.0;
    }
    return;
  }
  const std::uint32_t lease = pending_.leaseSeconds > 0 ? pending_.leaseSeconds : kLeaseSeconds;
  sinceRenew_ += dt;
  // Half the lease, which is the usual rule: one lost renewal still leaves as long
  // again to try in before the router drops the mapping mid-game.
  if (sinceRenew_ < static_cast<double>(lease) * 0.5) return;
  sinceRenew_ = 0.0;
  const std::uint16_t port = port_;
  opened_ = false;
  state_.store(MapState::Off);
  begin(port);
}

void PortMapper::release() {
  // The worker may still be out. Waiting for it is the point: releasing while an
  // attempt is in flight would race the router into leaving the mapping behind,
  // which is the one outcome this whole function exists to prevent.
  join();
  const MapState state = state_.load();
  if (state != MapState::Open && state != MapState::Unusable) {
    state_.store(MapState::Off);
    opened_ = false;
    return;
  }
  if (!enetAcquire()) return;
  if (pending_.method == "NAT-PMP") {
    deleteNatPmp(port_);
  } else {
    deleteUpnp(pending_.port != 0 ? pending_.port : port_);
  }
  enetRelease();
  log::info("net: released the forwarded port");
  pending_ = MapResult{};
  state_.store(MapState::Off);
  opened_ = false;
}

}  // namespace hr::net
