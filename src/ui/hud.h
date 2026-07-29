// The in-world heads-up display, ported from js/ui/hud.js and the #hud rules in
// css/style.css:296-347.
//
// Unlike the card screens, the HUD is drawn directly rather than through the layout
// engine. Every one of its parts is absolutely positioned against a viewport edge —
// there is no flow to solve — and it is the only interface that runs on every single
// frame, which is why the JS dirty-checked each widget against a signature string
// before touching the DOM. There is no DOM to touch here, so that optimisation is
// simply gone: emitting the quads costs less than building the signatures would.

#pragma once

#include <string>
#include <vector>

#include "core/camera.h"
#include "game/inventory.h"
#include "render/iconatlas.h"
#include "ui/text.h"
#include "ui/ui2d.h"

namespace hr::game {
class Player;
}
namespace hr::world {
class World;
}
namespace hr::render {
class Sky;
}

namespace hr::ui {

// What the HUD needs from the frame, gathered by App so the HUD does not reach into
// six subsystems itself.
struct HudFrame {
  const game::Player* player = nullptr;
  const game::Inventory* inventory = nullptr;
  const world::World* world = nullptr;
  const render::Sky* sky = nullptr;
  const render::IconAtlas* icons = nullptr;
  double fps = 0;
  // The block under the crosshair, for the debug panel's "looking at" line.
  bool hasTarget = false;
  int targetX = 0, targetY = 0, targetZ = 0;
  // 0..1 progress of the block currently being mined.
  float breakFraction = 0;
  // Filled in once multiplayer lands; empty means single player.
  std::string netLine;

  // One floating name above each remote player. The camera is needed to project
  // them, and is null in single player, which is also what switches the whole
  // thing off — an empty list costs one branch.
  struct Nameplate {
    Vec3 pos;
    std::string name;
    float health = 20.0f;
  };
  std::vector<Nameplate> nameplates;
  const Camera* camera = nullptr;
};

class Hud {
 public:
  void toggleDebug() { showDebug_ = !showDebug_; }
  bool debugVisible() const { return showDebug_; }

  // #held-item-label: flashes the name for 1.2s, then fades over .3s.
  void flashHeld(const std::string& name);
  void update(double dt, const game::Inventory& inventory);

  // Draws the crosshair, break bar, stats column, hotbar, held-item label and the F3
  // panel. The minimap is drawn by the Atlas module, which owns its tile cache.
  void draw(Ui2D& ui, Text& text, const HudFrame& frame);

  // Where the hotbar ended up, so the inventory screen's quick-move animation and the
  // click handler agree with what is on screen.
  Rect hotbarSlotRect(Ui2D& ui, int slot) const;

 private:
  void drawCrosshair(Ui2D& ui, Text& text) const;
  void drawStats(Ui2D& ui, Text& text, const game::Player& player, float bottom) const;
  void drawHotbar(Ui2D& ui, Text& text, const HudFrame& frame, float bottom) const;
  void drawDebug(Ui2D& ui, Text& text, const HudFrame& frame) const;
  // Projected through the camera; behind the eye or off screen they are skipped,
  // which is what the web build's `display: none` did.
  void drawNameplates(Ui2D& ui, Text& text, const HudFrame& frame) const;

  bool showDebug_ = false;
  std::string heldLabel_;
  double heldLabelAge_ = 1e9;  // starts expired
  int lastSelected_ = -1;
};

}  // namespace hr::ui
