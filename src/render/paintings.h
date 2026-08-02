// Pictures hung on walls, drawn as their own pass.
//
// Everything else in the world is textured from the one atlas, which is what lets
// a chunk be a single buffer and a single draw. A painting cannot be: its face is
// a photograph the player chose, unique per position, and there is no room for a
// hundred of those in a tile sheet. So the mesher emits only the frame — an
// ordinary shaped block — and the picture inside it is drawn here, one quad and
// one texture per painting.
//
// It writes the same three G-buffer targets as terrain, so a painting is lit by
// exactly the same composite as the wall it hangs on: it dims indoors, catches the
// sun through a window, and takes shadow like anything else.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/camera.h"
#include "core/gl.h"
#include "core/mat4.h"
#include "core/shader.h"
#include "game/blockentities.h"

namespace hr {
namespace world {
class World;
}
namespace render {

class PaintingRenderer {
 public:
  ~PaintingRenderer();

  bool init(ShaderCache& shaders);
  void shutdown();

  // Frustum-culled, then one draw per visible painting. Called inside the opaque
  // G-buffer pass, with its state already set.
  void drawGBuffer(const world::World& world, const Camera& camera);

  int drawn() const { return drawn_; }

 private:
  // One uploaded texture per painting position. Rebuilt wholesale when the world's
  // painting revision moves, which is cheap because it only moves when somebody
  // hangs, changes or breaks one — never per frame.
  struct Entry {
    GLuint tex = 0;
  };

  void syncTextures(const world::World& world);

  Program* prog_ = nullptr;
  GLuint vao_ = 0;
  GLuint vbo_ = 0;
  std::unordered_map<game::BlockEntityKey, Entry> textures_;
  std::uint32_t seenRevision_ = 0;
  bool haveRevision_ = false;
  std::vector<float> verts_;
  int drawn_ = 0;
};

}  // namespace render
}  // namespace hr
