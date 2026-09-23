// Inventory icons, one 64x64 cell per item, packed into a single sheet.
//
// The web build drew each icon into its own <canvas>, called toDataURL() on it and
// handed the result to an <img> (js/game/items.js:168-174). There is no canvas and
// no <img> here, so the same drawing happens into one atlas that the 2D interface
// samples with a UV rect.
//
// Drawing paths:
//
//  * Non-block items are their outlined 16x16 sprite at exactly 4x.
//  * Cross-rendered blocks (plants, torches) are their tile at the same 4x, outlined
//    the same way, so a flower sits in the bag as the same kind of object as an
//    ingot. They used to be scaled to 20x26, which is not a whole multiple of 16 in
//    either direction and stretched them tall with uneven pixel rows.
//  * Ladders and paintings are drawn the same flat way: in projection they were a
//    thin slab seen nearly edge-on.
//  * Doors are the whole door, upper tile over lower, at 2x.
//  * Other block items are a true 2:1 isometric projection of their *display shape*,
//    so stairs, slabs and beds are told apart at a glance instead of all drawing as
//    the same cube. Each visible face is an affine map of its atlas tile, darkened
//    per side so the shape reads with a fixed top-left light.
//
// The cells were 32px until the interface began drawing slots at whatever size the
// scale setting asks for. A 32px icon in a 40px slot is a 1.25x nearest-neighbour
// stretch, which doubles every fourth pixel row and column — outlines came out one
// and two pixels wide on the same icon. At 64px with mipmaps and linear
// minification, any slot from half size up draws evenly, and at 2x the sheet is
// sampled one to one.
//
// The projection is rasterised on the CPU rather than through a GL ortho pass. The
// original set `imageSmoothingEnabled = false`, so Canvas2D was already nearest-
// sampling these affine draws, and reproducing that directly keeps icons faithful
// and lets them be generated and diffed headlessly. A future resource pack shipping
// arbitrary item models would want the GL pass after all; ItemModel is the seam
// where that swap happens.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/gl.h"
#include "render/itemmodel.h"
#include "resource/atlas.h"
#include "resource/image.h"

namespace hr::render {

inline constexpr int kIconSize = 64;

class IconAtlas {
 public:
  ~IconAtlas();

  // Draws every registered item. `atlas` supplies the block tiles and the item
  // sprite tiles, so it must already be built.
  bool build(const resource::Atlas& atlas);

  void upload();
  void destroy();

  GLuint texture() const { return texture_; }
  const Image& image() const { return image_; }
  int width() const { return image_.width(); }
  int height() const { return image_.height(); }

  // Normalised rect for an item, by its registry index. Returns false for an index
  // with no cell, leaving the outputs untouched.
  bool uvFor(int itemIndex, float& u0, float& v0, float& u1, float& v1) const;
  bool uvFor(const std::string& key, float& u0, float& v0, float& u1, float& v1) const;

  // The parts of an icon a dye does not reach, alone in a cell of their own, for
  // drawing untinted over the tinted icon. Only items that are partly dyeable have
  // one — the bed, whose frame and pillow stay their colour — and for those, the
  // interface multiplies the dye over the whole icon and then lays this on top, so
  // the colour lands on the mattress and nowhere else. False for everything else.
  bool overlayFor(const std::string& key, float& u0, float& v0, float& u1, float& v1) const;

  bool writeDebugPng(const std::string& path) const;

 private:
  // `undyed`, when given, is filled with the pixels whose visible surface is a part
  // the dye does not reach; see overlayFor.
  void drawIcon(Image& into, int ox, int oy, const game::ItemDef& item,
                const resource::Atlas& atlas, Image* undyed);
  void drawBlockIcon(Image& into, int ox, int oy, const game::ItemDef& item,
                     const resource::Atlas& atlas, Image* undyed);
  bool cellRect(int cell, float& u0, float& v0, float& u1, float& v1) const;

  Image image_;
  int columns_ = 0;
  int count_ = 0;  // items; overlay cells follow them
  std::unordered_map<int, int> overlayCell_;  // item index -> cell
  GLuint texture_ = 0;
};

// Gives every fully transparent pixel the average colour of its opaque neighbours,
// leaving its alpha at zero. Linear filtering and mipmaps average across the edge of
// an icon, and without this they average in the black RGB of the empty space around
// it: every icon would grow a dark fringe as it shrank. Exposed for the self-test.
void bleedTransparent(Image& img, int passes);

}  // namespace hr::render
