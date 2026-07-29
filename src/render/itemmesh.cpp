#include "render/itemmesh.h"

#include <cmath>
#include <cstddef>

#include "world/shapes.h"

namespace hr::render {
namespace {

std::uint16_t quantUv(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 1.0f) return 65535;
  return static_cast<std::uint16_t>(std::lround(static_cast<double>(v) * 65535.0));
}

std::uint8_t quantShade(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 1.0f) return 255;
  return static_cast<std::uint8_t>(std::lround(static_cast<double>(v) * 255.0));
}

struct Corner {
  float x, y, z;
  float u, v;
};

// Four corners in ring order become two triangles, the same 0,1,2 / 0,2,3 winding
// the terrain mesher uses.
void emitQuad(std::vector<ItemVertex>& out, float shade, const Corner (&c)[4]) {
  static constexpr int kOrder[6] = {0, 1, 2, 0, 2, 3};
  const std::uint8_t s = quantShade(shade);
  for (int i : kOrder) {
    ItemVertex vert;
    vert.x = c[i].x;
    vert.y = c[i].y;
    vert.z = c[i].z;
    vert.u = quantUv(c[i].u);
    vert.v = quantUv(c[i].v);
    vert.shade = s;
    out.push_back(vert);
  }
}

// --- block display shapes ---------------------------------------------------
//
// The display boxes with the mesher's winding, shades and full-tile UVs — the
// world mesher stretches the tile over each sub-box face too, so a dropped stair
// matches a placed one. Centred by shifting x and z by -0.5.
std::vector<ItemVertex> buildShape(const ItemModel& model, const resource::Atlas& atlas) {
  std::vector<ItemVertex> out;
  const world::BlockDef& block = world::blocks().def(model.blockId);

  const auto quad = [&](int face, float shade, const float p0[3], const float p1[3],
                        const float p2[3], const float p3[3]) {
    const resource::TileRef& t = atlas.tile(block.faceTextures[face]);
    const float uv[4][2] = {{t.u0, t.v1}, {t.u1, t.v1}, {t.u1, t.v0}, {t.u0, t.v0}};
    const float* p[4] = {p0, p1, p2, p3};
    Corner c[4];
    for (int i = 0; i < 4; ++i) {
      c[i] = {p[i][0] - 0.5f, p[i][1], p[i][2] - 0.5f, uv[i][0], uv[i][1]};
    }
    emitQuad(out, shade, c);
  };

  for (const world::Box& box : world::displayBoxes(model.shape)) {
    const float x0 = box.x0, y0 = box.y0, z0 = box.z0;
    const float x1 = box.x1, y1 = box.y1, z1 = box.z1;
    const float pxA[3] = {x1, y0, z0}, pxB[3] = {x1, y0, z1};
    const float pxC[3] = {x1, y1, z1}, pxD[3] = {x1, y1, z0};
    quad(0, 0.68f, pxA, pxB, pxC, pxD);
    const float nxA[3] = {x0, y0, z1}, nxB[3] = {x0, y0, z0};
    const float nxC[3] = {x0, y1, z0}, nxD[3] = {x0, y1, z1};
    quad(1, 0.68f, nxA, nxB, nxC, nxD);
    const float pyA[3] = {x0, y1, z0}, pyB[3] = {x1, y1, z0};
    const float pyC[3] = {x1, y1, z1}, pyD[3] = {x0, y1, z1};
    quad(2, 1.00f, pyA, pyB, pyC, pyD);
    const float nyA[3] = {x0, y0, z1}, nyB[3] = {x1, y0, z1};
    const float nyC[3] = {x1, y0, z0}, nyD[3] = {x0, y0, z0};
    quad(3, 0.50f, nyA, nyB, nyC, nyD);
    const float pzA[3] = {x1, y0, z1}, pzB[3] = {x0, y0, z1};
    const float pzC[3] = {x0, y1, z1}, pzD[3] = {x1, y1, z1};
    quad(4, 0.85f, pzA, pzB, pzC, pzD);
    const float nzA[3] = {x0, y0, z0}, nzB[3] = {x1, y0, z0};
    const float nzC[3] = {x1, y1, z0}, nzD[3] = {x0, y1, z0};
    quad(5, 0.85f, nzA, nzB, nzC, nzD);
  }
  return out;
}

// --- extruded sprites -------------------------------------------------------

std::vector<ItemVertex> buildSprite(const ItemModel& model, const resource::Atlas& atlas) {
  std::vector<ItemVertex> out;
  const resource::TileRef& tile = atlas.tile(model.texture);
  const int rx = tile.x, ry = tile.y, tw = tile.w, th = tile.h;
  if (tw <= 0 || th <= 0) return out;

  const Image& pixels = atlas.image();
  const auto filled = [&](int px, int py) {
    return px >= 0 && px < tw && py >= 0 && py < th &&
           pixels.get(rx + px, ry + py).a >= game::kSpriteAlphaCutoff;
  };

  // One texel of thickness. The web build hardcoded 1/16 here while reading tw and
  // th from the tile rect (js/render/itemmesh.js:24), so at any tile resolution but
  // 16 the extruded models came out the wrong physical size — the exact one-way
  // door the resource-pack work exists to avoid, already present.
  const float T = 1.0f / static_cast<float>(tw);

  // Content bounds, so the model is centred on x and rests its lowest pixel on
  // y = 0: a dropped sword should not hover on its sprite's empty margin.
  int minX = tw, maxX = -1, maxY = -1;
  for (int py = 0; py < th; ++py) {
    for (int px = 0; px < tw; ++px) {
      if (!filled(px, py)) continue;
      if (px < minX) minX = px;
      if (px > maxX) maxX = px;
      if (py > maxY) maxY = py;
    }
  }
  if (maxX < 0) return out;  // a fully transparent tile

  const float shX = (static_cast<float>(minX + maxX + 1) / 2.0f) * T;  // centre -> x = 0
  const float shY = static_cast<float>(th - 1 - maxY) * T;             // lowest row -> y = 0
  const auto X = [&](int px) { return static_cast<float>(px) * T - shX; };
  const auto Y = [&](int py) { return static_cast<float>(th - py) * T - shY; };

  // Per-texel atlas coordinates for the edge walls. A hair of inset keeps run ends
  // from bleeding into the neighbouring tile under NEAREST sampling.
  const float W = static_cast<float>(atlas.width());
  const float H = static_cast<float>(atlas.height());
  constexpr float e = 0.02f;
  const auto U = [&](int px, bool end) { return (rx + px + (end ? -e : e)) / W; };
  const auto V = [&](int py, bool end) { return (ry + py + (end ? -e : e)) / H; };

  const float zF = T / 2.0f, zB = -T / 2.0f;
  const float xa = X(0), xb = X(tw), ya = Y(th), yb = Y(0);

  // Back plate, then the edge walls, then the front plate. Emission order no longer
  // matters for correctness — every consumer depth-tests — but keeping it back to
  // front costs nothing and helps early-z.
  //
  // The back plate uses the SAME uv-at-position mapping as the front: the alpha
  // holes of the two plates must line up exactly, or the back's art shows through
  // the front's transparent pixels as a ghosted mirror image. Seen from behind, the
  // sprite mirrors naturally, like a real extruded object.
  {
    const Corner c[4] = {{xa, ya, zB, tile.u0, tile.v1},
                         {xb, ya, zB, tile.u1, tile.v1},
                         {xb, yb, zB, tile.u1, tile.v0},
                         {xa, yb, zB, tile.u0, tile.v0}};
    emitQuad(out, 0.85f, c);
  }

  // Vertical walls. Wherever a filled texel borders an empty one (or the tile rim),
  // stand a one-texel-deep wall on that boundary, textured by the filled texel's own
  // column so the rim carries the sprite's colours. Adjacent boundary texels merge
  // into runs — one quad per run.
  for (int side = 0; side < 2; ++side) {
    const bool right = side == 1;
    for (int px = 0; px < tw; ++px) {
      const auto isEdge = [&](int py) {
        return filled(px, py) && !filled(right ? px + 1 : px - 1, py);
      };
      for (int py = 0; py < th; ++py) {
        if (!isEdge(py) || (py > 0 && isEdge(py - 1))) continue;
        int py1 = py;
        while (py1 + 1 < th && isEdge(py1 + 1)) ++py1;
        const float x = X(right ? px + 1 : px);
        const float y0 = Y(py1 + 1), y1 = Y(py);
        const float u = (rx + px + 0.5f) / W;
        const float v0 = V(py, false), v1 = V(py1 + 1, true);
        if (right) {
          const Corner c[4] = {
              {x, y0, zB, u, v1}, {x, y0, zF, u, v1}, {x, y1, zF, u, v0}, {x, y1, zB, u, v0}};
          emitQuad(out, 0.68f, c);
        } else {
          const Corner c[4] = {
              {x, y0, zF, u, v1}, {x, y0, zB, u, v1}, {x, y1, zB, u, v0}, {x, y1, zF, u, v0}};
          emitQuad(out, 0.68f, c);
        }
      }
    }
  }

  // Horizontal walls, the same idea a quarter-turn around.
  for (int side = 0; side < 2; ++side) {
    const bool top = side == 0;
    for (int py = 0; py < th; ++py) {
      const auto isEdge = [&](int px) {
        return filled(px, py) && !filled(px, top ? py - 1 : py + 1);
      };
      for (int px = 0; px < tw; ++px) {
        if (!isEdge(px) || (px > 0 && isEdge(px - 1))) continue;
        int px1 = px;
        while (px1 + 1 < tw && isEdge(px1 + 1)) ++px1;
        const float y = Y(top ? py : py + 1);
        const float x0 = X(px), x1 = X(px1 + 1);
        const float v = (ry + py + 0.5f) / H;
        const float u0 = U(px, false), u1 = U(px1 + 1, true);
        if (top) {
          const Corner c[4] = {
              {x0, y, zB, u0, v}, {x1, y, zB, u1, v}, {x1, y, zF, u1, v}, {x0, y, zF, u0, v}};
          emitQuad(out, 1.0f, c);
        } else {
          const Corner c[4] = {
              {x0, y, zF, u0, v}, {x1, y, zF, u1, v}, {x1, y, zB, u1, v}, {x0, y, zB, u0, v}};
          emitQuad(out, 0.5f, c);
        }
      }
    }
  }

  // Front plate: art upright and unmirrored for the viewer the face points at.
  {
    const Corner c[4] = {{xa, ya, zF, tile.u0, tile.v1},
                         {xb, ya, zF, tile.u1, tile.v1},
                         {xb, yb, zF, tile.u1, tile.v0},
                         {xa, yb, zF, tile.u0, tile.v0}};
    emitQuad(out, 0.85f, c);
  }
  return out;
}

}  // namespace

void bindItemAttributes() {
  using V = ItemVertex;
  const GLsizei stride = sizeof(V);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                        reinterpret_cast<const void*>(offsetof(V, x)));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_UNSIGNED_SHORT, GL_TRUE, stride,
                        reinterpret_cast<const void*>(offsetof(V, u)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 1, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                        reinterpret_cast<const void*>(offsetof(V, shade)));
  // Not normalised: the bone index is a whole number the skinning path indexes with.
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride,
                        reinterpret_cast<const void*>(offsetof(V, bone)));
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                        reinterpret_cast<const void*>(offsetof(V, r)));
}

std::vector<ItemVertex> buildItemMesh(const ItemModel& model, const resource::Atlas& atlas) {
  switch (model.kind) {
    case ItemModelKind::Shape: return buildShape(model, atlas);
    case ItemModelKind::Sprite: return buildSprite(model, atlas);
    case ItemModelKind::None: break;
  }
  return {};
}

const ItemMesh* ItemMeshCache::get(const std::string& key) {
  auto it = meshes_.find(key);
  if (it != meshes_.end()) return it->second.valid() ? &it->second : nullptr;

  ItemMesh mesh;
  if (atlas_) {
    const ItemModel model = itemModelFor(key);
    const std::vector<ItemVertex> verts = buildItemMesh(model, *atlas_);
    if (!verts.empty()) {
      mesh.kind = model.kind;
      mesh.shape = model.shape;
      mesh.count = static_cast<GLsizei>(verts.size());
      glGenVertexArrays(1, &mesh.vao);
      glGenBuffers(1, &mesh.vbo);
      glBindVertexArray(mesh.vao);
      glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
      glBufferData(GL_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(verts.size() * sizeof(ItemVertex)), verts.data(),
                   GL_STATIC_DRAW);
      bindItemAttributes();
      glBindVertexArray(0);
    }
  }
  // Cached either way, so a key with no art is not rebuilt every frame.
  auto [inserted, ok] = meshes_.emplace(key, mesh);
  (void)ok;
  return inserted->second.valid() ? &inserted->second : nullptr;
}

void ItemMeshCache::clear() {
  for (auto& [key, mesh] : meshes_) {
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
  }
  meshes_.clear();
}

}  // namespace hr::render
