#include "ui/uipacks.h"

#include <cstdio>

#include "core/log.h"
#include "ui/theme.h"

namespace hr::ui {
namespace {

// Reads a file, refusing anything past the cap before it is in memory rather than
// after. Returns false for a missing file, which is the ordinary case — most packs
// have no theme — so it is not logged here.
bool readCapped(const std::string& path, std::string& out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  if (size < 0 || static_cast<std::size_t>(size) > kMaxThemeBytes) {
    std::fclose(f);
    return false;
  }
  std::fseek(f, 0, SEEK_SET);
  out.resize(static_cast<std::size_t>(size));
  const std::size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  out.resize(got);
  return true;
}

}  // namespace

UiPackReport applyUiPacks(const std::vector<resource::PackInfo>& ordered) {
  UiPackReport report;
  std::vector<ThemeDoc> docs;

  // Reversed: `ordered` is highest priority first, and Theme::build applies its
  // documents lowest first. Getting this backwards would make the pack at the
  // bottom of the list the one that wins, which is the opposite of what the
  // screen showing that list says.
  for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
    const resource::PackInfo& pack = *it;
    if (!pack.usable() || !pack.hasUiTheme) continue;

    // The first namespace that has one, rather than merging both. A pack with two
    // themes has made a mistake, and picking one predictably is more useful than
    // combining them into a third thing its author never saw.
    std::string body;
    std::string from;
    for (const std::string& ns : pack.namespaces) {
      const std::string path = resource::packFile(pack, "assets/" + ns + "/ui/theme.json");
      if (path.empty()) continue;  // unsafe relative path; packFile already refused it
      if (readCapped(path, body)) {
        from = path;
        break;
      }
    }
    if (from.empty()) {
      report.problems.push_back(pack.name + ": ui/theme.json could not be read");
      continue;
    }

    ThemeDoc doc;
    std::string error;
    if (!parseThemeDoc(body, pack.id, doc, &error)) {
      report.problems.push_back(pack.name + ": ui/theme.json is malformed (" + error + ")");
      continue;
    }
    if (!doc.unknown.empty()) {
      // Named individually up to a point. "3 unknown names" tells an author there
      // is a typo; naming them tells them where.
      std::string list;
      for (std::size_t i = 0; i < doc.unknown.size() && i < 4; ++i) {
        if (!list.empty()) list += ", ";
        list += doc.unknown[i];
      }
      if (doc.unknown.size() > 4) {
        list += " and " + std::to_string(doc.unknown.size() - 4) + " more";
      }
      report.problems.push_back(pack.name + ": unknown theme names (" + list + ")");
    }
    if (doc.empty()) continue;

    report.packs++;
    report.colors += static_cast<int>(doc.palette.size() + doc.roles.size());
    report.scalars += static_cast<int>(doc.scalars.size());
    log::info("ui theme: %s (%zu palette, %zu roles, %zu scalars)", pack.id.c_str(),
              doc.palette.size(), doc.roles.size(), doc.scalars.size());
    docs.push_back(std::move(doc));
  }

  theme().build(docs);
  return report;
}

}  // namespace hr::ui
