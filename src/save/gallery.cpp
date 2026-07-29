#include "save/gallery.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>

#include "core/log.h"
#include "platform/paths.h"

#if defined(_WIN32)
#include <windows.h>
// WIN32_LEAN_AND_MEAN leaves the shell API out, and ShellExecute is the whole
// reason this file is platform-specific.
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;

namespace hr::save::gallery {

std::vector<Shot> list() {
  std::vector<Shot> out;
  std::error_code ec;
  const std::string dir = paths::screenshotsDir();
  if (!fs::is_directory(dir, ec)) return out;

  const auto now = std::chrono::system_clock::now();
  for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::string ext = entry.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".png") continue;

    Shot shot;
    shot.path = entry.path().string();
    shot.name = entry.path().stem().string();
    const auto written = fs::last_write_time(entry.path(), ec);
    if (!ec) {
      const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(written);
      shot.ageSeconds = std::chrono::duration<double>(now - systemTime).count();
    }
    ec.clear();
    out.push_back(std::move(shot));
  }

  // Newest first, which is the order js/save/gallery.js:66 listed them in.
  std::sort(out.begin(), out.end(),
            [](const Shot& a, const Shot& b) { return a.ageSeconds < b.ageSeconds; });
  return out;
}

bool erase(const std::string& path) {
  std::error_code ec;
  const bool ok = fs::remove(path, ec);
  if (!ok) log::warn("gallery: could not delete %s", path.c_str());
  return ok;
}

bool reveal(const std::string& path) {
#if defined(_WIN32)
  std::error_code ec;
  if (!fs::exists(path, ec)) return false;
  // /select, opens the folder with the file already highlighted. The path is
  // quoted because a screenshot lives under the player's data directory, which can
  // sit anywhere — including a path with spaces or a comma in it.
  const std::wstring wide = fs::path(path).make_preferred().wstring();
  const std::wstring args = L"/select,\"" + wide + L"\"";
  const auto result = reinterpret_cast<std::intptr_t>(
      ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
  // ShellExecute reports failure as a value of 32 or below.
  return result > 32;
#else
  // Left for whichever desktop environment a Linux or macOS build finds itself in;
  // the gallery logs the path either way, which is enough to find the file.
  log::info("screenshot: %s", path.c_str());
  return false;
#endif
}

}  // namespace hr::save::gallery
