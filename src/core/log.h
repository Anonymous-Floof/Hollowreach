// Minimal printf-style logging. The web build used console.log; this is the
// equivalent, plus a file sink so a player can send us a log after a crash.
//
// Levels are compile-time cheap: trace/debug compile to nothing in Release.

#pragma once

namespace hr::log {

enum class Level { Trace, Debug, Info, Warn, Error };

// Opens `path` as a second sink alongside stderr. Safe to skip entirely.
void openFile(const char* path);
void close();

void setMinLevel(Level level);

void write(Level level, const char* fmt, ...);

#if defined(HR_DEBUG)
#define HR_LOG_TRACE_ENABLED 1
#else
#define HR_LOG_TRACE_ENABLED 0
#endif

template <typename... Args>
inline void trace(const char* fmt, Args... args) {
#if HR_LOG_TRACE_ENABLED
  write(Level::Trace, fmt, args...);
#else
  (void)fmt;
  ((void)args, ...);
#endif
}

// Runtime-gated, not compile-time: --verbose has to work in an optimised build,
// which is the configuration most of the porting work runs in. Only `trace` is
// compiled out, since its call sites are inside per-frame loops.
template <typename... Args>
inline void debug(const char* fmt, Args... args) {
  write(Level::Debug, fmt, args...);
}

template <typename... Args>
inline void info(const char* fmt, Args... args) {
  write(Level::Info, fmt, args...);
}

template <typename... Args>
inline void warn(const char* fmt, Args... args) {
  write(Level::Warn, fmt, args...);
}

template <typename... Args>
inline void error(const char* fmt, Args... args) {
  write(Level::Error, fmt, args...);
}

}  // namespace hr::log
