#include "dev/packtool.h"

#include <cstdio>
#include <filesystem>
#include <vector>

#include "audio/soundbank.h"
#include "platform/paths.h"
#include "resource/pack.h"
#include "ui/settings.h"

namespace fs = std::filesystem;

namespace hr::dev {
namespace {

// Just the file name, relative to the pack root, so a column of these lines up
// instead of repeating an absolute path forty times.
std::string shorten(const std::string& path) {
  const std::string& root = paths::resourcePacksDir();
  if (path.size() > root.size() + 1 && path.compare(0, root.size(), root) == 0) {
    return path.substr(root.size() + 1);
  }
  return path;
}

}  // namespace

int listPacks(bool all) {
  ui::settings().load(paths::settingsFile());

  const std::vector<resource::PackInfo> installed = resource::scanPacks();
  std::printf("Resource packs in %s\n", paths::resourcePacksDir().c_str());
  if (installed.empty()) {
    std::printf("  (none installed)\n");
  }
  const std::vector<std::string> enabled = resource::enabledPackIds();
  for (const resource::PackInfo& pack : installed) {
    int position = 0;
    for (std::size_t i = 0; i < enabled.size(); ++i) {
      if (enabled[i] == pack.id) position = static_cast<int>(i) + 1;
    }
    std::printf("  [%s] %-24s %s\n", position > 0 ? "on " : "off", pack.id.c_str(),
                pack.name.c_str());
    if (!pack.usable()) {
      std::printf("        PROBLEM: %s\n", pack.problem.c_str());
      continue;
    }
    std::printf("        %d sounds, %d textures, pack_format %d%s", pack.soundFiles,
                pack.textureFiles, pack.packFormat, pack.hasSoundsJson ? ", sounds.json" : "");
    if (position > 0) std::printf(", priority %d", position);
    std::printf("\n");
  }

  const std::vector<resource::PackInfo> active = resource::enabledPacks(installed);
  audio::sounds().rebuild(active);

  for (const std::string& line : audio::sounds().warnings()) {
    std::printf("  ! %s\n", line.c_str());
  }

  std::printf("\nSound events (%d of %d replaced)\n", audio::sounds().stats().events,
              static_cast<int>(audio::soundEventCatalogue().size()));
  int shown = 0;
  for (const std::string& event : audio::soundEventCatalogue()) {
    // roll 0 picks the first variant deterministically, which is what a report
    // wants: the same command twice must print the same thing.
    const audio::SoundPick picked = audio::sounds().pick(event, 0.0f);
    if (!picked.valid()) {
      if (all) std::printf("  %-34s synthesised\n", event.c_str());
      continue;
    }
    ++shown;
    const std::string_view fallback = audio::soundEventFallback(event);
    const bool viaFallback = !audio::sounds().has(event) && !fallback.empty();
    std::printf("  %-34s %s  (%.2fs, %d Hz, x%.2f)%s\n", event.c_str(),
                shorten(picked.clip->source).c_str(), picked.clip->seconds(),
                picked.clip->sampleRate, picked.volume,
                viaFallback ? "  [via fallback]" : "");
  }
  if (shown == 0) std::printf("  (nothing replaced — every sound is synthesised)\n");
  return 0;
}

int makeExamplePack(const std::string& directory) {
  const std::string target =
      directory.empty() ? paths::join(paths::resourcePacksDir(), "ExamplePack") : directory;
  std::string error;
  if (!resource::writeExamplePack(target, audio::soundEventCatalogue(), &error)) {
    std::fprintf(stderr, "Could not write the example pack: %s\n", error.c_str());
    return 1;
  }
  std::printf("Wrote an example pack to %s\n", target.c_str());
  std::printf("  %d events, one empty folder each. See EVENTS.txt and README.txt.\n",
              static_cast<int>(audio::soundEventCatalogue().size()));
  std::printf("  Drop .ogg or .wav files in, then enable it in Resource Packs.\n");
  return 0;
}

}  // namespace hr::dev
