#include "ui/timewheel.h"

#include <algorithm>
#include <cmath>

#include "core/input.h"

namespace hr::ui {
namespace {

constexpr float kPi = 3.14159265358979f;

// The dial is drawn as this many trapezoids around the annulus. 144 is six per
// hour: fine enough that the rim reads as a smooth sweep from night to day and
// coarse enough to stay one flush of a few hundred vertices.
constexpr int kRimSegments = 144;

// Snap: a quarter of an hour. Dragging to the exact minute is neither achievable
// with a mouse nor interesting, and a wheel that lands on 07:00 rather than 06:58
// is the difference between the readout looking chosen and looking approximate.
constexpr float kSnapMinutes = 15.0f;

float snapTime(float t) {
  const float perDay = 24.0f * 60.0f / kSnapMinutes;
  float s = std::round(t * perDay) / perDay;
  s = s - std::floor(s);
  return s;
}

// What the rim looks like at a given hour: deep blue through the night, pale gold
// at midday, and a warm band across each of the two crossings. duskFactor is
// rebuilt here rather than borrowed from Sky because Sky's is about the current
// moment and this wants it for every hour at once.
Rgba rimColor(float t) {
  const float day = render::Sky::dayFactorAt(t);
  const float h = render::Sky::sunHeightAt(t);
  float dusk = 1.0f - std::fabs(h) * 4.0f;
  dusk = std::clamp(dusk, 0.0f, 1.0f) * std::clamp(day * 3.0f, 0.0f, 1.0f);

  const float nr = 24, ng = 32, nb = 56;      // night
  const float dr = 226, dg = 220, db = 188;   // midday
  float r = nr + (dr - nr) * day;
  float g = ng + (dg - ng) * day;
  float b = nb + (db - nb) * day;
  const float kr = 224, kg = 132, kb = 74;    // the warm crossing
  r += (kr - r) * dusk * 0.75f;
  g += (kg - g) * dusk * 0.75f;
  b += (kb - b) * dusk * 0.75f;
  return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
          static_cast<std::uint8_t>(b), 255};
}

// A flat-shaded ring segment between two angles, as one quad.
void rimQuad(Ui2D& ui, float cx, float cy, float inner, float outer, float a0, float a1,
             Rgba color) {
  const Vec2 pts[4] = {
      {cx + std::sin(a0) * outer, cy - std::cos(a0) * outer},
      {cx + std::sin(a1) * outer, cy - std::cos(a1) * outer},
      {cx + std::sin(a1) * inner, cy - std::cos(a1) * inner},
      {cx + std::sin(a0) * inner, cy - std::cos(a0) * inner},
  };
  ui.fillPoly(pts, 4, color);
}

void tick(Ui2D& ui, float cx, float cy, float t, float from, float to, Rgba color,
          float width) {
  const float a = t * kPi * 2.0f;
  const float sx = std::sin(a), sy = -std::cos(a);
  ui.line({cx + sx * from, cy + sy * from}, {cx + sx * to, cy + sy * to}, color, width);
}

void disc(Ui2D& ui, float cx, float cy, float r, Rgba color) {
  ui.fillRect({cx - r, cy - r, r * 2, r * 2}, color, r);
}

bool button(Ui2D& ui, Text& text, const Rect& box, const std::string& label, bool hovered,
            bool enabled, bool primary) {
  Rgba bg = color::panel2;
  Rgba edge = color::edge;
  Rgba fg = color::text;
  if (primary) {
    bg = hovered ? color::primaryHi : color::primaryLo;
    edge = color::primaryEdge;
    fg = hovered ? color::onPrimary : color::text;
  } else if (hovered) {
    bg = color::hover;
  }
  if (!enabled) {
    bg = color::slot;
    edge = color::slotEdge;
    fg = color::muted;
  }
  ui.fillRect(box, bg, 8.0f);
  ui.strokeRect(box, edge, 1.0f, 8.0f);
  TextStyle ts;
  ts.font = FontId::SansSemibold;
  ts.size = 14.0f;
  ts.color = fg;
  text.drawInBox(ui, box, label, ts, TextAlign::Center);
  return enabled && hovered;
}

}  // namespace

void TimeWheel::open(float now) {
  voter_.clear();
  dragging_ = false;
  // Dawn unless the sun is already up, in which case the next dawn is a whole day
  // away and offering it as the default would be a strange first suggestion. Half
  // a day on is the honest "you probably want the other end of this".
  // Snapped like any other reading, so the default shows a round 06:30 rather than
  // the 06:28 the raw dawn constant works out to.
  const float dawn = snapTime(render::Sky::kDawn);
  const float ahead = std::fmod(dawn - now + 1.0f, 1.0f);
  target_ = ahead > 0.02f && ahead < 0.75f ? dawn : snapTime(now + 0.5f);
}

void TimeWheel::openVote(float now, float proposed, const std::string& who) {
  (void)now;
  dragging_ = false;
  target_ = proposed;
  voter_ = who.empty() ? std::string("Someone") : who;
}

float TimeWheel::angleToTime(float dx, float dy) {
  // atan2 measured from straight up, running clockwise, mapped onto 0..1.
  float a = std::atan2(dx, -dy) / (kPi * 2.0f);
  a = a - std::floor(a);
  return a;
}

void TimeWheel::timeToUnit(float t, float& ux, float& uy) {
  const float a = t * kPi * 2.0f;
  ux = std::sin(a);
  uy = -std::cos(a);
}

bool TimeWheel::canSleep(const render::Sky& sky) const {
  // A vote is on somebody else's tiredness, not yours: the whole point of the
  // rework is that one person being ready to sleep is enough to ask the rest.
  return isVote() || sky.tired();
}

TimeWheel::Layout TimeWheel::layout(Ui2D& ui) const {
  Layout l;
  const float w = std::min(420.0f, ui.width() - 40.0f);
  const float h = std::min(520.0f, ui.height() - 40.0f);
  l.card = {(ui.width() - w) * 0.5f, (ui.height() - h) * 0.5f, w, h};
  // The 60px above and 200px below are not padding for its own sake: the hour
  // labels sit 15px outside the rim and are 16 tall, so the dial has to hold that
  // clear of the title at the top and of the note and buttons at the bottom.
  l.radius = std::min(w - 60.0f, h - 200.0f) * 0.5f - 8.0f;
  if (l.radius < 40.0f) l.radius = 40.0f;
  l.thickness = std::max(16.0f, l.radius * 0.20f);
  l.cx = l.card.centerX();
  l.cy = l.card.y + 70.0f + l.radius;

  const float bw = (w - 3 * 20.0f) * 0.5f;
  const float by = l.card.bottom() - 56.0f;
  l.cancel = {l.card.x + 20.0f, by, bw, 38.0f};
  l.confirm = {l.card.x + 20.0f + bw + 20.0f, by, bw, 38.0f};
  return l;
}

void TimeWheel::update(Ui2D& ui, Text& text, const UiEvent& event, const render::Sky& sky) {
  (void)text;
  const Layout l = layout(ui);
  const float dx = event.mouseX - l.cx;
  const float dy = event.mouseY - l.cy;
  const float dist = std::sqrt(dx * dx + dy * dy);

  hoverCancel_ = l.cancel.contains(event.mouseX, event.mouseY);
  hoverConfirm_ = l.confirm.contains(event.mouseX, event.mouseY);

  // The rim, generously: the band plus a grab margin either side, and the whole
  // disc once a drag has started so the handle does not fall off the pointer when
  // it strays toward the middle.
  const bool onRim = dist > l.radius - l.thickness - 14.0f && dist < l.radius + 14.0f;
  const bool editable = !isVote();

  if (editable) {
    if (event.leftClick && onRim) dragging_ = true;
    if (!event.leftDown) dragging_ = false;
    if (dragging_ && dist > 8.0f) target_ = snapTime(angleToTime(dx, dy));
    // Scroll nudges by the snap step, which is what makes an exact hour reachable
    // without fighting the pointer.
    if (event.wheel != 0.0f && !hoverCancel_ && !hoverConfirm_) {
      const float step = kSnapMinutes / (24.0f * 60.0f);
      target_ = snapTime(target_ + (event.wheel > 0 ? -step : step));
    }
  }

  const bool ready = canSleep(sky);
  bool confirm = event.leftClick && hoverConfirm_ && ready;
  bool cancel = event.leftClick && hoverCancel_;
  if (event.input) {
    if (event.input->pressed(Key::Enter) && ready) confirm = true;
  }
  if (confirm) {
    if (onConfirm) onConfirm(target_);
    return;
  }
  if (cancel && onCancel) onCancel();
}

void TimeWheel::draw(Ui2D& ui, Text& text, const render::Sky& sky) {
  const Layout l = layout(ui);
  const float now = sky.time;
  const bool ready = canSleep(sky);

  ui.fillRect({0, 0, ui.width(), ui.height()}, rgba(0, 0, 0, 0.55));
  ui.shadow(l.card, BoxShadow{0, 18, 40, 0, rgba(0, 0, 0, 0.55)}, 14.0f);
  ui.fillRect(l.card, color::panel, 14.0f);
  ui.strokeRect(l.card, color::edge, 1.0f, 14.0f);

  TextStyle title;
  title.font = FontId::SansBold;
  title.size = 17.0f;
  title.color = color::text;
  text.drawInBox(ui, {l.card.x, l.card.y + 14.0f, l.card.w, 24.0f},
                 isVote() ? voter_ + " wants to sleep" : "Time", title, TextAlign::Center);

  // ---- the rim: one day, painted ----
  const float inner = l.radius - l.thickness;
  for (int i = 0; i < kRimSegments; ++i) {
    const float t0 = static_cast<float>(i) / kRimSegments;
    const float t1 = static_cast<float>(i + 1) / kRimSegments;
    rimQuad(ui, l.cx, l.cy, inner, l.radius, t0 * kPi * 2.0f, t1 * kPi * 2.0f,
            rimColor((t0 + t1) * 0.5f));
  }

  // Hour ticks, longer every six hours, with the quarter-day labels outside.
  for (int hour = 0; hour < 24; ++hour) {
    const float t = static_cast<float>(hour) / 24.0f;
    const bool major = hour % 6 == 0;
    tick(ui, l.cx, l.cy, t, inner, inner + (major ? l.thickness : l.thickness * 0.45f),
         rgba(0, 0, 0, major ? 0.55 : 0.3), major ? 2.0f : 1.0f);
  }
  TextStyle hourLabel;
  hourLabel.font = FontId::SansSemibold;
  hourLabel.size = 11.0f;
  hourLabel.color = color::muted;
  for (int hour = 0; hour < 24; hour += 6) {
    float ux = 0, uy = 0;
    timeToUnit(static_cast<float>(hour) / 24.0f, ux, uy);
    const float lx = l.cx + ux * (l.radius + 15.0f);
    const float ly = l.cy + uy * (l.radius + 15.0f);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d", hour);
    text.drawInBox(ui, {lx - 14.0f, ly - 8.0f, 28.0f, 16.0f}, buf, hourLabel,
                   TextAlign::Center);
  }

  // ---- now, and the hour you have chosen ----
  float nx = 0, ny = 0;
  timeToUnit(now, nx, ny);
  tick(ui, l.cx, l.cy, now, inner - 8.0f, l.radius + 4.0f, color::text, 2.0f);
  disc(ui, l.cx + nx * (l.radius + 4.0f), l.cy + ny * (l.radius + 4.0f), 3.0f, color::text);

  float hx = 0, hy = 0;
  timeToUnit(target_, hx, hy);
  const float handleX = l.cx + hx * (l.radius - l.thickness * 0.5f);
  const float handleY = l.cy + hy * (l.radius - l.thickness * 0.5f);
  const Rgba handle = ready ? color::accent : color::muted;
  disc(ui, handleX, handleY, l.thickness * 0.62f, rgba(0, 0, 0, 0.5));
  disc(ui, handleX, handleY, l.thickness * 0.46f, handle);

  // ---- the middle: the numbers ----
  disc(ui, l.cx, l.cy, inner - 6.0f, color::bg);

  TextStyle big;
  big.font = FontId::SansBlack;
  big.size = 34.0f;
  big.color = color::text;
  TextStyle sub;
  sub.font = FontId::Sans;
  sub.size = 12.0f;
  sub.color = color::muted;

  text.drawInBox(ui, {l.cx - inner, l.cy - 30.0f, inner * 2, 34.0f},
                 render::Sky::clockStringAt(target_), big, TextAlign::Center);
  const float span = std::fmod(target_ - now + 1.0f, 1.0f);
  text.drawInBox(ui, {l.cx - inner, l.cy + 4.0f, inner * 2, 16.0f},
                 "sleep " + render::Sky::spanString(span <= 0.0005f ? 1.0f : span), sub,
                 TextAlign::Center);
  text.drawInBox(ui, {l.cx - inner, l.cy + 22.0f, inner * 2, 16.0f},
                 "now " + render::Sky::clockStringAt(now), sub, TextAlign::Center);

  // ---- why you cannot, when you cannot ----
  TextStyle note;
  note.font = FontId::Sans;
  note.size = 12.5f;
  note.color = ready ? color::muted : color::danger;
  std::string message;
  if (isVote()) {
    message = "Everyone has to agree before the night moves.";
  } else if (!ready) {
    message = "Not tired yet \xE2\x80\x94 " +
              render::Sky::spanString(sky.hoursUntilTired() / render::Sky::kHoursPerDay) +
              " until you could sleep.";
  } else {
    message = "Drag the dial, or close to just read the clock.";
  }
  text.drawInBox(ui, {l.card.x + 16.0f, l.confirm.y - 30.0f, l.card.w - 32.0f, 18.0f}, message,
                 note, TextAlign::Center);

  button(ui, text, l.cancel, "Close", hoverCancel_, true, false);
  button(ui, text, l.confirm, isVote() ? "Agree" : "Sleep", hoverConfirm_, ready, true);
}

}  // namespace hr::ui
