// Entry point and command-line parsing.
//
// The flags exist mostly for verification during the port: the web build could
// be poked from the browser console, and these are the native equivalent.

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <thread>
#include <string>

#include "app.h"
#include "core/log.h"
#include "dev/audiodump.h"
#include "dev/golden.h"
#include "dev/packtool.h"
#include "dev/savetool.h"
#include "dev/selftest.h"
#include "net/doctor.h"
#include "platform/paths.h"
#include "platform/update.h"
#include "world/worldgen.h"

namespace {

void printUsage() {
  std::printf(
      "Hollowreach " HR_VERSION " (native)\n"
      "\n"
      "  --dump-golden <file>  write determinism vectors and exit (\"-\" for stdout)\n"
      "  --sections <list>     limit --dump-golden to prng,noise,fields,worldgen,chunks\n"
      "  --dump-atlas <png>    build the texture atlas headlessly and write it\n"
      "  --dump-recipes <file> write the crafting, smelting and fuel tables and exit\n"
      "  --dump-loot <file>    write the loot tables and exit (\"-\" for stdout)\n"
      "  --dump-theme <file>   write the resolved interface theme and exit; the\n"
      "                        output is a valid theme.json to start a UI pack from\n"
      "  --find-dungeon        print the nearest dungeon to spawn and exit\n"
      "  --find-crop           print the wild crops near spawn and exit\n"
      "  --dump-audio <event> <path>  render one sound event to a wav and exit\n"
      "                        (\"all\" writes every event into a directory;\n"
      "                         \"list\" prints the event names)\n"
      "  --list-packs [all]    show installed resource packs and what each sound\n"
      "                        event resolves to, then exit\n"
      "  --example-pack [dir]  write a fill-in-the-blanks sound pack and exit\n"
      "  --selftest            run the headless behaviour checks and exit\n"
      "  --list-worlds         list the saved worlds and exit\n"
      "  --save-info <id|path> describe one save file and exit\n"
      "  --export-world <id> [path]  copy a world out for sharing and exit\n"
      "  --import-world <path> copy a world in under a fresh id and exit\n"
      "  --tile-res <px>       atlas tile resolution cap (default 128)\n"
      "  --mipmaps             generate atlas mipmaps\n"
      "  --data-dir <path>   worlds, screenshots and settings location\n"
      "                      (default: data/ beside the executable)\n"
      "  --width <px>        initial window width (default 1280)\n"
      "  --height <px>       initial window height (default 720)\n"
      "  --fullscreen        start fullscreen\n"
      "  --gl-info           print GPU and OpenGL details, then exit\n"
      "  --screenshot <png>  capture one frame to a PNG, then exit\n"
      "  --frames <n>        render n frames, then exit\n"
      "  --world [id]        open a saved world; with no id, generate a fresh one\n"
      "  --new-world <name>  create a saved world and enter it\n"
      "  --seed <n>          world seed (default 3918175327)\n"
      "  --render-distance <n>  view radius in chunks (default: the saved setting)\n"
      "  --at x,y,z[,yaw,pitch] exact camera placement, for reproducible captures\n"
      "  --time <0..1>       pin the sky clock (0 midnight, 0.5 noon)\n"
      "  --quality <preset>  low | medium | high | ultra (default: the saved setting)\n"
      "  --threads <n>       chunk worker threads (default: cores-1; 0 = inline)\n"
      "  --debug-view <n>    1 = ambient occlusion term, 2 = sun shadow term\n"
      "  --no-hud            hide the hotbar, hearts and crosshair (F1 in game)\n"
      "  --freeze            no player physics, so --at is held exactly\n"
      "  --no-vsync          uncap the frame rate, for measurement\n"
      "  --perf              per-pass GPU and CPU timing, and a report at exit\n"
      "  --give <list>       start holding items: key[:count][,key[:count]...]\n"
      "  --spawn <list>      place entities near spawn: type[:count][,...]\n"
      "  --net-doctor        check this machine's networking for multiplayer, and exit\n"
      "  --check-update [ver] ask GitHub for the latest release, print it, and exit\n"
      "  --apply-update [ver] download the latest release and install it over this one\n"
      "  --hang <png>        build a wall at spawn with this picture in a painting\n"
      "  --screen <name>     open a screen straight away: menu, worlds, newworld,\n"
      "                      about, join, pause, settings, inventory, workbench,\n"
      "                      forge, chest, recipes, map, gallery, packs, bed,\n"
      "                      bed-early, chat, chat-suggest\n"
      "  --command <line>    type a line into chat once the session is up, as if by\n"
      "                      hand; repeatable. A leading slash makes it a command,\n"
      "                      anything else is said out loud. Answers go to the log\n"
      "                      as well as the box: --command \"/give pick_stone\"\n"
      "  --dump-icons <png>  write the generated inventory icon sheet and exit\n"
      "  --verbose           log at debug level\n"
      "  --help              this message\n");
}

// Reads the value following a flag, reporting a missing operand rather than
// walking off the end of argv.
bool takeValue(int argc, char** argv, int& i, const char* flag, std::string& out) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "%s needs a value\n", flag);
    return false;
  }
  out = argv[++i];
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  hr::AppOptions options;
  std::string audioEvent;
  std::string audioPath;
  std::string goldenPath;
  std::string goldenSections;
  std::string atlasPath;
  std::string recipesPath;
  std::string lootPath;
  std::string themePath;
  bool findDungeon = false;
  bool findCrop = false;
  int listPacksMode = 0;  // 0 off, 1 replacements only, 2 every event
  bool examplePack = false;
  std::string examplePackDir;
  int atlasTileRes = 128;
  bool atlasMipmaps = false;
  bool runSelfTest = false;
  bool checkUpdate = false;
  bool netDoctor = false;
  bool applyUpdate = false;
  std::string pretendVersion;
  bool listWorldsOnly = false;
  std::string saveInfoTarget;
  std::string exportId;
  std::string exportPath;
  std::string importPath;
  // Set by any flag that only makes sense inside a world; see the note below.
  bool wantsWorld = false;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    std::string value;

    if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
      printUsage();
      return 0;
    } else if (std::strcmp(arg, "--data-dir") == 0) {
      if (!takeValue(argc, argv, i, arg, options.dataDir)) return 2;
    } else if (std::strcmp(arg, "--width") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      options.width = std::atoi(value.c_str());
    } else if (std::strcmp(arg, "--height") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      options.height = std::atoi(value.c_str());
    } else if (std::strcmp(arg, "--fullscreen") == 0) {
      options.fullscreen = true;
    } else if (std::strcmp(arg, "--gl-info") == 0) {
      options.listDevices = true;
    } else if (std::strcmp(arg, "--screenshot") == 0) {
      if (!takeValue(argc, argv, i, arg, options.screenshotPath)) return 2;
    } else if (std::strcmp(arg, "--frames") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      options.exitAfterFrames = std::atoi(value.c_str());
    } else if (std::strcmp(arg, "--dump-golden") == 0) {
      if (!takeValue(argc, argv, i, arg, goldenPath)) return 2;
    } else if (std::strcmp(arg, "--sections") == 0) {
      if (!takeValue(argc, argv, i, arg, goldenSections)) return 2;
    } else if (std::strcmp(arg, "--dump-atlas") == 0) {
      if (!takeValue(argc, argv, i, arg, atlasPath)) return 2;
    } else if (std::strcmp(arg, "--dump-recipes") == 0) {
      if (!takeValue(argc, argv, i, arg, recipesPath)) return 2;
    } else if (std::strcmp(arg, "--dump-loot") == 0) {
      if (!takeValue(argc, argv, i, arg, lootPath)) return 2;
    } else if (std::strcmp(arg, "--dump-theme") == 0) {
      if (!takeValue(argc, argv, i, arg, themePath)) return 2;
    } else if (std::strcmp(arg, "--find-dungeon") == 0) {
      findDungeon = true;
    } else if (std::strcmp(arg, "--find-crop") == 0) {
      findCrop = true;
    } else if (std::strcmp(arg, "--list-packs") == 0) {
      // A bare --list-packs shows only what the packs replace; "all" adds the
      // events still being synthesised, which is the list you fill a pack from.
      listPacksMode = 1;
      if (i + 1 < argc && std::strcmp(argv[i + 1], "all") == 0) {
        ++i;
        listPacksMode = 2;
      }
    } else if (std::strcmp(arg, "--example-pack") == 0) {
      examplePack = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') examplePackDir = argv[++i];
    } else if (std::strcmp(arg, "--dump-audio") == 0) {
      if (!takeValue(argc, argv, i, arg, audioEvent)) return 2;
      if (audioEvent != "list" && !takeValue(argc, argv, i, arg, audioPath)) return 2;
    } else if (std::strcmp(arg, "--tile-res") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      atlasTileRes = std::atoi(value.c_str());
    } else if (std::strcmp(arg, "--mipmaps") == 0) {
      atlasMipmaps = true;
    } else if (std::strcmp(arg, "--world") == 0) {
      // A bare --world still means "generate a fresh one", which is what every
      // capture command from the earlier milestones uses it for; an id after it
      // opens that save instead. Peeked rather than required, so both keep working.
      if (i + 1 < argc && argv[i + 1][0] != '-') options.worldId = argv[++i];
      wantsWorld = true;
    } else if (std::strcmp(arg, "--threads") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      options.threads = std::atoi(value.c_str());
    } else if (std::strcmp(arg, "--host") == 0) {
      options.hostGame = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        options.hostPort = static_cast<std::uint16_t>(std::atoi(argv[++i]));
      }
      wantsWorld = true;
    } else if (std::strcmp(arg, "--join") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      const std::size_t colon = value.rfind(':');
      if (colon != std::string::npos) {
        options.joinAddress = value.substr(0, colon);
        options.joinPort = static_cast<std::uint16_t>(std::atoi(value.c_str() + colon + 1));
      } else {
        options.joinAddress = value;
      }
      if (options.joinPort == 0) options.joinPort = hr::net::kDefaultGamePort;
    } else if (std::strcmp(arg, "--name") == 0) {
      if (!takeValue(argc, argv, i, arg, options.playerName)) return 2;
    } else if (std::strcmp(arg, "--new-world") == 0) {
      if (!takeValue(argc, argv, i, arg, options.newWorldName)) return 2;
      wantsWorld = true;
    } else if (std::strcmp(arg, "--list-worlds") == 0) {
      listWorldsOnly = true;
    } else if (std::strcmp(arg, "--save-info") == 0) {
      if (!takeValue(argc, argv, i, arg, saveInfoTarget)) return 2;
    } else if (std::strcmp(arg, "--export-world") == 0) {
      if (!takeValue(argc, argv, i, arg, exportId)) return 2;
      if (i + 1 < argc && argv[i + 1][0] != '-') exportPath = argv[++i];
    } else if (std::strcmp(arg, "--import-world") == 0) {
      if (!takeValue(argc, argv, i, arg, importPath)) return 2;
    } else if (std::strcmp(arg, "--seed") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      options.seed = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
      wantsWorld = true;
    } else if (std::strcmp(arg, "--render-distance") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      options.renderDistance = std::atoi(value.c_str());
      wantsWorld = true;
    } else if (std::strcmp(arg, "--at") == 0) {
      // x,y,z,yaw,pitch — the native equivalent of poking the browser console, and
      // what makes an A/B screenshot against the web build line up.
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      float parts[5] = {0, 0, 0, 0, 0};
      int n = 0;
      const char* p = value.c_str();
      while (n < 5 && *p) {
        parts[n++] = std::strtof(p, const_cast<char**>(&p));
        if (*p == ',') ++p;
      }
      if (n < 3) {
        std::fprintf(stderr, "--at needs at least x,y,z\n");
        return 2;
      }
      options.haveSpawnOverride = true;
      wantsWorld = true;
      options.spawnX = parts[0];
      options.spawnY = parts[1];
      options.spawnZ = parts[2];
      options.spawnYaw = parts[3];
      options.spawnPitch = parts[4];
    } else if (std::strcmp(arg, "--time") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      options.skyTime = static_cast<float>(std::atof(value.c_str()));
      wantsWorld = true;
    } else if (std::strcmp(arg, "--quality") == 0) {
      if (!takeValue(argc, argv, i, arg, options.quality)) return 2;
    } else if (std::strcmp(arg, "--no-hud") == 0) {
      options.hideHud = true;
      wantsWorld = true;
    } else if (std::strcmp(arg, "--no-vsync") == 0) {
      options.noVsync = true;
    } else if (std::strcmp(arg, "--perf") == 0) {
      options.perf = true;
      options.noVsync = true;  // a vsync-flattened frame time measures the display
    } else if (std::strcmp(arg, "--freeze") == 0) {
      options.freezePlayer = true;
      wantsWorld = true;
    } else if (std::strcmp(arg, "--creative") == 0) {
      options.creativeWorld = true;
      wantsWorld = true;
    } else if (std::strcmp(arg, "--debug-lines") == 0) {
      options.debugLines = true;
    } else if (std::strcmp(arg, "--debug-view") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      options.debugView = std::atoi(value.c_str());
      wantsWorld = true;
    } else if (std::strcmp(arg, "--give") == 0) {
      // key[:count] pairs, comma separated. The browser build could be handed
      // items from the console; this is the native equivalent, and it is how the
      // crafting chain gets exercised before the inventory screen exists.
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      std::size_t start = 0;
      while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        std::string entry = value.substr(start, comma - start);
        if (!entry.empty()) {
          int count = 1;
          const std::size_t colon = entry.find(':');
          if (colon != std::string::npos) {
            count = std::atoi(entry.c_str() + colon + 1);
            entry.resize(colon);
          }
          if (count > 0) options.startingItems.emplace_back(entry, count);
          wantsWorld = true;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    } else if (std::strcmp(arg, "--spawn") == 0) {
      // type[:count] pairs, the same shape as --give. Mob spawning is timed, capped
      // and gated on the time of day, so without this there is no way to capture a
      // sheep — or to walk a zombie's chase — reproducibly.
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      std::size_t start = 0;
      while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        std::string entry = value.substr(start, comma - start);
        if (!entry.empty()) {
          int count = 1;
          const std::size_t colon = entry.find(':');
          if (colon != std::string::npos) {
            count = std::atoi(entry.c_str() + colon + 1);
            entry.resize(colon);
          }
          if (count > 0) options.startingEntities.emplace_back(entry, count);
          wantsWorld = true;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    } else if (std::strcmp(arg, "--hang") == 0) {
      if (!takeValue(argc, argv, i, arg, options.hangPicture)) return 2;
      wantsWorld = true;
    } else if (std::strcmp(arg, "--dump-icons") == 0) {
      if (!takeValue(argc, argv, i, arg, options.iconDumpPath)) return 2;
    } else if (std::strcmp(arg, "--command") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      options.startupCommands.push_back(value);
      // Not `wantsWorld = true`: /help and /op need no world, and forcing one would
      // make a scripted `--command "/op ada"` generate terrain to do nothing with.
      // A command that does need one says so itself and refuses with a reason.
    } else if (std::strcmp(arg, "--screen") == 0) {
      if (!takeValue(argc, argv, i, arg, options.startScreen)) return 2;
    } else if (std::strcmp(arg, "--mouse") == 0) {
      if (!takeValue(argc, argv, i, arg, value)) return 2;
      float parts[2] = {0, 0};
      int n = 0;
      const char* p = value.c_str();
      while (n < 2 && *p) {
        parts[n++] = std::strtof(p, const_cast<char**>(&p));
        if (*p == ',') ++p;
      }
      if (n < 2) {
        std::fprintf(stderr, "--mouse needs x,y\n");
        return 2;
      }
      options.haveMouseOverride = true;
      options.mouseX = parts[0];
      options.mouseY = parts[1];
    } else if (std::strcmp(arg, "--net-doctor") == 0) {
      netDoctor = true;
    } else if (std::strcmp(arg, "--check-update") == 0) {
      checkUpdate = true;
      // An optional version to pretend to be, so the "an update exists" path can
      // be exercised against a release that is not actually newer than this build.
      if (i + 1 < argc && argv[i + 1][0] != '-') pretendVersion = argv[++i];
    } else if (std::strcmp(arg, "--apply-update") == 0) {
      checkUpdate = true;
      applyUpdate = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') pretendVersion = argv[++i];
    } else if (std::strcmp(arg, "--selftest") == 0) {
      runSelfTest = true;
    } else if (std::strcmp(arg, "--verbose") == 0) {
      hr::log::setMinLevel(hr::log::Level::Debug);
    } else {
      std::fprintf(stderr, "Unknown option: %s\n\n", arg);
      printUsage();
      return 2;
    }
  }

  // Headless, and before anything touches a window or GL: this must run in a
  // console build and could run in CI.
  if (!goldenPath.empty()) {
    return hr::dev::dumpGolden(goldenPath, goldenSections) ? 0 : 1;
  }
  if (!atlasPath.empty()) {
    return hr::dev::dumpAtlas(atlasPath, atlasTileRes, atlasMipmaps) ? 0 : 1;
  }
  if (!recipesPath.empty()) {
    return hr::dev::dumpRecipes(recipesPath) ? 0 : 1;
  }
  if (!lootPath.empty()) {
    return hr::dev::dumpLoot(lootPath) ? 0 : 1;
  }
  if (!themePath.empty()) {
    // Needs the data directory for the same reason --dump-audio does: the dump is
    // of the theme as it actually resolves, which is through whatever packs are on.
    hr::paths::init(options.dataDir);
    return hr::dev::dumpTheme(themePath) ? 0 : 1;
  }
  if (findDungeon) {
    // Uses --seed, so a specific world can be asked about. Headless like the dumps
    // above: it is arithmetic on the seed and generation, and needs no window.
    return hr::dev::dungeonInfo(options.seed);
  }
  if (findCrop) {
    return hr::dev::cropInfo(options.seed);
  }
  // Both need paths::init for data/resourcepacks, and nothing else — no window,
  // no world, no audio device. Reading a pack is a directory walk and a decode.
  if (listPacksMode > 0) {
    hr::paths::init(options.dataDir);
    return hr::dev::listPacks(listPacksMode == 2);
  }
  if (examplePack) {
    hr::paths::init(options.dataDir);
    hr::paths::ensureDirs();
    return hr::dev::makeExamplePack(examplePackDir);
  }
  if (!audioEvent.empty()) {
    // Needs the data directory: the dump renders through whatever resource packs
    // are enabled, and those live under it.
    hr::paths::init(options.dataDir);
    if (audioEvent == "list") {
      hr::dev::listAudioEvents();
      return 0;
    }
    return hr::dev::dumpAudio(audioEvent, audioPath) ? 0 : 1;
  }
  if (runSelfTest) {
    return hr::dev::runSelfTest();
  }
  // Needs paths::init for the executable path it reports, and nothing else —
  // no window, no world, no GL. It has to run on the machine that is failing,
  // which may well be one that cannot open a window at all.
  if (netDoctor) {
    hr::paths::init(options.dataDir);
    return hr::net::runNetDoctor(options.hostPort);
  }
  // The updater's check, headless. It is the one part of the game that depends on
  // a live server, so it needs a way to be exercised that does not involve
  // clicking a button and reading a label.
  if (checkUpdate) {
    hr::paths::init(options.dataDir);
    if (!hr::platform::update::supported()) {
      std::printf("updating is not supported on this platform\n");
      return 1;
    }
    using hr::platform::update::Stage;
    const auto settle = [](Stage busy) {
      for (int i = 0; i < 1200; ++i) {
        if (hr::platform::update::state().stage != busy) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      return false;
    };

    hr::platform::update::check(pretendVersion);
    if (!settle(Stage::Checking)) {
      std::printf("the check timed out\n");
      return 1;
    }
    hr::platform::update::State s = hr::platform::update::state();
    std::printf("this build : %s\n", HR_VERSION);
    std::printf("latest     : %s\n", s.latest.empty() ? "(unknown)" : s.latest.c_str());
    std::printf("result     : %s\n", s.message.c_str());
    if (s.stage == Stage::Failed) return 1;

    // With a pretend version the point is to exercise the download and the
    // unpack, so go the rest of the way — but never as far as apply(), which
    // would replace the build under test with the one it just fetched.
    if (!pretendVersion.empty() && s.stage == Stage::Available) {
      hr::platform::update::download();
      if (!settle(Stage::Downloading) || !settle(Stage::Staging)) {
        std::printf("the download timed out\n");
        return 1;
      }
      s = hr::platform::update::state();
      std::printf("staged     : %s\n", s.message.c_str());
      if (s.stage != Stage::ReadyToApply) return 1;
      // --apply-update goes the last step too, which is the only way to exercise
      // the handover without a window and a mouse. It replaces the binary that is
      // running it, so it is deliberately a separate flag.
      if (applyUpdate) {
        const bool started = hr::platform::update::apply();
        std::printf("handover   : %s\n", started ? "started" : "FAILED");
        return started ? 0 : 1;
      }
      return 0;
    }
    return 0;
  }
  // The save tools need the data directory resolved, and nothing else.
  if (listWorldsOnly || !saveInfoTarget.empty() || !exportId.empty() || !importPath.empty()) {
    hr::paths::init(options.dataDir);
    if (listWorldsOnly) return hr::dev::listWorlds();
    if (!saveInfoTarget.empty()) return hr::dev::saveInfo(saveInfoTarget);
    if (!exportId.empty()) return hr::dev::exportWorld(exportId, exportPath);
    return hr::dev::importWorld(importPath);
  }

  if (options.width < 320) options.width = 320;
  if (options.height < 240) options.height = 240;

  // The shipped game boots to the menu. Every verification flag that only makes sense
  // inside a world implies one, so the capture commands from the earlier milestones keep
  // working unchanged — and so does `--screen inventory`, which needs somewhere to stand.
  {
    static const char* kWorldScreens[] = {"pause",     "inventory", "workbench", "forge",
                                          "chest",     "recipes",   "map"};
    for (const char* name : kWorldScreens) {
      if (options.startScreen == name) wantsWorld = true;
    }
  }
  options.startWorld = wantsWorld;

  hr::App app;
  return app.run(options);
}

#if defined(_WIN32) && !defined(HR_DEBUG)
// Release builds are windowed (WIN32_EXECUTABLE), so Windows looks for WinMain.
// Forwarding to main keeps one entry point and preserves argv parsing.
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
