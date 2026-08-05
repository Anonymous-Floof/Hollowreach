#include "net/transport.h"

#include <algorithm>
#include <cstring>

#include <enet/enet.h>

#include "core/log.h"

namespace hr::net {
namespace {

int g_enetRefs = 0;
const std::string kNoAddress;

std::string addressText(const ENetAddress& address) {
  char buffer[64] = {};
  if (enet_address_get_host_ip(&address, buffer, sizeof buffer) != 0) return "?";
  return std::string(buffer) + ":" + std::to_string(address.port);
}

}  // namespace

bool enetAcquire() {
  if (g_enetRefs > 0) {
    ++g_enetRefs;
    return true;
  }
  if (enet_initialize() != 0) {
    log::error("net: ENet failed to initialise");
    return false;
  }
  g_enetRefs = 1;
  return true;
}

void enetRelease() {
  if (g_enetRefs <= 0) return;
  if (--g_enetRefs == 0) enet_deinitialize();
}

Transport::~Transport() { close(); }

bool Transport::listen(std::uint16_t port, int maxPeers, std::string* error) {
  close();
  if (!enetAcquire()) {
    if (error) *error = "networking is unavailable on this machine";
    return false;
  }

  ENetAddress address;
  address.host = ENET_HOST_ANY;
  address.port = port;
  ENetHost* host = enet_host_create(&address, static_cast<std::size_t>(std::max(1, maxPeers)),
                                    static_cast<std::size_t>(Channel::Count), 0, 0);
  if (!host) {
    enetRelease();
    if (error) *error = "could not bind port " + std::to_string(port);
    return false;
  }
  host_ = host;
  server_ = true;
  loggedServiceError_ = false;
  port_ = host->address.port;
  log::info("net: hosting on port %u", static_cast<unsigned>(port_));
  return true;
}

bool Transport::connect(const std::string& address, std::uint16_t port, std::string* error) {
  close();
  if (!enetAcquire()) {
    if (error) *error = "networking is unavailable on this machine";
    return false;
  }

  // One outgoing connection, so one peer slot.
  ENetHost* host = enet_host_create(nullptr, 1, static_cast<std::size_t>(Channel::Count), 0, 0);
  if (!host) {
    enetRelease();
    if (error) *error = "could not open a socket";
    return false;
  }

  ENetAddress target;
  target.port = port;
  if (enet_address_set_host(&target, address.c_str()) != 0) {
    enet_host_destroy(host);
    enetRelease();
    if (error) *error = "could not resolve \"" + address + "\"";
    return false;
  }

  ENetPeer* peer = enet_host_connect(host, &target, static_cast<std::size_t>(Channel::Count), 0);
  if (!peer) {
    enet_host_destroy(host);
    enetRelease();
    if (error) *error = "no free connection slot";
    return false;
  }

  host_ = host;
  server_ = false;
  loggedServiceError_ = false;
  port_ = 0;
  // The slot exists before the handshake completes so the caller can address it;
  // a Connected event still has to arrive before anything is sent.
  PeerSlot slot;
  slot.id = nextPeerId_++;
  slot.peer = peer;
  slot.address = addressText(target);
  peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot.id));
  peers_.push_back(std::move(slot));
  log::info("net: connecting to %s:%u", address.c_str(), static_cast<unsigned>(port));
  return true;
}

void Transport::close() {
  if (!host_) return;
  auto* host = static_cast<ENetHost*>(host_);
  // `enet_peer_disconnect_now` throws away everything still queued for that peer
  // AND makes the other end tear the connection down before it has acknowledged
  // what was already in flight — so closing straight into it loses the last thing
  // the caller sent. That is exactly the message that matters here: a guest's
  // parting gift is its final inventory, and the host's is a goodbye.
  //
  // `disconnect_later` instead waits for the outgoing queue to drain, and the
  // short service loop below is what gives it the chance. Forty milliseconds is
  // the worst case, once, on leaving a world — which is what a polite goodbye
  // costs and is not a price paid in any frame anybody is looking at.
  for (PeerSlot& slot : peers_) {
    if (slot.peer) enet_peer_disconnect_later(static_cast<ENetPeer*>(slot.peer), 0);
  }
  for (int i = 0; i < 20; ++i) {
    ENetEvent event;
    if (enet_host_service(host, &event, 2) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
      enet_packet_destroy(event.packet);
    }
  }
  enet_host_flush(host);
  enet_host_destroy(host);
  host_ = nullptr;
  server_ = false;
  port_ = 0;
  peers_.clear();
  enetRelease();
}

Transport::PeerSlot* Transport::slotFor(PeerId id) {
  for (PeerSlot& slot : peers_) {
    if (slot.id == id) return &slot;
  }
  return nullptr;
}

const Transport::PeerSlot* Transport::slotFor(PeerId id) const {
  for (const PeerSlot& slot : peers_) {
    if (slot.id == id) return &slot;
  }
  return nullptr;
}

Transport::PeerSlot* Transport::slotForPeer(void* peer) {
  for (PeerSlot& slot : peers_) {
    if (slot.peer == peer) return &slot;
  }
  return nullptr;
}

void Transport::poll(std::vector<TransportEvent>& out, std::uint32_t timeoutMs) {
  out.clear();
  if (!host_) return;
  auto* host = static_cast<ENetHost*>(host_);

  ENetEvent event;
  int first = enet_host_service(host, &event, timeoutMs);
  if (first < 0) {
    // A negative return is a socket-level failure, and ENet gives no detail. It
    // is logged once rather than every frame, because the frame after it will
    // fail the same way and sixty lines a second helps nobody.
    if (!loggedServiceError_) {
      loggedServiceError_ = true;
#if defined(_WIN32)
      log::error("net: the socket stopped responding (winsock %d)", WSAGetLastError());
#else
      log::error("net: the socket stopped responding");
#endif
    }
    return;
  }
  while (first > 0) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT: {
        PeerSlot* existing = slotForPeer(event.peer);
        if (!existing) {
          PeerSlot slot;
          slot.id = nextPeerId_++;
          slot.peer = event.peer;
          slot.address = addressText(event.peer->address);
          event.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot.id));
          peers_.push_back(std::move(slot));
          existing = &peers_.back();
        }
        TransportEvent e;
        e.kind = TransportEvent::Kind::Connected;
        e.peer = existing->id;
        out.push_back(std::move(e));
        break;
      }
      // ENet 1.3 has one disconnect event; a timeout arrives as the same thing.
      case ENET_EVENT_TYPE_DISCONNECT: {
        PeerSlot* slot = slotForPeer(event.peer);
        if (slot) {
          TransportEvent e;
          e.kind = TransportEvent::Kind::Disconnected;
          e.peer = slot->id;
          out.push_back(std::move(e));
          // The id is retired with the slot. Nothing may address it again, which
          // is the whole point of not handing ENetPeer* to the game layer: ENet
          // reuses that pointer for the next peer into the same slot.
          peers_.erase(peers_.begin() + (slot - peers_.data()));
        }
        event.peer->data = nullptr;
        break;
      }
      case ENET_EVENT_TYPE_RECEIVE: {
        PeerSlot* slot = slotForPeer(event.peer);
        if (slot && event.packet && event.packet->dataLength > 0) {
          TransportEvent e;
          e.kind = TransportEvent::Kind::Message;
          e.peer = slot->id;
          e.data.assign(event.packet->data, event.packet->data + event.packet->dataLength);
          out.push_back(std::move(e));
        }
        if (event.packet) enet_packet_destroy(event.packet);
        break;
      }
      default: break;
    }
    first = enet_host_service(host, &event, 0);
  }
}

void Transport::send(PeerId peer, Channel channel, const std::vector<std::uint8_t>& bytes) {
  if (!host_ || bytes.empty()) return;
  PeerSlot* slot = slotFor(peer);
  if (!slot || !slot->peer) return;
  // UNRELIABLE_FRAGMENT is not an alternative to UNSEQUENCED, it is the answer to
  // a different question: what to do when the packet does not fit in one datagram.
  //
  // enet_peer_send (peer.c:136) decides that with
  //
  //     (flags & (RELIABLE | UNRELIABLE_FRAGMENT)) == UNRELIABLE_FRAGMENT
  //
  // and note which flag is NOT in that test. UNSEQUENCED alone fails it, so an
  // oversized "unsequenced" packet falls into the else branch and is sent as
  // SEND_FRAGMENT | FLAG_ACKNOWLEDGE — reliable, ordered, acknowledged and
  // retransmitted, on the channel chosen precisely because it must be none of
  // those things. Nothing reports this; the call returns 0 like any other.
  //
  // The threshold is mtu - sizeof(ENetProtocolHeader) - sizeof(ENetProtocolSendFragment),
  // which is 1364 bytes on the default 1392 MTU. A snapshot entity costs about 33,
  // so a world with roughly forty things in it crosses it and stays across it. From
  // there the fast channel carried twenty multi-fragment reliable packets a second:
  // reliableDataInTransit climbs into the throttled window, the reliable send stalls
  // (protocol.c:1472), and because the next snapshot is already queued behind it the
  // backlog only grows. It does not recover. Channel 0 has its own window, which is
  // why the world kept being edited while everything alive in it stopped — mobs and
  // the other players froze together, because both of them travel in the snapshot.
  //
  // The flag is the fix for the channel; keeping a snapshot inside one datagram is
  // the fix for the game, and Host::sendSnapshot now does that. Both, because the
  // budget bounds the common case and this bounds the day something exceeds it.
  const enet_uint32 flags = channel == Channel::Reliable
                                ? ENET_PACKET_FLAG_RELIABLE
                                : (ENET_PACKET_FLAG_UNSEQUENCED |
                                   ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
  ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
  if (!packet) return;
  if (enet_peer_send(static_cast<ENetPeer*>(slot->peer), static_cast<enet_uint8>(channel),
                     packet) != 0) {
    enet_packet_destroy(packet);
  }
}

void Transport::broadcast(Channel channel, const std::vector<std::uint8_t>& bytes) {
  broadcastExcept(kNoPeer, channel, bytes);
}

void Transport::broadcastExcept(PeerId except, Channel channel,
                                const std::vector<std::uint8_t>& bytes) {
  if (!host_ || bytes.empty()) return;
  for (PeerSlot& slot : peers_) {
    if (slot.id == except) continue;
    send(slot.id, channel, bytes);
  }
}

void Transport::disconnect(PeerId peer) {
  PeerSlot* slot = slotFor(peer);
  if (!slot || !slot->peer) return;
  enet_peer_disconnect(static_cast<ENetPeer*>(slot->peer), 0);
}

std::vector<PeerId> Transport::peers() const {
  std::vector<PeerId> out;
  out.reserve(peers_.size());
  for (const PeerSlot& slot : peers_) out.push_back(slot.id);
  return out;
}

std::uint32_t Transport::roundTripMs(PeerId peer) const {
  const PeerSlot* slot = slotFor(peer);
  if (!slot || !slot->peer) return 0;
  return static_cast<ENetPeer*>(slot->peer)->roundTripTime;
}

const std::string& Transport::addressOf(PeerId peer) const {
  const PeerSlot* slot = slotFor(peer);
  return slot ? slot->address : kNoAddress;
}

}  // namespace hr::net
