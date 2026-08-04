#include "net/doctor.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "core/log.h"
#include "net/discovery.h"
#include "net/interfaces.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "platform/paths.h"

#ifndef HR_VERSION
#define HR_VERSION "dev"
#endif

namespace hr::net {
namespace {

void heading(const char* text) { std::printf("\n%s\n", text); }

// Can anything bind this UDP port? Answered by actually doing it, because the
// interesting failure — something else already holds it — cannot be predicted.
bool canBind(std::uint16_t port, std::string& detail) {
  if (!enetAcquire()) {
    detail = "networking is unavailable";
    return false;
  }
  const ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) {
    enetRelease();
    detail = "could not create a socket";
    return false;
  }
  // Without REUSEADDR this reports a false problem whenever the game is already
  // running, which is exactly when somebody is most likely to run this.
  enet_socket_set_option(s, ENET_SOCKOPT_REUSEADDR, 1);
  ENetAddress address;
  address.host = ENET_HOST_ANY;
  address.port = port;
  const bool ok = enet_socket_bind(s, &address) == 0;
  if (!ok) detail = "in use by another program";
  enet_socket_destroy(s);
  enetRelease();
  return ok;
}

}  // namespace

int runNetDoctor(std::uint16_t gamePort) {
  bool trouble = false;

  // The discovery classes log the interfaces they picked, which is the right
  // thing during a game and pure duplication inside a report that is about to
  // print the same list in a better form. Warnings and errors still come through,
  // because those are findings.
  log::setMinLevel(log::Level::Warn);

  std::printf("Hollowreach network check\n");
  std::printf("  build      %s\n", HR_VERSION);
  // Beacons carrying a different protocol version are dropped without comment, so
  // two machines on different builds see an empty list and no error whatsoever.
  // It is the first thing to compare between the two reports.
  std::printf("  protocol   %d   (both machines must match, or neither will list the "
              "other)\n",
              static_cast<int>(kNetVersion));
  const std::string& exe = paths::exePath();
  std::printf("  program    %s\n", exe.empty() ? "(unknown)" : exe.c_str());
  std::printf("             ^ Windows firewall rules are keyed on this exact path.\n");

  // ---- interfaces ----------------------------------------------------------
  heading("Networks a game here would be announced on");
  const std::vector<Interface> all = allInterfaces();
  int usable = 0;
  for (const Interface& iface : all) {
    if (iface.skipped.empty()) {
      ++usable;
      std::printf("  [use ] %-30s %-15s -> %s\n", iface.name.c_str(),
                  iface.addressText.c_str(), iface.broadcastText.c_str());
    } else {
      std::printf("  [skip] %-30s %-15s    %s\n", iface.name.c_str(),
                  iface.addressText.c_str(), iface.skipped.c_str());
    }
  }
  if (all.empty()) std::printf("  (none found)\n");
  if (usable == 0) {
    trouble = true;
    std::printf("\n  PROBLEM: no network here can carry a broadcast, so nobody will see a\n"
                "  game hosted on this machine in their list. Joining by typed address may\n"
                "  still work.\n");
  } else {
    std::printf("\n  The other machine's address should be on one of the [use] lines'\n"
                "  networks. If it is not, they are not on the same network and only a\n"
                "  typed address will do.\n");
  }

  // ---- ports ---------------------------------------------------------------
  heading("Ports");
  std::string why;
  if (canBind(kDiscoveryPort, why)) {
    std::printf("  [ ok ] discovery  UDP %u\n", static_cast<unsigned>(kDiscoveryPort));
  } else {
    trouble = true;
    std::printf("  [FAIL] discovery  UDP %u   %s\n", static_cast<unsigned>(kDiscoveryPort),
                why.c_str());
    std::printf("         Games will not be listed. Close whatever is holding it.\n");
  }
  why.clear();
  if (canBind(gamePort, why)) {
    std::printf("  [ ok ] game       UDP %u\n", static_cast<unsigned>(gamePort));
  } else {
    trouble = true;
    std::printf("  [FAIL] game       UDP %u   %s\n", static_cast<unsigned>(gamePort),
                why.c_str());
    std::printf("         Hosting will fail outright on this port.\n");
  }

  // ---- the whole path, end to end ------------------------------------------
  //
  // Advertise and listen at once and see whether the datagrams come back. This
  // covers the beacon, the query, the reply and the parsing together. It cannot
  // prove another machine will hear it — only that nothing local is eating it.
  heading("Talking to itself");
  {
    Advertiser host;
    Listener guest;
    const bool advertising = host.start(gamePort, "Net Doctor", "check");
    const bool listening = guest.start();
    bool heard = false;
    if (advertising && listening) {
      for (int i = 0; i < 150 && !heard; ++i) {
        host.update(1.0 / 60.0);
        guest.update(1.0 / 60.0);
        for (const Beacon& b : guest.found()) {
          if (b.worldName == "Net Doctor") heard = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }
    }
    if (heard) {
      std::printf("  [ ok ] announced a world and heard it come back\n");
    } else {
      trouble = true;
      std::printf("  [FAIL] announced a world and never heard it\n");
      std::printf("         Something on this machine is dropping the datagrams, and a\n"
                  "         firewall is much the most likely thing. See below.\n");
    }
  }

  // ---- what to do ----------------------------------------------------------
  heading("If a world still does not appear");
  std::printf(
      "  1. Run this on BOTH machines and compare the protocol numbers. Different\n"
      "     builds cannot see each other, and say nothing about it.\n"
      "  2. Allow the program path printed at the top through the firewall, for\n"
      "     PRIVATE networks, on the machine that is HOSTING. Windows drops\n"
      "     incoming announcements for a program it has not been asked about while\n"
      "     still allowing the outgoing connection a typed address makes -- which is\n"
      "     exactly why typing the address can work when the list stays empty.\n"
      "  3. Check the network is Private rather than Public. Public blocks far more.\n"
      "  4. A firewall BLOCK rule beats an ALLOW rule, so an old block for this path\n"
      "     silently overrides a new allow. Every copy of the game you have ever run\n"
      "     has its own rules, keyed on its own path; clearing the stale ones out is\n"
      "     safe, and Windows will ask again next time.\n"
      "  5. Failing all of that, the host's Open to LAN panel shows an address that\n"
      "     can be typed into Join directly.\n");

  std::printf("\n%s\n", trouble ? "Something above will stop this machine being found."
                                : "Nothing here would stop this machine being found.");
  return trouble ? 1 : 0;
}

}  // namespace hr::net
