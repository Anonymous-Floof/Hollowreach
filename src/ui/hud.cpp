#include "ui/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/jsmath.h"
#include "game/items.h"
#include "game/player.h"
#include "render/sky.h"
#include "ui/widgets.h"
#include "world/blocks.h"
#include "world/chunk.h"
#include "world/world.h"

namespace hr::ui {
namespace {

// The pip glyphs, as UTF-8. The stylesheet drew these as text and so does this; they
// come from Segoe UI Symbol via the font fallback chain.
constexpr const char* kHeartFull = "\xE2\x99\xA5";   // ♥
constexpr const char* kHeartEmpty = "\xE2\x99\xA1";  // ♡
constexpr const char* kFoodFull = "\xE2\x97\x86";    // ◆
constexpr const char* kFoodEmpty = "\xE2\x97\x87";   // ◇
constexpr const char* kBubble = "\xE2\x97\x8F";      // ●

// A row of pips: `.heart` is a 16x16 flex item, `.pip` is 15px text on a 16px line
// box. Both draw their glyph at the left of the box with the first line box of a
// block, which is what puts the taller glyphs slightly outside the 16px height —
// visible in the browser too.
struct PipRow {
  float pipWidth;
  float pipHeight;
  float gap;
};

float drawPips(Ui2D& ui, Text& text, float centerX, float top, const PipRow& row, int count,
               const std::function<std::pair<const char*, Rgba>(int)>& pip,
               const TextStyle& baseStyle, float lineHeight) {
  if (count <= 0) return 0.0f;
  const float totalW = row.pipWidth * static_cast<float>(count) +
                       row.gap * static_cast<float>(count - 1);
  float x = centerX - totalW * 0.5f;
  const TextMetrics m = text.metrics(baseStyle);
  // A block container's first line box starts at the top of the content box; the
  // baseline sits half a leading below it.
  const float baseline = top + (lineHeight - (m.ascent + m.descent)) * 0.5f + m.ascent;
  for (int i = 0; i < count; ++i) {
    const auto [glyph, color] = pip(i);
    TextStyle ts = baseStyle;
    ts.color = color;
    text.draw(ui, x, baseline, glyph, ts);
    x += row.pipWidth + row.gap;
  }
  return totalW;
}

}  // namespace

void Hud::flashHeld(const std::string& name) {
  heldLabel_ = name;
  heldLabelAge_ = 0.0;
}

void Hud::update(double dt, const game::Inventory& inventory) {
  heldLabelAge_ += dt;
  // js/ui/hud.js:88-92 — the label flashes when the selection changes, not when the
  // stack changes, so swapping a pick for a better pick in the same slot stays quiet.
  if (inventory.selected() != lastSelected_) {
    lastSelected_ = inventory.selected();
    const game::ItemStack& s = inventory.selectedSlot();
    if (!s.empty()) {
      if (const game::ItemDef* item = game::getItem(s.key)) flashHeld(item->name);
    }
  }
}

Rect Hud::hotbarSlotRect(Ui2D& ui, int slot) const {
  const float slotSize = metric::hotbarSlot;
  const float gap = metric::hotbarGap;
  const float totalW = slotSize * 9 + gap * 8;
  const float x = (ui.width() - totalW) * 0.5f;
  const float y = ui.height() - metric::hotbarBottom - slotSize;
  return {x + static_cast<float>(slot) * (slotSize + gap), y, slotSize, slotSize};
}

// A red wash that is transparent in the middle and strongest at the corners, so it
// reads in peripheral vision without obscuring what you are aiming at. Drawn before
// the crosshair, and deliberately not while the pause or inventory screens are up:
// those paint their own scrim and a second red one over it just muddies them.
void Hud::drawDamageVignette(Ui2D& ui, float strength) const {
  if (strength <= 0.0f) return;
  const float s = std::min(1.0f, strength);
  const float cx = ui.width() * 0.5f, cy = ui.height() * 0.5f;
  // Inner stop at 0.35 leaves the middle third of the screen essentially clear;
  // the alpha ramps in from there to the edge.
  ui.radialGradient({0, 0, ui.width(), ui.height()}, cx, cy, ui.width() * 0.72f,
                    ui.height() * 0.78f, 0.35f, 1.0f, rgba(150, 10, 12, 0.0),
                    rgba(150, 10, 12, 0.62f * s));
}

void Hud::drawCrosshair(Ui2D& ui, Text& text) const {
  // #crosshair { color: #fff; font-size: 22px; opacity: .8; mix-blend-mode: difference }
  //
  // Difference blending is not a GL blend equation, but for a white source it reduces to
  //   out = a + (1 - 2a) * dst
  // which at a = 0.8 is 0.8 - 0.6 * dst — expressible as
  // (ONE_MINUS_DST_COLOR, ONE_MINUS_SRC_ALPHA) over a source premultiplied by its own
  // alpha. Ui2D switches ui.frag to premultiplied output for exactly this reason: without
  // it the zero-coverage pixels of the glyph quad still contributed the full source
  // colour, and the crosshair came out as a solid box.
  TextStyle ts;
  ts.size = 22;
  ts.color = {255, 255, 255, 204};  // #fff at opacity 0.8
  const TextMetrics m = text.metrics(ts);
  const float w = text.measure("+", ts);
  ui.setBlendMode(BlendMode::Difference);
  text.draw(ui, ui.width() * 0.5f - w * 0.5f,
            ui.height() * 0.5f - m.lineHeight * 0.5f +
                (m.lineHeight - (m.ascent + m.descent)) * 0.5f + m.ascent,
            "+", ts);
  ui.setBlendMode(BlendMode::Normal);
}

void Hud::drawStats(Ui2D& ui, Text& text, const game::Player& player, float bottom) const {
  // #stats { flex-direction: column; align-items: center; gap: 3px }
  // #bars  { display: flex; gap: 14px }
  const float cx = ui.width() * 0.5f;

  TextStyle heartStyle;
  heartStyle.size = 16;
  TextStyle pipStyle;
  pipStyle.size = 15;
  pipStyle.withShadow(1, 1, 1, color::black);

  const float heartLine = 16.0f;  // .heart is a 16px box
  const float pipLine = 16.0f;    // .pip { line-height: 16px }

  // Row heights, bottom-up: the hotbar sits at `bottom`, the stats column above it.
  const float heartsW =
      metric::heartSize * 10 + metric::pipGap * 9;
  const bool showHunger = player.hungerOn();
  const float hungerW = showHunger ? metric::heartSize * 10 + metric::pipGap * 9 : 0.0f;
  const float barsW = heartsW + (showHunger ? metric::barsGap + hungerW : 0.0f);

  const int breathPips =
      player.breath() < player.maxBreath() - 0.01f
          ? static_cast<int>(std::ceil(player.breath() / player.maxBreath() * 10.0f))
          : -1;

  float y = bottom - heartLine;
  if (breathPips >= 0) y -= pipLine + 3.0f;

  if (breathPips > 0) {
    drawPips(ui, text, cx, y - 0.0f, {metric::heartSize, pipLine, metric::pipGap}, breathPips,
             [](int) { return std::pair<const char*, Rgba> {kBubble, color::breath}; }, pipStyle,
             pipLine);
    y += pipLine + 3.0f;
  } else if (breathPips == 0) {
    y += pipLine + 3.0f;
  }

  // Hearts and hunger share one row, centred as a pair.
  const float rowLeft = cx - barsW * 0.5f;
  const float health = player.health();
  drawPips(ui, text, rowLeft + heartsW * 0.5f, y, {metric::heartSize, heartLine, metric::pipGap},
           10,
           [health](int i) {
             const bool full = health >= static_cast<float>((i + 1) * 2) - 0.01f;
             const bool half = !full && health >= static_cast<float>(i * 2 + 1);
             const Rgba c = full ? color::heartFull : half ? color::heartHalf : color::heartEmpty;
             return std::pair<const char*, Rgba> {full || half ? kHeartFull : kHeartEmpty, c};
           },
           heartStyle, heartLine);

  if (showHunger) {
    const float hunger = player.hunger();
    drawPips(ui, text, rowLeft + heartsW + metric::barsGap + hungerW * 0.5f, y,
             {metric::heartSize, pipLine, metric::pipGap}, 10,
             [hunger](int i) {
               const bool full = hunger >= static_cast<float>((i + 1) * 2) - 0.01f;
               const bool half = !full && hunger >= static_cast<float>(i * 2 + 1);
               const Rgba c =
                   full ? color::hungerFull : half ? color::hungerHalf : color::hungerEmpty;
               return std::pair<const char*, Rgba> {full || half ? kFoodFull : kFoodEmpty, c};
             },
             pipStyle, pipLine);
  }
}

void Hud::drawHotbar(Ui2D& ui, Text& text, const HudFrame& frame, float bottom) const {
  const game::Inventory& inv = *frame.inventory;
  for (int i = 0; i < game::kHotbarSlots; ++i) {
    const Rect slot = hotbarSlotRect(ui, i);
    const bool selected = i == inv.selected();
    const Style s = widget::hotbarSlot(selected);
    ui.fillRect(slot, s.bg, s.radius);
    ui.strokeRect(slot, s.border, s.borderWidth, s.radius);
    // .hslot.sel { box-shadow: 0 0 0 2px rgba(255,255,255,0.25) } — a 2px outer ring.
    if (selected) {
      ui.strokeRect(slot.inset(-2.0f), rgba(255, 255, 255, 0.25), 2, s.radius + 2);
    }

    const game::ItemStack& stack = inv.slots()[i];
    if (stack.empty()) continue;
    widget::StackVisual v;
    if (frame.icons && frame.icons->uvFor(stack.key, v.icon.u0, v.icon.v0, v.icon.u1, v.icon.v1)) {
      v.icon.texture = frame.icons->texture();
    }
    v.count = stack.count;
    const int maxDura = game::maxDurability(stack.key);
    if (stack.wears() && maxDura > 0) {
      v.duraFraction = static_cast<float>(stack.dura) / static_cast<float>(maxDura);
    }
    widget::drawStack(ui, text, slot, v);
  }
  (void)bottom;
}

void Hud::drawDebug(Ui2D& ui, Text& text, const HudFrame& frame) const {
  const game::Player& p = *frame.player;
  const Vec3 pos = p.pos();
  const int cx = static_cast<int>(std::floor(pos.x / static_cast<float>(world::CX)));
  const int cz = static_cast<int>(std::floor(pos.z / static_cast<float>(world::CZ)));

  std::string targetName = "\xE2\x80\x94";  // em dash
  if (frame.hasTarget && frame.world) {
    targetName = world::blocks()
                     .def(frame.world->getBlock(frame.targetX, frame.targetY, frame.targetZ))
                     .name;
  }

  // js/ui/hud.js:158-159 — eight compass points, indexed off yaw in 45 degree steps.
  static const char* kDirs[8] = {"S", "SW", "W", "NW", "N", "NE", "E", "SE"};
  constexpr float kPi = 3.14159265358979323846f;
  // JS % keeps the sign of the dividend, so `wrapped` is negative for a negative yaw and
  // the rounding direction of a half matters: Math.round(-0.5) is -0 and indexes "S",
  // while std::lround(-0.5) is -1 and would index "SE".
  const float wrapped = std::fmod(p.yaw(), kPi * 2.0f);
  const int facing = static_cast<int>(jsmath::jsRoundF(wrapped / (kPi / 4.0f))) & 7;

  char buffer[512];
  std::snprintf(buffer, sizeof(buffer),
                "Hollowreach  %.0f fps\n"
                "xyz %.1f %.1f %.1f\n"
                "chunk %d, %d   facing %s\n"
                "time %s   chunks %zu\n"
                "looking at %s\n"
                "health %.1f   %s",
                frame.fps, pos.x, pos.y, pos.z, cx, cz, kDirs[facing],
                frame.sky ? frame.sky->clockString().c_str() : "--:--",
                frame.world ? frame.world->loadedChunkCount() : 0u, targetName.c_str(),
                p.health(), p.flying() ? "flying" : "walking");

  std::string body = buffer;
  if (!frame.netLine.empty()) body += "\n" + frame.netLine;

  // #debug { left: 8px; top: 8px; padding: 8px 10px; line-height: 1.5; white-space: pre }
  const TextStyle ts = widget::debug();
  const float lineHeight = ts.size * 1.5f;
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= body.size()) {
    const std::size_t nl = body.find('\n', start);
    lines.push_back(body.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    if (nl == std::string::npos) break;
    start = nl + 1;
  }

  float width = 0;
  for (const std::string& line : lines) width = std::max(width, text.measure(line, ts));
  const Rect box {8, 8, width + 20, lineHeight * static_cast<float>(lines.size()) + 16};
  ui.fillRect(box, rgba(8, 11, 15, 0.6), 6);

  const TextMetrics m = text.metrics(ts);
  float y = box.y + 8;
  for (const std::string& line : lines) {
    text.drawInBox(ui, {box.x + 10, y, width, lineHeight}, line, ts);
    y += lineHeight;
  }
  (void)m;
}

void Hud::drawNameplates(Ui2D& ui, Text& text, const HudFrame& frame) const {
  if (frame.nameplates.empty() || !frame.camera || !frame.player) return;

  const float* vp = frame.camera->viewProj().data();
  TextStyle style;
  style.size = 13;
  style.withShadow(0, 1, 2, rgba(0, 0, 0, 0.85));
  const TextMetrics tm = text.metrics(style);

  for (const HudFrame::Nameplate& plate : frame.nameplates) {
    // Above the head: the body is 1.8 blocks and the plate floats a little clear
    // of it, the same offset the waypoint tags use.
    const float x = plate.pos.x;
    const float y = plate.pos.y + 2.1f;
    const float z = plate.pos.z;
    const float cw = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
    if (cw <= 0.1f) continue;  // behind the eye
    const float ndcX = (vp[0] * x + vp[4] * y + vp[8] * z + vp[12]) / cw;
    const float ndcY = (vp[1] * x + vp[5] * y + vp[9] * z + vp[13]) / cw;
    if (ndcX < -1.05f || ndcX > 1.05f || ndcY < -1.05f || ndcY > 1.05f) continue;

    const float dx = x - frame.player->pos().x;
    const float dz = z - frame.player->pos().z;
    const float distance = std::sqrt(dx * dx + dz * dz);
    // Faded with distance rather than hidden: a name you can only just read is
    // still the difference between a stranger and a zombie at forty blocks.
    const float alpha = distance > 64.0f ? 0.0f : (distance > 32.0f ? 0.45f : 0.92f);
    if (alpha <= 0.0f) continue;

    const float sx = (ndcX + 1.0f) * 0.5f * ui.width();
    const float sy = (1.0f - ndcY) * 0.5f * ui.height();
    const float tw = text.measure(plate.name, style);
    const Rect box {sx - (tw + 14) * 0.5f, sy - (tm.lineHeight + 4), tw + 14, tm.lineHeight + 4};
    ui.fillRect(box, fade(rgba(10, 14, 18, 0.55), alpha), 4);

    // A hurt player's plate carries the news: a thin bar under the name, which is
    // the only health readout anybody has for somebody else.
    if (plate.health < 19.5f) {
      const float fraction = std::clamp(plate.health / 20.0f, 0.0f, 1.0f);
      const Rect bar {box.x + 3, box.y + box.h - 1, (box.w - 6) * fraction, 2};
      ui.fillRect(bar, fade(rgba(214, 74, 74, 0.95), alpha), 1);
    }

    TextStyle ts = style;
    ts.color = fade(ts.color, alpha);
    for (int i = 0; i < ts.shadowCount; ++i) {
      ts.shadows[i].color = fade(ts.shadows[i].color, alpha);
    }
    text.drawInBox(ui, {box.x + 7, box.y + 2, tw, tm.lineHeight}, plate.name, ts);
  }
}

void Hud::draw(Ui2D& ui, Text& text, const HudFrame& frame) {
  if (!frame.player || !frame.inventory) return;

  drawDamageVignette(ui, frame.hurtFlash);
  drawCrosshair(ui, text);

  // #break-overlay: a 40x5 bar at calc(50% + 16px), only while mining.
  if (frame.breakFraction > 0.0f) {
    const Rect bar {ui.width() * 0.5f - 20.0f, ui.height() * 0.5f + 16.0f, 40, 5};
    widget::drawBar(ui, bar, frame.breakFraction, color::progressFill, color::black, 3);
  }

  // #hud-bottom { bottom: 16px; column; gap: 8px } — hotbar last, stats above it.
  const float hotbarTop = ui.height() - metric::hotbarBottom - metric::hotbarSlot;
  drawHotbar(ui, text, frame, hotbarTop);
  drawStats(ui, text, *frame.player, hotbarTop - metric::hudColumnGap);

  // #held-item-label { bottom: 80px; opacity 0 -> 1, transition .3s }
  if (!heldLabel_.empty() && heldLabelAge_ < kHeldLabelHold + kHeldLabelFade) {
    float alpha = 1.0f;
    if (heldLabelAge_ > kHeldLabelHold) {
      alpha = 1.0f - easeDefault(static_cast<float>((heldLabelAge_ - kHeldLabelHold) /
                                                    kHeldLabelFade));
    }
    TextStyle ts = widget::heldItemLabel();
    ts.color = fade(ts.color, alpha);
    for (int i = 0; i < ts.shadowCount; ++i) ts.shadows[i].color = fade(ts.shadows[i].color, alpha);
    const TextMetrics m = text.metrics(ts);
    const float w = text.measure(heldLabel_, ts);
    text.draw(ui, ui.width() * 0.5f - w * 0.5f, ui.height() - 80.0f - m.descent, heldLabel_, ts);
  }

  drawNameplates(ui, text, frame);

  if (showDebug_) drawDebug(ui, text, frame);
}

}  // namespace hr::ui
