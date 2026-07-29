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
#include "core/assets.h"
#include "core/jobs.h"
#include "core/jsmath.h"
#include "core/log.h"
#include "core/prng.h"
#include "game/entities/types.h"
#include "platform/paths.h"
#include "resource/image.h"
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

  if (!renderer_.init(shaders_, &atlas_)) {
    log::error("Renderer initialisation failed; see the log above.");
    return 1;
  }

  ui::settings().load(paths::settingsFile());

  // The web build could not open an AudioContext until the player clicked something,
  // so the whole engine had an "unlocked" state and every call no-opped until then.
  // A native process has no such rule: the device opens at startup and stays open.
  // A machine with no output device is not an error — every audio entry point
  // no-ops when the engine is not running, which is the same contract.
  audio::engine().start();

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
  camera_.setProjection(window_.aspect(), 70.0f);

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
  }
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

// Every setting, pushed into whichever subsystem owns it. This is the whole of "all
// settings live-apply": the settings screen calls back here on every change, and
// nothing is read from the store anywhere else per frame.
void App::applySettings() {
  ui::SettingsStore& s = ui::settings();

  render::QualitySettings q = qualityPreset(s.text("graphicsQuality"));
  // The effect toggles gate features off within the selected tier, exactly as the web
  // build did — a zeroed sample count is how a pass is skipped.
  if (!s.flag("ambientOcclusion")) q.ssaoSamples = 0;
  if (!s.flag("godRays")) q.godrays = false;
  if (!s.flag("waterReflections")) q.ssrSteps = 0;
  if (!s.flag("castShadows")) q.shadowSize = 0;
  if (!s.flag("clouds")) q.cloudSteps = 0;
  q.cloudShadows = s.flag("cloudShadows") ? 1.0f : 0.0f;
  renderer_.setQuality(q);

  const float fov = static_cast<float>(s.number("fov"));
  camera_.setProjection(window_.aspect(), fov);

  if (world_) world_->setRenderDistance(static_cast<int>(s.number("renderDistance")));

  // js/game/player.js:50 — the stored 1..30 maps onto the same 0.0002 multiplier the
  // web build used against the browser's already-accelerated movementX.
  playerOptions_.sensitivity = static_cast<float>(s.number("sensitivity")) * 0.0002f;
  playerOptions_.invertY = s.flag("invertY");
  playerOptions_.flightAllowed = s.flag("flight");
  playerOptions_.hungerEnabled = s.flag("hunger");
  playerOptions_.fallDamageEnabled = s.flag("fallDamage");
  playerOptions_.stepHeight = s.flag("highStep") ? 1.0f : game::playerConst::kStep;

  window_.setRawMouseMotion(s.flag("rawMouse"));
  interface_.setUiScale(static_cast<float>(s.number("uiScale")) / 100.0f);

  // Guarded because setFullscreen is a real mode switch: calling it with the state it
  // is already in would still tear the window down and put it back, and this runs on
  // every change to any of the twenty-six rows.
  if (window_.fullscreen() != s.flag("fullscreen")) window_.setFullscreen(s.flag("fullscreen"));

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
  hooks.notify = [this](const std::string& message) { interface_.notify().push(message); };
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
  hooks.onSleep = [this] {
    // Alone, a bed is just a bed. In company it is a vote, and the host is the
    // only clock — a guest fast-forwarding its own sky would drift out of step
    // with everyone else's night.
    if (netGuest()) {
      net::SleepMsg m;
      m.on = true;
      netClient_.sendSleep(m.on);
      return;
    }
    if (netHosting()) {
      netHost_.onLocalSleep(true);
      return;
    }
    sky_.startSleep();
    interface_.notify().push("You slept through the night");
  };
  // The Soul Anchor, from js/main.js:765-780. The bound point is a save field, which
  // is why binding it lands here rather than with the sleep and respawn systems it
  // otherwise belongs to.
  hooks.onSetSpawn = [this](int x, int y, int z) {
    hasSpawn_ = true;
    spawn_ = {x + 0.5f, static_cast<float>(y + 1), z + 0.5f};
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
    interface_.notify().push("Spawn point unbound");
  };
  hooks.onEat = [this](const game::ItemDef& item) {
    if (!player_) return false;
    const bool ok = player_->eat({static_cast<float>(item.food), item.risky});
    if (ok) audio::sfx::eat();
    return ok;
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
      interface_.notify().push("The wayshard can't find the sky here");
      return false;
    }
    if (p.y >= ts - 0.5f) {
      interface_.notify().push("You're already under the open sky");
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
      netClient_.sendBoatMount(e.id, !e.data.rider);
      return true;
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
  hooks.entities = &entities_;
  hooks.entityContext = &entityContext_;
  return hooks;
}

// The interface's screen enum tracks AppState, but it is not the same machine: the
// inventory and the Atlas draw over a live world, which AppState models as its own
// states while the interface has to know both which screen is up and that the world is
// still there.
void App::syncScreen() {
  switch (state_) {
    case AppState::Boot: interface_.setScreen(ui::Screen::Boot); break;
    case AppState::Menu: interface_.setScreen(ui::Screen::Menu); break;
    case AppState::Playing: interface_.setScreen(ui::Screen::None); break;
    case AppState::Paused: interface_.setScreen(ui::Screen::Pause); break;
    case AppState::Settings: interface_.setScreen(ui::Screen::Settings); break;
    case AppState::Inventory: interface_.setScreen(ui::Screen::Inventory); break;
    case AppState::RecipeBook: interface_.setScreen(ui::Screen::RecipeBook); break;
    case AppState::Map: interface_.setScreen(ui::Screen::Map); break;
    case AppState::Gallery: interface_.setScreen(ui::Screen::Gallery); break;
  }
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
  f.version = HR_VERSION;

  // Nameplates come from whichever half of the net layer is live; both keep the
  // same list, because both draw the same remote bodies.
  const net::Ghosts* ghosts = netHosting()   ? &netHost_.ghosts()
                              : netGuest()   ? &netClient_.ghosts()
                                             : nullptr;
  if (ghosts) {
    for (const net::Ghosts::Nameplate& plate : ghosts->nameplates()) {
      f.nameplates.push_back(ui::UiFrame::Nameplate{plate.pos, plate.name, plate.health});
    }
    const int others = static_cast<int>(ghosts->playerCount());
    f.netLine = (netHosting() ? "hosting Â· " : "guest Â· ") +
                std::to_string(others + 1) + " player" + (others == 0 ? "" : "s");
  }
  return f;
}

void App::wireInterface() {
  interface_.callbacks.resume = [this] { resumePlaying(); };
  interface_.callbacks.quitGame = [this] { running_ = false; };
  interface_.callbacks.settingChanged = [this](const std::string&) { applySettings(); };
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
          return out;
        };
        state.input = wire(be->input);
        state.fuel = wire(be->fuel);
        state.output = wire(be->output);
        state.fuelLeft = be->fuelLeft;
        state.fuelMax = be->fuelMax;
        state.progress = be->progress;
        for (const game::ItemStack& slot : be->slots) state.slots.push_back(wire(slot));
        state.final = true;
        netClient_.sendBlockEntityState(state);
      }
    }
    stationOpen_ = false;
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
  interface_.callbacks.closeScreen = [this] { closeCurrentScreen(); };
  interface_.callbacks.saveAndQuit = [this] {
    const bool saved = netGuest() ? false : saveCurrentWorld();
    leaveWorld();
    // "Left the world" is the honest message for a world that was never on disk —
    // a --seed harness run has nothing to save and should not claim it did.
    interface_.notify().push(saved ? "Saved and left the world" : "Left the world");
  };

  interface_.menu().actions.createWorld = [this](const std::string& name, std::uint32_t seed) {
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
      interface_.notify().push("Could not delete that world");
    }
  };

  interface_.menu().actions.importWorlds = [this] {
    int failures = 0;
    const int imported = save::importAllFromExports(&failures);
    if (imported == 0 && failures == 0) {
      interface_.notify().push("Put a .hrw world in data/exports first");
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
      interface_.notify().push("That is not an address or an invite code");
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
    return "Invite code (copy from the log or data/hollowreach.log):\n" +
           net::makeInviteCode(net::localAddress(), netHost_.port(), worldMeta_.name) + "\n" +
           std::to_string(guests) + (guests == 1 ? " guest connected" : " guests connected");
  };

  interface_.menu().actions.exportWorld = [this](const std::string& idOrEmpty) {
    // The pause menu passes nothing, meaning "the world I am in".
    const std::string id = idOrEmpty.empty() ? worldMeta_.id : idOrEmpty;
    if (id.empty()) {
      interface_.notify().push("There is no saved world to export");
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
      interface_.notify().push("Export failed: " + error);
    }
  };

  interface_.inventory().attach(&inventory_, &icons_);
  interface_.atlas().attach(&atlas_);
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
void App::closeCurrentScreen() {
  switch (state_) {
    case AppState::Settings:
      state_ = settingsReturn_ == AppState::Paused ? AppState::Paused : AppState::Menu;
      if (state_ == AppState::Paused && !world_) state_ = AppState::Menu;
      break;
    case AppState::Inventory:
    case AppState::RecipeBook:
    case AppState::Map:
      resumePlaying();
      break;
    case AppState::Gallery:
      state_ = galleryReturn_ == AppState::Paused && world_ ? AppState::Paused
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
    world_->setExplored(loaded->explored);
    world_->blockEntities() = loaded->blockEntities;
  }

  entities_.clear();
  world_->setDropSink([this](float x, float y, float z, const std::string& key, int count,
                             int dura) {
    entities_.spawnDrop(Vec3{x, y, z}, key, count, dura);
  });

  interactHooks_ = makeInteractHooks();
  renderer_.setEntities(&entities_);
  if (loaded) {
    inventory_ = loaded->inventory;
    entities_.load(loaded->entities);
    sky_.time = loaded->meta.time;
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
  if (!options.haveSpawnOverride && !loaded) {
    // Drop in on the surface — or on the sea, when the ground here is under it.
    sy = static_cast<float>(world_->spawnHeight(static_cast<int>(std::floor(sx)),
                                                static_cast<int>(std::floor(sz))));
  }

  // Synchronous, so the player never spawns inside unloaded space and fall through.
  world_->primeSpawn(sx, sz);

  player_ = std::make_unique<game::Player>(sx, sy, sz);
  player_->setLook(options.spawnYaw, options.spawnPitch);
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
  hooks.notify = [this](const std::string& message) { interface_.notify().push(message); };
  hooks.buildSave = [this] { return buildSave(); };
  hooks.adoptWorld = [this](const save::WorldSave& data) { adoptRemoteWorld(data); };
  hooks.onDisconnected = [this](const std::string& reason) {
    leaveNetwork(reason.empty() ? "Disconnected" : reason);
  };
  hooks.onRosterChange = [] {};
  hooks.playSfx = [](const std::string& kind, const Vec3& pos) {
    audio::sfx::playNamed(kind, pos);
  };
  return hooks;
}

bool App::startHosting(std::uint16_t port) {
  if (multiplayer() || !world_ || !player_) return false;
  std::string error;
  if (!netHost_.start(port, playerId_, playerName_, gameRefs(), makeSessionHooks(), &error)) {
    interface_.notify().push("Could not host: " + error);
    return false;
  }
  // Every local edit — including the water simulation's own writes, which only
  // the host runs — goes out to the guests.
  world_->setEditSink([this](int x, int y, int z, world::BlockId id, int meta) {
    netHost_.onLocalEdit(x, y, z, static_cast<std::uint16_t>(id),
                         static_cast<std::uint8_t>(meta));
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
    interface_.notify().push("Could not join: " + error);
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
  if (!startWorld(options, &data)) {
    leaveNetwork("Could not build the host's world");
    return;
  }
  // A guest's world belongs to the host: it is never written, and the edit sink
  // sends local edits to the host for approval rather than recording them.
  worldMeta_ = data.meta;
  worldMeta_.id.clear();
  worldOnDisk_ = false;
  netClient_.attachGame(gameRefs());
  world_->setEditSink([this](int x, int y, int z, world::BlockId id, int meta) {
    netClient_.sendEdit(x, y, z, static_cast<std::uint16_t>(id),
                        static_cast<std::uint8_t>(meta));
  });
  interface_.notify().push("Joined " +
                           (data.meta.name.empty() ? std::string("the world") : data.meta.name));
}

void App::leaveNetwork(const std::string& reason) {
  advertiser_.stop();
  netHost_.stop();
  netClient_.stop(false);
  if (world_) world_->setEditSink(nullptr);
  if (!reason.empty()) interface_.notify().push(reason);
}

// ---- saves -----------------------------------------------------------------

save::WorldSave App::buildSave() {
  save::WorldSave out;
  out.meta = worldMeta_;
  out.meta.savedAt = save::nowSeconds();
  out.meta.gameVersion = HR_VERSION;
  out.meta.time = sky_.time;
  out.meta.hasSpawn = hasSpawn_;
  out.meta.spawn = spawn_;
  if (world_) {
    out.meta.seed = world_->seed();
    out.meta.genVersion = world_->genVersion();
    out.edits = world_->edits();
    out.explored.assign(world_->explored().begin(), world_->explored().end());
    out.blockEntities = world_->blockEntities();
  }
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
    interface_.notify().push("Could not save: " + error);
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
    interface_.notify().push("Could not load: " + error);
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
  // Nothing bound: the world origin column, on the surface or on the sea above it.
  return Vec3{kSpawnX,
              static_cast<float>(world_->spawnHeight(static_cast<int>(std::floor(kSpawnX)),
                                                     static_cast<int>(std::floor(kSpawnZ)))),
              kSpawnZ};
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
    if (netGuest()) {
      netClient_.sendToss(from, dir, s.key, s.count, s.dura);
    } else {
      entities_.spawnTossed(from, dir, s.key, s.count, s.dura);
    }
    s.clear();
  };
  for (game::ItemStack& s : inventory_.slots()) toss(s);
  for (game::ItemStack& s : inventory_.armor()) toss(s);

  const Vec3 wake = spawnPoint();
  // Synchronous, so the ground is there to be stood on before the next physics step.
  world_->primeSpawn(wake.x, wake.z);
  player_->teleport(wake);
  player_->reviveFull();
  interface_.notify().push("You blacked out and woke at spawn. Your things are where you fell.");
}

void App::frame() {
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

  if (state_ == AppState::Playing) updatePlaying(dt);

  // Block entities keep ticking behind an open station screen — a forge does not stop
  // smelting because you are looking at it.
  if (world_ && state_ != AppState::Playing) {
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

  window_.input().endFrame();
  window_.swapBuffers();
  ++frameIndex_;

  if (options_.exitAfterFrames > 0 && frameIndex_ >= options_.exitAfterFrames) {
    running_ = false;
  }
}

void App::updatePlaying(double dt) {
  Input& in = window_.input();
  const float fdt = static_cast<float>(dt);

  // Mouse look at render rate, not simulation rate, so aim latency stays low.
  if (window_.pointerCaptured()) {
    double dx = 0, dy = 0;
    in.takeMouse(dx, dy);
    player_->look(dx, dy, playerOptions_);
  }

  // The player and entities are the only things substepped: a long frame is split
  // so a fast body cannot pass through a block. Everything else takes the whole dt.
  //
  // --freeze skips it entirely, which is what makes `--at` mean what it says. The
  // flag has always claimed to be "an exact camera placement", and for anything
  // above the ground it was not: gravity ran for the two hundred frames the world
  // needs to stream in, and the capture happened wherever the player landed.
  if (!options_.freezePlayer) {
    double step = 0;
    const int steps = clock_.substeps(step);
    for (int i = 0; i < steps; ++i) {
      player_->update(static_cast<float>(step), in, *world_, playerOptions_, clock_.simTime());
    }
  }

  handleHotbarInput(in);
  // Interaction runs at render rate, on the same eye and forward vector the
  // crosshair is drawn from, so what you break is always what you were aiming at.
  // The click that re-captures the pointer is swallowed: clicking the window to
  // focus it should not also mine whatever happens to be under the crosshair.
  if (resumeClickGuard_ > 0) {
    --resumeClickGuard_;
    interact_.reset();
  } else {
    interact_.update(fdt, in, *player_, *world_, inventory_, interactHooks_);
  }

  render::Viewmodel& vm = renderer_.viewmodel();
  vm.setItem(inventory_.selectedSlot().key);
  if (interact_.swung()) vm.swing();
  vm.update(fdt, player_->bobPhase(), player_->bobMagnitude());

  world_->tickBlockEntities(fdt);
  // Water runs on its own accumulator at roughly six batches a second, and only
  // here: a multiplayer guest must not simulate it, because the host's edits are
  // what it will be told about (js/main.js:669 gates it the same way).
  if (!netGuest()) world_->tickWater(fdt);

  refreshEntityContext();
  entities_.tick(fdt, entityContext_);

  // Passive grazers on nearby grass in daylight, zombies on nearby ground at
  // night, both capped and both on a four-second cadence. The host owns every
  // entity in a multiplayer world, so a guest spawns nothing at all.
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
      entities_.trySpawnZombie(*world_, player_->pos().x, player_->pos().z, sky_.dayFactor());
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
    streamTotal_ += ms;
    ++streamFrames_;
    if (ms > streamWorst_) {
      streamWorst_ = ms;
      streamWorstFrame_ = frameIndex_;
    }
  }
  sky_.update(fdt);

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
  entityContext_.input = &window_.input();
  if (!entityContext_.notify) {
    entityContext_.notify = [this](const std::string& message) {
      interface_.notify().push(message);
    };
  }
}

void App::handleHotbarInput(Input& in) {
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

  // Q drops one; with control held, the whole stack. There is nowhere for a drop
  // to go until entities land, so this just discards for now — which is still the
  // only way to free a slot without an inventory screen.
  if (in.pressed(Key::Q)) {
    game::ItemStack& s = inventory_.selectedSlot();
    if (!s.empty() && world_ && player_) {
      const std::string key = s.key;
      const int dura = s.dura;
      const int count = in.ctrl() ? s.count : 1;
      if (in.ctrl()) s.clear();
      else inventory_.consumeSelected();
      const Vec3 eye = player_->eye();
      const Vec3 dir = player_->forward();
      const Vec3 from{eye.x + dir.x * 0.4f, eye.y - 0.2f, eye.z + dir.z * 0.4f};
      if (netGuest()) {
        netClient_.sendToss(from, dir, key, count, dura);
      } else {
        entities_.spawnTossed(from, dir, key, count, dura);
      }
      audio::sfx::toss();
    }
  }
}

void App::renderWorld() {
  // The eye used for rendering carries the head bob; the one used for aiming does
  // not, so the crosshair never drifts while walking.
  const Vec3 eye = player_->eye() + player_->viewBobOffset();
  camera_.update(eye, player_->yaw(), player_->pitch());

  // Submerged post-effect, eased rather than snapped so breaking the surface is a
  // transition instead of a flicker.
  const float target = player_->headInWater(*world_) ? 1.0f : 0.0f;
  underwater_ += (target - underwater_) * std::min(1.0f, static_cast<float>(clock_.dt()) * 9.0f);

  render::Renderer::Selection selection {interact_.selectionX(), interact_.selectionY(),
                                         interact_.selectionZ()};
  // The selection outline goes with the HUD: it is an aiming affordance, not part
  // of the world, and a wireframe box in the middle of a screenshot is exactly
  // what F1 is being pressed to get rid of.
  const bool showSelection = interact_.hasSelection() && interface_.hudVisible();
  renderer_.render(*world_, camera_, sky_, window_.width(), window_.height(),
                   showSelection ? &selection : nullptr,
                   inventory_.selectedSlot().key, underwater_, clock_.simTime());

  static Interval statsLog {2.0};
  if (statsLog.due(clock_.dt())) {
    const game::ItemStack& held = inventory_.selectedSlot();
    log::debug("%.0f fps | %d/%zu chunks | %zu pending | pos %.1f %.1f %.1f | %s | held %s x%d",
               clock_.fps(), renderer_.drawnChunks(), world_->loadedChunkCount(),
               world_->pendingCount(), player_->pos().x, player_->pos().y, player_->pos().z,
               sky_.clockString().c_str(), held.empty() ? "-" : held.key.c_str(), held.count);
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
    } else if (state_ == AppState::Inventory) {
      resumePlaying();
    } else if (state_ == AppState::RecipeBook) {
      closeCurrentScreen();
    }
  }

  if (in.pressed(Key::R)) {
    if (state_ == AppState::Playing || state_ == AppState::Inventory) {
      recipeReturn_ = state_;
      state_ = AppState::RecipeBook;
      window_.setPointerCaptured(false);
    } else if (state_ == AppState::RecipeBook) {
      state_ = recipeReturn_;
      if (state_ == AppState::Playing) resumePlaying();
    }
  }

  if (in.pressed(Key::N)) {
    if (state_ == AppState::Playing) {
      state_ = AppState::Map;
      window_.setPointerCaptured(false);
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
    else interface_.notify().push("Screenshot failed");
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
      {"pause", AppState::Paused, ui::MenuPage::Main, world::Station::None, false},
      {"settings", AppState::Settings, ui::MenuPage::Main, world::Station::None, false},
      {"inventory", AppState::Inventory, ui::MenuPage::Main, world::Station::None, true},
      {"workbench", AppState::Inventory, ui::MenuPage::Main, world::Station::Workbench, true},
      {"forge", AppState::Inventory, ui::MenuPage::Main, world::Station::Forge, true},
      {"chest", AppState::Inventory, ui::MenuPage::Main, world::Station::Chest, true},
      {"recipes", AppState::RecipeBook, ui::MenuPage::Main, world::Station::None, false},
      {"map", AppState::Map, ui::MenuPage::Main, world::Station::None, false},
      {"gallery", AppState::Gallery, ui::MenuPage::Main, world::Station::None, false},
  };
  for (const Entry& e : kEntries) {
    if (name != e.name) continue;
    state_ = e.state;
    syncScreen();
    if (e.state == AppState::Menu) interface_.menu().setPage(e.page);
    if (e.isStation) {
      // The forge and chest screens need a block entity to show. A scratch one lives
      // here rather than in the world, because the point is to capture the layout, not
      // to place a block.
      if (e.station == world::Station::Forge || e.station == world::Station::Chest) {
        scratchStation_ = e.station == world::Station::Forge ? game::makeForge()
                                                            : game::makeChest();
        interface_.callbacks.currentStation = [this] { return &scratchStation_; };
      }
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

void App::renderMenuScene(double /*dt*/) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, window_.width(), window_.height());
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);

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
