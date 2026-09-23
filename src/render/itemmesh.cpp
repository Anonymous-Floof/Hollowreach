#include "render/itemmesh.h"

#include <algorithm>
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

/// Four corners in ring order become two triangles, the same 0,1,2 / 0,2,3 winding
// the terrain mesher uses.
//
// `dyed` goes into the vertex alpha, which the entity and viewmodel shaders read as
// "does the stack's dye reach this surface". A dropped or held bed is one cached
// mesh multiplied by one colour; the mask is what keeps that colour on the mattress
// and off the frame, the same split the world mesher makes with two tints.
void emitQuad(std::vector<ItemVertex>& out, float shade, const Corner (&c)[4],
              bool dyed = true) {
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
    vert.a = dyed ? 255 : 0;
    out.push_back(vert);
  }
}

// --- block display shapes ---------------------------------------------------
//
// The display boxes with the mesher's winding, shades and position-based UVs
// (world::faceUv), so a dropped stair matches a placed one texel for texel.
// Centred by shifting x and z by -0.5.
std::vector<ItemVertex> buildShape(const ItemModel& model, const resource::Atlas& atlas) {
  std::vector<ItemVertex> out;
  const world::BlockDef& block = world::blocks().def(model.blockId);

  std::uint8_t part = 0;
  const auto quad = [&](int face, float shade, const float p0[3], const float p1[3],
                        const float p2[3], const float p3[3]) {
    const bool ownFaces = part == 0 || part > block.parts.size();
    const resource::TileRef& t =
        atlas.tile(ownFaces ? block.faceTextures[face] : block.parts[part - 1].texture);
    const float* p[4] = {p0, p1, p2, p3};
    Corner c[4];
    for (int i = 0; i < 4; ++i) {
      float fu = 0, fv = 0;
      world::faceUv(face, p[i][0], p[i][1], p[i][2], fu, fv);
      c[i] = {p[i][0] - 0.5f, p[i][1], p[i][2] - 0.5f, t.u0 + fu * (t.u1 - t.u0),
              t.v0 + fv * (t.v1 - t.v0)};
    }
    emitQuad(out, shade, c, ownFaces || block.parts[part - 1].dyed);
  };

  for (const world::Box& box : world::displayBoxes(model.shape)) {
    part = box.part;
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

  // The art, top to bottom. One tile for nearly everything; a door stacks its upper
  // half over its lower half and is extruded as one picture twice as tall.
  std::vector<const resource::TileRef*> stack {&atlas.tile(model.texture)};
  if (!model.textureBelow.empty()) stack.push_back(&atlas.tile(model.textureBelow));
  const int tw = stack[0]->w, tileH = stack[0]->h;
  if (tw <= 0 || tileH <= 0) return out;
  for (const resource::TileRef* t : stack) {
    if (t->w != tw || t->h != tileH) return out;  // tiles of different sizes cannot stack
  }
  const int th = tileH * static_cast<int>(stack.size());

  // Where art pixel (px, py) lives in the atlas image.
  const auto atlasX = [&](int px) { return stack[0]->x + px; };
  const auto atlasY = [&](int py) {
    return stack[static_cast<std::size_t>(py / tileH)]->y + py % tileH;
  };

  const Image& pixels = atlas.image();
  const auto filled = [&](int px, int py) {
    return px >= 0 && px < tw && py >= 0 && py < th &&
           pixels.get(atlasX(px), atlasY(py)).a >= game::kSpriteAlphaCutoff;
  };

  // One texel of thickness, and the art scaled so its LONGER side is one unit: a
  // 16x16 sprite is a block wide as it always was, and a two-tile door is a block
  // tall rather than two. The web build hardcoded 1/16 here while reading tw and
  // th from the tile rect (js/render/itemmesh.js:24), so at any tile resolution but
  // 16 the extruded models came out the wrong physical size.
  const float T = 1.0f / static_cast<float>(std::max(tw, th));

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
  // from bleeding into the neighbouring texel under NEAREST sampling. A vertical run
  // never crosses from one stacked tile into the next (see the walls below), so its
  // two ends can be looked up independently.
  const float W = static_cast<float>(atlas.width());
  const float H = static_cast<float>(atlas.height());
  constexpr float e = 0.02f;
  const auto U = [&](int px, bool end) { return (atlasX(px) + (end ? 1 - e : e)) / W; };
  const auto Vtop = [&](int py) { return (atlasY(py) + e) / H; };
  const auto Vbottom = [&](int py) { return (atlasY(py) + 1 - e) / H; };

  const float zF = T / 2.0f, zB = -T / 2.0f;
  const float xa = X(0), xb = X(tw);

  // The plates: one quad per stacked tile, front and back. Each spans its tile's
  // exact rect, so a texel boundary on the plate IS a texel boundary on the walls.
  // With the half-texel inset TileRef used to carry they disagreed by up to half a
  // texel, and the art hung off one side of its own silhouette and fell short of
  // the other — a gap you could see the ground through along every edge.
  //
  // The back plate uses the SAME uv-at-position mapping as the front: the alpha
  // holes of the two plates must line up exactly, or the back's art shows through
  // the front's transparent pixels as a ghosted mirror image. Seen from behind, the
  // sprite mirrors naturally, like a real extruded object.
  const auto plate = [&](float z, float shade) {
    for (std::size_t k = 0; k < stack.size(); ++k) {
      const resource::TileRef& tile = *stack[k];
      const float ya = Y(static_cast<int>(k + 1) * tileH);
      const float yb = Y(static_cast<int>(k) * tileH);
      const Corner c[4] = {{xa, ya, z, tile.u0, tile.v1},
                           {xb, ya, z, tile.u1, tile.v1},
                           {xb, yb, z, tile.u1, tile.v0},
                           {xa, yb, z, tile.u0, tile.v0}};
      emitQuad(out, shade, c);
    }
  };

  // Back plate, then the edge walls, then the front plate. Emission order no longer
  // matters for correctness — every consumer depth-tests — but keeping it back to
  // front costs nothing and helps early-z.
  plate(zB, 0.85f);

  // Vertical walls. Wherever a filled texel borders an empty one (or the art's rim),
  // stand a one-texel-deep wall on that boundary, textured by the filled texel's own
  // column so the rim carries the sprite's colours. Adjacent boundary texels merge
  // into runs — one quad per run, broken where the art passes into the next tile.
  for (int side = 0; side < 2; ++side) {
    const bool right = side == 1;
    for (int px = 0; px < tw; ++px) {
      const auto isEdge = [&](int py) {
        return filled(px, py) && !filled(right ? px + 1 : px - 1, py);
      };
      for (int py = 0; py < th; ++py) {
        if (!isEdge(py) || (py % tileH != 0 && isEdge(py - 1))) continue;
        int py1 = py;
        while (py1 + 1 < th && (py1 + 1) % tileH != 0 && isEdge(py1 + 1)) ++py1;
        const float x = X(right ? px + 1 : px);
        const float y0 = Y(py1 + 1), y1 = Y(py);
        const float u = (atlasX(px) + 0.5f) / W;
        const float v0 = Vtop(py), v1 = Vbottom(py1);
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
        const float v = (atlasY(py) + 0.5f) / H;
        const float u0 = U(px, false), u1 = U(px1, true);
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
  plate(zF, 0.85f);
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
