// Inventory icons, one 32x32 cell per item, packed into a single sheet.
//
// The web build drew each icon into its own <canvas>, called toDataURL() on it and
// handed the result to an <img> (js/game/items.js:168-174). There is no canvas and
// no <img> here, so the same drawing happens into one atlas that the 2D interface
// samples with a UV rect.
//
// Two drawing paths, mirroring the original exactly:
//
//  * Non-block items are their outlined 16x16 sprite blitted at 2x. Integer scale,
//    no filtering — pixel for pixel what the browser produced.
//  * Block items are a true 2:1 isometric projection of their *display shape*, so
//    stairs, slabs and doors are told apart at a glance instead of all drawing as
//    the same cube. Each visible face is an affine map of its atlas tile, darkened
//    per side so the shape reads with a fixed top-left light.
//
// The projection is rasterised on the CPU rather than through a GL ortho pass. That
// is a departure from the plan, and it is the better call: the original set
// `imageSmoothingEnabled = false`, so Canvas2D was already nearest-sampling these
// affine draws, and reproducing that directly keeps the icons essentially identical
// instead of merely similar. It also means icons can be generated and diffed
// headlessly. A future resource pack shipping arbitrary item models would want the
// GL pass after all; ItemModel is the seam where that swap happens.

#pragma once

#include <string>
#include <vector>

#include "core/gl.h"
#include "render/itemmodel.h"
#include "resource/atlas.h"
#include "resource/image.h"

namespace hr::render {

// The web build's canvas size. Two device pixels per sprite texel.
inline constexpr int kIconSize = 32;

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

  bool writeDebugPng(const std::string& path) const;

 private:
  void drawIcon(Image& into, int ox, int oy, const game::ItemDef& item,
                const resource::Atlas& atlas);
  void drawBlockIcon(Image& into, int ox, int oy, const game::ItemDef& item,
                     const resource::Atlas& atlas);

  Image image_;
  int columns_ = 0;
  int count_ = 0;
  GLuint texture_ = 0;
};

}  // namespace hr::render
