#include "net/interfaces.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace hr::net {
namespace {

std::string dotted(std::uint32_t networkOrder) {
  const std::uint8_t* b = reinterpret_cast<const std::uint8_t*>(&networkOrder);
  char buf[24];
  std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return buf;
}

// Host-order helpers, so the masking reads the way the arithmetic does rather
// than the way the bytes happen to sit.
std::uint32_t toHost(std::uint32_t networkOrder) { return ntohl(networkOrder); }
std::uint32_t toNetwork(std::uint32_t hostOrder) { return htonl(hostOrder); }

// Why a beacon will not go out here, or empty if it will.
std::string whySkipped(std::uint32_t hostAddr, int prefix) {
  if (hostAddr == 0) return "no address";
  if ((hostAddr >> 24) == 127) return "loopback";
  if (prefix > 30) return "point-to-point (/" + std::to_string(prefix) +
                          "), no subnet to broadcast into";
  if (prefix < 0) return "no netmask";
  return {};
}

void add(std::vector<Interface>& out, const std::string& name, std::uint32_t netAddr,
         int prefix) {
  const std::uint32_t host = toHost(netAddr);
  Interface iface;
  iface.name = name;
  iface.address = netAddr;
  iface.addressText = dotted(netAddr);
  iface.prefix = prefix;
  iface.skipped = whySkipped(host, prefix);
  if (iface.skipped.empty()) {
    const std::uint32_t mask = prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));
    iface.broadcast = toNetwork(host | ~mask);
    iface.broadcastText = dotted(iface.broadcast);
  }
  out.push_back(std::move(iface));
}

}  // namespace

#if defined(_WIN32)

std::vector<Interface> allInterfaces() {
  std::vector<Interface> out;
  // Asked twice on purpose: the buffer size depends on how many adapters exist
  // and the answer can change between the two calls, so a failure to fit is
  // retried rather than treated as an error.
  ULONG size = 16 * 1024;
  for (int attempt = 0; attempt < 3; ++attempt) {
    std::vector<std::uint8_t> buffer(size);
    auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    // The friendly name is deliberately NOT skipped. The whole reason these get
    // logged is so somebody staring at a discovery problem can see "Ethernet" and
    // "vEthernet (Default Switch)" and recognise which is which; AdapterName is a
    // GUID and tells them nothing.
    const ULONG flags =
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    const ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, first, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) continue;
    if (rc != NO_ERROR) return out;

    for (auto* a = first; a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp) continue;
      if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
      std::string name = "?";
      if (a->FriendlyName) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nullptr, 0, nullptr,
                                          nullptr);
        if (n > 1) {
          name.assign(static_cast<std::size_t>(n) - 1, '\0');
          WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name.data(), n, nullptr, nullptr);
        }
      } else if (a->AdapterName) {
        name = a->AdapterName;
      }
      for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
        if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
        std::uint32_t netAddr = 0;
        std::memcpy(&netAddr, &sin->sin_addr, sizeof netAddr);
        add(out, name, netAddr, u->OnLinkPrefixLength);
      }
    }
    return out;
  }
  return out;
}

#else

std::vector<Interface> allInterfaces() {
  std::vector<Interface> out;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0 || !list) return out;
  for (ifaddrs* a = list; a; a = a->ifa_next) {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
    if (!(a->ifa_flags & IFF_UP)) continue;
    if (a->ifa_flags & IFF_LOOPBACK) continue;

    std::uint32_t netAddr = 0;
    std::memcpy(&netAddr,
                &reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr, sizeof netAddr);

    // The kernel already knows the broadcast address when the interface has one,
    // and it is more trustworthy than recomputing it from the mask.
    if ((a->ifa_flags & IFF_BROADCAST) && a->ifa_broadaddr) {
      std::uint32_t bcast = 0;
      std::memcpy(&bcast,
                  &reinterpret_cast<const sockaddr_in*>(a->ifa_broadaddr)->sin_addr,
                  sizeof bcast);
      const std::uint32_t host = toHost(netAddr);
      Interface iface;
      iface.name = a->ifa_name ? a->ifa_name : "?";
      iface.address = netAddr;
      iface.addressText = dotted(netAddr);
      if (bcast != 0 && (host >> 24) != 127) {
        iface.broadcast = bcast;
        iface.broadcastText = dotted(bcast);
      } else {
        iface.skipped = (host >> 24) == 127 ? "loopback" : "no broadcast address";
      }
      out.push_back(std::move(iface));
      continue;
    }

    int prefix = 0;
    if (a->ifa_netmask) {
      std::uint32_t mask = 0;
      std::memcpy(&mask,
                  &reinterpret_cast<const sockaddr_in*>(a->ifa_netmask)->sin_addr, sizeof mask);
      std::uint32_t bits = toHost(mask);
      while (bits & 0x80000000u) {
        ++prefix;
        bits <<= 1;
      }
    }
    add(out, a->ifa_name ? a->ifa_name : "?", netAddr, prefix);
  }
  freeifaddrs(list);
  return out;
}

#endif

std::vector<Interface> localInterfaces() {
  std::vector<Interface> out;
  for (Interface& iface : allInterfaces()) {
    if (iface.skipped.empty()) out.push_back(std::move(iface));
  }
  return out;
}

}  // namespace hr::net
