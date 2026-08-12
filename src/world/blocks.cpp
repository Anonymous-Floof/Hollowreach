#include "world/blocks.h"

#include <cmath>
#include <string>

#include "core/log.h"
#include "resource/painters.h"  // kCropStages: how many tiles a crop is drawn at

namespace hr::world {
namespace {

// Fluent builder. The JS definitions were object literals with a dozen optional
// properties; designated initialisers would be correct but unreadable at this
// volume, and the chained form keeps each block on one or two lines the way the
// original did.
class Builder {
 public:
  Builder(std::vector<BlockDef>& out, std::string key, std::string name) : out_(out) {
    def_.key = std::move(key);
    def_.name = std::move(name);
  }

  ~Builder() {
    def_.breakable = std::isfinite(def_.hardness);
    def_.id = static_cast<BlockId>(out_.size());
    out_.push_back(std::move(def_));
  }

  Builder& render(RenderKind kind) { def_.render = kind; return *this; }
  Builder& cube() { def_.render = RenderKind::Cube; return *this; }
  // Cross-rendered blocks are billboards standing on the ground: torches, plants,
  // mushrooms, pebbles. Every one of them wants to fall when its ground goes and
  // to break when water reaches it, so the two flags come with the render kind
  // rather than being repeated on twenty definitions and forgotten on the
  // twenty-first. Either can still be turned back off afterwards.
  Builder& cross() {
    def_.render = RenderKind::Cross;
    def_.needsGround = true;
    def_.washesAway = true;
    return *this;
  }

  // A crop: a Cross plant that stands square instead of jittering, and that names
  // one texture per growth stage. The stage tiles are `crop_<key>_0` .. `_N-1` by
  // convention rather than by declaration, so a crop cannot half-declare its art.
  Builder& crop(std::string_view texKey, int stages = resource::kCropStages) {
    cross();
    def_.isPlant = true;
    def_.alignedPlant = true;
    def_.cropStages = stages;
    def_.plantHeight = 0.94f;
    def_.plantRadius = 0.45f;
    for (int s = 0; s < stages; ++s) {
      def_.textures.set("stage" + std::to_string(s),
                        "block/crop_" + std::string(texKey) + "_" + std::to_string(s));
    }
    // The last stage doubles as the plain `all` slot, so anything that has not been
    // taught about stages (the icon atlas, a resource pack's fallback) shows the
    // crop as the ripe thing a player would recognise it by.
    def_.textures.set("all", "block/crop_" + std::string(texKey) + "_" +
                                 std::to_string(stages - 1));
    return *this;
  }
  // What a ripe crop yields on top of its seed drop.
  Builder& ripe(std::string_view key, int lo, int hi) {
    def_.ripeDrop = std::string(key);
    def_.ripeDropMin = lo;
    def_.ripeDropMax = hi;
    return *this;
  }

  Builder& needsGround(bool on = true) { def_.needsGround = on; return *this; }
  Builder& washesAway(bool on = true) { def_.washesAway = on; return *this; }
  Builder& falls(bool on = true) { def_.falls = on; return *this; }

  Builder& solid(bool on = true) { def_.solid = on; return *this; }
  Builder& opaque(bool on = true) { def_.opaque = on; return *this; }
  // The overwhelmingly common pair.
  Builder& solidOpaque() { def_.solid = true; def_.opaque = true; return *this; }
  // See-through but still collidable: leaves, glass.
  Builder& solidClear() { def_.solid = true; def_.opaque = false; return *this; }

  // Texture slots. The "block/" folder is added here so no definition repeats it.
  Builder& tex(std::string_view all) {
    def_.textures.set("all", "block/" + std::string(all));
    return *this;
  }
  Builder& tex3(std::string_view top, std::string_view side, std::string_view bottom) {
    def_.textures.set("top", "block/" + std::string(top));
    def_.textures.set("side", "block/" + std::string(side));
    def_.textures.set("bottom", "block/" + std::string(bottom));
    return *this;
  }
  Builder& texSlot(std::string_view slot, std::string_view name) {
    def_.textures.set(slot, "block/" + std::string(name));
    return *this;
  }

  Builder& hard(float seconds) { def_.hardness = seconds; return *this; }
  Builder& unbreakable(int minTier) {
    def_.hardness = kUnbreakable;
    def_.minTier = minTier;
    return *this;
  }
  Builder& pick(int minTier) { def_.tool = ToolType::Pick; def_.minTier = minTier; return *this; }
  Builder& axe(int minTier = 0) { def_.tool = ToolType::Axe; def_.minTier = minTier; return *this; }
  Builder& shovel(int minTier = 0) {
    def_.tool = ToolType::Shovel;
    def_.minTier = minTier;
    return *this;
  }

  Builder& drops(std::string_view key, int count = 1) {
    def_.drop = std::string(key);
    def_.dropCount = count;
    return *this;
  }
  Builder& dropsSelf() { def_.drop = def_.key; return *this; }
  Builder& dropsNothing() { def_.drop.clear(); return *this; }

  Builder& emits(int level, float r, float g, float b) {
    def_.light = level;
    def_.lightColor = {r, g, b};
    return *this;
  }
  Builder& emits(int level) { def_.light = level; return *this; }

  Builder& station(Station s) { def_.station = s; return *this; }

  Builder& log() { def_.isLog = true; return *this; }
  Builder& leaf() { def_.isLeaf = true; return *this; }
  Builder& plank() { def_.isPlank = true; return *this; }
  Builder& climb() { def_.climb = true; return *this; }
  Builder& toggle() { def_.toggle = true; return *this; }
  Builder& tall() { def_.tall = true; return *this; }
  Builder& sleep() { def_.sleep = true; return *this; }
  Builder& anchor() { def_.anchor = true; return *this; }
  Builder& shore() { def_.shore = true; return *this; }

  Builder& plant(float height, float radius, bool replaceable = false) {
    def_.isPlant = true;
    def_.plantHeight = height;
    def_.plantRadius = radius;
    def_.replaceable = replaceable;
    return *this;
  }

 private:
  std::vector<BlockDef>& out_;
  BlockDef def_;
};

struct StoneFamily {
  const char* id;
  const char* name;
};
struct WoodFamily {
  const char* id;
  const char* name;
};

// Order matters: block ids come from table order, and the golden-vector diff
// compares raw voxel ids against the JS build.
const StoneFamily kStoneFamilies[] = {{"umber", "Umberstone"}, {"slate", "Slatestone"}};
const WoodFamily kWoodFamilies[] = {
    {"pine", "Pine"}, {"dusk", "Walnut"}, {"birch", "Birch"}, {"palm", "Palm"}};

struct PlantSpec {
  const char* key;
  const char* name;
  float height;
  const char* drop;  // nullptr = drops itself
};
const PlantSpec kPlants[] = {
    {"tall_grass", "Tall Grass", 0.92f, ""},
    {"fern", "Fern", 0.90f, ""},
    {"bush", "Shrub", 0.82f, ""},
    {"dead_shrub", "Dead Bush", 0.80f, "stick"},
    {"pebbles", "Pebbles", 0.26f, ""},
    {"mushroom_red", "Red Mushroom", 0.50f, nullptr},
    {"mushroom_brown", "Brown Mushroom", 0.50f, nullptr},
    {"flower_poppy", "Poppy", 0.70f, nullptr},
    {"flower_daisy", "Daisy", 0.66f, nullptr},
    {"flower_cornflower", "Cornflower", 0.72f, nullptr},
    {"flower_dandelion", "Dandelion", 0.64f, nullptr},
    {"flower_violet", "Violet", 0.60f, nullptr},
};

struct BuildMat {
  std::string key;
  ToolType tool;
  int tier;
  float hardness;
  std::string existingStair;  // empty unless the stair already exists under its own key
};

}  // namespace

BlockRegistry::BlockRegistry() {
  auto& B = blocks_;
  B.reserve(128);

  using T = ToolType;
  (void)sizeof(T);

  // ---------------------------------------------------------------------------
  // Hand-written definitions (js/world/blocks.js:20-156).
  // ---------------------------------------------------------------------------
  Builder(B, "air", "Air");

  Builder(B, "bedrock", "Bedrock").cube().solidOpaque().tex("bedrock").unbreakable(99)
      .pick(99).dropsNothing();
  Builder(B, "greystone", "Stone").cube().solidOpaque().tex("greystone").hard(1.5f)
      .pick(tier::kWood).drops("cobbled");
  Builder(B, "cobbled", "Cobblestone").cube().solidOpaque().tex("cobbled").hard(2.0f)
      .pick(tier::kWood).drops("cobbled");
  Builder(B, "loam", "Dirt").cube().solidOpaque().tex("loam").hard(1.0f).shovel().drops("loam");
  Builder(B, "turf", "Grass Block").cube().solidOpaque().tex3("turf_top", "turf_side", "loam")
      .hard(1.0f).shovel().drops("loam");
  Builder(B, "sand", "Sand").cube().solidOpaque().tex("sand").hard(0.8f).shovel().drops("sand")
      .falls();
  Builder(B, "snowturf", "Snowy Grass").cube().solidOpaque()
      .tex3("snow_top", "snowturf_side", "loam").hard(1.0f).shovel().drops("loam");
  Builder(B, "sandstone", "Sandstone").cube().solidOpaque().tex("sandstone").hard(1.2f)
      .pick(tier::kWood).drops("sandstone");
  Builder(B, "shingle", "Gravel").cube().solidOpaque().tex("shingle").hard(0.9f).shovel()
      .drops("shingle");
  Builder(B, "log", "Oak Log").cube().solidOpaque().log()
      .tex3("log_top", "log_side", "log_top").hard(1.2f).axe().drops("log");
  Builder(B, "leaves", "Oak Leaves").cube().solidClear().leaf().tex("leaves").hard(0.3f)
      .dropsNothing();
  Builder(B, "planks", "Oak Planks").cube().solidOpaque().plank().tex("planks").hard(1.0f)
      .axe().drops("planks");
  Builder(B, "bricks", "Stone Bricks").cube().solidOpaque().tex("bricks").hard(1.6f)
      .pick(tier::kWood).drops("bricks");
  Builder(B, "polished", "Polished Stone").cube().solidOpaque().tex("polished").hard(1.6f)
      .pick(tier::kWood).drops("polished");
  Builder(B, "wool", "White Wool").cube().solidOpaque().tex("wool").hard(0.5f).drops("wool");
  Builder(B, "glass", "Glass").cube().solidClear().tex("glass").hard(0.4f).drops("glass");
  Builder(B, "water", "Water").render(RenderKind::Liquid).tex("water").unbreakable(99)
      .dropsNothing();
  Builder(B, "emberlight", "Torch").cross().tex("torch").hard(0.1f).drops("emberlight")
      .emits(14, 1.0f, 0.62f, 0.26f);

  // ---- ores ----
  Builder(B, "ore_embercoal", "Coal Ore").cube().solidOpaque().tex("ore_embercoal").hard(1.9f)
      .pick(tier::kWood).drops("embercoal");
  Builder(B, "ore_copper", "Copper Ore").cube().solidOpaque().tex("ore_copper").hard(2.0f)
      .pick(tier::kWood).drops("raw_copper");
  Builder(B, "ore_ferralite", "Iron Ore").cube().solidOpaque().tex("ore_ferralite").hard(2.4f)
      .pick(tier::kStone).drops("raw_ferralite");
  Builder(B, "ore_sunbrass", "Gold Ore").cube().solidOpaque().tex("ore_sunbrass").hard(2.4f)
      .pick(tier::kCopper).drops("raw_sunbrass");
  // Diamond glows, faintly and blue. Dimmer than sparkstone on purpose: it is the
  // rarest thing down there and a lamp would give away every vein from across a
  // cavern, which is the opposite of what makes finding one worth anything. Blue
  // rather than the tile's own blue-green, so the light reads as its own colour
  // against the teal of the crystal rather than washing into it.
  Builder(B, "ore_aetherite", "Diamond Ore").cube().solidOpaque().tex("ore_aetherite").hard(2.8f)
      .pick(tier::kFerralite).drops("aetherite").emits(5, 0.34f, 0.62f, 1.0f);
  // Sparkstone's light matches the stone. It was cyan while the tile it lit was
  // 0xe0432f red — the ore looked like an ember and glowed like ice, because the
  // texture colour and the light colour are two different numbers and only one of
  // them had been chosen.
  Builder(B, "ore_sparkstone", "Sparkstone Ore").cube().solidOpaque().tex("ore_sparkstone")
      .hard(2.2f).pick(tier::kFerralite).drops("sparkstone", 4).emits(6, 1.0f, 0.42f, 0.24f);
  Builder(B, "ore_azurite", "Azurite Ore").cube().solidOpaque().tex("ore_azurite").hard(2.2f)
      .pick(tier::kStone).drops("azurite");
  // Gloamite: dusk-violet, deep only. Powers the Wayshard, reserved for future
  // teleport/void tech.
  Builder(B, "ore_gloamite", "Gloamite Ore").cube().solidOpaque().tex("ore_gloamite").hard(2.4f)
      .pick(tier::kFerralite).drops("gloamite").emits(4, 0.62f, 0.4f, 0.9f);
  // Verdanite: mossy living crystal, mid-depths. Reserved for farming/alchemy.
  Builder(B, "ore_verdanite", "Verdanite Ore").cube().solidOpaque().tex("ore_verdanite")
      .hard(2.2f).pick(tier::kStone).drops("verdanite");

  // ---- crafting stations ----
  Builder(B, "workbench", "Workbench").cube().solidOpaque()
      .tex3("workbench_top", "workbench_side", "planks").hard(1.2f).axe().drops("workbench")
      .station(Station::Workbench);
  Builder(B, "forge", "Forge").cube().solidOpaque()
      .tex3("forge_top", "forge_side", "greystone").hard(2.0f).pick(tier::kWood).drops("forge")
      .station(Station::Forge);
  Builder(B, "chest", "Chest").cube().solidOpaque()
      .tex3("chest_top", "chest_side", "planks").hard(1.4f).axe().drops("chest")
      .station(Station::Chest);

  // ---- shaped blocks (orientation/state in metadata; see world/shapes.h) ----
  Builder(B, "greystone_stairs", "Stone Stairs").render(RenderKind::Stair).solidClear()
      .tex("greystone").hard(1.5f).pick(tier::kWood).drops("greystone_stairs");
  Builder(B, "plank_stairs", "Oak Stairs").render(RenderKind::Stair).solidClear().tex("planks")
      .hard(1.0f).axe().drops("plank_stairs");
  Builder(B, "ladder", "Ladder").render(RenderKind::Ladder).climb().tex("ladder").hard(0.4f)
      .drops("ladder");
  Builder(B, "trapdoor", "Oak Trapdoor").render(RenderKind::Trapdoor).solidClear().toggle()
      .tex("trapdoor").hard(0.8f).axe().drops("trapdoor");
  Builder(B, "door", "Oak Door").render(RenderKind::Door).solidClear().toggle().tall()
      .tex("door").hard(0.8f).axe().drops("door");
  // The top slot holds the head (pillow) tile; the mesher swaps in `foot` for the
  // foot cell. `foot` is declared so the atlas paints it.
  Builder(B, "bed", "Bed").render(RenderKind::Bed).solidClear().sleep()
      .tex3("bed_head_top", "bed_side", "planks").texSlot("foot", "bed_foot_top").hard(0.6f)
      .axe().drops("bed");
  // Soul Anchor: right-click to attune your spawn point. Glows soul-teal.
  Builder(B, "soul_anchor", "Soul Anchor").cube().solidOpaque().anchor()
      .tex3("soul_anchor_top", "soul_anchor_side", "soul_anchor_top").hard(2.5f)
      .pick(tier::kWood).drops("soul_anchor").emits(9, 0.42f, 0.95f, 0.88f);
  // Papyrus: a shore reed 1-3 tall, stamped by worldgen and placeable beside
  // water. Not `replaceable`, unlike the decorative plants, so placing a block at
  // it does not silently destroy it. `stem` is used for segments with more papyrus
  // above, so only the top of a clump shows the tufted crown.
  Builder(B, "papyrus", "Papyrus").cross().plant(1.0f, 0.38f).shore()
      .tex("papyrus").texSlot("stem", "papyrus_stem").hard(0.2f).drops("papyrus");

  // ---------------------------------------------------------------------------
  // Generated families. New stones and woods behave exactly like the originals
  // but are recoloured; then every building material gets a stair, slab and
  // vertical slab. Generating these keeps the hand-written list short and makes
  // adding a material a one-line change.
  // ---------------------------------------------------------------------------
  for (const StoneFamily& s : kStoneFamilies) {
    const std::string id = s.id;
    const std::string name = s.name;
    Builder(B, id + "stone", name).cube().solidOpaque().tex(id + "stone").hard(1.5f)
        .pick(tier::kWood).drops(id + "stone");
    Builder(B, "polished_" + id, "Polished " + name).cube().solidOpaque()
        .tex("polished_" + id).hard(1.6f).pick(tier::kWood).drops("polished_" + id);
    Builder(B, "bricks_" + id, name + " Bricks").cube().solidOpaque().tex("bricks_" + id)
        .hard(1.6f).pick(tier::kWood).drops("bricks_" + id);
  }

  for (const WoodFamily& w : kWoodFamilies) {
    const std::string id = w.id;
    const std::string name = w.name;
    Builder(B, id + "_log", name + " Log").cube().solidOpaque().log()
        .tex3(id + "_log_top", id + "_log_side", id + "_log_top").hard(1.2f).axe()
        .drops(id + "_log");
    Builder(B, id + "_planks", name + " Planks").cube().solidOpaque().plank()
        .tex(id + "_planks").hard(1.0f).axe().drops(id + "_planks");
    Builder(B, id + "_leaves", name + " Leaves").cube().solidClear().leaf()
        .tex(id + "_leaves").hard(0.3f).dropsNothing();
    // Each wood gets its own door and trapdoor; oak keeps the original keys.
    Builder(B, id + "_door", name + " Door").render(RenderKind::Door).solidClear().toggle()
        .tall().tex(id + "_door").hard(0.8f).axe().drops(id + "_door");
    Builder(B, id + "_trapdoor", name + " Trapdoor").render(RenderKind::Trapdoor).solidClear()
        .toggle().tex(id + "_trapdoor").hard(0.8f).axe().drops(id + "_trapdoor");
  }

  // Plants: cross-rendered decoration. solid:false so you walk through them,
  // opaque:false so they do not block light, replaceable so a placed block
  // overwrites them in place. Instant-break, no tool.
  for (const PlantSpec& p : kPlants) {
    Builder(B, p.key, p.name).cross().plant(p.height, 0.45f, /*replaceable=*/true)
        .tex(p.key).hard(0.0f).drops(p.drop ? p.drop : p.key);
  }

  // Materials that get a stair + slab + vertical slab. The two pre-existing
  // stairs keep their original keys, so only their slabs are added.
  std::vector<BuildMat> buildMats = {
      {"planks", ToolType::Axe, 0, 1.0f, "plank_stairs"},
      {"greystone", ToolType::Pick, tier::kWood, 1.5f, "greystone_stairs"},
      {"cobbled", ToolType::Pick, tier::kWood, 2.0f, ""},
      {"polished", ToolType::Pick, tier::kWood, 1.6f, ""},
      {"bricks", ToolType::Pick, tier::kWood, 1.6f, ""},
      {"sandstone", ToolType::Pick, tier::kWood, 1.2f, ""},
  };
  for (const WoodFamily& w : kWoodFamilies) {
    buildMats.push_back({std::string(w.id) + "_planks", ToolType::Axe, 0, 1.0f, ""});
  }
  for (const StoneFamily& s : kStoneFamilies) {
    const std::string id = s.id;
    buildMats.push_back({id + "stone", ToolType::Pick, tier::kWood, 1.5f, ""});
    buildMats.push_back({"polished_" + id, ToolType::Pick, tier::kWood, 1.6f, ""});
    buildMats.push_back({"bricks_" + id, ToolType::Pick, tier::kWood, 1.6f, ""});
  }

  // Look up the base material's texture and display name from what has been
  // built so far, mirroring the JS texOf/nameOf helpers.
  auto findByKey = [&B](const std::string& key) -> const BlockDef* {
    for (const BlockDef& d : B) {
      if (d.key == key) return &d;
    }
    return nullptr;
  };

  for (const BuildMat& m : buildMats) {
    const std::string stairKey = m.existingStair.empty() ? m.key + "_stairs" : m.existingStair;
    const std::string slabKey = m.key + "_slab";
    const std::string vslabKey = m.key + "_vslab";
    buildMaterialKeys_.push_back(m.key);
    stairOf_[m.key] = stairKey;
    slabOf_[m.key] = slabKey;
    vslabOf_[slabKey] = vslabKey;
    slabFromVslab_[vslabKey] = slabKey;

    const BlockDef* base = findByKey(m.key);
    std::string texName = m.key;
    std::string baseName = m.key;
    if (base) {
      baseName = base->name;
      const TextureRef* ref = base->textures.find("all");
      if (!ref) ref = base->textures.find("side");
      if (ref && !ref->id().empty()) {
        // Strip the "block/" folder the builder adds, since tex() re-adds it.
        const std::string& path = ref->id().path();
        texName = path.rfind("block/", 0) == 0 ? path.substr(6) : path;
      }
    }

    auto shaped = [&](const std::string& key, const std::string& suffix, RenderKind kind) {
      Builder b(B, key, baseName + " " + suffix);
      b.render(kind).solidClear().tex(texName).hard(m.hardness).drops(key);
      switch (m.tool) {
        case ToolType::Pick: b.pick(m.tier); break;
        case ToolType::Axe: b.axe(m.tier); break;
        case ToolType::Shovel: b.shovel(m.tier); break;
        case ToolType::None: break;
      }
    };

    if (m.existingStair.empty()) shaped(stairKey, "Stairs", RenderKind::Stair);
    shaped(slabKey, "Slab", RenderKind::Slab);
    shaped(vslabKey, "Vertical Slab", RenderKind::VSlab);
  }

  // A picture frame on a wall. Registered LAST, deliberately: ids are handed out
  // in registration order, and the golden vectors hash a generated chunk's raw
  // voxel ids — so slotting a new block in beside the torch would move every id
  // above it and report the whole world as changed when not one cell of terrain
  // had. Saves are immune either way, because they store blocks by key.
  //
  // solidClear so the mesher emits its frame and the raycast finds it, while
  // collisionBoxes returns nothing for it (see shapes.cpp) so you can stand flat
  // against your own art. Its ground is the wall behind it, which supportOffset
  // knows how to find.
  Builder(B, "canvas", "Painting").render(RenderKind::Painting).tex("canvas").hard(0.3f)
      .solidClear().drops("canvas").needsGround().washesAway();

  // Evil Altar: the monster spawner. Also last, and for the same reason as the
  // painting above — appending is the only way to add a block without moving every
  // id after it and reporting the whole world as changed.
  //
  // No recipe and `dropsNothing`, so there is no route to one in survival at all
  // yet: it is here for the dungeons that will place it, and `--give evil_altar`
  // for anyone who wants to look at it now. Mining it destroys it, which is what
  // makes finding one and choosing not to break it a decision.
  //
  // Emits NO light, and that is load-bearing rather than an omission. Monsters
  // spawn in light level zero, so an altar that glowed would be an altar that had
  // switched itself off. Its darkness is the point; the runes have to look lit
  // without the block being lit, which is a job for the texture.
  Builder(B, "evil_altar", "Evil Altar").cube().solidOpaque()
      .tex3("evil_altar_top", "evil_altar_side", "evil_altar_top").hard(5.0f)
      .pick(tier::kStone).dropsNothing();

  // ---------------------------------------------------------------------------
  // Farmland and the crops. Appended, like everything above, because ids are
  // registration order and the golden vectors hash raw voxel ids.
  //
  // THE PRODUCE IS THE SEED. Planting a carrot yields carrots, so there are no
  // eighteen separate seed items shadowing eighteen separate crops in the recipe
  // book and the creative list. `wild_seeds` from tall grass is the one exception,
  // and it exists only to bootstrap a player who has not found a wild patch yet.
  //
  // Crop blocks are deliberately NOT given block items (see items.cpp): the only way
  // to plant one is to use its produce on farmland, so a crop cannot be stacked in a
  // hotbar slot as a decorative block or placed mid-air on a wall.
  // ---------------------------------------------------------------------------
  Builder(B, "farmland", "Farmland").cube().solidOpaque()
      .tex3("farmland", "loam", "loam").hard(0.6f).shovel().drops("loam");
  // (Fertilised farmland is registered at the very END of this table, not here
  // beside plain farmland where it belongs by subject. See the note down there.)

  {
    struct CropSpec {
      const char* key;      // block key; its tiles are crop_<produce>_N
      const char* name;
      const char* produce;  // the item it yields, and the item that plants it
      int ripeLo, ripeHi;
    };
    // Yields are tuned so a harvest more than repays the seed it cost — a crop that
    // gave back exactly one would make farming a way to lose time politely.
    const CropSpec kCrops[] = {
        {"crop_wheat", "Wheat", "wheat", 1, 3},
        {"crop_barley", "Barley", "barley", 1, 3},
        {"crop_rice", "Rice", "rice", 1, 3},
        {"crop_maize", "Maize", "maize", 1, 2},
        {"crop_carrot", "Carrots", "carrot", 2, 4},
        {"crop_potato", "Potatoes", "potato", 2, 4},
        {"crop_onion", "Onions", "onion", 2, 3},
        {"crop_beetroot", "Beetroots", "beetroot", 1, 3},
        {"crop_garlic", "Garlic", "garlic", 1, 2},
        {"crop_pumpkin", "Pumpkin", "pumpkin", 1, 2},
        {"crop_melon", "Melon", "melon", 1, 2},
        {"crop_tomato", "Tomatoes", "tomato", 2, 4},
        {"crop_chili", "Chilis", "chili", 1, 2},
        {"crop_strawberry", "Strawberries", "strawberry", 2, 3},
        {"crop_blueberry", "Blueberries", "blueberry", 2, 3},
        {"crop_grapes", "Grapes", "grapes", 1, 3},
        {"crop_cabbage", "Cabbage", "cabbage", 1, 2},
        {"crop_soybean", "Soybeans", "soybean", 2, 3},
    };
    for (const CropSpec& c : kCrops) {
      Builder(B, c.key, c.name).crop(c.produce).hard(0.0f)
          .drops(c.produce, 1).ripe(c.produce, c.ripeLo, c.ripeHi);
    }
  }

  // The kitchen: three stations, all craftable at a workbench. Cooking is something
  // you set up once you have a base, not something you trip over.
  Builder(B, "cutting_board", "Cutting Board").cube().solidOpaque()
      .tex3("cutting_board_top", "cutting_board_side", "planks").hard(1.4f).axe()
      .drops("cutting_board").station(Station::Cutting);

  // The stove throws light, the way the forge does — it is the one station you want
  // to be able to find in a dark kitchen.
  Builder(B, "stove", "Stove").cube().solidOpaque()
      .tex3("stove_top", "stove_side", "greystone").hard(3.0f).pick(tier::kWood)
      .drops("stove").station(Station::Stove).emits(8, 1.0f, 0.72f, 0.36f);

  Builder(B, "cooking_pot", "Cooking Pot").cube().solidOpaque()
      .tex3("cooking_pot_top", "cooking_pot_side", "greystone").hard(2.2f)
      .pick(tier::kWood).drops("cooking_pot").station(Station::Pot);

  // Enriched soil. A separate block rather than a bit of farmland's metadata,
  // because the mesher only picks a tile by meta for CROSS blocks — that is how crop
  // stages work. A cube's faces are resolved once at registry-build time, so a
  // fertilised tile has to BE a different block to look like one, and it must look
  // like one or nobody can tell which rows they have already treated.
  //
  // Registered LAST rather than beside plain farmland, where it obviously belongs.
  // It was written there first and that moved every crop and station id up by one,
  // which the golden gate would have reported as the whole v6 world changing when
  // not a single cell of terrain had. Ids are handed out in registration order; new
  // blocks go at the end, however badly they read there.
  Builder(B, "farmland_rich", "Fertilised Farmland").cube().solidOpaque()
      .tex3("farmland_rich", "loam", "loam").hard(0.6f).shovel().drops("loam");

  // ---------------------------------------------------------------------------
  // Resolve the texture slots into six independent faces.
  //
  // The order mirrors the web build's texForFace: an `all` slot wins outright,
  // otherwise top and bottom fall back through side to all. Doing it once here is
  // what lets a future model loader address faces individually without the mesher
  // caring where the id came from.
  // ---------------------------------------------------------------------------
  for (BlockDef& d : B) {
    auto slot = [&d](const char* name) -> const ResourceId* {
      const TextureRef* ref = d.textures.find(name);
      if (!ref || ref->isVariable() || ref->id().empty()) return nullptr;
      return &ref->id();
    };
    const ResourceId* all = slot("all");
    const ResourceId* top = slot("top");
    const ResourceId* side = slot("side");
    const ResourceId* bottom = slot("bottom");

    auto pick = [&](int face) -> ResourceId {
      if (all) return *all;
      if (face == 2) return top ? *top : (side ? *side : ResourceId());
      if (face == 3) return bottom ? *bottom : (side ? *side : ResourceId());
      return side ? *side : ResourceId();
    };
    for (int face = 0; face < 6; ++face) d.faceTextures[face] = pick(face);

    if (const ResourceId* foot = slot("foot")) d.footTexture = *foot;
    if (const ResourceId* stem = slot("stem")) d.stemTexture = *stem;

    // Produce -> crop, so sowing is a map lookup rather than a scan of the table.
    if (d.cropStages > 0 && !d.drop.empty()) cropByProduce_.emplace(d.drop, d.id);

    // Growth stages, resolved the same way and for the same reason: the mesher
    // indexes this array once per crop cell and must never be doing string work.
    if (d.cropStages > 0) {
      d.stageTextures.reserve(static_cast<std::size_t>(d.cropStages));
      for (int s = 0; s < d.cropStages; ++s) {
        const std::string name = "stage" + std::to_string(s);
        const ResourceId* tile = slot(name.c_str());
        d.stageTextures.push_back(tile ? *tile : d.faceTextures[0]);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Lookup tables.
  // ---------------------------------------------------------------------------
  byKey_.reserve(B.size() * 2);
  opaque_.resize(B.size());
  solid_.resize(B.size());
  emit_.resize(B.size());
  std::vector<std::string> plankKeys;
  std::vector<std::string> logKeys;
  for (const BlockDef& d : B) {
    byKey_[d.key] = d.id;
    opaque_[d.id] = d.opaque ? 1 : 0;
    solid_[d.id] = d.solid ? 1 : 0;
    emit_[d.id] = static_cast<std::uint8_t>(d.light);
    if (d.isPlank) plankKeys.push_back(d.key);
    if (d.isLog) logKeys.push_back(d.key);
  }
  tags_["planks"] = std::move(plankKeys);
  tags_["logs"] = std::move(logKeys);

  log::info("blocks: %zu registered (last = %s)", B.size(), B.back().key.c_str());
}

const BlockRegistry& BlockRegistry::get() {
  static const BlockRegistry instance;
  return instance;
}

BlockId BlockRegistry::cropForProduce(std::string_view produceKey) const {
  const auto it = cropByProduce_.find(std::string(produceKey));
  return it == cropByProduce_.end() ? kAir : it->second;
}

BlockId BlockRegistry::idOf(std::string_view key) const {
  auto it = byKey_.find(std::string(key));
  return it == byKey_.end() ? kAir : it->second;
}

bool BlockRegistry::has(std::string_view key) const {
  return byKey_.find(std::string(key)) != byKey_.end();
}

const std::vector<std::string>* BlockRegistry::tag(std::string_view name) const {
  auto it = tags_.find(std::string(name));
  return it == tags_.end() ? nullptr : &it->second;
}

bool BlockRegistry::ingredientMatches(std::string_view required, std::string_view slotKey) const {
  if (!required.empty() && required.front() == '#') {
    const std::vector<std::string>* members = tag(required.substr(1));
    if (!members) return false;
    for (const std::string& member : *members) {
      if (member == slotKey) return true;
    }
    return false;
  }
  return required == slotKey;
}

std::string BlockRegistry::tagRepresentative(std::string_view required) const {
  if (!required.empty() && required.front() == '#') {
    const std::vector<std::string>* members = tag(required.substr(1));
    if (members && !members->empty()) return members->front();
  }
  return std::string(required);
}

const WellKnownBlocks& wk() {
  static const WellKnownBlocks resolved = [] {
    const BlockRegistry& r = BlockRegistry::get();
    WellKnownBlocks w;
    auto id = [&r](const char* key) { return r.idOf(key); };
    w.air = id("air");
    w.bedrock = id("bedrock");
    w.greystone = id("greystone");
    w.cobbled = id("cobbled");
    w.loam = id("loam");
    w.turf = id("turf");
    w.sand = id("sand");
    w.snowturf = id("snowturf");
    w.sandstone = id("sandstone");
    w.shingle = id("shingle");
    w.log = id("log");
    w.leaves = id("leaves");
    w.planks = id("planks");
    w.water = id("water");
    w.emberlight = id("emberlight");
    w.canvas = id("canvas");
    w.glass = id("glass");
    w.umberstone = id("umberstone");
    w.slatestone = id("slatestone");
    w.papyrus = id("papyrus");
    w.ladder = id("ladder");
    w.bed = id("bed");
    w.tall_grass = id("tall_grass");
    w.fern = id("fern");
    w.bush = id("bush");
    w.dead_shrub = id("dead_shrub");
    w.pebbles = id("pebbles");
    w.mushroom_red = id("mushroom_red");
    w.mushroom_brown = id("mushroom_brown");
    w.pine_log = id("pine_log");
    w.pine_leaves = id("pine_leaves");
    w.dusk_log = id("dusk_log");
    w.dusk_leaves = id("dusk_leaves");
    w.birch_log = id("birch_log");
    w.birch_leaves = id("birch_leaves");
    w.palm_log = id("palm_log");
    w.palm_leaves = id("palm_leaves");
    // Order is load-bearing: worldgen indexes this array with a hash roll.
    w.flowers = {id("flower_poppy"), id("flower_daisy"), id("flower_cornflower"),
                 id("flower_dandelion"), id("flower_violet")};

    w.farmland = id("farmland");
    w.farmlandRich = id("farmland_rich");

    // Wild crop lists, per biome. Order is load-bearing here too — worldgen indexes
    // these with a hash roll, so REORDERING ONE CHANGES EVERY WORLD that has already
    // generated a patch from it. Append within a list; do not shuffle.
    //
    // The overlaps are deliberate. A crop in two biomes is a crop you can find
    // without crossing the map, and every biome keeps at least one it alone grows,
    // so travel is rewarded without being demanded.
    w.cropsMeadow = {id("crop_wheat"), id("crop_barley"), id("crop_carrot"),
                     id("crop_onion"), id("crop_tomato"), id("crop_grapes")};
    w.cropsForest = {id("crop_potato"), id("crop_garlic"), id("crop_pumpkin"),
                     id("crop_soybean"), id("crop_blueberry"), id("crop_cabbage")};
    w.cropsBirch = {id("crop_beetroot"), id("crop_strawberry"), id("crop_barley"),
                    id("crop_cabbage")};
    w.cropsDesert = {id("crop_melon"), id("crop_chili"), id("crop_maize")};
    w.cropsSnow = {id("crop_cabbage"), id("crop_beetroot"), id("crop_potato")};

    // Rice is in no biome list on purpose. It answers to WETNESS rather than to
    // biome, so worldgen reaches for it directly in the boggiest cells of anywhere,
    // which is both truer to the crop and one fewer list to keep in step.
    w.cropRice = id("crop_rice");

    w.cropsAll.clear();
    for (const BlockDef& d : r.all()) {
      if (d.cropStages > 0) w.cropsAll.push_back(d.id);
    }
    return w;
  }();
  return resolved;
}

}  // namespace hr::world
