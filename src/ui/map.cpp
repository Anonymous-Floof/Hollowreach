#include "ui/map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio/sfx.h"
#include "core/jsmath.h"
#include "game/player.h"
#include "world/blocks.h"
#include "world/worldgen.h"

namespace hr::ui {
namespace {

// js/ui/map.js:20-24, unchanged. The tile budget and the 30 ms redraw cap are what keep
// the Atlas from eating a frame, and they are shared with the minimap deliberately.
constexpr int kTileBudget = 10;
constexpr double kRedrawSeconds = 0.030;
constexpr int kMinimapTileBudget = 6;
constexpr float kMinimapSize = metric::minimapSize;
constexpr int kMaxWaypoints = 64;
// Alpha below this counts as a cutout pixel and is skipped when averaging a tile.
constexpr int kCutoutAlpha = 40;

enum : int {
  kTagCanvas = 300,
  kTagSwatch = 301,   // index = waypoint
  kTagName = 302,
  kTagCentre = 303,
  kTagDelete = 304,
  kTagPanel = 305,
};

const float kZoomSteps[] = {1, 2, 3, 4, 6, 8};

}  // namespace

Atlas::~Atlas() { destroy(); }

void Atlas::destroy() {
  for (auto& [key, tile] : tiles_) {
    if (tile.texture) glDeleteTextures(1, &tile.texture);
  }
  tiles_.clear();
}

void Atlas::reset() {
  destroy();
  waypoints_.clear();
  open_ = false;
  editingWaypoint_ = -1;
}

bool Atlas::hasAtlasItem(const game::Inventory& inventory) {
  for (const game::ItemStack& s : inventory.slots()) {
    if (!s.empty() && s.key == "atlas") return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Block colours: average each block's top-face tile out of the atlas image.
// ---------------------------------------------------------------------------
void Atlas::ensureColors() {
  if (!colors_.empty() || !atlas_) return;
  const world::BlockRegistry& reg = world::blocks();
  const Image& image = atlas_->image();
  colors_.assign(reg.count() * 3, 0);

  for (const world::BlockDef& b : reg.all()) {
    if (b.render == world::RenderKind::None) continue;
    // Face 2 is +y, the top — which is what a top-down map should be coloured by.
    const resource::TileRef& t = atlas_->tile(b.faceTextures[2]);
    if (t.w <= 0 || t.h <= 0) continue;
    long r = 0, g = 0, bl = 0, count = 0;
    for (int y = 0; y < t.h; ++y) {
      for (int x = 0; x < t.w; ++x) {
        const Rgba p = image.get(t.x + x, t.y + y);
        if (p.a < kCutoutAlpha) continue;  // skip cutout pixels
        r += p.r;
        g += p.g;
        bl += p.b;
        ++count;
      }
    }
    if (count == 0) continue;
    const std::size_t o = static_cast<std::size_t>(b.id) * 3;
    colors_[o] = static_cast<std::uint8_t>(r / count);
    colors_[o + 1] = static_cast<std::uint8_t>(g / count);
    colors_[o + 2] = static_cast<std::uint8_t>(bl / count);
  }

  // Water gets a hand-picked deep blue, because its tile is translucent and averaging
  // it produces a washed-out grey.
  const std::size_t wo = static_cast<std::size_t>(world::wk().water) * 3;
  if (wo + 2 < colors_.size()) {
    colors_[wo] = static_cast<std::uint8_t>(kWaterMapRgb[0]);
    colors_[wo + 1] = static_cast<std::uint8_t>(kWaterMapRgb[1]);
    colors_[wo + 2] = static_cast<std::uint8_t>(kWaterMapRgb[2]);
  }
}

// ---------------------------------------------------------------------------
// Column sampling
// ---------------------------------------------------------------------------
Atlas::Column Atlas::sampleLoaded(const world::LoadedChunk& lc, int lx, int lz) const {
  const world::BlockRegistry& reg = world::blocks();
  const world::ChunkData& d = *lc.chunk.data;
  int water = 0;
  for (int y = world::WH - 1; y >= 0; --y) {
    const world::BlockId id = d.voxels[world::localIdx(lx, y, lz)];
    if (id == 0) continue;
    const world::BlockDef& b = reg.def(id);
    // Plants do not hide the ground they stand on.
    if (b.render == world::RenderKind::Cross) continue;
    if (id == world::wk().water) {
      ++water;
      continue;
    }
    return {id, y, water};
  }
  return {world::wk().bedrock, 0, water};
}

Atlas::Column Atlas::samplePreview(const world::World& world, int wx, int wz) const {
  const world::SurfacePreview p = world::surfacePreview(world.noise(), wx, wz,
                                                        world.genVersion());
  if (p.key == "water") {
    return {world::wk().greystone, std::max(2, p.h),
            std::max(1, world::seaLevel(world.genVersion()) - p.h)};
  }
  const world::BlockId id = world::blocks().idOf(p.key);
  return {id != 0 ? id : world::wk().turf, p.h, 0};
}

int Atlas::surfaceHeight(const world::World& world, int wx, int wz) const {
  const int cx = wx >= 0 ? wx >> 4 : ~((~wx) >> 4);
  const int cz = wz >= 0 ? wz >> 4 : ~((~wz) >> 4);
  if (const world::LoadedChunk* lc = world.chunkReady(cx, cz) ? world.chunkAt(cx, cz) : nullptr) {
    const Column c = sampleLoaded(*lc, wx - cx * world::CX, wz - cz * world::CZ);
    return c.height + c.water;
  }
  const Column c = samplePreview(world, wx, wz);
  return c.height + c.water;
}

// ---------------------------------------------------------------------------
// Chunk tiles
// ---------------------------------------------------------------------------
Atlas::Tile Atlas::renderTile(world::World& world, int cx, int cz) {
  ensureColors();
  Image tile(world::CX, world::CZ, Rgba {0, 0, 0, 255});
  // Ungenerated is not the same as loaded now that generation is a job: a chunk
  // whose job is still out would draw as a tile of air.
  const world::LoadedChunk* lc = world.chunkReady(cx, cz) ? world.chunkAt(cx, cz) : nullptr;
  const int baseX = cx * world::CX;
  const int baseZ = cz * world::CZ;

  // Heights of the row north of the tile, so row 0 gets relief shading too.
  float north[world::CX];
  for (int x = 0; x < world::CX; ++x) {
    north[x] = static_cast<float>(surfaceHeight(world, baseX + x, baseZ - 1));
  }

  for (int z = 0; z < world::CZ; ++z) {
    for (int x = 0; x < world::CX; ++x) {
      const Column c = lc ? sampleLoaded(*lc, x, z)
                          : samplePreview(world, baseX + x, baseZ + z);
      const std::size_t o = static_cast<std::size_t>(c.id) * 3;
      float r = o + 2 < colors_.size() ? colors_[o] : 128;
      float g = o + 2 < colors_.size() ? colors_[o + 1] : 128;
      float b = o + 2 < colors_.size() ? colors_[o + 2] : 128;

      if (c.water > 0) {
        // Deeper water reads bluer and darker.
        const float k = std::min(0.88f, 0.42f + static_cast<float>(c.water) * 0.055f);
        const float fade = 1.0f - static_cast<float>(c.water) * 0.02f;
        r = r * (1 - k) + static_cast<float>(kWaterMapRgb[0]) * k * fade;
        g = g * (1 - k) + static_cast<float>(kWaterMapRgb[1]) * k * fade;
        b = b * (1 - k) + static_cast<float>(kWaterMapRgb[2]) * k;
      }

      // Classic cartographic hillshade: brighter stepping up from the northern
      // neighbour, darker stepping down.
      const float hs = static_cast<float>(c.height + c.water);
      const float dh = hs - north[x];
      const float m = dh > 0 ? std::min(1.22f, 1.0f + dh * 0.07f)
                             : dh < 0 ? std::max(0.72f, 1.0f + dh * 0.07f) : 1.0f;
      north[x] = hs;

      tile.set(x, z,
               Rgba {static_cast<std::uint8_t>(std::min(255.0f, r * m)),
                     static_cast<std::uint8_t>(std::min(255.0f, g * m)),
                     static_cast<std::uint8_t>(std::min(255.0f, b * m)), 255});
    }
  }

  Tile out;
  out.loaded = lc != nullptr;
  glGenTextures(1, &out.texture);
  glBindTexture(GL_TEXTURE_2D, out.texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, world::CX, world::CZ, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, tile.data());
  // image-rendering: pixelated — one map pixel per block, never filtered.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  return out;
}

void Atlas::consumeDirty(world::World& world) {
  if (world.mapDirty().empty()) return;
  for (world::ChunkKey key : world.mapDirty()) {
    auto it = tiles_.find(key);
    if (it == tiles_.end()) continue;
    if (it->second.texture) glDeleteTextures(1, &it->second.texture);
    tiles_.erase(it);
  }
  world.mapDirty().clear();
}

const Atlas::Tile* Atlas::tileAt(world::World& world, int cx, int cz, int& budget,
                                 bool& fog) {
  fog = false;
  const world::ChunkKey key = world::chunkKey(cx, cz);
  if (!world.isExplored(cx, cz)) {
    fog = true;
    return nullptr;
  }
  auto it = tiles_.find(key);
  // A prediction tile upgrades to the real thing once its chunk is loaded.
  if (it != tiles_.end() && !it->second.loaded && world.chunkReady(cx, cz) && budget > 0) {
    if (it->second.texture) glDeleteTextures(1, &it->second.texture);
    tiles_.erase(it);
    it = tiles_.end();
  }
  if (it == tiles_.end()) {
    if (budget <= 0) return nullptr;
    --budget;
    // Bounded cache, as in the JS: drop a thousand of the oldest entries when it grows
    // past nine thousand tiles.
    if (tiles_.size() > 9000) {
      int drop = 1000;
      for (auto k = tiles_.begin(); k != tiles_.end() && drop > 0;) {
        if (k->second.texture) glDeleteTextures(1, &k->second.texture);
        k = tiles_.erase(k);
        --drop;
      }
    }
    it = tiles_.emplace(key, renderTile(world, cx, cz)).first;
  }
  return &it->second;
}

// ---------------------------------------------------------------------------
// Fullscreen map
// ---------------------------------------------------------------------------
void Atlas::open(const game::Player& player, float viewWidth, float viewHeight) {
  open_ = true;
  centerX_ = player.pos().x;
  centerZ_ = player.pos().z;
  viewW_ = viewWidth;
  viewH_ = viewHeight;
  editingWaypoint_ = -1;
  redrawTimer_ = kRedrawSeconds;  // draw immediately on the first frame
}

void Atlas::close() {
  open_ = false;
  dragging_ = false;
  editingWaypoint_ = -1;
}

void Atlas::addWaypoint(world::World& world, float wx, float wz) {
  if (waypoints_.size() >= kMaxWaypoints) return;
  int placed = 0;
  for (const Waypoint& w : waypoints_) {
    if (!w.death) ++placed;
  }
  const int n = placed + 1;
  Waypoint w;
  w.x = jsmath::jsRoundF(wx * 10.0f) / 10.0f;
  w.z = jsmath::jsRoundF(wz * 10.0f) / 10.0f;
  w.y = static_cast<float>(surfaceHeight(world, static_cast<int>(std::floor(wx)),
                                         static_cast<int>(std::floor(wz))));
  w.name = "Waypoint " + std::to_string(n);
  w.color = kWaypointColors[(n - 1) % kWaypointColorCount];
  waypoints_.push_back(std::move(w));
}

void Atlas::deleteNear(float wx, float wz) {
  const float tolerance = 12.0f / zoom_;  // about twelve screen pixels
  int best = -1;
  float bestDistance = tolerance;
  for (std::size_t i = 0; i < waypoints_.size(); ++i) {
    const float dx = waypoints_[i].x - wx;
    const float dz = waypoints_[i].z - wz;
    const float d = std::sqrt(dx * dx + dz * dz);
    if (d < bestDistance) {
      bestDistance = d;
      best = static_cast<int>(i);
    }
  }
  if (best >= 0) waypoints_.erase(waypoints_.begin() + best);
}

void Atlas::buildPanel(Ui2D& ui, Text& text, const UiEvent& event) {
  panel_.reset(&text);
  // #map-side { top: 16px; right: 16px; width: 240px; max-height: calc(100vh - 32px) }
  Style root = Doc::column(0, Align::Stretch);
  root.width = 240;
  root.padding = Edges(12, 14);
  root.bg = rgba(14, 18, 24, 0.88);
  root.border = rgba(255, 255, 255, 0.10);
  root.borderWidth = 1;
  root.radius = 10;
  root.maxHeight = ui.height() - 32.0f;
  root.scrollY = true;
  panel_.begin(root, kTagPanel);

  Style heading;
  heading.margin = Edges(0, 0, 8, 0);
  TextStyle h3 = widget::h3();
  h3.size = 15;
  panel_.label("Waypoints", h3, heading);

  if (waypoints_.empty()) {
    Style hint;
    hint.margin = Edges(8, 0, 0, 0);
    hint.maxWidth = 212;
    TextStyle ts = widget::muted(11.5f);
    panel_.label("Click anywhere on the map to drop a waypoint.", ts, hint);
  }

  // .wp-list { flex-direction: column; gap: 6px }
  Style list = Doc::column(6, Align::Stretch);
  panel_.begin(list);
  for (std::size_t i = 0; i < waypoints_.size(); ++i) {
    const Waypoint& w = waypoints_[i];
    Style row = Doc::row(6, Justify::Start, Align::Center);
    panel_.begin(row);

    // .wp-swatch — an 18px colour chip that cycles the palette on click.
    Style swatch;
    swatch.width = 18;
    swatch.height = 18;
    swatch.radius = 4;
    swatch.bg = w.color;
    swatch.border = rgba(0, 0, 0, 0.5);
    swatch.borderWidth = 1;
    panel_.box(swatch, kTagSwatch, static_cast<int>(i));

    Style field = Doc::row(0, Justify::Start, Align::Center);
    field.grow = 1;
    field.minWidth = 0;
    field.bg = color::fieldBg;
    field.border = color::slotEdge;
    field.borderWidth = 1;
    field.radius = 5;
    field.padding = Edges(4, 7);
    panel_.box(field, kTagName, static_cast<int>(i));

    for (int tag : {kTagCentre, kTagDelete}) {
      const bool hovered = hoveredTag_ == tag && hoveredIndex_ == static_cast<int>(i);
      Style b = widget::btnSmall(hovered, false, widget::ButtonKind::Normal);
      b.width = kAuto;
      b.margin = Edges(0);
      b.padding = Edges(3, 8);
      panel_.begin(b, tag, static_cast<int>(i));
      TextStyle ts;
      ts.size = 12;
      // ◎ centres the map here; ✕ deletes.
      panel_.label(tag == kTagCentre ? "\xE2\x97\x8E" : "\xE2\x9C\x95", ts);
      panel_.end();
    }
    panel_.end();
  }
  panel_.end();

  Style hint;
  hint.margin = Edges(8, 0, 0, 0);
  hint.maxWidth = 212;
  panel_.label("Click: add \xC2\xB7 right-click: remove \xC2\xB7 drag: pan \xC2\xB7 "
               "scroll: zoom \xC2\xB7 M/Esc: close",
               widget::muted(11.5f), hint);
  panel_.end();

  panel_.layout({ui.width() - 16.0f - 240.0f, 16.0f, 240.0f, ui.height() - 32.0f});
  (void)event;
}

void Atlas::update(Ui2D& ui, Text& text, const UiEvent& event, world::World& world,
                   const game::Player& player) {
  time_ += event.dt;
  // The tile clock has to advance from the frame that is actually drawing the map. It
  // used to be advanced only from the HUD path, which meant the fullscreen map rendered
  // its first ten tiles and then never asked for another.
  redrawTimer_ += event.dt;
  viewW_ = ui.width();
  viewH_ = ui.height();

  buildPanel(ui, text, event);
  const int hit = panel_.hitTest(event.mouseX, event.mouseY);
  // main.js:120 hung one listener on the document and ticked whenever the click
  // landed inside a <button>; this is that listener.
  if (event.leftClick && panel_.clickedButton(hit)) audio::sfx::uiClick();
  hoveredTag_ = hit >= 0 ? panel_.node(hit).tag : 0;
  hoveredIndex_ = hit >= 0 ? panel_.node(hit).index : 0;
  const bool overPanel = panel_.root() >= 0 &&
                         panel_.node(panel_.root()).rect.contains(event.mouseX, event.mouseY);

  // The waypoint name field, when one is being edited.
  if (editingWaypoint_ >= 0 && editingWaypoint_ < static_cast<int>(waypoints_.size())) {
    bool submitted = false;
    if (nameField_.handle(event, submitted)) {
      waypoints_[static_cast<std::size_t>(editingWaypoint_)].name = nameField_.text();
    }
    if (submitted) {
      nameField_.setFocused(false, const_cast<Input*>(event.input));
      editingWaypoint_ = -1;
    }
  }

  if (event.leftClick) {
    switch (hoveredTag_) {
      case kTagSwatch: {
        Waypoint& w = waypoints_[static_cast<std::size_t>(hoveredIndex_)];
        int index = 0;
        for (int k = 0; k < kWaypointColorCount; ++k) {
          const Rgba c = kWaypointColors[k];
          if (c.r == w.color.r && c.g == w.color.g && c.b == w.color.b) {
            index = k;
            break;
          }
        }
        w.color = kWaypointColors[(index + 1) % kWaypointColorCount];
        break;
      }
      case kTagName: {
        editingWaypoint_ = hoveredIndex_;
        nameField_.maxLength = 24;
        nameField_.setText(waypoints_[static_cast<std::size_t>(hoveredIndex_)].name);
        nameField_.setFocused(true, const_cast<Input*>(event.input));
        break;
      }
      case kTagCentre:
        centerX_ = waypoints_[static_cast<std::size_t>(hoveredIndex_)].x;
        centerZ_ = waypoints_[static_cast<std::size_t>(hoveredIndex_)].z;
        break;
      case kTagDelete:
        waypoints_.erase(waypoints_.begin() + hoveredIndex_);
        if (editingWaypoint_ == hoveredIndex_) {
          nameField_.setFocused(false, const_cast<Input*>(event.input));
          editingWaypoint_ = -1;
        }
        break;
      default:
        if (!overPanel) {
          nameField_.setFocused(false, const_cast<Input*>(event.input));
          editingWaypoint_ = -1;
        }
        break;
    }
  }

  // Drag to pan; a press that never moved is a click that drops or removes a waypoint.
  const auto screenToWorld = [&](float sx, float sy, float& wx, float& wz) {
    wx = centerX_ + (sx - viewW_ * 0.5f) / zoom_;
    wz = centerZ_ + (sy - viewH_ * 0.5f) / zoom_;
  };

  if (!overPanel && (event.leftClick || event.rightClick)) {
    dragging_ = true;
    dragMoved_ = false;
    dragButton_ = event.leftClick ? 0 : 1;
    dragX_ = event.mouseX;
    dragY_ = event.mouseY;
  }
  if (dragging_) {
    const float dx = event.mouseX - dragX_;
    const float dy = event.mouseY - dragY_;
    if (std::fabs(dx) + std::fabs(dy) > 3.0f) dragMoved_ = true;
    if (dragMoved_) {
      centerX_ -= dx / zoom_;
      centerZ_ -= dy / zoom_;
      dragX_ = event.mouseX;
      dragY_ = event.mouseY;
    }
    const bool released = dragButton_ == 0 ? event.leftRelease : event.rightRelease;
    if (released) {
      dragging_ = false;
      if (!dragMoved_) {
        float wx = 0, wz = 0;
        screenToWorld(event.mouseX, event.mouseY, wx, wz);
        if (dragButton_ == 1) deleteNear(wx, wz);
        else addWaypoint(world, wx, wz);
      }
    }
  }

  if (event.wheel != 0.0f && !overPanel) {
    const int direction = event.wheel > 0 ? 1 : -1;
    int index = 2;
    for (int i = 0; i < static_cast<int>(std::size(kZoomSteps)); ++i) {
      if (kZoomSteps[i] == zoom_) index = i;
    }
    index = std::max(0, std::min(static_cast<int>(std::size(kZoomSteps)) - 1, index - direction));
    zoom_ = kZoomSteps[index];
  }

  buildPanel(ui, text, event);
  (void)player;
}

void Atlas::drawDiamond(Ui2D& ui, float x, float y, float r, Rgba color) const {
  const Vec2 points[4] = {{x, y - r}, {x + r, y}, {x, y + r}, {x - r, y}};
  ui.fillPoly(points, 4, color);
  ui.strokePoly(points, 4, rgba(10, 12, 16, 0.9), 1.5f);
}

void Atlas::drawArrow(Ui2D& ui, float x, float y, float angle, float r, Rgba color) const {
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  const auto rot = [&](float px, float py) {
    return Vec2 {x + px * c - py * s, y + px * s + py * c};
  };
  const Vec2 points[4] = {rot(0, -r), rot(r * 0.7f, r), rot(0, r * 0.45f), rot(-r * 0.7f, r)};
  ui.fillPoly(points, 4, color);
  ui.strokePoly(points, 4, rgba(10, 12, 16, 0.9), 1.5f);
}

void Atlas::draw(Ui2D& ui, Text& text, world::World& world, const game::Player& player) {
  const float w = ui.width();
  const float h = ui.height();
  ui.fillRect({0, 0, w, h}, color::mapBg);

  // Tiles are only re-rendered on the throttled clock; the existing cache is drawn every
  // frame so panning stays smooth.
  const bool refresh = redrawTimer_ >= kRedrawSeconds;
  if (refresh) {
    redrawTimer_ = 0;
    consumeDirty(world);
  }
  int budget = refresh ? kTileBudget : 0;

  const float z = zoom_;
  const int cx0 = static_cast<int>(std::floor((centerX_ - w * 0.5f / z) / world::CX));
  const int cx1 = static_cast<int>(std::floor((centerX_ + w * 0.5f / z) / world::CX));
  const int cz0 = static_cast<int>(std::floor((centerZ_ - h * 0.5f / z) / world::CZ));
  const int cz1 = static_cast<int>(std::floor((centerZ_ + h * 0.5f / z) / world::CZ));

  // Near-centre first, so detail fills out from where the player is looking.
  struct Cell {
    int x, z;
    float distance;
  };
  std::vector<Cell> order;
  const float ccx = (static_cast<float>(cx0) + static_cast<float>(cx1)) * 0.5f;
  const float ccz = (static_cast<float>(cz0) + static_cast<float>(cz1)) * 0.5f;
  for (int tz = cz0; tz <= cz1; ++tz) {
    for (int tx = cx0; tx <= cx1; ++tx) {
      const float dx = static_cast<float>(tx) - ccx;
      const float dz = static_cast<float>(tz) - ccz;
      order.push_back({tx, tz, dx * dx + dz * dz});
    }
  }
  std::sort(order.begin(), order.end(),
            [](const Cell& a, const Cell& b) { return a.distance < b.distance; });

  const float tileScreen = static_cast<float>(world::CX) * z;
  for (const Cell& cell : order) {
    bool fog = false;
    const Tile* tile = tileAt(world, cell.x, cell.z, budget, fog);
    // jsRoundF, not std::round: consecutive tiles are exactly one tile apart, and
    // std::round's away-from-zero half would make that spacing 17 across the origin.
    const float sx = jsmath::jsRoundF((static_cast<float>(cell.x * world::CX) - centerX_) * z +
                                      w * 0.5f);
    const float sy = jsmath::jsRoundF((static_cast<float>(cell.z * world::CZ) - centerZ_) * z +
                                      h * 0.5f);
    if (fog) {
      ui.fillRect({sx, sy, tileScreen, tileScreen}, color::mapFog);
      continue;
    }
    if (!tile) {
      ui.fillRect({sx, sy, tileScreen, tileScreen}, color::pending);
      continue;
    }
    ui.setTexture(tile->texture);
    ui.texturedRect({sx, sy, tileScreen, tileScreen}, 0, 0, 1, 1);
    ui.setTexture(0);
  }

  // Waypoints, each with a label plate above it.
  TextStyle labelStyle;
  labelStyle.size = 12;
  const TextMetrics lm = text.metrics(labelStyle);
  for (const Waypoint& wp : waypoints_) {
    const float sx = (wp.x - centerX_) * z + w * 0.5f;
    const float sy = (wp.z - centerZ_) * z + h * 0.5f;
    if (sx < -20 || sx > w + 20 || sy < -20 || sy > h + 20) continue;
    drawDiamond(ui, sx, sy, 6, wp.color);
    const std::string label = wp.death ? "\xE2\x98\xA0 " + wp.name : wp.name;
    const float tw = text.measure(label, labelStyle);
    ui.fillRect({sx - tw * 0.5f - 4, sy - 24, tw + 8, 15}, rgba(8, 10, 14, 0.75));
    TextStyle ts = labelStyle;
    ts.color = rgb(0xe8edf2);
    text.draw(ui, sx - tw * 0.5f, sy - 12.5f, label, ts);
  }
  (void)lm;

  // The player: a heading arrow. The map's y axis is world z, so the yaw has to be
  // remapped the same way the JS did with atan2(-sin, cos).
  const float px = (player.pos().x - centerX_) * z + w * 0.5f;
  const float py = (player.pos().z - centerZ_) * z + h * 0.5f;
  drawArrow(ui, px, py, std::atan2(-std::sin(player.yaw()), std::cos(player.yaw())), 8,
            color::white);

  panel_.paint(ui);
  // The waypoint name fields, which draw their own caret.
  for (int i = 0; i < panel_.count(); ++i) {
    const Node& n = panel_.node(i);
    if (n.tag != kTagName) continue;
    TextStyle ts;
    ts.size = 12;
    ts.color = rgb(0xe8edf2);
    const Rect box {n.rect.x + 7, n.rect.y, n.rect.w - 14, n.rect.h};
    if (n.index == editingWaypoint_) {
      nameField_.draw(ui, text, box, ts, time_);
    } else {
      text.drawInBox(ui, box, waypoints_[static_cast<std::size_t>(n.index)].name, ts);
    }
  }
}

// ---------------------------------------------------------------------------
// HUD side: the corner minimap and the in-world tags
// ---------------------------------------------------------------------------
void Atlas::drawHud(Ui2D& ui, Text& text, world::World& world, const game::Player& player,
                    const Camera& camera, bool minimapEnabled, double dt) {

  if (minimapEnabled && !open_) {
    const float S = kMinimapSize;
    const Rect box {ui.width() - metric::minimapInset - S, metric::minimapInset, S, S};
    ui.fillRect(box, color::mapBg, 8);

    minimapTimer_ += dt;
    const bool refresh = minimapTimer_ >= kRedrawSeconds;
    if (refresh) {
      minimapTimer_ = 0;
      consumeDirty(world);
    }
    int budget = refresh ? kMinimapTileBudget : 0;

    const float px = player.pos().x;
    const float pz = player.pos().z;
    const int cx0 = static_cast<int>(std::floor((px - S * 0.5f) / world::CX));
    const int cx1 = static_cast<int>(std::floor((px + S * 0.5f) / world::CX));
    const int cz0 = static_cast<int>(std::floor((pz - S * 0.5f) / world::CZ));
    const int cz1 = static_cast<int>(std::floor((pz + S * 0.5f) / world::CZ));

    // One block per pixel, clipped to the rounded frame. Fog simply stays the backdrop.
    ui.pushClip(box);
    for (int tz = cz0; tz <= cz1; ++tz) {
      for (int tx = cx0; tx <= cx1; ++tx) {
        bool fog = false;
        const Tile* tile = tileAt(world, tx, tz, budget, fog);
        if (!tile || fog) continue;
        const float sx =
            box.x + jsmath::jsRoundF(static_cast<float>(tx * world::CX) - px + S * 0.5f);
        const float sy =
            box.y + jsmath::jsRoundF(static_cast<float>(tz * world::CZ) - pz + S * 0.5f);
        ui.setTexture(tile->texture);
        ui.texturedRectMasked({sx, sy, world::CX, world::CZ}, 0, 0, 1, 1, box, 8.0f);
        ui.setTexture(0);
      }
    }
    // Waypoint blips, clamped to the rim so an off-screen one still points the way.
    for (const Waypoint& wp : waypoints_) {
      float dx = wp.x - px;
      float dz = wp.z - pz;
      const float d = std::max(std::fabs(dx), std::fabs(dz));
      const float limit = S * 0.5f - 6.0f;
      if (d > limit) {
        dx = dx / d * limit;
        dz = dz / d * limit;
      }
      drawDiamond(ui, box.x + S * 0.5f + dx, box.y + S * 0.5f + dz, 3.5f, wp.color);
    }
    drawArrow(ui, box.x + S * 0.5f, box.y + S * 0.5f,
              std::atan2(-std::sin(player.yaw()), std::cos(player.yaw())), 6, color::white);
    ui.popClip();
    ui.strokeRect(box, rgba(255, 255, 255, 0.18), 2, 8);
  }

  if (open_) return;

  // Floating waypoint tags, projected through the camera. Behind the eye or outside the
  // frustum they simply are not drawn, which is what the JS `display: none` did.
  const float* vp = camera.viewProj().data();
  TextStyle tagStyle;
  tagStyle.size = 12;
  tagStyle.withShadow(0, 1, 2, rgba(0, 0, 0, 0.8));
  const TextMetrics tm = text.metrics(tagStyle);
  for (const Waypoint& wp : waypoints_) {
    const float x = wp.x;
    const float y = wp.y + 1.6f;
    const float zc = wp.z;
    const float cw = vp[3] * x + vp[7] * y + vp[11] * zc + vp[15];
    if (cw <= 0.1f) continue;
    const float ndcX = (vp[0] * x + vp[4] * y + vp[8] * zc + vp[12]) / cw;
    const float ndcY = (vp[1] * x + vp[5] * y + vp[9] * zc + vp[13]) / cw;
    if (ndcX < -1.05f || ndcX > 1.05f || ndcY < -1.05f || ndcY > 1.05f) continue;

    const float dx = wp.x - player.pos().x;
    const float dz = wp.z - player.pos().z;
    const float distance = std::sqrt(dx * dx + dz * dz);
    char label[96];
    std::snprintf(label, sizeof(label), "%s %s \xC2\xB7 %dm",
                  wp.death ? "\xE2\x98\xA0" : "\xE2\x97\x86", wp.name.c_str(),
                  static_cast<int>(std::lround(distance)));

    const float sx = (ndcX + 1.0f) * 0.5f * ui.width();
    const float sy = (1.0f - ndcY) * 0.5f * ui.height();
    const float alpha = distance < 12.0f ? 0.45f : 0.92f;
    const float tw = text.measure(label, tagStyle);
    // translate(-50%, -100%): centred horizontally, sitting above the point.
    const Rect plate {sx - (tw + 16) * 0.5f, sy - (tm.lineHeight + 4), tw + 16,
                      tm.lineHeight + 4};
    ui.fillRect(plate, fade(rgba(10, 14, 18, 0.55), alpha), 4);
    TextStyle ts = tagStyle;
    ts.color = fade(wp.color, alpha);
    for (int i = 0; i < ts.shadowCount; ++i) ts.shadows[i].color = fade(ts.shadows[i].color, alpha);
    text.drawInBox(ui, {plate.x + 8, plate.y + 2, tw, tm.lineHeight}, label, ts);
  }
}

}  // namespace hr::ui
