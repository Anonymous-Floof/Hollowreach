// Schema-driven settings, ported from js/ui/settings.js.
//
// The original's central idea is worth keeping exactly: one SCHEMA table drives the
// interface, the persistence and every read, so adding a setting is one row. That is
// why this is a table of rows with a runtime-typed value rather than a struct of
// fields — a struct would need the settings screen to know each field by name, which
// is the thing the original deliberately avoided.
//
// Persistence goes to data/settings.json rather than localStorage. The format is a
// flat object of numbers, booleans and strings, which is all the schema can produce,
// so it stays hand-editable the way the browser's stored blob was.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hr::ui {

// Text is a stored string with no fixed option list and no widget — the menu
// background's file path is the only one today, and it is a `hidden` row. It
// exists because Select deliberately refuses any value not in its `options`, so
// that a renamed preset falls back to the default instead of selecting nothing;
// a free-form path has no option list to be in, so storing it as a Select meant
// it saved correctly and was silently dropped on every load.
enum class SettingType { Slider, Toggle, Select, Text };

// Who a setting belongs to.
//
// Global is the installation's: how the game looks on this screen, how the mouse
// feels in this hand, how loud it is in this room. Carrying those from world to
// world is obviously right, and they have nothing to do with anyone else.
//
// World is the world's own rules — whether monsters come, whether you can fly.
// Those are properties of the place rather than of the player, so they live in the
// save and travel with it when it is shared. In multiplayer they are the HOST's:
// a guest reads them and cannot set them, because a rule that only half the room
// agreed on is not a rule.
//
// The split is by CATEGORY rather than by row, so the settings screen can say
// which tabs belong to the world and a player never has to wonder which of two
// adjacent switches follows them home.
enum class SettingScope { Global, World };

struct SettingDef {
  const char* key;
  const char* label;
  SettingType type;
  const char* category;
  double min = 0, max = 0, step = 1;
  double defNumber = 0;
  bool defBool = false;
  const char* defString = "";
  std::vector<const char*> options;
  // Persisted like any other setting, but not a row on the settings screen. For
  // values the player picks somewhere they can actually see the choice — the menu
  // background is chosen from the Gallery, where the pictures are.
  bool hidden = false;
  // Derived from the category rather than written per row: every row in a category
  // shares it, and settings.cpp static-checks that.
  SettingScope scope = SettingScope::Global;
};

// Graphics-quality presets (js/ui/settings.js:9-14). The renderer's own
// QualitySettings is built from whichever of these is selected.
struct QualityPreset {
  const char* name;
  float scale;
  int ssaoSamples;
  int godraySamples;
  int ssrSteps;
  int shadowSize;
  int cloudSteps;
};
const std::vector<QualityPreset>& qualityPresets();

const std::vector<SettingDef>& settingsSchema();
// Category names in schema order, which is the tab order.
std::vector<std::string> settingsCategories();
// The scope every row in a category shares. Global for a name that is not a
// category, so a caller that gets this wrong shows a tab rather than losing one.
SettingScope categoryScope(const std::string& category);

class SettingsStore {
 public:
  // Defaults are in place from construction, not from load(). Before this, a store
  // that had never been loaded answered false and zero for every key rather than
  // what the schema says — which is invisible in the game, where load() always
  // runs first, and exactly the sort of thing that makes a headless test quietly
  // agree with itself about nothing.
  SettingsStore();

  // Re-fills every key with its default, then overlays whatever the file holds. A
  // missing or malformed file is not an error — it just means defaults, exactly as
  // the JS swallowed a localStorage parse failure.
  void load(const std::string& path);
  bool save() const;

  double number(const std::string& key) const;
  bool flag(const std::string& key) const;
  const std::string& text(const std::string& key) const;

  void setNumber(const std::string& key, double v);
  // `persist` false changes the value for this run only. It exists for command-line
  // flags: --fullscreen is a request about this launch, and rewriting the player's
  // settings.json behind their back because they passed it once is not what it asked
  // for.
  void setFlag(const std::string& key, bool v, bool persist = true);
  void setText(const std::string& key, const std::string& v);

  // Index of `text(key)` in the definition's option list, or 0.
  int selectedIndex(const std::string& key) const;

  const SettingDef* find(const std::string& key) const;

  // --- world scope ----------------------------------------------------------
  //
  // While a world is open, world-scoped keys read and write ITS values instead of
  // the installation's. With no world open they read their schema defaults, which
  // is what the main menu and the new-world screen should show: there is no world
  // whose rules could be displayed, so showing the last one's would be a lie.
  //
  // Stored and returned as strings so this layer has one representation to care
  // about and the save and the wire can both carry it verbatim.
  using WorldValues = std::vector<std::pair<std::string, std::string>>;
  // Keys absent from `stored` fall back to their schema default, which is what a
  // brand new world and a save written before this existed both supply.
  void beginWorld(const WorldValues& stored);
  void endWorld();
  bool inWorld() const { return inWorld_; }
  // Sorted by key, so a save and a wire message are byte-stable.
  WorldValues worldValues() const;

  // A guest does not own the world's rules. Set while joined; the host's values
  // arrive with the world and are refreshed whenever the host changes one.
  void setWorldLocked(bool locked) { worldLocked_ = locked; }
  bool worldLocked() const { return worldLocked_; }

  // Bumped by every write and by opening or leaving a world. App re-applies the
  // cached copies -- player options, renderer quality -- when it moves.
  //
  // A counter rather than a callback because the paths that change a setting are
  // not all in one place and were never going to be: the settings screen, a
  // command-line flag, opening a world, leaving one, and a guest being told the
  // host's rules mid-session. That last one is why this exists at all -- nothing
  // re-applied the world's rules when they arrived over the wire, so a guest went
  // on flying in a world that had just forbidden it.
  std::uint32_t revision() const { return revision_; }

  bool isWorldScoped(const std::string& key) const;
  // Whether the settings screen should let this row be touched at all.
  bool editable(const std::string& key) const;

 private:
  struct Value {
    std::string key;
    double number = 0;
    bool flag = false;
    std::string text;
  };
  Value* value(const std::string& key);
  const Value* value(const std::string& key) const;
  void fillDefaults();
  Value* valueIn(std::vector<Value>& where, const std::string& key);
  const Value* valueIn(const std::vector<Value>& where, const std::string& key) const;

  // The installation's. World-scoped keys sit here too, holding their schema
  // defaults and never written to settings.json — which is what makes reading one
  // with no world open give the default rather than the last world's answer.
  std::vector<Value> values_;
  // The open world's, for world-scoped keys only.
  std::vector<Value> world_;
  std::uint32_t revision_ = 0;
  bool inWorld_ = false;
  bool worldLocked_ = false;
  std::string path_;
  std::string empty_;
};

// The process-wide store, matching the JS module-level `Settings` singleton.
SettingsStore& settings();

}  // namespace hr::ui
