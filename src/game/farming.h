// What using an item on a block means to a farmer.
//
// This is a PURE FUNCTION on purpose. The decision it makes — till this, sow that,
// or do nothing — used to live inside a lambda in App, where the only way to reach
// it was to run the whole game: a window, a renderer, an audio device. So it went
// untested, which for the one function that decides whether farming works at all is
// the wrong thing to leave uncovered.
//
// Split this way, App keeps the part that only App can do (write to the world, play
// a sound, put a message on screen) and everything that can be got wrong is a
// function taking three values and returning one.

#pragma once

#include <cstdint>

#include "game/items.h"
#include "world/blocks.h"

namespace hr::game {

enum class FarmAction : std::uint8_t {
  None,
  Till,        // turn the target into farmland; the tool is not consumed
  Sow,         // plant `crop` in the cell above; one seed is consumed
  NeedsTilling  // they are holding a seed over bare soil: worth saying so
};

struct FarmPlan {
  FarmAction action = FarmAction::None;
  world::BlockId crop = 0;  // Sow only
};

// `target` is the block being pointed at; `clearAbove` is whether the cell above it
// is air. Air specifically, not "not solid": a torch or a tuft of grass standing on
// the soil would otherwise be destroyed by a click meant for the ground.
//
// `wildSeedPick` chooses which common crop a handful of wild seed comes up as. It is
// passed in rather than rolled here so this function stays pure and a test can pin
// the outcome.
FarmPlan planFarmUse(const ItemDef& item, world::BlockId target, bool clearAbove,
                     int wildSeedPick);

// The common crops wild seed can produce, and how many there are.
const char* const* wildSeedCrops(int& count);

}  // namespace hr::game
