// Entry point + game loop + top-level state machine
// (menu / playing / paused / inventory / settings).

import { createGL } from "./core/gl.js";
import { Camera } from "./core/camera.js";
import { Input } from "./core/input.js";
import { buildAtlas } from "./render/texatlas.js";
import { Renderer } from "./render/renderer.js";
import { Sky } from "./render/sky.js";
import { World } from "./world/world.js";
import { heightAt, SEA_LEVEL } from "./world/worldgen.js";
import { Skybox } from "./render/panorama.js";
import { addScreenshot, addPanorama, getMenuPanorama, setMenuPanorama, makeThumb } from "./save/gallery.js";
import { Player } from "./game/player.js";
import { Interact } from "./game/interact.js";
import { Inventory } from "./game/inventory.js";
import { buildIcons } from "./game/items.js";
import { HUD } from "./ui/hud.js";
import { Menu } from "./ui/menu.js";
import { InventoryUI } from "./ui/inventoryui.js";
import { RecipeBook } from "./ui/recipebook.js";
import { Settings, renderSettings, QUALITY_PRESETS } from "./ui/settings.js";
import { initNotify, notify } from "./ui/notify.js";
import { engine as audio } from "./audio/engine.js";
import { sfx } from "./audio/sfx.js";
import { director } from "./audio/director.js";
import { serialize, deserializeEdits, deserializeBlockEntities, SAVE_VERSION } from "./save/serialize.js";
import { GAME_VERSION } from "./version.js";
import { saveWorld, newId } from "./save/storage.js";
import { exportWorld } from "./save/transfer.js";
import { NetHost } from "./net/host.js";
import { NetClient } from "./net/client.js";
import { clientRideBoat } from "./game/entities/boat.js";
import { localPlayerId, getPlayerName } from "./net/protocol.js";
import { Nameplates } from "./ui/mpui.js";
import { WorldMap } from "./ui/map.js";

const SPAWN_XZ = [8.5, 8.5];

class Game {
  constructor() {
    this.canvas = document.getElementById("gl");
    try {
      this.gl = createGL(this.canvas);
    } catch (e) {
      document.getElementById("boot").innerHTML =
        `<div class="menu-card"><h2>WebGL2 not available</h2><p style="color:#9aa7b4">This game needs a browser with WebGL2 (recent Chrome, Edge or Firefox).</p></div>`;
      throw e;
    }

    this.atlas = buildAtlas(this.gl);
    buildIcons(this.atlas);
    this.renderer = new Renderer(this.gl, this.atlas);
    this.applyGraphicsSettings();
    this.camera = new Camera();
    this.input = new Input(this.canvas);
    this.sky = new Sky();
    this.hud = new HUD();
    this.map = new WorldMap(this, this.atlas);   // the Atlas: world map/minimap/waypoints
    this.waypoints = [];
    initNotify();

    this.state = "menu";
    this.world = null;
    this.player = null;
    this.inventory = null;
    this.interact = new Interact();
    this.meta = null;
    this.spawn = null;
    this.settingsReturn = "menu";
    // ---- multiplayer ----
    this.net = null;                 // NetHost | NetClient | null
    this.pid = localPlayerId();      // stable anonymous id for rejoin slots
    this.remotePlayersStore = {};    // guests' saved progress (host side)
    this.nameplates = new Nameplates();
    this._mpRefresh = null;          // set by the multiplayer panel while open
    // Static menu-background panorama (a skybox from six captured faces — see
    // render/panorama.js). Shows the player's last-played world; zero world sim.
    this.skybox = new Skybox(this.gl);
    this._menuT = 0;
    this._shotQueued = false;    // F2 screenshot pending capture after this frame
    this._panoQueued = false;    // F8 panorama pending capture after this frame

    this.menu = new Menu({
      gameRef: this,
      startNew: (p) => this.startNew(p),
      loadWorld: (s) => this.loadWorld(s),
      importWorld: (s) => this.importWorld(s),
      openSettings: (fromPause) => this.openSettings(fromPause),
      resume: () => this.resume(),
      saveAndQuit: () => this.saveAndQuit(),
      exportCurrent: () => this.exportCurrent(),
      openGallery: (fromPause) => this.openGallery(fromPause),
      setMenuBackground: (faces) => this.setMenuBackground(faces),
      closeGallery: () => this.closeGallery(),
    });

    this.invUI = new InventoryUI(new Inventory(), { onClose: () => this.afterInventoryClosed() });
    this.recipeBook = new RecipeBook();
    this._rbReturn = "playing";

    this.bindGlobalUI();
    this.input.onLockChange((locked) => {
      if (!locked && this.state === "playing") this.pause();
    });
    this.canvas.addEventListener("mousedown", () => {
      if (this.state === "playing" && !this.input.locked) this.input.requestLock();
    });
    addEventListener("resize", () => this.resize());

    // Audio can only start from a user gesture; the first click/keypress wakes
    // the engine (and every later one is a cheap no-op resume).
    const wake = () => { audio.unlock(); this.applyAudioSettings(); };
    addEventListener("pointerdown", wake);
    addEventListener("mousedown", wake);
    addEventListener("keydown", wake);
    // Every real button in the UI gives a soft click (capture phase so handlers
    // that re-render the DOM can't swallow it).
    addEventListener("click", (e) => {
      if (e.target && e.target.closest && e.target.closest("button")) sfx.uiClick();
    }, { capture: true });

    // Guard against accidentally losing progress while in a world: confirm on
    // unload (catches close / Ctrl+W) and swallow browser keyboard shortcuts so a
    // stray reload / bookmark / find / new-tab combo can't yank you out of play.
    addEventListener("beforeunload", (e) => {
      if (this.world) { e.preventDefault(); e.returnValue = ""; }
    });
    addEventListener("keydown", (e) => {
      if (!this.world) return;
      // Never swallow keys aimed at a text field (world-name box, future search).
      const t = e.target;
      if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.isContentEditable)) return;
      if (this.isBrowserShortcut(e)) e.preventDefault();
    }, { capture: true });
    // Ctrl+scroll zooms the browser page — block it while in a world so sprinting
    // (Ctrl) + hotbar scroll doesn't zoom. The game's own wheel handler still reads
    // the scroll for hotbar selection.
    addEventListener("wheel", (e) => {
      if (this.world && e.ctrlKey) e.preventDefault();
    }, { passive: false, capture: true });

    this.resize();
    document.querySelector(".version-tag").textContent = `Hollowreach v${GAME_VERSION}`;
    this.menu.showMain();
    this.showScreen("menu");     // shown, but the boot overlay covers it until ready

    this.last = performance.now();
    this._fpsT = 0; this._fpsC = 0; this.fps = 0;
    this._autosaveT = 0;
    this._sheepT = 0;
    this._zombieT = 0;
    requestAnimationFrame((t) => this.loop(t));

    // Load the cached menu panorama (or bake the default once, behind the boot
    // screen), then reveal the menu.
    this.initMenuBackground().finally(() => {
      document.getElementById("boot").classList.add("hidden");
    });
    // (pig shares the _sheepT grazer timer)
  }

  // True for keystrokes the browser would normally act on (reload, new/close/
  // switch tab, bookmark, find, print, save, history, downloads, address bar,
  // zoom, back/forward). We swallow these while in a world so they can't
  // interrupt play. NOTE: a few combos (Ctrl+T / Ctrl+N / Ctrl+W / Ctrl+Tab) are
  // reserved by the browser and aren't cancelable from a page — the beforeunload
  // confirm is the backstop for those, and the Keyboard Lock API (engaged in
  // fullscreen, see enterPlay) captures them outright when available.
  isBrowserShortcut(e) {
    const c = e.code;
    if (c === "F5" || c === "F3" || c === "F6" || c === "F7") return true;   // reload / find / addr-bar / caret
    if (e.altKey && (c === "ArrowLeft" || c === "ArrowRight" || c === "KeyD" || c === "Home")) return true; // nav / addr-bar
    if (e.ctrlKey || e.metaKey) {
      const BLOCK_CODES = new Set([
        "KeyT", "KeyN", "KeyW", "KeyR", "KeyD", "KeyF", "KeyG", "KeyH", "KeyJ",
        "KeyL", "KeyO", "KeyP", "KeyS", "KeyU", "KeyA", "KeyE", "KeyK",
        "Tab", "PageUp", "PageDown", "Minus", "Equal", "NumpadAdd", "NumpadSubtract",
        "Digit0", "Digit1", "Digit2", "Digit3", "Digit4",
        "Digit5", "Digit6", "Digit7", "Digit8", "Digit9",
      ]);
      if (BLOCK_CODES.has(c)) return true;
    }
    return false;
  }

  // Best-effort: grab system shortcut keys (Ctrl+T/W/N, Esc, …) outright. The
  // Keyboard Lock API only engages while the document is fullscreen, so this is a
  // no-op windowed; it just upgrades protection for players who go fullscreen.
  engageKeyboardLock() {
    try { navigator.keyboard?.lock?.(); } catch { /* unsupported */ }
  }

  bindGlobalUI() {
    document.getElementById("settings-back").onclick = () => this.closeSettings();
    document.getElementById("recipebook-back").onclick = () => this.closeRecipeBook();
    document.getElementById("gallery-back").onclick = () => this.closeGallery();
  }

  resize() {
    const w = innerWidth, h = innerHeight;
    this.canvas.width = w;
    this.canvas.height = h;
    this.renderer.resize(w, h);
  }

  showScreen(name) {
    for (const id of ["menu", "pause", "settings", "inventory", "gallery", "map"]) {
      document.getElementById(id).classList.toggle("hidden", id !== name && name !== "playing");
      if (name === "playing") document.getElementById(id).classList.add("hidden");
    }
    this.hud.show(name === "playing" || name === "paused" || name === "inventory");
  }

  // ---------- world lifecycle ----------
  startNew({ name, seed }) {
    this.meta = { id: newId(), name, createdAt: Date.now() };
    this.remotePlayersStore = {};
    this.waypoints = [];
    this.world = new World(this.gl, this.atlas, seed, Settings.get("renderDistance"));
    this.inventory = new Inventory();
    this.invUI.inv = this.inventory;
    this.sky.time = 0.32;
    const [sx, sz] = SPAWN_XZ;
    this.world.primeSpawn(sx, sz);
    const y = this.world.spawnHeight(Math.floor(sx), Math.floor(sz));
    this.player = new Player(sx, y, sz);
    this.spawn = [sx, y, sz];
    this.enterPlay();
    notify(`Welcome to ${name}`);
  }

  loadWorld(save) {
    this.meta = { id: save.id || newId(), name: save.name, createdAt: save.createdAt || Date.now() };
    this.remotePlayersStore = save.remotePlayers || {};
    this.waypoints = Array.isArray(save.waypoints) ? save.waypoints : [];
    // worlds carry the generator version they were created with (missing = v1),
    // so terrain keeps generating exactly as it did when the world was born
    this.world = new World(this.gl, this.atlas, save.seed, Settings.get("renderDistance"), save.genVersion || 1);
    if (Array.isArray(save.explored)) for (const k of save.explored) this.world.explored.add(k);
    this.world.edits = deserializeEdits(save);
    this.world.blockEntities = deserializeBlockEntities(save);
    this.world.entities.load(save.entities);
    this.inventory = Inventory.fromJSON(save.inventory);
    this.invUI.inv = this.inventory;
    this.sky.time = save.time ?? 0.32;
    this.player = new Player(SPAWN_XZ[0], 80, SPAWN_XZ[1]);
    this.player.loadJSON(save.player);
    this.spawn = Array.isArray(save.spawn) && save.spawn.length === 3
      ? save.spawn.slice()
      : [SPAWN_XZ[0], this.world.spawnHeight(SPAWN_XZ[0] | 0, SPAWN_XZ[1] | 0), SPAWN_XZ[1]];
    this.world.primeSpawn(this.player.pos[0], this.player.pos[2]);
    this.enterPlay();
    notify(`Loaded ${save.name}`);
  }

  async importWorld(save) {
    if (!save.id) save.id = newId();
    save.version = save.version || SAVE_VERSION;
    await saveWorld(save);           // copy into the worlds/ folder
    this.loadWorld(save);
  }

  enterPlay() {
    this.player.inventory = this.inventory;   // so taking a hit can wear the player's armour
    this.state = "playing";
    audio.setDucked(false);
    this.showScreen("playing");
    this.input.requestLock();
    this.engageKeyboardLock();
  }

  pause() {
    if (this.state !== "playing") return;
    this.state = "paused";
    audio.setDucked(true);
    // The Esc that exited pointer-lock (and triggered this) is still queued as a
    // just-pressed edge; drop it so the same Esc doesn't immediately resume.
    this.input.justPressed.delete("Escape");
    this.menu.showPause();
    this.showScreen("pause");
  }
  resume() {
    this.state = "playing";
    audio.setDucked(false);
    this.showScreen("playing");
    this.input.requestLock();
  }

  openInventory(mode, be = null) {
    this.state = "inventory";
    this.input.exitLock();
    this.invUI.open(mode, be);
    this.showScreen("inventory");
  }
  closeInventory() {
    if (this.invUI.mode === "chest" && this._chestPos) sfx.chestClose(this._chestPos);
    this.invUI.close();                             // triggers afterInventoryClosed via onClose
  }
  afterInventoryClosed() {
    // guests hand the container back to the host (final contents + unlock)
    if (this.net && this.net.isClient) this.net.closeBE();
    this.state = "playing";
    this.showScreen("playing");
    this.input.requestLock();
  }
  openStation(station, x, y, z) {
    const stateful = station === "forge" || station === "chest";
    // Guests ask the host for the container's contents (and an exclusive open
    // lock); the UI opens when the reply arrives — see onRemoteBEOpen.
    if (stateful && this.net && this.net.isClient) {
      this.net.requestBE(station, x, y, z);
      return;
    }
    // Hosts respect a guest's open lock.
    if (stateful && this.net && this.net.isHost && this.net.beLockedByGuest(x, y, z)) {
      notify("Someone else is using this");
      return;
    }
    // Forges/chests carry persistent state at their world position.
    const be = stateful ? this.world.getOrCreateBlockEntity(x, y, z, station) : null;
    if (station === "chest") {
      this._chestPos = [x + 0.5, y + 0.5, z + 0.5];
      sfx.chestOpen(this._chestPos);
    }
    this.openInventory(station, be);
  }

  // Recipe book floats over whatever screen is active (play or inventory) and
  // restores it on close. It never changes the world, so no state teardown.
  openRecipeBook() {
    if (this.recipeBook.isOpen()) return;
    this._rbReturn = this.state;
    this.state = "recipebook";
    this.input.exitLock();
    this.recipeBook.open();
  }
  closeRecipeBook() {
    this.recipeBook.close();
    this.state = this._rbReturn;
    if (this.state === "playing") this.input.requestLock();
  }

  // Push the graphics-quality preset + per-feature toggles into the renderer's
  // quality knobs. The preset sets intensity (render scale + sample counts +
  // shadow-map resolution); the toggles gate each effect on/off within that tier.
  applyGraphicsSettings() {
    const q = QUALITY_PRESETS[Settings.get("graphicsQuality")] || QUALITY_PRESETS.High;
    this.renderer.setQuality({
      scale: q.scale,
      ssaoSamples: Settings.get("ambientOcclusion") ? q.ssaoSamples : 0,
      godrays: Settings.get("godRays") ? 1 : 0,
      godraySamples: q.godraySamples,
      ssrSteps: Settings.get("waterReflections") ? q.ssrSteps : 0,
      shadowSize: Settings.get("castShadows") ? (q.shadowSize ?? 0) : 0,
      cloudSteps: Settings.get("clouds") ? (q.cloudSteps ?? 0) : 0,
      cloudShadows: Settings.get("cloudShadows") ? 1 : 0,
    });
  }

  // Push the audio sliders (0-100) into the engine's bus gains.
  applyAudioSettings() {
    audio.setVolumes({
      master: Settings.get("masterVolume") / 100,
      sfx: Settings.get("sfxVolume") / 100,
      ambient: Settings.get("ambientVolume") / 100,
      ui: Settings.get("uiVolume") / 100,
    });
  }

  openSettings(fromPause) {
    this.settingsReturn = fromPause ? "pause" : "menu";
    this.state = "settings";
    renderSettings(document.getElementById("settings-root"), (key) => {
      if (key === "renderDistance" && this.world) this.world.renderDist = Settings.get("renderDistance");
      if (["graphicsQuality", "ambientOcclusion", "godRays", "waterReflections", "castShadows", "clouds", "cloudShadows"].includes(key)) this.applyGraphicsSettings();
      if (key.endsWith("Volume")) this.applyAudioSettings();
      // menuPanorama: renderMenuScene reads the setting each frame, so nothing to
      // do here — but if it was just turned on and no panorama exists yet, load/bake it.
      if (key === "menuPanorama" && Settings.get("menuPanorama") && !this.skybox.ready()) this.initMenuBackground();
    });
    this.showScreen("settings");
  }
  closeSettings() {
    if (this.settingsReturn === "pause") { this.state = "paused"; this.showScreen("pause"); }
    else { this.state = "menu"; this.showScreen("menu"); }
  }

  openGallery(fromPause) {
    this._galleryReturn = fromPause ? "pause" : "menu";
    this.state = "gallery";
    this.menu.showGallery();
    this.showScreen("gallery");
  }
  closeGallery() {
    if (this._galleryReturn === "pause") { this.state = "paused"; this.showScreen("pause"); }
    else { this.state = "menu"; this.showScreen("menu"); }
  }

  buildSave() {
    const remotePlayers = this.net && this.net.isHost
      ? this.net.remotePlayersForSave() : this.remotePlayersStore;
    return serialize(this.world, this.player, this.inventory,
      { ...this.meta, time: this.sky.time, remotePlayers, spawn: this.spawn, waypoints: this.waypoints });
  }
  saveCurrent() {
    // NEVER persist a world we joined as a guest — it belongs to the host.
    if (!this.world || (this.meta && this.meta.remote)) return null;
    const save = this.buildSave();
    return saveWorld(save).then(() => save);   // promise that resolves to the save
  }
  async saveAndQuit() {
    director.stop();             // settle the ambience beds before the menu
    if (this.net && this.net.isClient) {
      this.net.dispose(true);    // sends our final state + a polite bye
      this.net = null;
      notify("Left the world");
    } else {
      await this.saveCurrent(); // (includes guests' progress when hosting)
      if (this.net) { this.net.dispose(); this.net = null; }
      notify("World saved");
    }
    // Capture the menu panorama from where you logged out (before tearing down the
    // world). The pause overlay is still up, hiding the brief square-canvas render.
    this.captureMenuPanorama();
    try { navigator.keyboard?.unlock?.(); } catch { /* unsupported */ }
    if (this.world) this.world.dispose();
    this.world = this.player = this.inventory = null;
    this.waypoints = [];
    this.map.reset();
    this.nameplates.clear();
    this.state = "menu";
    this.menu.showMain();
    this.showScreen("menu");
  }

  // ---------- multiplayer lifecycle ----------
  startHosting() {
    if (!this.world || this.net || (this.meta && this.meta.remote)) return null;
    this.net = new NetHost(this, this.pid, getPlayerName(),
      () => { if (this._mpRefresh) this._mpRefresh(); });
    return this.net;
  }
  stopHosting() {
    if (this.net && this.net.isHost) { this.net.dispose(); this.net = null; }
  }

  // Built by the Join panel; becomes this.net once the world payload arrives.
  createJoinClient(name) {
    const client = new NetClient(this.pid, name, {
      onWorld: (payload) => this.startRemote(payload, client),
      onDisconnect: (reason) => this.onNetDisconnect(reason),
      onNotify: (m) => notify(m),
      onRoster: () => { if (this._mpRefresh) this._mpRefresh(); },
      onBEOpen: (kind, x, y, z, be) => this.onRemoteBEOpen(kind, x, y, z, be),
      onBEForceClose: () => { if (this.state === "inventory") this.closeInventory(); },
    });
    return client;
  }

  // Enter a host's world (the payload has been validated by the net layer).
  startRemote(payload, client) {
    if (this.world) return;                    // shouldn't happen — join UI is menu-only
    this.net = client;
    this.meta = { id: null, name: payload.name, remote: true };
    this.remotePlayersStore = {};
    this.waypoints = [];
    this.world = new World(this.gl, this.atlas, payload.seed, Settings.get("renderDistance"), payload.genVer || 1);
    this.world.edits = deserializeEdits({ edits: payload.edits });
    this.world.blockEntities = deserializeBlockEntities({ blockEntities: payload.blockEntities });
    this.inventory = payload.inventory ? Inventory.fromJSON(payload.inventory) : new Inventory();
    this.invUI.inv = this.inventory;
    this.sky.time = payload.time;
    // ownSpawn = the Soul Anchor spot we attuned in a previous session on this host
    this.spawn = payload.ownSpawn ? payload.ownSpawn.slice() : payload.spawn.slice();
    const startPos = payload.player ? payload.player.pos : payload.spawn;
    this.player = new Player(startPos[0], startPos[1], startPos[2]);
    if (payload.player) this.player.loadJSON(payload.player);
    this.player.pos = [startPos[0], startPos[1], startPos[2]];
    this.world.primeSpawn(this.player.pos[0], this.player.pos[2]);
    client.attach(this);
    client.ready();
    this.enterPlay();
    notify(`Joined ${payload.name}`);
  }

  // The connection died (host quit, kick, network). Guests fall back to the menu.
  onNetDisconnect(reason) {
    notify(reason || "Disconnected");
    if (this.net && this.net.isClient) {
      this.net = null;
      if (this.world) {
        director.stop();
        try { navigator.keyboard?.unlock?.(); } catch { /* unsupported */ }
        this.world.dispose();
        this.world = this.player = this.inventory = null;
        this.waypoints = [];
        this.map.reset();
        this.nameplates.clear();
        this.state = "menu";
        this.menu.showMain();
        this.showScreen("menu");
      }
    }
  }

  onNetNotify(msg) { notify(msg); }

  // A guest's container request came back with the authoritative contents.
  onRemoteBEOpen(kind, x, y, z, be) {
    if (this.state !== "playing") { if (this.net) this.net.closeBE(); return; }
    if (kind === "chest") {
      this._chestPos = [x + 0.5, y + 0.5, z + 0.5];
      sfx.chestOpen(this._chestPos);
    }
    this.openInventory(kind, be);
  }
  async exportCurrent() {
    const save = await this.saveCurrent();
    if (save) exportWorld(save);
  }

  // ---------- main loop ----------
  loop(now) {
    let dt = (now - this.last) / 1000;
    this.last = now;
    if (dt > 0.05) dt = 0.05;

    this._fpsC++; this._fpsT += dt;
    if (this._fpsT >= 0.5) { this.fps = Math.round(this._fpsC / this._fpsT); this._fpsC = 0; this._fpsT = 0; }

    // Menu / container hotkeys are handled here every frame (edge-triggered,
    // then cleared by endFrame) so each press fires exactly once. Doing this in
    // a separate DOM listener as well caused menus to reopen and lock to thrash.
    this.handleGlobalKeys();

    if (this.state === "playing") this.updatePlaying(dt);
    // Multiplayer never pauses: menus/inventory only stop YOUR input, the world
    // (and the connection) keeps running underneath.
    else if (this.world && this.net) this.updateNetIdle(dt);
    else if (this.state === "inventory") { if (this.world) this.world.tickBlockEntities(dt); this.invUI.tick(dt); }

    if (this.state === "map") this.map.update(dt);   // the fullscreen atlas redraw

    if (this.world) this.renderFrame();
    else this.renderMenuScene(dt);

    if (this.world && this.player) this.map.tickHud(dt, this.camera);   // minimap + waypoint markers

    // Captures run right after this frame's render (same JS turn) so a screenshot
    // reads exactly what was drawn. A panorama re-renders the normal frame after,
    // so its brief square-canvas render never reaches the screen.
    if (this._shotQueued) { this._shotQueued = false; this.takeScreenshot(); }
    if (this._panoQueued) { this._panoQueued = false; this.takePanorama(); if (this.world) this.renderFrame(); }

    this.input.endFrame();
    requestAnimationFrame((t) => this.loop(t));
  }

  // Single source of truth for Esc / E / R / M across every state.
  handleGlobalKeys() {
    const input = this.input;

    // Never steal letters from a focused text field (waypoint names, world
    // name…). Escape still works so you can always back out.
    const ae = document.activeElement;
    if (ae && (ae.tagName === "INPUT" || ae.tagName === "TEXTAREA" || ae.isContentEditable)) {
      if (input.pressed("Escape")) ae.blur();
      return;
    }

    if (input.pressed("Escape")) {
      if (this.state === "recipebook") this.closeRecipeBook();
      else if (this.state === "map") this.closeMap();
      else if (this.state === "gallery") this.closeGallery();
      else if (this.state === "inventory") this.closeInventory();
      else if (this.state === "settings") this.closeSettings();
      else if (this.state === "paused") this.resume();
      // Locked play pauses via the browser's pointer-lock exit (onLockChange);
      // this covers the rare case where play is already unlocked.
      else if (this.state === "playing" && !this.input.locked) this.pause();
      return;
    }

    // E opens the inventory while playing and closes any open container/menu
    // (inventory, workbench, forge, recipe book) — but never the pause menu.
    if (input.pressed("KeyE")) {
      if (this.state === "playing") this.openInventory("inventory");
      else if (this.state === "inventory") this.closeInventory();
      else if (this.state === "recipebook") this.closeRecipeBook();
      return;
    }

    if (input.pressed("KeyR")) {
      if (this.state === "playing" || this.state === "inventory") this.openRecipeBook();
      else if (this.state === "recipebook") this.closeRecipeBook();
      return;
    }

    // M: the Atlas world map (needs the Atlas item in your inventory)
    if (input.pressed("KeyM")) {
      if (this.state === "playing") this.openMap();
      else if (this.state === "map") this.closeMap();
    }
  }

  openMap() {
    if (!this.world) return;
    if (!this.map.hasAtlas()) { notify("You need an Atlas (3 paper + 1 leather + 1 azurite)"); return; }
    this.state = "map";
    this.input.exitLock();
    this.map.open();
    this.showScreen("map");
  }
  closeMap() {
    this.map.close();
    this.state = "playing";
    this.showScreen("playing");
    this.input.requestLock();
  }

  updatePlaying(dt) {
    const input = this.input;

    if (input.pressed("F3")) this.hud.toggleDebug();
    if (input.pressed("F2")) this._shotQueued = true;    // screenshot -> gallery
    if (input.pressed("F8")) this._panoQueued = true;    // panorama -> gallery
    if (input.pressed("KeyN")) {                          // minimap toggle
      Settings.set("minimap", !Settings.get("minimap"));
      notify(Settings.get("minimap") ? "Minimap on" : "Minimap off");
    }
    if (input.pressed("KeyQ")) this.dropSelected(input.down("ControlLeft"));

    for (let i = 0; i < 9; i++) if (input.pressed("Digit" + (i + 1))) this.inventory.selected = i;
    const wheel = input.takeWheel();
    if (wheel) this.inventory.selected = (this.inventory.selected + (wheel > 0 ? 1 : -1) + 9) % 9;

    if (input.locked) {
      const [dx, dy] = input.takeMouse();
      this.player.sensitivity = Settings.get("sensitivity") * 0.0002;
      this.player.look(dx, dy, Settings.get("invertY"));
    }

    const opts = {
      fallDamage: Settings.get("fallDamage"),
      defense: this.inventory.totalDefense(),
      hunger: Settings.get("hunger"),
      flightAllowed: Settings.get("flight"),
      stepHeight: Settings.get("highStep") ? 1.0 : 0.6,
      notify,
      onHurt: () => { },
    };
    this.player.update(dt, input, this.world, opts);

    // Simulation split in multiplayer: guests only predict THEMSELVES — the
    // host runs water, forges, mobs, grass and the clock, and streams results.
    const isClient = this.net && this.net.isClient;
    const isHost = this.net && this.net.isHost;

    // guests riding a ghost boat simulate the ride locally (their pose stream
    // carries it; the host pins the real boat underneath them)
    if (isClient && this.player.mount && this.player.mount.ghost) {
      clientRideBoat(this.player.mount, dt, this.player, input, this.world, this.net, notify);
    }

    this.world.renderDist = Settings.get("renderDistance");
    this.world.update(this.player.pos[0], this.player.pos[2], 4, 6);
    if (!isClient) this.world.tickBlockEntities(dt);
    if (!isClient) this.world.tickWater(dt);

    // smoothly ramp the underwater post-effect as the camera dips below a surface
    const uwT = this.player.headInWater(this.world) ? 1 : 0;
    this._underwater = (this._underwater || 0) + (uwT - (this._underwater || 0)) * Math.min(1, dt * 9);

    this.interact.update(dt, input, this.player, this.world, this.inventory, {
      onOpenStation: (st, x, y, z) => this.openStation(st, x, y, z),
      onSleep: () => this.trySleep(),
      onEat: (item) => { const ok = this.player.eat(item); if (ok) sfx.eat(); return ok; },
      onSetSpawn: (x, y, z) => this.attuneAnchor(x, y, z),
      onAnchorBroken: (x, y, z) => this.anchorBroken(x, y, z),
      onWarp: () => this.useWayshard(),
      notify,
      net: this.net,
    });

    // The first-person hand: advance its swing/equip/sway clocks, and start a
    // swing on any frame the player actually used what they are holding.
    const vm = this.renderer.viewmodel;
    const held = this.inventory.selectedSlot();
    vm.setItem(held ? held.key : null);
    vm.update(dt, this.player.bobState());
    if (this.interact.swung) vm.swing();

    // Tick entities after interaction so drops spawned this frame are collected
    // (or start falling) immediately.
    this.world.tickEntities(dt, this.entityCtx());

    this.sky.update(dt);
    director.update(dt, this);   // listener pose, footsteps, ambience beds, mob calls
    if (!isClient) this.world.spreadGrass(this.sky.advanced, this.player.pos[0], this.player.pos[2]); // grass creep tracks in-game time (incl. sleep)

    // occasionally spawn passive grazers (sheep + pig) on nearby grass (daylight, capped)
    this._sheepT += dt;
    if (this._sheepT >= 4 && !isClient) {
      this._sheepT = 0;
      const df = this.sky.dayFactor();
      this.world.trySpawnSheep(this.player.pos[0], this.player.pos[2], df);
      this.world.trySpawnPig(this.player.pos[0], this.player.pos[2], df);
      this.world.trySpawnCow(this.player.pos[0], this.player.pos[2], df);
      // hosts also populate around remote players, so mobs exist out there too
      if (isHost) for (const c of this.net.remoteCenters()) {
        this.world.trySpawnSheep(c[0], c[1], df);
        this.world.trySpawnPig(c[0], c[1], df);
        this.world.trySpawnCow(c[0], c[1], df);
      }
    }

    // and zombies on nearby ground at night (capped) — unless monsters are off
    this._zombieT += dt;
    if (this._zombieT >= 4 && !isClient) {
      this._zombieT = 0;
      if (Settings.get("monsters")) {
        this.world.trySpawnZombie(this.player.pos[0], this.player.pos[2], this.sky.dayFactor());
        if (isHost) for (const c of this.net.remoteCenters()) {
          this.world.trySpawnZombie(c[0], c[1], this.sky.dayFactor());
        }
      }
    }

    // periodic autosave so a crash / accidental close costs at most ~15 minutes
    // (guests don't save — their progress lives in the host's world file)
    this._autosaveT += dt;
    if (this._autosaveT >= 900 && !isClient) { this._autosaveT = 0; this.saveCurrent(); }

    if (this.net) this.net.update(dt);

    if (this.player.health <= 0) this.respawn();
  }

  // Shared entity-tick context. Hosts add the multi-player combat target list.
  entityCtx() {
    return {
      world: this.world, player: this.player, inventory: this.inventory,
      input: this.input, notify, sky: this.sky,
      players: this.net && this.net.isHost ? this.net.combatTargets() : undefined,
    };
  }

  // While hosting/joined but not in the "playing" state (pause menu, inventory,
  // settings…): the world and connection keep ticking, only input is idle.
  updateNetIdle(dt) {
    const isClient = this.net.isClient;
    this.world.update(this.player.pos[0], this.player.pos[2], 4, 6);
    if (!isClient) {
      this.world.tickBlockEntities(dt);
      this.world.tickWater(dt);
    }
    this.world.tickEntities(dt, this.entityCtx());
    this.sky.update(dt);
    this.net.update(dt);
    if (this.state === "inventory") this.invUI.tick(dt);
    if (this.player.health <= 0) this.respawn();
  }

  // ---------- soul anchor & wayshard ----------
  attuneAnchor(x, y, z) {
    this.spawn = [x + 0.5, y + 1, z + 0.5];
    sfx.craft();
    notify("Soul Anchor attuned — you will wake here.");
  }

  // Breaking the anchor you were bound to unbinds your spawn.
  anchorBroken(x, y, z) {
    if (!this.spawn) return;
    if (Math.abs(this.spawn[0] - (x + 0.5)) < 0.01 &&
        Math.abs(this.spawn[1] - (y + 1)) < 0.01 &&
        Math.abs(this.spawn[2] - (z + 0.5)) < 0.01) {
      const [sx, sz] = SPAWN_XZ;
      this.spawn = [sx, this.world.spawnHeight(Math.floor(sx), Math.floor(sz)), sz];
      notify("The Soul Anchor breaks — your spawn returns to the world origin.");
    }
  }

  // Wayshard: consume to warp to the surface directly above. Returns true if
  // the warp happened (the caller then consumes the item).
  useWayshard() {
    if (this.player.mount) { notify("Not while riding — dismount first"); return false; }
    const p = this.player.pos;
    const ts = this.world.topSolidY(Math.floor(p[0]), Math.floor(p[2]));
    if (ts < 0) { notify("The wayshard can't find the sky here"); return false; }
    if (p[1] >= ts - 0.5) { notify("You're already under the open sky"); return false; }
    this.player.pos = [p[0], ts + 1, p[2]];
    this.player.vel = [0, 0, 0];
    this.player._fallStart = null;
    // tell the host this vertical jump is a wayshard, not a speed hack, so the
    // movement sanity check doesn't snap us back underground
    if (this.net && this.net.isClient) this.net.sendWarp();
    sfx.warp();
    notify("The wayshard shatters — daylight.");
    return true;
  }

  respawn() {
    sfx.died();
    if (this.player.mount) {
      const m = this.player.mount;
      m.data.rider = false;
      m.localPin = false;
      if (m.ghost && this.net && this.net.isClient && m.netId != null) this.net.sendBoatMount(m.netId, false);
      this.player.mount = null;
    }
    // pin a death waypoint on the atlas (replacing the previous one)
    if (Settings.get("deathWaypoints")) {
      const [px, py, pz] = this.player.pos;
      this.waypoints = (this.waypoints || []).filter((w) => !w.death);
      this.waypoints.push({
        x: Math.round(px * 10) / 10, y: Math.round(py), z: Math.round(pz * 10) / 10,
        name: "Where you fell", color: "#e05252", death: true,
      });
      this.map.onWaypointsChanged();
    }
    this.dropItemsOnDeath();
    this.player.pos = this.spawn.slice();
    this.player.vel = [0, 0, 0];
    this.player.health = this.player.maxHealth;
    this.player.hunger = this.player.maxHunger; this.player.saturation = 5;
    this.player.breath = this.player.maxBreath; this.player._exhaustion = 0;
    this.world.primeSpawn(this.spawn[0], this.spawn[2]);
    notify("You blacked out and woke at spawn. Your things are where you fell.");
  }

  // Scatter the player's inventory + armour at the death site as tossed drops,
  // so you can run back and recover them before they despawn (10 min).
  // Guests route each toss to the host (drops are host-owned entities).
  dropItemsOnDeath() {
    const [px, py, pz] = this.player.pos;
    const isClient = this.net && this.net.isClient;
    const toss = (s) => {
      if (!s) return;
      const d = [Math.random() * 2 - 1, 0.2, Math.random() * 2 - 1];
      if (isClient) this.net.sendToss([px, py + 0.6, pz], d, s.key, s.count, s.dura);
      else this.world.spawnTossed(px, py + 0.6, pz, d, s.key, s.count, s.dura);
    };
    for (let i = 0; i < this.inventory.slots.length; i++) { toss(this.inventory.slots[i]); this.inventory.slots[i] = null; }
    for (let i = 0; i < this.inventory.armor.length; i++) { toss(this.inventory.armor[i]); this.inventory.armor[i] = null; }
  }

  // Drop the selected hotbar item (Q = one, Ctrl+Q = the whole stack) in front.
  dropSelected(all) {
    const inv = this.inventory;
    const slot = inv.selectedSlot();
    if (!slot) return;
    const count = all ? slot.count : 1;
    const key = slot.key, dura = slot.dura;
    if (all || slot.count <= 1) inv.slots[inv.selected] = null;
    else slot.count -= 1;
    const eye = this.player.eye();
    const dir = this.player.forward();
    const p = [eye[0] + dir[0] * 0.6, eye[1] - 0.1, eye[2] + dir[2] * 0.6];
    if (this.net && this.net.isClient) this.net.sendToss(p, dir, key, count, dura);
    else this.world.spawnTossed(p[0], p[1], p[2], dir, key, count, dura);
    sfx.toss();
  }

  // Sleep in a bed: fast-forward to morning (only at night). In multiplayer
  // it's a vote — everyone in the world has to be in bed.
  trySleep() {
    if (this.sky.isSleeping()) return;
    if (!this.sky.isNight()) { notify("You can only sleep at night."); return; }
    if (this.net && this.net.isClient) {
      this.net.voteSleep(true);
      notify("Waiting for the others to sleep…");
      return;
    }
    if (this.net && this.net.isHost && this.net.activePeers().length) {
      this.net.hostVoteSleep();
      return;
    }
    this.sky.startSleep();
    notify("Sleeping…");
  }

  // ---------- menu background (a static panorama skybox) ----------
  // The menu shows a still panorama captured from a world (the player's last, or a
  // baked default), rendered as a rotating skybox — see render/panorama.js. Zero
  // world simulation, so it costs almost nothing.
  async initMenuBackground() {
    try {
      let faces = null;
      if (Settings.get("menuPanorama")) {
        faces = await getMenuPanorama();
        if (!faces) {                              // first ever launch: bake a default
          faces = this._bakeDefaultPanorama();
          if (faces) await setMenuPanorama(faces, { def: true });
        }
      }
      if (faces) await new Promise((r) => this.skybox.setFaces(faces, r));
    } catch (e) {
      console.warn("Menu background unavailable:", e);
    }
  }

  // One-time default: synchronously build a curated scenic world, capture six
  // faces from a raised vantage, then throw the world away. Runs behind the boot
  // screen so its cost is never seen; afterward the menu is a pure static skybox.
  _bakeDefaultPanorama() {
    try {
      const seed = 0x50f7a3;
      const spot = this._scenicSpot(seed);
      const w = new World(this.gl, this.atlas, seed, 6);
      w.primeArea(spot[0], spot[2], 5);
      const sky = new Sky(); sky.time = 0.40;      // bright mid-morning
      const eye = [spot[0], Math.max(spot[1], SEA_LEVEL) + 15, spot[2]];
      const faces = this.renderer.capturePanorama(w, sky, eye, 1024);
      w.dispose();
      return faces;
    } catch (e) {
      console.warn("Default panorama bake failed:", e);
      return null;
    }
  }

  // Scan a grid of candidate centres and keep the one with the best mix of dry
  // land, nearby water and vertical relief, so the default opens on a scenic coast
  // with hills rather than flat plains or open ocean. heightAt is cheap.
  _scenicSpot(seed) {
    let best = [0.5, SEA_LEVEL + 6, 0.5], bestScore = -1;
    const STEP = 48, SPAN = 6;                    // (2*SPAN+1)^2 = 169 candidates
    for (let gz = -SPAN; gz <= SPAN; gz++) {
      for (let gx = -SPAN; gx <= SPAN; gx++) {
        const cx = gx * STEP, cz = gz * STEP;
        let lo = 999, hi = -999, land = 0, water = 0;
        for (let s = 0; s < 9; s++) {
          const h = heightAt(seed, cx + (s % 3 - 1) * 14, cz + ((s / 3 | 0) - 1) * 14);
          if (h < lo) lo = h;
          if (h > hi) hi = h;
          if (h > SEA_LEVEL + 1) land++; else water++;
        }
        const coast = (land > 0 && water > 0) ? 1 : 0;
        const score = (hi - lo) + coast * 16 + land;
        if (score > bestScore) { bestScore = score; best = [cx + 0.5, hi, cz + 0.5]; }
      }
    }
    return best;
  }

  // Capture the menu panorama from where the player logged out (on Save & Quit).
  // Synchronous capture (the canvas is briefly square, hidden behind the pause
  // overlay); the six faces become the new menu background and are persisted.
  captureMenuPanorama() {
    if (!this.world || !this.player || !Settings.get("menuPanorama")) return;
    try {
      const faces = this.renderer.capturePanorama(this.world, this.sky, this.player.eye(), 1024);
      this.skybox.setFaces(faces, () => {});
      setMenuPanorama(faces, { world: this.meta && this.meta.name }).catch(() => {});
    } catch (e) { console.warn("Menu panorama capture failed:", e); }
  }

  // F2: flat screenshot of the current frame -> gallery. Reads the framebuffer
  // that was just rendered this turn (see loop), so it captures what you see.
  takeScreenshot() {
    try {
      const { data, w, h } = this.renderer.captureFlat(0.92);
      const world = this.meta && this.meta.name;
      makeThumb(data, 360).then((thumb) => addScreenshot(data, thumb, w, h, world)).catch(() => {});
      sfx.shutter();
      notify("Screenshot saved to gallery");
    } catch (e) { console.warn(e); notify("Screenshot failed"); }
  }

  // F8: capture a panorama at the player's spot -> gallery (to view / set later).
  takePanorama() {
    try {
      const faces = this.renderer.capturePanorama(this.world, this.sky, this.player.eye(), 1024);
      const world = this.meta && this.meta.name;
      makeThumb(faces[4], 360).then((thumb) => addPanorama(faces, thumb, world)).catch(() => {});
      notify("Panorama captured to gallery");
    } catch (e) { console.warn(e); notify("Panorama failed"); }
  }

  // Apply a gallery panorama as the live menu background (from the gallery UI).
  async setMenuBackground(faces) {
    if (!faces) return;
    await new Promise((r) => this.skybox.setFaces(faces, r));
    setMenuPanorama(faces, {}).catch(() => {});
    document.getElementById("menu").classList.add("has-pano");
    notify("Menu background updated");
  }

  renderMenuScene(dt) {
    const el = document.getElementById("menu");
    if (Settings.get("menuPanorama") && this.skybox.ready()) {
      el.classList.add("has-pano");
      this._menuT += dt;
      const t = this._menuT;
      const yaw = t * 0.03;                        // slow drift, ~3.5 min per turn
      const pitch = -0.06 + Math.sin(t * 0.08) * 0.05;
      this.skybox.render(yaw, pitch, 72, this.canvas.width, this.canvas.height);
    } else {
      el.classList.remove("has-pano");             // -> the CSS gradient shows instead
    }
  }

  renderFrame() {
    const aspect = this.canvas.width / this.canvas.height;
    this.camera.setProjection(aspect, Settings.get("fov"));
    // The camera eye gets the walking head-bob; the interaction eye() does not, so
    // aim stays steady. The same bob phase drives the held-item sway.
    const eye = this.player.eye();
    const b = this.player.viewBobOffset();
    this.camera.update([eye[0] + b[0], eye[1] + b[1], eye[2] + b[2]], this.player.yaw, this.player.pitch);

    const selection = this.state === "playing" ? this.interact.selection : null;
    const slot = this.inventory.selectedSlot();

    this.renderer.render(this.world, this.camera, this.sky, selection, slot ? slot.key : null, this._underwater || 0);

    // floating names over remote players
    if (this.net && this.net.ghosts && this.net.ghosts.players.size) {
      this.nameplates.update(this.camera.viewProj, this.net.ghosts.players, innerWidth, innerHeight);
    } else {
      this.nameplates.clear();
    }

    this.hud.netInfo = this.net
      ? { role: this.net.isHost ? "host" : "client", players: this.net.playerCount(),
          ping: this.net.isClient ? Math.round(this.net.rtt) : 0 }
      : null;
    this.hud.update(this.player, this.inventory, this.world, this.sky, this.fps, this.interact.selection, this.interact.breakFrac);
  }
}

window.__game = new Game();
