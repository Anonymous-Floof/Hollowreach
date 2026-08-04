// The machine's own IPv4 interfaces, and the broadcast address of each subnet.
//
// This exists because of one specific failure, which is the most common way LAN
// discovery quietly does not work. A datagram sent to 255.255.255.255 — the
// "limited broadcast" — from a socket that is not bound to an interface goes out
// exactly ONE of them, chosen by the routing table. A developer machine typically
// has several: a real NIC, a Hyper-V or WSL virtual switch, a VPN, sometimes
// VirtualBox or Docker as well. When the stack picks a virtual one, the beacon is
// broadcast perfectly into a network with nobody on it, and there is no error to
// notice: send() succeeds.
//
// What that looks like from the outside is exactly what was reported here —
// hosting from one machine works and hosting from the other does not, worlds
// sometimes appear and sometimes do not, and typing the address in by hand works
// when the list is empty. It is not intermittent; it is a property of which
// adapters each machine happens to have.
//
// The fix is to stop asking the routing table to guess. A DIRECTED broadcast
// (192.168.10.255 rather than 255.255.255.255) names the subnet it belongs to, so
// the route for it is the directly-connected one and it leaves by the interface
// that actually owns that network. Send one per interface and every network the
// machine is on gets a beacon.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hr::net {

struct Interface {
  std::string name;
  // Both in network byte order, ready to assign to ENetAddress::host.
  std::uint32_t address = 0;
  std::uint32_t broadcast = 0;  // zero when there is no subnet to broadcast into
  std::string addressText;      // dotted quad, for logging
  std::string broadcastText;
  int prefix = 0;
  // Empty when this interface is used. Otherwise why it is not — which is worth
  // carrying rather than dropping, because "my VPN is not in the list" is the
  // first thing anybody asks when discovery is not working, and the answer is
  // usually that it correctly is not.
  std::string skipped;
};

// Every IPv4 interface the machine has, including the ones a beacon will not be
// sent on, each carrying the reason. For --net-doctor.
std::vector<Interface> allInterfaces();

// Just the usable ones: up, not loopback, and with a subnet worth broadcasting
// to. Point-to-point interfaces with a /31 or /32 are skipped — their
// "broadcast" address is the interface itself or the peer, so a datagram to it
// reaches nobody a beacon is meant for. An empty list is not an error; it means
// fall back to the limited broadcast.
std::vector<Interface> localInterfaces();

}  // namespace hr::net
