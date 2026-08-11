// Every colour and measurement the interface draws with, as a runtime table a
// resource pack can replace.
//
// This used to be sixty `constexpr Rgba` constants transcribed from the web
// build's stylesheet. Constants are the one thing a pack can never reach: a
// value baked into the binary at compile time is not a value anybody can
// override, so "resource packs could supply UI" was impossible by construction
// rather than merely unimplemented. Everything here is now looked up at draw
// time from a table that is built at load time.
//
// ---------------------------------------------------------------------------
// Two tiers, and why
//
// A **palette** of the two dozen colours a theme actually decides, and ~110
// **roles** derived from it — `button.primary.fill`, `slot.edge`,
// `chat.whisper`. Roles are what the drawing code names; the palette is what a
// theme file sets.
//
// The alternative was one flat table of 130 names. That sounds simpler until
// somebody writes a pack: a flat table means getting 130 colours to agree with
// each other by hand, and the first mistake is a button whose border no longer
// belongs to the theme it sits in. With two tiers a complete, coherent theme is
//
//     { "palette": { "accent": "#e8a13c", "panel": "#221c15" } }
//
// and everything that hangs off accent and panel moves with them. A pack that
// wants one specific thing different pins that one role instead:
//
//     { "roles":   { "button.danger.fill.hover": "#8e2a1e" } }
//
// ---------------------------------------------------------------------------
// Names are for authors, indices are for frames
//
// A token is written `"panel.raised"` in JSON and resolved **once, at load**,
// into a dense array. Drawing code says `col(Role::PanelRaised)`, which is an
// array index — the same cost the old constants had, which is what makes it
// acceptable to do three hundred times a frame.
//
// The enum and the name table are generated from one X-macro list for a single
// reason: two hand-maintained lists of a hundred names WILL drift, and the drift
// is silent. A role that loses its name still compiles and still draws — it just
// becomes a colour no pack can ever address, and nobody finds out.
//
// ---------------------------------------------------------------------------
// How a build is ordered
//
// Documents are applied lowest priority first — the built-in theme, then each
// enabled pack. Each document in turn:
//
//   1. sets whichever palette entries it names,
//   2. re-derives every role from the palette,
//   3. sets whichever roles it names.
//
// So a later pack changing `accent` re-derives the accent-hued roles and
// therefore overrides an earlier pack's tweaks to them, while a later pack's
// explicit role pin beats everything. One sentence, and it is the only ordering
// rule there is: **the last document to speak about a colour wins**, where
// setting a palette entry counts as speaking about everything derived from it.

#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "resource/image.h"

namespace hr::ui {

// ---------------------------------------------------------------------------
// Colour construction and mixing
// ---------------------------------------------------------------------------

// #rrggbb, fully opaque.
constexpr Rgba rgb(std::uint32_t hex) {
  return {static_cast<std::uint8_t>((hex >> 16) & 0xFFu),
          static_cast<std::uint8_t>((hex >> 8) & 0xFFu),
          static_cast<std::uint8_t>(hex & 0xFFu), 255};
}

// CSS rgba(r, g, b, a) with a in 0..1. Kept because it is how every wash and
// shadow in the interface is still most readably written at the call site.
inline Rgba rgba(int r, int g, int b, double a) {
  const long v = std::lround(a * 255.0);
  const std::uint8_t alpha = static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
  return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
          static_cast<std::uint8_t>(b), alpha};
}

// Scales the alpha a colour already has. `fade(c, 0.5)` halves it.
inline Rgba fade(Rgba c, double a) {
  const long v = std::lround(static_cast<double>(c.a) * a);
  c.a = static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
  return c;
}

// Linear mix in 8-bit sRGB. Not gamma correct, and deliberately so: every value
// in this file was picked by eye against the others in the same space, and
// mixing "properly" would move all of them relative to the ones that were not
// mixed at all.
Rgba mix(Rgba a, Rgba b, float t);
// Toward black and toward white. The two derivations almost every role uses.
Rgba shade(Rgba c, float t);
Rgba tint(Rgba c, float t);
// Replaces the alpha outright, rather than scaling it the way fade() does.
Rgba withAlpha(Rgba c, float a);

// Not theme decisions. White is white in every theme; a shadow's base is black
// in every theme. Anything that genuinely varies is a token below.
inline constexpr Rgba kTransparent {0, 0, 0, 0};
inline constexpr Rgba kWhite = rgb(0xffffff);
inline constexpr Rgba kBlack = rgb(0x000000);

// ---------------------------------------------------------------------------
// The palette
//
// What a theme decides. Everything else is derived from these, so a pack that
// sets only these gets a complete and internally consistent interface.
//
// The three chat tints are here rather than derived because they are three
// distinct HUES carrying three distinct meanings, and no derivation from a
// single accent can produce three hues. Same argument for the seven waypoint
// swatches: they exist to be told apart from each other.
// ---------------------------------------------------------------------------

// The built-in theme is **Lantern**: warm amber on a near-black warm brown-grey,
// the interface as seen by lamplight underground. Two of its choices are worth
// knowing before changing anything here.
//
// The ground is warm, not neutral. Every grey below has more red than blue in it,
// which is what stops the amber accent reading as a yellow stain on a cold
// surface — the failing that made the previous green-on-blue-grey theme look
// washed out. Shifting `panel` toward blue and leaving `accent` alone would undo
// most of this theme in one line.
//
// Hunger is a brown, not an amber. The obvious food colour sits 29/255 from the
// lantern accent, which is not enough to tell apart at pip size in the corner of
// an eye that is looking at something else. #a5661c is 67 from the accent and 59
// from the heart red — the best separation from BOTH that still reads as food.
#define HR_UI_PALETTE(X)                                                            \
  /* Ground and structure. */                                                       \
  X(Bg,            "bg",             0x14110d)  /* behind everything */              \
  X(Panel,         "panel",          0x221c15)  /* a card, a dialog */               \
  X(Edge,          "edge",           0x0b0908)  /* the line around one */            \
  X(Scrim,         "scrim",          0x0a0806)  /* every wash and shadow base */     \
  /* The accent, and the ink on top of things. */                                    \
  X(Accent,        "accent",         0xe8a13c)  /* lantern amber */                  \
  X(Text,          "text",           0xf2e9dc)  /* warm bone */                      \
  X(Muted,         "muted",          0xa89880)                                       \
  /* Meaning. Never decorative — if one of these is on screen it is saying          \
     something, which is why they survive a theme change with their hue intact. */   \
  X(Danger,        "danger",         0xd9584a)                                       \
  X(Health,        "health",         0xe0463a)                                       \
  X(Hunger,        "hunger",         0xa5661c)  /* see the note above */             \
  X(Breath,        "breath",         0x6ec8ff)  /* the one cold thing, on purpose */ \
  /* Surfaces that are not the panel they sit on. A slot is its own material. */     \
  X(Slot,          "slot",           0x2b2219)                                       \
  X(SlotResult,    "slot.result",    0x3d2c17)  /* the warm one an output sits in */ \
  X(SlotArmor,     "slot.armor",     0x23272b)  /* cooler: armour is not loot */     \
  /* Chat. Three tints and no more: a line's meaning belongs to whoever wrote it,   \
     and a box that colours text nine ways is one nobody can read at a glance. */    \
  X(ChatSystem,    "chat.system",    0x8fc6e8)  /* the world talking */              \
  X(ChatWhisper,   "chat.whisper",   0xc2a3e0)  /* to or from one person */          \
  X(ChatReply,     "chat.reply",     0x9de0b4)  /* the answer to a command */        \
  /* Atlas waypoint swatches, in cycle order. */                                     \
  X(Waypoint1,     "waypoint.1",     0xe8c84a)                                       \
  X(Waypoint2,     "waypoint.2",     0x52b6e8)                                       \
  X(Waypoint3,     "waypoint.3",     0x7ade6a)                                       \
  X(Waypoint4,     "waypoint.4",     0xe07ad0)                                       \
  X(Waypoint5,     "waypoint.5",     0xf08748)                                       \
  X(Waypoint6,     "waypoint.6",     0xa48aff)                                       \
  X(Waypoint7,     "waypoint.7",     0xf0f0f0)

// ---------------------------------------------------------------------------
// The roles
//
// What the drawing code names. Every one is derived in deriveRoles() — there is
// no role with a hardcoded default, because a role that ignored the palette
// would be a colour that silently refused to follow a theme.
// ---------------------------------------------------------------------------

#define HR_UI_ROLES(X)                                                              \
  /* --- surfaces ------------------------------------------------------------ */   \
  X(PanelRaised,        "panel.raised")       /* a panel sitting on a panel */       \
  X(PanelHover,         "panel.hover")                                               \
  X(PanelRule,          "panel.rule")         /* the hairline between rows */        \
  X(PanelPending,       "panel.pending")      /* a map tile not drawn yet */         \
  X(CardBg,             "card.bg")                                                   \
  X(CardEdge,           "card.edge")                                                 \
  X(CardShadow,         "card.shadow")                                               \
  X(GlassTop,           "glass.top")          /* the menu card's gradient */         \
  X(GlassBottom,        "glass.bottom")                                              \
  X(GlassEdge,          "glass.edge")                                                \
  X(GlassHighlight,     "glass.highlight")    /* the 1px inset top line */           \
  /* Washes. Named rather than written as alphas at the call site, because the      \
     whole point is that a theme can decide how heavily its interface veils what    \
     is behind it — and forty scattered rgba(0,0,0,0.55) could not be told. */       \
  X(WashScreen,         "wash.screen")        /* a screen over the world */          \
  X(WashPanel,          "wash.panel")         /* a floating bar over the world */    \
  X(WashPopup,          "wash.popup")         /* opaque: a completion list */        \
  X(WashTooltip,        "wash.tooltip")                                              \
  /* Opaque, and scaled at the call site: `col(Role::Shadow, 0.62f)`. A theme with  \
     a pale ground wants a different shadow from one with a black ground, and a     \
     hardcoded rgba(0,0,0,x) is the one thing that could never follow it. */         \
  X(Shadow,             "shadow")                                                    \
  /* A surface that is barely there — a card inside a card, on glass. */             \
  X(SubtleFill,         "subtle.fill")                                               \
  X(SubtleEdge,         "subtle.edge")                                               \
  /* A panel floating over the live world rather than over a dimmed screen. */       \
  X(OverlayBg,          "overlay.bg")                                                \
  X(OverlayEdge,        "overlay.edge")                                              \
  /* --- the accent family -------------------------------------------------- */    \
  X(AccentDeep,         "accent.deep")        /* the resting primary fill */         \
  X(AccentEdge,         "accent.edge")                                               \
  X(AccentHi,           "accent.hi")          /* gradient ends */                    \
  X(AccentLo,           "accent.lo")                                                 \
  X(AccentInk,          "accent.ink")         /* text ON the accent */               \
  /* --- buttons ------------------------------------------------------------- */   \
  X(ButtonFill,         "button.fill")                                               \
  X(ButtonFillHover,    "button.fill.hover")                                         \
  X(ButtonEdge,         "button.edge")                                               \
  X(ButtonEdgeHover,    "button.edge.hover")                                         \
  X(ButtonInk,          "button.ink")                                                \
  X(ButtonPrimaryFill,      "button.primary.fill")                                   \
  X(ButtonPrimaryFillHover, "button.primary.fill.hover")                             \
  X(ButtonPrimaryEdge,      "button.primary.edge")                                   \
  X(ButtonPrimaryInk,       "button.primary.ink")                                    \
  X(ButtonPrimaryInkHover,  "button.primary.ink.hover")                              \
  X(ButtonDangerFillHover,  "button.danger.fill.hover")                              \
  X(ButtonDangerEdgeHover,  "button.danger.edge.hover")                              \
  X(MenuButtonFill,     "menu.button.fill")                                          \
  X(MenuButtonFillHover, "menu.button.fill.hover")                                   \
  X(MenuButtonEdge,     "menu.button.edge")                                          \
  X(MenuButtonEdgeHover, "menu.button.edge.hover")                                   \
  X(MenuButtonInkHover, "menu.button.ink.hover")                                     \
  /* --- tabs ---------------------------------------------------------------- */   \
  X(TabFill,            "tab.fill")                                                  \
  X(TabFillHover,       "tab.fill.hover")                                            \
  X(TabEdge,            "tab.edge")                                                  \
  X(TabActiveFill,      "tab.active.fill")                                           \
  X(TabActiveEdge,      "tab.active.edge")                                           \
  X(TabActiveInk,       "tab.active.ink")                                            \
  /* --- form controls ------------------------------------------------------- */   \
  X(InputBg,            "input.bg")                                                  \
  X(InputEdge,          "input.edge")                                                \
  X(InputEdgeFocus,     "input.edge.focus")                                          \
  X(InputCaret,         "input.caret")                                               \
  X(InputSelection,     "input.selection")                                           \
  X(FieldBg,            "field.bg")           /* an inset value, not an input */     \
  X(SearchBg,           "search.bg")                                                 \
  X(SearchEdgeFocus,    "search.edge.focus")                                          \
  X(TrackBg,            "track.bg")           /* a slider's groove */                \
  X(TrackFill,          "track.fill")                                                \
  /* --- slots --------------------------------------------------------------- */   \
  X(SlotFill,           "slot.fill")                                                 \
  X(SlotEdge,           "slot.edge")                                                 \
  X(SlotHover,          "slot.hover")                                                \
  X(SlotResultFill,     "slot.result.fill")                                          \
  X(SlotArmorFill,      "slot.armor.fill")                                           \
  X(SlotSelected,       "slot.selected")      /* the hotbar's current slot */        \
  X(SlotSelectedRing,   "slot.selected.ring")                                        \
  X(HotbarSlotFill,     "hotbar.slot.fill")   /* translucent: the world shows */     \
  X(SlotCountInk,       "slot.count.ink")                                            \
  /* --- ink ----------------------------------------------------------------- */   \
  X(InkBody,            "ink.body")                                                  \
  X(InkMuted,           "ink.muted")                                                 \
  X(InkHeading,         "ink.heading")                                               \
  X(InkSubtitle,        "ink.subtitle")                                              \
  X(InkLead,            "ink.lead")           /* the opening line of a panel */      \
  X(InkProse,           "ink.prose")          /* a paragraph, not a label */         \
  X(InkStrong,          "ink.strong")                                                \
  X(InkValue,           "ink.value")          /* a setting's current value */        \
  X(InkKbd,             "ink.kbd")            /* a key name */                       \
  X(InkBadge,           "ink.badge")                                                 \
  X(InkNameplate,       "ink.nameplate")                                             \
  X(InkPlaceholder,     "ink.placeholder")                                           \
  X(InkFaint,           "ink.faint")          /* a version tag, a signature */       \
  /* --- meaning ------------------------------------------------------------- */   \
  X(HealthFull,         "health.full")                                               \
  X(HealthHalf,         "health.half")                                               \
  X(HealthEmpty,        "health.empty")                                              \
  X(HungerFull,         "hunger.full")                                               \
  X(HungerHalf,         "hunger.half")                                               \
  X(HungerEmpty,        "hunger.empty")                                              \
  X(BreathPip,          "breath.pip")                                                \
  X(FuelFill,           "fuel.fill")                                                 \
  X(ProgressFill,       "progress.fill")      /* smelting, and the break bar */      \
  X(ProgressTrack,      "progress.track")                                            \
  X(DangerEdge,         "danger.edge")                                               \
  /* --- the world underneath ------------------------------------------------ */   \
  X(Crosshair,          "crosshair")                                                 \
  X(HurtFlash,          "hurt.flash")                                                \
  X(MapBg,              "map.bg")                                                    \
  X(MapFog,             "map.fog")                                                   \
  X(MapEdge,            "map.edge")                                                  \
  X(TitleTop,           "title.top")          /* the three-stop menu title */        \
  X(TitleMid,           "title.mid")                                                 \
  X(TitleBottom,        "title.bottom")                                              \
  X(ToastBg,            "toast.bg")                                                  \
  X(ToastEdge,          "toast.edge")                                                \
  X(TooltipBg,          "tooltip.bg")                                                \
  X(TooltipEdge,        "tooltip.edge")                                              \
  X(DebugInk,           "debug.ink")                                                 \
  X(DebugBg,            "debug.bg")                                                  \
  X(BadgeBg,            "badge.bg")                                                  \
  X(BadgePano,          "badge.pano")         /* the Gallery's panorama marker */    \
  X(Selection,          "selection")          /* highlighted text in the log */      \
  X(Focus,              "focus")              /* the keyboard focus ring */

enum class Role : std::uint16_t {
#define HR_UI_X(id, name, hex) id,
  HR_UI_PALETTE(HR_UI_X)
#undef HR_UI_X
  // Boundary marker. Holds no colour; everything below it is derived, everything
  // above it is set by a theme. Nothing indexes it.
  PaletteEnd,
#define HR_UI_X(id, name) id,
  HR_UI_ROLES(HR_UI_X)
#undef HR_UI_X
  Count,
};

inline constexpr int kPaletteCount = static_cast<int>(Role::PaletteEnd);
inline constexpr int kRoleCount = static_cast<int>(Role::Count);

// ---------------------------------------------------------------------------
// Measurements
//
// One tier, because there is no equivalent of "derive a whole family from the
// accent" for a number — a card's corner radius and a hotbar slot's size are
// simply two independent decisions. They are here rather than as constants for
// the same reason the colours are: a pack that wants a denser interface has to
// be able to say so.
//
// All in layout pixels, before Ui2D multiplies by the ui scale.
// ---------------------------------------------------------------------------

#define HR_UI_SCALARS(X)                                                            \
  /* Rhythm. Most padding and gap in the interface is a multiple of one of these. */ \
  X(GapTight,        "gap.tight",         4.0f)                                      \
  X(Gap,             "gap",               8.0f)                                      \
  X(GapWide,         "gap.wide",          16.0f)                                     \
  X(PadTight,        "pad.tight",         8.0f)                                      \
  X(Pad,             "pad",               16.0f)                                     \
  X(PadWide,         "pad.wide",          28.0f)                                     \
  /* Corners and lines. */                                                           \
  X(RadiusSmall,     "radius.small",      6.0f)                                      \
  X(Radius,          "radius",            9.0f)                                      \
  X(RadiusCard,      "radius.card",       14.0f)                                     \
  X(BorderThin,      "border.thin",       1.0f)                                      \
  X(Border,          "border",            2.0f)                                      \
  /* A control's own padding, kept apart from the layout rhythm above. A theme      \
     that wants a denser interface usually means denser CONTROLS — smaller buttons  \
     and tighter rows — and not necessarily narrower gaps between the cards they    \
     sit in, so conflating the two would make `pad` unusable for either. */          \
  X(ControlPadY,     "control.pad.y",     12.0f)                                     \
  X(ControlPadX,     "control.pad.x",     18.0f)                                     \
  /* Type. A theme that wants larger text everywhere moves the scale, not each      \
     of forty call sites. */                                                         \
  X(FontSmall,       "font.small",        12.0f)                                     \
  X(FontBody,        "font.body",         14.0f)                                     \
  X(FontLarge,       "font.large",        16.0f)                                     \
  X(FontHeading,     "font.heading",      20.0f)                                     \
  /* The HUD. */                                                                     \
  X(HotbarSlot,      "hotbar.slot",       52.0f)                                      \
  X(HotbarGap,       "hotbar.gap",        4.0f)                                       \
  X(HotbarBottom,    "hotbar.bottom",     16.0f)                                      \
  X(HudColumnGap,    "hud.column.gap",    8.0f)                                       \
  X(PipSize,         "pip.size",          16.0f)                                      \
  X(PipGap,          "pip.gap",           2.0f)                                       \
  X(BarsGap,         "bars.gap",          14.0f)                                      \
  X(MinimapSize,     "minimap.size",      168.0f)                                     \
  X(MinimapInset,    "minimap.inset",     14.0f)                                      \
  X(ToastTop,        "toast.top",         196.0f)                                     \
  X(ToastRight,      "toast.right",       14.0f)                                      \
  /* Containers. */                                                                   \
  X(InvSlot,         "inv.slot",          46.0f)                                      \
  X(InvSlotGap,      "inv.slot.gap",      4.0f)                                        \
  X(InvPanelGap,     "inv.panel.gap",     18.0f)                                       \
  X(RecipeCell,      "recipe.cell",       24.0f)                                       \
  X(RecipeIcon,      "recipe.icon",       22.0f)                                       \
  X(RecipeIconOut,   "recipe.icon.out",   30.0f)

enum class Scalar : std::uint16_t {
#define HR_UI_X(id, name, value) id,
  HR_UI_SCALARS(HR_UI_X)
#undef HR_UI_X
  Count,
};

inline constexpr int kScalarCount = static_cast<int>(Scalar::Count);

// ---------------------------------------------------------------------------
// A theme document
//
// One layer: the built-in theme, or one pack's theme.json. Kept as parsed
// name/value pairs rather than as a resolved table, because the ordering rule at
// the top of this file needs to know WHICH keys a document mentioned, not merely
// what the table looked like after it.
// ---------------------------------------------------------------------------

struct ThemeDoc {
  // Where it came from, for the log and for the error a bad file produces.
  std::string source;
  std::vector<std::pair<Role, Rgba>> palette;
  std::vector<std::pair<Role, Rgba>> roles;
  std::vector<std::pair<Scalar, float>> scalars;

  // Names the file used that this build has never heard of. Not an error: a pack
  // written for a later version naming a role that does not exist yet should lose
  // that one line, not fail to load. Reported so an author can see the typo.
  std::vector<std::string> unknown;

  bool empty() const { return palette.empty() && roles.empty() && scalars.empty(); }
};

// Parses one theme.json. Returns false and fills `errorOut` only for a file that
// is not JSON at all or whose top level is not an object — every other problem is
// tolerated and recorded in `doc.unknown`, because one mistyped line should cost
// that line rather than the pack.
bool parseThemeDoc(std::string_view json, std::string_view source, ThemeDoc& doc,
                   std::string* errorOut);

// Name lookup, for the parser and for --dump-theme. Returns false when the name
// belongs to no token.
bool roleByName(std::string_view name, Role& out);
bool scalarByName(std::string_view name, Scalar& out);
const char* nameOf(Role r);
const char* nameOf(Scalar s);
// Whether a role is a palette entry, which is what decides which of the three
// build steps sets it.
constexpr bool isPalette(Role r) { return static_cast<int>(r) < kPaletteCount; }

class Theme {
 public:
  Theme() { build({}); }

  // Rebuilds from the built-in defaults plus `docs`, lowest priority first. This
  // is the only way the table changes — there is no setter, because a theme that
  // could be poked one colour at a time would have no defined answer to "what
  // does a pack being turned off restore".
  void build(const std::vector<ThemeDoc>& docs);

  Rgba color(Role r) const { return colors_[static_cast<int>(r)]; }
  float scalar(Scalar s) const { return scalars_[static_cast<int>(s)]; }

  // Bumped by every build(). Anything caching a resolved style — the skin, a
  // pre-rendered title — compares against it rather than being told, for the same
  // reason SettingsStore::revision exists.
  std::uint32_t revision() const { return revision_; }

  // Every name and its resolved value, in table order. Backs --dump-theme, which
  // is how a pack author finds out what there is to override.
  std::string dump() const;

 private:
  void resetPalette();
  void deriveRoles();

  Rgba colors_[kRoleCount] {};
  float scalars_[kScalarCount] {};
  std::uint32_t revision_ = 0;
};

// The process-wide theme, matching how settings() and blocks() are reached.
Theme& theme();

// The two accessors the drawing code actually uses. Short on purpose: they appear
// several hundred times, and `col(Role::ButtonFill)` stays readable at that
// density in a way `theme().color(Role::ButtonFill)` does not.
inline Rgba col(Role r) { return theme().color(r); }
// The same, with the alpha scaled — `col(Role::Scrim, 0.55f)`. This is what
// replaced three dozen hand-written rgba(0, 0, 0, 0.55) literals, none of which a
// pack could reach.
inline Rgba col(Role r, float alpha) { return fade(theme().color(r), alpha); }
inline float px(Scalar s) { return theme().scalar(s); }

// The waypoint swatch cycle, which the Atlas walks by index.
inline constexpr int kWaypointColorCount = 7;
Rgba waypointColor(int index);

// The map's hand-picked deep blue for water, whose tile is translucent. Not a
// token: it is composited into map tile pixels rather than drawn, so it has to be
// stable for as long as a cached tile lives, and a theme change mid-session would
// leave half the Atlas one blue and half another.
inline constexpr int kWaterMapRgb[3] = {43, 93, 165};

// ---------------------------------------------------------------------------
// Timings
//
// Not themed. A pack deciding how long a toast lingers is a pack deciding how
// long you have to read something, which is an accessibility question rather than
// a decorative one.
// ---------------------------------------------------------------------------

inline constexpr double kToastIn = 0.2;
inline constexpr double kToastHold = 2.2;
inline constexpr double kToastOut = 0.3;
inline constexpr double kToastLife = 2.6;

inline constexpr double kHeldLabelHold = 1.2;
inline constexpr double kHeldLabelFade = 0.3;

}  // namespace hr::ui
