#include "resource/pack.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/json.h"
#include "core/log.h"
#include "platform/paths.h"
#include "ui/settings.h"

namespace fs = std::filesystem;

namespace hr::resource {
namespace {

// Enough to walk a pack's sounds/ and textures/ trees for the counts on the
// screen without ever being the reason a startup is slow.
constexpr int kMaxScanFiles = 20000;

bool hasExtension(const fs::path& p, std::initializer_list<const char*> exts) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const char* want : exts) {
    if (e == want) return true;
  }
  return false;
}

// Walks a tree without ever throwing.
//
// `for (auto& e : fs::recursive_directory_iterator(dir, ec))` looks like it
// cannot throw, and that is wrong in a way it took a crash to find: the
// error_code is consumed by the CONSTRUCTOR, while the range-for's `operator++`
// is the throwing overload. Windows' 260-character path limit is what trips it —
// descend into a directory whose full path is longer and the increment fails,
// throws filesystem_error, and with nothing to catch it the process fast-fails
// with no message at all.
//
// A pack is a deep tree by nature (assets/<ns>/sounds/entity/player/attack/…),
// this scan runs at startup, and a player is free to unzip the game anywhere —
// so the game refused to start for anyone whose install path was long enough.
//
// Files past the limit are skipped rather than read. That limitation is the
// platform's, but skipping is the difference between a pack that half loads and
// a game that does not open.
int countFiles(const fs::path& dir, std::initializer_list<const char*> exts) {
  std::error_code open;
  if (!fs::is_directory(dir, open)) return 0;

  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, open);
  const fs::recursive_directory_iterator end;
  if (open) return 0;

  int matched = 0;
  int visited = 0;
  // A probe error_code of its own. Sharing one with the walk is how this used to
  // stop early: a failed existence check is *expected* here and leaves the code
  // set, so anything that then tests it reads the last probe's result rather
  // than the iterator's.
  std::error_code probe;
  while (it != end) {
    // The ceiling counts everything walked, not everything matched: a folder of
    // 200,000 files is slow to walk whether or not any of them are .ogg.
    if (++visited > kMaxScanFiles) break;
    if (it->is_regular_file(probe) && hasExtension(it->path(), exts)) ++matched;
    it.increment(open);
    if (open) break;  // unreadable, or past the path limit — stop, never throw
  }
  return matched;
}

// pack.mcmeta's description is a string in every pack anyone has written by hand,
// and a chat-component object in some generated ones. Both are read, because
// rejecting the object form would show an empty name for a pack that works.
std::string readDescription(const json::Value& description) {
  if (description.isString()) return description.str();
  if (description.isObject()) {
    const std::string text = description["text"].str();
    if (!text.empty()) return text;
  }
  if (description.isArray()) {
    std::string joined;
    for (const json::Value& part : description.items()) {
      if (part.isString()) joined += part.str();
      else if (part.isObject()) joined += part["text"].str();
    }
    return joined;
  }
  return {};
}

// A description may carry Minecraft's section-sign colour codes. They are two
// bytes each and mean nothing here, so they are stripped rather than drawn as
// stray glyphs in the middle of a pack name.
std::string stripFormatting(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    // U+00A7 is 0xC2 0xA7 in UTF-8; the code that follows it is one ASCII byte.
    if (static_cast<unsigned char>(s[i]) == 0xC2 && i + 2 < s.size() &&
        static_cast<unsigned char>(s[i + 1]) == 0xA7) {
      i += 2;
      continue;
    }
    if (s[i] == '\n' || s[i] == '\r') {
      out.push_back(' ');
      continue;
    }
    out.push_back(s[i]);
  }
  return out;
}

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}

}  // namespace

const std::vector<std::string>& namespaceSearchOrder() {
  static const std::vector<std::string> kOrder = {"hollowreach", "minecraft"};
  return kOrder;
}

// ---------------------------------------------------------------------------

bool safeRelativePath(std::string_view path) {
  if (path.empty() || path.size() > 512) return false;
  // Absolute in any spelling: a leading separator, or a drive letter.
  if (path.front() == '/' || path.front() == '\\') return false;
  if (path.size() >= 2 && path[1] == ':') return false;
  // Backslashes are rejected outright rather than normalised. On Windows they
  // are separators and on Linux they are a legal filename character, so a single
  // string would mean two different things — and a pack that uses them is
  // already not portable.
  if (path.find('\\') != std::string_view::npos) return false;
  // Control characters, and the NUL that would truncate the path a layer down.
  for (char c : path) {
    if (static_cast<unsigned char>(c) < 0x20) return false;
  }

  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string_view part =
        path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    // "" catches a leading, trailing or doubled separator; "." and ".." catch
    // the traversal itself. `..` is the one that matters: without this check a
    // sounds.json entry of "../../../../../../windows/win.ini" is a file this
    // process will happily open and decode.
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

std::string packFile(const PackInfo& pack, std::string_view relative) {
  if (!safeRelativePath(relative)) return {};
  return (fs::path(pack.root) / fs::path(std::string(relative))).string();
}

// ---------------------------------------------------------------------------

std::vector<PackInfo> scanPacks() {
  std::vector<PackInfo> out;
  const std::string& dir = paths::resourcePacksDir();
  std::error_code open;
  if (!fs::is_directory(dir, open)) return out;

  // `ec` is a scratch code for the existence probes below, and is deliberately
  // never tested as a loop condition.
  //
  // It used to be one code shared with `if (ec) break;` at the top of the loop,
  // and that quietly truncated the scan to a single pack: probing for a namespace
  // folder that is not there is the NORMAL case — every pack lacks one of
  // hollowreach/ or minecraft/ — so the first pack always left the code set, and
  // the second iteration read it and stopped. With one pack installed everything
  // looked perfect.
  std::error_code ec;
  fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, open);
  const fs::directory_iterator end;
  for (; !open && it != end; it.increment(open)) {
    // Incremented with an error_code for the same reason countFiles is: the
    // range-for's ++ throws, and this walk is on the startup path.
    const fs::directory_entry& entry = *it;
    if (!entry.is_directory(ec)) {
      // A .zip sitting in the folder is the single most likely thing a player
      // will try, so it gets a row that says what to do rather than no row.
      if (entry.is_regular_file(ec) && hasExtension(entry.path(), {".zip"})) {
        PackInfo bad;
        bad.id = entry.path().filename().string();
        bad.name = entry.path().stem().string();
        bad.root = entry.path().string();
        bad.problem = "zip packs are not supported yet — unzip it into this folder";
        out.push_back(std::move(bad));
      }
      continue;
    }

    PackInfo pack;
    pack.id = entry.path().filename().string();
    pack.root = entry.path().string();
    pack.name = pack.id;
    // The id goes into a single settings string; a separator inside one would
    // split it into two ids that name nothing.
    if (pack.id.find('|') != std::string::npos) {
      pack.problem = "the folder name cannot contain '|'";
      out.push_back(std::move(pack));
      continue;
    }

    const fs::path meta = entry.path() / "pack.mcmeta";
    if (fs::is_regular_file(meta, ec)) {
      std::string error;
      const json::Value doc = json::parseFile(meta.string(), &error);
      if (!error.empty()) {
        // Not fatal. A pack whose metadata is malformed still has its files, and
        // refusing it would be refusing the content over the label.
        pack.problem = "pack.mcmeta could not be read (" + error + ")";
      } else {
        const json::Value& section = doc["pack"];
        pack.packFormat = static_cast<int>(section["pack_format"].num(0));
        pack.description = trim(stripFormatting(readDescription(section["description"])));
        if (!pack.description.empty()) pack.name = pack.description;
      }
    }

    const fs::path assets = entry.path() / "assets";
    for (const std::string& ns : namespaceSearchOrder()) {
      const fs::path nsDir = assets / ns;
      if (!fs::is_directory(nsDir, ec)) continue;
      pack.namespaces.push_back(ns);
      pack.soundFiles += countFiles(nsDir / "sounds", {".ogg", ".wav"});
      pack.textureFiles += countFiles(nsDir / "textures", {".png"});
      if (fs::is_regular_file(nsDir / "sounds.json", ec)) pack.hasSoundsJson = true;
      if (fs::is_regular_file(nsDir / "ui" / "theme.json", ec)) pack.hasUiTheme = true;
    }

    if (pack.namespaces.empty() && pack.problem.empty()) {
      // The commonest packaging mistake by a distance: a zip that contained the
      // pack folder, extracted so that assets/ is one level deeper than it looks.
      pack.problem = fs::is_directory(entry.path() / "assets", ec)
                         ? "assets/ has no hollowreach/ or minecraft/ folder inside it"
                         : "no assets/ folder — is there an extra folder in between?";
    }
    out.push_back(std::move(pack));
  }

  std::sort(out.begin(), out.end(),
            [](const PackInfo& a, const PackInfo& b) { return a.id < b.id; });
  return out;
}

// ---------------------------------------------------------------------------

std::vector<std::string> enabledPackIds() {
  std::vector<std::string> out;
  const std::string& raw = ui::settings().text(kEnabledSetting);
  std::size_t start = 0;
  while (start <= raw.size()) {
    const std::size_t bar = raw.find('|', start);
    std::string id = raw.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
    id = trim(id);
    if (!id.empty()) out.push_back(id);
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  return out;
}

void setEnabledPackIds(const std::vector<std::string>& ids) {
  std::string joined;
  for (const std::string& id : ids) {
    if (id.empty() || id.find('|') != std::string::npos) continue;
    if (!joined.empty()) joined.push_back('|');
    joined += id;
  }
  ui::settings().setText(kEnabledSetting, joined);
}

std::vector<PackInfo> enabledPacks(const std::vector<PackInfo>& installed) {
  std::vector<PackInfo> out;
  for (const std::string& id : enabledPackIds()) {
    for (const PackInfo& pack : installed) {
      if (pack.id != id || !pack.usable()) continue;
      out.push_back(pack);
      break;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------

bool writeExamplePack(const std::string& directory, const std::vector<std::string>& events,
                      std::string* errorOut) {
  std::error_code ec;
  const fs::path root(directory);
  const std::string ns = namespaceSearchOrder().front();
  const fs::path assets = root / "assets" / ns;

  fs::create_directories(assets / "sounds", ec);
  if (ec) {
    if (errorOut) *errorOut = "cannot create " + assets.string() + " (" + ec.message() + ")";
    return false;
  }

  {
    std::ofstream meta(root / "pack.mcmeta", std::ios::binary);
    if (!meta) {
      if (errorOut) *errorOut = "cannot write pack.mcmeta";
      return false;
    }
    meta << "{\n  \"pack\": {\n    \"pack_format\": " << kPackFormat
         << ",\n    \"description\": \"Example sound pack\"\n  }\n}\n";
  }

  // Deliberately NOT written as `sounds.json`.
  //
  // A generated sounds.json listing all 78 events points every one of them at a
  // file that is not there yet, so a pack you just created reports 78 broken
  // entries — which buries the one real message the moment there is one. The
  // convention path exists precisely so that a pack needs no sounds.json at all:
  // drop a file where the event name says and it is picked up. This file is the
  // complete reference for when a pack outgrows that and wants weights, variants
  // or per-sound volume; renaming it is the whole of "opting in".
  //
  // The folder tree IS created, for every event, because an empty tree is far
  // easier to fill than a list of paths somebody has to reconstruct by hand.
  {
    std::ofstream sounds(assets / "sounds.example.json", std::ios::binary);
    if (!sounds) {
      if (errorOut) *errorOut = "cannot write sounds.example.json";
      return false;
    }
    sounds << "{\n";
    for (std::size_t i = 0; i < events.size(); ++i) {
      std::string path = events[i];
      std::replace(path.begin(), path.end(), '.', '/');
      const fs::path folder = assets / "sounds" / fs::path(path).parent_path();
      fs::create_directories(folder, ec);
      ec.clear();
      sounds << "  \"" << json::escape(events[i]) << "\": {\n"
             << "    \"sounds\": [\n"
             << "      { \"name\": \"" << json::escape(path)
             << "\", \"volume\": 1.0, \"pitch\": 1.0, \"weight\": 1 }\n"
             << "    ]\n"
             << "  }" << (i + 1 < events.size() ? "," : "") << "\n";
    }
    sounds << "}\n";
  }

  // The same information as one path per line, which is what you want open in a
  // second window while filling the tree in.
  {
    std::ofstream list(root / "EVENTS.txt", std::ios::binary);
    if (list) {
      list << "Every sound this build can play, and the file that replaces it.\n"
              "Paths are relative to assets/"
           << ns << "/sounds/ and may be .ogg or .wav.\n\n";
      for (const std::string& event : events) {
        std::string path = event;
        std::replace(path.begin(), path.end(), '.', '/');
        list << event << "\n    " << path << ".ogg\n";
      }
    }
  }

  {
    std::ofstream readme(root / "README.txt", std::ios::binary);
    if (readme) {
      readme << "Hollowreach sound pack\n"
                "======================\n\n"
                "THE SHORT VERSION\n"
                "  Drop an .ogg or .wav into assets/"
             << ns
             << "/sounds/ at the path the\n"
                "  event name spells out, with the dots turned into slashes:\n\n"
                "      block.stone.break  ->  assets/"
             << ns
             << "/sounds/block/stone/break.ogg\n"
                "      entity.cow.hurt    ->  assets/"
             << ns
             << "/sounds/entity/cow/hurt.wav\n\n"
                "  The folders are already here, empty, one per event. EVENTS.txt lists\n"
                "  every event and the file that replaces it. That is the whole job --\n"
                "  no sounds.json needed.\n\n"
                "  Variants work too: break1.ogg .. break4.ogg alongside (or instead of)\n"
                "  break.ogg are chosen between at random.\n\n"
                "ANYTHING YOU LEAVE OUT\n"
                "  keeps the game's own synthesised sound. A pack that replaces four\n"
                "  sounds is a perfectly good pack.\n\n"
                "THE LONGER VERSION (per-sound volume, pitch and weight)\n"
                "  Rename sounds.example.json to sounds.json and edit it. It lists every\n"
                "  event already. The format is Minecraft's:\n\n"
                "      \"block.stone.break\": {\n"
                "        \"sounds\": [\n"
                "          \"block/stone/break1\",\n"
                "          { \"name\": \"block/stone/break2\", \"volume\": 0.8,\n"
                "            \"pitch\": 1.1, \"weight\": 3 }\n"
                "        ]\n"
                "      }\n\n"
                "  A higher-priority pack's entries REPLACE a lower one's for the same\n"
                "  event. \"replace\": false on an event adds to them instead, which is\n"
                "  Minecraft's default rather than this one -- see README's LIMITS.\n\n"
                "  Once sounds.json names an event, it is in charge of that event and the\n"
                "  drop-a-file-in shortcut no longer applies to it. An entry naming a file\n"
                "  that is not there is reported in the Resource Packs screen.\n\n"
                "MINECRAFT PACKS\n"
                "  Drop one in as-is and enable it. The event names above ARE Minecraft's,\n"
                "  and the game also knows Minecraft's own default file paths -- so a pack\n"
                "  that just replaces dig/stone1.ogg, step/grass1.ogg or mob/cow/say1.ogg\n"
                "  with no sounds.json of its own works, which is how most of them are made.\n"
                "  Sounds this game has and Minecraft does not fall back sensibly (ores use\n"
                "  the stone set, leaves use grass, trapdoors use doors).\n\n"
                "LIMITS AND DIFFERENCES FROM MINECRAFT\n"
                "  Clips are mixed down to mono -- the 3D panner needs one channel -- and\n"
                "  must be under 30 seconds. .ogg and .wav only; no .mp3, no .flac.\n"
                "  Packs are folders, not .zip files: unzip it into this directory.\n"
                "  \"replace\" defaults to true here and to false in Minecraft, so that\n"
                "  the pack you put on top actually wins instead of its sounds being\n"
                "  shuffled in with the pack below. With one pack installed there is no\n"
                "  difference. Set \"replace\": false to get Minecraft's behaviour back.\n"
                "  \"type\": \"event\" redirects are not supported.\n"
                "  Textures in a pack are detected and counted but not applied yet.\n";
    }
  }

  log::info("packs: wrote an example pack to %s", root.string().c_str());
  if (errorOut) errorOut->clear();
  return true;
}

}  // namespace hr::resource
