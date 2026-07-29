// 3D meshes for items, ported from js/render/itemmesh.js.
//
// Two kinds, chosen by the ItemModel: a block's display shape, or its 16x16 sprite
// extruded one texel thick (front and back plates plus a little wall along every
// pixel edge). Meshes are built in unit space — x and z centred on the origin, y up
// from 0 — and cached; consumers place them with a model matrix.
//
// The vertex layout is the one the plan specifies for entities as well (enabler 7).
// The web build had no UV channel on mobs at all: js/render/entityrenderer.js:62
// smuggled colour R,G through the UV slot, the bone index through the skylight slot
// and colour B through the blocklight slot, while itemmesh.js used the same stride
// *with* real UVs. Since a resource pack does replace entity/*.png, the layout is
// declared explicitly here and shared, rather than reusing slots for other meanings.

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/gl.h"
#include "render/itemmodel.h"
#include "resource/atlas.h"

namespace hr::render {

// 24 bytes:
//   0  aPos    3 x float          unit model space
//   1  aUV     2 x ushort norm    atlas coordinates
//   2  aShade  1 x ubyte norm     baked face shade
//   3  aBone   1 x ubyte          skeletal bone index; 0 for items
//      pad     2 bytes            keeps the colour 4-byte aligned
//   4  aColor  4 x ubyte norm     per-vertex tint; white for items
struct ItemVertex {
  float x = 0, y = 0, z = 0;
  std::uint16_t u = 0, v = 0;
  std::uint8_t shade = 255;
  std::uint8_t bone = 0;
  std::uint16_t pad = 0;
  std::uint8_t r = 255, g = 255, b = 255, a = 255;
};
static_assert(sizeof(ItemVertex) == 24, "item vertex must stay tightly packed");

// Binds the layout above on the currently bound VAO and VBO.
void bindItemAttributes();

// Builds the mesh on the CPU. Headless, so tooling and the icon path can use it
// without a GL context. Returns empty for a model with no geometry — a fully
// transparent sprite tile, say.
std::vector<ItemVertex> buildItemMesh(const ItemModel& model, const resource::Atlas& atlas);

// One uploaded mesh.
struct ItemMesh {
  GLuint vao = 0;
  GLuint vbo = 0;
  GLsizei count = 0;
  ItemModelKind kind = ItemModelKind::None;
  // The block's render shape for a Shape mesh, so the viewmodel can hold a door
  // differently from a cube without consulting the block table.
  world::RenderKind shape = world::RenderKind::None;

  bool valid() const { return count > 0; }
};

// Lazily builds and owns one mesh per item key.
class ItemMeshCache {
 public:
  ~ItemMeshCache() { clear(); }

  void setAtlas(const resource::Atlas* atlas) { atlas_ = atlas; }

  // Null for an unknown item or one whose art is entirely transparent.
  const ItemMesh* get(const std::string& key);

  void clear();

 private:
  const resource::Atlas* atlas_ = nullptr;
  // Holds an entry even for a failed build, so a bad key is not retried per frame.
  std::unordered_map<std::string, ItemMesh> meshes_;
};

}  // namespace hr::render
