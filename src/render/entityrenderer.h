// Draws entities with a per-instance model matrix, ported from
// js/render/entityrenderer.js.
//
// Dropped items reuse the shared item-model cache — the very meshes the held
// viewmodel draws — so a dropped stair is stair-shaped and a dropped sword is its
// extruded sprite. Mobs and boats are multi-box meshes built here: a list of
// coloured boxes, each optionally attached to one of five animated bones, and each
// wearing a greyscale surface tile (fur, wool, cloth, wood) that its colour tints.
//
// The walk cycle is derived purely from how an entity's position changes frame to
// frame, with no cooperation from the AI at all. That is what lets a local mob, a
// network ghost and a remote player animate identically with nothing in the wire
// protocol about it.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/camera.h"
#include "core/gl.h"
#include "core/shader.h"
#include "game/entities/manager.h"
#include "render/itemmesh.h"
#include "resource/atlas.h"

namespace hr::world {
class World;
}

namespace hr::render {

// The detail a mob box wears, as one of the greyscale entity/* tiles its colour is
// multiplied over. Plain is a white tile, for parts too small to show a texture.
enum class Surface : std::uint8_t { Plain, Hide, Wool, Cloth, Grain };

// One box of a multi-box mesh: a centre, a half-extent, a colour, which bone
// carries it, and what its surface is made of. Bone 0 never moves.
struct MeshBox {
  float cx = 0, cy = 0, cz = 0;
  float hx = 0, hy = 0, hz = 0;
  float r = 1, g = 1, b = 1;
  int bone = 0;
  Surface surface = Surface::Hide;
};

// The vertices for a list of boxes, headless so the self-test can check them.
// `tiles` gives each Surface's atlas rect in enum order; with none, every UV is
// zero and the mesh is drawn untextured, as mobs were before they had surfaces.
std::vector<ItemVertex> buildBoxVertices(const std::vector<MeshBox>& boxes,
                                         const resource::TileRef* tiles);

// Every mob's model as a list of boxes, at rest. Free functions rather than members
// so the self-test can inspect them without a GL context — which is how coplanar
// faces are caught: two boxes sharing a face plane and a facing fight over the same
// pixels, and at rest that flicker is invisible in any list of numbers.
std::vector<MeshBox> sheepBoxes();
std::vector<MeshBox> pigBoxes();
std::vector<MeshBox> cowBoxes();
std::vector<MeshBox> zombieBoxes();
std::vector<MeshBox> playerBoxes(int palette);
std::vector<MeshBox> boatBoxes();

class EntityRenderer {
 public:
  bool init(ShaderCache& shaders, const resource::Atlas* atlas, ItemMeshCache* itemMeshes);
  void dispose();

  // Entities into the G-buffer. Surface light is the baked sky/block level at the
  // entity's cell, so the composite pass turns it into final colour exactly as it
  // does for terrain — no fog or daylight maths here.
  void drawGBuffer(const world::World& world, const game::EntityManager& entities,
                   const Camera& camera, double now);

  // The same meshes depth-only into the sun shadow map.
  void drawShadow(const world::World& world, const game::EntityManager& entities,
                  const float lightVP[16], double now);

 private:
  struct Mesh {
    GLuint vao = 0, vbo = 0;
    GLsizei count = 0;
  };
  // What to draw for one entity, and how.
  struct Model {
    const ItemMesh* item = nullptr;  // set for a textured item drop
    const Mesh* mesh = nullptr;      // set for an untextured multi-box
    bool textured = false;
    float tint[3] = {1, 1, 1};
    float yOff = 0;
    float scale = 1;
  };
  // Per-entity walk state, integrated from position deltas.
  struct AnimState {
    double t = 0;
    float x = 0, z = 0;
    float phase = 0, amp = 0;
    float head = 0;
    float idle = 0;  // below zero is the grazing window
    double seen = 0;
  };

  const Mesh& buildMultiBox(Mesh& slot, const std::vector<MeshBox>& boxes);
  const Mesh& sheepMesh();
  const Mesh& pigMesh();
  const Mesh& cowMesh();
  const Mesh& zombieMesh();
  const Mesh& playerMesh(int palette);
  const Mesh& boatMesh();
  const Mesh& unitCube();

  bool modelFor(const game::Entity& e, Model& out);
  AnimState& animState(const game::Entity& e, double now);
  // Fills `bones` with six (pivot.xyz, angle) quads. False for a type with no
  // skeleton — drops and boats — for which zeros are uploaded instead.
  bool bonesFor(const game::Entity& e, double now, float bones[24]);
  void pruneAnim(double now);

  const resource::Atlas* atlas_ = nullptr;
  ItemMeshCache* itemMeshes_ = nullptr;
  Program* gbufferProg_ = nullptr;
  Program* shadowProg_ = nullptr;

  Mesh sheep_, pig_, cow_, zombie_, boat_, cube_;
  // One per palette: the eight shirt colours a remote player can wear, built on
  // demand so a solo game never allocates any of them.
  Mesh players_[8];
  std::unordered_map<int, AnimState> anim_;
  std::uint32_t frame_ = 0;
};

}  // namespace hr::render
