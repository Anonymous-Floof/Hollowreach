// The texture atlas.
//
// Ported from js/render/texatlas.js's buildAtlas, with the four constraints a
// future Minecraft resource pack imposes designed in from the start. The web build
// hardcoded a 16px tile, packed a ceil(sqrt(n)) grid with no padding, used a
// half-texel UV inset instead, and never generated a mip chain. All four of those
// are fine for procedural 16px art and none of them survive contact with a 128px
// pack, so instead:
//
//  * The tile resolution is chosen from the sources, not hardcoded. Procedural
//    painter output is scaled up with nearest sampling, which is how it looked
//    anyway.
//  * Tiles get a real gutter of edge-clamped bleed, sized for the mip chain.
//  * Mips are generated per tile so filtering never crosses a tile boundary, with
//    alpha-weighted averaging so cutout fringes do not darken.
//  * CPU pixels are retained. This is not optional: item models are built by
//    reading the atlas back (js/render/itemmesh.js:86), and so are the GUI icons.
//
// Mips and anisotropy sit behind a setting, default off, which reproduces the
// original's NEAREST/NEAREST look exactly.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/gl.h"
#include "resource/identifier.h"
#include "resource/image.h"
#include "resource/packstack.h"

namespace hr::resource {

// Where a texture lives in the atlas.
struct TileRef {
  // Normalised rect, inset by half a texel. u0,v0 is the top-left corner.
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  // Pixel rect of the tile interior, excluding the gutter. Item-model extrusion
  // and icon generation read the CPU image through this.
  int x = 0, y = 0, w = 0, h = 0;
  // Animation, for a future .mcmeta sidecar. frames == 1 means static.
  int frames = 1;
  float frameTimeSeconds = 0.0f;
};

struct AtlasSettings {
  // Upper bound on tile resolution, so a 512px pack cannot demand a 32k atlas.
  int maxTileResolution = 128;
  bool mipmaps = false;
  int anisotropy = 0;  // 0 = off
};

class Atlas {
 public:
  ~Atlas();

  // Builds from the ids `wanted`, resolving each through `stack`. Ids the stack
  // does not provide get a magenta checker so the mistake is visible in game.
  bool build(const PackStack& stack, const std::vector<ResourceId>& wanted,
             const AtlasSettings& settings);

  // Uploads (or re-uploads) the CPU image to GL. Separate from build() so the
  // atlas can be built headlessly for tooling.
  void upload();
  void destroy();

  GLuint texture() const { return texture_; }
  int tileResolution() const { return tileRes_; }
  int width() const { return image_.width(); }
  int height() const { return image_.height(); }

  // Retained CPU pixels. Item meshes and icons are generated from these.
  const Image& image() const { return image_; }

  // Unknown ids return the reserved "missing" tile rather than tile zero. The web
  // build fell back to whichever tile happened to be first, which turned a typo
  // into a plausible-looking wrong texture.
  const TileRef& tile(const ResourceId& id) const;
  bool hasTile(const ResourceId& id) const;

  // Writes the atlas to a PNG. This is the M2 verification step: the output is
  // diffed against a capture of the browser's canvas.
  bool writeDebugPng(const std::string& path) const;

 private:
  void applyFilters() const;

  Image image_;
  std::unordered_map<ResourceId, TileRef> tiles_;
  TileRef missing_;
  int tileRes_ = 16;
  int mipLevels_ = 1;
  AtlasSettings settings_;
  GLuint texture_ = 0;
  // Mip chain built on the CPU, level 0 excluded (that is image_).
  std::vector<Image> mips_;
};

// Collects every texture id the block table references, resolving the top/side/
// bottom slots. Item sprite ids join this list when items land.
std::vector<ResourceId> collectBlockTextureIds();

}  // namespace hr::resource
