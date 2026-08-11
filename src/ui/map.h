// The Atlas: a fullscreen top-down world map, waypoints with floating in-world
// markers, and a corner minimap. Ported from js/ui/map.js.
//
// The rendering model is the interesting part and is kept exactly: the world is drawn
// one CHUNK TILE at a time — a 16x16 image, one pixel per block, coloured by averaging
// each block's top-face texture out of the atlas, with cartographic relief shading and
// depth-tinted water. Tiles are cached and only redrawn when an edit dirties their
// chunk. Loaded chunks sample real voxels so builds show up; chunks the player has
// generated but since unloaded are drawn from worldgen's cheap surface prediction, and
// chunks never visited stay fogged.
//
// The one structural change: the JS drew each tile into its own <canvas> and blitted it
// with drawImage. Here each tile is a CPU image uploaded to its own small GL texture and
// drawn as a nearest-sampled quad, which is the same operation — `imageSmoothingEnabled
// = false` meant the browser was nearest-sampling those blits too.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/camera.h"
#include "core/gl.h"
#include "game/inventory.h"
#include "game/player.h"
#include "resource/atlas.h"
#include "ui/confirm.h"
#include "ui/dom.h"
#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"
#include "world/chunk.h"
#include "world/world.h"

namespace hr::ui {

// A player-placed or death marker. Saved with the world, so this is the record shape
// the save format will store.
struct Waypoint {
  float x = 0, y = 64, z = 0;
  std::string name;
  Rgba color = waypointColor(0);
  bool death = false;
};

class Atlas {
 public:
  ~Atlas();

  void attach(const resource::Atlas* atlas) { atlas_ = atlas; }
  void destroy();

  // A world is being torn down: drop every per-world visual.
  void reset();

  std::vector<Waypoint>& waypoints() { return waypoints_; }
  const std::vector<Waypoint>& waypoints() const { return waypoints_; }

  // Somebody else in the same world, as the map wants them: where they are, which
  // way they are facing, and what to call them. Refreshed every frame from the net
  // layer's roster of ghosts, and empty in single player — a map that can show you
  // where your friends are is most of what a map in a shared world is for.
  struct Companion {
    float x = 0, z = 0;
    float yaw = 0;
    std::string name;
  };
  void setCompanions(std::vector<Companion> who) { companions_ = std::move(who); }

  // The Atlas item gates the whole feature, exactly as in the original.
  static bool hasAtlasItem(const game::Inventory& inventory);

  void open(const game::Player& player, float viewWidth, float viewHeight);
  void close();
  bool isOpen() const { return open_; }

  // Fullscreen map: pointer, then draw. Throttled internally to the same 30 ms the JS
  // used, shared with the minimap, so the map can never eat the frame budget.
  void update(Ui2D& ui, Text& text, const UiEvent& event, world::World& world,
              const game::Player& player);
  void draw(Ui2D& ui, Text& text, world::World& world, const game::Player& player);

  // The HUD side: the corner minimap and the in-world floating tags. Called every
  // rendered frame while a world is up.
  void drawHud(Ui2D& ui, Text& text, world::World& world, const game::Player& player,
               const Camera& camera, bool minimapEnabled, double dt);

 private:
  // Asks before a waypoint is thrown away. Shared with the world list and the
  // Gallery; see ui/confirm.h.
  ConfirmPrompt confirm_;

  // One column as the map sees it.
  struct Column {
    world::BlockId id = 0;
    int height = 0;
    int water = 0;
  };

  struct Tile {
    GLuint texture = 0;
    bool loaded = false;  // false = drawn from the seed prediction, upgradeable
  };

  void ensureColors();
  Column sampleLoaded(const world::LoadedChunk& chunk, int lx, int lz) const;
  Column samplePreview(const world::World& world, int wx, int wz) const;
  int surfaceHeight(const world::World& world, int wx, int wz) const;

  Tile renderTile(world::World& world, int cx, int cz);
  void consumeDirty(world::World& world);
  // Returns a tile, or null when the per-redraw budget is spent. `fog` reports a chunk
  // the player has never generated, which costs nothing to draw.
  const Tile* tileAt(world::World& world, int cx, int cz, int& budget, bool& fog);

  void addWaypoint(world::World& world, float wx, float wz);
  void deleteNear(float wx, float wz);
  void buildPanel(Ui2D& ui, Text& text, const UiEvent& event);

  void drawDiamond(Ui2D& ui, float x, float y, float r, Rgba color) const;
  void drawArrow(Ui2D& ui, float x, float y, float angle, float r, Rgba color) const;

  const resource::Atlas* atlas_ = nullptr;
  // Per-block-id average RGB from the atlas, built once.
  std::vector<std::uint8_t> colors_;

  std::unordered_map<world::ChunkKey, Tile> tiles_;

  bool open_ = false;
  float zoom_ = 3.0f;  // screen pixels per block
  float centerX_ = 0, centerZ_ = 0;
  double redrawTimer_ = 0;
  double minimapTimer_ = 99;

  // Drag-to-pan, which doubles as click detection: a press that never moved more than
  // three pixels is a click, which is how one button both pans and drops waypoints.
  bool dragging_ = false;
  bool dragMoved_ = false;
  int dragButton_ = 0;
  float dragX_ = 0, dragY_ = 0;

  std::vector<Waypoint> waypoints_;
  std::vector<Companion> companions_;
  // A stable colour per person, so the arrow you learned to look for stays the
  // colour it was. Derived from the name rather than handed out in join order,
  // which would recolour everyone when somebody leaves.
  static Rgba companionColor(const std::string& name);
  Doc panel_;
  int hoveredTag_ = 0;
  int hoveredIndex_ = 0;
  int editingWaypoint_ = -1;
  TextField nameField_;
  double time_ = 0;
  float viewW_ = 0, viewH_ = 0;
};

}  // namespace hr::ui
