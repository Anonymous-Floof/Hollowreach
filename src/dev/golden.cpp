#include "dev/golden.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/jsmath.h"
#include "core/prng.h"
#include "game/crafting.h"
#include "game/items.h"
#include "game/recipes.h"
#include "resource/atlas.h"
#include "resource/packstack.h"
#include "world/chunk.h"
#include "world/lighting.h"
#include "world/mesher.h"
#include "world/noise.h"
#include "world/worldgen.h"

namespace hr::dev {
namespace {

// --- formatting, matched to gen_golden.mjs -----------------------------------

// A double as its raw IEEE-754 bits, big-endian, 16 lowercase hex digits — the
// same thing DataView.setFloat64 produces on the JS side. Decimal formatting
// would hide exactly the sub-ulp differences this dump exists to catch.
std::string d(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(bits));
  return buffer;
}

// The light hashes cover only the bottom 128 layers of a chunk, because that is
// the whole of the JavaScript's world and this build's is 192. Restricting the
// slice rather than dropping the check keeps it meaningful: the arrays are laid
// out with y slowest (localIdx = (y*CZ + z)*CX + x), so the shared region is a
// contiguous prefix and compares byte for byte. What sits above it is asserted
// separately in --selftest — air, full skylight, no blocklight — because a hash
// that silently covered a different amount of world would be worse than no hash.
constexpr std::size_t kJsCells = 128 * world::CZ * world::CX;
constexpr std::size_t kJsLightBytes = kJsCells;

std::string u32(std::uint32_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%08x", value);
  return buffer;
}

std::uint32_t fnv1a(const std::uint8_t* bytes, std::size_t count) {
  std::uint32_t h = 0x811c9dc5u;
  for (std::size_t i = 0; i < count; ++i) {
    h ^= bytes[i];
    h = jsmath::imul(h, 0x01000193u);
  }
  return h;
}

class Dump {
 public:
  void line(const std::string& text) { lines_.push_back(text); }
  void section(const std::string& name) { lines_.push_back("\n## " + name); }

  // printf-style, since most lines interleave several formatted numbers.
  void linef(const char* fmt, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    lines_.emplace_back(buffer);
  }

  bool write(const std::string& path) const {
    std::string body;
    for (const std::string& l : lines_) {
      body += l;
      body.push_back('\n');
    }
    if (path == "-") {
      std::fwrite(body.data(), 1, body.size(), stdout);
      return true;
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    return true;
  }

 private:
  std::vector<std::string> lines_;
};

// --- the fixture, identical to gen_golden.mjs --------------------------------
// The first two are the seeds of the worlds in worlds/, and both exceed 2^31 —
// the case that catches a signed/unsigned slip in the hashes.
const std::uint32_t kSeeds[] = {3479357960u, 3918175327u, 1u, 12345u, 4294967295u};
const char* const kSeedStrings[] = {"",        "a",           "turf_top", "greystone",
                                   "papyrus_stem", "item:pick_copper", "0"};

struct Salt {
  const char* name;
  std::uint32_t value;
};
// The twelve fields worldgen's ensure() builds (js/world/worldgen.js:22-42).
const Salt kSalts[] = {
    {"terrain", 0}, {"hills", 0x9e37}, {"cave", 0x1b3f},  {"cave2", 0x77d1},
    {"ore", 0x2c91}, {"stonevar", 0x5a17}, {"flora", 0x0f10}, {"temp", 0x3c5a},
    {"moist", 0x66b1}, {"mount", 0x14e9}, {"ridge", 0x7f23}, {"ravine", 0x2ba7},
};

bool wants(const std::string& sections, const char* name) {
  if (sections.empty()) return true;
  return sections.find(name) != std::string::npos;
}

}  // namespace

bool dumpGolden(const std::string& path, const std::string& sections) {
  Dump out;

  out.line("# hollowreach golden vectors v1");
  out.line("# source: c++");
  // The dump covers v1 and v2 only, which is what the JavaScript implements, so it
  // reports the sea level of those rather than of the current default.
  out.linef("# GEN_VERSION=%d SEA_LEVEL=%d", 2, world::seaLevel(2));
  out.linef("# blocks=%zu CX=%d CZ=%d WH=%d", world::blocks().count(), world::CX, world::CZ,
            world::WH);

  if (wants(sections, "prng")) {
    out.section("prng.hashSeed");
    for (const char* s : kSeedStrings) {
      out.linef("hashSeed(\"%s\") = %s", s, u32(hashSeed(s)).c_str());
    }

    out.section("prng.mulberry32");
    for (std::uint32_t seed : kSeeds) {
      Mulberry32 rng(seed);
      std::string values;
      for (int i = 0; i < 8; ++i) {
        if (i) values += ' ';
        values += d(rng.next());
      }
      out.linef("mulberry32(%u) = %s", seed, values.c_str());
    }

    out.section("prng.hash2i");
    const std::int32_t pairs[][2] = {{0, 0},                        {1, 0},
                                     {-1, 0},                       {0, -1},
                                     {INT32_MIN, INT32_MAX},        {12345, -6789}};
    for (std::uint32_t seed : kSeeds) {
      for (const auto& p : pairs) {
        out.linef("hash2i(%u, %d, %d) = %s", seed, p[0], p[1],
                  d(hash2i(seed, p[0], p[1])).c_str());
      }
    }

    out.section("prng.hash3i");
    const std::int32_t triples[][3] = {{0, 0, 0}, {1, 2, 3}, {-1, -2, -3}, {999999, -50, 12}};
    for (std::uint32_t seed : kSeeds) {
      for (const auto& t : triples) {
        out.linef("hash3i(%u, %d, %d, %d) = %s", seed, t[0], t[1], t[2],
                  d(hash3i(seed, t[0], t[1], t[2])).c_str());
      }
    }
  }

  if (wants(sections, "noise")) {
    out.section("noise.perm");
    for (std::uint32_t seed : kSeeds) {
      for (const Salt& salt : kSalts) {
        Noise n(seed ^ salt.value);
        const auto& perm = n.permutation();
        std::string head;
        for (int i = 0; i < 16; ++i) {
          if (i) head += ',';
          head += std::to_string(perm[i]);
        }
        out.linef("perm(%u,%s) = %s [%s]", seed, salt.name,
                  u32(fnv1a(perm.data(), perm.size())).c_str(), head.c_str());
      }
    }
  }

  if (wants(sections, "fields")) {
    out.section("noise.samples");
    const std::uint32_t sampleSeeds[] = {3918175327u, 12345u};
    const double pairs2[][2] = {
        {0, 0}, {0.5, 0.5}, {1.5, 2.5}, {-3.25, 7.125}, {1234.5678, -8765.4321}};
    const double triples3[][3] = {
        {0, 0, 0}, {0.5, 0.5, 0.5}, {-3.25, 7.125, 0.75}, {123.5, 45.25, -67.125}};
    // The label text has to match the JS side or the diff pairs the wrong lines
    // up. %.10g reproduces JavaScript's shortest-round-trip formatting for every
    // literal in this fixture; plain %g truncates 1234.5678 to 1234.57.
    for (std::uint32_t seed : sampleSeeds) {
      Noise n(seed);
      for (const auto& p : pairs2) {
        out.linef("noise2(%u; %.10g, %.10g) = %s", seed, p[0], p[1], d(n.noise2(p[0], p[1])).c_str());
      }
      for (const auto& t : triples3) {
        out.linef("noise3(%u; %.10g, %.10g, %.10g) = %s", seed, t[0], t[1], t[2],
                  d(n.noise3(t[0], t[1], t[2])).c_str());
      }
      for (int oct = 1; oct <= 4; ++oct) {
        out.linef("fbm2(%u; 12.34, -56.78, %d) = %s", seed, oct,
                  d(n.fbm2(12.34, -56.78, oct)).c_str());
        out.linef("fbm3(%u; 1.5, 2.5, 3.5, %d) = %s", seed, oct,
                  d(n.fbm3(1.5, 2.5, 3.5, oct)).c_str());
      }
    }
  }

  if (wants(sections, "worldgen")) {
    out.section("worldgen.heightAt");
    for (std::uint32_t seed : kSeeds) {
      world::NoiseSet noise(seed);
      for (int ver = 1; ver <= 2; ++ver) {
        // A coarse grid wide enough to cross biome and mountain-mask boundaries.
        std::string heights;
        for (int i = 0; i < 32; ++i) {
          const int wx = (i - 16) * 137;
          const int wz = (i * 89) - 800;
          if (i) heights += ',';
          heights += std::to_string(world::heightAt(noise, wx, wz, ver));
        }
        out.linef("heightAt(%u, v%d) = %s", seed, ver, heights.c_str());
      }
    }

    out.section("worldgen.biomeAt");
    for (std::uint32_t seed : kSeeds) {
      world::NoiseSet noise(seed);
      // A histogram over a wide area: biomeOf's thresholds are tuned to the field
      // distribution, so counts drifting means the fields drifted.
      int counts[world::kBiomeCount] = {0, 0, 0, 0, 0};
      for (int x = -2000; x <= 2000; x += 53) {
        for (int z = -2000; z <= 2000; z += 53) {
          counts[static_cast<int>(world::biomeAt(noise, x, z, 2))]++;
        }
      }
      out.linef("biomeHist(%u) = %d,%d,%d,%d,%d", seed, counts[0], counts[1], counts[2],
                counts[3], counts[4]);
    }

    out.section("worldgen.surfacePreview");
    for (std::uint32_t seed : {3479357960u, 3918175327u}) {
      world::NoiseSet noise(seed);
      for (int ver = 1; ver <= 2; ++ver) {
        std::string cells;
        for (int i = 0; i < 24; ++i) {
          const world::SurfacePreview p =
              world::surfacePreview(noise, i * 61 - 700, i * -47 + 300, ver);
          if (i) cells += ' ';
          cells += p.key + "@" + std::to_string(p.h);
        }
        out.linef("surfacePreview(%u, v%d) = %s", seed, ver, cells.c_str());
      }
    }
  }

  if (wants(sections, "chunks")) {
    out.section("worldgen.generate");
    // Both an id hash and a key hash. The id hash also proves the block table is
    // ordered identically to the JS; if only the key hash matches, the ids drifted
    // but the terrain itself is right.
    const world::BlockRegistry& registry = world::blocks();
    const int chunkList[][2] = {{0, 0}, {1, 0}, {0, 1}, {-1, -1}, {7, -3}, {-12, 25}, {100, 100}};

    for (std::uint32_t seed : {3479357960u, 3918175327u, 12345u}) {
      world::NoiseSet noise(seed);
      for (int ver = 1; ver <= 2; ++ver) {
        for (const auto& cc : chunkList) {
          world::Chunk chunk;
          chunk.cx = cc[0];
          chunk.cz = cc[1];
          world::generate(chunk, noise, ver);

          const auto& voxels = chunk.data->voxels;
          // Only the bottom 128 layers, for the reason kJsLightBytes gives: this
          // build's world is 192 tall and the reference's is 128, and hashing a
          // different amount of world would be a comparison that always passes.
          // nonAir is counted over the WHOLE column, so anything generated in the
          // new space above would still show up here as a mismatch.
          const std::size_t shared = std::min<std::size_t>(voxels.size(), kJsCells);
          std::vector<std::uint8_t> keyBytes;
          keyBytes.reserve(shared * 6);
          int nonAir = 0;
          for (std::size_t i = 0; i < voxels.size(); ++i) {
            if (voxels[i] != world::kAir) ++nonAir;
            if (i >= shared) continue;
            const std::string& key = registry.def(voxels[i]).key;
            for (char ch : key) keyBytes.push_back(static_cast<std::uint8_t>(ch));
            keyBytes.push_back(0);
          }
          out.linef("chunk(%u, v%d, %d, %d) ids=%s keys=%s nonAir=%d", seed, ver, cc[0], cc[1],
                    u32(fnv1a(reinterpret_cast<const std::uint8_t*>(voxels.data()),
                              shared * sizeof(world::BlockId)))
                        .c_str(),
                    u32(fnv1a(keyBytes.data(), keyBytes.size())).c_str(), nonAir);
        }
      }
    }
  }

  if (wants(sections, "mesh") || wants(sections, "meshverts")) {
    const bool vertexMode = wants(sections, "meshverts") && !sections.empty();
    if (!vertexMode) out.section("mesher.chunks");

    // The atlas is needed for tile lookups, but its UVs are not hashed: the two
    // builds lay their atlases out differently by design. See mesh_golden.mjs.
    resource::AtlasSettings atlasSettings;
    resource::Atlas atlas;
    atlas.build(resource::defaultStack(), resource::collectBlockTextureIds(), atlasSettings);
    world::BlockTileTable tiles;
    tiles.build(atlas);

    // In vertex mode only one chunk is dumped, matching mesh_golden.mjs --verts.
    using Coord = std::array<int, 2>;
    // Vertex mode targets one chunk, given as
    // "meshverts:<seed>:<ver>:<cx>:<cz>" (defaults to 12345:1:0:0), matching
    // mesh_golden.mjs --verts.
    std::uint32_t vSeed = 12345u;
    int vVer = 1, vCx = 0, vCz = 0;
    if (vertexMode) {
      const std::size_t at = sections.find("meshverts:");
      if (at != std::string::npos) {
        std::string spec = sections.substr(at + 10);
        const std::size_t end = spec.find(',');
        if (end != std::string::npos) spec = spec.substr(0, end);
        std::vector<long long> parts;
        std::size_t pos = 0;
        while (pos <= spec.size()) {
          const std::size_t colon = spec.find(':', pos);
          const std::string token =
              spec.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
          if (!token.empty()) parts.push_back(std::strtoll(token.c_str(), nullptr, 10));
          if (colon == std::string::npos) break;
          pos = colon + 1;
        }
        if (parts.size() >= 4) {
          vSeed = static_cast<std::uint32_t>(parts[0]);
          vVer = static_cast<int>(parts[1]);
          vCx = static_cast<int>(parts[2]);
          vCz = static_cast<int>(parts[3]);
        }
      }
    }
    const std::vector<Coord> meshChunks =
        vertexMode ? std::vector<Coord> {{vCx, vCz}}
                   : std::vector<Coord> {{0, 0}, {1, 0}, {-1, -1}, {7, -3}, {-12, 25}};
    const std::vector<std::uint32_t> meshSeeds =
        vertexMode ? std::vector<std::uint32_t> {vSeed}
                   : std::vector<std::uint32_t> {3479357960u, 3918175327u, 12345u};
    const int verLoMesh = vertexMode ? vVer : 1;
    const int verHi = vertexMode ? vVer : 2;

    // Positions as int32 at 1/16 of a block, then quantised shade / sky / block /
    // wave. See mesh_golden.mjs for the full reasoning: std::cos and std::sin are
    // not bit-specified and differ from V8's by one ULP on a few of the angles the
    // plant jitter uses, so a fine grid makes the comparison flaky without making
    // it more sensitive to anything real.
    auto hashVerts = [](const std::vector<world::TerrainVertex>& verts) {
      std::vector<std::uint8_t> bytes(verts.size() * 16);
      std::size_t o = 0;
      auto quantPos = [](float value) {
        return static_cast<std::int32_t>(std::lround(static_cast<double>(value) * 16.0));
      };
      for (const world::TerrainVertex& v : verts) {
        const std::int32_t qx = quantPos(v.x), qy = quantPos(v.y), qz = quantPos(v.z);
        std::memcpy(&bytes[o], &qx, 4);
        std::memcpy(&bytes[o + 4], &qy, 4);
        std::memcpy(&bytes[o + 8], &qz, 4);
        bytes[o + 12] = v.shade;
        bytes[o + 13] = v.sky;
        bytes[o + 14] = v.block;
        bytes[o + 15] = static_cast<std::uint8_t>(v.flags & 3);
        o += 16;
      }
      return fnv1a(bytes.data(), bytes.size());
    };

    for (std::uint32_t seed : meshSeeds) {
      world::NoiseSet noise(seed);
      for (int ver = verLoMesh; ver <= verHi; ++ver) {
        for (const Coord& cc : meshChunks) {
          // A generated and lit 3x3 neighbourhood, so border faces and edge
          // smooth-lighting are correct rather than approximated.
          std::vector<world::Chunk> ring(9);
          for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
              world::Chunk& c = ring[(dz + 1) * 3 + (dx + 1)];
              c.cx = cc[0] + dx;
              c.cz = cc[1] + dz;
              world::generate(c, noise, ver);
            }
          }

          // Light each chunk against whichever neighbours the ring provides, the
          // same partial view the streamer gives it.
          for (int gi = 0; gi < 9; ++gi) {
            world::Chunk& c = ring[gi];
            world::LightNeighbourhood ln;
            ln.cx = c.cx;
            ln.cz = c.cz;
            for (int dz = -1; dz <= 1; ++dz) {
              for (int dx = -1; dx <= 1; ++dx) {
                const int nx = c.cx + dx, nz = c.cz + dz;
                for (const world::Chunk& other : ring) {
                  if (other.cx == nx && other.cz == nz) {
                    ln.grid[(dz + 1) * 3 + (dx + 1)] = other.data.get();
                    break;
                  }
                }
              }
            }
            world::computeLight(*c.data, ln, c.emitters);
          }

          const world::Chunk& centre = ring[4];
          world::MeshNeighbourhood nb;
          nb.cx = centre.cx;
          nb.cz = centre.cz;
          for (int gi = 0; gi < 9; ++gi) nb.grid[gi] = ring[gi].data.get();

          const world::MeshResult mesh = world::meshChunk(nb, tiles);
          const auto& sky = centre.data->skylight;
          const auto& blk = centre.data->blocklight;

          if (vertexMode) {
            // One line per vertex, for locating the first disagreement with the
            // JavaScript when a chunk hash differs.
            // Raw float32 bits, not decimals: a one-ULP position difference is
            // exactly what a hash mismatch with visually identical output means.
            auto fx = [](float value) {
              std::uint32_t bits = 0;
              std::memcpy(&bits, &value, 4);
              char buf[16];
              std::snprintf(buf, sizeof(buf), "%08x", bits);
              return std::string(buf);
            };
            std::size_t n = 0;
            for (const world::TerrainVertex& v : mesh.opaque) {
              out.linef("%zu %s %s %s %u %u %u %u", n++, fx(v.x).c_str(), fx(v.y).c_str(),
                        fx(v.z).c_str(), static_cast<unsigned>(v.shade),
                        static_cast<unsigned>(v.sky), static_cast<unsigned>(v.block),
                        static_cast<unsigned>(v.flags & 3));
            }
            continue;
          }
          out.linef(
              "mesh(%u, v%d, %d, %d) opaqueVerts=%zu waterVerts=%zu opaque=%s water=%s "
              "sky=%s blk=%s emitters=%zu",
              seed, ver, cc[0], cc[1], mesh.opaque.size(), mesh.water.size(),
              u32(hashVerts(mesh.opaque)).c_str(), u32(hashVerts(mesh.water)).c_str(),
              u32(fnv1a(sky.data(), kJsLightBytes)).c_str(),
              u32(fnv1a(blk.data(), kJsLightBytes)).c_str(), centre.emitters.size());
        }
      }
    }
  }

  return out.write(path);
}

bool dumpAtlas(const std::string& pngPath, int maxTileResolution, bool mipmaps) {
  resource::AtlasSettings settings;
  settings.maxTileResolution = maxTileResolution;
  settings.mipmaps = mipmaps;

  // The headless path never runs App::run, so the item sprite provider has to be
  // installed here too — otherwise every `item/*` id would resolve to the magenta
  // missing tile.
  resource::defaultStack().push(game::makeItemSpriteProvider());

  std::vector<ResourceId> wanted = resource::collectBlockTextureIds();
  {
    const std::vector<ResourceId> itemIds = game::collectItemTextureIds();
    wanted.insert(wanted.end(), itemIds.begin(), itemIds.end());
  }
  resource::Atlas atlas;
  if (!atlas.build(resource::defaultStack(), wanted, settings)) return false;
  if (!atlas.writeDebugPng(pngPath)) return false;

  // The sidecar is what actually gets diffed. Per-tile hashes localise a failure
  // to one painter, which a whole-atlas hash cannot do — and the atlas *layout*
  // differs from the web build by design (power-of-two, gutters), so comparing the
  // images directly would report every tile as moved.
  Dump out;
  out.line("# hollowreach atlas tiles v1");
  out.line("# source: c++");
  out.linef("# tileRes=%d tiles=%zu", atlas.tileResolution(), wanted.size());

  const Image& img = atlas.image();
  std::vector<std::pair<std::string, std::string>> rows;
  rows.reserve(wanted.size());
  for (const ResourceId& id : wanted) {
    const resource::TileRef& t = atlas.tile(id);
    // Hash the tile interior only: the gutter is a native-build concept and would
    // make every hash differ from the browser's.
    std::vector<std::uint8_t> pixels;
    pixels.reserve(static_cast<std::size_t>(t.w) * t.h * 4);
    for (int y = 0; y < t.h; ++y) {
      for (int x = 0; x < t.w; ++x) {
        const Rgba c = img.get(t.x + x, t.y + y);
        pixels.push_back(c.r);
        pixels.push_back(c.g);
        pixels.push_back(c.b);
        pixels.push_back(c.a);
      }
    }
    rows.emplace_back(id.path(), u32(fnv1a(pixels.data(), pixels.size())));
  }
  // Sorted by name so the order does not depend on the layout.
  std::sort(rows.begin(), rows.end());
  out.section("atlas.tiles");
  for (const auto& [name, hash] : rows) {
    out.linef("tile(%s) = %s", name.c_str(), hash.c_str());
  }

  const std::string sidecar = pngPath + ".txt";
  if (!out.write(sidecar)) return false;
  std::printf("atlas: %s (%dx%d), tile hashes in %s\n", pngPath.c_str(), atlas.width(),
              atlas.height(), sidecar.c_str());
  return true;
}

// --- crafting tables ---------------------------------------------------------

namespace {

const char* stationName(game::CraftStation s) {
  return s == game::CraftStation::Hand ? "hand" : "workbench";
}

std::string canonicalRecipe(const game::Recipe& r) {
  std::string out;
  if (r.type == game::RecipeType::Shapeless) {
    std::vector<std::string> ings;
    ings.reserve(r.ingredients.size());
    for (const auto& [key, count] : r.ingredients) {
      ings.push_back(key + "*" + std::to_string(count));
    }
    std::sort(ings.begin(), ings.end());
    out = "shapeless|" + std::string(stationName(r.station)) + "|";
    for (std::size_t i = 0; i < ings.size(); ++i) {
      if (i) out += ' ';
      out += ings[i];
    }
  } else {
    out = "shaped|" + std::string(stationName(r.station)) + "|" + std::to_string(r.width) +
          "x" + std::to_string(r.height) + "|";
    for (std::size_t i = 0; i < r.cells.size(); ++i) {
      if (i) out += ' ';
      out += std::to_string(r.cells[i].row) + "," + std::to_string(r.cells[i].col) + "," +
             r.cells[i].key;
    }
  }
  out += "|out=" + r.outKey + "*" + std::to_string(r.outCount);
  return out;
}

// Kept in step with the same list in tools/recipes_golden.mjs.
struct CraftCase {
  const char* label;
  int size;
  game::CraftStation station;
  std::vector<std::array<int, 2>> at;   // row, col
  std::vector<const char*> keys;
};

std::vector<CraftCase> craftCases() {
  using S = game::CraftStation;
  return {
      {"log->planks (2x2 hand)", 2, S::Hand, {{0, 0}}, {"log"}},
      {"sticks (2x2 hand)", 2, S::Hand, {{0, 0}, {1, 0}}, {"planks", "planks"}},
      {"sticks from pine (tag)", 2, S::Hand, {{0, 0}, {1, 0}},
       {"pine_planks", "pine_planks"}},
      {"sticks mixed woods (tag)", 2, S::Hand, {{0, 0}, {1, 0}}, {"planks", "birch_planks"}},
      {"workbench (2x2 hand)", 2, S::Hand, {{0, 0}, {0, 1}, {1, 0}, {1, 1}},
       {"planks", "planks", "planks", "planks"}},
      {"torch (2x2 hand)", 2, S::Hand, {{0, 0}, {1, 0}}, {"embercoal", "stick"}},
      {"forge needs a bench (2x2)", 2, S::Hand, {{0, 0}, {0, 1}, {1, 0}, {1, 1}},
       {"cobbled", "cobbled", "cobbled", "cobbled"}},
      {"chest (3x3 bench)", 3, S::Workbench,
       {{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 2}, {2, 0}, {2, 1}, {2, 2}},
       {"planks", "planks", "planks", "planks", "planks", "planks", "planks", "planks"}},
      {"wooden pick (3x3 bench)", 3, S::Workbench, {{0, 0}, {0, 1}, {0, 2}, {1, 1}, {2, 1}},
       {"planks", "planks", "planks", "stick", "stick"}},
      {"iron chestguard (3x3 bench)", 3, S::Workbench,
       {{0, 0}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {2, 0}, {2, 1}, {2, 2}},
       {"ferralite_ingot", "ferralite_ingot", "ferralite_ingot", "ferralite_ingot",
        "ferralite_ingot", "ferralite_ingot", "ferralite_ingot", "ferralite_ingot"}},
      {"slab row, offset in the grid", 3, S::Workbench, {{2, 0}, {2, 1}, {2, 2}},
       {"cobbled", "cobbled", "cobbled"}},
      {"stairs (3-2-1)", 3, S::Workbench,
       {{0, 0}, {1, 0}, {1, 1}, {2, 0}, {2, 1}, {2, 2}},
       {"greystone", "greystone", "greystone", "greystone", "greystone", "greystone"}},
      {"bucket (V of iron)", 3, S::Workbench, {{0, 0}, {0, 2}, {1, 1}},
       {"ferralite_ingot", "ferralite_ingot", "ferralite_ingot"}},
      {"soul anchor (full 3x3)", 3, S::Workbench,
       {{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {2, 0}, {2, 1}, {2, 2}},
       {"embercoal", "raw_copper", "raw_ferralite", "azurite", "sparkstone", "raw_sunbrass",
        "verdanite", "aetherite", "gloamite"}},
      {"wayshard (shapeless hand)", 2, S::Hand, {{0, 0}, {0, 1}}, {"gloamite", "sparkstone"}},
      {"slab <-> vertical slab", 2, S::Hand, {{0, 0}}, {"cobbled_slab"}},
      {"nothing (empty grid)", 3, S::Workbench, {}, {}},
      {"nothing (nonsense)", 2, S::Hand, {{0, 0}, {1, 1}}, {"aetherite", "leather"}},
  };
}

// Matches the JS number formatting: integers print bare, fractions keep only the
// digits they need.
std::string number(double v) {
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.17g", v);
  // Round-trip through a shorter form where that is lossless, as JS does.
  for (int precision = 1; precision < 17; ++precision) {
    char candidate[64];
    std::snprintf(candidate, sizeof candidate, "%.*g", precision, v);
    if (std::strtod(candidate, nullptr) == v) return candidate;
  }
  return buffer;
}

}  // namespace

bool dumpRecipes(const std::string& path) {
  const game::RecipeBook& book = game::recipeBook();

  Dump out;
  out.line("# hollowreach recipes v1");
  out.line("# source: c++");
  out.linef("# recipes=%zu smelting=%zu", book.recipes().size(), book.smelting().size());

  out.section("recipes");
  for (std::size_t i = 0; i < book.recipes().size(); ++i) {
    out.linef("recipe(%03zu) = %s", i, canonicalRecipe(book.recipes()[i]).c_str());
  }

  out.section("smelting");
  for (std::size_t i = 0; i < book.smelting().size(); ++i) {
    const game::SmeltingRecipe& s = book.smelting()[i];
    out.linef("smelt(%02zu) = %s -> %s in %ss", i, s.in.c_str(), s.out.c_str(),
              number(s.seconds).c_str());
  }

  out.section("fuel");
  for (const game::ItemDef& item : game::items().all()) {
    const float v = game::fuelValue(item.key);
    if (v > 0) out.linef("fuel(%s) = %s", item.key.c_str(), number(v).c_str());
  }

  out.section("craft");
  for (const CraftCase& c : craftCases()) {
    std::vector<game::ItemStack> grid(static_cast<std::size_t>(c.size) * c.size);
    for (std::size_t i = 0; i < c.at.size(); ++i) {
      grid[static_cast<std::size_t>(c.at[i][0]) * c.size + c.at[i][1]] =
          game::ItemStack {c.keys[i], 1, -1};
    }
    const game::CraftMatch m = game::matchGrid(grid, c.size, c.station);
    if (m) {
      out.linef("craft(%s) = %s*%d", c.label, m.outKey.c_str(), m.outCount);
    } else {
      out.linef("craft(%s) = none", c.label);
    }
  }

  if (!out.write(path)) return false;
  if (path != "-") std::printf("recipes: %s\n", path.c_str());
  return true;
}

}  // namespace hr::dev
