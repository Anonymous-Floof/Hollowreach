// Main menu, world select, world creation, About, and the pause menu.
// Ported from js/ui/menu.js and the .menu-glass / .about / .world-list rules.
//
// The signature piece is the title: 54px Segoe UI Black, letter-spaced 10px, filled
// with a three-stop vertical gradient through `background-clip: text`, and carrying
// two stacked drop-shadows. The plan's call was to render it once into a texture
// rather than teach the shader about multi-stop gradients and Gaussian blur, and that
// is what happens here — it is a fixed string, so it costs one image at startup.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/gl.h"
#include "ui/dom.h"
#include "ui/settings.h"
#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"

namespace hr::ui {

// Which page of the menu card is showing. The JS swapped innerHTML on #menu-root; the
// page is the state that survives a rebuild.
enum class MenuPage { Main, Worlds, NewWorld, About, Join };

// One row of the world list, filled from the save layer's listing.
struct WorldEntry {
  std::string id;
  std::string name;
  std::uint32_t seed = 0;
  // Seconds since the save, for the "saved 3m ago" line.
  double ageSeconds = 0;
};

// One game heard on the local network. Filled from net::Beacon; the interface
// deliberately does not include the net layer for it.
struct LanGame {
  std::string address;
  std::uint16_t port = 0;
  std::string worldName;
  std::string hostName;
  int players = 0;
  int maxPlayers = 0;
};

// What the menu asks the app to do.
struct MenuActions {
  std::function<void()> playSingle;   // Play with no worlds yet: straight to New World
  std::function<void(const std::string& name, std::uint32_t seed)> createWorld;
  std::function<void(const std::string& id)> loadWorld;
  std::function<void(const std::string& id)> deleteWorld;
  // Writes a shareable copy to data/exports. The row's button has existed since the
  // screens landed; the save layer is what it was waiting for.
  std::function<void(const std::string& id)> exportWorld;
  // Takes in every world file sitting in data/exports. The browser's counterpart
  // was a file picker, which a native build has no way to open without pulling in a
  // dialog library, so the same folder serves as both outbox and inbox.
  std::function<void()> importWorlds;
  std::function<void()> openSettings;
  std::function<void()> openGallery;
  std::function<void()> resume;
  std::function<void()> saveAndQuit;
  std::function<void()> quitGame;
  std::function<std::vector<WorldEntry>()> listWorlds;

  // --- multiplayer ---
  // Games heard on the LAN, refreshed while the Join page is open.
  std::function<std::vector<LanGame>()> lanGames;
  // An address, an address:port, or a pasted HRW1 invite code — the panel does
  // not try to tell them apart, because the app already has the parser.
  std::function<void(const std::string& target)> joinGame;
  // Opens the world being played to guests, or closes it again.
  std::function<void()> toggleHosting;
  // A line for the pause menu: who is connected and the invite code, or empty
  // when this is a single-player world.
  std::function<std::string()> hostStatus;
  // Puts the invite code on the clipboard. The alternative was telling people to
  // find it in a log file.
  std::function<void()> copyInvite;
  // Whether the pause menu's Multiplayer button should read "Open to LAN" or
  // "Stop Hosting", and whether a guest may press it at all.
  std::function<bool()> isHosting;
  std::function<bool()> isGuest;
};

class Menu {
 public:
  bool init(Text& text);
  void destroy();

  MenuActions actions;

  // The blurred copy of the frame behind the card, for backdrop-filter. 0 disables it.
  void setBackdrop(GLuint texture) { backdrop_ = texture; }
  // Whether a live panorama is behind the card. The stylesheet has two different
  // backgrounds for this: the vignette wash over a panorama, and an entirely separate
  // calm gradient without one (`#menu:not(.has-pano)`).
  // Whether something is already drawn behind the menu — today a picture chosen
  // from the Gallery, a panorama if that ever lands. It selects between washing
  // over that image and painting an opaque gradient of our own, so leaving it
  // false while a background is set covers the picture completely.
  void setHasBackdrop(bool on) { hasBackdrop_ = on; }
  void setPage(MenuPage page);
  MenuPage page() const { return page_; }
  // Called when the menu screen opens, so the world list is re-read and the pointer
  // does not arrive mid-hover on a stale button.
  void onShow();

  void updateMain(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens,
                  const std::string& version);
  void drawMain(Ui2D& ui, Text& text);

  void updatePause(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens,
                   bool hasWorld);
  void drawPause(Ui2D& ui, Text& text);

  // True when a text field on the New World page has focus.
  bool editing() const {
    return nameField_.focused() || seedField_.focused() || joinField_.focused();
  }
  // Escape while typing should leave the field, not the screen.
  void blurFields(Input* input);
  // Whether the pointer is over a pause-menu button, so a click on empty space can
  // resume the way clicking the canvas did in the browser.
  bool pauseHovering() const { return pauseHoveredTag_ != 0; }

 private:
  void buildMainCard(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens,
                     const std::string& version);
  void buildPageMain(const UiEvent& event, TweenStore& tweens);
  void buildPageWorlds(const UiEvent& event, TweenStore& tweens);
  void buildPageNewWorld(const UiEvent& event, TweenStore& tweens);
  void buildPageAbout(const UiEvent& event, TweenStore& tweens);
  void buildPageJoin(const UiEvent& event, TweenStore& tweens);
  void handleMain(const UiEvent& event, Text& text);
  void handlePause(const UiEvent& event);

  // The pre-rendered title, and where its ink sits relative to the brand block.
  bool buildTitleTexture(Text& text);
  GLuint titleTexture_ = 0;
  GLuint backdrop_ = 0;
  bool hasBackdrop_ = false;
  int titleWidth_ = 0, titleHeight_ = 0;

  Doc main_;
  Doc pause_;
  MenuPage page_ = MenuPage::Main;
  int hoveredTag_ = 0;
  int hoveredIndex_ = 0;
  int pauseHoveredTag_ = 0;

  std::vector<WorldEntry> worlds_;
  bool worldsLoaded_ = false;

  TextField nameField_;
  TextField seedField_;
  TextField joinField_;
  std::vector<LanGame> lanGames_;
  double lanRefresh_ = 0;
  double time_ = 0;
  bool pauseHasWorld_ = true;
};

}  // namespace hr::ui
