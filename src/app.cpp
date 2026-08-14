#include "app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include "audio/director.h"
#include "audio/engine.h"
#include "audio/sfx.h"
#include "audio/soundbank.h"
#include "ui/uipacks.h"
#include "ui/uisprites.h"
#include "core/assets.h"
#include "core/jobs.h"
#include "core/jsmath.h"
#include "core/log.h"
#include "core/prng.h"
#include "game/entities/types.h"
#include "game/farming.h"
#include "game/loot.h"
#include "platform/paths.h"
#include "resource/image.h"
#include "resource/pack.h"
#include "resource/packstack.h"
#include "save/storage.h"
#include "save/transfer.h"
#include "world/chunk.h"
#include "world/worldgen.h"

namespace fs = std::filesystem;

namespace hr {
namespace {
// js/main.js:733 — fifteen minutes.
constexpr float kAutosaveSeconds = 900.0f;

// js/main.js:38 SPAWN_XZ. The world origin column: where a new world drops you in,
// and where you wake with no Soul Anchor bound.
constexpr float kSpawnX = 8.5f;
constexpr float kSpawnZ = 8.5f;

double nowMillis() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}
}  // namespace

App::App() = default;
App::~App() = default;

int App::run(const AppOptions& options) {
  options_ = options;
  paths::init(options.dataDir);
  paths::ensureDirs();
  log::openFile(paths::logFile().c_str());
  log::info("Hollowreach %s (native) starting", HR_VERSION);
  log::info("data directory: %s", paths::dataDir().c_str());

  // Asset override, disk first. An assets/ folder beside the executable wins, so a
  // player can drop one in; otherwise a local build falls back to the source tree,
  // which makes shader hot-reload work in every configuration and not just Debug.
  // HR_SOURCE_ASSET_DIR is an absolute path baked in at compile time, so on a
  // machine that only has the shipped zip it simply does not exist.
  {
    std::error_code ec;
    fs::path beside = fs::path(paths::exeDir()) / "assets";
    if (fs::is_directory(beside, ec)) {
      assets::setOverrideDir(beside.string());
    }
#if defined(HR_SOURCE_ASSET_DIR)
    else if (fs::is_directory(HR_SOURCE_ASSET_DIR, ec)) {
      assets::setOverrideDir(HR_SOURCE_ASSET_DIR);
    }
#endif
  }

  WindowConfig cfg;
  cfg.width = options.width;
  cfg.height = options.height;
  cfg.title = "Hollowreach";
  cfg.vsync = !options.noVsync;
  std::string error;
  if (!window_.create(cfg, error)) {
    log::error("%s", error.c_str());
    return 1;
  }

  if (options.listDevices) {
    // Adapter details were already logged during context creation.
    return 0;
  }
  screenQuad_.create();
  if (!loadShaders()) {
    log::error("Shader compilation failed at startup; see the log above.");
    return 1;
  }

  // Item sprites are a provider like any other, so a resource pack overriding
  // `item/pick_copper` replaces the icon and both 3D models at once.
  resource::defaultStack().push(game::makeItemSpriteProvider());

  // The atlas has to exist before any chunk is meshed: the mesher resolves every
  // face through the tile table built from it, and item models are extruded from
  // its pixels.
  std::vector<ResourceId> textureIds = resource::collectBlockTextureIds();
  {
    const std::vector<ResourceId> itemIds = game::collectItemTextureIds();
    textureIds.insert(textureIds.end(), itemIds.begin(), itemIds.end());
  }
  resource::AtlasSettings atlasSettings;
  if (!atlas_.build(resource::defaultStack(), textureIds, atlasSettings)) {
    log::error("Failed to build the texture atlas.");
    return 1;
  }
  atlas_.upload();
  tiles_.build(atlas_);

  icons_.build(atlas_);
  if (!options.iconDumpPath.empty()) {
    const bool ok = icons_.writeDebugPng(options.iconDumpPath);
    log::info("icon sheet: %s", ok ? options.iconDumpPath.c_str() : "FAILED");
    return ok ? 0 : 1;
  }
  icons_.upload();

  // Before init: the query ring is allocated there, and only when asked for.
  renderer_.profiler().setEnabled(options.perf);
  if (!renderer_.init(shaders_, &atlas_)) {
    log::error("Renderer initialisation failed; see the log above.");
    return 1;
  }

  ui::settings().load(paths::settingsFile());
  // Straight after, because the globally saved colours ARE a setting — they live in
  // the same file and mean nothing without it having been read first.
  loadGlobalFavourites();
  // Who is trusted, banned, or whitelisted. Beside settings.json rather than in a
  // world, because being an operator is a fact about a person — see cmd/access.h.
  // A missing file is a fresh install with nobody trusted and nobody barred.
  access_.load(paths::accessFile());
  // Seed BEFORE anything applies settings, so the first apply sees "unchanged"
  // and leaves the stored render scale alone.
  lastQuality_ = ui::settings().text("graphicsQuality");

  // The web build could not open an AudioContext until the player clicked something,
  // so the whole engine had an "unlocked" state and every call no-opped until then.
  // A native process has no such rule: the device opens at startup and stays open.
  // A machine with no output device is not an error — every audio entry point
  // no-ops when the engine is not running, which is the same contract.
  audio::engine().start();
  // Before the interface, so the Resource Packs screen has a summary to show the
  // first time it is opened rather than after the first reload.
  applyResourcePacks();

  if (!interface_.init(shaders_, &icons_)) {
    log::error("Interface initialisation failed; see the log above.");
    return 1;
  }
  wireInterface();

  // The command line wins over the stored settings, so a screenshot run is
  // reproducible whatever the player last chose.
  if (!options.quality.empty()) {
    std::string name = options.quality;
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    ui::settings().setText("graphicsQuality", name);
  }
  if (options.renderDistance > 0) {
    ui::settings().setNumber("renderDistance", options.renderDistance);
  }
  // --fullscreen asks about this launch, not about every launch, so it is not written
  // back. applySettings() is what acts on it either way, which keeps one path from the
  // store to the window.
  if (options.fullscreen) ui::settings().setFlag("fullscreen", true, /*persist=*/false);
  applySettings();
  if (options.hideHud) interface_.setHudVisible(false);
  renderer_.setDebugView(options.debugView);
  if (options.skyTime >= 0.0f) {
    sky_.time = options.skyTime;
    sky_.paused = true;
  }

  window_.onResize = [this](int w, int h) {
    glViewport(0, 0, w, h);
    if (h > 0) camera_.setProjection(static_cast<float>(w) / h, camera_.fov());
  };
  window_.onFocusChange = [this](bool focused) {
    // A capture run must not be at the mercy of which window the desktop decided
    // to put in front. Two of these launched back to back would otherwise give one
    // screenshot of the world and one of the pause menu.
    if (!options_.screenshotPath.empty() || options_.exitAfterFrames > 0) return;
    // Losing focus while playing releases the pointer, matching how the web build
    // auto-paused when pointer lock was lost.
    if (!focused && state_ == AppState::Playing) {
      window_.setPointerCaptured(false);
      state_ = AppState::Paused;
    }
  };

  glViewport(0, 0, window_.width(), window_.height());
  // Re-established because applySettings() may have switched the window to
  // fullscreen or borderless above, which changes the aspect ratio.
  //
  // The field of view comes from the camera, NOT from a literal. It was 70.0f
  // here, three lines of code after applySettings() had just set the stored one —
  // so every launch silently threw the player's Field of View away and started at
  // the default. It read as "the slider does not persist" because the slider was
  // the one part of it that did: the value was loaded, shown, and saved correctly,
  // and only the camera never heard about it.
  camera_.setProjection(window_.aspect(), camera_.fov());

  // Workers start before the first world, and only for the game: the headless
  // tools never call this, so every one of them keeps running its jobs inline on
  // the thread that submitted them.
  jobs::system().start(options.threads < 0 ? jobs::defaultThreadCount() : options.threads);

  if (!options.worldId.empty()) {
    if (!loadWorldFromDisk(options.worldId)) {
      log::error("--world: could not open \"%s\"", options.worldId.c_str());
      return 1;
    }
  } else if (options.startWorld && !startWorld(options)) {
    return 1;
  }
  // --new-world: the same three steps the menu's Create button takes, in the same
  // order, so the scripted path and the played one exercise one code path.
  if (!options.newWorldName.empty() && world_) {
    worldMeta_.id = save::newId();
    worldMeta_.name = options.newWorldName;
    worldMeta_.seed = world_->seed();
    worldMeta_.genVersion = world_->genVersion();
    worldMeta_.createdAt = save::nowSeconds();
    worldOnDisk_ = true;
    if (!saveCurrentWorld()) return 1;
    log::info("--new-world: created \"%s\" as %s", worldMeta_.name.c_str(),
              worldMeta_.id.c_str());
  }
  // The web build's boot card existed because the browser had to show something while the
  // ES modules loaded. Here startup is synchronous and finishes before the first frame, so
  // Boot is never a state anyone can see: whatever happens above, we open on the menu
  // unless a world was asked for.
  if (state_ == AppState::Boot) {
    state_ = AppState::Menu;
    window_.setPointerCaptured(false);
  }
  if (!options.startScreen.empty()) applyStartScreen(options.startScreen);

  // Listening costs one bound socket and a datagram a second per host on the
  // network; starting it once here means the Join page is populated the moment it
  // opens rather than a second later.
  lanListener_.start();

  // --- multiplayer identity --------------------------------------------------
  // A coat-check ticket, not a fingerprint: it exists so a guest rejoining a host
  // lands back in the slot holding their inventory. Kept in its own file rather
  // than in settings.json, because the settings schema drives the settings screen
  // and neither of these belongs on it.
  {
    const std::string identityPath = paths::join(paths::dataDir(), "player.txt");
    std::vector<std::uint8_t> raw;
    std::string ignored;
    if (save::readFile(identityPath, raw, &ignored)) {
      const std::string text(raw.begin(), raw.end());
      const std::size_t newline = text.find('\n');
      playerId_ = text.substr(0, newline == std::string::npos ? text.size() : newline);
      if (newline != std::string::npos) {
        playerName_ = net::cleanName(text.substr(newline + 1));
      }
    }
    if (!net::validPlayerId(playerId_)) playerId_ = net::makePlayerId();
    if (!options.playerName.empty()) playerName_ = net::cleanName(options.playerName);
    if (playerName_.empty()) playerName_ = "Player";
    const std::string out = playerId_ + "\n" + playerName_;
    save::writeFileAtomic(identityPath,
                          std::vector<std::uint8_t>(out.begin(), out.end()), &ignored);
  }

  if (options.hostGame) {
    if (!world_) {
      log::error("--host needs a world; add --world, --new-world or --seed");
      return 1;
    }
    if (!startHosting(options.hostPort)) return 1;
  } else if (!options.joinAddress.empty()) {
    if (!startJoining(options.joinAddress, options.joinPort)) return 1;
  }

  running_ = true;

  while (running_ && !window_.shouldClose()) {
    frame();
  }

  if (streamFrames_ > 0) {
    log::info("streaming on the main thread over %lld frames: mean %.3f ms, worst %.2f ms "
              "at frame %lld (%d chunk worker(s))",
              streamFrames_, streamTotal_ / streamFrames_, streamWorst_, streamWorstFrame_,
              jobs::system().threadCount());
    if (world_) {
      // Against what the same chunks would have cost as flat arrays, which is the
      // one comparison that needs no second binary to make: before banded
      // storage every chunk cost kCellsPerChunk * 5 bytes whatever was in it.
      const std::size_t chunks = world_->loadedChunkCount();
      const double flat =
          static_cast<double>(chunks) * world::kCellsPerChunk * 5.0 / (1024.0 * 1024.0);
      const double now = static_cast<double>(world_->residentBytes()) / (1024.0 * 1024.0);
      log::info("chunk storage: %zu chunks, %.1f MB resident, %.1f MB if flat (%.2fx)",
                chunks, now, flat, now > 0.0 ? flat / now : 0.0);
    }
  }
  renderer_.profiler().logReport();
  log::info("shutting down");
  // Closing the window is the native equivalent of closing the browser tab, and the
  // web build lost everything since its last autosave when that happened. Here the
  // shutdown is ours to run, so it costs one write to not lose the session.
  leaveNetwork("");
  if (worldOnDisk_ && !netGuest()) saveCurrentWorld();
  // Before the world goes away: a worker holding a chunk snapshot keeps it alive,
  // but a worker still writing into a result queue that is about to be destroyed
  // does not end well.
  jobs::system().stop();
  audio::engine().shutdown();
  world_.reset();
  renderer_.dispose();
  icons_.destroy();
  atlas_.destroy();
  shaders_.clear();
  screenQuad_.destroy();
  window_.destroy();
  log::close();
  return 0;
}

bool App::loadShaders() {
  menuBackdrop_ = shaders_.load({
      .name = "menuBackdrop",
      .vertAsset = "shaders/fullscreen.vert",
      .fragAsset = "shaders/menu_backdrop.frag",
      .defines = {},
      .attribs = {"aPos"},
  });
  return menuBackdrop_ != nullptr;
}

// The four graphics-quality presets, which set the resource-heavy knobs; the
// individual effect toggles then zero one out within whichever tier is chosen. The
// table itself lives with the settings schema now, so the settings screen and the
// renderer read the same rows.
render::QualitySettings App::qualityPreset(const std::string& name) {
  render::QualitySettings q;
  const auto ieq = [](const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i]))) {
        return false;
      }
    }
    return i == a.size() && b[i] == '\0';
  };
  for (const ui::QualityPreset& p : ui::qualityPresets()) {
    if (!ieq(name, p.name)) continue;
    q.scale = p.scale;
    q.ssaoSamples = p.ssaoSamples;
    q.godraySamples = p.godraySamples;
    q.ssrSteps = p.ssrSteps;
    q.shadowSize = p.shadowSize;
    q.cloudSteps = p.cloudSteps;
    break;
  }
  return q;
}

void App::applyResourcePacks() {
  const std::vector<resource::PackInfo> installed = resource::scanPacks();
  const std::vector<resource::PackInfo> active = resource::enabledPacks(installed);
  audio::sounds().rebuild(active);

  // The interface, from the same list and in the same order. Always called, even
  // with no packs enabled: rebuilding from no documents is what restores the
  // built-in theme after the last pack is switched off.
  const ui::UiPackReport themed = ui::applyUiPacks(active);
  for (const std::string& problem : themed.problems) log::warn("%s", problem.c_str());

  // Nine-slice art, which needs a GL context. Both callers of this function have
  // one: the atlas and the renderer are already uploaded by the time the first
  // runs, and the second is a button on a screen that is being drawn.
  const ui::SpriteReport sprites = ui::loadUiSprites(active);
  for (const std::string& problem : sprites.problems) log::warn("%s", problem.c_str());

  const audio::SoundBank::Stats& stats = audio::sounds().stats();
  char line[192];
  if (active.empty()) {
    std::snprintf(line, sizeof(line),
                  "No packs enabled \xE2\x80\x94 every sound is synthesised and the interface "
                  "is the built-in theme.");
  } else if (stats.events == 0 && themed.changedAnything()) {
    // A pack that only themes the interface is a real and reasonable kind of
    // pack. Reporting "0 of N sounds replaced" for one would read as broken.
    std::snprintf(line, sizeof(line), "%d interface value%s changed by %d pack%s",
                  themed.colors + themed.scalars, themed.colors + themed.scalars == 1 ? "" : "s",
                  themed.packs, themed.packs == 1 ? "" : "s");
  } else {
    // The count of what is REPLACED, not of what a pack contains: the number a
    // player wants after switching one on is how much of the game now sounds
    // different, and a pack full of files the game never asks for would otherwise
    // report a large number and change nothing.
    std::snprintf(line, sizeof(line),
                  "%d of %d sounds replaced by %d pack%s (%d clip%s, %.1f MB)", stats.events,
                  static_cast<int>(audio::soundEventCatalogue().size()),
                  static_cast<int>(active.size()), active.size() == 1 ? "" : "s", stats.clips,
                  stats.clips == 1 ? "" : "s",
                  static_cast<double>(stats.bytes) / (1024.0 * 1024.0));
  }

  std::string summary = line;
  // Appended rather than folded into the branch above, because a pack may well
  // supply both and the sound line is the one that has to stay intact.
  if (themed.changedAnything() && stats.events > 0) {
    char extra[96];
    std::snprintf(extra, sizeof(extra), " \xC2\xB7 %d interface value%s",
                  themed.colors + themed.scalars, themed.colors + themed.scalars == 1 ? "" : "s");
    summary += extra;
  }
  interface_.packs().setSummary(summary);
}

// Every setting, pushed into whichever subsystem owns it. This is the whole of "all
// settings live-apply": the settings screen calls back here on every change, and
// nothing is read from the store anywhere else per frame.
void App::applySettings() {
  ui::SettingsStore& s = ui::settings();

  // Choosing a preset moves the resolution slider to that preset's own scale, and
  // then the slider is the only thing that decides. Multiplying the two instead
  // would make a row reading "100%" render at 75, and letting the slider silently
  // override would delete the one thing the Low preset does for a weak GPU. Only
  // on an actual CHANGE, and never on the first call: `lastQuality_` starts empty
  // and is seeded from the stored value, because a startup that rewrites a
  // player's setting from a default is the exact bug --quality shipped with.
  const std::string qualityName = s.text("graphicsQuality");
  if (!lastQuality_.empty() && qualityName != lastQuality_) {
    for (const ui::QualityPreset& p : ui::qualityPresets()) {
      if (qualityName == p.name) {
        s.setNumber("renderScale", std::lround(p.scale * 100.0f));
        break;
      }
    }
  }
  lastQuality_ = qualityName;

  render::QualitySettings q = qualityPreset(qualityName);
  // The effect toggles gate features off within the selected tier, exactly as the web
  // build did — a zeroed sample count is how a pass is skipped.
  if (!s.flag("ambientOcclusion")) q.ssaoSamples = 0;
  if (!s.flag("godRays")) q.godrays = false;
  if (!s.flag("waterReflections")) q.ssrSteps = 0;
  if (!s.flag("castShadows")) q.shadowSize = 0;
  if (!s.flag("clouds")) q.cloudSteps = 0;
  // Overrides the preset's own scale, so the slider means what it says whichever
  // tier is selected rather than being silently multiplied by it.
  q.scale = static_cast<float>(s.number("renderScale")) / 100.0f;
  q.cloudShadows = s.flag("cloudShadows") ? 1.0f : 0.0f;
  renderer_.setQuality(q);

  const float fov = static_cast<float>(s.number("fov"));
  camera_.setProjection(window_.aspect(), fov);

  if (world_) world_->setRenderDistance(static_cast<int>(s.number("renderDistance")));

  // js/game/player.js:50 — the stored 1..30 maps onto the same 0.0002 multiplier the
  // web build used against the browser's already-accelerated movementX.
  playerOptions_.sensitivity = static_cast<float>(s.number("sensitivity")) * 0.0002f;
  playerOptions_.invertY = s.flag("invertY");
  // Creative unlocks the rows; it does not press them. An earlier version forced
  // flight on whenever creative was, which meant the Allow Flight switch sat there
  // reading Off while the player flew — a switch that lies about the thing it
  // controls is worse than no switch. Every creative rule is now exactly what its
  // own row says, and creative's job is to make those rows exist at all.
  const bool creative = s.available("creativeMode") && s.flag("creativeMode");
  playerOptions_.noClip = creative && s.flag("noClip");
  // The one exception, and it is not a default but a consequence: a body that never
  // collides never lands, so no-clip has to fly. Player::update enforces the same
  // thing; this keeps the two from disagreeing.
  // ANDed with creative rather than read on its own. A world saved while flight was
  // allowed still carries flight=true in its rules, and reading the row without
  // asking whether it is still reachable would go on granting flight in a survival
  // world forever. `available` is the single question "does this row exist here".
  playerOptions_.flightAllowed =
      (creative && s.flag("flight")) || playerOptions_.noClip;
  playerOptions_.invulnerable = creative && s.flag("noHealth");
  playerOptions_.instantBreak = creative && s.flag("instantBreak");
  playerOptions_.hungerEnabled = s.flag("hunger") && !playerOptions_.invulnerable;
  playerOptions_.fallDamageEnabled = s.flag("fallDamage");
  playerOptions_.stepHeight = s.flag("highStep") ? 1.0f : game::playerConst::kStep;

  // The debug view was launch-flag-only until now — set once in init and never
  // again. The command line still wins, so a capture run is not at the mercy of
  // whatever the settings file happens to say.
  if (options_.debugView != 0) {
    renderer_.setDebugView(options_.debugView);
  } else if (s.available("debugView")) {
    renderer_.setDebugView(s.selectedIndex("debugView"));
  } else {
    renderer_.setDebugView(0);
  }

  interact_.setInstantBreak(playerOptions_.instantBreak);
  // The recipe book becomes an item picker in a creative world, and goes back to
  // being a recipe book the moment creative is switched off.
  interface_.recipeBook().setCreative(creative);
  // And the bin appears with it. Switching creative OFF while something is sitting
  // in the bin has to hand that something back: it was never thrown away, and a slot
  // that stops being drawn is not a slot anyone can retrieve from.
  if (!creative) emptyTrashToPlayer();
  interface_.inventory().setCreative(creative);

  window_.setRawMouseMotion(s.flag("rawMouse"));
  interface_.setUiScale(static_cast<float>(s.number("uiScale")) / 100.0f);

  // Guarded because setFullscreen is a real mode switch: calling it with the state it
  // is already in would still tear the window down and put it back, and this runs on
  // every change to any of the twenty-six rows.
  if (window_.borderless() != s.flag("borderless")) window_.setBorderless(s.flag("borderless"));
  if (window_.fullscreen() != s.flag("fullscreen")) window_.setFullscreen(s.flag("fullscreen"));

  // --no-vsync is a measurement flag and outranks the row for the run it is given
  // on; otherwise the setting decides. Applied unconditionally because
  // glfwSwapInterval is cheap and idempotent, unlike a window mode switch.
  window_.setVsync(!options_.noVsync && s.flag("vsync"));
  frameLimit_ = options_.noVsync ? 0 : static_cast<int>(s.number("frameLimit"));

  // The four sliders are stored 0..100 and squared inside the engine, so they feel
  // linear-ish in loudness rather than in amplitude.
  audio::engine().setVolumes(static_cast<float>(s.number("masterVolume")) / 100.0f,
                             static_cast<float>(s.number("sfxVolume")) / 100.0f,
                             static_cast<float>(s.number("ambientVolume")) / 100.0f,
                             static_cast<float>(s.number("uiVolume")) / 100.0f);
}

// Everything Interact hands back to the wider game.
game::InteractHooks App::makeInteractHooks() {
  game::InteractHooks hooks;
  hooks.notify = [this](const std::string& message) {
    interface_.notify().push(message, ui::Toast::Important);
  };
  hooks.onOpenPainting = [this](int x, int y, int z) {
    paintingX_ = x;
    paintingY_ = y;
    paintingZ_ = z;
    state_ = AppState::PaintingPick;
    window_.setPointerCaptured(false);
  };
  hooks.onOpenStation = [this](world::Station station, int x, int y, int z) {
    stationOpen_ = true;
    stationX_ = x;
    stationY_ = y;
    stationZ_ = z;
    if (station == world::Station::Chest) {
      audio::sfx::chestOpen(Vec3{x + 0.5f, y + 0.5f, z + 0.5f});
    }
    // A guest asks the host for the real contents and for the lock on it; what is
    // in the local block entity until the answer arrives is whatever the last
    // snapshot of the world left there.
    if (netGuest() && station != world::Station::Workbench) {
      netClient_.sendBlockEntityRequest(
          x, y, z, static_cast<std::uint8_t>(station == world::Station::Forge
                                                 ? game::BlockEntityKind::Forge
                                                 : game::BlockEntityKind::Chest));
    }
    interface_.openStation(station);
    state_ = AppState::Inventory;
    window_.setPointerCaptured(false);
  };
  // A bed no longer sleeps on contact. It opens the wheel, which is a clock you can
  // read for nothing and a chooser you can only act on when you are tired enough.
  // The decision of what the confirm button *does* is made here rather than in the
  // screen, because it differs by mode and the screen should not know about the
  // network at all.
  hooks.onSleep = [this] {
    if (!world_) return;
    const float proposed = netHosting() ? netHost_.proposedSleep()
                                        : (netGuest() ? netClient_.proposedSleep() : -1.0f);
    if (proposed >= 0.0f) {
      // Somebody is already waiting on an hour. Show theirs, and make this a yes.
      interface_.timeWheel().openVote(
          sky_.time, proposed,
          netHosting() ? netHost_.proposer() : netClient_.proposer());
    } else {
      interface_.timeWheel().open(sky_.time);
    }
    state_ = AppState::TimeWheel;
    window_.setPointerCaptured(false);
  };
  // The Soul Anchor, from js/main.js:765-780. The bound point is a save field, which
  // is why binding it lands here rather than with the sleep and respawn systems it
  // otherwise belongs to.
  hooks.onSetSpawn = [this](int x, int y, int z) {
    hasSpawn_ = true;
    spawn_ = {x + 0.5f, static_cast<float>(y + 1), z + 0.5f};
    // A guest's spawn is the host's to remember, since a guest's world is never
    // written. It travels with the rest of their progress in PlayerState.
    if (netGuest()) netClient_.setSpawn(true, spawn_);
    log::info("spawn bound to %d %d %d", x, y, z);
    interface_.notify().push("Spawn point bound");
  };
  hooks.onAnchorBroken = [this](int x, int y, int z) {
    if (!hasSpawn_) return;
    // Only the anchor you were actually bound to unbinds you; breaking somebody
    // else's spare one must not move your spawn.
    if (std::fabs(spawn_.x - (x + 0.5f)) > 0.01f ||
        std::fabs(spawn_.y - static_cast<float>(y + 1)) > 0.01f ||
        std::fabs(spawn_.z - (z + 0.5f)) > 0.01f) {
      return;
    }
    hasSpawn_ = false;
    if (netGuest()) netClient_.setSpawn(false, Vec3{});
    interface_.notify().push("Spawn point unbound");
  };
  hooks.onEat = [this](const game::ItemDef& item) {
    if (!player_) return false;
    const bool ok =
        player_->eat({static_cast<float>(item.food), item.risky, item.sat, item.group});
    if (ok) audio::sfx::eat();
    return ok;
  };
  // Tilling and sowing. Both are "use the held item on that block", which is why
  // they share one hook rather than having one each — see InteractHooks::onUseOn.
  hooks.onUseOn = [this](const game::ItemDef& item, int x, int y, int z) {
    if (!world_) return game::UseResult::Ignored;
    const world::BlockRegistry& reg = world::blocks();
    const world::WellKnownBlocks& w = world::wk();

    // The decision is a pure function so it can be tested without a window; App
    // keeps only the parts that need a world, a speaker and a screen.
    const game::FarmPlan plan =
        game::planFarmUse(item, world_->getBlock(x, y, z),
                          world_->getBlock(x, y + 1, z) == world::kAir, std::rand());

    switch (plan.action) {
      case game::FarmAction::Till:
        // The right one of the four straight away, rather than plain soil that the
        // sweep corrects a second or two later. Tilling next to a river should look
        // damp the instant the hoe comes down.
        world_->setBlock(x, y, z, world::farmlandFor(false, world_->moistFarmland(x, y, z)),
                         0);
        audio::sfx::blockPlace(
            reg.def(w.farmland),
            Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
        return game::UseResult::Used;  // a hoe is not consumed by using it
      case game::FarmAction::Enrich:
        world_->setBlock(x, y, z, world::farmlandFor(true, world_->moistFarmland(x, y, z)),
                         0);
        audio::sfx::blockPlace(
            reg.def(w.farmlandRich),
            Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
        return game::UseResult::UsedAndConsume;
      case game::FarmAction::Sow:
        world_->setBlock(x, y + 1, z, plan.crop, world::cropMetaFor(0));
        audio::sfx::blockPlace(
            reg.def(plan.crop),
            Vec3{static_cast<float>(x), static_cast<float>(y + 1), static_cast<float>(z)});
        return game::UseResult::UsedAndConsume;
      case game::FarmAction::None:
        break;
    }
    return game::UseResult::Ignored;
  };

  // The wayshard: one use, straight up to the open sky. js/main.js:785.
  hooks.onWarp = [this] {
    if (!world_ || !player_) return false;
    if (player_->mount() != 0) {
      interface_.notify().push("Not while riding \xE2\x80\x94 dismount first");
      return false;
    }
    const Vec3 p = player_->pos();
    const int ts = world_->topSolidY(static_cast<int>(std::floor(p.x)),
                                     static_cast<int>(std::floor(p.z)));
    if (ts < 0) {
      interface_.notify().push("The wayshard can't find the sky here", ui::Toast::Important);
      return false;
    }
    if (p.y >= ts - 0.5f) {
      interface_.notify().push("You're already under the open sky", ui::Toast::Important);
      return false;
    }
    // teleport rather than setPos: the trip up must not be charged as the fall it
    // is not, which is the same trap the respawn path has.
    player_->teleport(Vec3{p.x, static_cast<float>(ts + 1), p.z});
    // Tell the host this vertical jump was a wayshard and not a speed hack, or its
    // movement check snaps the player straight back underground.
    if (netGuest()) netClient_.sendWarp();
    audio::sfx::warp();
    interface_.notify().push("The wayshard shatters \xE2\x80\x94 daylight.");
    return true;
  };
  // A guest's boat belongs to the host: consume the item locally so the click
  // feels instant, ask for the entity, and let the next snapshot deliver it.
  hooks.spawnBoat = [this](const Vec3& at) {
    if (!world_) return false;
    if (netGuest()) {
      netClient_.sendBoatSpawn(at);
      return true;
    }
    return entities_.spawn(game::EntityType::Boat, Vec3{at.x, at.y + game::kBoatSeatY, at.z}) !=
           nullptr;
  };
  hooks.relayEntity = [this](const game::Entity& e, game::InteractButton button) {
    if (!netGuest()) return false;
    if (e.type == game::EntityType::Boat && button == game::InteractButton::Right) {
      // `netId`, not `id`. A ghost's id is -netId - 1, so the host was being asked
      // about an entity that could not exist and answered BoatDeny every single
      // time — which is the whole of "boats do not work for the guest". Every
      // other relay below already got this right, which is exactly why it went
      // unnoticed: the mistake was in the one line that did not look like the
      // others.
      netClient_.sendBoatMount(e.netId, !e.data.rider);
      mountedNetId_ = e.netId;
      // Deliberately NOT handled — the local hook runs too, and seats us. Waiting
      // a round trip to sit down in a boat you are standing against feels like the
      // click was ignored, so the guest climbs in immediately and a BoatDeny is
      // what stands it back up. The hull is then simulated here, because a boat
      // you are riding is part of your body; the host is told where it went by
      // your pose and mirrors it for everyone else.
      return false;
    }
    if (e.type == game::EntityType::RemotePlayer) {
      // PvP. The ghost carries the network id, not the player id, so the target
      // is found back through the roster the ghosts keep.
      for (const net::Ghosts::Nameplate& plate : netClient_.ghosts().nameplates()) {
        const float dx = plate.pos.x - e.pos.x, dz = plate.pos.z - e.pos.z;
        if (dx * dx + dz * dz < 0.25f) {
          netClient_.sendPlayerHit(plate.playerId, inventory_.selectedSlot().key, false);
          return true;
        }
      }
      return true;  // an unidentified body is still not ours to hit locally
    }
    if (button == game::InteractButton::Left) {
      netClient_.sendHit(e.netId, inventory_.selectedSlot().key, false);
      return true;
    }
    // Right-clicking a mob — shearing, milking — is the host's to resolve too,
    // and it answers with the item through `give`.
    netClient_.sendHit(e.netId, inventory_.selectedSlot().key, false);
    return true;
  };
  // The palette opens its own screen. HERE, with the rest of them, and not in
  // wireInterface where it was first written: `interactHooks_ = makeInteractHooks()`
  // assigns the whole struct, so anything bolted onto interactHooks_ outside this
  // function is silently wiped the next time it runs. The symptom was a palette that
  // did nothing at all when right-clicked, with no error anywhere.
  hooks.onPalette = [this] {
    interface_.palette().open();
    interface_.openPalette();
    state_ = AppState::Palette;
    window_.setPointerCaptured(false);
    return true;
  };
  hooks.entities = &entities_;
  hooks.entityContext = &entityContext_;
  return hooks;
}

// The interface's screen enum tracks AppState, but it is not the same machine: the
// inventory and the Atlas draw over a live world, which AppState models as its own
// states while the interface has to know both which screen is up and that the world is
// still there.
void App::syncScreen() {
  // Anything but Playing means the world has stopped, and the held item's clocks
  // stop with it. A swing caught mid-arc by a screen is dropped rather than left
  // paused: it would otherwise complete itself on the way back, with nothing
  // having been clicked to cause it.
  if (state_ != AppState::Playing) renderer_.viewmodel().cancelSwing();
  // The picker is the gallery with one extra flag set, so it goes through the
  // interface's own opener instead of through setScreen.
  if (state_ == AppState::PaintingPick) {
    interface_.openPaintingPicker();
    return;
  }
  interface_.setScreen(screenFor(state_));
}

// Which screen each state puts up.
//
// Two states can name the same screen, and that is the point of writing it down
// rather than leaving it inside syncScreen: the palette IS the inventory screen in
// another mode, and the picker IS the gallery wired to a different action. Anything
// that ought to be true of a screen can now be decided by the screen instead of by
// remembering to name each of its states — which is a thing that was not remembered.
ui::Screen screenFor(AppState state) {
  switch (state) {
    case AppState::Boot: return ui::Screen::Boot;
    case AppState::Menu: return ui::Screen::Menu;
    case AppState::Playing: return ui::Screen::None;
    case AppState::Paused: return ui::Screen::Pause;
    case AppState::Settings: return ui::Screen::Settings;
    case AppState::Inventory: return ui::Screen::Inventory;
    case AppState::RecipeBook: return ui::Screen::RecipeBook;
    case AppState::Map: return ui::Screen::Map;
    case AppState::Gallery: return ui::Screen::Gallery;
    case AppState::Packs: return ui::Screen::Packs;
    case AppState::PaintingPick: return ui::Screen::Gallery;
    case AppState::TimeWheel: return ui::Screen::TimeWheel;
    case AppState::Palette: return ui::Screen::Inventory;
  }
  return ui::Screen::None;
}

// What E shuts as well as opens: the inventory screen in any of its modes — the bag,
// a station, the palette — and the recipe book, which is the one screen E closes that
// it did not open.
//
// Asked of the SCREEN, not of a list of states. Written as a list it was correct on
// the day it was written and wrong by the end of the update: the palette arrived as
// a state of its own, was not added, and E did nothing at all in the one screen where
// both hands are full and Escape is the last key anyone reaches for.
bool closesWithE(AppState state) {
  return screenFor(state) == ui::Screen::Inventory || state == AppState::RecipeBook;
}

ui::UiFrame App::uiFrame() {
  ui::UiFrame f;
  f.player = player_.get();
  f.world = world_.get();
  f.sky = &sky_;
  f.inventory = &inventory_;
  f.camera = &camera_;
  f.atlas = &atlas_;
  f.fps = clock_.fps();
  f.hasTarget = interact_.hasSelection();
  f.targetX = interact_.selectionX();
  f.targetY = interact_.selectionY();
  f.targetZ = interact_.selectionZ();
  f.breakFraction = interact_.breakFraction();
  f.hurtFlash = hurtFlash_;
  f.version = HR_VERSION;

  // Nameplates come from whichever half of the net layer is live; both keep the
  // same list, because both draw the same remote bodies.
  const net::Ghosts* ghosts = netHosting()   ? &netHost_.ghosts()
                              : netGuest()   ? &netClient_.ghosts()
                                             : nullptr;
  if (ghosts) {
    for (const net::Ghosts::Nameplate& plate : ghosts->nameplates()) {
      f.nameplates.push_back(
          ui::UiFrame::Nameplate{plate.pos, plate.name, plate.health, plate.yaw});
    }
    const int others = static_cast<int>(ghosts->playerCount());
    f.netLine = (netHosting() ? "hosting \xC2\xB7 " : "guest \xC2\xB7 ") +
                std::to_string(others + 1) + " player" + (others == 0 ? "" : "s");
  }
  return f;
}

void App::wireInterface() {
  interface_.callbacks.resume = [this] { resumePlaying(); };
  interface_.callbacks.quitGame = [this] { running_ = false; };
  interface_.inventory().onDropStack = [this](const game::ItemStack& s) { tossStack(s); };
  interface_.callbacks.toggleRecipeBook = [this] { toggleRecipeBook(); };
  interface_.chat().onSubmit = [this](const std::string& line) { submitChat(line); };
  // The clipboard belongs to the window, so the chat box borrows it through here.
  // Copying a seed out of the log, or pasting one into a command, is most of what
  // makes the log worth reading rather than just watching.
  interface_.chat().onCopy = [this](const std::string& text) {
    window_.setClipboardText(text);
  };
  interface_.chat().onPaste = [this] { return window_.clipboardText(); };
  interface_.callbacks.settingChanged = [this](const std::string& key) {
    // Actions do nothing to the settings and everything here. Handled before
    // applySettings, which would only re-read a row that stores nothing.
    if (key == "openResourcePacks") {
      packsReturn_ = state_;
      state_ = AppState::Packs;
      window_.setPointerCaptured(false);
      return;
    }
    if (key == "locateDungeon") {
      if (!world_ || !player_) return;
      world::DungeonSite site;
      const Vec3 at = player_->pos();
      if (world::findDungeon(world_->noise(), world_->seed(), world_->genVersion(),
                             static_cast<int>(at.x), static_cast<int>(at.z), 6, site)) {
        char line[96];
        std::snprintf(line, sizeof(line), "Dungeon at %d %d %d", site.x, site.y, site.z);
        interface_.notify().push(line);
        // Pinned as well as printed: a coordinate you have to remember while you
        // walk is a coordinate you write on a sticky note. Straight into the list
        // rather than through addWaypoint, which derives its own height from the
        // surface and numbers its own name — neither of which suits a thing that is
        // forty blocks underground and has a name already.
        std::vector<ui::Waypoint>& pins = interface_.atlas().waypoints();
        ui::Waypoint pin;
        pin.x = static_cast<float>(site.x);
        pin.y = static_cast<float>(site.y);
        pin.z = static_cast<float>(site.z);
        pin.name = "Dungeon";
        pin.color = ui::waypointColor(0);
        pins.push_back(std::move(pin));
      } else {
        interface_.notify().push("No dungeon within range", ui::Toast::Important);
      }
      return;
    }
    applySettings();
    // A world's own rule changed, and everyone in it plays by it. Guests cannot
    // reach these rows at all, so this only ever fires on the host.
    if (netHost_.running() && ui::settings().isWorldScoped(key)) {
      netHost_.broadcastWorldSettings();
    }
  };
  interface_.callbacks.closeStation = [this] {
    if (stationOpen_ && interface_.inventory().mode() == ui::InventoryMode::Chest) {
      audio::sfx::chestClose(Vec3{stationX_ + 0.5f, stationY_ + 0.5f, stationZ_ + 0.5f});
    }
    if (stationOpen_ && netGuest() && world_) {
      // Sent on close rather than per click: the host holds the lock for as long
      // as the screen is open, so nobody else can be writing in between, and one
      // message carries the result of a whole rummage.
      if (const game::BlockEntity* be = world_->getBlockEntity(stationX_, stationY_, stationZ_)) {
        net::BeStateMsg state;
        state.x = stationX_;
        state.y = stationY_;
        state.z = stationZ_;
        state.kind = static_cast<std::uint8_t>(be->kind);
        const auto wire = [](const game::ItemStack& slot) {
          net::WireSlot out;
          if (slot.empty()) return out;
          out.key = slot.key;
          out.count = slot.count;
          out.dura = slot.dura;
          out.tint = slot.tint;
          return out;
        };
        state.input = wire(be->input);
        state.fuel = wire(be->fuel);
        state.output = wire(be->output);
        state.fuelLeft = be->fuelLeft;
        state.fuelMax = be->fuelMax;
        state.progress = be->progress;
        for (const game::ItemStack& slot : be->slots) state.slots.push_back(wire(slot));
        state.container = wire(be->container);
        state.final = true;
        netClient_.sendBlockEntityState(state);
      }
    }
    stationOpen_ = false;
  };
  interface_.callbacks.currentDiet = [this]() -> const game::Diet* {
    return player_ ? &player_->diet() : nullptr;
  };
  interface_.callbacks.currentStation = [this]() -> game::BlockEntity* {
    if (!stationOpen_ || !world_) return nullptr;
    return world_->getBlockEntity(stationX_, stationY_, stationZ_);
  };
  interface_.callbacks.openSettings = [this] {
    settingsReturn_ = state_;
    state_ = AppState::Settings;
    window_.setPointerCaptured(false);
  };
  interface_.callbacks.openGallery = [this] {
    galleryReturn_ = state_;
    state_ = AppState::Gallery;
    window_.setPointerCaptured(false);
  };
  interface_.callbacks.openPacks = [this] {
    packsReturn_ = state_;
    state_ = AppState::Packs;
    window_.setPointerCaptured(false);
  };
  interface_.packs().onApply = [this] {
    applyResourcePacks();
    ui::settings().save();
  };
  interface_.packs().onCreateExample = [this](std::string& messageOut) {
    const std::string dir = paths::join(paths::resourcePacksDir(), "ExamplePack");
    std::string error;
    if (!resource::writeExamplePack(dir, audio::soundEventCatalogue(), &error)) {
      messageOut = "Could not create it: " + error;
      return false;
    }
    messageOut = "Wrote ExamplePack \xE2\x80\x94 read its README.txt, drop sounds in, press Reload.";
    return true;
  };
  interface_.gallery().onPick = [this](const std::string& path) {
    const int x = paintingX_, y = paintingY_, z = paintingZ_;
    resumePlaying();
    game::Painting art;
    if (!game::paintingFromPng(path, art)) {
      interface_.notify().push("That picture could not be read", ui::Toast::Important);
      return;
    }
    // A guest asks; the host decides and tells everyone, itself included. Same
    // division as hitting a mob: the world is the host's, and a guest that hung a
    // picture locally would be the only person who ever saw it.
    if (netGuest()) {
      netClient_.sendPainting(x, y, z, art);
    } else {
      applyPainting(x, y, z, std::move(art));
    }
  };
  // The palette. Its slot and both favourite lists are App's, handed to the screen
  // by pointer — the screen draws them and reports changes, and this is what writes
  // them anywhere.
  interface_.inventory().attachTrash(&trashSlot_);
  interface_.palette().attach(&inventory_, &paletteSlot_);
  // And the inventory screen, which owns the slot the player actually fills.
  interface_.inventory().attachPalette(&interface_.palette(), &paletteSlot_);
  interface_.palette().worldFavourites = &paletteWorldFavourites_;
  interface_.palette().globalFavourites = &paletteGlobalFavourites_;
  interface_.palette().onClose = [this] { closeCurrentScreen(); };
  interface_.palette().onFavouritesChanged = [this](bool global) {
    // A world list rides out with the next world save like everything else; a global
    // one has no such moment, so it is written the instant it changes.
    if (global) saveGlobalFavourites();
  };
  interface_.timeWheel().onCancel = [this] { closeCurrentScreen(); };
  interface_.timeWheel().onConfirm = [this](float target) {
    closeCurrentScreen();
    // Alone, a bed is just a bed. In company it is a proposal, and the host is the
    // only clock — a guest fast-forwarding its own sky would drift out of step with
    // everyone else's night.
    if (netGuest()) {
      netClient_.sendSleep(true, target);
      return;
    }
    if (netHosting()) {
      netHost_.onLocalSleep(true, target);
      return;
    }
    const float span = std::fmod(target - sky_.time + 1.0f, 1.0f);
    sky_.startSleep(target);
    interface_.notify().push("You sleep for " +
                             render::Sky::spanString(span <= 0.0005f ? 1.0f : span));
  };
  // Creative's stopgap item source. The book only lists things with a recipe, so
  // raw ore and mob drops are not reachable this way — a proper item picker is the
  // eventual answer; this is the half of it that costs nothing.
  interface_.recipeBook().canGive = [this] {
    return ui::settings().available("creativeMode") && ui::settings().flag("creativeMode");
  };
  interface_.recipeBook().onGive = [this](const std::string& key, int count) {
    if (!game::getItem(key)) return;
    const int want = count > 0 ? count : 1;
    const int left = inventory_.give(key, want);
    // Spilled rather than swallowed, which is what every other give in the game
    // does with its remainder.
    if (left > 0 && player_) tossStack(game::ItemStack{key, left});
    interface_.notify().push(std::to_string(want - left) + "x " +
                             game::getItem(key)->name);
  };

  interface_.recipeBook().onAutoFill = [this](const game::Recipe& recipe) {
    // Only from a crafting screen: the book reached with H from the world has no
    // grid behind it, and opening one uninvited would be a surprise rather than a
    // convenience.
    if (state_ != AppState::RecipeBook || recipeReturn_ != AppState::Inventory) {
      interface_.notify().push("Open a crafting screen to lay a recipe out", ui::Toast::Important);
      return;
    }
    state_ = recipeReturn_;
    interface_.openStation(recipeStation_);
    switch (interface_.inventory().autoFill(recipe)) {
      case ui::InventoryUI::FillResult::Ok: break;
      case ui::InventoryUI::FillResult::TooBig:
        interface_.notify().push("That needs a bigger crafting grid");
        break;
      case ui::InventoryUI::FillResult::Missing:
        interface_.notify().push("You are missing something for that");
        break;
      case ui::InventoryUI::FillResult::NoGrid:
        interface_.notify().push("No crafting grid open", ui::Toast::Important);
        break;
    }
  };
  interface_.callbacks.closeScreen = [this] { closeCurrentScreen(); };
  interface_.callbacks.saveAndQuit = [this] {
    const bool saved = netGuest() ? false : saveCurrentWorld();
    leaveWorld();
    // "Left the world" is the honest message for a world that was never on disk —
    // a --seed harness run has nothing to save and should not claim it did.
    interface_.notify().push(saved ? "Saved and left the world" : "Left the world");
  };

  interface_.menu().actions.createWorld = [this](const std::string& name, std::uint32_t seed,
                                                 bool creative) {
    // Set before startWorld, which is what reads it: the mode has to be in place by
    // the time the world's settings come into scope, or the first frame is played
    // under the wrong rules and saved that way.
    pendingCreative_ = creative;
    AppOptions fresh = options_;
    fresh.seed = seed;
    fresh.haveSpawnOverride = false;
    fresh.startingItems.clear();
    fresh.startingEntities.clear();
    inventory_.clear();
    interface_.atlas().reset();
    world_.reset();
    player_.reset();
    if (!startWorld(fresh)) return;

    worldMeta_ = save::WorldMeta{};
    worldMeta_.id = save::newId();
    worldMeta_.name = name.empty() ? "New World" : name;
    worldMeta_.seed = seed;
    worldMeta_.genVersion = world_->genVersion();
    worldMeta_.createdAt = save::nowSeconds();
    worldOnDisk_ = true;
    // Written immediately, so the world appears in the list even if the player
    // quits with alt-F4 rather than through the menu.
    saveCurrentWorld();
    interface_.notify().push("Welcome to " + worldMeta_.name);
  };

  interface_.menu().actions.listWorlds = [] {
    std::vector<ui::WorldEntry> rows;
    const std::int64_t now = save::nowSeconds();
    for (const save::WorldListing& w : save::list()) {
      ui::WorldEntry row;
      row.id = w.id;
      row.name = w.name;
      row.seed = w.seed;
      row.ageSeconds = static_cast<double>(now - w.savedAt);
      row.genVersion = w.genVersion;
      row.currentGenVersion = world::kGenVersion;
      rows.push_back(std::move(row));
    }
    return rows;
  };

  interface_.menu().actions.loadWorld = [this](const std::string& id) {
    if (world_) {
      saveCurrentWorld();
      leaveWorld();
    }
    loadWorldFromDisk(id);
  };

  interface_.menu().actions.deleteWorld = [this](const std::string& id) {
    // Deleting the world that is open would leave the game holding a save with
    // nowhere to write to, so it is closed first — without saving, which would put
    // the file straight back.
    if (worldOnDisk_ && worldMeta_.id == id) leaveWorld();
    if (save::erase(id)) {
      interface_.notify().push("World deleted");
    } else {
      interface_.notify().push("Could not delete that world", ui::Toast::Important);
    }
  };

  // Both of these refuse to touch a world that is currently open, for the same
  // reason deleteWorld closes it first: the running game holds the authoritative
  // copy and would write it straight back over whatever was done here.
  interface_.menu().actions.backupWorld = [this](const std::string& id) {
    if (worldOnDisk_ && worldMeta_.id == id) saveCurrentWorld();
    std::string error;
    if (save::backup(id, nullptr, &error)) {
      interface_.notify().push("Copy saved");
    } else {
      log::warn("backup failed: %s", error.c_str());
      interface_.notify().push("Could not copy that world", ui::Toast::Important);
    }
  };

  interface_.menu().actions.upgradeWorld = [this](const std::string& id) {
    if (worldOnDisk_ && worldMeta_.id == id) saveCurrentWorld();
    // The copy first, and the update only if it worked. A failed backup that still
    // let the update through would be the one outcome this whole screen exists to
    // prevent.
    std::string error;
    if (!save::backup(id, nullptr, &error)) {
      log::warn("upgrade aborted, backup failed: %s", error.c_str());
      interface_.notify().push("Could not copy the world, so nothing was changed", ui::Toast::Important);
      return;
    }
    if (save::setGenVersion(id, world::kGenVersion, &error)) {
      interface_.notify().push("World updated \xE2\x80\x94 a copy of the old one was kept");
    } else {
      log::warn("upgrade failed: %s", error.c_str());
      interface_.notify().push("Could not update that world", ui::Toast::Important);
    }
  };

  interface_.menu().actions.importWorlds = [this] {
    int failures = 0;
    const int imported = save::importAllFromExports(&failures);
    if (imported == 0 && failures == 0) {
      interface_.notify().push("Put a .hrw world in data/exports first", ui::Toast::Important);
    } else if (failures == 0) {
      interface_.notify().push("Imported " + std::to_string(imported) +
                               (imported == 1 ? " world" : " worlds"));
    } else {
      interface_.notify().push("Imported " + std::to_string(imported) + ", " +
                               std::to_string(failures) + " unreadable");
    }
  };

  interface_.menu().actions.lanGames = [this] {
    std::vector<ui::LanGame> out;
    for (const net::Beacon& b : lanListener_.found()) {
      ui::LanGame g;
      g.address = b.address;
      g.port = b.port;
      g.worldName = b.worldName;
      g.hostName = b.hostName;
      g.players = b.players;
      g.maxPlayers = b.maxPlayers;
      out.push_back(std::move(g));
    }
    return out;
  };

  interface_.menu().actions.joinGame = [this](const std::string& target) {
    // One field for three things a player might have: a pasted invite code, an
    // address with a port, or a bare address. Tried in that order, because only
    // the first is unambiguous.
    std::string address = target;
    std::uint16_t port = net::kDefaultGamePort;
    std::string worldName;
    if (!net::parseInviteCode(target, address, port, worldName)) {
      address = target;
      const std::size_t colon = target.rfind(':');
      if (colon != std::string::npos) {
        address = target.substr(0, colon);
        port = static_cast<std::uint16_t>(std::atoi(target.c_str() + colon + 1));
      }
      if (port == 0) port = net::kDefaultGamePort;
    }
    if (address.empty()) {
      interface_.notify().push("That is not an address or an invite code", ui::Toast::Important);
      return;
    }
    if (world_) leaveWorld();
    startJoining(address, port);
  };

  interface_.menu().actions.toggleHosting = [this] {
    if (netGuest()) {
      leaveNetwork("Left the host");
      leaveWorld();
      return;
    }
    if (netHosting()) {
      leaveNetwork("Stopped hosting");
      return;
    }
    startHosting(net::kDefaultGamePort);
  };
  interface_.menu().actions.isHosting = [this] { return netHosting(); };
  interface_.menu().actions.isGuest = [this] { return netGuest(); };
  interface_.menu().actions.hostStatus = [this]() -> std::string {
    if (netGuest()) {
      return "Playing on " + netClient_.roster().front().name + "'s world";
    }
    if (!netHosting()) return {};
    const int guests = netHost_.guestCount();
    return net::makeInviteCode(net::localAddress(), netHost_.port(), worldMeta_.name) + "\n" +
           std::to_string(guests) + (guests == 1 ? " guest connected" : " guests connected");
  };
  // Telling somebody to go and read a log file for the one string they need to
  // send a friend was never a feature. The code is on screen; this puts it on the
  // clipboard.
  interface_.menu().actions.copyInvite = [this] {
    if (!netHosting()) return;
    window_.setClipboardText(
        net::makeInviteCode(net::localAddress(), netHost_.port(), worldMeta_.name));
    interface_.notify().push("Invite code copied");
  };

  interface_.menu().actions.exportWorld = [this](const std::string& idOrEmpty) {
    // The pause menu passes nothing, meaning "the world I am in".
    const std::string id = idOrEmpty.empty() ? worldMeta_.id : idOrEmpty;
    if (id.empty()) {
      interface_.notify().push("There is no saved world to export", ui::Toast::Important);
      return;
    }
    // Export reads the file, so an open world is written out first — otherwise the
    // shared copy is whatever was on disk at the last autosave.
    if (worldOnDisk_ && worldMeta_.id == id) saveCurrentWorld();
    std::string path;
    std::string error;
    if (save::exportWorld(id, "", &path, &error)) {
      log::info("exported to %s", path.c_str());
      interface_.notify().push("Exported to data/exports");
    } else {
      interface_.notify().push("Export failed: " + error, ui::Toast::Important);
    }
  };

  interface_.inventory().attach(&inventory_, &icons_);
  interface_.atlas().attach(&atlas_);
}

// Whether cartography is unlocked right now.
//
// A property of what is being carried, asked every time rather than latched when
// the map opens — "lose the Atlas and the map goes with it" is the documented
// rule, and the way you lose it is by dying, which happens while you are holding
// something else entirely.
bool App::hasAtlas() const {
  return player_ != nullptr && ui::Atlas::hasAtlasItem(inventory_);
}

void App::resumePlaying() {
  if (!world_ || !player_) {
    state_ = AppState::Menu;
    return;
  }
  state_ = AppState::Playing;
  window_.setPointerCaptured(true);
  resumeClickGuard_ = 1;
}

// One place for "the Escape / close-button path", because each screen returns somewhere
// different: settings goes back where it was opened from, the overlays go back to the
// world, and the pause menu resumes.
void App::saveGlobalFavourites() {
  std::string joined;
  for (const std::uint32_t c : paletteGlobalFavourites_) {
    if (!joined.empty()) joined.push_back('|');
    joined += ui::toHex(c);
  }
  ui::settings().setText("paletteFavourites", joined);
  ui::settings().save();
}

void App::loadGlobalFavourites() {
  paletteGlobalFavourites_.clear();
  const std::string& joined = ui::settings().text("paletteFavourites");
  std::size_t start = 0;
  while (start <= joined.size()) {
    const std::size_t bar = joined.find('|', start);
    const std::string piece = joined.substr(start, bar == std::string::npos ? bar : bar - start);
    std::uint32_t rgb = 0;
    // Anything unparseable is dropped rather than defaulting to black. A settings
    // file edited by hand should not be able to inject a colour nobody chose.
    if (ui::parseHex(piece, rgb)) paletteGlobalFavourites_.push_back(rgb);
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  if (paletteGlobalFavourites_.size() > 7) paletteGlobalFavourites_.resize(7);
}

void App::emptyTrashToPlayer() {
  if (trashSlot_.empty()) return;
  const int left = inventory_.give(trashSlot_.key, trashSlot_.count, trashSlot_.dura,
                                   trashSlot_.tint);
  if (left <= 0) {
    trashSlot_.clear();
    return;
  }
  // A full bag. Onto the floor, the same way every other overflow in the game
  // spills — but only if there is a world to spill into. Without one the item stays
  // where it is rather than being quietly deleted by a settings change.
  if (!world_ || !player_) {
    trashSlot_.count = left;
    return;
  }
  game::ItemStack spill = trashSlot_;
  spill.count = left;
  tossStack(spill);
  trashSlot_.clear();
}

void App::closeCurrentScreen() {
  switch (state_) {
    case AppState::Settings:
      state_ = settingsReturn_ == AppState::Paused ? AppState::Paused : AppState::Menu;
      if (state_ == AppState::Paused && !world_) state_ = AppState::Menu;
      break;
    case AppState::Inventory:
    case AppState::RecipeBook:
    case AppState::Map:
    case AppState::PaintingPick:
    case AppState::TimeWheel:
    case AppState::Palette:
      resumePlaying();
      break;
    case AppState::Gallery:
      state_ = galleryReturn_ == AppState::Paused && world_ ? AppState::Paused
                                                           : AppState::Menu;
      break;
    case AppState::Packs:
      // Opened from Settings, Back returns to Settings — otherwise a trip to the
      // packs screen from inside a world dumps you at the main menu.
      state_ = packsReturn_ == AppState::Settings ? AppState::Settings
               : packsReturn_ == AppState::Paused && world_ ? AppState::Paused
                                                            : AppState::Menu;
      break;
    case AppState::Paused: resumePlaying(); break;
    default: break;
  }
}

bool App::startWorld(const AppOptions& options, const save::WorldSave* loaded) {
  // A saved world regenerates from its own seed and generator version, never from
  // whatever the options happen to hold — terrain has to come back exactly as it
  // was, and the generator version is the whole reason that field exists.
  const std::uint32_t seed = loaded ? loaded->meta.seed : options.seed;
  const int genVersion = loaded ? loaded->meta.genVersion : world::kGenVersion;
  const int distance = options.renderDistance > 0
                           ? options.renderDistance
                           : static_cast<int>(ui::settings().number("renderDistance"));
  world_ = std::make_unique<world::World>(seed, distance, genVersion);
  world_->setTileTable(&tiles_);

  // Before anything streams in: generateChunk replays the edit map over freshly
  // generated terrain, so the edits must already be installed when the first chunk
  // is built. Installing them afterwards would leave the spawn chunks unmodified.
  if (loaded) {
    world_->setEdits(loaded->edits);
    // Beside setEdits and for the same reason spelled out above: applyEdits replays
    // both onto freshly generated terrain, so a colour installed after the first
    // chunk lands is a colour that chunk was never meshed with.
    world_->setTints(loaded->tints);
    world_->setExplored(loaded->explored);
    world_->blockEntities() = loaded->blockEntities;
    world_->installPaintings(loaded->paintings);
    paletteWorldFavourites_ = loaded->paletteFavourites;
  }

  entities_.clear();
  world_->setDropSink([this](float x, float y, float z, const std::string& key, int count,
                             int dura, std::int32_t tint) {
    // A guest owns no entities, so it cannot make one here. Everything that
    // reaches this sink on a guest is something the local player just broke —
    // mining, or a bucket that would not fit back in the bag — so it goes
    // straight into their hands, which is what the local drop entity did anyway
    // half a second later. Spawning one instead built a private item that only
    // this guest could ever see, and that nobody could ever tidy away.
    //
    // Whatever will not fit goes to the host as a real thrown item, so a full bag
    // spills onto the floor in front of everyone rather than deleting the ore.
    if (netGuest()) {
      const int left = inventory_.give(key, count, dura, tint);
      if (left > 0) {
        netClient_.sendToss(Vec3{x, y, z}, Vec3{0, 0, 0},
                            game::ItemStack{key, left, dura, tint});
      }
      return;
    }
    entities_.spawnDrop(Vec3{x, y, z}, key, count, dura, tint);
  });
  world_->setLootSink([this](int x, int y, int z, bool rich) {
    // A guest fills nothing. It regenerates the same terrain locally, so it sees
    // exactly the same chests appear — but what is inside one is the host's to
    // decide, and it arrives on request through the block-entity path. Rolling it
    // here as well would build a private guess that the first click overwrites.
    if (netGuest()) return;
    // Already there means already filled. Chunks are regenerated every time the
    // player walks back into them, but block entities are not unloaded with a
    // chunk and are saved with the world — so an existing one is a reliable record
    // that this chest has been seen before, whether it is still full or the player
    // emptied it an hour ago.
    if (world_->getBlockEntity(x, y, z)) return;

    game::BlockEntity* be =
        world_->getOrCreateBlockEntity(x, y, z, game::BlockEntityKind::Chest);
    if (!be) return;
    // Seeded from the world and the position, so the same chest holds the same
    // things however many times it is generated, on whichever machine.
    const std::vector<game::ItemStack> loot =
        game::rollLoot(rich ? "dungeon/altar" : "dungeon/chest", world_->seed(), x, y, z);
    for (std::size_t i = 0; i < loot.size() && i < be->slots.size(); ++i) {
      be->slots[i] = loot[i];
    }
  });
  world_->fallSink = [this](float x, float y, float z, world::BlockId id, int meta) {
    if (game::Entity* e = entities_.spawn(game::EntityType::FallingBlock, Vec3{x, y, z})) {
      // `dura` is the BlockId it will put back down; `key` exists only so the
      // renderer can find the right cube. See fallingblock.cpp.
      e->data.dura = static_cast<int>(id);
      e->data.key = world::blocks().def(id).key;
      e->data.count = meta;
      if (const game::EntityDef* def = game::defOf(game::EntityType::FallingBlock)) {
        if (def->spawn) def->spawn(*e);
      }
    }
  };

  interactHooks_ = makeInteractHooks();
  renderer_.setEntities(&entities_);
  // The world's own rules come into scope here and go out of it in leaveWorld.
  // Both paths, because a brand new world needs the defaults installed just as
  // much as a loaded one needs what it stored -- otherwise the first world opened
  // in a session would read whatever the menu happened to be showing.
  ui::settings().beginWorld(loaded ? loaded->worldSettings
                                   : ui::SettingsStore::WorldValues{});
  // What the world was made as. A loaded world says so in its own section; a fresh
  // one is whatever the New World screen asked for, which createWorld left here
  // before calling in. Not a setting, so nothing in the interface can change it —
  // that is the entire point of it being separate.
  createdCreative_ = loaded ? loaded->createdCreative : (pendingCreative_ || options.creativeWorld);
  ui::settings().setVirtualFlag("worldIsCreative", createdCreative_);
  // A world made Creative starts in it. Anything else and the row is not there to
  // be read, so this is the only place the mode is ever assumed rather than stored.
  if (createdCreative_ && !loaded) ui::settings().setFlag("creativeMode", true);
  if (options.creativeWorld) ui::settings().setFlag("creativeMode", true);
  // Apply them NOW, not on the first frame of play.
  //
  // syncSettingsIfChanged only runs inside updatePlaying, so a world entered with
  // a screen already open — the recipe book, the pause menu, anything --screen
  // opens — got its rules applied only once the player closed that screen and the
  // world started ticking. Everything a world's rules decide was therefore wrong
  // for exactly as long as somebody stood in a menu: flight, invulnerability, and
  // whether the recipe book is a recipe book at all.
  applySettings();
  settingsRevision_ = ui::settings().revision();
  if (loaded) {
    inventory_ = loaded->inventory;
    entities_.load(loaded->entities);
    sky_.time = loaded->meta.time;
    sky_.setHoursAwake(loaded->hoursAwake);
    hasSpawn_ = loaded->meta.hasSpawn;
    spawn_ = loaded->meta.spawn;

    std::vector<ui::Waypoint>& points = interface_.atlas().waypoints();
    points.clear();
    points.reserve(loaded->waypoints.size());
    for (const save::WaypointSave& w : loaded->waypoints) {
      ui::Waypoint p;
      p.x = w.x;
      p.y = w.y;
      p.z = w.z;
      p.name = w.name;
      p.color = Rgba{static_cast<std::uint8_t>((w.color >> 24) & 0xFF),
                     static_cast<std::uint8_t>((w.color >> 16) & 0xFF),
                     static_cast<std::uint8_t>((w.color >> 8) & 0xFF),
                     static_cast<std::uint8_t>(w.color & 0xFF)};
      p.death = w.death;
      points.push_back(std::move(p));
    }
  }
  for (const auto& [key, count] : options.startingItems) {
    if (!game::getItem(key)) {
      log::warn("--give: unknown item \"%s\"", key.c_str());
      continue;
    }
    const int left = inventory_.give(key, count);
    if (left > 0) log::warn("--give: %s x%d did not fit", key.c_str(), left);
  }

  float sx = options.spawnX;
  float sz = options.spawnZ;
  float sy = options.spawnY;
  // A saved player stands where it was left, unless --at overrode it — which is
  // what makes `--world <id> --at x,0,z` a usable way to go and look at something.
  if (loaded && !options.haveSpawnOverride) {
    sx = loaded->player.pos.x;
    sy = loaded->player.pos.y;
    sz = loaded->player.pos.z;
  }
  // `--at x,0,z` asks for the surface at (x, z): y = 0 is bedrock and never a
  // camera position anyone wants, so it reads as "wherever the ground is" and saves
  // looking a height up by hand for every capture.
  if (options.haveSpawnOverride && sy == 0.0f) {
    sy = static_cast<float>(
        world::heightAt(world_->noise(), static_cast<int>(std::floor(sx)),
                        static_cast<int>(std::floor(sz)), world_->genVersion())) +
         2.0f;
  }
  // Where this world calls home: dry, reasonably flat, clear of ravines, and as
  // near the origin as all that allows. Computed for every world and not only for
  // a new one, because it is also where you come back after dying without a soul
  // anchor — and it is a pure function of the seed and the generator version, so
  // asking it again on load gives a saved world exactly the home it was created
  // with, and no save format has to carry it.
  worldSpawn_ = world_->findSpawn(kSpawnX, kSpawnZ);
  if (!options.haveSpawnOverride && !loaded) {
    // A saved world keeps wherever the player was left, and --at overrides both.
    sx = worldSpawn_.x;
    sz = worldSpawn_.z;
    sy = worldSpawn_.y;
  }

  // Synchronous, so the player never spawns inside unloaded space and fall through.
  world_->primeSpawn(sx, sz);

  player_ = std::make_unique<game::Player>(sx, sy, sz);
  player_->setLook(options.spawnYaw, options.spawnPitch);
  // Player::onHurt has existed since M5 and nothing ever read it. Set rather than
  // accumulated, so a second hit during the fade restarts the flash at full rather
  // than stacking into an opaque red screen.
  player_->onHurt = [this] { hurtFlash_ = 1.0f; };
  if (loaded) {
    // Health, hunger and the look direction, then the position put back on top —
    // loadState uses the saved one, and --at has to win over it when given.
    player_->loadState(loaded->player);
    if (options.haveSpawnOverride) {
      player_->setPos(Vec3{sx, sy, sz});
      player_->setLook(options.spawnYaw, options.spawnPitch);
    }
  }
  state_ = AppState::Playing;

  // --hang: a wall two blocks north of spawn with a painting on the near face.
  if (!options.hangPicture.empty()) {
    game::Painting art;
    if (game::paintingFromPng(options.hangPicture, art)) {
      const int bx = static_cast<int>(std::floor(sx));
      const int bz = static_cast<int>(std::floor(sz)) - 4;
      const int by = static_cast<int>(sy) + 1;
      for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = 0; dy <= 2; ++dy) {
          world_->setBlock(bx + dx, by + dy, bz, world::wk().greystone, 0);
        }
      }
      // The wall is at bz and the canvas one step toward spawn at bz+1, so the
      // wall is on the canvas's -z side: meta 3, which faces +z, back at spawn.
      world_->setBlock(bx, by + 1, bz + 1, world::wk().canvas, 3);
      world_->setPainting(bx, by + 1, bz + 1, std::move(art));
      log::info("--hang: painting at %d,%d,%d", bx, by + 1, bz + 1);
    }
  }

  // --spawn: a ring of entities around the spawn point, dropped from a little above
  // the surface so they settle onto whatever is actually there.
  if (!options.startingEntities.empty()) {
    refreshEntityContext();
    float angle = 0.0f;
    for (const auto& [key, count] : options.startingEntities) {
      const game::EntityType type = game::entityTypeFromKey(key);
      if (type == game::EntityType::None) {
        log::warn("--spawn: unknown entity \"%s\"", key.c_str());
        continue;
      }
      for (int n = 0; n < count; ++n) {
        angle += 1.7f;  // an irrational-ish step, so a crowd fans out
        const float radius = 5.0f + 1.4f * n;
        const int wx = static_cast<int>(std::floor(sx + std::cos(angle) * radius));
        const int wz = static_cast<int>(std::floor(sz + std::sin(angle) * radius));
        int wy = static_cast<int>(sy);
        while (wy < world::WH - 2 && world_->getBlock(wx, wy, wz) != world::kAir) ++wy;
        entities_.spawn(type, Vec3{wx + 0.5f, static_cast<float>(wy) + 0.1f, wz + 0.5f});
      }
    }
    log::info("--spawn: %d entities placed", entities_.count());
  }

  // A screenshot run must not wait for a click to capture the pointer.
  if (options.screenshotPath.empty() && options.exitAfterFrames == 0) {
    window_.setPointerCaptured(true);
  }
  autosaveT_ = 0.0f;
  log::info("world ready at (%.1f, %.1f, %.1f)", sx, sy, sz);
  return true;
}

// ---- multiplayer ------------------------------------------------------------

net::GameRefs App::gameRefs() {
  net::GameRefs refs;
  refs.world = world_.get();
  refs.player = player_.get();
  refs.inventory = &inventory_;
  refs.entities = &entities_;
  refs.sky = &sky_;
  return refs;
}

net::SessionHooks App::makeSessionHooks() {
  net::SessionHooks hooks;
  hooks.notify = [this](const std::string& message) {
    interface_.notify().push(message, ui::Toast::Important);
  };
  hooks.buildSave = [this] { return buildSave(); };
  hooks.adoptWorld = [this](const save::WorldSave& data) { adoptRemoteWorld(data); };
  hooks.onDisconnected = [this](const std::string& reason) {
    // The one case with nobody left to tell: this IS the host telling us it has
    // gone, or the connection failing on its own.
    leaveNetwork(reason.empty() ? "Disconnected" : reason, /*sayGoodbye=*/false);
  };
  hooks.onRosterChange = [] {};
  hooks.playSfx = [](const std::string& kind, const Vec3& pos) {
    audio::sfx::playNamed(kind, pos);
  };
  // The host will not give us that container. Only close the screen if it is the
  // one being shown — a denial can arrive for a chest we asked about, changed our
  // mind on, and walked away from, and shutting the player's inventory for that
  // would be a worse surprise than the one it is fixing.
  hooks.onMountDenied = [this](int netId) {
    if (!player_ || mountedNetId_ != netId) return;
    mountedNetId_ = 0;
    if (game::Entity* boat = entities_.byId(player_->mount())) {
      boat->data.rider = false;
    }
    player_->setMount(0);
    interface_.notify().push("That boat is taken");
  };
  // --- chat and commands ------------------------------------------------------
  // Host: a guest's line, run here at THAT guest's level. The Host handed it up
  // without looking at it; every decision about it is taken in runCommand.
  hooks.onChatLine = [this](const std::string& playerId, const std::string& line) {
    runCommand(playerId, line);
  };
  // Guest: a line the host wants shown.
  hooks.onChatShow = [this](std::uint8_t kind, const std::string& from,
                            const std::string& text) {
    // Clamped rather than refused: a newer host adding a colour must not cost an
    // older guest the line itself.
    const auto safe = kind <= static_cast<std::uint8_t>(ui::Chat::Kind::Error)
                          ? static_cast<ui::Chat::Kind>(kind)
                          : ui::Chat::Kind::Say;
    showChat(safe, from, text);
  };
  hooks.levelOf = [this](const std::string& playerId, const std::string& name) {
    return static_cast<std::uint8_t>(levelFor(playerId, name));
  };
  hooks.mayJoin = [this](const std::string& playerId, const std::string& name,
                         std::string& reason) {
    const bool allowed = access_.mayJoin(playerId, name, reason);
    // Whoever this turned out to be, tie their id to the row that names them. A ban
    // or a grant is always typed against a NAME, because that is what the person
    // typing it knows; this is the only moment the game ever learns which id that
    // name belongs to, and without it a banned player walks back in under a new one.
    // Done for a refused peer too — especially for a refused peer.
    if (access_.remember(playerId, name)) access_.save();
    return allowed;
  };
  hooks.onPermission = [this](const std::string& playerId, std::uint8_t level) {
    netLevels_[playerId] = level <= 3 ? static_cast<cmd::Level>(level) : cmd::Level::Anyone;
  };
  hooks.onSetState = [this](float health, bool clearInventory) {
    if (clearInventory) inventory_.clear();
    if (health >= 0.0f && player_) {
      if (health <= 0.0f) {
        player_->setHealth(0.0f);
      } else {
        player_->reviveFull();
      }
    }
  };
  hooks.onContainerDenied = [this](int x, int y, int z, const std::string&) {
    if (!stationOpen_ || x != stationX_ || y != stationY_ || z != stationZ_) return;
    // Straight to false rather than through closeStation, whose whole job is to
    // send the host what we did with the container. There is nothing to send: the
    // host just told us we never had it.
    stationOpen_ = false;
    if (state_ == AppState::Inventory || state_ == AppState::RecipeBook) resumePlaying();
  };
  return hooks;
}

// ---------------------------------------------------------------------------
// Chat and commands
//
// Every command in the game runs on whichever machine owns the world. In single
// player and while hosting that is this one; while joined it is the host's, and
// this build only sends the text. There is deliberately no second path — see
// cmd/command.h's trust model.
// ---------------------------------------------------------------------------

cmd::Level App::localLevel() const {
  // Owning the world is what Owner means. Nobody granted it and nobody can take it
  // away, which is also why the access list is not consulted here: a host who had
  // demoted themselves in access.json would be locked out of their own machine.
  if (!netGuest()) return cmd::Level::Owner;
  const auto it = netLevels_.find(playerId_);
  return it == netLevels_.end() ? cmd::Level::Anyone : it->second;
}

cmd::Level App::levelFor(const std::string& playerId, const std::string& name) const {
  if (playerId == playerId_) return localLevel();
  // As a guest, whatever the host last said. We are not the authority and must not
  // answer from our own access list, which is about people on THIS machine.
  if (netGuest()) {
    const auto it = netLevels_.find(playerId);
    return it == netLevels_.end() ? cmd::Level::Anyone : it->second;
  }
  return access_.levelOf(playerId, name);
}

std::vector<cmd::Participant> App::participants(const std::string& callerId) const {
  std::vector<cmd::Participant> out;
  const auto add = [&](const std::string& id, const std::string& name, bool host) {
    cmd::Participant p;
    p.playerId = id;
    p.name = name;
    p.level = levelFor(id, name);
    p.self = id == callerId;
    p.host = host;
    if (id == playerId_) {
      if (player_) {
        p.pos = player_->pos();
        p.hasPos = true;
      }
    } else if (netHosting()) {
      p.hasPos = netHost_.ghosts().playerPos(id, p.pos);
    } else if (netGuest()) {
      p.hasPos = netClient_.ghosts().playerPos(id, p.pos);
    }
    out.push_back(std::move(p));
  };

  if (multiplayer()) {
    const std::vector<net::RosterEntry> roster =
        netHosting() ? netHost_.roster() : netClient_.roster();
    for (const net::RosterEntry& r : roster) add(r.playerId, r.name, r.host);
    return out;
  }
  // Single player: one person, and they are the host of a world with no guests.
  if (player_) add(playerId_, playerName_, true);
  return out;
}

void App::showChat(ui::Chat::Kind kind, const std::string& from, const std::string& text) {
  // The name is composed in here rather than at the sender, so a line that arrived
  // over the wire and one produced locally read identically. Only plain speech
  // wears a name — everything else has whatever name it needs already baked into
  // its text, because the host wrote it.
  const std::string line =
      kind == ui::Chat::Kind::Say && !from.empty() ? "<" + from + "> " + text : text;
  interface_.chat().push(kind, line);
  // Also to the log. Two reasons, both of them real: a headless run driven by
  // --command has no screen to read the answer off, and somebody running a world
  // for other people will want to know what was said in it after the fact.
  log::info("chat: %s", line.c_str());
}

void App::chatTo(const std::string& playerId, ui::Chat::Kind kind, const std::string& from,
                 const std::string& text) {
  if (playerId.empty() || playerId == playerId_) {
    showChat(kind, from, text);
    return;
  }
  if (netHosting()) {
    netHost_.sendChatLine(playerId, static_cast<std::uint8_t>(kind), from, text);
  }
}

void App::chatAll(ui::Chat::Kind kind, const std::string& from, const std::string& text) {
  showChat(kind, from, text);
  if (netHosting()) netHost_.broadcastChatLine(static_cast<std::uint8_t>(kind), from, text);
}

cmd::Hooks App::makeCommandHooks() {
  cmd::Hooks h;

  // Everything below reads `replyTo_` at call time rather than capturing it, so one
  // Hooks can serve every caller: the alternative is rebuilding twenty
  // std::functions per typed line.
  h.reply = [this](std::string_view text) {
    chatTo(replyTo_, ui::Chat::Kind::Reply, {}, std::string(text));
  };
  h.announce = [this](std::string_view text) {
    chatAll(ui::Chat::Kind::System, {}, std::string(text));
  };
  h.participants = [this] { return participants(replyTo_); };

  h.whisper = [this](const cmd::Participant& who, const std::string& text, std::string& error) {
    if (!multiplayer()) {
      error = "there is nobody else here";
      return false;
    }
    chatTo(who.playerId, ui::Chat::Kind::Whisper, {}, replyName_ + " \xE2\x86\x92 you: " + text);
    return true;
  };

  h.teleport = [this](const cmd::Participant& who, const Vec3& to, std::string& error) {
    if (who.playerId == playerId_) {
      if (!player_) {
        error = "you have no body here to move";
        return false;
      }
      player_->teleport(to);
      // The host has to be warned, or its movement check answers our jump with a
      // teleport back — the same reason a wayshard sends one.
      if (netGuest()) netClient_.sendWarp();
      return true;
    }
    if (!netHosting() || !netHost_.teleportPlayer(who.playerId, to)) {
      error = who.name + " is not connected";
      return false;
    }
    return true;
  };

  h.give = [this](const cmd::Participant& who, const std::string& key, int count,
                  std::string& error) {
    if (who.playerId == playerId_) {
      if (!world_) {
        error = "there is no world to put it in";
        return false;
      }
      const int left = inventory_.give(key, count, -1);
      // Whatever did not fit goes on the floor rather than nowhere, which is what
      // every other path that hands out items does.
      if (left > 0) tossStack(game::ItemStack{key, left});
      return true;
    }
    if (!netHosting() || !netHost_.givePlayer(who.playerId, key, count, -1)) {
      error = who.name + " is not connected";
      return false;
    }
    return true;
  };

  h.setVitals = [this](const cmd::Participant& who, float health, std::string& error) {
    if (who.playerId == playerId_) {
      if (!player_) {
        error = "you have no body here";
        return false;
      }
      if (health <= 0.0f) {
        player_->setHealth(0.0f);
      } else {
        player_->reviveFull();
      }
      return true;
    }
    if (!netHosting() || !netHost_.setPlayerState(who.playerId, health, false)) {
      error = who.name + " is not connected";
      return false;
    }
    return true;
  };

  h.clearInventory = [this](const cmd::Participant& who, std::string& error) {
    if (who.playerId == playerId_) {
      inventory_.clear();
      return true;
    }
    if (!netHosting() || !netHost_.setPlayerState(who.playerId, -1.0f, true)) {
      error = who.name + " is not connected";
      return false;
    }
    return true;
  };

  h.kick = [this](const cmd::Participant& who, std::string_view reason, std::string& error) {
    if (!netHosting()) {
      error = "nobody else is connected";
      return false;
    }
    if (!netHost_.kick(who.playerId, std::string(reason))) {
      error = who.name + " is not connected";
      return false;
    }
    return true;
  };

  // --- the access list -------------------------------------------------------
  // Every one of these writes the file immediately. A crash between opping
  // somebody and quitting cleanly should not un-op them, and the file is a few
  // hundred bytes.

  h.setLevel = [this](const std::string& playerId, const std::string& name, cmd::Level level,
                      std::string& error) {
    if (!access_.setLevel(playerId, name, level)) {
      error = "the access list is full";
      return false;
    }
    access_.save();
    if (netHosting() && !playerId.empty()) {
      netHost_.broadcastPermission(playerId, static_cast<std::uint8_t>(level));
    }
    return true;
  };

  h.setBanned = [this](const std::string& name, bool on, std::string_view reason,
                       std::string& error) {
    // By name, because whoever typed it knew a name. The id is filled in from the
    // roster when that person is actually here, which is what makes the ban follow
    // them across a rename — see cmd::Access::touch.
    std::string id;
    for (const cmd::Participant& p : participants(replyTo_)) {
      if (p.name == name) id = p.playerId;
    }
    if (!access_.setBanned(id, name, on, std::string(reason))) {
      error = "the access list is full";
      return false;
    }
    access_.save();
    return true;
  };

  h.setAllowed = [this](const std::string& name, bool on, std::string& error) {
    std::string id;
    for (const cmd::Participant& p : participants(replyTo_)) {
      if (p.name == name) id = p.playerId;
    }
    if (!access_.setAllowed(id, name, on)) {
      error = "the access list is full";
      return false;
    }
    access_.save();
    return true;
  };

  h.setWhitelistEnabled = [this](bool on, std::string&) {
    access_.setWhitelistEnabled(on);
    access_.save();
    return true;
  };

  h.permList = [this] {
    std::vector<std::string> out;
    for (const cmd::AccessEntry& e : access_.entries()) {
      if (e.level == cmd::Level::Anyone) continue;
      out.push_back((e.name.empty() ? e.playerId : e.name) + " \xC2\xB7 " +
                    cmd::levelName(e.level));
    }
    return out;
  };
  h.banList = [this] {
    std::vector<std::string> out;
    for (const cmd::AccessEntry& e : access_.entries()) {
      if (!e.banned) continue;
      out.push_back((e.name.empty() ? e.playerId : e.name) +
                    (e.reason.empty() ? "" : " \xC2\xB7 " + e.reason));
    }
    return out;
  };
  h.allowList = [this] {
    std::vector<std::string> out;
    for (const cmd::AccessEntry& e : access_.entries()) {
      if (!e.allowed) continue;
      out.push_back(e.name.empty() ? e.playerId : e.name);
    }
    if (!access_.whitelistEnabled() && !out.empty()) out.push_back("(the whitelist is off)");
    return out;
  };

  // --- the world ------------------------------------------------------------

  h.saveWorld = [this](std::string& error) {
    if (!world_) {
      error = "there is no world open";
      return false;
    }
    if (netGuest()) {
      error = "this world belongs to the host";
      return false;
    }
    if (!saveCurrentWorld()) {
      error = "the world could not be written";
      return false;
    }
    return true;
  };

  h.stopSession = [this](std::string& error) {
    if (!world_) {
      error = "there is no world open";
      return false;
    }
    // Deferred to the end of the frame. Tearing the world down here would free the
    // world and player this command's own Context still points at — and this runs
    // from inside the host's message loop, which is walking a peer list that
    // leaving the world would empty.
    pendingStop_ = true;
    return true;
  };

  h.applySetting = [this](const std::string& key, const std::string& value,
                          std::string& error) {
    const ui::SettingDef* def = ui::settings().find(key);
    if (!def) {
      error = "there is no setting called '" + key + "'";
      return false;
    }
    // The same gate the settings screen uses, so the rule that a survival world can
    // never become creative lives in exactly one place: the schema row.
    if (!ui::settings().editable(key)) {
      error = std::string(def->label) + " cannot be changed here";
      return false;
    }
    switch (def->type) {
      case ui::SettingType::Toggle: {
        bool on = false;
        if (!cmd::parseBool(value, on)) {
          error = "say true or false";
          return false;
        }
        ui::settings().setFlag(key, on);
        break;
      }
      case ui::SettingType::Slider: {
        float number = 0;
        if (!cmd::parseCoord(value, 0.0f, number) || number < def->min || number > def->max) {
          char range[64];
          std::snprintf(range, sizeof(range), "give a number from %g to %g", def->min, def->max);
          error = range;
          return false;
        }
        ui::settings().setNumber(key, number);
        break;
      }
      case ui::SettingType::Select: {
        bool known = false;
        for (const char* option : def->options) known = known || value == option;
        if (!known) {
          error = "that is not one of the choices";
          return false;
        }
        ui::settings().setText(key, value);
        break;
      }
      case ui::SettingType::Text:
        ui::settings().setText(key, value);
        break;
      case ui::SettingType::Action:
        error = "that is a button, not a setting";
        return false;
    }
    applySettings();
    // A world rule changing mid-session has to reach the guests, or half the room
    // is playing by the old one and nobody is told.
    if (netHosting() && def->scope == ui::SettingScope::World) {
      netHost_.broadcastWorldSettings();
    }
    return true;
  };

  h.summon = [this](const std::string& type, int count, std::string& error) {
    const game::EntityType kind = game::entityTypeFromKey(type);
    // Drops, falling blocks and remote players are how the game represents
    // something that has already happened; summoning one directly produces an
    // entity with no item, no block, or nobody behind it.
    const bool summonable = kind == game::EntityType::Sheep || kind == game::EntityType::Pig ||
                            kind == game::EntityType::Cow ||
                            kind == game::EntityType::Zombie ||
                            kind == game::EntityType::Boat;
    if (!summonable) {
      error = "there is nothing called '" + type + "' to summon";
      return false;
    }
    if (netGuest()) {
      error = "only the host can put things in this world";
      return false;
    }
    // Around whoever asked, not around whoever is hosting.
    Vec3 at;
    bool have = false;
    for (const cmd::Participant& p : participants(replyTo_)) {
      if (p.self && p.hasPos) {
        at = p.pos;
        have = true;
      }
    }
    if (!have) {
      error = "you are not anywhere to summon it beside";
      return false;
    }
    for (int i = 0; i < count; ++i) {
      // A small ring rather than all on one spot, so a stack of eight sheep is
      // eight sheep rather than one shape flickering.
      const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
      entities_.spawn(kind, Vec3{at.x + std::cos(angle) * 1.6f, at.y + 0.5f,
                                 at.z + std::sin(angle) * 1.6f});
    }
    return true;
  };

  h.locateDungeon = [this](Vec3& out, std::string& error) {
    if (!world_) {
      error = "there is no world to search";
      return false;
    }
    Vec3 from{0, 0, 0};
    for (const cmd::Participant& p : participants(replyTo_)) {
      if (p.self && p.hasPos) from = p.pos;
    }
    world::DungeonSite site;
    if (!world::findDungeon(world_->noise(), world_->seed(), world_->genVersion(),
                            static_cast<int>(from.x), static_cast<int>(from.z), 6, site)) {
      error = "no dungeon within a few thousand blocks";
      return false;
    }
    out = Vec3{static_cast<float>(site.x), static_cast<float>(site.y),
               static_cast<float>(site.z)};
    return true;
  };

  h.worldSpawn = [this](Vec3& out, std::string& error) {
    if (!world_) {
      error = "there is no world open";
      return false;
    }
    out = worldSpawn_;
    return true;
  };

  return h;
}

void App::runCommand(const std::string& playerId, const std::string& line, bool console) {
  // Who is asking, resolved here rather than taken from anywhere — which is the
  // whole point of running every command on the authoritative side.
  std::string name = playerName_;
  if (playerId != playerId_ && netHosting()) {
    for (const net::RosterEntry& r : netHost_.roster()) {
      if (r.playerId == playerId) name = r.name;
    }
  }

  if (line.empty() || line[0] != '/') {
    chatAll(ui::Chat::Kind::Say, name, line);
    return;
  }

  replyTo_ = playerId;
  replyName_ = name;

  cmd::Hooks hooks = makeCommandHooks();
  cmd::Context ctx;
  ctx.playerId = playerId;
  ctx.name = name;
  ctx.level = levelFor(playerId, name);
  ctx.console = console;
  ctx.world = world_.get();
  // The body and bag belong to the caller, and this machine only holds its own. A
  // guest's are reached through the hooks, which is why nothing in the command
  // table reads these directly.
  ctx.player = playerId == playerId_ ? player_.get() : nullptr;
  ctx.inventory = playerId == playerId_ ? &inventory_ : nullptr;
  ctx.entities = &entities_;
  ctx.sky = &sky_;
  ctx.hooks = &hooks;

  const cmd::Result result = cmd::run(line, ctx);
  if (!result.message.empty()) {
    chatTo(playerId, result.ok ? ui::Chat::Kind::Reply : ui::Chat::Kind::Error, {},
           result.message);
  }
  replyTo_.clear();
  replyName_.clear();
}

void App::runStartupCommands() {
  if (startupCommandsDone_ || options_.startupCommands.empty()) return;
  // A guest has to be joined before a line means anything: sent earlier it would
  // run locally at this build's own level, which is precisely what a guest never
  // gets to do. Failed counts as settled — the commands then run here and mostly
  // refuse, which is a truthful answer rather than a silent nothing.
  if (netClient_.running() && netClient_.state() != net::Client::State::Playing &&
      netClient_.state() != net::Client::State::Failed) {
    return;
  }
  startupCommandsDone_ = true;
  for (const std::string& line : options_.startupCommands) {
    log::info("--command %s", line.c_str());
    // Verbatim, and through submitChat: this is the chat box with the typing done
    // for you, so a leading slash makes it a command and anything else is
    // something said out loud — exactly as it would be if a person typed it. And a
    // guest's line takes the same road a typed one takes, to the host, to be
    // judged there rather than here.
    submitChat(line);
  }
}

void App::submitChat(const std::string& line) {
  // A guest decides nothing. Even a line that is obviously not a command goes to
  // the host, because the host is what everybody else hears.
  if (netGuest()) {
    netClient_.sendChat(line);
    return;
  }
  runCommand(playerId_, line);
}

void App::openChat(bool withSlash) {
  // Chat is not a screen, and it has nowhere sensible to draw over one.
  if (state_ != AppState::Playing) return;
  refreshChatSources();
  interface_.chat().open(&window_.input(), withSlash ? "/" : "");
  window_.setPointerCaptured(false);
}

void App::refreshChatSources() {
  cmd::Sources sources;
  sources.level = localLevel();
  sources.inWorld = world_ != nullptr;
  for (const cmd::Participant& p : participants(playerId_)) sources.players.push_back(p.name);
  interface_.chat().sources = std::move(sources);
}

bool App::startHosting(std::uint16_t port) {
  if (multiplayer() || !world_ || !player_) return false;
  std::string error;
  if (!netHost_.start(port, playerId_, playerName_, gameRefs(), makeSessionHooks(), &error)) {
    interface_.notify().push("Could not host: " + error, ui::Toast::Important);
    return false;
  }
  // Every local edit — including the water simulation's own writes, which only
  // the host runs — goes out to the guests.
  world_->setEditSink([this](int x, int y, int z, world::BlockId id, int meta, int tint) {
    netHost_.onLocalEdit(x, y, z, static_cast<std::uint16_t>(id),
                         static_cast<std::uint8_t>(meta), tint);
  });
  netHost_.restoreGuests(lastLoadedGuests_);
  advertiser_.start(netHost_.port(),
                    worldMeta_.name.empty() ? std::string("Hollowreach") : worldMeta_.name,
                    playerName_);
  const std::string address = net::localAddress();
  interface_.notify().push("Hosting on port " + std::to_string(netHost_.port()));
  log::info("net: invite code %s",
            net::makeInviteCode(address, netHost_.port(), worldMeta_.name).c_str());
  return true;
}

bool App::startJoining(const std::string& address, std::uint16_t port) {
  if (multiplayer()) return false;
  std::string error;
  if (!netClient_.start(address, port, playerId_, playerName_, makeSessionHooks(), &error)) {
    interface_.notify().push("Could not join: " + error, ui::Toast::Important);
    return false;
  }
  interface_.notify().push("Connecting to " + address + "...");
  return true;
}

void App::adoptRemoteWorld(const save::WorldSave& data) {
  // Whatever was open is put away first — a guest joining from its own world must
  // not carry that world's chunks, entities or inventory into the host's.
  if (world_) {
    entities_.clear();
    world_.reset();
    player_.reset();
    inventory_.clear();
    interface_.atlas().reset();
  }
  AppOptions options = options_;
  // `--at` survives: a guest normally lands where the host's save puts it, but a
  // capture has to be able to stand somewhere it can see the host from.
  options.startingItems.clear();
  options.startingEntities.clear();
  // Nothing living comes out of the payload. A current host has already stripped
  // this (net/host.cpp sendWorld says why); it is stripped again here because the
  // consequence of getting it wrong is a world full of animals only one player can
  // see, and because the sender is not ours to trust. The copy costs one pass over
  // the edit map, once, at the moment a session begins.
  save::WorldSave scrubbed = data;
  scrubbed.entities.clear();
  if (!startWorld(options, &scrubbed)) {
    leaveNetwork("Could not build the host's world");
    return;
  }
  // A guest's world belongs to the host: it is never written, and the edit sink
  // sends local edits to the host for approval rather than recording them.
  worldMeta_ = data.meta;
  worldMeta_.id.clear();
  worldOnDisk_ = false;
  netClient_.attachGame(gameRefs());
  // The spawn the host was holding for us, handed straight back. Without this the
  // first PlayerState of the session would report an unbound spawn and overwrite
  // the very anchor the payload had just restored — a Soul Anchor that survived
  // exactly until ten seconds after rejoining.
  netClient_.setSpawn(hasSpawn_, spawn_);
  // startWorld installed the host's rules from the payload; they are not this
  // player's to change.
  ui::settings().setWorldLocked(true);
  world_->setEditSink([this](int x, int y, int z, world::BlockId id, int meta, int tint) {
    netClient_.sendEdit(x, y, z, static_cast<std::uint16_t>(id),
                        static_cast<std::uint8_t>(meta), tint);
  });
  interface_.notify().push("Joined " +
                           (data.meta.name.empty() ? std::string("the world") : data.meta.name));
}

void App::leaveNetwork(const std::string& reason, bool sayGoodbye) {
  advertiser_.stop();
  netHost_.stop();
  netClient_.stop(sayGoodbye);
  // Nothing on the far side to tell any more, and a stale id here would be
  // answered by whichever boat happened to hold it in the next world joined.
  mountedNetId_ = 0;
  if (world_) world_->setEditSink(nullptr);
  if (!reason.empty()) interface_.notify().push(reason, ui::Toast::Important);
}

// ---- saves -----------------------------------------------------------------

save::WorldSave App::buildSave() {
  save::WorldSave out;
  out.meta = worldMeta_;
  out.meta.savedAt = save::nowSeconds();
  out.meta.gameVersion = HR_VERSION;
  out.meta.time = sky_.time;
  out.hoursAwake = sky_.hoursAwake();
  out.worldSettings = ui::settings().worldValues();
  out.createdCreative = createdCreative_;
  out.meta.hasSpawn = hasSpawn_;
  out.meta.spawn = spawn_;
  // One call rather than seven assignments, so the list lives somewhere a test can
  // reach it. See save::captureWorld.
  if (world_) save::captureWorld(*world_, out);
  // The world-scoped saved colours. Not world_->something(): the list belongs to
  // the player's use of the palette in this world, and the World does not know the
  // palette exists.
  out.paletteFavourites = paletteWorldFavourites_;
  if (player_) out.player = player_->state();
  out.inventory = inventory_;
  out.entities = entities_.serialize();

  // Guest progress, so a friend rejoining picks up where they left off. Only the
  // host has any; a guest's own save is never written at all.
  if (netHosting()) out.guests = netHost_.guestsForSave();

  for (const ui::Waypoint& p : interface_.atlas().waypoints()) {
    save::WaypointSave w;
    w.x = p.x;
    w.y = p.y;
    w.z = p.z;
    w.name = p.name;
    w.color = (static_cast<std::uint32_t>(p.color.r) << 24) |
              (static_cast<std::uint32_t>(p.color.g) << 16) |
              (static_cast<std::uint32_t>(p.color.b) << 8) |
              static_cast<std::uint32_t>(p.color.a);
    w.death = p.death;
    out.waypoints.push_back(std::move(w));
  }
  return out;
}

bool App::saveCurrentWorld() {
  if (!world_ || !player_ || !worldOnDisk_) return false;
  const save::WorldSave data = buildSave();
  std::string error;
  if (!save::write(data, &error)) {
    log::error("save failed: %s", error.c_str());
    // Loud, because a save that quietly did not happen is the one failure the
    // player cannot recover from by trying again later.
    interface_.notify().push("Could not save: " + error, ui::Toast::Important);
    return false;
  }
  worldMeta_.savedAt = data.meta.savedAt;
  log::info("saved \"%s\" (%s)", worldMeta_.name.c_str(), worldMeta_.id.c_str());
  return true;
}

bool App::loadWorldFromDisk(const std::string& id) {
  save::WorldSave data;
  std::string error;
  if (!save::read(id, data, &error)) {
    log::error("load failed: %s", error.c_str());
    interface_.notify().push("Could not load: " + error, ui::Toast::Important);
    return false;
  }

  AppOptions options = options_;
  options.seed = data.meta.seed;
  options.haveSpawnOverride = false;
  options.startingItems.clear();
  options.startingEntities.clear();

  inventory_.clear();
  world_.reset();
  player_.reset();
  if (!startWorld(options, &data)) return false;

  worldMeta_ = data.meta;
  lastLoadedGuests_ = data.guests;
  worldOnDisk_ = true;
  interface_.notify().push("Loaded " + (worldMeta_.name.empty() ? id : worldMeta_.name));
  return true;
}

void App::leaveWorld() {
  leaveNetwork("");
  // The box goes with the world it was open over — and closing it here is what
  // gives the keyboard back, since the field holds text capture until it does.
  interface_.chat().close(&window_.input());
  interface_.chat().clear();
  netLevels_.clear();
  ui::settings().endWorld();
  ui::settings().clearVirtualFlags();
  createdCreative_ = false;
  // The bin does not survive leaving the world, which is Terraria's rule and the
  // right one: it is never written to the save, so anything still in it would
  // otherwise reappear in whatever world was opened next. Emptied rather than
  // handed back, because by here the save has already been written and the bag
  // below is about to be cleared anyway.
  trashSlot_.clear();
  audio::director().stop();  // settle the ambience beds before the menu
  entities_.clear();
  world_.reset();
  player_.reset();
  inventory_.clear();
  interface_.atlas().reset();
  stationOpen_ = false;
  worldMeta_ = save::WorldMeta{};
  worldOnDisk_ = false;
  hasSpawn_ = false;
  autosaveT_ = 0.0f;
  state_ = AppState::Menu;
  window_.setPointerCaptured(false);
}

Vec3 App::spawnPoint() const {
  if (hasSpawn_) return spawn_;
  // Nothing bound: back to where the world started you. This used to be the origin
  // column whatever was there, which is not the same place — so a world whose home
  // had been moved off a beach or a canyon put you back on it the first time you
  // died, and if the origin was a ravine lip that was a loop you could not leave.
  return worldSpawn_;
}

void App::applyPainting(int x, int y, int z, game::Painting art) {
  if (!world_) return;
  // The block has to still be a canvas: a picture chosen while somebody else mined
  // the frame out from under it would otherwise sit in the world's map forever,
  // invisible, and be written to the save.
  if (world_->getBlock(x, y, z) != world::wk().canvas) {
    interface_.notify().push("That painting is gone", ui::Toast::Important);
    return;
  }
  world_->setPainting(x, y, z, art);
  if (netHosting()) netHost_.broadcastPainting(x, y, z, art);
}

void App::respawnPlayer() {
  audio::sfx::died();

  // Off the boat first. A dead rider left in the seat is pulled back to it by the
  // boat's own tick, which would undo the respawn a frame later.
  if (player_->mount() != 0) {
    if (game::Entity* mount = entities_.byId(player_->mount())) mount->data.rider = false;
    player_->setMount(0);
  }

  const Vec3 fell = player_->pos();

  // One death waypoint, replacing the last: the Atlas is for finding your way back to
  // your things, and a pin for every death you have ever had is the opposite of that.
  if (ui::settings().flag("deathWaypoints")) {
    std::vector<ui::Waypoint>& points = interface_.atlas().waypoints();
    points.erase(std::remove_if(points.begin(), points.end(),
                                [](const ui::Waypoint& w) { return w.death; }),
                 points.end());
    ui::Waypoint w;
    w.x = static_cast<float>(jsmath::jsRound(fell.x * 10.0) / 10.0);
    w.y = static_cast<float>(jsmath::jsRound(fell.y));
    w.z = static_cast<float>(jsmath::jsRound(fell.z * 10.0) / 10.0);
    w.name = "Where you fell";
    w.color = Rgba{0xE0, 0x52, 0x52, 0xFF};
    w.death = true;
    points.push_back(std::move(w));
  }

  // Everything carried, scattered where it fell, so dying is a journey back rather
  // than a loss. A guest routes each toss through the host, which owns every drop in
  // the world — the same path Q takes.
  const auto toss = [&](game::ItemStack& s) {
    if (s.empty()) return;
    const Vec3 dir{static_cast<float>(randomUnit()) * 2.0f - 1.0f, 0.2f,
                   static_cast<float>(randomUnit()) * 2.0f - 1.0f};
    const Vec3 from{fell.x, fell.y + 0.6f, fell.z};
    throwStack(from, dir, s);
    s.clear();
  };
  for (game::ItemStack& s : inventory_.slots()) toss(s);
  for (game::ItemStack& s : inventory_.armor()) toss(s);

  const Vec3 wake = spawnPoint();
  // Synchronous, so the ground is there to be stood on before the next physics step.
  world_->primeSpawn(wake.x, wake.z);
  // Warn the host before the jump, not after. Waking at spawn moves the body
  // further in one step than any speed allows, and the host's movement check
  // answered exactly that by teleporting the guest back where it came from — which
  // is the spot they had just died on. Anyone who died more than about twelve
  // blocks from their spawn point respawned standing in their own dropped things.
  if (netGuest()) netClient_.sendWarp();
  player_->teleport(wake);
  player_->reviveFull();
  interface_.notify().push("You blacked out and woke at spawn. Your things are where you fell.");
}

void App::frame() {
  if (frameStart_ <= 0.0) frameStart_ = nowMillis();
  window_.pollEvents();
  const double dt = clock_.tick(window_.time());

  if (assets::hasOverrideDir() && shaderWatch_.due(dt)) {
    shaders_.reloadChanged();
  }

  // A parked cursor for reproducible hover captures. Fed rather than assigned, so the
  // screens cannot tell it from a real pointer.
  if (options_.haveMouseOverride) {
    window_.input().feedMouseMove(options_.mouseX, options_.mouseY, 0, 0);
  }

  // A --screen capture must not have its screen closed by the key handler or by the
  // pointer-recapture click, so the whole input path is skipped for it.
  if (options_.startScreen.empty()) handleGlobalKeys();

  // The Atlas can be lost while its own map is open. Dying scatters everything you
  // carry, and the map is exactly the screen you might be reading when something
  // reaches you — so the gate is a per-frame invariant and not merely a check on
  // the way in. This is the same shape as onContainerDenied: a screen whose reason
  // to exist has gone closes itself rather than waiting to be dismissed.
  //
  // Skipped under --screen for the reason the key handler above is: a capture must
  // not have its screen closed out from under it.
  if (options_.startScreen.empty() && state_ == AppState::Map && !hasAtlas()) {
    resumePlaying();
    interface_.notify().push("Your Atlas is gone", ui::Toast::Important);
  }

  // Chat is not a screen and it is not a pause. The reason you are typing is
  // usually something you are looking at, and a command whose effect you cannot
  // watch happen is one you have to run twice to believe — so the world carries on
  // exactly as it does for a screen open in a shared world, on an input nothing
  // ever feeds. Keeping the sources fresh here rather than in openChat is what
  // makes the completion popup notice somebody joining while the box is already up.
  const bool chatting = interface_.chat().isOpen();
  if (chatting) refreshChatSources();
  // The box took the pointer when it opened and has to give it back. Closing runs
  // inside the interface — Escape, or a line sent — so there is no single call site
  // to hang this off; the transition is the event. Without it the cursor stayed
  // free over a world that was still reading the mouse, so looking around simply
  // stopped working until you opened and closed a screen.
  if (chatWasOpen_ && !chatting && state_ == AppState::Playing) {
    window_.setPointerCaptured(true);
    // The same guard resuming from a screen uses: the click that dismissed the box
    // must not also swing at whatever is under the crosshair.
    resumeClickGuard_ = 1;
  }
  chatWasOpen_ = chatting;

  if (state_ == AppState::Playing && !chatting) {
    updatePlaying(dt);
  } else if (world_ && player_ && (multiplayer() || chatting)) {
    // A screen is open, and this world has other people in it. Theirs keeps
    // running, so ours has to as well — a paused body is a body that does not
    // fall, does not drown and cannot be hit, and the guests would be watching
    // someone stood in mid-air ignoring a zombie.
    //
    // The input is an empty one rather than the window's: the world goes on
    // without us, but nothing we type into a menu reaches the body standing in it.
    syncSettingsIfChanged();
    stepPlayer(idleInput_);
    stepWorld(dt);
  } else if (world_) {
    // Single player, screen open: the world is genuinely stopped. Block entities
    // are the one exception, and always have been — a forge does not stop smelting
    // because you are looking at it.
    world_->tickBlockEntities(static_cast<float>(dt));
  }

  // The listener pose, the underwater muffle, footsteps and the ambience beds. Run
  // outside updatePlaying because the beds keep breathing behind the pause menu —
  // they fade to 40% rather than out, which is what makes pausing feel like a pause
  // rather than a mute. `underwater_` is a frame behind here (renderWorld computes
  // it), which is a millisecond of lag on a filter whose own ramp is 120 ms.
  // The net layer is pumped every frame, not only while playing: a guest that
  // opened the pause menu is still connected, and a host whose player is in the
  // inventory screen still owes its guests snapshots.
  {
    const double netNow = clock_.simTime();
    if (netHost_.running()) {
      netHost_.update(dt, netNow);
      advertiser_.setPlayers(netHost_.guestCount() + 1, net::kMaxGuests + 1);
      advertiser_.update(dt);
    }
    if (netClient_.running()) netClient_.update(dt, netNow);
    lanListener_.update(dt);
  }

  runStartupCommands();

  audio::engine().setDucked(state_ != AppState::Playing);
  if (world_ && player_) {
    audio::DirectorContext actx;
    actx.world = world_.get();
    actx.player = player_.get();
    actx.dayFactor = sky_.dayFactor();
    actx.underwater = underwater_;
    actx.active = state_ == AppState::Playing;
    actx.entities = &entities_;
    audio::director().update(static_cast<float>(dt), actx);
  }

  syncScreen();
  interface_.update(dt, uiFrame());

  if (world_ && player_) {
    renderWorld();
    // Over a live world the pause and settings screens keep their own opaque
    // gradient, exactly as the browser's .screen rule did. The chosen picture is
    // a MENU background; it has no business dimming a world you are stood in.
    interface_.setHasBackdrop(false);
  } else {
    renderMenuScene(dt);
  }
  interface_.draw(window_, window_.input(), uiFrame());

  // Captured before the swap, while the frame is still in the back buffer.
  if (!options_.screenshotPath.empty()) {
    // The first frames can land before the compositor has settled the window to its
    // final size, and before chunk streaming has caught up, so give it a moment.
    // With `--frames n` the capture waits until the last one instead: a run that
    // was told how long to live has usually been told that because something has
    // to happen first — a world to finish streaming, or a guest to finish joining.
    const long long captureAt =
        options_.exitAfterFrames > 30 ? options_.exitAfterFrames - 1 : 30;
    if (frameIndex_ >= captureAt) {
      captureScreenshot(options_.screenshotPath);
      options_.screenshotPath.clear();
      if (options_.exitAfterFrames == 0) running_ = false;
    }
  }

  // /stop, run here rather than where it was typed. It reaches this point through
  // two routes that both forbid doing it in place: the local chat box, which is
  // inside interface_.draw and would free the world mid-frame, and a guest's line,
  // which arrives inside the host's own message loop while that loop is walking a
  // peer list closing the world would empty.
  if (pendingStop_) {
    pendingStop_ = false;
    if (world_) leaveWorld();
  }

  window_.input().endFrame();
  window_.swapBuffers();
  limitFrameRate();
  ++frameIndex_;

  if (options_.exitAfterFrames > 0 && frameIndex_ >= options_.exitAfterFrames) {
    running_ = false;
  }
}

// applySettings caches settings into playerOptions_ and the renderer, so anything
// that changes one has to say so. Watching a counter rather than wiring a callback
// into each path catches the one that would otherwise be missed: a guest being told
// the host's rules mid-session, which arrives deep in the net client and would
// leave them flying in a world that had just forbidden it.
void App::syncSettingsIfChanged() {
  if (ui::settings().revision() == settingsRevision_) return;
  settingsRevision_ = ui::settings().revision();
  applySettings();
}

// The body, substepped: a long frame is split so a fast one cannot pass through a
// block. Everything else takes the whole dt.
//
// No dt argument, unlike everything around it — the substep count and size both
// come from the clock, which is where the clamp that makes them safe lives.
//
// --freeze skips it entirely, which is what makes `--at` mean what it says. The
// flag has always claimed to be "an exact camera placement", and for anything
// above the ground it was not: gravity ran for the two hundred frames the world
// needs to stream in, and the capture happened wherever the player landed.
void App::stepPlayer(Input& in) {
  if (options_.freezePlayer) return;
  // Once, before the substeps: this reads a key edge, and the substep loop would
  // see the same press several times over.
  // Once, before the substeps: this reads a key edge, and the substep loop would
  // see the same press several times over.
  player_->tryToggleFlight(in, playerOptions_, clock_.simTime());
  double step = 0;
  const int steps = clock_.substeps(step);
  for (int i = 0; i < steps; ++i) {
    player_->update(static_cast<float>(step), in, *world_, playerOptions_, clock_.simTime());
  }
}

void App::updatePlaying(double dt) {
  Input& in = window_.input();

  syncSettingsIfChanged();

  // Mouse look at render rate, not simulation rate, so aim latency stays low.
  if (window_.pointerCaptured()) {
    double dx = 0, dy = 0;
    in.takeMouse(dx, dy);
    player_->look(dx, dy, playerOptions_);
  }

  stepPlayer(in);

  handleHotbarInput(in, dt);
  // Interaction runs at render rate, on the same eye and forward vector the
  // crosshair is drawn from, so what you break is always what you were aiming at.
  // The click that re-captures the pointer is swallowed: clicking the window to
  // focus it should not also mine whatever happens to be under the crosshair.
  if (resumeClickGuard_ > 0) {
    --resumeClickGuard_;
    interact_.reset();
  } else {
    interact_.update(static_cast<float>(dt), in, *player_, *world_, inventory_, interactHooks_);
  }

  render::Viewmodel& vm = renderer_.viewmodel();
  vm.setItem(inventory_.selectedSlot().key);
  if (interact_.swung()) vm.swing();
  vm.update(static_cast<float>(dt), player_->bobPhase(), player_->bobMagnitude());

  stepWorld(dt);
}

// Everything that happens whether or not anyone is looking: the weather of the
// place, as against what the player is doing to it.
//
// Split out of updatePlaying because of what "paused" has to mean once somebody
// else is in the world. Opening the inventory used to stop the whole simulation,
// which is exactly right on your own: the game waits for you. In a shared world it
// is not yours to stop, and stopping it stopped it for everyone — the host glanced
// at a chest and every mob froze mid-stride, every dropped item hung in the air,
// every furnace stopped, and the guests watched a still photograph until the
// screen closed. That is the whole of "the game still pauses per player".
void App::stepWorld(double dt) {
  const float fdt = static_cast<float>(dt);

  world_->tickBlockEntities(fdt);
  // Water runs on its own accumulator at roughly six batches a second, and only
  // here: a multiplayer guest must not simulate it, because the host's edits are
  // what it will be told about (js/main.js:669 gates it the same way).
  if (!netGuest()) {
    world_->tickWater(fdt);
    // Support and falling blocks, on the same host-only rule and for the same
    // reason: a guest is told what changed, it does not decide.
    world_->tickBlockUpdates(fdt);
    // Crops, on that same rule. A guest that grew its own would watch a field
    // ripen that the host's world had not, and the next edit from the host would
    // snap it back.
    world_->tickCrops(fdt);
  }

  refreshEntityContext();
  {
    render::CpuScope cs(renderer_.profiler(), render::CpuPhase::Entities);
    entities_.tick(fdt, entityContext_);
  }

  // Getting out of a boat is decided inside the boat's own update hook, which
  // knows nothing about the network and should not: it is Shift, and it is the
  // same key whoever presses it. So the standing-up is noticed here instead, by
  // watching the one thing it changes. Without this the host keeps the hull
  // reserved for a guest who walked off it — unbreakable, unmountable by anyone
  // else, and pinned to a pose that is no longer sitting in it.
  if (netGuest() && mountedNetId_ != 0 && player_->mount() == 0) {
    netClient_.sendBoatMount(mountedNetId_, false);
    mountedNetId_ = 0;
  }

  // Passive grazers on nearby grass in daylight, zombies anywhere dark enough,
  // both capped and both on a four-second cadence. The host owns every entity in
  // a multiplayer world, so a guest spawns nothing at all.
  if (netGuest()) {
    grazerSpawnT_ = zombieSpawnT_ = 0.0f;
  } else {
  grazerSpawnT_ += fdt;
  if (grazerSpawnT_ >= 4.0f) {
    grazerSpawnT_ = 0.0f;
    const float day = sky_.dayFactor();
    const float px = player_->pos().x, pz = player_->pos().z;
    entities_.trySpawnGrazer(*world_, game::EntityType::Sheep, px, pz, day,
                             game::EntityManager::kMaxSheep);
    entities_.trySpawnGrazer(*world_, game::EntityType::Pig, px, pz, day,
                             game::EntityManager::kMaxPig);
    entities_.trySpawnGrazer(*world_, game::EntityType::Cow, px, pz, day,
                             game::EntityManager::kMaxCow);
  }
  zombieSpawnT_ += fdt;
  if (zombieSpawnT_ >= 4.0f) {
    zombieSpawnT_ = 0.0f;
    if (ui::settings().flag("monsters")) {
      const float day = sky_.dayFactor();
      entities_.trySpawnZombie(*world_, player_->pos(), day);
      // Altars share the timer but not the cap, and are equally subject to the
      // setting: turning monsters off has to turn off every source of them.
      entities_.tickEvilAltars(*world_, player_->pos(), day);
    }
  }
  }

  // Periodic autosave, so a crash or an accidental close costs at most fifteen
  // minutes. The subtraction rather than a reset is deliberate and is one of the
  // ~10 accumulator fixes the plan called for: `= 0` throws away the overshoot and
  // makes the real period the sum of the interval and the frame it landed on.
  // Never for a guest: the world belongs to the host, and `worldOnDisk_` is
  // already false for one — this is belt and braces on the single most damaging
  // thing a bug here could do, which is overwrite the player's own save with
  // somebody else's world.
  if (worldOnDisk_ && !netGuest()) {
    autosaveT_ += fdt;
    if (autosaveT_ >= kAutosaveSeconds) {
      autosaveT_ -= kAutosaveSeconds;
      if (saveCurrentWorld()) interface_.notify().push("Autosaved");
    }
  }

  // What this milestone is actually measured on. Frame time is the wrong ruler
  // here — vsync flattens it and the dt clamp caps it at 50 ms — so what gets
  // recorded is the time chunk streaming spends on THIS thread, which is precisely
  // what moving generation, lighting and meshing to workers was meant to remove.
  {
    const double t0 = nowMillis();
    world_->update(player_->pos().x, player_->pos().z);
    const double ms = nowMillis() - t0;
    renderer_.profiler().addCpu(render::CpuPhase::WorldUpdate, ms);
    streamTotal_ += ms;
    ++streamFrames_;
    if (ms > streamWorst_) {
      streamWorst_ = ms;
      streamWorstFrame_ = frameIndex_;
    }
  }
  sky_.update(fdt);

  // ~0.9 s to fade. Long enough to notice after the fact — the whole point is the
  // hit you did not see land — and short enough not to sit over the next fight.
  if (hurtFlash_ > 0.0f) hurtFlash_ = std::max(0.0f, hurtFlash_ - fdt * 1.1f);

  // Last, after everything that could have landed the blow: js/main.js:737 polls it
  // in the same place, and for the same reason — a respawn spawns drops and moves the
  // player, neither of which is safe in the middle of a tick.
  if (player_->dead()) respawnPlayer();
}

// The world and player are torn down and rebuilt whenever a world is entered or
// left, so the context is refilled rather than wired once.
void App::refreshEntityContext() {
  entityContext_.world = world_.get();
  entityContext_.player = player_.get();
  entityContext_.inventory = &inventory_;
  entityContext_.entities = &entities_;
  entityContext_.sky = &sky_;
  // The rideable boat is the only thing that reads this, and behind an open screen
  // it must read nothing: the world goes on without us, but a key pressed at a
  // menu does not steer a boat.
  entityContext_.input = state_ == AppState::Playing ? &window_.input() : &idleInput_;
  // Whether anyone else is standing in this world. Today it decides whether a
  // mined item is vacuumed up from any distance or has to be walked over; see
  // EntityContext. True for the host only — a guest owns no entities at all, so
  // there is nothing on that side for the rule to apply to.
  entityContext_.sharedWorld = netHosting();
  entityContext_.playerOptions = &playerOptions_;
  if (!entityContext_.notify) {
    entityContext_.notify = [this](const std::string& message) {
      interface_.notify().push(message, ui::Toast::Important);
    };
  }
}

void App::handleHotbarInput(Input& in, double dt) {
  // 1..9 select a hotbar slot directly.
  static constexpr Key kDigits[game::kHotbarSlots] = {Key::Digit1, Key::Digit2, Key::Digit3,
                                                      Key::Digit4, Key::Digit5, Key::Digit6,
                                                      Key::Digit7, Key::Digit8, Key::Digit9};
  for (int i = 0; i < game::kHotbarSlots; ++i) {
    if (in.pressed(kDigits[i])) inventory_.setSelected(i);
  }

  // The wheel cycles, wrapping in both directions. The platform layer already
  // normalises GLFW's "up is positive" to the browser's deltaY sign, so scrolling
  // down advances the slot exactly as it did in the web build.
  const double wheel = in.takeWheel();
  if (wheel != 0.0) inventory_.cycleSelected(wheel > 0 ? 1 : -1);

  // Q drops one; with control held, the whole stack. Held down, it repeats and
  // speeds up, the same run the inventory screen does over a hovered slot.
  //
  // Control rather than shift out here, where the inventory uses shift: shift is
  // crouch in the world, and a gesture that both drops your stack and steps you
  // off a ledge is not one to offer.
  if (!in.down(Key::Q)) {
    dropRun_.stop();
  } else {
    // The same cadence the inventory screen uses, from the same object, so the two
    // cannot drift apart.
    const int n = dropRun_.tick(/*sameTarget=*/!in.pressed(Key::Q), dt);
    for (int i = 0; i < n; ++i) {
      game::ItemStack& s = inventory_.selectedSlot();
      if (s.empty() || !world_ || !player_) break;
      // A copy of the slot with only the count changed, so the thrown item is the
      // held item in every other respect — its wear, and its colour.
      game::ItemStack out = s;
      if (!in.ctrl()) out.count = 1;
      if (in.ctrl()) s.clear();
      else inventory_.consumeSelected();
      tossStack(out);
    }
  }
}

// One throw, out of the player's face along their aim. Shared by Q in the world
// and by Q over a slot in the inventory screen. The scatter on death does not come
// through here: it throws from where the body fell, in a random direction, and
// borrowing this would have the dropped items appear wherever the corpse was
// looking. It shares the crossing below with it instead.
void App::tossStack(const game::ItemStack& stack) {
  if (!player_) return;
  const Vec3 eye = player_->eye();
  const Vec3 dir = player_->forward();
  const Vec3 from{eye.x + dir.x * 0.4f, eye.y - 0.2f, eye.z + dir.z * 0.4f};
  throwStack(from, dir, stack);
  audio::sfx::toss();
}

// The one place a stack becomes a thrown item. A guest owns no entities, so its
// throw is a request to the host; a host makes the item itself.
//
// The whole stack goes in, and every field of it comes out the other side. This
// used to take key, count and dura, which is why a dyed item thrown on the floor
// was a plain one when it was picked up again: nothing here was wrong, the colour
// simply never arrived, and an undyed item is what an undyed item looks like.
void App::throwStack(const Vec3& from, const Vec3& dir, const game::ItemStack& stack) {
  if (!world_ || !player_ || stack.empty()) return;
  if (netGuest()) {
    netClient_.sendToss(from, dir, stack);
  } else {
    entities_.spawnTossed(from, dir, stack.key, stack.count, stack.dura, stack.tint);
  }
}

void App::renderWorld() {
  // The eye used for rendering carries the head bob; the one used for aiming does
  // not, so the crosshair never drifts while walking.
  const Vec3 eye = player_->eye() + player_->viewOffset();
  camera_.update(eye, player_->yaw(), player_->pitch());

  // Submerged post-effect, eased rather than snapped so breaking the surface is a
  // transition instead of a flicker.
  const float target = player_->headInWater(*world_) ? 1.0f : 0.0f;
  underwater_ += (target - underwater_) * std::min(1.0f, static_cast<float>(clock_.dt()) * 9.0f);

  // The same shape, for the same reason: a snap would flicker every time the eye
  // crossed a block face while flying through a hillside. One lookup at the eye,
  // and only while no-clip is actually on — off, this costs a bool and nothing.
  float xrayTarget = 0.0f;
  if (playerOptions_.noClip) {
    const Vec3 e = player_->eye();
    const world::BlockId at = world_->getBlock(static_cast<int>(std::floor(e.x)),
                                               static_cast<int>(std::floor(e.y)),
                                               static_cast<int>(std::floor(e.z)));
    if (world::blocks().solid(at)) xrayTarget = 1.0f;
  }
  xray_ += (xrayTarget - xray_) * std::min(1.0f, static_cast<float>(clock_.dt()) * 9.0f);
  renderer_.setXray(xray_);

  // The wireframe overlays go with the HUD, for the same reason the block selection
  // does: they are diagnostics rather than part of the world, and F1 is pressed to
  // get exactly this sort of thing out of a screenshot.
  render::Renderer::DebugOverlays overlays;
  if (options_.debugLines) {
    // The capture flag turns all three on and skips the gate, the same way
    // --debug-view outranks the settings row.
    overlays.paths = overlays.chunks = overlays.boxes = true;
  } else if (interface_.hudVisible() && ui::settings().available("debugPaths")) {
    overlays.paths = ui::settings().flag("debugPaths");
    overlays.chunks = ui::settings().flag("debugChunks");
    overlays.boxes = ui::settings().flag("debugBoxes");
  }
  renderer_.setDebugOverlays(overlays);

  render::Renderer::Selection selection {interact_.selectionX(), interact_.selectionY(),
                                         interact_.selectionZ()};
  // The selection outline goes with the HUD: it is an aiming affordance, not part
  // of the world, and a wireframe box in the middle of a screenshot is exactly
  // what F1 is being pressed to get rid of.
  const bool showSelection = interact_.hasSelection() && interface_.hudVisible();
  {
    // Everything in render() that is not one of the bracketed GPU passes: the
    // uniform setting, the state changes and the driver's own submission cost.
    render::CpuScope cs(renderer_.profiler(), render::CpuPhase::Submit);
    renderer_.render(*world_, camera_, sky_, window_.width(), window_.height(),
                     showSelection ? &selection : nullptr,
                     inventory_.selectedSlot().key, inventory_.selectedSlot().tint,
                     underwater_, clock_.simTime());
  }
  renderer_.profiler().endFrame(clock_.rawDt() * 1000.0);

  if (renderer_.profiler().enabled()) {
    static Interval perfLog {1.0};
    if (perfLog.due(clock_.dt())) log::info("%s", renderer_.profiler().summaryLine());
  }

  static Interval statsLog {2.0};
  if (statsLog.due(clock_.dt())) {
    const game::ItemStack& held = inventory_.selectedSlot();
    // Live mob counts by type, because "are animals still spawning" is otherwise
    // a question you can only answer by walking around looking for them.
    int nSheep = 0, nPig = 0, nCow = 0, nZombie = 0, nDrop = 0;
    for (const game::Entity& e : entities_.all()) {
      if (e.dead) continue;
      switch (e.type) {
        case game::EntityType::Sheep: ++nSheep; break;
        case game::EntityType::Pig: ++nPig; break;
        case game::EntityType::Cow: ++nCow; break;
        case game::EntityType::Zombie: ++nZombie; break;
        case game::EntityType::Drop: ++nDrop; break;
        default: break;
      }
    }
    log::debug("%.0f fps (%.1f ms) | %d/%zu chunks | %.2fM draw / %.2fM shadow / %.2fM loaded "
               "tris | %zu pending | pos %.1f %.1f %.1f | %s day %.2f | "
               "mobs %ds/%dp/%dc/%dz %dd | held %s x%d",
               clock_.fps(), clock_.frameMs(), renderer_.drawnChunks(),
               world_->loadedChunkCount(),
               renderer_.drawnTris() / 1e6, renderer_.shadowTris() / 1e6,
               renderer_.loadedTris() / 1e6,
               world_->pendingCount(), player_->pos().x, player_->pos().y, player_->pos().z,
               sky_.clockString().c_str(), sky_.dayFactor(),
               nSheep, nPig, nCow, nZombie, nDrop,
               held.empty() ? "-" : held.key.c_str(), held.count);
  }
}

// js/main.js:570-630. The order matters: Escape unwinds the innermost screen first, and
// E and R toggle rather than stack, so pressing E twice returns you to the world instead
// of leaving two overlays open.
void App::handleGlobalKeys() {
  Input& in = window_.input();

  // A focused text field owns the keyboard: typing "n" into a world name must not open
  // the Atlas.
  if (in.capturingText()) {
    if (in.pressed(Key::Escape)) interface_.menu().blurFields(&in);
    if (in.pressed(Key::Enter) && in.alt()) toggleFullscreen();
    return;
  }

  // T talks, slash commands. The slash key opens the box with the slash already
  // in it — the character itself is lost, because Input::feedChar drops anything
  // typed while text capture is off and capture only begins on the line below.
  // Prefilling is what makes that invisible rather than a swallowed keystroke.
  if (state_ == AppState::Playing) {
    if (in.pressed(Key::T)) openChat(false);
    if (in.pressed(Key::Slash)) openChat(true);
  }

  if (in.pressed(Key::Escape)) {
    switch (state_) {
      case AppState::Playing:
        state_ = AppState::Paused;
        window_.setPointerCaptured(false);
        break;
      case AppState::Menu: running_ = false; break;
      default: closeCurrentScreen(); break;
    }
  }

  if (in.pressed(Key::E)) {
    if (state_ == AppState::Playing) {
      interface_.openStation(world::Station::None);
      state_ = AppState::Inventory;
      window_.setPointerCaptured(false);
    } else if (closesWithE(state_)) {
      // Through closeCurrentScreen rather than resumePlaying, because the recipe
      // book has somewhere to go back TO. Escape has always run this; E listed the
      // states it knew about instead, and the palette — which arrived as its own
      // AppState after that list was written — was not on it. The key did nothing,
      // in the one screen where the hands are full of an item and Escape is the
      // least likely thing to be reached for.
      closeCurrentScreen();
    }
  }

  // H, not R: R is wanted for a Vintage Story style handbook later, and the recipe
  // book is the closest thing this game has to one.
  if (in.pressed(Key::H)) toggleRecipeBook();

  // M for the Atlas. It was N because the browser build could not have M — the
  // native port has no such constraint, and M is where every player's hand goes.
  //
  // The Atlas ITEM is what unlocks cartography, and this is one of the two places
  // that has to be true. The other is below, in frame(): the minimap and the
  // in-world waypoint tags were gated here from the start, but the fullscreen map
  // was not, so the whole progression — papyrus, paper, leather, azurite — could
  // be skipped by pressing a key.
  if (in.pressed(Key::M)) {
    if (state_ == AppState::Playing) {
      if (hasAtlas()) {
        state_ = AppState::Map;
        window_.setPointerCaptured(false);
      } else {
        // Named, not silent. A key that does nothing reads as a broken key; a key
        // that says what it wants reads as something to go and craft.
        interface_.notify().push("You need an Atlas to chart the world", ui::Toast::Important);
      }
    } else if (state_ == AppState::Map) {
      resumePlaying();
    }
  }

  if (in.pressed(Key::F1)) interface_.setHudVisible(!interface_.hudVisible());
  if (in.pressed(Key::F3)) interface_.hud().toggleDebug();

  if (in.pressed(Key::F2)) {
    audio::sfx::shutter();
    const std::string path = paths::join(paths::screenshotsDir(), nextScreenshotName());
    if (captureScreenshot(path)) interface_.notify().push("Screenshot saved");
    else interface_.notify().push("Screenshot failed", ui::Toast::Important);
  }

  // Clicking the window re-captures the pointer, as the web build did on canvas
  // mousedown after pointer lock was lost. The pause menu's own buttons are handled by
  // the interface, so only a click on empty space resumes.
  if (state_ == AppState::Paused && in.clicked(MouseButton::Left) &&
      !interface_.pausePointerOverButton()) {
    resumePlaying();
  }

  // Alt+Enter is the native convention; the web build used the browser's own control.
  if (in.pressed(Key::Enter) && in.alt()) toggleFullscreen();
}

// Through the store rather than straight at the window, so the settings row and the
// shortcut can never disagree about which one is telling the truth — and so the choice
// is still there on the next launch.
// H in the world or in a station screen, and the button on every crafting screen.
// Opening from a station remembers it, so closing the book puts the workbench back
// rather than dropping you into the world with the grid still full.
void App::toggleRecipeBook() {
  if (state_ == AppState::Playing || state_ == AppState::Inventory) {
    recipeReturn_ = state_;
    // WHICH inventory, not just "an inventory". Leaving Screen::Inventory runs
    // Interface::closeInventory (ui/interface.cpp:81), which empties the crafting
    // grid back into the bag and drops the station — so coming back has to open a
    // station again or the screen renders with a closed InventoryUI behind it:
    // no panel, no grid, and a free cursor over a world that is not accepting
    // input. That was the bug; the Back button was reached and did its job.
    // Read back from the InventoryUI rather than tracked separately, so there is
    // no second copy of "which station is open" to fall out of step with the one
    // the screen is actually drawing. Still open at this point: setScreen has not
    // run yet, so mode() is live.
    switch (interface_.inventory().mode()) {
      case ui::InventoryMode::Workbench: recipeStation_ = world::Station::Workbench; break;
      case ui::InventoryMode::Forge: recipeStation_ = world::Station::Forge; break;
      case ui::InventoryMode::Chest: recipeStation_ = world::Station::Chest; break;
      // The kitchens, for exactly the reason spelled out above: without these the
      // default arm sends you back to a plain inventory and the station you were
      // standing at is simply gone.
      case ui::InventoryMode::Cutting: recipeStation_ = world::Station::Cutting; break;
      case ui::InventoryMode::Stove: recipeStation_ = world::Station::Stove; break;
      case ui::InventoryMode::Pot: recipeStation_ = world::Station::Pot; break;
      default: recipeStation_ = world::Station::None; break;
    }
    state_ = AppState::RecipeBook;
    window_.setPointerCaptured(false);
  } else if (state_ == AppState::RecipeBook) {
    state_ = recipeReturn_;
    if (state_ == AppState::Playing) {
      resumePlaying();
    } else if (state_ == AppState::Inventory) {
      interface_.openStation(recipeStation_);
    }
  }
}

void App::toggleFullscreen() {
  ui::settings().setFlag("fullscreen", !window_.fullscreen());
  applySettings();
}

bool App::applyStartScreen(const std::string& name) {
  struct Entry {
    const char* name;
    AppState state;
    ui::MenuPage page;
    world::Station station;
    bool isStation;
  };
  static constexpr Entry kEntries[] = {
      {"menu", AppState::Menu, ui::MenuPage::Main, world::Station::None, false},
      {"worlds", AppState::Menu, ui::MenuPage::Worlds, world::Station::None, false},
      {"newworld", AppState::Menu, ui::MenuPage::NewWorld, world::Station::None, false},
      {"about", AppState::Menu, ui::MenuPage::About, world::Station::None, false},
      {"join", AppState::Menu, ui::MenuPage::Join, world::Station::None, false},
      // Reachable only by clicking Update on an old world, which no capture can do.
      // It opens with no world named, which the page already words for.
      {"upgrade", AppState::Menu, ui::MenuPage::WorldUpgrade, world::Station::None, false},
      {"pause", AppState::Paused, ui::MenuPage::Main, world::Station::None, false},
      {"settings", AppState::Settings, ui::MenuPage::Main, world::Station::None, false},
      {"inventory", AppState::Inventory, ui::MenuPage::Main, world::Station::None, true},
      {"workbench", AppState::Inventory, ui::MenuPage::Main, world::Station::Workbench, true},
      {"forge", AppState::Inventory, ui::MenuPage::Main, world::Station::Forge, true},
      {"chest", AppState::Inventory, ui::MenuPage::Main, world::Station::Chest, true},
      {"cutting", AppState::Inventory, ui::MenuPage::Main, world::Station::Cutting, true},
      {"stove", AppState::Inventory, ui::MenuPage::Main, world::Station::Stove, true},
      {"pot", AppState::Inventory, ui::MenuPage::Main, world::Station::Pot, true},
      {"recipes", AppState::RecipeBook, ui::MenuPage::Main, world::Station::None, false},
      {"map", AppState::Map, ui::MenuPage::Main, world::Station::None, false},
      {"gallery", AppState::Gallery, ui::MenuPage::Main, world::Station::None, false},
      {"packs", AppState::Packs, ui::MenuPage::Main, world::Station::None, false},
      {"palette", AppState::Palette, ui::MenuPage::Main, world::Station::None, false},
  };
  // Chat is not in the table because it is not a Screen at all — it draws over a
  // live world and App keeps that world running underneath it. It is here for
  // exactly the reason --screen exists in the first place: a headless capture
  // cannot press T. "chat" is the box open and empty, "chat-suggest" is it
  // halfway through a command with the completion list up, which is the half worth
  // looking at.
  if (name == "chat" || name == "chat-suggest") {
    if (!world_) {
      log::error("--screen %s needs a world; add --seed, --world or --new-world",
                 name.c_str());
      return false;
    }
    state_ = AppState::Playing;
    syncScreen();
    openChat(name == "chat-suggest");
    if (name == "chat-suggest") interface_.chat().setTyped("/give sto");
    return true;
  }
  // The bed is not in the table because it is the one screen that needs seeding
  // rather than merely opening: the wheel has to be told the hour, and whether the
  // sleep button is live is a property of the world rather than of the screen.
  // "bed" captures the usable state and "bed-early" the refusal.
  if (name == "bed" || name == "bed-early") {
    sky_.setHoursAwake(name == "bed" ? render::Sky::kRestedHours : 1.5f);
    interface_.timeWheel().open(sky_.time);
    state_ = AppState::TimeWheel;
    syncScreen();
    window_.setPointerCaptured(false);
    return true;
  }
  for (const Entry& e : kEntries) {
    if (name != e.name) continue;
    state_ = e.state;
    syncScreen();
    if (e.state == AppState::Menu) interface_.menu().setPage(e.page);
    if (e.state == AppState::Palette) {
      // Seeded for the same reason the kitchens are: an empty wheel over an empty
      // slot shows the widget and not the feature.
      paletteSlot_ = game::ItemStack {"wool", 8, -1};
      inventory_.give("glass", 12);
      inventory_.give("dyed_chest_ferralite", 1);
      inventory_.give("dye_blue", 3);
      inventory_.give("dye_red", 2);
      paletteWorldFavourites_ = {0xd23a34u, 0x4a6fe0u, 0x4fae53u};
      paletteGlobalFavourites_ = {0xf2c53au, 0x9a5ac2u};
      interface_.palette().open();
      interface_.openPalette();
    }
    if (e.isStation) {
      // Every station screen needs a block entity to show. A scratch one lives here
      // rather than in the world, because the point is to capture the layout, not to
      // place a block.
      //
      // The kitchens are SEEDED with plausible contents. The forge and chest
      // captures show bare grids, which is why nobody can tell from them what those
      // screens are for — a pot with vegetables and a bowl in it explains itself.
      bool scratch = true;
      switch (e.station) {
        case world::Station::Forge: scratchStation_ = game::makeForge(); break;
        case world::Station::Chest: scratchStation_ = game::makeChest(); break;
        case world::Station::Cutting:
          scratchStation_ = game::makeCutting();
          scratchStation_.input = game::ItemStack {"beef_raw", 1, -1};
          break;
        case world::Station::Stove:
          scratchStation_ = game::makeStove();
          scratchStation_.slots[0] = game::ItemStack {"flour", 2, -1};
          scratchStation_.slots[1] = game::ItemStack {"carrot", 2, -1};
          scratchStation_.fuel = game::ItemStack {"embercoal", 8, -1};
          scratchStation_.output = game::ItemStack {"veg_pie", 1, -1};
          break;
        case world::Station::Pot:
          scratchStation_ = game::makePot();
          scratchStation_.slots[0] = game::ItemStack {"carrot", 2, -1};
          scratchStation_.slots[1] = game::ItemStack {"onion", 1, -1};
          scratchStation_.slots[2] = game::ItemStack {"tomato", 1, -1};
          scratchStation_.slots[3] = game::ItemStack {"garlic", 1, -1};
          scratchStation_.container = game::ItemStack {"bowl", 4, -1};
          scratchStation_.fuel = game::ItemStack {"embercoal", 8, -1};
          break;
        default: scratch = false; break;
      }
      if (scratch) interface_.callbacks.currentStation = [this] { return &scratchStation_; };
      interface_.openStation(e.station);
    }
    window_.setPointerCaptured(false);
    return true;
  }
  log::warn("--screen: unknown screen \"%s\"", name.c_str());
  return false;
}

// data/screenshots/hollowreach-YYYYMMDD-HHMMSS.png, which sorts chronologically and
// needs no index file.
std::string App::nextScreenshotName() const {
  const std::time_t now = std::time(nullptr);
  std::tm tm {};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "hollowreach-%04d%02d%02d-%02d%02d%02d.png",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buffer;
}

// The chosen screenshot, uploaded once and kept until the choice changes. Reloading
// a 2 MB PNG every frame for a static backdrop would be absurd, and the menu is the
// one screen with no frame budget pressure to hide it behind.
void App::refreshMenuBackground() {
  const std::string& want = ui::settings().text("menuBackground");
  if (want == menuBgPath_) return;
  menuBgPath_ = want;
  if (menuBgTex_) {
    glDeleteTextures(1, &menuBgTex_);
    menuBgTex_ = 0;
  }
  if (menuBgPath_.empty()) return;

  Image image;
  if (!Image::loadPng(menuBgPath_, image)) {
    // Deleted from under us, or never readable. Fall back to the gradient and stop
    // pointing at it, so this is not retried on every menu frame.
    log::warn("menu background: could not read %s", menuBgPath_.c_str());
    ui::settings().setText("menuBackground", "");
    menuBgPath_.clear();
    return;
  }
  glGenTextures(1, &menuBgTex_);
  glBindTexture(GL_TEXTURE_2D, menuBgTex_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width(), image.height(), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, image.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  menuBgAspect_ = image.height() > 0
                      ? static_cast<float>(image.width()) / static_cast<float>(image.height())
                      : 1.0f;
}

// Every exit from refreshMenuBackground has to leave the screens agreeing about
// whether there is a picture behind them, including the failure path that clears
// the setting — so the flag is pushed here rather than at the one call site that
// happened to load successfully.
void App::syncMenuBackdrop() {
  refreshMenuBackground();
  interface_.setHasBackdrop(menuBgTex_ != 0);
}

// Sleeps out whatever is left of the frame's budget after the swap.
//
// Sleep for most of it and spin for the last half-millisecond: a bare sleep is
// only accurate to the OS scheduler's tick — around 1-15 ms on Windows — which at
// 144 fps is most of a frame and would make the cap land anywhere. The spin is
// bounded and only ever runs at the very end, so it costs a fraction of a core
// rather than the busy-wait a pure spin would be.
//
// The next frame's dt then measures the whole wall-clock interval including this
// wait, which is what makes the limit show up honestly in the fps readout instead
// of being invisible.
void App::limitFrameRate() {
  if (frameLimit_ <= 0) {
    frameStart_ = nowMillis();
    return;
  }
  const double targetMs = 1000.0 / static_cast<double>(frameLimit_);
  const double deadline = frameStart_ + targetMs;
  for (;;) {
    const double left = deadline - nowMillis();
    if (left <= 0.0) break;
    if (left > 0.5) {
      std::this_thread::sleep_for(
          std::chrono::microseconds(static_cast<long long>((left - 0.5) * 1000.0)));
    }
  }
  // ADVANCE the deadline rather than restarting from now. Resetting loses the
  // fraction of a millisecond the sleep overshot by, every frame, and that
  // compounds: a 120 cap measured 116 with a reset and lands on 120 with this.
  // Resynchronised when a frame has genuinely blown the budget, so a stall does
  // not leave the limiter trying to claw back time by running fast.
  frameStart_ += targetMs;
  const double now = nowMillis();
  if (now - frameStart_ > targetMs) frameStart_ = now;
}

void App::renderMenuScene(double /*dt*/) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, window_.width(), window_.height());
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);

  syncMenuBackdrop();
  if (menuBgTex_ != 0) {
    // Cover, not stretch: a 16:9 capture on a 16:10 window should crop, not squash.
    // The interface draws its own scrim over this, so the picture stays a backdrop
    // rather than competing with the buttons.
    interface_.drawFullscreenImage(window_, menuBgTex_, menuBgAspect_);
    return;
  }

  menuBackdrop_->use();
  menuBackdrop_->set("uAspect", window_.aspect());
  screenQuad_.draw();
}

bool App::captureScreenshot(const std::string& path) {
  const int w = window_.width();
  const int h = window_.height();
  if (w <= 0 || h <= 0) return false;

  Image shot(w, h);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glReadBuffer(GL_BACK);
  // Rows are tightly packed 4-byte RGBA, so alignment 4 already holds; set it
  // explicitly because an earlier pass may have changed it for a 3-byte upload.
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, shot.data());

  // GL's origin is bottom-left; PNG's is top-left.
  Image flipped(w, h);
  for (int y = 0; y < h; ++y) {
    flipped.blitRegion(shot, 0, h - 1 - y, w, 1, 0, y, /*skipTransparent=*/false);
  }
  // The window has no alpha channel, but readback still reports one, and a 0-alpha
  // PNG would look empty in most viewers.
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      Rgba c = flipped.get(x, y);
      c.a = 255;
      flipped.set(x, y, c);
    }
  }

  if (!flipped.writePng(path)) return false;
  log::info("screenshot: %s (%dx%d)", path.c_str(), w, h);
  return true;
}

}  // namespace hr
