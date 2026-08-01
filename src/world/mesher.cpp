#include "world/mesher.h"

#include <cmath>

#include "core/prng.h"
#include "world/shapes.h"

namespace hr::world {
namespace {

// Face index: 0:+x 1:-x 2:+y 3:-y 4:+z 5:-z, matching the resolved face textures.
struct FaceSpec {
  int dir[3];
  int normalAxis;
  int sign;
  int tangentAxis;
  int bitangentAxis;
  double shade;
};

// Face brightness. A flat directional term baked into the vertex, which is why the
// world still reads as lit before any dynamic lighting is applied.
const FaceSpec kFaces[6] = {
    {{1, 0, 0}, 0, 1, 2, 1, 0.68f},
    {{-1, 0, 0}, 0, -1, 2, 1, 0.68f},
    {{0, 1, 0}, 1, 1, 0, 2, 1.00f},
    {{0, -1, 0}, 1, -1, 0, 2, 0.50f},
    {{0, 0, 1}, 2, 1, 0, 1, 0.85f},
    {{0, 0, -1}, 2, -1, 0, 1, 0.85f},
};

// The classic four-level corner darkening.
constexpr double kAoLevels[4] = {0.5, 0.7, 0.86, 1.0};

// Surface height of a still or source water block: a touch below the cell top, so
// it reads as a liquid with a meniscus rather than a solid cube face.
constexpr float kWaterFull = 0.875f;

// Top-face UV rotation for a bed by facing (0:+x 1:-x 2:+z 3:-z) so the pillow end
// always points the way the bed is laid.
constexpr int kBedTopRot[4] = {1, 3, 0, 2};

std::uint8_t quant(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 1.0f) return 255;
  return static_cast<std::uint8_t>(std::lround(static_cast<double>(v) * 255.0));
}

std::uint16_t quantUv(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 1.0f) return 65535;
  return static_cast<std::uint16_t>(std::lround(static_cast<double>(v) * 65535.0));
}

// One corner of a quad, before packing.
struct Corner {
  float x = 0, y = 0, z = 0;
  float u = 0, v = 0;
  float shade = 1.0f;
  float sky = 0.0f, block = 0.0f;
  std::uint8_t wave = kWaveNone;
};

// The single vertex-emitting path. Corners arrive in ring order, and the two
// triangles are (0,1,2) and (0,2,3) — which is exactly the JS's [0,1,3,0,3,2] once
// its (i, j) grid corners are reordered into a ring, so the AO interpolation across
// the diagonal is unchanged.
void emitQuad(std::vector<TerrainVertex>& out, const Corner c[4], resource::Rgb8 tint) {
  static constexpr int kOrder[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; ++i) {
    const Corner& k = c[kOrder[i]];
    TerrainVertex vert;
    vert.x = k.x;
    vert.y = k.y;
    vert.z = k.z;
    vert.u = quantUv(k.u);
    vert.v = quantUv(k.v);
    vert.shade = quant(k.shade);
    vert.sky = quant(k.sky);
    vert.block = quant(k.block);
    vert.flags = k.wave;
    vert.tintR = tint.r;
    vert.tintG = tint.g;
    vert.tintB = tint.b;
    vert.tintA = 255;
    out.push_back(vert);
  }
}

// Reads the 3x3 neighbourhood. Coordinates are chunk-local and may be -1 or 16 on
// x and z; y is clamped with the same out-of-range answers the JS gave.
class Sampler {
 public:
  explicit Sampler(const MeshNeighbourhood& nb) : nb_(nb) {}

  const ChunkData* pick(int lx, int lz) const {
    int gx = 1, gz = 1;
    if (lx < 0) gx = 0;
    else if (lx >= CX) gx = 2;
    if (lz < 0) gz = 0;
    else if (lz >= CZ) gz = 2;
    return nb_.grid[gz * 3 + gx];
  }

  // (lx + CX) & 15 is the JS's wrap for the -1..CX range these see; it depends on
  // CX == CZ == 16.
  static int wrap(int v) { return (v + CX) & 15; }

  BlockId voxel(int lx, int ly, int lz) const {
    if (ly < 0) return bedrock_;
    if (ly >= WH) return kAir;
    const ChunkData* c = pick(lx, lz);
    return c ? c->voxels[localIdx(wrap(lx), ly, wrap(lz))] : kAir;
  }
  int sky(int lx, int ly, int lz) const {
    if (ly < 0) return 0;
    if (ly >= WH) return 15;
    const ChunkData* c = pick(lx, lz);
    return c ? c->skylight[localIdx(wrap(lx), ly, wrap(lz))] : 15;
  }
  int blockLight(int lx, int ly, int lz) const {
    if (ly < 0 || ly >= WH) return 0;
    const ChunkData* c = pick(lx, lz);
    return c ? c->blocklight[localIdx(wrap(lx), ly, wrap(lz))] : 0;
  }
  int meta(int lx, int ly, int lz) const {
    if (ly < 0 || ly >= WH) return 0;
    const ChunkData* c = pick(lx, lz);
    return c ? c->meta[localIdx(wrap(lx), ly, wrap(lz))] : 0;
  }
  bool opaque(int lx, int ly, int lz) const { return reg_.opaque(voxel(lx, ly, lz)); }
  bool isWater(int lx, int ly, int lz) const { return voxel(lx, ly, lz) == water_; }

 private:
  const MeshNeighbourhood& nb_;
  const BlockRegistry& reg_ = blocks();
  BlockId bedrock_ = wk().bedrock;
  BlockId water_ = wk().water;
};

bool faceVisible(BlockId self, BlockId neighbour) {
  if (neighbour == kAir) return true;
  if (blocks().opaque(neighbour)) return false;  // fully hidden
  // Same transparent block merges, so a body of glass or leaves has no internal
  // faces. Note this is not Minecraft's rule (it uses a per-block predicate); keep
  // ours, but it stays out of the model data so a pack cannot depend on it.
  return neighbour != self;
}

}  // namespace

// --- BlockTileTable ----------------------------------------------------------

void BlockTileTable::build(const resource::Atlas& atlas) {
  const BlockRegistry& reg = blocks();
  const std::size_t n = reg.count();
  faces_.assign(n * 6, {});
  foot_.assign(n, {});
  stem_.assign(n, {});
  hasStem_.assign(n, 0);

  for (const BlockDef& def : reg.all()) {
    for (int face = 0; face < 6; ++face) {
      faces_[static_cast<std::size_t>(def.id) * 6 + face] = atlas.tile(def.faceTextures[face]);
    }
    if (!def.footTexture.empty()) foot_[def.id] = atlas.tile(def.footTexture);
    if (!def.stemTexture.empty()) {
      stem_[def.id] = atlas.tile(def.stemTexture);
      hasStem_[def.id] = 1;
    }
  }
}

// --- the mesher --------------------------------------------------------------

MeshResult meshChunk(const MeshNeighbourhood& nb, const BlockTileTable& tiles,
                     const resource::ClimateTile* climate) {
  MeshResult result;
  const ChunkData* centre = nb.centre();
  if (!centre) return result;

  // Roughly what a surface chunk produces, to avoid a dozen reallocations.
  result.opaque.reserve(8192);

  const BlockRegistry& reg = blocks();
  const Sampler s(nb);
  const int baseX = nb.cx * CX;
  const int baseZ = nb.cz * CZ;

  // Water surface height of a cell, or -1 when it is not water. A cell with water
  // above renders as a full column so the body has no internal seam; a source or
  // falling cell sits at kWaterFull; a flowing cell drops toward a thin film.
  // In double, narrowed once at the vertex: a flowing cell's height is (8-level)/9,
  // which is not representable, so accumulating the corner average in float would
  // round twice and drift from the original by an ULP.
  auto fluidH = [&](int lx, int ly, int lz) -> double {
    if (!s.isWater(lx, ly, lz)) return -1.0;
    if (s.isWater(lx, ly + 1, lz)) return 1.0;
    const int m = s.meta(lx, ly, lz);
    if (m == 0 || (m & 8)) return kWaterFull;
    return static_cast<double>(8 - (m & 7)) / 9.0;
  };
  // A top corner's height is the average of the up-to-four water cells meeting at
  // it. Land cells are skipped, so the surface droops toward shores.
  auto cornerH = [&](int lx, int ly, int lz, int sx, int sz) -> double {
    double sum = 0;
    int count = 0;
    const double samples[4] = {fluidH(lx, ly, lz), fluidH(lx + sx, ly, lz),
                               fluidH(lx, ly, lz + sz), fluidH(lx + sx, ly, lz + sz)};
    for (double v : samples) {
      if (v >= 0) {
        sum += v;
        ++count;
      }
    }
    return count ? sum / count : kWaterFull;
  };

  // Section bookkeeping: y is the outermost loop, so everything emitted while y
  // is inside a section belongs to that section and the ranges are contiguous by
  // construction. Recorded as we go rather than sorted afterwards.
  int section = -1;
  for (int y = 0; y < WH; ++y) {
    const int sec = y / kSectionHeight;
    if (sec != section) {
      section = sec;
      result.opaqueSections.first[sec] = static_cast<std::uint32_t>(result.opaque.size());
      result.waterSections.first[sec] = static_cast<std::uint32_t>(result.water.size());
    }
    for (int z = 0; z < CZ; ++z) {
      for (int x = 0; x < CX; ++x) {
        const BlockId id = centre->voxels[localIdx(x, y, z)];
        if (id == kAir) continue;

        const BlockDef& def = reg.def(id);
        const float wx = static_cast<float>(baseX + x);
        const float wy = static_cast<float>(y);
        const float wz = static_cast<float>(baseZ + z);
        const int worldX = baseX + x;
        const int worldZ = baseZ + z;

        const resource::Rgb8 tint =
            def.tintIndex == resource::kNoTint
                ? resource::kWhite
                : resource::tintFor(def.tintIndex,
                                    climate ? climate->at(x, z) : Biome::Meadow);

        const float cellSky = s.sky(x, y, z) / 15.0;
        const float cellBlock = s.blockLight(x, y, z) / 15.0;

        // ---- cross billboards: torches and plants ---------------------------
        if (def.render == RenderKind::Cross) {
          if (def.isPlant) {
            // A stacking plant (papyrus) uses the plain stem tile for segments with
            // more of itself above, so only the top shows the tufted crown.
            const resource::TileRef* tile = &tiles.face(id, 0);
            if (tiles.hasStem(id) && y + 1 < WH &&
                centre->voxels[localIdx(x, y + 1, z)] == id) {
              tile = &tiles.stem(id);
            }
            const double H = def.plantHeight > 0 ? def.plantHeight : 0.9;
            const double r = def.plantRadius > 0 ? def.plantRadius : 0.45;

            // Deterministic per-cell jitter, so a meadow is not grid-stamped. Not a
            // vanilla behaviour: Minecraft offsets some plants and never rotates
            // them, so a future model loader must be able to opt out of this.
            //
            // Computed in double and narrowed once at the corner, matching how the
            // JS evaluated these expressions before storing into a Float32Array.
            // Doing the arithmetic in float instead rounds twice, and the resulting
            // one-ULP drift is enough to fail the mesh golden-vector diff.
            const double ang = hash2i(0x91b7u, worldX, worldZ) * 1.5707963;
            const double ox = (hash2i(0x33a1u, worldX, worldZ) - 0.5) * 0.22;
            const double oz = (hash2i(0x77c5u, worldX, worldZ) - 0.5) * 0.22;
            const double cxp = wx + 0.5 + ox;
            const double czp = wz + 0.5 + oz;
            const double y0 = wy, y1 = wy + H;
            const double ca = std::cos(ang) * r;
            const double sa = std::sin(ang) * r;

            // The base verts stay static and the top verts carry the sway flag, so
            // a tuft bends from the top while staying rooted.
            auto billboard = [&](double dx, double dz) {
              const float bx0 = static_cast<float>(cxp - dx);
              const float bx1 = static_cast<float>(cxp + dx);
              const float bz0 = static_cast<float>(czp - dz);
              const float bz1 = static_cast<float>(czp + dz);
              const float fy0 = static_cast<float>(y0);
              const float fy1 = static_cast<float>(y1);
              Corner c[4];
              c[0] = {bx0, fy0, bz0, tile->u0, tile->v1, 1.0f, cellSky, cellBlock, kWaveNone};
              c[1] = {bx1, fy0, bz1, tile->u1, tile->v1, 1.0f, cellSky, cellBlock, kWaveNone};
              c[2] = {bx1, fy1, bz1, tile->u1, tile->v0, 1.0f, cellSky, cellBlock, kWaveLeaf};
              c[3] = {bx0, fy1, bz0, tile->u0, tile->v0, 1.0f, cellSky, cellBlock, kWaveLeaf};
              emitQuad(result.opaque, c, tint);
            };
            billboard(ca, sa);
            billboard(-sa, ca);
          } else {
            // Torch: a thin post as two crossed billboards running base to top.
            // Positions in double for the same single-rounding reason as plants.
            const resource::TileRef& tile = tiles.face(id, 0);
            constexpr double hw = 0.09, H = 0.625;
            double bx = wx + 0.5, bz = wz + 0.5, by = wy;
            double tx = wx + 0.5, tz = wz + 0.5, ty = wy + H;
            int dx = 0, dz = 0;
            if (crossMountDir(centre->meta[localIdx(x, y, z)], dx, dz)) {
              by = wy + 0.18;                 // sit up the wall a little
              bx = wx + 0.5 - dx * 0.42;      // base against the wall
              bz = wz + 0.5 - dz * 0.42;
              tx = wx + 0.5 + dx * 0.18;      // top leans into the room
              tz = wz + 0.5 + dz * 0.18;
              ty = by + H;
            }
            auto plane = [&](double ox, double oz) {
              Corner c[4];
              c[0] = {static_cast<float>(bx - ox), static_cast<float>(by),
                      static_cast<float>(bz - oz), tile.u0, tile.v1, 1.0f, cellSky, cellBlock};
              c[1] = {static_cast<float>(bx + ox), static_cast<float>(by),
                      static_cast<float>(bz + oz), tile.u1, tile.v1, 1.0f, cellSky, cellBlock};
              c[2] = {static_cast<float>(tx + ox), static_cast<float>(ty),
                      static_cast<float>(tz + oz), tile.u1, tile.v0, 1.0f, cellSky, cellBlock};
              c[3] = {static_cast<float>(tx - ox), static_cast<float>(ty),
                      static_cast<float>(tz - oz), tile.u0, tile.v0, 1.0f, cellSky, cellBlock};
              emitQuad(result.opaque, c, tint);
            };
            plane(hw, 0);  // faces +/-z
            plane(0, hw);  // faces +/-x
          }
          continue;
        }

        // ---- shaped blocks: each sub-box as a small textured cuboid ----------
        if (isShaped(def.render)) {
          const int meta = centre->meta[localIdx(x, y, z)];
          const std::vector<Box> boxes = renderBoxes(def.render, meta);
          // The bed's top texture turns with the block so the pillow stays at the
          // head end; its foot cell shows the blanket instead of the pillow.
          const int bedRot = def.render == RenderKind::Bed ? kBedTopRot[meta & 3] : 0;
          const bool bedFoot = def.render == RenderKind::Bed && !(meta & 4);

          auto boxQuad = [&](int faceDir, double shade, const float p0[3], const float p1[3],
                             const float p2[3], const float p3[3]) {
            const resource::TileRef& tile =
                (bedFoot && faceDir == 2) ? tiles.foot(id) : tiles.face(id, faceDir);
            float uv[4][2] = {{tile.u0, tile.v1},
                              {tile.u1, tile.v1},
                              {tile.u1, tile.v0},
                              {tile.u0, tile.v0}};
            if (faceDir == 2 && bedRot) {
              float rotated[4][2];
              for (int i = 0; i < 4; ++i) {
                rotated[i][0] = uv[(bedRot + i) & 3][0];
                rotated[i][1] = uv[(bedRot + i) & 3][1];
              }
              for (int i = 0; i < 4; ++i) {
                uv[i][0] = rotated[i][0];
                uv[i][1] = rotated[i][1];
              }
            }
            const float* p[4] = {p0, p1, p2, p3};
            Corner c[4];
            for (int i = 0; i < 4; ++i) {
              // Light is one flat sample from the block's own cell, so shaped
              // blocks get no ambient occlusion — matching the original.
              c[i] = {p[i][0], p[i][1], p[i][2], uv[i][0], uv[i][1],
                      static_cast<float>(shade), cellSky, cellBlock};
            }
            emitQuad(result.opaque, c, tint);
          };

          for (const Box& b : boxes) {
            const float x0 = wx + b.x0, y0 = wy + b.y0, z0 = wz + b.z0;
            const float x1 = wx + b.x1, y1 = wy + b.y1, z1 = wz + b.z1;
            // A sub-box face is culled only when it is flush with the cell boundary
            // *and* the neighbour there is opaque.
            const float A[3] = {x1, y0, z0}, Bp[3] = {x1, y0, z1}, C[3] = {x1, y1, z1},
                        D[3] = {x1, y1, z0};
            if (!(b.x1 == 1.0f && s.opaque(x + 1, y, z))) boxQuad(0, 0.68f, A, Bp, C, D);
            const float E[3] = {x0, y0, z1}, F[3] = {x0, y0, z0}, G[3] = {x0, y1, z0},
                        H[3] = {x0, y1, z1};
            if (!(b.x0 == 0.0f && s.opaque(x - 1, y, z))) boxQuad(1, 0.68f, E, F, G, H);
            const float I[3] = {x0, y1, z0}, J[3] = {x1, y1, z0}, K[3] = {x1, y1, z1},
                        L[3] = {x0, y1, z1};
            if (!(b.y1 == 1.0f && s.opaque(x, y + 1, z))) boxQuad(2, 1.00f, I, J, K, L);
            const float M[3] = {x0, y0, z1}, N[3] = {x1, y0, z1}, O[3] = {x1, y0, z0},
                        P[3] = {x0, y0, z0};
            if (!(b.y0 == 0.0f && s.opaque(x, y - 1, z))) boxQuad(3, 0.50f, M, N, O, P);
            const float Q[3] = {x1, y0, z1}, R[3] = {x0, y0, z1}, S[3] = {x0, y1, z1},
                        Tc[3] = {x1, y1, z1};
            if (!(b.z1 == 1.0f && s.opaque(x, y, z + 1))) boxQuad(4, 0.85f, Q, R, S, Tc);
            const float U[3] = {x0, y0, z0}, V[3] = {x1, y0, z0}, W2[3] = {x1, y1, z0},
                        X2[3] = {x0, y1, z0};
            if (!(b.z0 == 0.0f && s.opaque(x, y, z - 1))) boxQuad(5, 0.85f, U, V, W2, X2);
          }
          continue;
        }

        // ---- water: a variable-height liquid --------------------------------
        if (def.render == RenderKind::Liquid) {
          const resource::TileRef& tile = tiles.face(id, 0);
          const float h00 = static_cast<float>(wy + cornerH(x, y, z, -1, -1));
          const float h10 = static_cast<float>(wy + cornerH(x, y, z, +1, -1));
          const float h01 = static_cast<float>(wy + cornerH(x, y, z, -1, +1));
          const float h11 = static_cast<float>(wy + cornerH(x, y, z, +1, +1));
          const float X0 = wx, X1 = wx + 1, Z0 = wz, Z1 = wz + 1;
          const bool submerged = s.isWater(x, y + 1, z);

          // wA is the wave flag for the first edge, wB for the second: a side passes
          // wA = 0 for its static floor edge and wB = 2 so its top edge ripples in
          // lockstep with the surface and the two never pull apart.
          auto waterQuad = [&](const float p0[3], const float p1[3], const float p2[3],
                               const float p3[3], double shade, double sk, double bl,
                               std::uint8_t wA, std::uint8_t wB) {
            const float* p[4] = {p0, p1, p2, p3};
            const float u[4] = {tile.u0, tile.u1, tile.u1, tile.u0};
            const float v[4] = {tile.v1, tile.v1, tile.v0, tile.v0};
            const std::uint8_t waves[4] = {wA, wA, wB, wB};
            Corner c[4];
            for (int i = 0; i < 4; ++i) {
              c[i] = {p[i][0], p[i][1], p[i][2], u[i],
                      v[i],      static_cast<float>(shade), static_cast<float>(sk),
                      static_cast<float>(bl), waves[i]};
            }
            emitQuad(result.water, c, tint);
          };

          if (!submerged) {
            // Light comes from the cell above, unless that cell is solid — water
            // under a cave ceiling, whose light is 0 and used to paint the surface
            // black. Fall back to the water cell's own light, which the flood fed
            // from the sides.
            const bool roofed = s.opaque(x, y + 1, z);
            const float sk = (roofed ? s.sky(x, y, z) : s.sky(x, y + 1, z)) / 15.0;
            const float bl =
                (roofed ? s.blockLight(x, y, z) : s.blockLight(x, y + 1, z)) / 15.0;
            const float p0[3] = {X0, h00, Z0}, p1[3] = {X1, h10, Z0}, p2[3] = {X1, h11, Z1},
                        p3[3] = {X0, h01, Z1};
            waterQuad(p0, p1, p2, p3, 1.0f, sk, bl, kWaveWater, kWaveWater);
          }

          // A submerged column keeps a static full-height edge so deep walls tile
          // seamlessly; only the actual surface cell ripples.
          const std::uint8_t topWave = submerged ? kWaveNone : kWaveWater;
          auto sideOpen = [&](int ax, int ay, int az) {
            return !s.isWater(ax, ay, az) && !s.opaque(ax, ay, az);
          };

          if (sideOpen(x + 1, y, z)) {
            const float sk = s.sky(x + 1, y, z) / 15.0, bl = s.blockLight(x + 1, y, z) / 15.0;
            const float p0[3] = {X1, wy, Z0}, p1[3] = {X1, wy, Z1}, p2[3] = {X1, h11, Z1},
                        p3[3] = {X1, h10, Z0};
            waterQuad(p0, p1, p2, p3, 0.68f, sk, bl, kWaveNone, topWave);
          }
          if (sideOpen(x - 1, y, z)) {
            const float sk = s.sky(x - 1, y, z) / 15.0, bl = s.blockLight(x - 1, y, z) / 15.0;
            const float p0[3] = {X0, wy, Z1}, p1[3] = {X0, wy, Z0}, p2[3] = {X0, h00, Z0},
                        p3[3] = {X0, h01, Z1};
            waterQuad(p0, p1, p2, p3, 0.68f, sk, bl, kWaveNone, topWave);
          }
          if (sideOpen(x, y, z + 1)) {
            const float sk = s.sky(x, y, z + 1) / 15.0, bl = s.blockLight(x, y, z + 1) / 15.0;
            const float p0[3] = {X1, wy, Z1}, p1[3] = {X0, wy, Z1}, p2[3] = {X0, h01, Z1},
                        p3[3] = {X1, h11, Z1};
            waterQuad(p0, p1, p2, p3, 0.85f, sk, bl, kWaveNone, topWave);
          }
          if (sideOpen(x, y, z - 1)) {
            const float sk = s.sky(x, y, z - 1) / 15.0, bl = s.blockLight(x, y, z - 1) / 15.0;
            const float p0[3] = {X0, wy, Z0}, p1[3] = {X1, wy, Z0}, p2[3] = {X1, h10, Z0},
                        p3[3] = {X0, h00, Z0};
            waterQuad(p0, p1, p2, p3, 0.85f, sk, bl, kWaveNone, topWave);
          }
          // Bottom only under an overhang: flat and static.
          if (sideOpen(x, y - 1, z)) {
            const float sk = s.sky(x, y - 1, z) / 15.0, bl = s.blockLight(x, y - 1, z) / 15.0;
            const float p0[3] = {X0, wy, Z1}, p1[3] = {X1, wy, Z1}, p2[3] = {X1, wy, Z0},
                        p3[3] = {X0, wy, Z0};
            waterQuad(p0, p1, p2, p3, 0.5f, sk, bl, kWaveNone, kWaveNone);
          }
          continue;
        }

        // ---- the cube path ---------------------------------------------------
        for (int fi = 0; fi < 6; ++fi) {
          const FaceSpec& f = kFaces[fi];
          const int alx = x + f.dir[0], aly = y + f.dir[1], alz = z + f.dir[2];
          if (!faceVisible(id, s.voxel(alx, aly, alz))) continue;

          const resource::TileRef& tile = tiles.face(id, fi);
          // Leaves sway as a whole. The original also had a liquid case here, but
          // liquids never reach this path — they are routed to the water emitter
          // above — so it was vestigial.
          const std::uint8_t wave = def.isLeaf ? kWaveLeaf : kWaveNone;

          const int dt[3] = {f.tangentAxis == 0, f.tangentAxis == 1, f.tangentAxis == 2};
          const int db[3] = {f.bitangentAxis == 0, f.bitangentAxis == 1, f.bitangentAxis == 2};
          const int nOff = f.sign > 0 ? 1 : 0;

          // The face looks into one air cell, which always contributes light.
          const float skyC = static_cast<float>(s.sky(alx, aly, alz));
          const float blkC = static_cast<float>(s.blockLight(alx, aly, alz));

          // Per-corner attributes, i along the tangent and j along the bitangent.
          // Light is smoothed: each vertex averages the non-occluded cells touching
          // that corner on the air side, which is the classic "smooth lighting".
          Corner grid[4];
          int k = 0;
          for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
              float px = wx, py = wy, pz = wz;
              if (f.normalAxis == 0) px += nOff;
              else if (f.normalAxis == 1) py += nOff;
              else pz += nOff;
              px += dt[0] * i + db[0] * j;
              py += dt[1] * i + db[1] * j;
              pz += dt[2] * i + db[2] * j;

              const int si = 2 * i - 1, sj = 2 * j - 1;
              // The three neighbour cells sharing this corner, on the air side.
              const int ix = alx + dt[0] * si, iy = aly + dt[1] * si, iz = alz + dt[2] * si;
              const int jx = alx + db[0] * sj, jy = aly + db[1] * sj, jz = alz + db[2] * sj;
              const int dx = alx + dt[0] * si + db[0] * sj;
              const int dy = aly + dt[1] * si + db[1] * sj;
              const int dz = alz + dt[2] * si + db[2] * sj;

              const int su = s.opaque(ix, iy, iz) ? 1 : 0;
              const int sv = s.opaque(jx, jy, jz) ? 1 : 0;
              const int sc = s.opaque(dx, dy, dz) ? 1 : 0;
              const int ao = (su && sv) ? 0 : 3 - (su + sv + sc);

              // Average over the air cell plus each non-opaque neighbour; the
              // diagonal only counts when it is not sealed off by both edges.
              double skySum = skyC, blkSum = blkC;
              int count = 1;
              if (!su) {
                skySum += s.sky(ix, iy, iz);
                blkSum += s.blockLight(ix, iy, iz);
                ++count;
              }
              if (!sv) {
                skySum += s.sky(jx, jy, jz);
                blkSum += s.blockLight(jx, jy, jz);
                ++count;
              }
              if (!sc && !(su && sv)) {
                skySum += s.sky(dx, dy, dz);
                blkSum += s.blockLight(dx, dy, dz);
                ++count;
              }

              grid[k].x = px;
              grid[k].y = py;
              grid[k].z = pz;
              grid[k].u = i ? tile.u1 : tile.u0;
              grid[k].v = j ? tile.v0 : tile.v1;
              grid[k].shade = static_cast<float>(f.shade * kAoLevels[ao]);
              grid[k].sky = static_cast<float>(skySum / count / 15.0);
              grid[k].block = static_cast<float>(blkSum / count / 15.0);
              grid[k].wave = wave;
              ++k;
            }
          }

          // Grid corners are [i0j0, i1j0, i0j1, i1j1]; the emitter wants a ring, so
          // swap the last two. This preserves the original's triangle split, which
          // matters because the AO gradient is not symmetric across the diagonal.
          const Corner ring[4] = {grid[0], grid[1], grid[3], grid[2]};
          emitQuad(result.opaque, ring, tint);
        }
      }
    }
  }

  // One pass over the finished vertices to mark which sections the sun reaches.
  // Done here rather than in the emit loop so the inner loop stays untouched.
  auto markSunlit = [](const std::vector<TerrainVertex>& verts, MeshSections& sec) {
    for (int i = 0; i < kSections; ++i) {
      const std::uint32_t end = sec.first[i] + sec.count[i];
      for (std::uint32_t v = sec.first[i]; v < end; ++v) {
        if (verts[v].sky > 0) {
          sec.sunlit[i] = true;
          break;
        }
      }
    }
  };

  // Counts fall out of the offsets: a section runs to the start of the next
  // non-empty one, and the last runs to the end.
  for (int i = 0; i < kSections; ++i) {
    const std::uint32_t opaqueEnd = i + 1 < kSections
                                        ? result.opaqueSections.first[i + 1]
                                        : static_cast<std::uint32_t>(result.opaque.size());
    const std::uint32_t waterEnd = i + 1 < kSections
                                       ? result.waterSections.first[i + 1]
                                       : static_cast<std::uint32_t>(result.water.size());
    result.opaqueSections.count[i] = opaqueEnd - result.opaqueSections.first[i];
    result.waterSections.count[i] = waterEnd - result.waterSections.first[i];
  }

  markSunlit(result.opaque, result.opaqueSections);
  markSunlit(result.water, result.waterSections);

  return result;
}

}  // namespace hr::world
