#include "render/itemmodel.h"

namespace hr::render {
namespace holdStyles {

// Every style is complete, with no inheritance to trace. Ported from
// js/render/viewmodel.js:39-79.

const HoldStyle& block() {
  // A block held out at the classic three-quarter angle: you can see its top and
  // two sides at once, so stairs read as stairs and slabs as slabs.
  static const HoldStyle s {{0.30f, -0.66f, 0}, 0.66f, -0.95f, 0.34f, 0.62f, {0, 0, 0}, false};
  return s;
}

const HoldStyle& tool() {
  // Pick, axe, shovel: handle butt in the hand at the bottom right, rolled so the
  // head leans up toward the crosshair, tipped away so it reaches into the scene,
  // and turned a few degrees off face-on so the extruded edge shows and the sprite
  // reads as an object rather than a sticker.
  static const HoldStyle s {{-0.28f, -0.46f, 0.52f}, 0.62f, -1.02f, 0.56f, 0.62f, {0, 0, 0},
                            false};
  return s;
}

const HoldStyle& sword() {
  // A sword stands closer to upright than a tool. Its art is drawn on the diagonal,
  // so the roll here is NEGATIVE — it cancels most of the 45 degrees the sprite
  // already has and lifts the blade up the screen.
  static const HoldStyle s {{-0.22f, -0.46f, -0.48f}, 0.62f, -1.06f, 0.58f, 0.62f,
                            {0.30f, 0.06f, 0}, true};
  return s;
}

const HoldStyle& food() {
  // Food is brought up nearer the middle and turned to face you — you are about to
  // eat it, so it should be readable.
  static const HoldStyle s {{-0.10f, -0.34f, 0.22f}, 0.58f, -0.92f, 0.40f, 0.62f, {0, 0, 0},
                            false};
  return s;
}

const HoldStyle& panel() {
  // Doors, trapdoors and ladders are flat panels: held at a cube's angle they are a
  // meaningless sliver, so they get turned to show their face.
  static const HoldStyle s {{0.10f, -1.15f, 0.18f}, 0.66f, -0.92f, 0.44f, 0.62f, {0, 0, 0},
                            false};
  return s;
}

const HoldStyle& item() {
  // Everything else: ingots, gems, buckets, torches, the atlas. Mostly face-on with
  // a hint of turn so the extrusion catches the light.
  static const HoldStyle s {{-0.10f, -0.42f, 0.20f}, 0.66f, -0.98f, 0.36f, 0.62f, {0, 0, 0},
                            false};
  return s;
}

}  // namespace holdStyles

namespace {

// Which style an item is held in. An item can name one explicitly with `hold` in
// the registry; otherwise it falls out of its geometry and type, so a new item
// inherits a sensible pose for free.
const HoldStyle& styleFor(const game::ItemDef& item, ItemModelKind kind,
                          world::RenderKind shape) {
  if (item.hold == "block") return holdStyles::block();
  if (item.hold == "tool") return holdStyles::tool();
  if (item.hold == "sword") return holdStyles::sword();
  if (item.hold == "food") return holdStyles::food();
  if (item.hold == "panel") return holdStyles::panel();
  if (item.hold == "item") return holdStyles::item();

  if (kind == ItemModelKind::Shape) {
    // Keyed on the render shape rather than the block, so every wood's door gets
    // the panel pose for free.
    switch (shape) {
      case world::RenderKind::Door:
      case world::RenderKind::Trapdoor:
      case world::RenderKind::Ladder: return holdStyles::panel();
      default: return holdStyles::block();
    }
  }

  if (item.toolType == game::ToolKind::Sword) return holdStyles::sword();
  if (item.type == game::ItemType::Tool) return holdStyles::tool();
  if (item.type == game::ItemType::Food) return holdStyles::food();
  return holdStyles::item();
}

}  // namespace

ItemModel itemModelFor(const game::ItemDef& item) {
  ItemModel model;

  if (item.type == game::ItemType::Block) {
    const world::BlockDef& b = world::blocks().def(item.blockId);
    if (b.render == world::RenderKind::Cross) {
      // A cross-rendered block's own tile is already a 16x16 sprite, so extruding
      // it gives a torch or a flower the same treatment as any item.
      model.kind = ItemModelKind::Sprite;
      model.texture = b.faceTextures[0];
    } else {
      model.kind = ItemModelKind::Shape;
      model.blockId = item.blockId;
      model.shape = b.render;
    }
  } else {
    model.kind = ItemModelKind::Sprite;
    model.texture = ResourceId("item/" + item.key);
  }

  model.hold = styleFor(item, model.kind, model.shape);
  return model;
}

ItemModel itemModelFor(std::string_view key) {
  const game::ItemDef* item = game::getItem(key);
  return item ? itemModelFor(*item) : ItemModel {};
}

}  // namespace hr::render
