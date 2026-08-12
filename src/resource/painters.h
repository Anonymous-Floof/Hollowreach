// Procedural texture painters, ported from js/render/texatlas.js.
//
// There are no image files. Every block texture is drawn pixel by pixel from a
// seed derived from its own name, so it looks identical every run. The web build
// did this with Canvas2D 1x1 fillRect calls; here it is direct writes into an
// Image, which is the same operation with the browser removed.
//
// This is also the first link in the future resource-pack chain: a painter is
// registered as a *provider* of a texture id, so a pack supplying a PNG for the
// same id simply takes priority in the PackStack. Nothing about the painters needs
// to change for that to work.
//
// Painters are authored at 16x16. The atlas may run at a higher tile resolution
// when a pack asks for it, in which case painter output is scaled up with nearest
// sampling — which is exactly how the original looked anyway.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/prng.h"
#include "resource/identifier.h"
#include "resource/image.h"

namespace hr::resource {

// The resolution painters are written against.
inline constexpr int kPainterTile = 16;

// How many tiles a crop is drawn at, from seedling to harvestable.
//
// It lives here rather than in world/blocks.h because world/ may depend on
// resource/ and not the other way round, and both ends need the number: the
// painters emit `crop_<key>_0` .. `_3`, and the block table declares that many
// stage slots. Four is enough that growth reads as progress and few enough that
// every stage can look meaningfully different from its neighbours.
inline constexpr int kCropStages = 4;

// Draws one tile at (ox, oy) into `into`, consuming `rng` in a fixed order.
using PainterFn = std::function<void(Image& into, int ox, int oy, Mulberry32& rng)>;

struct PainterEntry {
  ResourceId id;
  // The bare texture name the web build hashed for the seed ("turf_top", not
  // "block/turf_top"). Keeping it explicit is what makes the ported output
  // identical rather than merely similar.
  std::string seedName;
  PainterFn fn;
};

// Every built-in painter, in registration order.
const std::vector<PainterEntry>& builtinPainters();

// Looks one up by id. Returns nullptr when nothing is registered, which is how
// the atlas decides to emit its magenta "missing" tile.
const PainterEntry* findPainter(const ResourceId& id);

// Paints `entry` into a freshly allocated 16x16 tile, seeded the way the atlas
// does. Used by the atlas builder and by tooling that wants a single tile.
Image paintTile(const PainterEntry& entry);

}  // namespace hr::resource
