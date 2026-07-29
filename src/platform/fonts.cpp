#include "platform/fonts.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include "core/log.h"

namespace fs = std::filesystem;

namespace hr::platform {
namespace {

// One installed face: the file, its CSS weight, and whether it is the italic cut.
struct Face {
  const char* file;
  int weight;
  bool italic;
};

// Segoe UI as Windows installs it. All six weights share the typographic family
// name "Segoe UI", which is why the browser treats Light/Semilight/Semibold/Black
// as weights of one family rather than separate families.
constexpr Face kSegoe[] = {
    {"segoeuil.ttf", 300, false},  {"seguili.ttf", 300, true},
    {"segoeuisl.ttf", 350, false}, {"seguisli.ttf", 350, true},
    {"segoeui.ttf", 400, false},   {"segoeuii.ttf", 400, true},
    {"seguisb.ttf", 600, false},   {"seguisbi.ttf", 600, true},
    {"segoeuib.ttf", 700, false},  {"segoeuiz.ttf", 700, true},
    {"seguibl.ttf", 900, false},   {"seguibli.ttf", 900, true},
};

// The stylesheet's second choice, for a Windows install missing Segoe UI.
constexpr Face kTrebuchet[] = {
    {"trebuc.ttf", 400, false},
    {"trebucit.ttf", 400, true},
    {"trebucbd.ttf", 700, false},
    {"trebucbi.ttf", 700, true},
};

constexpr Face kConsolas[] = {
    {"consola.ttf", 400, false},
    {"consolai.ttf", 400, true},
    {"consolab.ttf", 700, false},
    {"consolaz.ttf", 700, true},
};

// Non-Windows families, so a Linux or macOS build still has an interface. Not a
// parity target — the browser reference only exists on the platform that has Segoe.
constexpr const char* kSansFallbackFiles[] = {
    "DejaVuSans.ttf", "LiberationSans-Regular.ttf", "NotoSans-Regular.ttf",
    "Helvetica.ttc",  "SFNSDisplay.ttf",            "Arial.ttf",
    "arial.ttf",
};
constexpr const char* kSansBoldFallbackFiles[] = {
    "DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf", "NotoSans-Bold.ttf",
    "Helvetica.ttc",       "Arial Bold.ttf",          "arialbd.ttf",
};
constexpr const char* kMonoFallbackFiles[] = {
    "DejaVuSansMono.ttf", "LiberationMono-Regular.ttf", "NotoSansMono-Regular.ttf",
    "Menlo.ttc",          "cour.ttf",
};

std::vector<std::string> buildDirs() {
  std::vector<std::string> dirs;
#if defined(_WIN32)
  if (const char* windir = std::getenv("WINDIR")) {
    dirs.push_back(std::string(windir) + "\\Fonts");
  }
  dirs.push_back("C:\\Windows\\Fonts");
  // Per-user installs (Windows 10 1809+) land here and are just as valid.
  if (const char* local = std::getenv("LOCALAPPDATA")) {
    dirs.push_back(std::string(local) + "\\Microsoft\\Windows\\Fonts");
  }
#elif defined(__APPLE__)
  dirs.push_back("/System/Library/Fonts");
  dirs.push_back("/System/Library/Fonts/Supplemental");
  dirs.push_back("/Library/Fonts");
  if (const char* home = std::getenv("HOME")) dirs.push_back(std::string(home) + "/Library/Fonts");
#else
  dirs.push_back("/usr/share/fonts/truetype/dejavu");
  dirs.push_back("/usr/share/fonts/truetype/liberation");
  dirs.push_back("/usr/share/fonts/truetype");
  dirs.push_back("/usr/share/fonts/TTF");
  dirs.push_back("/usr/share/fonts");
  dirs.push_back("/usr/local/share/fonts");
  if (const char* home = std::getenv("HOME")) dirs.push_back(std::string(home) + "/.fonts");
#endif
  return dirs;
}

const std::vector<std::string>& dirs() {
  static const std::vector<std::string> value = buildDirs();
  return value;
}

// Searches every font directory for `file`, recursing one level because Linux
// distributions nest by foundry.
std::string locate(const std::string& file) {
  std::error_code ec;
  for (const std::string& dir : dirs()) {
    fs::path direct = fs::path(dir) / file;
    if (fs::is_regular_file(direct, ec)) return direct.string();
  }
  for (const std::string& dir : dirs()) {
    if (!fs::is_directory(dir, ec)) continue;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
      if (!e.is_directory(ec)) continue;
      fs::path nested = e.path() / file;
      if (fs::is_regular_file(nested, ec)) return nested.string();
    }
  }
  return {};
}

// CSS Fonts 4 weight matching, for the case that actually arises here (target
// >= 500): weights at or above the target are tried in ascending order, then
// weights below it in descending order.
std::string matchWeight(const Face* faces, std::size_t count, int weight, bool italic) {
  struct Candidate {
    int weight;
    std::string path;
  };
  std::vector<Candidate> available;
  for (std::size_t i = 0; i < count; ++i) {
    if (faces[i].italic != italic) continue;
    std::string path = locate(faces[i].file);
    if (!path.empty()) available.push_back({faces[i].weight, std::move(path)});
  }
  if (available.empty()) return {};

  std::sort(available.begin(), available.end(),
            [](const Candidate& a, const Candidate& b) { return a.weight < b.weight; });

  if (weight >= 500) {
    for (const Candidate& c : available) {
      if (c.weight >= weight) return c.path;
    }
    return available.back().path;
  }
  // Below 500: weights at or under the target descending, then above ascending.
  for (auto it = available.rbegin(); it != available.rend(); ++it) {
    if (it->weight <= weight) return it->path;
  }
  return available.front().path;
}

std::string firstOf(const char* const* files, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    std::string path = locate(files[i]);
    if (!path.empty()) return path;
  }
  return {};
}

}  // namespace

std::string findFont(const FontRequest& req) {
  if (req.family == FontFamily::Mono) {
    std::string path = matchWeight(kConsolas, std::size(kConsolas), req.weight, req.italic);
    if (!path.empty()) return path;
    return firstOf(kMonoFallbackFiles, std::size(kMonoFallbackFiles));
  }

  std::string path = matchWeight(kSegoe, std::size(kSegoe), req.weight, req.italic);
  if (!path.empty()) return path;
  path = matchWeight(kTrebuchet, std::size(kTrebuchet), req.weight, req.italic);
  if (!path.empty()) return path;
  if (req.weight >= 600) {
    path = firstOf(kSansBoldFallbackFiles, std::size(kSansBoldFallbackFiles));
    if (!path.empty()) return path;
  }
  return firstOf(kSansFallbackFiles, std::size(kSansFallbackFiles));
}

std::vector<std::string> fallbackFonts() {
  std::vector<std::string> out;
  // Segoe UI Symbol and Segoe UI Emoji between them cover every dingbat the
  // interface uses: ♥ ♡ ◆ ◇ ● ☠ ◎ ✕ ➤ ‹ ›.
  for (const char* file : {"seguisym.ttf", "seguiemj.ttf", "segmdl2.ttf",
                           "DejaVuSans.ttf", "NotoSansSymbols2-Regular.ttf",
                           "AppleSymbols.ttf", "Symbola.ttf"}) {
    std::string path = locate(file);
    if (!path.empty()) out.push_back(std::move(path));
  }
  return out;
}

std::vector<std::string> fontDirectories() { return dirs(); }

}  // namespace hr::platform
