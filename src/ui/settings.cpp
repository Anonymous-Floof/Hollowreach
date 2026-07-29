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
      {"menuPanorama", "Menu Panorama Background", SettingType::Toggle, "Graphics", 0, 0, 0, 0,
       true},
      // No counterpart in the web build, which had the browser's own fullscreen
      // control. Alt+Enter writes this row too, so the two never disagree.
      {"fullscreen", "Fullscreen (Alt+Enter)", SettingType::Toggle, "Graphics", 0, 0, 0, 0,
       false},
      {"sensitivity", "Mouse Sensitivity", SettingType::Slider, "Controls", 1, 30, 1, 12},
      {"invertY", "Invert Vertical Look", SettingType::Toggle, "Controls", 0, 0, 0, 0, false},
      // Raw motion has no counterpart in the web build: browser movementX is
      // OS-accelerated, and the ported sensitivity constant is tuned against that,
      // so raw input has to be opt-in or the same number feels wrong.
      {"rawMouse", "Raw Mouse Input (no acceleration)", SettingType::Toggle, "Controls", 0, 0, 0,
       0, false},
      {"uiScale", "Interface Scale", SettingType::Slider, "Controls", 75, 200, 5, 100},
      {"fallDamage", "Take Fall Damage", SettingType::Toggle, "Gameplay", 0, 0, 0, 0, true},
      {"hunger", "Hunger", SettingType::Toggle, "Gameplay", 0, 0, 0, 0, true},
      {"monsters", "Spawn Monsters", SettingType::Toggle, "Gameplay", 0, 0, 0, 0, true},
      // Off by default, where the web build had it on. A double-tap of Space is easy
      // to do by accident while jumping, and drifting off the ground is a strange
      // first thing to have happen in a survival world.
      {"flight", "Allow Flight (double-tap Space)", SettingType::Toggle, "Gameplay", 0, 0, 0, 0,
       false},
      {"highStep", "High Step (walk up full blocks)", SettingType::Toggle, "Gameplay", 0, 0, 0, 0,
       false},
      {"minimap", "Minimap (needs the Atlas \xC2\xB7 N)", SettingType::Toggle, "Gameplay", 0, 0, 0,
       0, true},
      {"deathWaypoints", "Death Waypoints on the Atlas", SettingType::Toggle, "Gameplay", 0, 0, 0,
       0, true},
      {"masterVolume", "Master Volume", SettingType::Slider, "Audio", 0, 100, 1, 80},
      {"sfxVolume", "Effects Volume", SettingType::Slider, "Audio", 0, 100, 1, 80},
      {"ambientVolume", "Ambience Volume", SettingType::Slider, "Audio", 0, 100, 1, 40},
      {"uiVolume", "Interface Volume", SettingType::Slider, "Audio", 0, 100, 1, 50},
  };
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

const std::vector<SettingDef>& settingsSchema() { return schema(); }

std::vector<std::string> settingsCategories() {
  std::vector<std::string> cats;
  for (const SettingDef& s : schema()) {
    if (std::find(cats.begin(), cats.end(), s.category) == cats.end()) cats.emplace_back(s.category);
  }
  return cats;
}

const std::vector<QualityPreset>& qualityPresets() {
  static const std::vector<QualityPreset> presets = {
      {"Low", 0.75f, 8, 24, 0, 0, 10},
      {"Medium", 1.0f, 12, 40, 16, 1024, 14},
      {"High", 1.0f, 16, 48, 24, 2048, 22},
      {"Ultra", 1.0f, 24, 64, 40, 4096, 34},
  };
  return presets;
}

const SettingDef* SettingsStore::find(const std::string& key) const {
  for (const SettingDef& s : schema()) {
    if (key == s.key) return &s;
  }
  return nullptr;
}

SettingsStore::Value* SettingsStore::value(const std::string& key) {
  for (Value& v : values_) {
    if (v.key == key) return &v;
  }
  return nullptr;
}

const SettingsStore::Value* SettingsStore::value(const std::string& key) const {
  for (const Value& v : values_) {
    if (v.key == key) return &v;
  }
  return nullptr;
}

void SettingsStore::load(const std::string& path) {
  path_ = path;
  values_.clear();
  for (const SettingDef& s : schema()) {
    Value v;
    v.key = s.key;
    v.number = s.defNumber;
    v.flag = s.defBool;
    v.text = s.defString;
    values_.push_back(std::move(v));
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) return;
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string src = buffer.str();

  scanFlatJson(src, [this](const std::string& key, const std::string& raw) {
    const SettingDef* def = find(key);
    Value* v = value(key);
    if (!def || !v) return;  // an unknown key is a setting from a newer build
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
        out << '"' << escapeJson(v->text) << '"';
        break;
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
  if (Value* v = value(key)) {
    v->number = n;
    save();
  }
}

void SettingsStore::setFlag(const std::string& key, bool f, bool persist) {
  if (Value* v = value(key)) {
    v->flag = f;
    if (persist) save();
  }
}

void SettingsStore::setText(const std::string& key, const std::string& t) {
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
