#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace hr::log {
namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;
Level g_minLevel =
#if defined(HR_DEBUG)
    Level::Debug;
#else
    Level::Info;
#endif

const char* tagOf(Level level) {
  switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info ";
    case Level::Warn: return "warn ";
    case Level::Error: return "ERROR";
  }
  return "?????";
}

}  // namespace

void openFile(const char* path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) std::fclose(g_file);
  g_file = std::fopen(path, "w");
}

void close() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) {
    std::fclose(g_file);
    g_file = nullptr;
  }
}

void setMinLevel(Level level) { g_minLevel = level; }

void write(Level level, const char* fmt, ...) {
  if (static_cast<int>(level) < static_cast<int>(g_minLevel)) return;

  char body[2048];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);

  std::time_t now = std::time(nullptr);
  std::tm tm {};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char stamp[16];
  std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);

  std::lock_guard<std::mutex> lock(g_mutex);
  std::fprintf(stderr, "[%s %s] %s\n", stamp, tagOf(level), body);
  if (g_file) {
    std::fprintf(g_file, "[%s %s] %s\n", stamp, tagOf(level), body);
    // Flushed per line on purpose: if we crash, the last line before the crash
    // is the one worth having.
    std::fflush(g_file);
  }
}

}  // namespace hr::log
