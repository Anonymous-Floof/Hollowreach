// One description per item, shared by every rendition of it.
//
// The web build smeared an item's appearance across four files: extrusion in
// render/itemmesh.js, the held transform in render/viewmodel.js, the dropped
// rendering in render/entityrenderer.js, and the GUI icon in game/items.js. Adding
// a way to describe an item therefore meant editing four places, and a resource
// pack could not have described one at all.
//
// Minecraft models items too — `item/generated` with layer0..4, or `item/handheld`,
// plus a `display` block holding a transform per context (gui, ground, fixed,
// firstperson_righthand, thirdperson_righthand, head). This is that shape, reduced
// to what the game actually uses today: which geometry an item has, and how it is
// posed in the hand. The icon path and the mesh path both read it, so they cannot
// disagree, and a pack loader later populates the same struct from JSON.

#pragma once

#include <cstdint>

#include "core/mat4.h"
#include "game/items.h"
#include "resource/identifier.h"
#include "world/blocks.h"

namespace hr::render {

// How the first-person hand holds an item — Minecraft's
// `display.firstperson_righthand`, expressed in the framing the web build used.
//
// The framing is resolution-independent on purpose: `anchor` is a fraction of the
// half-frame (±1 = screen edge) and `size` a fraction of the frame HEIGHT, both
// measured at the hold distance. So the item sits in the same spot of the picture
// on an ultrawide monitor, a 4:3 window or a phone — fixed view-space offsets slid
// off the edge as the aspect ratio narrowed.
struct HoldStyle {
  // Radians, applied roll(z) -> pitch(x) -> yaw(y) about the grip. +z rolls the
  // item's top to the LEFT, -x tips its top AWAY into the scene, -y turns its face
  // back toward the camera.
  Vec3 rot {0, 0, 0};
  // Where the grip lands: [0,0] is the crosshair, [1,-1] the bottom-right corner.
  float anchorX = 0.66f;
  float anchorY = -0.98f;
  // Model height as a fraction of the frame height. Meshes are unit-scaled — one
  // block, or one full 16px sprite — so a slab really does read as half a block.
  float size = 0.36f;
  // How far in front of the camera the item hangs.
  float dist = 0.62f;
  // The model-space point that lands on the anchor. Item meshes are built
  // bottom-centred, so {0,0,0} is the butt of a tool's handle.
  Vec3 grip {0, 0, 0};
  // Mirror the model's x. Sword art is drawn hilt-low-left, blade-high-right (the
  // icon convention); mirrored, the hilt falls into the hand and the blade reaches
  // for the crosshair.
  bool flip = false;
};

// The named styles, so they can be listed by a future tuning overlay.
namespace holdStyles {
const HoldStyle& block();
const HoldStyle& tool();
const HoldStyle& sword();
const HoldStyle& food();
const HoldStyle& panel();
const HoldStyle& item();
}  // namespace holdStyles

enum class ItemModelKind : std::uint8_t {
  None = 0,
  // Block items with solid geometry render as their actual display shape
  // (world::displayBoxes: stairs are stairs, doors are doors), textured from the
  // block's atlas tiles with the mesher's face shades, so a dropped slab matches
  // a placed slab.
  Shape,
  // Everything else renders as its 16x16 sprite extruded one texel thick. Non-block
  // items use their `item/<key>` tile; cross-render blocks (torches, flowers) use
  // their block tile, since that art already *is* their sprite.
  Sprite,
};

struct ItemModel {
  ItemModelKind kind = ItemModelKind::None;

  world::BlockId blockId = 0;                              // Shape
  world::RenderKind shape = world::RenderKind::None;       // Shape: which display shape
  ResourceId texture;                                      // Sprite: the tile to extrude

  HoldStyle hold;

  explicit operator bool() const { return kind != ItemModelKind::None; }
};

// Resolves an item to its model. Unknown items return an empty model rather than
// a plausible-looking wrong one.
ItemModel itemModelFor(const game::ItemDef& item);
ItemModel itemModelFor(std::string_view key);

}  // namespace hr::render
