// The entity definitions, one per kind. Split out of registry.cpp so each mob's
// numbers and behaviour sit in their own file, mirroring js/game/entities/.

#pragma once

#include "game/entities/entity.h"

namespace hr::game {

extern const EntityDef kDropDef;
extern const EntityDef kBoatDef;
extern const EntityDef kSheepDef;
extern const EntityDef kPigDef;
extern const EntityDef kCowDef;
extern const EntityDef kZombieDef;
extern const EntityDef kRemotePlayerDef;

// The boat's seat: how far above the hull origin the rider's feet sit.
inline constexpr float kBoatSeatY = 0.25f;

}  // namespace hr::game
