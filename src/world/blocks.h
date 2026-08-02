// The central block registry, ported from js/world/blocks.js.
//
// Everything keys off this table: worldgen, meshing, lighting, mining, crafting
// and saving. To add a block, append one entry in blocks.cpp and register a
// painter under the matching texture id.
//
// Ids are assigned by table order, exactly as the JS did, and the generated
// stone/wood/plant/build families are produced in the same order — so the numeric
// ids match the web build and the golden-vector diff can compare raw voxel data.
// Saves still store the stable string `key`, never the id, so the table can be
// reordered without invalidating a world.
//
// Two deliberate departures from the JS, both for future resource-pack support:
//
//  * `tex: {all|top|side|bottom}` becomes an MC-shaped texture map of named slots
//    holding TextureRefs, and `texForFace`'s top/side/bottom collapse is resolved
//    once at registry-build time into six independent faces.
//  * `tintIndex` exists (always -1 today). Minecraft packs ship greyscale grass
//    and foliage that must be tinted per biome; the web build had no tint path at
//    all, with grass green baked into the painter.

#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "resource/identifier.h"

namespace hr::world {

// Block identity. Voxel storage stays 16-bit for cache behaviour, but the API
// boundary is 32-bit so widening later — for Minecraft-style blockstates, which
// routinely exceed the JS build's 64-value metadata ceiling — is a typedef change
// rather than a format break.
using BlockId = std::uint16_t;
using BlockStateId = std::uint32_t;

inline constexpr BlockId kAir = 0;
inline constexpr int kMaxMeta = 0xFFFF;

// Mining progression gate (js/world/blocks.js:7-15). Sunbrass is a side-grade:
// the same gate as ferralite, but fast and fragile.
namespace tier {
inline constexpr int kHand = 0;
inline constexpr int kWood = 1;
inline constexpr int kStone = 2;
inline constexpr int kCopper = 3;
inline constexpr int kFerralite = 4;
inline constexpr int kSunbrass = 4;
inline constexpr int kAetherite = 5;
}  // namespace tier

enum class RenderKind : std::uint8_t {
  None = 0,  // air
  Cube,
  Cross,   // X-shaped billboard: torches, plants
  Liquid,  // variable-height water surface
  Stair,
  Slab,
  VSlab,
  Ladder,
  Trapdoor,
  Door,
  Bed,
  // A picture hung flat on a wall. The frame is a thin box like a ladder's; what
  // makes it its own kind is that the face is not an atlas tile — it is a texture
  // of its own, per position, so the mesher cannot draw it and a separate pass
  // does. See render/paintings.h.
  Painting,
};

enum class ToolType : std::uint8_t { None = 0, Pick, Axe, Shovel };

enum class Station : std::uint8_t { None = 0, Workbench, Forge, Chest };

struct Rgb {
  float r = 1.0f, g = 1.0f, b = 1.0f;
};

// Named texture slots, mirroring Minecraft's model `textures` map. Kept as a
// small ordered vector rather than a hash map: definitions have at most four
// entries and this is walked at load time only.
class TextureMap {
 public:
  void set(std::string_view slot, std::string_view spec) {
    for (auto& entry : entries_) {
      if (entry.first == slot) {
        entry.second = TextureRef(spec);
        return;
      }
    }
    entries_.emplace_back(std::string(slot), TextureRef(spec));
  }

  const TextureRef* find(std::string_view slot) const {
    for (const auto& entry : entries_) {
      if (entry.first == slot) return &entry.second;
    }
    return nullptr;
  }

  bool empty() const { return entries_.empty(); }
  const std::vector<std::pair<std::string, TextureRef>>& entries() const { return entries_; }

 private:
  std::vector<std::pair<std::string, TextureRef>> entries_;
};

inline constexpr float kUnbreakable = std::numeric_limits<float>::infinity();

struct BlockDef {
  BlockId id = 0;
  std::string key;   // stable; this is what saves and recipes use
  std::string name;  // display

  RenderKind render = RenderKind::None;
  bool solid = false;   // participates in collision
  bool opaque = false;  // blocks light AND culls neighbour faces

  TextureMap textures;

  float hardness = kUnbreakable;  // seconds to break at speed 1
  ToolType tool = ToolType::None;
  int minTier = 0;
  std::string drop;  // item key; empty = drops nothing
  int dropCount = 1;

  int light = 0;                       // emission 0..15
  Rgb lightColor {1.0f, 0.85f, 0.6f};  // warm default, overridden per block

  Station station = Station::None;

  // Behaviour flags, one per JS property.
  bool isLog = false;
  bool isLeaf = false;
  bool isPlant = false;
  bool replaceable = false;  // a placed block overwrites it in place
  bool climb = false;
  bool toggle = false;
  bool tall = false;  // occupies two stacked cells (doors)
  bool sleep = false;
  bool anchor = false;
  bool shore = false;  // needs adjacent water to be placed
  bool isPlank = false;

  // --- physical support, checked by BlockUpdateSim --------------------------
  //
  // These are what stop a torch hanging in mid-air after you mine the wall it was
  // on, and they are deliberately data rather than a switch on block key: a
  // resource pack that adds a plant should get the behaviour by declaring it.
  //
  // Breaks into its drop when the cell beneath stops being able to hold it. Every
  // Cross-rendered block — torches, plants, mushrooms, pebbles — wants this.
  bool needsGround = false;
  // Breaks when water enters its own cell. A torch does not survive a flood, and
  // nor does a tuft of grass.
  bool washesAway = false;
  // Falls as an entity when the cell beneath is air. Sand and anything like it.
  bool falls = false;

  float plantHeight = 0.0f;  // billboard height in blocks
  float plantRadius = 0.0f;

  // Resource-pack forward: which biome colormap tints this block, or -1 for none.
  // Always -1 today because the procedural textures are painted pre-coloured.
  std::int8_t tintIndex = -1;

  bool breakable = false;  // derived: hardness is finite

  // --- resolved at registry-build time -------------------------------------
  // The web build called texForFace(block, faceDir) per face per voxel, collapsing
  // {all|top|side|bottom} on every call. Minecraft models address six faces
  // independently, so the collapse happens once here instead and the mesher just
  // indexes. Face order: 0:+x 1:-x 2:+y 3:-y 4:+z 5:-z.
  std::array<ResourceId, 6> faceTextures;
  // Special slots the mesher substitutes: the bed's foot cell shows the blanket
  // rather than the pillow on its top face, and a papyrus segment with more
  // papyrus above shows the plain stem so a clump reads as continuous reeds.
  ResourceId footTexture;
  ResourceId stemTexture;
};

// The registry. Built once at startup and then read-only, so worker threads can
// share it without synchronisation.
class BlockRegistry {
 public:
  static const BlockRegistry& get();

  const std::vector<BlockDef>& all() const { return blocks_; }
  std::size_t count() const { return blocks_.size(); }

  // Out-of-range ids return air, matching the JS `BLOCKS[id] || BLOCKS[AIR]`.
  const BlockDef& def(BlockId id) const {
    return id < blocks_.size() ? blocks_[id] : blocks_[kAir];
  }

  // Unknown keys return kAir, matching `BLOCK[key] ?? AIR` on load.
  BlockId idOf(std::string_view key) const;
  bool has(std::string_view key) const;

  // Flat property tables for the per-voxel hot paths: the lighting flood, mesher
  // face culling and collision. The JS used Uint8Arrays here for the same reason.
  bool opaque(BlockId id) const { return id < opaque_.size() && opaque_[id] != 0; }
  bool solid(BlockId id) const { return id < solid_.size() && solid_[id] != 0; }
  int emit(BlockId id) const { return id < emit_.size() ? emit_[id] : 0; }

  // Ingredient tags: "#planks" matches any of the five plank keys.
  bool ingredientMatches(std::string_view required, std::string_view slotKey) const;
  std::string tagRepresentative(std::string_view required) const;
  const std::vector<std::string>* tag(std::string_view name) const;

  // The base materials that get a stair/slab/vertical-slab family, in definition
  // order. Recipe generation walks this rather than the maps below so the recipe
  // list comes out in the same order every run — an unordered_map would shuffle
  // the recipe book between builds.
  const std::vector<std::string>& buildMaterialKeys() const { return buildMaterialKeys_; }

  // base key -> shaped variant, consumed by the recipe tables.
  const std::unordered_map<std::string, std::string>& stairOf() const { return stairOf_; }
  const std::unordered_map<std::string, std::string>& slabOf() const { return slabOf_; }
  const std::unordered_map<std::string, std::string>& vslabOf() const { return vslabOf_; }
  const std::unordered_map<std::string, std::string>& slabFromVslab() const {
    return slabFromVslab_;
  }

 private:
  BlockRegistry();

  std::vector<BlockDef> blocks_;
  std::unordered_map<std::string, BlockId> byKey_;
  std::vector<std::uint8_t> opaque_, solid_, emit_;
  std::unordered_map<std::string, std::vector<std::string>> tags_;
  std::vector<std::string> buildMaterialKeys_;
  std::unordered_map<std::string, std::string> stairOf_, slabOf_, vslabOf_, slabFromVslab_;
};

// Convenience accessor, since almost every call site wants the singleton.
inline const BlockRegistry& blocks() { return BlockRegistry::get(); }

// The block ids worldgen, the mesher and interaction refer to by name. Resolved
// once at startup rather than looked up by string in inner loops.
struct WellKnownBlocks {
  BlockId air = 0, bedrock = 0, greystone = 0, cobbled = 0, loam = 0, turf = 0, sand = 0,
          snowturf = 0, sandstone = 0, shingle = 0, log = 0, leaves = 0, planks = 0, water = 0,
          emberlight = 0, glass = 0, umberstone = 0, slatestone = 0, papyrus = 0, ladder = 0,
          canvas = 0,
          bed = 0, tall_grass = 0, fern = 0, bush = 0, dead_shrub = 0, pebbles = 0,
          mushroom_red = 0, mushroom_brown = 0, pine_log = 0, pine_leaves = 0, dusk_log = 0,
          dusk_leaves = 0, birch_log = 0, birch_leaves = 0, palm_log = 0, palm_leaves = 0;

  // The five flowers, in the order worldgen indexes them.
  std::array<BlockId, 5> flowers {};
};

const WellKnownBlocks& wk();

}  // namespace hr::world
