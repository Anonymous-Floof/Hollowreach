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
#include <unordered_map>
#include <utility>
#include <vector>

#include "cmd/access.h"
#include "cmd/command.h"
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
  // Which resource packs are on. Reachable from the main menu and from Settings,
  // so like Settings it remembers where it came from.
  Packs,
  // The Gallery again, opened from a painting to choose what hangs in it. A state
  // of its own so Escape returns you to the world rather than to the main menu.
  PaintingPick,
  // A bed's 24-hour dial: read the clock, or pick an hour and sleep to it.
  TimeWheel,
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
  // --debug-lines: turns the world-space overlays on without going through the
  // settings screen, which a headless capture cannot click. Same reason --screen
  // exists.
  bool debugLines = false;
  // --creative: makes the generated world a creative one and turns the mode on,
  // so the creative screens can be captured without clicking through a menu.
  bool creativeWorld = false;
  // --no-hud: the same thing F1 does, for a capture of the world alone.
  bool hideHud = false;
  // --no-vsync: uncap the frame rate. With vsync on, frame time quantises to
  // refresh divisors (60, 30, 20, 15 on a 60 Hz panel), which hides what a change
  // actually cost -- a pass that got 40% cheaper can leave the number at 20 and
  // look like it did nothing.
  bool noVsync = false;
  // --perf: bracket every render pass with a GL_TIME_ELAPSED query and every
  // main-thread phase with a timer, publish a rolling mean, and print a breakdown
  // at exit. Implies --no-vsync. This exists because every performance number
  // this project produced before it was a single wall-clock figure with no
  // attribution, and two separate diagnoses drawn from one turned out to be
  // wrong. Off by default: the queries cost two GL calls per pass, and a
  // profiler nobody asked for should not be in a player's frame.
  bool perf = false;
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
  // --hang <png>: builds a wall in front of spawn and hangs this picture on it.
  // The only way to capture a painting reproducibly — everything else about one
  // goes through a mouse click on a screen that needs a gallery behind it.
  std::string hangPicture;
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

  // --command <line>: run one command once the world is up, repeatable. This is
  // what makes the command layer testable in the real binary at all — every other
  // route to one goes through a chat box that a headless capture cannot type into.
  std::vector<std::string> startupCommands;

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
  // The half of a frame that reads the mouse and keyboard: looking, the hotbar,
  // mining and placing. Only runs while the world is actually in front of you.
  void updatePlaying(double dt);
  // The half that does not: entities, water, block updates, spawning, streaming,
  // the sky, the autosave and the respawn poll. Runs behind an open screen too
  // whenever anyone else is in the world — see its definition.
  void stepWorld(double dt);
  // The body. Split out because it is simulated in both halves of that split, with
  // real input in one and none in the other. Takes its step size from the clock.
  void stepPlayer(Input& input);
  void syncSettingsIfChanged();
  void renderWorld();
  void renderMenuScene(double dt);
  void limitFrameRate();
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
  // Hangs a picture, and tells everyone else if we are the host. One place, so the
  // local path, the guest's relayed request and a host's own click all land here.
  void applyPainting(int x, int y, int z, game::Painting art);
  // The bound Soul Anchor, or the world origin column when nothing is bound.
  Vec3 spawnPoint() const;

  // Hotbar selection: number keys, the mouse wheel, and Q to drop.
  void handleHotbarInput(Input& input, double dt);
  // Q held in the world: the same drop run the inventory screen does over a
  // hovered slot, kept here because the hotbar has no UI object to own it.
  ui::DropRun dropRun_;
  // Last settings revision applied; see updatePlaying.
  std::uint32_t settingsRevision_ = 0;
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
  // Rescans data/resourcepacks, rebuilds the sound bank from whichever are
  // enabled, and puts the result on the Resource Packs screen. Called at startup
  // and whenever the selection changes; safe to call with none installed, which
  // leaves every sound synthesised as it always was.
  void applyResourcePacks();
  // Assembles what the interface draws from this frame.
  ui::UiFrame uiFrame();
  // Fills entityContext_ from the current subsystems. Called every frame because
  // the world and player are recreated whenever a world is entered or left.
  void refreshEntityContext();
  // Drives the interface's own state machine off AppState.
  void syncScreen();
  // Re-enters the world: pointer captured, and the click that did it swallowed.
  void resumePlaying();
  // Whether cartography is unlocked: the Atlas item is being carried. Asked on
  // every frame the map is open rather than latched when it opens — see frame().
  bool hasAtlas() const;
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
  void syncMenuBackdrop();
  // The graphics preset as of the last applySettings, so a CHANGE can be told
  // from a re-apply. Seeded from the store before the first apply; see the
  // comment at its use for why that matters.
  std::string lastQuality_;
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

  // The player-set spawn point, from a bound Soul Anchor. Absent means "fall back
  // to the world's own home", which is why it is a flag and not a sentinel value.
  bool hasSpawn_ = false;
  Vec3 spawn_;

  // What the open world was CREATED as, and what the next one to be created should
  // be. Deliberately not a setting: `creativeMode` is a world rule the host can
  // flip, and this is the thing that decides whether they are allowed to. A world
  // made Survival can never become Creative, which is what makes the choice on the
  // New World screen a choice rather than a starting position.
  bool createdCreative_ = false;
  bool pendingCreative_ = false;
  // The world's home: what findSpawn chose on entry, and where an unbound player
  // respawns. Derived from the seed rather than saved, so it is the same place for
  // a world made today and one loaded from before this field existed.
  Vec3 worldSpawn_;

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
  // Who is trusted, banned, or on the list. The installation's, not the world's —
  // see cmd/access.h for why that is the right scope.
  cmd::Access access_;
  // What the host says everyone's level is, while we are a guest. Held here rather
  // than in the Client because it is the interface that reads it, and because a
  // level is a fact about the session rather than about the connection.
  std::unordered_map<std::string, cmd::Level> netLevels_;
  // The reply channel for the command currently running, or empty for the local
  // player. Set for the length of one dispatch, so a command's ctx.reply() reaches
  // the guest that asked without every hook having to carry an address.
  std::string replyTo_;
  std::string replyName_;
  // /stop, deferred to the end of the frame. Closing the world from inside a
  // running command would free the world and player the Context still points at —
  // and the command layer is reached from inside the host's own message loop, so
  // it would be freeing them from under that too.
  bool pendingStop_ = false;
  net::SessionHooks makeSessionHooks();
  net::GameRefs gameRefs();

  // --- chat and commands ------------------------------------------------------
  //
  // Every command in the game runs HERE, on whichever machine is authoritative. In
  // single player that is this one; hosting, it is this one for the host's own
  // lines and for every guest's; joining, it is the host's and this build only
  // sends the text. There is deliberately no second path — a guest that executed
  // its own commands would be deciding the world's rules for everybody.
  cmd::Hooks makeCommandHooks();
  // Runs one line on behalf of `playerId`, and routes every reply back to whoever
  // asked — the local chat box for our own, a ChatLine message for a guest's.
  // `console` marks a line that came from somewhere with no body behind it —
  // --command today, the dedicated server's own input later. It is what makes `~`
  // refuse rather than silently mean the world origin.
  void runCommand(const std::string& playerId, const std::string& line, bool console = false);
  // A line the local player typed. Sends it to the host when we are a guest, and
  // runs it here otherwise.
  void submitChat(const std::string& line);
  // Puts a line in the local chat box. One place, so a toast and a chat line never
  // disagree about wording.
  void showChat(ui::Chat::Kind kind, const std::string& from, const std::string& text);
  // Sends one line to one player, wherever they are.
  void chatTo(const std::string& playerId, ui::Chat::Kind kind, const std::string& from,
              const std::string& text);
  void chatAll(ui::Chat::Kind kind, const std::string& from, const std::string& text);
  // Everybody in the session as the command layer sees them, with `self` set on
  // whoever is asking.
  std::vector<cmd::Participant> participants(const std::string& callerId) const;
  // What the local player may do. Owner in single player and while hosting; while
  // joined, whatever the host last told us.
  cmd::Level localLevel() const;
  cmd::Level levelFor(const std::string& playerId, const std::string& name) const;
  // T and slash. Refuses when a screen is up, since chat is not a screen and
  // stacking it over one has nowhere sensible to draw.
  void openChat(bool withSlash);
  // Fills the chat box's completion sources from the live roster, every frame it
  // is open. Cheap: a handful of names.
  void refreshChatSources();
  // Runs --command, once, as soon as the session can actually answer.
  //
  // NOT at startup. A guest has to be joined before its line means anything —
  // sending it earlier would have it run locally at this build's own level, which
  // is the one thing a guest never gets to do — and even a host has a world that
  // is still streaming. They go through submitChat, so a guest's line takes
  // exactly the road a typed one takes.
  void runStartupCommands();
  bool startupCommandsDone_ = false;
  // Whether the chat box was up last frame, so closing it can hand the pointer
  // back. Held here rather than asked of the interface twice, because the answer
  // has to be the one from BEFORE this frame's input was handled.
  bool chatWasOpen_ = false;
  bool startHosting(std::uint16_t port);
  bool startJoining(const std::string& address, std::uint16_t port);
  // `sayGoodbye` false only when the connection is already gone — the host dropped
  // us, or it never came up. Every other route out is a decision we took, and the
  // host is owed a Bye: without one it keeps a ghost of us in the roster until
  // ENet's own handshake times out, which is seconds of everyone watching a body
  // that has already quit the game. Host::onMessage's Bye handler exists for
  // exactly this and was never being reached.
  void leaveNetwork(const std::string& reason, bool sayGoodbye = true);
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
  // Which station the recipe book was opened over, so Back rebuilds that screen
  // rather than a bare inventory.
  world::Station recipeStation_ = world::Station::None;
  // 1 on a hit, fading to 0. Drives the red edge vignette.
  float hurtFlash_ = 0.0f;
  // Frames per second cap, 0 for uncapped. See App::limitFrameRate.
  int frameLimit_ = 0;
  double frameStart_ = 0.0;
  AppState galleryReturn_ = AppState::Menu;
  AppState packsReturn_ = AppState::Menu;
  // Which painting the picker was opened for. Remembered rather than passed
  // through the interface, because the Gallery does not know paintings exist.
  int paintingX_ = 0, paintingY_ = 0, paintingZ_ = 0;
  // A detached forge/chest for --screen forge|chest, so a station screen can be
  // captured without placing a block first.
  game::BlockEntity scratchStation_;

  // 0..1 strength of the submerged post, eased toward the target each frame.
  float underwater_ = 0.0f;
  // 0..1 strength of the see-inside-terrain lift, eased the same way.
  float xray_ = 0.0f;

  // An Input nothing ever feeds. It is what the body and the entities are given
  // while a screen is open in a shared world: the simulation carries on, and reads
  // every key as up and every button as unclicked.
  Input idleInput_;

  // The host's id for the boat this guest is riding, or 0. Held separately from
  // `player_->mount()`, which is the LOCAL entity id: the two differ (a ghost's is
  // negative), and the host only answers to its own. Kept after a dismount just
  // long enough to say so, since by then the local mount is already zero.
  int mountedNetId_ = 0;

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
