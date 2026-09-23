#include "render/entityrenderer.h"

#include <algorithm>
#include <cmath>

#include "core/mat4.h"
#include "core/prng.h"
#include "world/world.h"

namespace hr::render {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kCubeHalf = 0.22f;  // the unknown-item fallback cube

// Face index 0:+x 1:-x 2:+y 3:-y 4:+z 5:-z, with its flat shade and four corners
// as +/-1 offsets. The same six shades the block mesher uses, so a mob standing on
// grass is lit on the same curve as the grass.
struct Face {
  float shade;
  int c[4][3];
};
constexpr Face kFaces[6] = {
    {0.80f, {{1, -1, -1}, {1, -1, 1}, {1, 1, 1}, {1, 1, -1}}},
    {0.80f, {{-1, -1, 1}, {-1, -1, -1}, {-1, 1, -1}, {-1, 1, 1}}},
    {1.00f, {{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}}},
    {0.55f, {{-1, -1, 1}, {1, -1, 1}, {1, -1, -1}, {-1, -1, -1}}},
    {0.85f, {{1, -1, 1}, {-1, -1, 1}, {-1, 1, 1}, {1, 1, 1}}},
    {0.85f, {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}}},
};

// Per-type animation tuning. `ref` is the speed in blocks per second that reads as
// a full stride; `stride` is the phase advanced per block travelled, so shorter
// legs scurry faster. `legs` is legX, frontZ, backZ and the head pivot height.
struct AnimTuning {
  bool valid = false;
  float ref = 1.0f, stride = 1.0f;
  bool graze = false;
  float hip = 0, legX = 0, frontZ = 0, backZ = 0, headY = 0, neckZ = 0;
};

AnimTuning tuningFor(game::EntityType type) {
  switch (type) {
    case game::EntityType::Sheep:
      return {true, 1.7f, 3.4f, true, 0.38f, 0.18f, 0.28f, -0.26f, 0.62f, 0.40f};
    case game::EntityType::Pig:
      return {true, 1.6f, 3.6f, true, 0.27f, 0.17f, 0.26f, -0.24f, 0.48f, 0.34f};
    case game::EntityType::Cow:
      return {true, 1.4f, 2.8f, true, 0.56f, 0.23f, 0.38f, -0.36f, 0.98f, 0.52f};
    case game::EntityType::Zombie:
      return {true, 2.4f, 2.2f};
    case game::EntityType::RemotePlayer:
      return {true, 4.3f, 2.3f};
    default:
      return {};
  }
}

MeshBox box(float cx, float cy, float cz, float hx, float hy, float hz, float r, float g,
            float b, int bone = 0) {
  return MeshBox{cx, cy, cz, hx, hy, hz, r, g, b, bone};
}

// Gives every box of one colour a surface. The models are authored as colour lists,
// and a colour is already what says "this is the fleece" or "this is the shirt", so
// matching on it keeps the surfaces next to the palette instead of threading a sixth
// argument through a hundred box() calls.
void wear(std::vector<MeshBox>& boxes, const float colour[3], Surface surface) {
  for (MeshBox& b : boxes) {
    if (b.r == colour[0] && b.g == colour[1] && b.b == colour[2]) b.surface = surface;
  }
}

// A cheap integer mix, for placing texture windows. Not a PRNG: the same box and
// face must land on the same texels every time the mesh is rebuilt.
std::uint32_t hashU32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

}  // namespace

// Mob textures are laid on at the world's own density — one tile to a block —
// so a sheep's fleece has curls the size of a grass blade's pixels rather than
// being one tile stretched over the whole body. A face wider than a block is the
// only thing that stretches.
//
// Each face takes its own window of the tile, placed by a hash of the box and the
// face, so the six sides of a body and the four identical legs under it do not all
// show the same sixteen texels.
std::vector<ItemVertex> buildBoxVertices(const std::vector<MeshBox>& boxes,
                                         const resource::TileRef* tiles) {
  // Tiles average about 0.9 (see the entity/* painters), so colours are lifted by
  // the inverse to keep each mob the colour it was designed in.
  constexpr float kLift = 1.1f;
  // Anything thinner than this is an eye, a nostril or a painted-on blaze — a decal
  // a single texel of noise would only smudge.
  constexpr float kDecal = 0.012f;
  // The tangent axes of each face (u, then v), in kFaces order.
  static constexpr int kAxes[6][2] = {{2, 1}, {2, 1}, {0, 2}, {0, 2}, {0, 1}, {0, 1}};

  std::vector<ItemVertex> verts;
  verts.reserve(boxes.size() * 36);
  std::uint32_t boxIndex = 0;
  for (const MeshBox& b : boxes) {
    const float half[3] = {b.hx, b.hy, b.hz};
    const bool decal = std::min(b.hx, std::min(b.hy, b.hz)) < kDecal;
    const Surface surface = decal ? Surface::Plain : b.surface;
    const resource::TileRef* tile = tiles ? &tiles[static_cast<int>(surface)] : nullptr;
    const float lift = (tiles && surface != Surface::Plain) ? kLift : 1.0f;

    for (int fi = 0; fi < 6; ++fi) {
      const Face& f = kFaces[fi];
      const int ua = kAxes[fi][0], va = kAxes[fi][1];
      // The face's size in texels, capped at the tile, and where in the tile it sits.
      float uTex = 0, vTex = 0, u0 = 0, v0 = 0;
      if (tile) {
        const float res = static_cast<float>(tile->w);
        uTex = std::min(res, std::max(1.0f, std::round(half[ua] * 2.0f * res)));
        vTex = std::min(res, std::max(1.0f, std::round(half[va] * 2.0f * res)));
        const std::uint32_t h = hashU32((boxIndex * 6u + static_cast<std::uint32_t>(fi)) *
                                        2654435761u);
        u0 = static_cast<float>(h % static_cast<std::uint32_t>(res - uTex + 1));
        v0 = static_cast<float>((h >> 8) % static_cast<std::uint32_t>(res - vTex + 1));
      }
      const auto corner = [&](int i) {
        ItemVertex v;
        v.x = b.cx + f.c[i][0] * b.hx;
        v.y = b.cy + f.c[i][1] * b.hy;
        v.z = b.cz + f.c[i][2] * b.hz;
        if (tile) {
          // -1..1 across the face; v runs top to bottom, so +y is the tile's top.
          const float su = (f.c[i][ua] + 1) * 0.5f;
          const float sv = va == 1 ? (1 - f.c[i][1]) * 0.5f : (f.c[i][va] + 1) * 0.5f;
          const float du = (tile->u1 - tile->u0) / static_cast<float>(tile->w);
          const float dv = (tile->v1 - tile->v0) / static_cast<float>(tile->h);
          v.u = static_cast<std::uint16_t>(
              std::lround((tile->u0 + (u0 + su * uTex) * du) * 65535.0f));
          v.v = static_cast<std::uint16_t>(
              std::lround((tile->v0 + (v0 + sv * vTex) * dv) * 65535.0f));
        }
        v.shade = static_cast<std::uint8_t>(std::lround(f.shade * 255.0f));
        v.bone = static_cast<std::uint8_t>(b.bone);
        v.r = static_cast<std::uint8_t>(std::lround(std::min(1.0f, b.r * lift) * 255.0f));
        v.g = static_cast<std::uint8_t>(std::lround(std::min(1.0f, b.g * lift) * 255.0f));
        v.b = static_cast<std::uint8_t>(std::lround(std::min(1.0f, b.b * lift) * 255.0f));
        v.a = 255;
        verts.push_back(v);
      };
      corner(0);
      corner(1);
      corner(2);
      corner(0);
      corner(2);
      corner(3);
    }
    ++boxIndex;
  }
  return verts;
}

bool EntityRenderer::init(ShaderCache& shaders, const resource::Atlas* atlas,
                          ItemMeshCache* itemMeshes) {
  atlas_ = atlas;
  itemMeshes_ = itemMeshes;
  gbufferProg_ = shaders.load({
      .name = "gbufferEntity",
      .vertAsset = "shaders/gbuffer_entity.vert",
      .fragAsset = "shaders/gbuffer_entity.frag",
      .defines = {},
      .attribs = {"aPos", "aUV", "aShade", "aBone", "aColor"},
  });
  shadowProg_ = shaders.load({
      .name = "shadowEntity",
      .vertAsset = "shaders/shadow_entity.vert",
      .fragAsset = "shaders/shadow_entity.frag",
      .defines = {},
      .attribs = {"aPos", "aUV", "aShade", "aBone", "aColor"},
  });
  return gbufferProg_ && shadowProg_;
}

void EntityRenderer::dispose() {
  for (Mesh* m : {&sheep_, &pig_, &cow_, &zombie_, &boat_, &cube_}) {
    if (m->vbo) glDeleteBuffers(1, &m->vbo);
    if (m->vao) glDeleteVertexArrays(1, &m->vao);
    *m = Mesh{};
  }
  anim_.clear();
}

const EntityRenderer::Mesh& EntityRenderer::buildMultiBox(Mesh& slot,
                                                          const std::vector<MeshBox>& boxes) {
  if (slot.count > 0) return slot;

  // The surface tiles, in Surface order. Without an atlas the mesh keeps zero UVs
  // and modelFor draws it untextured, which is exactly what every mob was before.
  resource::TileRef tiles[5];
  const bool textured = atlas_ != nullptr;
  if (textured) {
    static const char* kNames[5] = {"entity/plain", "entity/hide", "entity/wool",
                                    "entity/cloth", "entity/grain"};
    for (int i = 0; i < 5; ++i) tiles[i] = atlas_->tile(ResourceId(kNames[i]));
  }
  const std::vector<ItemVertex> verts = buildBoxVertices(boxes, textured ? tiles : nullptr);

  glGenVertexArrays(1, &slot.vao);
  glBindVertexArray(slot.vao);
  glGenBuffers(1, &slot.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, slot.vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(ItemVertex)),
               verts.data(), GL_STATIC_DRAW);
  bindItemAttributes();
  glBindVertexArray(0);
  slot.count = static_cast<GLsizei>(verts.size());
  return slot;
}

// +z is forward for every mob, matching the model convention the AI's yaw uses.

std::vector<MeshBox> sheepBoxes() {
  // A fluffy cream fleece, a tan face with eyes and drooping ears under a woolly
  // cap, a tuft of a tail, and short dark legs with woolly cuffs at the top.
  // Bones: 1-4 the legs (FL FR BL BR, cuffs riding along), 5 the head group.
  //
  // Rebuilt so that no two boxes share a face plane. The old body had a "woolly
  // tail end" block whose back sat exactly flush with the back of the body, and
  // two faces at one depth flicker between each other from any angle that sees
  // them: the shimmering rump every sheep had. The fleece is now a core with a
  // wider band around its middle, which rounds the silhouette the way the tail
  // block was meant to, and every surface of it is at a depth of its own.
  const float W[3] = {0.95f, 0.94f, 0.90f};
  const float F[3] = {0.84f, 0.71f, 0.57f};
  const float S[3] = {0.74f, 0.60f, 0.47f};
  const float L[3] = {0.42f, 0.34f, 0.28f};
  const float E[3] = {0.10f, 0.09f, 0.08f};
  std::vector<MeshBox> boxes = {
      box(0, 0.58f, -0.02f, 0.30f, 0.28f, 0.43f, W[0], W[1], W[2]),     // fleece core
      box(0, 0.60f, -0.02f, 0.33f, 0.24f, 0.40f, W[0], W[1], W[2]),     // ...and its band
      box(0, 0.70f, -0.48f, 0.09f, 0.08f, 0.05f, W[0], W[1], W[2]),     // tail tuft
      box(0, 0.66f, 0.50f, 0.18f, 0.19f, 0.17f, F[0], F[1], F[2], 5),  // head
      box(0, 0.58f, 0.66f, 0.11f, 0.10f, 0.07f, S[0], S[1], S[2], 5),  // snout
      box(0, 0.84f, 0.485f, 0.19f, 0.07f, 0.165f, W[0], W[1], W[2], 5),  // woolly cap
      box(0.085f, 0.71f, 0.672f, 0.026f, 0.032f, 0.006f, E[0], E[1], E[2], 5),
      box(-0.085f, 0.71f, 0.672f, 0.026f, 0.032f, 0.006f, E[0], E[1], E[2], 5),
      box(0.21f, 0.72f, 0.46f, 0.045f, 0.032f, 0.065f, F[0], F[1], F[2], 5),
      box(-0.21f, 0.72f, 0.46f, 0.045f, 0.032f, 0.065f, F[0], F[1], F[2], 5),
  };
  const float cuffZ[4] = {0.28f, 0.28f, -0.26f, -0.26f};
  const float legX[4] = {0.18f, -0.18f, 0.18f, -0.18f};
  for (int i = 0; i < 4; ++i) {
    boxes.push_back(box(legX[i], 0.32f, cuffZ[i], 0.095f, 0.05f, 0.095f, W[0], W[1], W[2], i + 1));
    boxes.push_back(box(legX[i], 0.17f, cuffZ[i], 0.08f, 0.17f, 0.08f, L[0], L[1], L[2], i + 1));
  }
  wear(boxes, W, Surface::Wool);
  return boxes;
}

std::vector<MeshBox> pigBoxes() {
  // A rounded pink body, a flat snout with nostrils, perky ears, four little legs
  // and a curly tail nub at the back.
  const float B[3] = {0.91f, 0.60f, 0.64f};
  const float S[3] = {0.83f, 0.49f, 0.55f};
  const float L[3] = {0.80f, 0.50f, 0.54f};
  const float E[3] = {0.12f, 0.10f, 0.10f};
  const float N[3] = {0.62f, 0.32f, 0.38f};
  std::vector<MeshBox> boxes = {
      box(0, 0.46f, 0, 0.30f, 0.26f, 0.42f, B[0], B[1], B[2]),
      box(0, 0.50f, 0.46f, 0.20f, 0.19f, 0.14f, B[0], B[1], B[2], 5),
      box(0, 0.45f, 0.62f, 0.11f, 0.09f, 0.05f, S[0], S[1], S[2], 5),
      box(0.12f, 0.70f, 0.44f, 0.05f, 0.055f, 0.03f, S[0], S[1], S[2], 5),
      box(-0.12f, 0.70f, 0.44f, 0.05f, 0.055f, 0.03f, S[0], S[1], S[2], 5),
      box(0.09f, 0.565f, 0.602f, 0.026f, 0.03f, 0.006f, E[0], E[1], E[2], 5),
      box(-0.09f, 0.565f, 0.602f, 0.026f, 0.03f, 0.006f, E[0], E[1], E[2], 5),
      box(0.04f, 0.45f, 0.673f, 0.016f, 0.026f, 0.006f, N[0], N[1], N[2], 5),
      box(-0.04f, 0.45f, 0.673f, 0.016f, 0.026f, 0.006f, N[0], N[1], N[2], 5),
      box(0, 0.56f, -0.44f, 0.028f, 0.028f, 0.045f, S[0], S[1], S[2]),
  };
  const float legZ[4] = {0.26f, 0.26f, -0.24f, -0.24f};
  const float legX[4] = {0.17f, -0.17f, 0.17f, -0.17f};
  for (int i = 0; i < 4; ++i) {
    boxes.push_back(box(legX[i], 0.13f, legZ[i], 0.08f, 0.13f, 0.08f, L[0], L[1], L[2], i + 1));
  }
  return boxes;
}

std::vector<MeshBox> cowBoxes() {
  // A big barrel body in brown with white patches, a blazed face with pale horns,
  // a pink muzzle and udder, and tall dark legs.
  const float B[3] = {0.45f, 0.32f, 0.24f};
  const float P[3] = {0.93f, 0.91f, 0.86f};
  const float H[3] = {0.88f, 0.85f, 0.74f};
  const float M[3] = {0.85f, 0.62f, 0.60f};
  const float U[3] = {0.90f, 0.68f, 0.66f};
  const float L[3] = {0.32f, 0.24f, 0.19f};
  const float E[3] = {0.10f, 0.09f, 0.08f};
  std::vector<MeshBox> boxes = {
      box(0, 0.86f, -0.02f, 0.36f, 0.30f, 0.55f, B[0], B[1], B[2]),
      box(0.21f, 0.94f, -0.28f, 0.16f, 0.23f, 0.20f, P[0], P[1], P[2]),   // hip patch
      box(-0.18f, 0.80f, 0.22f, 0.19f, 0.20f, 0.18f, P[0], P[1], P[2]),   // shoulder patch
      box(0, 1.06f, 0.68f, 0.20f, 0.20f, 0.16f, B[0], B[1], B[2], 5),
      box(0, 1.10f, 0.845f, 0.07f, 0.15f, 0.006f, P[0], P[1], P[2], 5),   // blaze
      box(0, 0.94f, 0.82f, 0.14f, 0.09f, 0.06f, M[0], M[1], M[2], 5),
      box(0.105f, 1.12f, 0.842f, 0.028f, 0.032f, 0.006f, E[0], E[1], E[2], 5),
      box(-0.105f, 1.12f, 0.842f, 0.028f, 0.032f, 0.006f, E[0], E[1], E[2], 5),
      box(0.155f, 1.26f, 0.60f, 0.035f, 0.075f, 0.035f, H[0], H[1], H[2], 5),
      box(-0.155f, 1.26f, 0.60f, 0.035f, 0.075f, 0.035f, H[0], H[1], H[2], 5),
      box(0.24f, 1.16f, 0.60f, 0.055f, 0.035f, 0.03f, B[0], B[1], B[2], 5),
      box(-0.24f, 1.16f, 0.60f, 0.055f, 0.035f, 0.03f, B[0], B[1], B[2], 5),
      box(0, 0.50f, -0.20f, 0.14f, 0.08f, 0.16f, U[0], U[1], U[2]),
      box(0, 1.00f, -0.585f, 0.03f, 0.14f, 0.03f, B[0], B[1], B[2]),  // tail, hung below the back
  };
  const float legZ[4] = {0.38f, 0.38f, -0.36f, -0.36f};
  const float legX[4] = {0.23f, -0.23f, 0.23f, -0.23f};
  for (int i = 0; i < 4; ++i) {
    boxes.push_back(box(legX[i], 0.28f, legZ[i], 0.095f, 0.28f, 0.095f, L[0], L[1], L[2], i + 1));
  }
  return boxes;
}

std::vector<MeshBox> zombieBoxes() {
  // Green skin, a tattered shirt torn open over the belly, trousers with ripped
  // hems, arms split into sleeve and reaching bare hands, and sunken dark eyes
  // under a heavy brow. Bones: 1/2 the legs, 3/4 the arms.
  const float SK[3] = {0.36f, 0.55f, 0.34f};
  const float SD[3] = {0.30f, 0.46f, 0.28f};
  const float SH[3] = {0.22f, 0.32f, 0.46f};
  const float PA[3] = {0.26f, 0.23f, 0.34f};
  const float EY[3] = {0.08f, 0.05f, 0.05f};
  std::vector<MeshBox> boxes = {
      box(-0.13f, 0.14f, 0, 0.105f, 0.14f, 0.105f, SD[0], SD[1], SD[2], 2),  // ripped hems
      box(0.13f, 0.14f, 0, 0.105f, 0.14f, 0.105f, SD[0], SD[1], SD[2], 1),
      box(-0.13f, 0.52f, 0, 0.12f, 0.24f, 0.12f, PA[0], PA[1], PA[2], 2),
      box(0.13f, 0.52f, 0, 0.12f, 0.24f, 0.12f, PA[0], PA[1], PA[2], 1),
      box(0, 1.06f, 0, 0.24f, 0.32f, 0.14f, SH[0], SH[1], SH[2]),
      box(0.06f, 0.88f, 0.148f, 0.10f, 0.075f, 0.004f, SD[0], SD[1], SD[2]),  // shirt torn open
      box(-0.19f, 1.30f, 0.148f, 0.05f, 0.05f, 0.004f, SD[0], SD[1], SD[2]),
      box(0, 1.60f, 0.02f, 0.21f, 0.21f, 0.21f, SK[0], SK[1], SK[2]),
      box(0, 1.70f, 0.225f, 0.16f, 0.028f, 0.012f, SD[0], SD[1], SD[2]),  // brow
      box(0.095f, 1.645f, 0.232f, 0.035f, 0.032f, 0.006f, EY[0], EY[1], EY[2]),
      box(-0.095f, 1.645f, 0.232f, 0.035f, 0.032f, 0.006f, EY[0], EY[1], EY[2]),
  };
  // The arms reach forward: a shirt sleeve at the shoulder, bare hands out front.
  for (int side = 0; side < 2; ++side) {
    const float x = side == 0 ? 0.32f : -0.32f;
    const int bone = side == 0 ? 3 : 4;
    boxes.push_back(box(x, 1.20f, 0.09f, 0.105f, 0.105f, 0.115f, SH[0], SH[1], SH[2], bone));
    boxes.push_back(box(x, 1.20f, 0.34f, 0.10f, 0.10f, 0.20f, SK[0], SK[1], SK[2], bone));
    boxes.push_back(box(x, 1.19f, 0.555f, 0.085f, 0.085f, 0.045f, SD[0], SD[1], SD[2], bone));
  }
  wear(boxes, SH, Surface::Cloth);
  wear(boxes, PA, Surface::Cloth);
  return boxes;
}

std::vector<MeshBox> playerBoxes(int palette) {
  const int index = ((palette % 8) + 8) % 8;

  // The one piece of js/render/entityrenderer.js that M8 left unported, because
  // nothing could spawn a ghost until there was a transport. Eight shirt colours,
  // picked by the player id, so two guests are told apart at a glance without any
  // texture work — which is also why this is a per-palette mesh rather than one
  // mesh with a tint: the vertex colour is baked, and the entity vertex's spare
  // RGBA channel is reserved for the hurt flash.
  static constexpr float kShirts[8][3] = {
      {0.31f, 0.55f, 0.78f},  // blue
      {0.72f, 0.35f, 0.33f},  // rust
      {0.42f, 0.63f, 0.36f},  // green
      {0.78f, 0.64f, 0.30f},  // amber
      {0.55f, 0.42f, 0.70f},  // violet
      {0.30f, 0.63f, 0.62f},  // teal
      {0.80f, 0.52f, 0.66f},  // rose
      {0.55f, 0.55f, 0.58f},  // slate
  };
  const float* SH = kShirts[index];
  const float SK[3] = {0.83f, 0.66f, 0.52f};  // skin
  const float TR[3] = {0.26f, 0.28f, 0.34f};  // trousers
  const float HR[3] = {0.24f, 0.18f, 0.14f};  // hair
  const float EY[3] = {0.10f, 0.10f, 0.13f};

  // Proportioned to the 1.8-block body the player physics uses: legs to 0.72,
  // torso to 1.44, head on top. Bones 1/2 are the legs and 3/4 the arms, which is
  // the same skeleton the zombie uses, so the walk cycle needs no special case.
  std::vector<MeshBox> boxes = {
      // Legs stand ON the boots rather than running to the ground inside them,
      // which shared the soles and the heels with the boot boxes.
      box(-0.11f, 0.41f, 0, 0.105f, 0.31f, 0.11f, TR[0], TR[1], TR[2], 2),
      box(0.11f, 0.41f, 0, 0.105f, 0.31f, 0.11f, TR[0], TR[1], TR[2], 1),
      box(-0.11f, 0.05f, 0.02f, 0.11f, 0.05f, 0.13f, HR[0], HR[1], HR[2], 2),  // boots
      box(0.11f, 0.05f, 0.02f, 0.11f, 0.05f, 0.13f, HR[0], HR[1], HR[2], 1),
      box(0, 1.08f, 0, 0.23f, 0.36f, 0.12f, SH[0], SH[1], SH[2]),   // torso
      box(0, 0.76f, 0, 0.235f, 0.05f, 0.125f, TR[0], TR[1], TR[2]),  // belt
      // Bone 5 is the neck: the head follows the remote player's own camera pitch,
      // which is the one piece of a body that says where somebody is looking.
      box(0, 1.60f, 0, 0.155f, 0.155f, 0.155f, SK[0], SK[1], SK[2], 5),   // head
      box(0, 1.68f, -0.01f, 0.16f, 0.09f, 0.16f, HR[0], HR[1], HR[2], 5),  // hair cap
      box(0.065f, 1.60f, 0.158f, 0.032f, 0.028f, 0.006f, EY[0], EY[1], EY[2], 5),
      box(-0.065f, 1.60f, 0.158f, 0.032f, 0.028f, 0.006f, EY[0], EY[1], EY[2], 5),
  };
  // Arms hang at the sides: a sleeve from the shoulder, a bare hand below.
  for (int side = 0; side < 2; ++side) {
    const float x = side == 0 ? 0.335f : -0.335f;
    const int bone = side == 0 ? 3 : 4;
    boxes.push_back(box(x, 1.20f, 0, 0.105f, 0.24f, 0.105f, SH[0], SH[1], SH[2], bone));
    boxes.push_back(box(x, 0.88f, 0, 0.10f, 0.09f, 0.10f, SK[0], SK[1], SK[2], bone));
  }
  wear(boxes, SH, Surface::Cloth);
  wear(boxes, TR, Surface::Cloth);
  return boxes;
}

std::vector<MeshBox> boatBoxes() {
  // A proper rowboat: a dark keel slab, plank side walls and stern, a two-step
  // tapered bow with a small foredeck, a pale interior floor and seat bench, and
  // darker gunwale caps along the rims. Origin at the hull bottom; the waterline
  // rides at about y = 0.18.
  const float W[3] = {0.63f, 0.47f, 0.29f};
  const float D[3] = {0.45f, 0.33f, 0.20f};
  const float L[3] = {0.74f, 0.59f, 0.40f};
  // The hull is modelled bow-to--z. Mobs use "+z is forward" and derive their yaw
  // from movement, but a ridden boat is handed the PLAYER's yaw (boat.cpp:95), and
  // the player's yaw 0 faces -z (core/mat4.cpp lookDir). The two conventions are
  // 180 degrees apart, which is why the original sat you facing the stern -- in the
  // browser too. Flipping the model rather than the yaw keeps the one place the
  // boat's heading is decided, and an unridden boat has no heading of its own to
  // disagree with.
  std::vector<MeshBox> boxes = {
      // No two of these share a face plane: planks that met flush flickered
      // against each other wherever they overlapped, most of all along the floor
      // and the gunwales. Each now starts and ends a hair inside or outside its
      // neighbour.
      box(0, 0.06f, 0.04f, 0.42f, 0.06f, 0.60f, D[0], D[1], D[2]),      // keel
      box(0, 0.14f, 0.04f, 0.37f, 0.025f, 0.54f, L[0], L[1], L[2]),     // floor
      box(0.44f, 0.265f, 0.06f, 0.075f, 0.155f, 0.545f, W[0], W[1], W[2]),
      box(-0.44f, 0.265f, 0.06f, 0.075f, 0.155f, 0.545f, W[0], W[1], W[2]),
      box(0, 0.27f, 0.60f, 0.45f, 0.14f, 0.075f, W[0], W[1], W[2]),     // stern
      box(0, 0.265f, -0.53f, 0.34f, 0.145f, 0.075f, W[0], W[1], W[2]),  // bow
      box(0, 0.28f, -0.65f, 0.20f, 0.12f, 0.06f, W[0], W[1], W[2]),
      box(0, 0.315f, -0.735f, 0.08f, 0.095f, 0.035f, D[0], D[1], D[2]),  // bow tip
      box(0.44f, 0.435f, 0.06f, 0.085f, 0.02f, 0.55f, D[0], D[1], D[2]),
      box(-0.44f, 0.435f, 0.06f, 0.085f, 0.02f, 0.55f, D[0], D[1], D[2]),
      box(0, 0.44f, 0.60f, 0.46f, 0.02f, 0.09f, D[0], D[1], D[2]),
      box(0, 0.24f, 0.30f, 0.36f, 0.03f, 0.11f, L[0], L[1], L[2]),      // seat
      box(0, 0.395f, -0.54f, 0.21f, 0.022f, 0.16f, L[0], L[1], L[2]),   // foredeck
  };
  for (MeshBox& b : boxes) b.surface = Surface::Grain;
  return boxes;
}

// The GL side: each model built once, on first sight, from the lists above.
const EntityRenderer::Mesh& EntityRenderer::sheepMesh() {
  return sheep_.count > 0 ? sheep_ : buildMultiBox(sheep_, sheepBoxes());
}

const EntityRenderer::Mesh& EntityRenderer::pigMesh() {
  return pig_.count > 0 ? pig_ : buildMultiBox(pig_, pigBoxes());
}

const EntityRenderer::Mesh& EntityRenderer::cowMesh() {
  return cow_.count > 0 ? cow_ : buildMultiBox(cow_, cowBoxes());
}

const EntityRenderer::Mesh& EntityRenderer::zombieMesh() {
  return zombie_.count > 0 ? zombie_ : buildMultiBox(zombie_, zombieBoxes());
}

const EntityRenderer::Mesh& EntityRenderer::boatMesh() {
  return boat_.count > 0 ? boat_ : buildMultiBox(boat_, boatBoxes());
}

const EntityRenderer::Mesh& EntityRenderer::playerMesh(int palette) {
  const int index = ((palette % 8) + 8) % 8;
  Mesh& slot = players_[index];
  return slot.count > 0 ? slot : buildMultiBox(slot, playerBoxes(index));
}

const EntityRenderer::Mesh& EntityRenderer::unitCube() {
  if (cube_.count > 0) return cube_;
  MeshBox cube = box(0, 0, 0, kCubeHalf, kCubeHalf, kCubeHalf, 1, 1, 1);
  cube.surface = Surface::Plain;
  return buildMultiBox(cube_, {cube});
}

bool EntityRenderer::modelFor(const game::Entity& e, Model& out) {
  out = Model{};
  const bool hurt = e.data.hurtFlash > 0.0f;
  const auto hurtTint = [&](Model& m) {
    if (!hurt) return;
    m.tint[0] = 1.4f;
    m.tint[1] = 0.5f;
    m.tint[2] = 0.5f;
  };

  switch (e.type) {
    case game::EntityType::Drop: {
      const ItemMesh* m = itemMeshes_ ? itemMeshes_->get(e.data.key) : nullptr;
      if (m && m->valid()) {
        out.item = m;
        out.textured = true;
        out.yOff = 0.06f;
        out.scale = m->kind == ItemModelKind::Shape ? 0.44f : 0.58f;
        // A dyed drop wears its colour on the ground. Folded into the per-model tint
        // that already exists — the one the hit flash uses — rather than into a new
        // uniform or a second cached mesh: item meshes are keyed by item key, so a
        // per-vertex colour would fork the cache once per shade anybody ever mixed.
        // A drop never flashes, so the two uses cannot collide.
        if (e.data.tint >= 0) {
          out.tint[0] = static_cast<float>((e.data.tint >> 16) & 0xFF) / 255.0f;
          out.tint[1] = static_cast<float>((e.data.tint >> 8) & 0xFF) / 255.0f;
          out.tint[2] = static_cast<float>(e.data.tint & 0xFF) / 255.0f;
        }
        return true;
      }
      out.mesh = &unitCube();
      out.textured = atlas_ != nullptr;
      out.yOff = kCubeHalf;
      out.tint[0] = out.tint[1] = out.tint[2] = 0.8f;
      return true;
    }
    case game::EntityType::FallingBlock: {
      // The block's own item model, at full size and unrotated — it is the same
      // cube that was in the grid a moment ago and has to read as that cube, not
      // as a dropped item. `key` is carried purely so this lookup works; the
      // BlockId the entity actually puts back down lives in `dura`.
      const ItemMesh* m = itemMeshes_ ? itemMeshes_->get(e.data.key) : nullptr;
      if (m && m->valid()) {
        out.item = m;
        out.textured = true;
        out.scale = 1.0f;
        out.yOff = kCubeHalf;
        return true;
      }
      out.mesh = &unitCube();
      out.yOff = kCubeHalf;
      return true;
    }
    case game::EntityType::Boat:
      out.mesh = &boatMesh();
      out.textured = atlas_ != nullptr;
      return true;
    case game::EntityType::Sheep:
      out.mesh = &sheepMesh();
      out.textured = atlas_ != nullptr;
      hurtTint(out);
      return true;
    case game::EntityType::Pig:
      out.mesh = &pigMesh();
      out.textured = atlas_ != nullptr;
      hurtTint(out);
      return true;
    case game::EntityType::Cow:
      out.mesh = &cowMesh();
      out.textured = atlas_ != nullptr;
      hurtTint(out);
      return true;
    case game::EntityType::Zombie:
      out.mesh = &zombieMesh();
      out.textured = atlas_ != nullptr;
      hurtTint(out);
      return true;
    case game::EntityType::RemotePlayer:
      // The palette comes off the network id, which is stable for as long as the
      // player is connected — so their colour does not change while you watch.
      out.mesh = &playerMesh(e.netId);
      out.textured = atlas_ != nullptr;
      hurtTint(out);
      return true;
    default:
      return false;
  }
}

EntityRenderer::AnimState& EntityRenderer::animState(const game::Entity& e, double now) {
  auto it = anim_.find(e.id);
  if (it == anim_.end()) {
    AnimState st;
    st.t = now;
    st.x = e.pos.x;
    st.z = e.pos.z;
    st.phase = static_cast<float>(randomUnit()) * kPi * 2.0f;
    st.idle = 3.0f + static_cast<float>(randomUnit()) * 6.0f;
    st.seen = now;
    it = anim_.emplace(e.id, st).first;
  }
  AnimState& st = it->second;

  const double dtSec = now - st.t;
  if (dtSec > 0.0) {
    const float dt = static_cast<float>(std::min(0.1, dtSec));
    const float dx = e.pos.x - st.x, dz = e.pos.z - st.z;
    float speed = std::sqrt(dx * dx + dz * dz) / dt;
    if (speed > 20.0f) speed = 0.0f;  // a teleport or a respawn, not a sprint
    const AnimTuning a = tuningFor(e.type);
    st.phase += speed * a.stride * dt;
    st.amp += (std::min(1.15f, speed / a.ref) - st.amp) * std::min(1.0f, dt * 12.0f);
    st.idle -= dt;
    if (st.idle < -2.8f) st.idle = 3.0f + static_cast<float>(randomUnit()) * 7.0f;
    // Grazers dip their head while standing still — the idle window.
    const float dip = (a.graze && st.idle < 0.0f && st.amp < 0.2f) ? 0.55f : 0.0f;
    st.head += (dip - st.head) * std::min(1.0f, dt * 3.5f);
    st.x = e.pos.x;
    st.z = e.pos.z;
    st.t = now;
  }
  st.seen = now;
  return st;
}

bool EntityRenderer::bonesFor(const game::Entity& e, double now, float bones[24]) {
  const AnimTuning a = tuningFor(e.type);
  for (int i = 0; i < 24; ++i) bones[i] = 0.0f;
  if (!a.valid) return false;

  const AnimState& st = animState(e, now);
  const auto set = [&](int i, float px, float py, float pz, float angle) {
    bones[i * 4 + 0] = px;
    bones[i * 4 + 1] = py;
    bones[i * 4 + 2] = pz;
    bones[i * 4 + 3] = angle;
  };
  const float sw = std::sin(st.phase) * st.amp;

  if (e.type == game::EntityType::Sheep || e.type == game::EntityType::Pig ||
      e.type == game::EntityType::Cow) {
    // Diagonal gait: front-left and back-right swing together, the other pair
    // opposite, which is what stops a quadruped looking like it is hopping.
    const float s = sw * 0.75f;
    set(1, a.legX, a.hip, a.frontZ, s);
    set(2, -a.legX, a.hip, a.frontZ, -s);
    set(3, a.legX, a.hip, a.backZ, -s);
    set(4, -a.legX, a.hip, a.backZ, s);
    set(5, 0, a.headY, a.neckZ, st.head + sw * 0.05f);
  } else if (e.type == game::EntityType::Zombie) {
    const float s = sw * 0.6f;
    set(1, 0.13f, 0.76f, 0, s);
    set(2, -0.13f, 0.76f, 0, -s);
    // The arms stay outstretched; a slow shamble sway plus a touch of walk bob.
    const float wob = std::sin(static_cast<float>(now) * 1.9f + (e.id % 7)) * 0.07f;
    set(3, 0.32f, 1.28f, 0, sw * 0.18f + wob);
    set(4, -0.32f, 1.28f, 0, -sw * 0.18f + wob);
  } else if (e.type == game::EntityType::RemotePlayer) {
    const float s = sw * 0.8f;
    set(1, 0.125f, 0.78f, 0, s);
    set(2, -0.125f, 0.78f, 0, -s);
    set(3, 0.345f, 1.32f, 0, -s * 0.7f);
    set(4, -0.345f, 1.32f, 0, s * 0.7f);
    // Camera pitch is positive up, and a positive bone angle tips the face down.
    set(5, 0, 1.44f, 0.06f, -e.pitch * 0.85f);
  }
  return true;
}

void EntityRenderer::pruneAnim(double now) {
  if ((++frame_ & 255) != 0) return;
  for (auto it = anim_.begin(); it != anim_.end();) {
    it = (now - it->second.seen > 5.0) ? anim_.erase(it) : std::next(it);
  }
}

void EntityRenderer::drawGBuffer(const world::World& world, const game::EntityManager& entities,
                                 const Camera& camera, double now) {
  if (entities.all().empty() || !gbufferProg_) return;
  pruneAnim(now);

  gbufferProg_->use();
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  gbufferProg_->setMat4("uViewProj", camera.viewProj().data());
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, atlas_ ? atlas_->texture() : 0);
  gbufferProg_->set("uAtlas", 0);

  float bones[24];
  for (const game::Entity& e : entities.all()) {
    if (e.dead) continue;
    Model m;
    if (!modelFor(e, m)) continue;

    const int cx = static_cast<int>(std::floor(e.pos.x));
    const int cy = static_cast<int>(std::floor(e.pos.y + e.h * 0.5f));
    const int cz = static_cast<int>(std::floor(e.pos.z));
    const float bob = e.type == game::EntityType::Drop ? std::sin(e.data.bob) * 0.06f : 0.0f;

    const Mat4 model =
        Mat4::model(e.pos.x, e.pos.y + m.yOff + bob, e.pos.z, e.yaw, m.scale);
    gbufferProg_->setMat4("uModel", model.data());
    bonesFor(e, now, bones);
    gbufferProg_->setVec4Array("uBones", bones, 6);
    gbufferProg_->set("uSky", world.getSky(cx, cy, cz) / 15.0f);
    gbufferProg_->set("uBlock", world.getBlockLight(cx, cy, cz) / 15.0f);
    gbufferProg_->set("uTextured", m.textured ? 1.0f : 0.0f);
    gbufferProg_->setVec3("uTint", m.tint);

    glBindVertexArray(m.item ? m.item->vao : m.mesh->vao);
    glDrawArrays(GL_TRIANGLES, 0, m.item ? m.item->count : m.mesh->count);
  }
  glBindVertexArray(0);
}

void EntityRenderer::drawShadow(const world::World& world, const game::EntityManager& entities,
                                const float lightVP[16], double now) {
  (void)world;
  if (entities.all().empty() || !shadowProg_) return;

  shadowProg_->use();
  shadowProg_->setMat4("uLightVP", lightVP);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, atlas_ ? atlas_->texture() : 0);
  shadowProg_->set("uAtlas", 0);

  float bones[24];
  for (const game::Entity& e : entities.all()) {
    if (e.dead) continue;
    Model m;
    if (!modelFor(e, m)) continue;
    const float bob = e.type == game::EntityType::Drop ? std::sin(e.data.bob) * 0.06f : 0.0f;
    const Mat4 model =
        Mat4::model(e.pos.x, e.pos.y + m.yOff + bob, e.pos.z, e.yaw, m.scale);
    shadowProg_->setMat4("uModel", model.data());
    bonesFor(e, now, bones);
    shadowProg_->setVec4Array("uBones", bones, 6);
    shadowProg_->set("uCutout", m.textured ? 1.0f : 0.0f);
    glBindVertexArray(m.item ? m.item->vao : m.mesh->vao);
    glDrawArrays(GL_TRIANGLES, 0, m.item ? m.item->count : m.mesh->count);
  }
  glBindVertexArray(0);
}

}  // namespace hr::render
