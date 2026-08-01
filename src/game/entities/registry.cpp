// The entity type table, ported from js/game/entities/registry.js.
//
// The same data-driven pattern as BLOCKS / ITEMS / RECIPES: a definition describes
// a kind, an instance carries only state. To add a mob, projectile or vehicle,
// write a definition in its own file and list it here.

#include <array>
#include <string_view>

#include "game/entities/entity.h"
#include "game/entities/types.h"

namespace hr::game {
namespace {

// Indexed by EntityType, so `defOf` is an array lookup rather than a search.
const std::array<const EntityDef*, static_cast<std::size_t>(EntityType::Count)> kDefs = {
    nullptr,  // None
    &kDropDef,   &kBoatDef,          &kSheepDef,          &kPigDef,
    &kCowDef,    &kZombieDef,        &kRemotePlayerDef,   &kFallingBlockDef,
};

}  // namespace

const EntityDef* defOf(EntityType type) {
  const auto index = static_cast<std::size_t>(type);
  return index < kDefs.size() ? kDefs[index] : nullptr;
}

const char* entityTypeKey(EntityType type) {
  const EntityDef* def = defOf(type);
  return def ? def->key : "";
}

EntityType entityTypeFromKey(std::string_view key) {
  for (std::size_t i = 1; i < kDefs.size(); ++i) {
    if (kDefs[i] && key == kDefs[i]->key) return static_cast<EntityType>(i);
  }
  return EntityType::None;
}

}  // namespace hr::game
