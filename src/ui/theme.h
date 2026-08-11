// Every colour and metric the interface draws with, in one table.
//
// css/style.css is the specification for this whole milestone, and its `:root`
// custom properties are transcribed here verbatim. The literals that appear
// inline in the stylesheet get names too, because a hex value repeated across
// four screens in the CSS should be one constant here — and because a resource
// pack or a theme file is the obvious later extension, and it wants a table to
// override rather than 200 scattered literals.
//
// Alpha follows the CSS convention: rgba() takes 0..1 and rounds to 8 bits, which
// is what the browser composites with.

#pragma once

#include <cmath>
#include <cstdint>

#include "resource/image.h"

namespace hr::ui {

// ---------------------------------------------------------------------------
// Colour construction
// ---------------------------------------------------------------------------

// #rrggbb, fully opaque.
constexpr Rgba rgb(std::uint32_t hex) {
  return {static_cast<std::uint8_t>((hex >> 16) & 0xFFu),
          static_cast<std::uint8_t>((hex >> 8) & 0xFFu),
          static_cast<std::uint8_t>(hex & 0xFFu), 255};
}

// CSS rgba(r, g, b, a) with a in 0..1.
inline Rgba rgba(int r, int g, int b, double a) {
  const long v = std::lround(a * 255.0);
  const std::uint8_t alpha = static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
  return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
          static_cast<std::uint8_t>(b), alpha};
}

// #rrggbb with an explicit 0..1 alpha, for `color-mix`-style tinting.
inline Rgba fade(Rgba c, double a) {
  const long v = std::lround(static_cast<double>(c.a) * a);
  c.a = static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
  return c;
}

inline constexpr Rgba kTransparent {0, 0, 0, 0};

// ---------------------------------------------------------------------------
// :root custom properties (css/style.css:1-13)
// ---------------------------------------------------------------------------

namespace color {

inline constexpr Rgba bg = rgb(0x11151c);
inline constexpr Rgba panel = rgb(0x1c2530);
inline constexpr Rgba panel2 = rgb(0x263240);
inline constexpr Rgba edge = rgb(0x0c0f14);
inline constexpr Rgba accent = rgb(0x7fd17f);
inline constexpr Rgba accentDark = rgb(0x4f8f57);
inline constexpr Rgba text = rgb(0xe7ecf2);
inline constexpr Rgba muted = rgb(0x9aa7b4);
inline constexpr Rgba slot = rgb(0x2b3744);
inline constexpr Rgba slotEdge = rgb(0x11161d);
inline constexpr Rgba danger = rgb(0xd96a5a);

// Repeated literals, named where the stylesheet only had the value.
inline constexpr Rgba hover = rgb(0x32445a);            // .btn:hover background
inline constexpr Rgba primaryEdge = rgb(0x2f6238);      // .btn.primary border
inline constexpr Rgba primaryHi = rgb(0x5fae68);        // menu primary hover gradient end
inline constexpr Rgba primaryLo = rgb(0x3c7444);        // menu primary gradient end
inline constexpr Rgba dangerEdge = rgb(0x92382c);
inline constexpr Rgba onPrimary = rgb(0x11251a);        // .btn.primary:hover text
inline constexpr Rgba onPrimaryMenu = rgb(0x0d2413);    // #menu-root .btn.primary:hover text
inline constexpr Rgba onAccent = rgb(0x10240f);         // .rb-tab.active text
inline constexpr Rgba inputBg = rgb(0x0c0f14);
inline constexpr Rgba inputEdge = rgb(0x000000);
inline constexpr Rgba fieldBg = rgb(0x10161e);          // .wp-row input, .mp-code
inline constexpr Rgba settingRule = rgb(0x141a22);      // .setting bottom rule
inline constexpr Rgba pending = rgb(0x141a22);          // map tile not yet drawn
inline constexpr Rgba mapBg = rgb(0x0b0e13);
inline constexpr Rgba mapFog = rgb(0x10141b);
inline constexpr Rgba resultSlot = rgb(0x34291f);       // .islot.result
inline constexpr Rgba armorSlot = rgb(0x20303a);        // .islot.armor
inline constexpr Rgba fuelFill = rgb(0xe0883a);
inline constexpr Rgba progressFill = rgb(0xcfe8cf);     // smelt + break bars, #debug text
inline constexpr Rgba kbdText = rgb(0xd6e6d6);
inline constexpr Rgba aboutText = rgb(0xcdd7e1);
inline constexpr Rgba aboutLead = rgb(0xe7eef4);
inline constexpr Rgba aboutBold = rgb(0xeafff0);
inline constexpr Rgba featText = rgb(0xa9b5c1);
inline constexpr Rgba controlValue = rgb(0xaab6c2);
inline constexpr Rgba nameplateText = rgb(0xeef4f8);
// Chat. Three tints and no more: a line's meaning belongs to whoever wrote it, and
// a box that colours text nine ways is one nobody can read at a glance. Plain
// speech uses `text` and a refusal uses `danger`, so these are the three cases that
// are neither.
inline constexpr Rgba chatSystem = rgb(0x8fc6e8);   // the world talking
inline constexpr Rgba chatWhisper = rgb(0xc2a3e0);  // to or from one person
inline constexpr Rgba chatReply = rgb(0x9de0b4);    // the answer to a command
inline constexpr Rgba galBadge = rgb(0xcfe8cf);
inline constexpr Rgba galBadgePano = rgb(0xcfe0ff);
inline constexpr Rgba subtitleGlass = rgb(0xb9c7d3);
inline constexpr Rgba settingsTabActive = rgb(0xeaffea);
inline constexpr Rgba white = rgb(0xffffff);
inline constexpr Rgba black = rgb(0x000000);

// Title gradient (.menu-glass .game-title, three stops).
inline constexpr Rgba titleTop = rgb(0xeafff0);
inline constexpr Rgba titleMid = rgb(0x9fe3a6);
inline constexpr Rgba titleBottom = rgb(0x5aa564);

// HUD pip colours (js/ui/hud.js:101,116,130).
inline constexpr Rgba heartFull = rgb(0xe0463a);
inline constexpr Rgba heartHalf = rgb(0xb8463a);
inline constexpr Rgba heartEmpty = rgb(0x3a2526);
inline constexpr Rgba hungerFull = rgb(0xd2901f);
inline constexpr Rgba hungerHalf = rgb(0x9c6a22);
inline constexpr Rgba hungerEmpty = rgb(0x2e241a);
inline constexpr Rgba breath = rgb(0x6ec8ff);

}  // namespace color

// Waypoint swatches, in cycle order (js/ui/map.js:18).
inline constexpr Rgba kWaypointColors[7] = {
    rgb(0xe8c84a), rgb(0x52b6e8), rgb(0x7ade6a), rgb(0xe07ad0),
    rgb(0xf08748), rgb(0xa48aff), rgb(0xf0f0f0),
};
inline constexpr int kWaypointColorCount = 7;

// The map's hand-picked deep blue for water, whose tile is translucent
// (js/ui/map.js:19).
inline constexpr int kWaterMapRgb[3] = {43, 93, 165};

// ---------------------------------------------------------------------------
// Metrics
//
// The stylesheet is entirely in fixed px, which makes the browser's own zoom its
// de facto scaler. `uiScale` is the one place this port improves on the original:
// every value below is multiplied at layout time, so the whole interface scales
// as a unit on a high-DPI display.
// ---------------------------------------------------------------------------

namespace metric {

inline constexpr float hotbarSlot = 52.0f;       // .hslot
inline constexpr float hotbarGap = 4.0f;         // #hotbar gap
inline constexpr float hotbarBottom = 16.0f;     // #hud-bottom bottom
inline constexpr float hudColumnGap = 8.0f;      // #hud-bottom gap
inline constexpr float invSlot = 46.0f;          // .islot
inline constexpr float invSlotGap = 4.0f;        // .slot-grid gap
inline constexpr float invPanelGap = 18.0f;      // #inventory-root gap
inline constexpr float heartSize = 16.0f;        // .heart
inline constexpr float barsGap = 14.0f;          // #bars gap
inline constexpr float pipGap = 2.0f;            // #hearts/#hunger/#breath gap
inline constexpr float minimapSize = 168.0f;     // #minimap
inline constexpr float minimapInset = 14.0f;
inline constexpr float toastTop = 196.0f;        // #notify top
inline constexpr float toastRight = 14.0f;
inline constexpr float rbCell = 24.0f;           // .rb-cell
inline constexpr float rbIcon = 22.0f;           // .rb-icon
inline constexpr float rbIconOut = 30.0f;        // .rb-icon-out

}  // namespace metric

// Toast lifetime: 0.2s in, 2.2s hold before the 0.3s out (css toastin/toastout),
// and js/ui/notify.js removes the element at 2.6s.
inline constexpr double kToastIn = 0.2;
inline constexpr double kToastHold = 2.2;
inline constexpr double kToastOut = 0.3;
inline constexpr double kToastLife = 2.6;

// #held-item-label: 1.2s visible, then a .3s opacity transition to 0.
inline constexpr double kHeldLabelHold = 1.2;
inline constexpr double kHeldLabelFade = 0.3;

}  // namespace hr::ui
