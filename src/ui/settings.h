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

#include <string>
#include <vector>

namespace hr::ui {

enum class SettingType { Slider, Toggle, Select };

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

class SettingsStore {
 public:
  // Fills every key with its default, then overlays whatever the file holds. A
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

 private:
  struct Value {
    std::string key;
    double number = 0;
    bool flag = false;
    std::string text;
  };
  Value* value(const std::string& key);
  const Value* value(const std::string& key) const;

  std::vector<Value> values_;
  std::string path_;
  std::string empty_;
};

// The process-wide store, matching the JS module-level `Settings` singleton.
SettingsStore& settings();

}  // namespace hr::ui
