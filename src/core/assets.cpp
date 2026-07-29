#include "core/assets.h"

#include <filesystem>
#include <fstream>

#include "core/log.h"

namespace fs = std::filesystem;

namespace hr::assets {
namespace {

std::string g_overrideDir;

const Entry* findEmbedded(std::string_view name) {
  for (unsigned i = 0; i < kEmbeddedCount; ++i) {
    if (name == kEmbedded[i].name) return &kEmbedded[i];
  }
  return nullptr;
}

std::optional<std::vector<unsigned char>> readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::vector<unsigned char>(std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>());
}

}  // namespace

void setOverrideDir(std::string dir) {
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) return;
  g_overrideDir = std::move(dir);
  log::info("assets: reading from %s (overriding embedded copies)", g_overrideDir.c_str());
}

bool hasOverrideDir() { return !g_overrideDir.empty(); }

std::string overridePath(std::string_view name) {
  if (g_overrideDir.empty()) return {};
  return (fs::path(g_overrideDir) / name).string();
}

std::optional<std::string> readText(std::string_view name) {
  if (!g_overrideDir.empty()) {
    if (auto bytes = readFile(fs::path(g_overrideDir) / name)) {
      return std::string(bytes->begin(), bytes->end());
    }
  }
  if (const Entry* e = findEmbedded(name)) {
    return std::string(reinterpret_cast<const char*>(e->data), e->size);
  }
  return std::nullopt;
}

std::vector<unsigned char> readBytes(std::string_view name) {
  if (!g_overrideDir.empty()) {
    if (auto bytes = readFile(fs::path(g_overrideDir) / name)) return std::move(*bytes);
  }
  if (const Entry* e = findEmbedded(name)) {
    return std::vector<unsigned char>(e->data, e->data + e->size);
  }
  return {};
}

long long overrideMTime(std::string_view name) {
  if (g_overrideDir.empty()) return 0;
  std::error_code ec;
  auto t = fs::last_write_time(fs::path(g_overrideDir) / name, ec);
  if (ec) return 0;
  return t.time_since_epoch().count();
}

std::vector<std::string> list(std::string_view prefix) {
  std::vector<std::string> out;
  for (unsigned i = 0; i < kEmbeddedCount; ++i) {
    std::string_view name(kEmbedded[i].name);
    if (name.rfind(prefix, 0) == 0) out.emplace_back(name);
  }
  // With embedding off the table is empty, so fall back to walking the override
  // directory — otherwise a non-embedded build could not enumerate anything.
  if (out.empty() && !g_overrideDir.empty()) {
    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(g_overrideDir, ec)) {
      if (!entry.is_regular_file()) continue;
      std::string rel = fs::relative(entry.path(), g_overrideDir, ec).generic_string();
      if (rel.rfind(prefix, 0) == 0) out.push_back(rel);
    }
  }
  return out;
}

}  // namespace hr::assets
