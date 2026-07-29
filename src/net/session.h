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
