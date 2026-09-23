// --showroom: every model the game draws, laid out on one platform so a single
// screenshot can show all of them.
//
// Block tiles and inventory icons can be dumped as flat sheets (--dump-atlas,
// --dump-icons), but that is not where the art goes wrong. A texture stretched over
// a sub-box, a gap between two faces that should meet, a dropped sword hovering off
// its shadow — those only exist once the pieces are assembled in the world and lit,
// and until this there was no reproducible way to put them in front of a camera.

#pragma once

#include <string>

namespace hr::world {
class World;
}
namespace hr::game {
class EntityManager;
}

namespace hr::dev {

// "blocks": every registered block in a grid, one cell apart, each standing on the
//           floor it needs (crops on farmland, plants on turf), plus a row of dyed
//           samples.
// "items":  one dropped stack of every item, held still in a grid.
//
// The floor is laid at `floorY` and the grid runs toward -z from `z`, centred on
// `x`, so `--at x,floorY+1,z,0,-0.3` looks straight down it. Returns false for an
// unknown mode, having built nothing.
bool buildShowroom(world::World& world, game::EntityManager& entities, const std::string& mode,
                   int x, int floorY, int z);

}  // namespace hr::dev
