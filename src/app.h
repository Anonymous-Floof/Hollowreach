// Top-level application: owns every subsystem and runs the frame loop.
//
// This is the native counterpart of the `Game` class in js/main.js — the same single
// object passed by reference to everything, and the same top-level state machine.
// The web build wired cross-layer hooks by assigning function properties
// (world.onNetEdit, game._mpRefresh); here those become explicit members, but the
// shape is deliberately the same so the two stay comparable during the port.

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/camera.h"
#include "core/input.h"
#include "core/shader.h"
#include "core/time.h"
#include "game/entities/manager.h"
#include "game/interact.h"
#include "game/inventory.h"
#include "game/player.h"
#include "platform/window_glfw.h"
#include "render/chunkmesh.h"
#include "render/iconatlas.h"
#include "render/renderer.h"
#include "render/screenquad.h"
#include "render/sky.h"
#include "net/client.h"
#include "net/discovery.h"
#include "net/host.h"
#include "resource/atlas.h"
#include "save/format.h"
#include "ui/interface.h"
#include "world/mesher.h"
#include "world/world.h"

namespace hr {

// js/main.js kept this as a string; an enum makes the transitions checkable.
enum class AppState {
  Boot,
  Menu,
  Playing,
  Paused,
  Settings,
  Inventory,
  RecipeBook,
  Map,
  Gallery,
};

struct AppOptions {
  std::string dataDir;      // --data-dir
  bool listDevices = false; // --gl-info: print adapter details and exit
  int width = 1280;
  int height = 720;
  bool fullscreen = false;

  // Verification harness. The web build could be poked from the browser console;
  // these give the native build reproducible captures to diff against it, and they
  // are how every later milestone is checked.
  std::string screenshotPath;  // --screenshot <png>: capture, then exit
  int exitAfterFrames = 0;     // --frames <n>: render n frames, then exit

  // Whether to drop straight into a world instead of opening the menu. Off by default
  // now that the menu exists; main.cpp turns it on for any flag that implies a world.
  bool startWorld = false;
  std::uint32_t seed = 3918175327u;
  // Zero and empty mean "not given": both of these override the stored setting AND
  // are written back to it, so a real default here would silently reset the player's
  // choice on every launch — which is exactly what it did until it was noticed.
  int renderDistance = 0;
  // --at x,y,z,yaw,pitch: an exact camera placement, so a screenshot is
  // reproducible and can be lined up against the browser build.
  bool haveSpawnOverride = false;
  float spawnX = 8.5f, spawnY = 0.0f, spawnZ = 8.5f;
  float spawnYaw = 0.0f, spawnPitch = 0.0f;

  // --time <0..1>: pins the sky clock, so an A/B capture is not at the mercy of
  // whatever moment the frame landed on. Negative means "run normally".
  float skyTime = -1.0f;
  // --quality low|medium|high|ultra. Empty means "leave the stored setting alone".
  std::string quality;
  // --threads <n>: worker threads for chunk generation, lighting and meshing.
  // Negative means "pick from hardware_concurrency"; 0 runs every job inline on
  // the main thread, which is both the escape hatch and the reference the
  // determinism check compares the threaded build against.
  int threads = -1;
  // --debug-view 1 = AO term, 2 = sun shadow term
  int debugView = 0;
  // --no-hud: the same thing F1 does, for a capture of the world alone.
  bool hideHud = false;
  // --no-vsync: uncap the frame rate. With vsync on, frame time quantises to
  // refresh divisors (60, 30, 20, 15 on a 60 Hz panel), which hides what a change
  // actually cost -- a pass that got 40% cheaper can leave the number at 20 and
  // look like it did nothing.
  bool noVsync = false;
  // --freeze: no player physics, so --at is honoured exactly rather than being a
  // starting position to fall from. The world still streams and the sky still
  // turns; only the body is held still.
  bool freezePlayer = false;

  // --give <key>[:count][,...]: fill hotbar slots at startup. The interface
  // milestone brings the inventory screen and the recipe book; until then this is
  // how a crafting chain or a tool tier gets exercised.
  std::vector<std::pair<std::string, int>> startingItems;
  // --spawn <type>[:count][,...]: place entities in a ring around the spawn point.
  // Mob spawning is timed, capped and gated on the time of day, so a capture that
  // has to contain a sheep needs a way to ask for one.
  std::vector<std::pair<std::string, int>> startingEntities;
  // --dump-icons <png>: write the generated icon sheet and exit.
  std::string iconDumpPath;

  // --world <id>: open a saved world instead of generating a fresh one. Every other
  // world flag still applies on top, so a save can be inspected from an exact
  // camera at an exact time of day like any other capture.
  std::string worldId;
  // --new-world <name>: create a persistent world and enter it, which is what the
  // menu's Create button does. Without it the save path is only reachable by hand
  // through the menus, and "create, play, quit, reload" is not a scriptable check.
  std::string newWorldName;

  // --host [port]: open the world being played to guests. --join <addr[:port]>:
  // connect to one. Two processes and two flags is what makes multiplayer
  // testable at all without two people and two machines.
  bool hostGame = false;
  std::uint16_t hostPort = net::kDefaultGamePort;
  std::string joinAddress;
  std::uint16_t joinPort = net::kDefaultGamePort;
  // --name <text>: the display name on the nameplate and in the roster.
  std::string playerName;

  // --screen <name>: open a screen straight away, so `--screenshot` can capture any
  // one of them without a human driving the menus. This is what makes the interface
  // A/B-comparable against the browser at all — the alternative is clicking through
  // to the same state by hand on both sides and hoping the cursor landed identically.
  std::string startScreen;
  // --mouse x,y: park the cursor, so hover states are part of a capture too.
  bool haveMouseOverride = false;
  float mouseX = 0, mouseY = 0;
};

class App {
 public:
  App();
  ~App();

  // Returns a process exit code. Reports startup failures through the log and,
  // where possible, an on-screen message — the equivalent of the web build's
  // "WebGL2 not available" card.
  int run(const AppOptions& options);

 private:
  void frame();
  void updatePlaying(double dt);
  void renderWorld();
  void renderMenuScene(double dt);
  void handleGlobalKeys();
  // Alt+Enter and the settings row are the same switch: both go through the store.
  void toggleFullscreen();
  // H, and the button on the inventory and station screens.
  void toggleRecipeBook();

  bool loadShaders();
  // Enters a world. With `loaded` null this generates a fresh one from the options;
  // with a save it restores that instead — the two paths share everything from the
  // chunk streamer down, and the differences are exactly the fields a save carries.
  bool startWorld(const AppOptions& options, const save::WorldSave* loaded = nullptr);
  static render::QualitySettings qualityPreset(const std::string& name);

  // --- saves ---
  // Gathers the live world into a save. Reads only: an autosave happens mid-play,
  // so nothing here may perturb what it walks.
  save::WorldSave buildSave();
  // Writes it. Reports failure through the notification queue as well as the log,
  // because a save that silently did not happen is the worst possible outcome.
  bool saveCurrentWorld();
  bool loadWorldFromDisk(const std::string& id);
  // Tears the world down and returns to the menu. Shared by Save & Quit, by loading
  // another world, and by deleting the one that is open.
  void leaveWorld();

  // Death, from js/main.js:803. Scatters everything carried where the player fell,
  // pins a death waypoint and wakes them at their bound spawn. Polled at the end of
  // updatePlaying rather than driven from Player::onDeath, because the killing blow
  // can land inside entities_.tick and scattering an inventory from there would
  // spawn drops into the vector being iterated.
  void respawnPlayer();
  // The bound Soul Anchor, or the world origin column when nothing is bound.
  Vec3 spawnPoint() const;

  // Hotbar selection: number keys, the mouse wheel, and Q to drop.
  void handleHotbarInput(Input& input);
  // Throws one stack into the world, routing through the host for a guest.
  void tossStack(const std::string& key, int count, int dura);
  // The hooks Interact calls back into. Built once, since they capture `this`.
  game::InteractHooks makeInteractHooks();
  // The hooks the interface calls back into, likewise.
  void wireInterface();
  // Pushes every setting into the subsystem that owns it. Called at startup and from
  // the settings screen's live change callback, which is what makes all of them apply
  // without a restart.
  void applySettings();
  // Assembles what the interface draws from this frame.
  ui::UiFrame uiFrame();
  // Fills entityContext_ from the current subsystems. Called every frame because
  // the world and player are recreated whenever a world is entered or left.
  void refreshEntityContext();
  // Drives the interface's own state machine off AppState.
  void syncScreen();
  // Re-enters the world: pointer captured, and the click that did it swallowed.
  void resumePlaying();
  // The Escape / Back path. Each screen returns somewhere different.
  void closeCurrentScreen();
  std::string nextScreenshotName() const;
  // --screen <name> -> the state and, for the station screens, the inventory mode.
  bool applyStartScreen(const std::string& name);

  // Reads the default framebuffer back and writes a PNG. GL hands back rows
  // bottom-up, so the capture is flipped on the way out.
  bool captureScreenshot(const std::string& path);

  Window window_;
  ShaderCache shaders_;
  FrameClock clock_;
  render::ScreenQuad screenQuad_;

  Program* menuBackdrop_ = nullptr;
  // The menu's chosen background picture, uploaded once per choice. Empty path and a
  // zero texture mean the stylesheet's radial-gradient fallback.
  void refreshMenuBackground();
  std::string menuBgPath_;
  GLuint menuBgTex_ = 0;
  float menuBgAspect_ = 1.0f;

  resource::Atlas atlas_;
  world::BlockTileTable tiles_;
  render::IconAtlas icons_;
  render::Renderer renderer_;
  render::Sky sky_;

  Camera camera_;
  std::unique_ptr<world::World> world_;
  std::unique_ptr<game::Player> player_;
  game::PlayerOptions playerOptions_;
  game::Inventory inventory_;
  game::Interact interact_;
  game::InteractHooks interactHooks_;
  // Live entities, and the context their hooks reach back through. The context
  // is a member rather than a local because InteractHooks holds a pointer to it.
  game::EntityManager entities_;
  game::EntityContext entityContext_;
  // The mob spawner runs on a four-second cadence, as in the web build.
  float grazerSpawnT_ = 0.0f;
  float zombieSpawnT_ = 0.0f;
  ui::Interface interface_;

  // Which world on disk is open, if any. A world started from the command line has
  // no id, and an idless world is never written — so a screenshot run cannot leave
  // a save behind, and `--seed` stays the throwaway harness it has been since M3.
  save::WorldMeta worldMeta_;
  bool worldOnDisk_ = false;
  float autosaveT_ = 0.0f;

  // The player-set spawn point, from a bound Soul Anchor. Absent means "derive the
  // default from the terrain", which is why it is a flag and not a sentinel value.
  bool hasSpawn_ = false;
  Vec3 spawn_;

  // --- multiplayer ---
  // Exactly one of these is live at a time, or neither. A guest's world is never
  // saved and never simulated beyond its own player, which is why `netGuest()` is
  // checked in as many places as it is.
  net::Host netHost_;
  net::Client netClient_;
  net::Advertiser advertiser_;
  net::Listener lanListener_;
  std::string playerId_;
  std::string playerName_;
  bool netGuest() const { return netClient_.running(); }
  bool netHosting() const { return netHost_.running(); }
  bool multiplayer() const { return netGuest() || netHosting(); }
  // Guest progress carried from the last loaded save, so opening a world to the
  // LAN restores what returning friends had. Kept on App rather than in the Host
  // because it outlives any one hosting session.
  std::vector<save::GuestSave> lastLoadedGuests_;
  net::SessionHooks makeSessionHooks();
  net::GameRefs gameRefs();
  bool startHosting(std::uint16_t port);
  bool startJoining(const std::string& address, std::uint16_t port);
  void leaveNetwork(const std::string& reason);
  // A guest's world arrives over the wire rather than off disk.
  void adoptRemoteWorld(const save::WorldSave& data);

  // The block entity a station screen is showing, so it can be reopened after a
  // rebuild and closed if the block is broken while the screen is up.
  bool stationOpen_ = false;
  int stationX_ = 0, stationY_ = 0, stationZ_ = 0;
  // Where Back from the settings and recipe screens should return to, matching the JS
  // settingsReturn / _rbReturn.
  AppState settingsReturn_ = AppState::Menu;
  AppState recipeReturn_ = AppState::Playing;
  AppState galleryReturn_ = AppState::Menu;
  // A detached forge/chest for --screen forge|chest, so a station screen can be
  // captured without placing a block first.
  game::BlockEntity scratchStation_;

  // 0..1 strength of the submerged post, eased toward the target each frame.
  float underwater_ = 0.0f;

  // Frames of interaction to swallow after the pointer is re-captured.
  int resumeClickGuard_ = 0;

  AppState state_ = AppState::Boot;
  bool running_ = false;
  long long frameIndex_ = 0;
  // How much of each frame chunk streaming costs this thread, reported at exit.
  // See the note where it is accumulated for why this and not frame time.
  double streamTotal_ = 0.0;
  double streamWorst_ = 0.0;
  long long streamFrames_ = 0;
  long long streamWorstFrame_ = 0;
  AppOptions options_;

  // Shader hot-reload polls the source directory rather than watching it: a few
  // stat calls twice a second cost nothing and need no platform-specific filesystem
  // notification code.
  Interval shaderWatch_ {0.5};
};

}  // namespace hr
