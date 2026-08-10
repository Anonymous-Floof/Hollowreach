#include "ui/interface.h"

#include "core/log.h"
#include "game/player.h"
#include "platform/window_glfw.h"
#include "render/sky.h"
#include "ui/settings.h"
#include "world/world.h"

namespace hr::ui {

Interface::Interface() = default;
Interface::~Interface() = default;

bool Interface::init(ShaderCache& shaders, const render::IconAtlas* icons) {
  icons_ = icons;
  if (!ui_.init(shaders)) {
    log::error("interface shader failed to build");
    return false;
  }
  if (!text_.init()) return false;
  if (!menu_.init(text_)) return false;
  recipeBook_.attach(icons);
  // Not fatal: without it the glass card falls back to its plain translucent gradient.
  if (!backdrop_.init(shaders)) log::warn("backdrop blur unavailable");
  gallery_.onBack = [this] {
    if (callbacks.closeScreen) callbacks.closeScreen();
  };

  settingsScreen_.onChange = [this](const std::string& key) {
    if (callbacks.settingChanged) callbacks.settingChanged(key);
  };
  settingsScreen_.onBack = [this] {
    if (callbacks.closeScreen) callbacks.closeScreen();
  };
  menu_.actions.quitGame = [this] {
    if (callbacks.quitGame) callbacks.quitGame();
  };
  // Both directions of the same toggle: App owns which screen you came from.
  inventoryUI_.onOpenRecipeBook = [this] {
    if (callbacks.toggleRecipeBook) callbacks.toggleRecipeBook();
  };
  recipeBook_.onBack = [this] {
    if (callbacks.toggleRecipeBook) callbacks.toggleRecipeBook();
  };
  menu_.actions.resume = [this] {
    if (callbacks.resume) callbacks.resume();
  };
  menu_.actions.saveAndQuit = [this] {
    if (callbacks.saveAndQuit) callbacks.saveAndQuit();
  };
  menu_.actions.openSettings = [this] {
    if (callbacks.openSettings) callbacks.openSettings();
  };
  menu_.actions.openGallery = [this] {
    if (callbacks.openGallery) callbacks.openGallery();
  };
  menu_.actions.openPacks = [this] {
    if (callbacks.openPacks) callbacks.openPacks();
  };
  packsScreen_.onBack = [this] {
    if (callbacks.closeScreen) callbacks.closeScreen();
  };
  return true;
}

void Interface::destroy() {
  backdrop_.destroy();
  atlas_.destroy();
  gallery_.destroy();
  menu_.destroy();
  text_.destroy();
  ui_.destroy();
}

void Interface::setScreen(Screen s) {
  if (screen_ == s) return;
  const Screen previous = screen_;
  screen_ = s;
  // The click that opened this screen must not also be spent inside it. Right
  // clicking a chest opens the inventory and, on that very frame, the same right
  // click reaches the slot grid and splits whatever stack the pointer happens to
  // be resting on — which, the pointer having been captured until a moment ago, is
  // wherever it was left the last time a screen was up. So you open a chest and
  // find half a stack stuck to the cursor that you never picked up.
  //
  // Guarding here rather than at the eight call sites that open screens: they all
  // come through setScreen, and it already knows a change from a repeat.
  guardMouse();
  if (s == Screen::Menu) menu_.onShow();
  if (s == Screen::Settings) settingsScreen_.onShow();
  if (s == Screen::Packs) packsScreen_.onShow();
  if (s == Screen::RecipeBook) recipeBook_.open();
  if (s == Screen::Gallery) {
    if (pendingPicker_) gallery_.onShowPicker();
    else gallery_.onShow();
  }
  pendingPicker_ = false;
  if (previous == Screen::RecipeBook) recipeBook_.close();
  if (previous == Screen::Map && s != Screen::Map) atlas_.close();
  // Leaving the inventory returns the crafting grid and the cursor stack to the bag.
  if (previous == Screen::Inventory && s != Screen::Inventory) closeInventory();
}

void Interface::openStation(world::Station station) {
  game::BlockEntity* be = callbacks.currentStation ? callbacks.currentStation() : nullptr;
  switch (station) {
    case world::Station::Workbench:
      inventoryUI_.open(InventoryMode::Workbench, nullptr);
      break;
    case world::Station::Forge: inventoryUI_.open(InventoryMode::Forge, be); break;
    case world::Station::Chest: inventoryUI_.open(InventoryMode::Chest, be); break;
    case world::Station::None: inventoryUI_.open(InventoryMode::Inventory, nullptr); break;
  }
}

void Interface::closeInventory() {
  inventoryUI_.close();
  if (callbacks.closeStation) callbacks.closeStation();
}

bool Interface::wantsCursor() const { return screen_ != Screen::None; }

bool Interface::blocksGameplay() const {
  // The inventory and the Atlas sit over a running world but still swallow input; only
  // the bare HUD lets gameplay keys through.
  return screen_ != Screen::None;
}

void Interface::update(double dt, const UiFrame& frame) {
  dt_ = dt;
  tweens_.beginFrame(dt);
  notify_.update(dt);
  if (frame.inventory) hud_.update(dt, *frame.inventory);
  // The station's block entity is re-resolved every frame: mining a forge while its
  // screen is open must not leave the interface writing through a dangling pointer.
  if (screen_ == Screen::Inventory && callbacks.currentStation) {
    const InventoryMode mode = inventoryUI_.mode();
    if (mode == InventoryMode::Forge || mode == InventoryMode::Chest) {
      inventoryUI_.setStation(callbacks.currentStation());
    }
  }
}

UiEvent Interface::gatherEvent(const Window& window, const Input& input, double dt) {
  UiEvent e;
  // Node rects are in layout pixels; the OS reports the cursor in device pixels.
  e.mouseX = static_cast<float>(input.mouseX()) / scale_;
  e.mouseY = static_cast<float>(input.mouseY()) / scale_;

  // A freshly opened screen ignores the mouse until the guard lifts; MouseGuard
  // says why, and setScreen is what arms it.
  const bool held = input.buttonDown(MouseButton::Left) ||
                    input.buttonDown(MouseButton::Right) ||
                    input.buttonDown(MouseButton::Middle);
  if (mouseGuard_.update(dt, held)) {
    // Position and the keyboard still come through. Hover has to keep working or
    // the screen appears with nothing highlighted, and Escape has to keep closing
    // it; it is only the buttons that are deaf, and only for a moment.
    e.shift = input.shift();
    e.ctrl = input.ctrl();
    e.alt = input.alt();
    e.input = &input;
    e.dt = dt;
    (void)window;
    return e;
  }

  e.leftClick = input.clicked(MouseButton::Left);
  e.rightClick = input.clicked(MouseButton::Right);
  e.leftDown = input.buttonDown(MouseButton::Left);
  e.rightDown = input.buttonDown(MouseButton::Right);
  e.leftRelease = input.releasedButton(MouseButton::Left);
  e.rightRelease = input.releasedButton(MouseButton::Right);
  e.shift = input.shift();
  e.ctrl = input.ctrl();
  e.alt = input.alt();
  e.input = &input;
  e.dt = dt;
  (void)window;
  return e;
}

void Interface::drawFullscreenImage(const Window& window, GLuint texture, float imageAspect) {
  // A pass of its own, before the interface's own frame: the menu backdrop is drawn
  // by App during the world-less branch, and the card blur later captures whatever
  // is already on the default framebuffer.
  ui_.begin(window.width(), window.height(), 1.0f);
  const float w = ui_.width(), h = ui_.height();
  const float viewAspect = h > 0 ? w / h : 1.0f;
  // Cover: crop the long axis rather than squash it, by pulling the UVs in.
  float u = 0.5f, v = 0.5f;
  if (imageAspect > viewAspect) {
    u = 0.5f * viewAspect / imageAspect;  // image is wider: trim the sides
  } else if (imageAspect > 0.0f) {
    v = 0.5f * imageAspect / viewAspect;  // image is taller: trim top and bottom
  }
  ui_.setTexture(texture);
  ui_.texturedRect({0, 0, w, h}, 0.5f - u, 0.5f - v, 0.5f + u, 0.5f + v, color::white);
  ui_.setTexture(0);
  ui_.end();
}

void Interface::draw(const Window& window, const Input& input, const UiFrame& frame) {
  // backdrop-filter reads the frame *behind* the card, so the capture has to happen
  // before any interface pixel lands on it.
  if (screen_ == Screen::Menu) {
    backdrop_.capture(window.width(), window.height());
    menu_.setBackdrop(backdrop_.ready() ? backdrop_.texture() : 0);
  }

  ui_.begin(window.width(), window.height(), scale_);
  text_.beginFrame(scale_);

  UiEvent event = gatherEvent(window, input, dt_);
  // The wheel is consumed by whichever screen is up; the hotbar only sees it while
  // nothing is open, which App enforces by checking blocksGameplay().
  if (screen_ != Screen::None && !mouseGuard_.guarding()) {
    event.wheel = static_cast<float>(input.wheelPeek());
  }

  // The same remote bodies the nameplates are drawn from, handed to the Atlas so
  // the map and the minimap can show where everyone is. Refreshed before either
  // of them is drawn, since the minimap goes out with the HUD and the full map
  // with the screen switch below.
  {
    std::vector<Atlas::Companion> who;
    who.reserve(frame.nameplates.size());
    for (const UiFrame::Nameplate& p : frame.nameplates) {
      who.push_back(Atlas::Companion{p.pos.x, p.pos.z, p.yaw, p.name});
    }
    atlas_.setCompanions(std::move(who));
  }

  // The HUD shows over the world and over the screens that overlay it — exactly the set
  // the web build passed to hud.show() (js/main.js:212).
  const bool hudVisible = hudEnabled_ && frame.player && frame.inventory &&
                          (screen_ == Screen::None || screen_ == Screen::Pause ||
                           screen_ == Screen::Inventory);
  if (hudVisible) {
    HudFrame hf;
    hf.player = frame.player;
    hf.inventory = frame.inventory;
    hf.world = frame.world;
    hf.sky = frame.sky;
    hf.icons = icons_;
    hf.fps = frame.fps;
    hf.hasTarget = frame.hasTarget;
    hf.targetX = frame.targetX;
    hf.targetY = frame.targetY;
    hf.targetZ = frame.targetZ;
    hf.breakFraction = frame.breakFraction;
    hf.hurtFlash = frame.hurtFlash;
    hf.netLine = frame.netLine;
    hf.camera = frame.camera;
    hf.nameplates.reserve(frame.nameplates.size());
    for (const UiFrame::Nameplate& p : frame.nameplates) {
      hf.nameplates.push_back(HudFrame::Nameplate{p.pos, p.name, p.health});
    }
    hud_.draw(ui_, text_, hf);
    // The Atlas item gates the whole feature, and the minimap has its own setting.
    if (frame.world && frame.camera && screen_ == Screen::None &&
        Atlas::hasAtlasItem(*frame.inventory)) {
      atlas_.attach(frame.atlas);
      atlas_.drawHud(ui_, text_, *frame.world, *frame.player, *frame.camera,
                     settings().flag("minimap"), dt_);
    }
  }

  switch (screen_) {
    case Screen::Menu:
      menu_.updateMain(ui_, text_, event, tweens_, frame.version);
      menu_.drawMain(ui_, text_);
      break;
    case Screen::Pause:
      menu_.updatePause(ui_, text_, event, tweens_, frame.world != nullptr);
      menu_.drawPause(ui_, text_);
      break;
    case Screen::Settings:
      settingsScreen_.update(ui_, text_, event, tweens_);
      settingsScreen_.draw(ui_, text_);
      break;
    case Screen::Inventory:
      inventoryUI_.update(ui_, text_, event, tweens_);
      inventoryUI_.draw(ui_, text_);
      break;
    case Screen::RecipeBook:
      recipeBook_.update(ui_, text_, event, tweens_);
      recipeBook_.draw(ui_, text_);
      break;
    case Screen::Map:
      if (frame.world && frame.player) {
        atlas_.update(ui_, text_, event, *frame.world, *frame.player);
        atlas_.draw(ui_, text_, *frame.world, *frame.player);
      }
      break;
    case Screen::Gallery:
      gallery_.update(ui_, text_, event, tweens_);
      gallery_.draw(ui_, text_);
      break;
    case Screen::Packs:
      packsScreen_.update(ui_, text_, event, tweens_);
      packsScreen_.draw(ui_, text_);
      break;
    case Screen::TimeWheel:
      // Needs the sky and nothing else — it is a clock face over a paused world.
      if (frame.sky) {
        timeWheel_.update(ui_, text_, event, *frame.sky);
        // update() can confirm, which closes this screen from under us; drawing a
        // dial over a world that has already started fast-forwarding would be a
        // frame of the wrong thing.
        if (screen_ == Screen::TimeWheel) timeWheel_.draw(ui_, text_, *frame.sky);
      }
      break;
    case Screen::Boot:
    case Screen::None:
      break;
  }

  notify_.draw(ui_, text_);
  ui_.end();
}

}  // namespace hr::ui
