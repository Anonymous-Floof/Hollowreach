// What the net layer is allowed to reach, and what it hands back.
//
// The web build passed the whole `Game` object into NetHost and NetClient, which
// meant the net layer could touch anything — settings, saves, other worlds. This
// names the five live objects it actually needs and a handful of callbacks for
// everything else, so "what the host shares" is a list you can read rather than a
// property of what happened to be in scope.

#pragma once

#include <functional>
#include <string>

#include "core/mat4.h"

namespace hr::world {
class World;
}
namespace hr::render {
class Sky;
}
namespace hr::save {
struct WorldSave;
}
namespace hr::game {
class Player;
class Inventory;
class EntityManager;
}  // namespace hr::game

namespace hr::net {

struct GameRefs {
  world::World* world = nullptr;
  game::Player* player = nullptr;
  game::Inventory* inventory = nullptr;
  game::EntityManager* entities = nullptr;
  render::Sky* sky = nullptr;
};

struct SessionHooks {
  // A line of text for the player.
  std::function<void(const std::string&)> notify;
  // Host: the world payload a joining guest is sent. This is the save format, so
  // the host shares exactly what it would write to disk and nothing else.
  std::function<save::WorldSave()> buildSave;
  // Guest: adopt the host's world. Tears down whatever was open and enters the
  // received one.
  std::function<void(const save::WorldSave&)> adoptWorld;
  // Guest: the connection ended. The argument is a reason to show, or empty for a
  // clean goodbye.
  std::function<void(const std::string&)> onDisconnected;
  // Either side: the roster changed, so the interface should redraw it.
  std::function<void()> onRosterChange;
  // A positional one-shot named by the host.
  std::function<void(const std::string& kind, const Vec3& pos)> playSfx;
  // Guest: the host will not let us have that container — it is not there, or
  // somebody else is already in it. A screen showing one is showing a copy the
  // host will never accept a word of, so it has to close rather than let the
  // player spend a minute sorting something that will not be saved.
  std::function<void(int x, int y, int z, const std::string& reason)> onContainerDenied;
  // Guest: the host refused the boat we already climbed into. The argument is the
  // host's entity id for it, so a stale refusal for a boat we have since got out
  // of cannot throw us out of the one we are in now.
  std::function<void(int netId)> onMountDenied;

  // --- chat and commands ------------------------------------------------------
  //
  // Host: a guest sent a line. The transport hands it straight up without looking
  // at it, because everything that decides what happens next — whether it is a
  // command, who may run that command, who sees the answer — belongs to the
  // session and not to the wire. Host has no business owning a command registry.
  std::function<void(const std::string& playerId, const std::string& line)> onChatLine;
  // Guest: a line to show. `kind` matches ui::Chat::Kind.
  std::function<void(std::uint8_t kind, const std::string& from, const std::string& text)>
      onChatShow;
  // Host: what level this player holds, for the Permission message a joining guest
  // is sent. Read-only, and asked rather than cached because a level can change
  // between one guest joining and the next.
  std::function<std::uint8_t(const std::string& playerId, const std::string& name)> levelOf;
  // Host: may this peer connect at all? False refuses the handshake with `reason`,
  // which is the only thing the refused peer ever sees. This is where the ban list
  // and the whitelist are consulted; the Host itself holds neither.
  std::function<bool(const std::string& playerId, const std::string& name, std::string& reason)>
      mayJoin;
  // Guest: somebody's level changed, including possibly our own.
  std::function<void(const std::string& playerId, std::uint8_t level)> onPermission;
  // Guest: an operator changed our health or emptied our bag. Applied without
  // asking, because the host has already decided it may be — see SetStateMsg.
  std::function<void(float health, bool clearInventory)> onSetState;
};

// One entry in the player list, for the interface.
struct RosterEntry {
  std::string playerId;
  std::string name;
  std::uint32_t pingMs = 0;
  bool self = false;
  bool host = false;
};

}  // namespace hr::net
