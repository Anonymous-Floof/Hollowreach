#include "platform/paths.h"

#include <filesystem>
#include <fstream>

#include "core/log.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace hr::paths {
namespace {

std::string g_exeDir, g_dataDir, g_worlds, g_exports, g_screenshots, g_packs, g_settings, g_log;

std::string executableDir() {
  std::error_code ec;
#if defined(_WIN32)
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  if (n == 0) return fs::current_path(ec).string();
  return fs::path(std::wstring(buf, n)).parent_path().string();
#elif defined(__APPLE__)
  // /proc is absent on macOS; the current directory is a reasonable fallback
  // for a development build, and the app bundle path is handled by the caller.
  return fs::current_path(ec).string();
#else
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return fs::current_path(ec).string();
  buf[n] = '\0';
  return fs::path(buf).parent_path().string();
#endif
}

std::string userDataDir() {
  std::error_code ec;
#if defined(_WIN32)
  if (const char* appdata = std::getenv("APPDATA")) {
    return (fs::path(appdata) / "Hollowreach").string();
  }
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) {
    return (fs::path(home) / "Library" / "Application Support" / "Hollowreach").string();
  }
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
    return (fs::path(xdg) / "hollowreach").string();
  }
  if (const char* home = std::getenv("HOME")) {
    return (fs::path(home) / ".local" / "share" / "hollowreach").string();
  }
#endif
  return (fs::current_path(ec) / "data").string();
}

// A directory we can actually create files in. Probing beats checking
// permissions: a Windows virtual store can report a path as writable and then
// silently redirect the write somewhere else.
bool isWritable(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (!fs::is_directory(dir, ec)) return false;
  fs::path probe = dir / ".hr_write_probe";
  {
    std::ofstream out(probe);
    if (!out) return false;
    out << "ok";
    if (!out.good()) return false;
  }
  fs::remove(probe, ec);
  return true;
}

}  // namespace

void init(const std::string& dataDirOverride) {
  g_exeDir = executableDir();

  if (!dataDirOverride.empty()) {
    g_dataDir = fs::path(dataDirOverride).lexically_normal().string();
  } else {
    fs::path portable = fs::path(g_exeDir) / "data";
    if (isWritable(portable)) {
      g_dataDir = portable.string();
    } else {
      g_dataDir = userDataDir();
      log::info("paths: %s is not writable, using %s", portable.string().c_str(),
                g_dataDir.c_str());
    }
  }

  g_worlds = (fs::path(g_dataDir) / "worlds").string();
  g_exports = (fs::path(g_dataDir) / "exports").string();
  g_screenshots = (fs::path(g_dataDir) / "screenshots").string();
  g_packs = (fs::path(g_dataDir) / "resourcepacks").string();
  g_settings = (fs::path(g_dataDir) / "settings.json").string();
  g_log = (fs::path(g_dataDir) / "hollowreach.log").string();
}

const std::string& exeDir() { return g_exeDir; }
const std::string& dataDir() { return g_dataDir; }
const std::string& worldsDir() { return g_worlds; }
const std::string& exportsDir() { return g_exports; }
const std::string& screenshotsDir() { return g_screenshots; }
const std::string& resourcePacksDir() { return g_packs; }
const std::string& settingsFile() { return g_settings; }
const std::string& logFile() { return g_log; }

bool ensureDirs() {
  std::error_code ec;
  bool ok = true;
  for (const std::string* dir : {&g_dataDir, &g_worlds, &g_exports, &g_screenshots, &g_packs}) {
    fs::create_directories(*dir, ec);
    if (ec) {
      log::error("paths: cannot create %s (%s)", dir->c_str(), ec.message().c_str());
      ok = false;
      ec.clear();
    }
  }
  return ok;
}

std::string join(const std::string& a, const std::string& b) {
  return (fs::path(a) / b).generic_string();
}

}  // namespace hr::paths
