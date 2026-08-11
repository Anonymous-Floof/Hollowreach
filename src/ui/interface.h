// The interface as one object: the drawing layer, the fonts, and every screen.
//
// App owned nine separate interface objects in the web build (hud, menu, invUI,
// recipeBook, map, notify…) and wired them together with assigned function
// properties. Here they live behind one facade with an explicit callback struct, so
// App has one member to hold and one call per frame, and so the screens can talk to
// each other (the inventory's tooltip, the map's minimap on the HUD) without going
// back out through App.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/input.h"
#include "core/shader.h"
#include "game/blockentities.h"
#include "game/inventory.h"
#include "render/iconatlas.h"
#include "ui/backdrop.h"
#include "ui/chat.h"
#include "ui/gallery.h"
#include "ui/hud.h"
#include "core/camera.h"
#include "ui/inventoryui.h"
#include "ui/map.h"
#include "ui/menu.h"
#include "ui/packsui.h"
#include "ui/recipebook.h"
#include "ui/notify.h"
#include "ui/settingsui.h"
#include "ui/text.h"
#include "ui/timewheel.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"
#include "world/blocks.h"

namespace hr {
class Window;
}
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

// Which screen is up. Mirrors the AppState machine, but the interface needs its own
// copy because a screen can be open over a running world (inventory, map).
enum class Screen {
  None,      // playing, HUD only
  Boot,
  Menu,
  Pause,
  Settings,
  Inventory,
  RecipeBook,
  Map,
  Gallery,
  // Which resource packs are on, and in what order. Its own screen rather than a
  // page of the menu card because it has to be reachable from the pause menu too,
  // and menu pages only exist on the main menu.
  Packs,
  // The bed's 24-hour dial. A screen rather than a HUD widget because it takes the
  // cursor and pauses the world the same way a chest does.
  TimeWheel,
};

// Everything a screen asks App to do. Assigned once at startup.
struct UiCallbacks {
  std::function<void()> resume;
  std::function<void()> saveAndQuit;
  std::function<void()> quitGame;
  std::function<void(const std::string& key)> settingChanged;
  // Leaves whatever screen is up. App owns the state machine, so a Back button says
  // "go back" and App decides where that is.
  std::function<void()> closeScreen;
  std::function<void()> openSettings;
  std::function<void()> openPacks;
  // Opens the recipe book from a crafting screen, and closes it back to wherever it
  // was opened from. One callback because it is one toggle.
  std::function<void()> toggleRecipeBook;
  std::function<void()> openGallery;
  // The block entity behind an open station screen, or nullptr when it has gone.
  std::function<game::BlockEntity*()> currentStation;
  std::function<void()> closeStation;
};

// Per-frame state the interface draws from. Assembled by App.
struct UiFrame {
  game::Player* player = nullptr;
  world::World* world = nullptr;
  render::Sky* sky = nullptr;
  game::Inventory* inventory = nullptr;
  const Camera* camera = nullptr;
  const resource::Atlas* atlas = nullptr;
  double fps = 0;
  bool hasTarget = false;
  int targetX = 0, targetY = 0, targetZ = 0;
  float breakFraction = 0;
  float hurtFlash = 0;  // 0..1, see HudFrame::hurtFlash
  std::string version;

  // Multiplayer. Empty in single player, which is what switches the whole
  // nameplate path off.
  struct Nameplate {
    Vec3 pos;
    std::string name;
    float health = 20.0f;
    float yaw = 0.0f;
  };
  std::vector<Nameplate> nameplates;
  std::string netLine;
};

class Interface {
 public:
  Interface();
  ~Interface();

  bool init(ShaderCache& shaders, const render::IconAtlas* icons);
  void destroy();

  UiCallbacks callbacks;

  Screen screen() const { return screen_; }
  void setScreen(Screen s);

  // Makes the interface deaf to the mouse for a moment, so the click that opened
  // a screen is not also spent inside it. Armed on every screen change; see
  // MouseGuard for why it is not simply a timer. Public because anything that
  // puts a screen up out of band should be able to say so.
  void guardMouse() { mouseGuard_.arm(); }
  // The Gallery, opened to choose a picture for a painting rather than to browse.
  // Not a Screen of its own because it IS the gallery — same scan, same thumbnails,
  // same grid — with its cards wired to a different action.
  void openPaintingPicker() {
    pendingPicker_ = true;
    setScreen(Screen::Gallery);
  }
  // True while a screen wants the cursor and the keyboard — App uses this to decide
  // whether to hold pointer capture and whether gameplay keybinds fire.
  bool wantsCursor() const;
  bool blocksGameplay() const;

  Hud& hud() { return hud_; }
  Notify& notify() { return notify_; }
  Menu& menu() { return menu_; }
  // Not a Screen: it draws over a live world and App keeps that world running
  // while it is up. See ui/chat.h.
  Chat& chat() { return chat_; }

  // One call for every screen that decides between washing over what is behind it
  // and painting its own opaque background. App sets it whenever the chosen menu
  // picture changes, so no screen can be left with the wrong answer.
  void setHasBackdrop(bool on) {
    menu_.setHasBackdrop(on);
    settingsScreen_.setHasBackdrop(on);
    packsScreen_.setHasBackdrop(on);
  }
  PacksScreen& packs() { return packsScreen_; }
  InventoryUI& inventory() { return inventoryUI_; }

  // Draws one texture over the whole window, cropped to cover rather than stretched.
  // Used for the menu's chosen background picture.
  void drawFullscreenImage(const Window& window, GLuint texture, float imageAspect);
  Atlas& atlas() { return atlas_; }
  Gallery& gallery() { return gallery_; }
  TimeWheel& timeWheel() { return timeWheel_; }
  RecipeBook& recipeBook() { return recipeBook_; }
  Ui2D& ui() { return ui_; }
  Text& text() { return text_; }

  // Opens the inventory in the mode a station calls for. Station::None is the plain
  // bag with its 2x2 grid.
  void openStation(world::Station station);
  void closeInventory();
  bool pausePointerOverButton() const { return menu_.pauseHovering(); }

  // Drives tweens, toast ages and the held-item label. Called every frame, before
  // draw(), whatever screen is up.
  void update(double dt, const UiFrame& frame);

  // Handles the pointer and keyboard for whichever screen is up, then draws
  // everything. Split from update() only because drawing needs the finished frame.
  //
  // The Input is not const because the chat box takes and gives up the keyboard
  // from in here: closing it has to end text capture, and doing that anywhere else
  // would mean the frame that closed chat still swallowed its keys.
  void draw(const Window& window, Input& input, const UiFrame& frame);

  float uiScale() const { return scale_; }
  void setUiScale(float s) { scale_ = s > 0.1f ? s : 1.0f; }

  // Hides the hotbar, hearts, crosshair and minimap without touching any screen.
  // F1 in game, --no-hud for a scripted capture: the README's shots are of the
  // world, and a hotbar across the bottom of every one of them is not the world.
  void setHudVisible(bool on) { hudEnabled_ = on; }
  bool hudVisible() const { return hudEnabled_; }

 private:
  UiEvent gatherEvent(const Window& window, const Input& input, double dt);

  Ui2D ui_;
  Text text_;
  Hud hud_;
  Notify notify_;
  Chat chat_;
  Menu menu_;
  SettingsScreen settingsScreen_;
  PacksScreen packsScreen_;
  InventoryUI inventoryUI_;
  Atlas atlas_;
  RecipeBook recipeBook_;
  Gallery gallery_;
  TimeWheel timeWheel_;
  bool pendingPicker_ = false;
  Backdrop backdrop_;
  TweenStore tweens_;
  const render::IconAtlas* icons_ = nullptr;

  Screen screen_ = Screen::None;
  MouseGuard mouseGuard_;
  float scale_ = 1.0f;
  bool hudEnabled_ = true;
  double dt_ = 0;
};

}  // namespace hr::ui
