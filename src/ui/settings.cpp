#include "ui/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>

#include "core/log.h"

namespace hr::ui {
namespace {

// js/ui/settings.js:16-40, row for row and in the same order — the order is the tab
// order and the row order inside each tab.
const std::vector<SettingDef>& schema() {
  static const std::vector<SettingDef> table = {
      {"renderDistance", "Render Distance", SettingType::Slider, "Graphics", 3, 12, 1, 7},
      {"fov", "Field of View", SettingType::Slider, "Graphics", 50, 100, 1, 70},
      // The scene is rendered at this fraction of the window and upscaled at the
      // present pass; the interface is always drawn at native size, so text and
      // the hotbar stay sharp whatever this is set to. The renderer has always had
      // the knob — it is what the Low preset's 0.75 uses — but only a preset could
      // reach it. 100 is native; below that trades sharpness for frame rate, above
      // it is supersampling for a machine with headroom to spare.
      {"renderScale", "Render Resolution", SettingType::Slider, "Graphics", 50, 150, 5, 100},
      {"graphicsQuality", "Graphics Quality", SettingType::Select, "Graphics", 0, 0, 0, 0, false,
       "High", {"Low", "Medium", "High", "Ultra"}},
      {"ambientOcclusion", "Ambient Occlusion (SSAO)", SettingType::Toggle, "Graphics", 0, 0, 0,
       0, true},
      {"godRays", "God Rays (sun shafts)", SettingType::Toggle, "Graphics", 0, 0, 0, 0, true},
      {"waterReflections", "Water Reflections (SSR)", SettingType::Toggle, "Graphics", 0, 0, 0, 0,
       true},
      {"castShadows", "Cast Shadows (sun)", SettingType::Toggle, "Graphics", 0, 0, 0, 0, true},
      {"clouds", "Volumetric Clouds", SettingType::Toggle, "Graphics", 0, 0, 0, 0, true},
      {"cloudShadows", "Cloud Shadows", SettingType::Toggle, "Graphics", 0, 0, 0, 0, true},
      // The panorama toggle is gone: there was never a panorama to toggle, so the
      // row switched between the fallback gradient and the same fallback gradient.
      // What replaced it is a picture chosen from the Gallery — empty means the
      // gradient, which is what every fresh install gets.
      {"menuBackground", "Menu Background", SettingType::Text, "Graphics", 0, 0, 0, 0, false,
       "", {}, /*hidden=*/true},
      // No counterpart in the web build, which had the browser's own fullscreen
      // control. Alt+Enter writes this row too, so the two never disagree.
      {"fullscreen", "Fullscreen (Alt+Enter)", SettingType::Toggle, "Graphics", 0, 0, 0, 0,
       false},
      // Borderless wins over fullscreen when both are set; see
      // Window::applyWindowMode.
      {"borderless", "Borderless Window", SettingType::Toggle, "Graphics", 0, 0, 0, 0, false},
      {"vsync", "V-Sync", SettingType::Toggle, "Graphics", 0, 0, 0, 0, true},
      // 0 is "uncapped", which is why the slider starts below any real refresh
      // rate rather than at 30. Independent of V-Sync deliberately: capping below
      // the refresh rate is how you hold a steady 60 on a 144 Hz panel without the
      // input latency vsync adds, and the two are separate knobs everywhere else.
      {"frameLimit", "Frame Rate Limit (0 = off)", SettingType::Slider, "Graphics", 0, 300, 10,
       0},
      {"sensitivity", "Mouse Sensitivity", SettingType::Slider, "Controls", 1, 30, 1, 12},
      {"invertY", "Invert Vertical Look", SettingType::Toggle, "Controls", 0, 0, 0, 0, false},
      // Raw motion has no counterpart in the web build: browser movementX is
      // OS-accelerated, and the ported sensitivity constant is tuned against that,
      // so raw input has to be opt-in or the same number feels wrong.
      {"rawMouse", "Raw Mouse Input (no acceleration)", SettingType::Toggle, "Controls", 0, 0, 0,
       0, false},
      {"uiScale", "Interface Scale", SettingType::Slider, "Controls", 75, 200, 5, 100},
      // --- Gameplay: the player's own, and they follow them between worlds -----
      // How the game is driven and what the interface offers, which is a matter of
      // taste and of the machine rather than of the place.
      {"highStep", "High Step (walk up full blocks)", SettingType::Toggle, "Gameplay", 0, 0, 0, 0,
       false},
      {"minimap", "Minimap (needs the Atlas \xC2\xB7 M)", SettingType::Toggle, "Gameplay", 0, 0, 0,
       0, true},
      {"deathWaypoints", "Death Waypoints on the Atlas", SettingType::Toggle, "Gameplay", 0, 0, 0,
       0, true},
      // This used to gate only the routine toasts, on the argument that silencing a
      // refusal would make a control look broken. In practice the refusals are the
      // ones that repeat — the farming hint fired every time anyone ate a carrot
      // while looking at grass — so the honest label is now the honest behaviour:
      // off means off.
      {"notifications", "Notifications (\"Autosaved\", hints, refusals)", SettingType::Toggle,
       "Gameplay", 0, 0, 0, 0, true},

      // --- Multiplayer: this machine's network, not this world's rules ---------
      //
      // Global scope, because it is a fact about the house's router rather than
      // about the place being played in, and a player who has decided once should
      // not have to decide again per world.
      //
      // OFF, and it has to stay off by default. Every other row here changes how
      // the game looks or plays; this one changes who can send packets to this
      // computer. The label says the important half because the settings screen has
      // no room for a paragraph — the paragraph is in README.md under "Playing with
      // friends over the internet", and the Open to LAN panel repeats the warning
      // at the moment the address is copied, which is the moment it matters.
      {"portForward", "Open a Port for Internet Play (anyone can reach it)",
       SettingType::Toggle, "Multiplayer", 0, 0, 0, 0, false},

      // --- Difficulty: the world's, and they stay with it ----------------------
      // How hard the place is to survive. A world shared with a friend has to be
      // the same world for both of them, so these live in the save and the host
      // owns them.
      {"fallDamage", "Take Fall Damage", SettingType::Toggle, "Difficulty", 0, 0, 0, 0, true},
      {"hunger", "Hunger", SettingType::Toggle, "Difficulty", 0, 0, 0, 0, true},
      {"monsters", "Spawn Monsters", SettingType::Toggle, "Difficulty", 0, 0, 0, 0, true},

      // --- Cheats: the world's too, for the same reason ------------------------
      // Kept apart from Difficulty because turning one of these on is a different
      // kind of decision — it is not making the world easier, it is stepping
      // outside its rules — and a world where somebody can fly should say so.
      //
      // Creative only, and the Cheats tab is empty without it — which is the
      // intended state for now. A survival world has no flight at all: it was a
      // leftover from the web build, it is the one "cheat" that changes how the
      // whole game is played rather than adding a tool, and a double-tap of Space
      // is easy enough to do by accident while jumping that people met it without
      // meaning to. The tab and the category stay wired up for whatever comes next.
      {"flight", "Fly (double-tap Space)", SettingType::Toggle, "Cheats", 0, 0, 0, 0,
       false, "", {}, false, "creativeMode"},
      // Only in a world that was CREATED creative. `worldIsCreative` is not a row;
      // it is a fact about the save, set by App when the world opens, and that is
      // the whole reason a survival world can never turn into a creative one.
      {"creativeMode", "Creative Mode", SettingType::Toggle, "Cheats", 0, 0, 0, 0, false, "", {},
       false, "worldIsCreative"},
      {"noHealth", "No Health or Hunger", SettingType::Toggle, "Cheats", 0, 0, 0, 0, false, "",
       {}, false, "creativeMode"},
      {"noClip", "Walk Through Walls (fly to move)", SettingType::Toggle, "Cheats", 0, 0, 0, 0,
       false, "", {}, false, "creativeMode"},
      // Mining without the mining. Drops nothing: the point is clearing space to
      // build in, and a creative player who wanted the block can ask for one.
      {"instantBreak", "Break Blocks Instantly", SettingType::Toggle, "Cheats", 0, 0, 0, 0,
       false, "", {}, false, "creativeMode"},
      {"locateDungeon", "Locate Nearest Dungeon", SettingType::Action, "Cheats", 0, 0, 0, 0,
       false, "", {}, false, "creativeMode"},

      // --- Debug: how the world is drawn, not what it is ----------------------
      //
      // Every row here is Global, INCLUDING the switch that reveals the rest.
      //
      // The first attempt put that switch in Cheats, where it was world-scoped, on
      // the reasoning that a host should decide whether their world is a place
      // people can see through. That was wrong twice over. It is not a rule about
      // the world — none of these change a single thing about the game, they only
      // change how it is drawn for the one person looking — and making it the
      // world's business meant it lived in the save, defaulted off in every world,
      // and took the entire Debug tab away with it. Which is exactly what "the
      // debug tools do not do anything" turned out to mean.
      //
      // Global also means they are reachable from the main menu, survive between
      // worlds, and stay off the wire, where the world-settings message caps at
      // 64 pairs.
      {"debugTools", "Debug Tools", SettingType::Toggle, "Debug", 0, 0, 0, 0, false},
      // Option order IS the uDebug value, and the first two are 1 and 2 because
      // --debug-view has meant exactly that since before this screen existed. The
      // light views are appended rather than slotted in front, so a documented flag
      // and a shipped README go on being true.
      //
      // "Surface light" rather than "light level": what the shader has is the
      // smoothed per-vertex value the mesher baked, not the integer a cell holds.
      // It shows where the dark is, which is the question being asked, but a cell
      // reading 7 may draw as 6.5 and the label should not pretend otherwise.
      {"debugView", "Debug View", SettingType::Select, "Debug", 0, 0, 0, 0, false, "Off",
       {"Off", "Ambient Occlusion", "Sun Shadow", "Surface Light", "Block Light", "Sky Light"},
       false, "debugTools"},
      {"debugPaths", "Draw Mob Paths", SettingType::Toggle, "Debug", 0, 0, 0, 0, false, "", {},
       false, "debugTools"},
      {"debugChunks", "Draw Chunk Borders", SettingType::Toggle, "Debug", 0, 0, 0, 0, false, "",
       {}, false, "debugTools"},
      {"debugBoxes", "Draw Entity Boxes", SettingType::Toggle, "Debug", 0, 0, 0, 0, false, "",
       {}, false, "debugTools"},
      {"masterVolume", "Master Volume", SettingType::Slider, "Audio", 0, 100, 1, 80},
      {"sfxVolume", "Effects Volume", SettingType::Slider, "Audio", 0, 100, 1, 80},
      {"ambientVolume", "Ambience Volume", SettingType::Slider, "Audio", 0, 100, 1, 40},
      {"uiVolume", "Interface Volume", SettingType::Slider, "Audio", 0, 100, 1, 50},
      // Filed under Audio because sound is the only thing a pack replaces today,
      // and because a player looking for it will be looking at the volume
      // sliders. An Action row: it opens a screen rather than holding a value.
      {"openResourcePacks", "Resource Packs", SettingType::Action, "Audio", 0, 0, 0, 0},
      // Which packs are on, in priority order, joined by '|'. Hidden, like the
      // menu background: it is chosen on the screen that shows the packs, and a
      // row that made you cycle through folder names would be worse than useless.
      {"resourcePacks", "Resource Packs", SettingType::Text, "Audio", 0, 0, 0, 0, false, "", {},
       true},
      // The palette's globally saved colours, hex and joined by '|'. Hidden for the
      // same reason and installation-scoped on purpose: "save everywhere" means
      // exactly that, so it cannot live in a world file. The per-world list is a
      // section of the world save instead.
      {"paletteFavourites", "Saved Colours", SettingType::Text, "Audio", 0, 0, 0, 0, false, "",
       {}, true},
  };
  return table;
}

// Which categories belong to the world rather than the installation. One list,
// consulted by the schema accessor below, so a new row lands in the right scope by
// virtue of the tab it is filed under and cannot disagree with its neighbours.
bool worldCategory(std::string_view category) {
  return category == "Difficulty" || category == "Cheats";
}

// The schema with each row's scope filled in from its category.
const std::vector<SettingDef>& scopedSchema() {
  static const std::vector<SettingDef> table = [] {
    std::vector<SettingDef> out = schema();
    for (SettingDef& def : out) {
      def.scope = worldCategory(def.category) ? SettingScope::World : SettingScope::Global;
    }
    return out;
  }();
  return table;
}

std::string escapeJson(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// A flat object of numbers, booleans and strings is all the schema can produce, so
// this is a scanner rather than a parser: find "key", find the value after the colon,
// hand both to the caller. Anything it does not understand is skipped, which is the
// same tolerance the JS had by wrapping JSON.parse in a try.
void scanFlatJson(const std::string& src,
                  const std::function<void(const std::string&, const std::string&)>& onPair) {
  std::size_t i = 0;
  while (i < src.size()) {
    if (src[i] != '"') {
      ++i;
      continue;
    }
    const std::size_t keyStart = ++i;
    while (i < src.size() && src[i] != '"') {
      if (src[i] == '\\') ++i;
      ++i;
    }
    if (i >= src.size()) return;
    const std::string key = src.substr(keyStart, i - keyStart);
    ++i;
    while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) {
      ++i;
    }
    if (i >= src.size() || src[i] != ':') continue;
    ++i;
    while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) {
      ++i;
    }
    std::string value;
    if (i < src.size() && src[i] == '"') {
      ++i;
      while (i < src.size() && src[i] != '"') {
        if (src[i] == '\\' && i + 1 < src.size()) ++i;
        value.push_back(src[i]);
        ++i;
      }
      if (i < src.size()) ++i;
    } else {
      while (i < src.size() && src[i] != ',' && src[i] != '}' && src[i] != '\n') {
        value.push_back(src[i]);
        ++i;
      }
      while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.pop_back();
    }
    onPair(key, value);
  }
}

}  // namespace

const std::vector<SettingDef>& settingsSchema() { return scopedSchema(); }

SettingScope categoryScope(const std::string& category) {
  return worldCategory(category) ? SettingScope::World : SettingScope::Global;
}

std::vector<std::string> settingsCategories() {
  std::vector<std::string> cats;
  for (const SettingDef& s : scopedSchema()) {
    if (s.hidden) continue;  // a hidden row must not conjure a tab of its own
    if (std::find(cats.begin(), cats.end(), s.category) == cats.end()) cats.emplace_back(s.category);
  }
  return cats;
}

const std::vector<QualityPreset>& qualityPresets() {
  static const std::vector<QualityPreset> presets = {
      // shadowSize is a texel budget over a FIXED +/-72 block box, so it is a
      // sharpness figure, not a range figure: 2048 is 14 shadow texels per block
      // and 4096 is 28. Voxel geometry is axis-aligned and has nothing under a
      // block wide to resolve, so past roughly 8 texels per block the extra
      // resolution is spent on edges that are already straight. Ultra drops to
      // 2048 (0.79 -> 0.27 ms) and High to 1536 (10 texels per block); both stay
      // above the point where a shadow edge starts to look stepped, which 512
      // does not. Everything else is unchanged — the samples are what the presets
      // are for, and after the vertex-buffer fix none of them is expensive.
      {"Low", 0.75f, 8, 24, 0, 0, 10},
      {"Medium", 1.0f, 12, 40, 16, 1024, 14},
      {"High", 1.0f, 16, 48, 24, 1536, 22},
      {"Ultra", 1.0f, 24, 64, 40, 2048, 34},
  };
  return presets;
}

const SettingDef* SettingsStore::find(const std::string& key) const {
  for (const SettingDef& s : scopedSchema()) {
    if (key == s.key) return &s;
  }
  return nullptr;
}

// The one place the scope is resolved. A world-scoped key answers from the open
// world when there is one and from its schema default when there is not — which is
// what makes the main menu show a new world's rules rather than the last one's.
SettingsStore::Value* SettingsStore::value(const std::string& key) {
  if (inWorld_ && isWorldScoped(key)) {
    if (Value* v = valueIn(world_, key)) return v;
  }
  return valueIn(values_, key);
}

const SettingsStore::Value* SettingsStore::value(const std::string& key) const {
  return const_cast<SettingsStore*>(this)->value(key);
}

SettingsStore::Value* SettingsStore::valueIn(std::vector<Value>& where,
                                             const std::string& key) {
  for (Value& v : where) {
    if (v.key == key) return &v;
  }
  return nullptr;
}

const SettingsStore::Value* SettingsStore::valueIn(const std::vector<Value>& where,
                                                   const std::string& key) const {
  for (const Value& v : where) {
    if (v.key == key) return &v;
  }
  return nullptr;
}

bool SettingsStore::isWorldScoped(const std::string& key) const {
  const SettingDef* def = find(key);
  return def && def->scope == SettingScope::World;
}

bool SettingsStore::editable(const std::string& key) const {
  // A gate that is off means the row is not there at all, so there is nothing to
  // edit — checked before scope, because an ungated global row is still unavailable
  // when its gate is off.
  if (!available(key)) return false;
  if (!isWorldScoped(key)) return true;
  // A world's rules need a world to belong to, and belong to whoever is hosting.
  return inWorld_ && !worldLocked_;
}

void SettingsStore::setVirtualFlag(const std::string& key, bool on) {
  virtualFlags_[key] = on;
  ++revision_;
}

void SettingsStore::clearVirtualFlags() {
  if (virtualFlags_.empty()) return;
  virtualFlags_.clear();
  ++revision_;
}

bool SettingsStore::available(const SettingDef& def) const {
  if (!def.gate) return true;
  const std::string gate = def.gate;
  // A synthetic fact first: those are not rows and flag() would report the schema
  // default for one, which for an absent row is false — right by accident today and
  // wrong the moment a gate names something real.
  const auto it = virtualFlags_.find(gate);
  if (it != virtualFlags_.end()) return it->second;
  return flag(gate);
}

bool SettingsStore::available(const std::string& key) const {
  const SettingDef* def = find(key);
  return def ? available(*def) : true;
}

// --- the open world's own values ---------------------------------------------

void SettingsStore::beginWorld(const WorldValues& stored) {
  ++revision_;
  world_.clear();
  for (const SettingDef& s : scopedSchema()) {
    if (s.scope != SettingScope::World) continue;
    Value v;
    v.key = s.key;
    v.number = s.defNumber;
    v.flag = s.defBool;
    v.text = s.defString;
    world_.push_back(std::move(v));
  }
  inWorld_ = true;
  worldLocked_ = false;

  // Anything the save did not carry keeps its default, which is what a brand new
  // world and a save written before this existed both supply.
  for (const auto& [key, raw] : stored) {
    const SettingDef* def = find(key);
    if (!def || def->scope != SettingScope::World) continue;
    Value* v = valueIn(world_, key);
    if (!v) continue;
    switch (def->type) {
      case SettingType::Slider:
        v->number = std::min(std::max(std::strtod(raw.c_str(), nullptr), def->min), def->max);
        break;
      case SettingType::Toggle:
        v->flag = raw == "true" || raw == "1";
        break;
      case SettingType::Select: {
        for (const char* opt : def->options) {
          if (raw == opt) v->text = raw;
        }
        break;
      }
      case SettingType::Text:
        v->text = raw;
        break;
    }
  }
}

void SettingsStore::endWorld() {
  ++revision_;
  world_.clear();
  inWorld_ = false;
  worldLocked_ = false;
}

SettingsStore::WorldValues SettingsStore::worldValues() const {
  WorldValues out;
  for (const SettingDef& s : scopedSchema()) {
    if (s.scope != SettingScope::World) continue;
    const Value* v = valueIn(inWorld_ ? world_ : values_, s.key);
    if (!v) continue;
    switch (s.type) {
      case SettingType::Slider: {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v->number);
        out.emplace_back(s.key, buf);
        break;
      }
      case SettingType::Toggle:
        out.emplace_back(s.key, v->flag ? "true" : "false");
        break;
      case SettingType::Select:
      case SettingType::Text:
        out.emplace_back(s.key, v->text);
        break;
    }
  }
  // Schema order is already stable and is what both the save and the wire want.
  return out;
}

SettingsStore::SettingsStore() { fillDefaults(); }

void SettingsStore::fillDefaults() {
  values_.clear();
  for (const SettingDef& s : scopedSchema()) {
    Value v;
    v.key = s.key;
    v.number = s.defNumber;
    v.flag = s.defBool;
    v.text = s.defString;
    values_.push_back(std::move(v));
  }
}

void SettingsStore::load(const std::string& path) {
  path_ = path;
  fillDefaults();

  std::ifstream in(path, std::ios::binary);
  if (!in) return;
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string src = buffer.str();

  scanFlatJson(src, [this](const std::string& key, const std::string& raw) {
    const SettingDef* def = find(key);
    if (!def) return;  // an unknown key is a setting from a newer build
    // A world's rules are not the installation's. An older settings.json still has
    // these in it and they are deliberately ignored rather than migrated: the
    // world they were last set in is the only place they meant anything.
    if (def->scope == SettingScope::World) return;
    Value* v = valueIn(values_, key);
    if (!v) return;
    switch (def->type) {
      case SettingType::Slider: {
        const double parsed = std::strtod(raw.c_str(), nullptr);
        v->number = std::min(std::max(parsed, def->min), def->max);
        break;
      }
      case SettingType::Toggle:
        v->flag = raw == "true" || raw == "1";
        break;
      case SettingType::Select: {
        // Only an option the schema still offers is accepted, so a renamed preset
        // falls back to the default rather than selecting nothing.
        for (const char* opt : def->options) {
          if (raw == opt) {
            v->text = raw;
            return;
          }
        }
        break;
      }
      case SettingType::Text:
        // No option list to validate against; whatever was written comes back.
        // Anything that has to be valid — the menu background's file has to still
        // exist — is checked by whoever reads it, not here.
        v->text = raw;
        break;
    }
  });
}

bool SettingsStore::save() const {
  if (path_.empty()) return false;
  std::ofstream out(path_, std::ios::binary | std::ios::trunc);
  if (!out) {
    log::warn("could not write %s", path_.c_str());
    return false;
  }
  out << "{\n";
  bool first = true;
  for (const SettingDef& s : schema()) {
    // An Action stores nothing, so there is nothing to write — and writing it
    // anyway produced `"locateDungeon": ,` with no value at all, which is not
    // JSON. The file then failed to parse on the next launch and every setting
    // in it silently reverted to its default. The switch below has a case per
    // type and simply had none for Action, so it emitted the key, the colon, and
    // then nothing.
    if (s.type == SettingType::Action) continue;
    const Value* v = value(s.key);
    if (!v) continue;
    if (!first) out << ",\n";
    first = false;
    out << "  \"" << s.key << "\": ";
    switch (s.type) {
      case SettingType::Slider: {
        // Every slider in the schema steps by whole numbers, so write integers and
        // keep the file readable.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v->number);
        out << buf;
        break;
      }
      case SettingType::Toggle:
        out << (v->flag ? "true" : "false");
        break;
      case SettingType::Select:
      case SettingType::Text:
        out << '"' << escapeJson(v->text) << '"';
        break;
      // Skipped above, before anything was written. Listed so that adding a
      // fourth storable type is a compiler error here rather than another key
      // with an empty value.
      case SettingType::Action: break;
    }
  }
  out << "\n}\n";
  return true;
}

double SettingsStore::number(const std::string& key) const {
  const Value* v = value(key);
  return v ? v->number : 0.0;
}

bool SettingsStore::flag(const std::string& key) const {
  const Value* v = value(key);
  return v ? v->flag : false;
}

const std::string& SettingsStore::text(const std::string& key) const {
  const Value* v = value(key);
  return v ? v->text : empty_;
}

void SettingsStore::setNumber(const std::string& key, double n) {
  ++revision_;
  if (Value* v = value(key)) {
    v->number = n;
    save();
  }
}

void SettingsStore::setFlag(const std::string& key, bool f, bool persist) {
  ++revision_;
  if (Value* v = value(key)) {
    v->flag = f;
    if (persist) save();
  }
}

void SettingsStore::setText(const std::string& key, const std::string& t) {
  ++revision_;
  if (Value* v = value(key)) {
    v->text = t;
    save();
  }
}

int SettingsStore::selectedIndex(const std::string& key) const {
  const SettingDef* def = find(key);
  if (!def) return 0;
  const std::string& current = text(key);
  for (std::size_t i = 0; i < def->options.size(); ++i) {
    if (current == def->options[i]) return static_cast<int>(i);
  }
  return 0;
}

SettingsStore& settings() {
  static SettingsStore store;
  return store;
}

}  // namespace hr::ui
