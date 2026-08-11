#include "ui/theme.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/json.h"

namespace hr::ui {
namespace {

// The name tables, generated from the same lists the enums came from. This is the
// whole reason for the X-macro: a role cannot be added to one and forgotten in the
// other, because there is only one list.
const char* const kPaletteNames[] = {
#define HR_UI_X(id, name, hex) name,
    HR_UI_PALETTE(HR_UI_X)
#undef HR_UI_X
};

const std::uint32_t kPaletteDefaults[] = {
#define HR_UI_X(id, name, hex) hex,
    HR_UI_PALETTE(HR_UI_X)
#undef HR_UI_X
};

const char* const kRoleNames[] = {
#define HR_UI_X(id, name) name,
    HR_UI_ROLES(HR_UI_X)
#undef HR_UI_X
};

const char* const kScalarNames[] = {
#define HR_UI_X(id, name, value) name,
    HR_UI_SCALARS(HR_UI_X)
#undef HR_UI_X
};

const float kScalarDefaults[] = {
#define HR_UI_X(id, name, value) value,
    HR_UI_SCALARS(HR_UI_X)
#undef HR_UI_X
};

static_assert(sizeof(kPaletteNames) / sizeof(kPaletteNames[0]) == kPaletteCount,
              "palette name table and enum disagree");
static_assert(sizeof(kRoleNames) / sizeof(kRoleNames[0]) == kRoleCount - kPaletteCount - 1,
              "role name table and enum disagree");
static_assert(sizeof(kScalarNames) / sizeof(kScalarNames[0]) == kScalarCount,
              "scalar name table and enum disagree");

std::uint8_t clamp8(float v) {
  return static_cast<std::uint8_t>(v < 0.0f ? 0 : (v > 255.0f ? 255 : v + 0.5f));
}

// One hex digit, or -1.
int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "#rgb", "#rrggbb", "#rrggbbaa", with or without the leading '#'. Anything else
// fails, and a failed colour is a line the pack loses rather than a pack that
// does not load.
bool parseColor(std::string_view s, Rgba& out) {
  if (!s.empty() && s.front() == '#') s.remove_prefix(1);
  if (s.size() != 3 && s.size() != 6 && s.size() != 8) return false;
  int v[8] = {};
  for (std::size_t i = 0; i < s.size(); ++i) {
    v[i] = hexDigit(s[i]);
    if (v[i] < 0) return false;
  }
  if (s.size() == 3) {
    // #abc means #aabbcc, which is what every author expects and what CSS does.
    out = {static_cast<std::uint8_t>(v[0] * 17), static_cast<std::uint8_t>(v[1] * 17),
           static_cast<std::uint8_t>(v[2] * 17), 255};
    return true;
  }
  out = {static_cast<std::uint8_t>(v[0] * 16 + v[1]),
         static_cast<std::uint8_t>(v[2] * 16 + v[3]),
         static_cast<std::uint8_t>(v[4] * 16 + v[5]),
         s.size() == 8 ? static_cast<std::uint8_t>(v[6] * 16 + v[7]) : std::uint8_t{255}};
  return true;
}

std::string formatColor(Rgba c) {
  char buffer[16];
  if (c.a == 255) {
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", c.r, c.g, c.b);
  } else {
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
  }
  return buffer;
}

}  // namespace

// ---------------------------------------------------------------------------
// Mixing
// ---------------------------------------------------------------------------

Rgba mix(Rgba a, Rgba b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const auto lerp = [t](std::uint8_t x, std::uint8_t y) {
    return clamp8(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t);
  };
  return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
}

// Toward black and toward white, keeping the alpha the colour already had — a
// darker version of a 55% wash is still a 55% wash.
Rgba shade(Rgba c, float t) { return mix(c, Rgba{0, 0, 0, c.a}, t); }

Rgba tint(Rgba c, float t) { return mix(c, Rgba{255, 255, 255, c.a}, t); }

Rgba withAlpha(Rgba c, float a) {
  c.a = clamp8(a * 255.0f);
  return c;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

const char* nameOf(Role r) {
  const int i = static_cast<int>(r);
  if (i < kPaletteCount) return kPaletteNames[i];
  if (i == kPaletteCount) return "";  // the boundary marker holds no colour
  return kRoleNames[i - kPaletteCount - 1];
}

const char* nameOf(Scalar s) { return kScalarNames[static_cast<int>(s)]; }

bool roleByName(std::string_view name, Role& out) {
  for (int i = 0; i < kRoleCount; ++i) {
    if (i == kPaletteCount) continue;  // never addressable
    if (name == nameOf(static_cast<Role>(i))) {
      out = static_cast<Role>(i);
      return true;
    }
  }
  return false;
}

bool scalarByName(std::string_view name, Scalar& out) {
  for (int i = 0; i < kScalarCount; ++i) {
    if (name == kScalarNames[i]) {
      out = static_cast<Scalar>(i);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Building the table
// ---------------------------------------------------------------------------

void Theme::resetPalette() {
  for (int i = 0; i < kPaletteCount; ++i) colors_[i] = rgb(kPaletteDefaults[i]);
  for (int i = 0; i < kScalarCount; ++i) scalars_[i] = kScalarDefaults[i];
}

// Every role, in one place, expressed as a function of the palette.
//
// The rules here are the theme's actual design, more than the palette is: the
// palette says what colour the accent is, and this says what it means to be a
// button that is hovered, or ink that sits on top of the accent, or a slot that
// is empty. Changing a line here changes what every theme built on this engine
// looks like; changing a palette entry changes one theme.
void Theme::deriveRoles() {
  const auto set = [this](Role r, Rgba c) { colors_[static_cast<int>(r)] = c; };
  const auto get = [this](Role r) { return colors_[static_cast<int>(r)]; };

  const Rgba bg = get(Role::Bg);
  const Rgba panel = get(Role::Panel);
  const Rgba edge = get(Role::Edge);
  const Rgba scrim = get(Role::Scrim);
  const Rgba accent = get(Role::Accent);
  const Rgba text = get(Role::Text);
  const Rgba muted = get(Role::Muted);
  const Rgba danger = get(Role::Danger);
  const Rgba health = get(Role::Health);
  const Rgba hunger = get(Role::Hunger);
  const Rgba breath = get(Role::Breath);
  const Rgba slot = get(Role::Slot);

  // --- surfaces -------------------------------------------------------------
  // A panel on a panel is the same material seen a little closer to the light,
  // and a hover is one step further. Doing it this way rather than with three
  // independent greys is what keeps a theme's surfaces reading as one substance.
  const Rgba panelRaised = tint(panel, 0.055f);
  // A hover is the raised surface caught by the light the accent is coming from —
  // lighter, and a little way toward the accent's hue. A purely neutral lightening
  // was the first version and it read as dead: hovering has to feel like the
  // interface noticing, and in a theme with any character at all the accent is
  // where that character lives.
  const Rgba panelHover = mix(tint(panelRaised, 0.09f), accent, 0.06f);
  set(Role::PanelRaised, panelRaised);
  set(Role::PanelHover, panelHover);
  set(Role::PanelRule, mix(panel, edge, 0.55f));
  set(Role::PanelPending, mix(panel, edge, 0.55f));
  set(Role::CardBg, panel);
  set(Role::CardEdge, edge);
  set(Role::CardShadow, withAlpha(kBlack, 0.5f));
  set(Role::GlassTop, withAlpha(panel, 0.80f));
  set(Role::GlassBottom, withAlpha(shade(panel, 0.40f), 0.86f));
  set(Role::GlassEdge, withAlpha(accent, 0.22f));
  set(Role::GlassHighlight, withAlpha(kWhite, 0.06f));

  set(Role::WashScreen, withAlpha(scrim, 0.55f));
  set(Role::WashPanel, withAlpha(scrim, 0.42f));
  // Opaque on purpose. At 0.97 the history behind a completion list ghosted
  // through between the rows, which is the unreadable overlap that drawing it in
  // the wrong order caused in the first place, only fainter.
  set(Role::WashPopup, withAlpha(shade(scrim, 0.25f), 1.0f));
  set(Role::WashTooltip, withAlpha(mix(scrim, panel, 0.25f), 0.92f));
  // Opaque. Every shadow scales this itself, so the alpha here would only ever be
  // multiplied away — and a shadow role that arrived pre-faded could not be made
  // heavier by a caller that needed it to be.
  set(Role::Shadow, withAlpha(shade(scrim, 0.80f), 1.0f));
  set(Role::SubtleFill, withAlpha(kWhite, 0.04f));
  set(Role::SubtleEdge, withAlpha(kWhite, 0.09f));
  set(Role::OverlayBg, withAlpha(mix(scrim, panel, 0.28f), 0.88f));
  set(Role::OverlayEdge, withAlpha(kWhite, 0.10f));

  // --- the accent family ----------------------------------------------------
  const Rgba accentDeep = shade(accent, 0.32f);
  set(Role::AccentDeep, accentDeep);
  set(Role::AccentEdge, shade(accent, 0.53f));
  set(Role::AccentHi, shade(accent, 0.18f));
  set(Role::AccentLo, shade(accent, 0.44f));
  // Text that sits ON the accent. One token, where there used to be three
  // near-identical near-black greens under three different names — which is
  // exactly the duplication a token table exists to kill.
  set(Role::AccentInk, shade(accent, 0.87f));

  // --- buttons --------------------------------------------------------------
  set(Role::ButtonFill, panelRaised);
  set(Role::ButtonFillHover, panelHover);
  set(Role::ButtonEdge, edge);
  set(Role::ButtonEdgeHover, accentDeep);
  set(Role::ButtonInk, text);
  set(Role::ButtonPrimaryFill, accentDeep);
  set(Role::ButtonPrimaryFillHover, accent);
  set(Role::ButtonPrimaryEdge, shade(accent, 0.53f));
  set(Role::ButtonPrimaryInk, text);
  set(Role::ButtonPrimaryInkHover, shade(accent, 0.87f));
  set(Role::ButtonDangerFillHover, danger);
  set(Role::ButtonDangerEdgeHover, shade(danger, 0.33f));
  set(Role::MenuButtonFill, withAlpha(panelRaised, 0.70f));
  set(Role::MenuButtonFillHover, withAlpha(panelHover, 0.86f));
  set(Role::MenuButtonEdge, withAlpha(kWhite, 0.08f));
  set(Role::MenuButtonEdgeHover, withAlpha(accent, 0.42f));
  set(Role::MenuButtonInkHover, shade(accent, 0.89f));

  // --- tabs -----------------------------------------------------------------
  set(Role::TabFill, panelRaised);
  set(Role::TabFillHover, panelHover);
  set(Role::TabEdge, edge);
  set(Role::TabActiveFill, accentDeep);
  set(Role::TabActiveEdge, shade(accent, 0.53f));
  set(Role::TabActiveInk, tint(accent, 0.85f));

  // --- form controls --------------------------------------------------------
  const Rgba inputBg = shade(panel, 0.56f);
  set(Role::InputBg, inputBg);
  set(Role::InputEdge, kBlack);
  set(Role::InputEdgeFocus, accentDeep);
  set(Role::InputCaret, accent);
  set(Role::InputSelection, withAlpha(accent, 0.30f));
  set(Role::FieldBg, shade(panel, 0.42f));
  set(Role::SearchBg, panelRaised);
  set(Role::SearchEdgeFocus, accent);
  set(Role::TrackBg, inputBg);
  set(Role::TrackFill, accent);

  // --- slots ----------------------------------------------------------------
  set(Role::SlotFill, slot);
  set(Role::SlotEdge, shade(slot, 0.60f));
  set(Role::SlotHover, tint(slot, 0.10f));
  set(Role::SlotResultFill, get(Role::SlotResult));
  set(Role::SlotArmorFill, get(Role::SlotArmor));
  set(Role::SlotSelected, accent);
  // Bright, and themed rather than white. The ring has to read against snow and
  // against a cave, which is what the old flat white was for — a light tint of the
  // accent is just as separable from both and belongs to the theme.
  set(Role::SlotSelectedRing, withAlpha(tint(accent, 0.55f), 0.85f));
  set(Role::HotbarSlotFill, withAlpha(mix(scrim, panel, 0.35f), 0.72f));
  set(Role::SlotCountInk, text);

  // --- ink ------------------------------------------------------------------
  set(Role::InkBody, text);
  set(Role::InkMuted, muted);
  set(Role::InkHeading, text);
  set(Role::InkSubtitle, tint(muted, 0.28f));
  set(Role::InkLead, tint(text, 0.03f));
  // A paragraph rather than a label. Slightly off full brightness because a long
  // block of text set at the same weight as a heading is a wall, and slightly
  // above muted because it is the thing being read rather than an annotation.
  set(Role::InkProse, mix(text, muted, 0.40f));
  set(Role::InkStrong, tint(accent, 0.85f));
  set(Role::InkValue, tint(muted, 0.14f));
  set(Role::InkKbd, tint(accent, 0.62f));
  set(Role::InkBadge, tint(accent, 0.55f));
  set(Role::InkNameplate, tint(text, 0.15f));
  set(Role::InkPlaceholder, withAlpha(muted, 0.55f));
  set(Role::InkFaint, withAlpha(tint(muted, 0.40f), 0.62f));

  // --- meaning --------------------------------------------------------------
  // The empty pip is the full one buried in the background rather than a grey:
  // a row of hearts should read as the same object present or absent, and a
  // neutral grey reads as a different object.
  set(Role::HealthFull, health);
  set(Role::HealthHalf, shade(health, 0.18f));
  set(Role::HealthEmpty, mix(health, bg, 0.82f));
  set(Role::HungerFull, hunger);
  set(Role::HungerHalf, shade(hunger, 0.26f));
  set(Role::HungerEmpty, mix(hunger, bg, 0.84f));
  set(Role::BreathPip, breath);
  set(Role::FuelFill, mix(hunger, danger, 0.35f));
  set(Role::ProgressFill, tint(accent, 0.55f));
  set(Role::ProgressTrack, inputBg);
  set(Role::DangerEdge, shade(danger, 0.33f));

  // --- the world underneath -------------------------------------------------
  // The crosshair is composited with a difference blend so it stays visible
  // against anything, which only works from white.
  set(Role::Crosshair, kWhite);
  set(Role::HurtFlash, shade(health, 0.33f));
  set(Role::MapBg, shade(bg, 0.35f));
  set(Role::MapFog, shade(bg, 0.06f));
  set(Role::MapEdge, edge);
  set(Role::TitleTop, tint(accent, 0.85f));
  set(Role::TitleMid, tint(accent, 0.20f));
  set(Role::TitleBottom, shade(accent, 0.28f));
  set(Role::ToastBg, withAlpha(mix(scrim, panel, 0.30f), 0.92f));
  set(Role::ToastEdge, withAlpha(kWhite, 0.10f));
  set(Role::TooltipBg, withAlpha(mix(scrim, panel, 0.20f), 0.95f));
  set(Role::TooltipEdge, withAlpha(accent, 0.35f));
  set(Role::DebugInk, tint(accent, 0.55f));
  set(Role::DebugBg, withAlpha(scrim, 0.55f));
  set(Role::BadgeBg, withAlpha(scrim, 0.72f));
  set(Role::BadgePano, tint(breath, 0.55f));
  set(Role::Selection, accentDeep);
  set(Role::Focus, accent);
}

void Theme::build(const std::vector<ThemeDoc>& docs) {
  resetPalette();
  deriveRoles();

  // Lowest priority first. Each document sets its palette, re-derives, then pins
  // its roles — so a later pack changing the accent overrides an earlier pack's
  // accent-derived tweaks, and a later pack's explicit pin beats everything. See
  // the ordering note in theme.h; this loop IS that rule.
  for (const ThemeDoc& doc : docs) {
    for (const auto& [role, value] : doc.palette) colors_[static_cast<int>(role)] = value;
    for (const auto& [scalar, value] : doc.scalars) {
      scalars_[static_cast<int>(scalar)] = value;
    }
    if (!doc.palette.empty()) deriveRoles();
    for (const auto& [role, value] : doc.roles) colors_[static_cast<int>(role)] = value;
  }
  ++revision_;
}

std::string Theme::dump() const {
  std::string out;
  out += "{\n";
  out += "  \"palette\": {\n";
  for (int i = 0; i < kPaletteCount; ++i) {
    out += "    \"";
    out += kPaletteNames[i];
    out += "\": \"";
    out += formatColor(colors_[i]);
    out += i + 1 < kPaletteCount ? "\",\n" : "\"\n";
  }
  out += "  },\n";
  out += "  \"roles\": {\n";
  for (int i = kPaletteCount + 1; i < kRoleCount; ++i) {
    out += "    \"";
    out += nameOf(static_cast<Role>(i));
    out += "\": \"";
    out += formatColor(colors_[i]);
    out += i + 1 < kRoleCount ? "\",\n" : "\"\n";
  }
  out += "  },\n";
  out += "  \"scalars\": {\n";
  for (int i = 0; i < kScalarCount; ++i) {
    char number[32];
    std::snprintf(number, sizeof(number), "%g", static_cast<double>(scalars_[i]));
    out += "    \"";
    out += kScalarNames[i];
    out += "\": ";
    out += number;
    out += i + 1 < kScalarCount ? ",\n" : "\n";
  }
  out += "  }\n}\n";
  return out;
}

Theme& theme() {
  static Theme instance;
  return instance;
}

Rgba waypointColor(int index) {
  if (index < 0) index = 0;
  const int slot = index % kWaypointColorCount;
  return theme().color(static_cast<Role>(static_cast<int>(Role::Waypoint1) + slot));
}

// ---------------------------------------------------------------------------
// Reading a theme.json
// ---------------------------------------------------------------------------

bool parseThemeDoc(std::string_view json, std::string_view source, ThemeDoc& doc,
                   std::string* errorOut) {
  doc = ThemeDoc{};
  doc.source = std::string(source);

  std::string parseError;
  const hr::json::Value root = hr::json::parse(json, &parseError);
  if (!root.isObject()) {
    if (errorOut) {
      *errorOut = parseError.empty() ? "theme.json's top level must be an object" : parseError;
    }
    return false;
  }

  // Both sections take colours; which one a name lands in is decided by the
  // token, not by the section it was written under. Putting "accent" under
  // "roles" is a mistake an author will make, and refusing it would teach them
  // nothing that quietly doing the right thing does not.
  const auto readColors = [&](const hr::json::Value& section) {
    for (const auto& [key, value] : section.fields()) {
      Role role{};
      Rgba colour{};
      if (!roleByName(key, role)) {
        doc.unknown.push_back(key);
        continue;
      }
      if (!value.isString() || !parseColor(value.str(), colour)) {
        doc.unknown.push_back(key + " (not a #rrggbb colour)");
        continue;
      }
      if (isPalette(role)) {
        doc.palette.emplace_back(role, colour);
      } else {
        doc.roles.emplace_back(role, colour);
      }
    }
  };

  readColors(root["palette"]);
  readColors(root["roles"]);

  for (const auto& [key, value] : root["scalars"].fields()) {
    Scalar scalar{};
    if (!scalarByName(key, scalar)) {
      doc.unknown.push_back(key);
      continue;
    }
    if (!value.isNumber()) {
      doc.unknown.push_back(key + " (not a number)");
      continue;
    }
    // Bounded rather than trusted. A pack is a folder somebody downloaded, and a
    // hotbar slot of 1e9 layout pixels is an interface nobody can use and a
    // vertex buffer nobody can afford. Zero is allowed: it is how a pack hides
    // something.
    const double v = value.num();
    if (!(v >= 0.0 && v <= 4096.0)) {
      doc.unknown.push_back(key + " (out of range)");
      continue;
    }
    doc.scalars.emplace_back(scalar, static_cast<float>(v));
  }

  return true;
}

}  // namespace hr::ui
