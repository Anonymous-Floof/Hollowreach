// Resource packs on disk: what is installed, what each one supplies, and which
// of them are on and in what order.
//
// The layout is Minecraft's, deliberately and exactly:
//
//     data/resourcepacks/
//       MyPack/
//         pack.mcmeta                      { "pack": { "pack_format": 1,
//                                                      "description": "..." } }
//         pack.png                         (optional, not read yet)
//         assets/
//           minecraft/
//             sounds.json                  event -> files, volume, pitch, weight
//             sounds/block/stone/break1.ogg
//             textures/block/stone.png
//
// A pack is a **folder**, not a zip. Reading a zip means a DEFLATE decompressor,
// and there is no point paying for one before the folder path is proven — a pack
// somebody is authoring is a folder anyway, and unzipping a downloaded one is a
// double-click. That is the one deliberate gap in the format compatibility.
//
// Identity is the folder name, never the display name. The display name comes out
// of pack.mcmeta and an author may change it between versions; the enabled list
// has to survive that, and two packs are perfectly entitled to describe themselves
// identically.
//
// **Nothing here trusts the pack.** A pack is a folder somebody downloaded, so
// every path that reaches the filesystem goes through safeRelativePath() first —
// a `sounds.json` naming "../../../../windows/system32/config/sam" is a file this
// process can open, and there is no other layer between that string and the disk.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hr::resource {

// What this build writes into a pack it generates, and the highest it claims to
// understand. Minecraft's own numbers are on a different scale entirely and are
// read for display only — refusing a pack because its pack_format is 34 would
// reject every real Minecraft pack, and the sound layout has not changed across
// any of them.
inline constexpr int kPackFormat = 1;

// Namespaces searched, in order, for any resource. `minecraft` is second so a
// pack written for Minecraft resolves without being repackaged, while a pack
// written for this game can override one of its files by using the first.
const std::vector<std::string>& namespaceSearchOrder();

struct PackInfo {
  // The folder name. The pack's identity everywhere: the enabled list, the load
  // order, the log lines.
  std::string id;
  // From pack.mcmeta's description, falling back to the folder name.
  std::string name;
  std::string description;
  // Absolute path of the pack folder.
  std::string root;
  int packFormat = 0;

  // Which assets/<ns>/ folders exist, in search order.
  std::vector<std::string> namespaces;
  int soundFiles = 0;
  int textureFiles = 0;
  bool hasSoundsJson = false;
  // assets/<ns>/ui/theme.json — the interface's colours and measurements. See
  // ui/theme.h for what it may contain and ui/uipacks.h for how it is applied.
  bool hasUiTheme = false;

  // Non-empty when the pack cannot be used, and phrased for a player rather than
  // for a log: it is shown on the row in the Resource Packs screen, because a
  // pack that silently does nothing is the worst outcome of all.
  std::string problem;

  bool usable() const { return problem.empty(); }
  // A pack with no sounds is not broken — it may be a texture or UI pack — but it
  // has nothing the audio layer can apply.
  bool suppliesSound() const { return soundFiles > 0; }
  bool suppliesUi() const { return hasUiTheme; }
};

// Every folder under data/resourcepacks/, sorted by id. Unreadable and malformed
// packs are returned too, carrying their `problem`, so the screen can say why one
// is not working instead of leaving a player wondering where it went.
std::vector<PackInfo> scanPacks();

// Refuses anything that could leave the pack folder: absolute paths, drive
// letters, backslashes, and any `..` component. Public because the sound bank
// applies it to every name it reads out of a sounds.json.
bool safeRelativePath(std::string_view path);

// Joins a checked relative path onto a pack root. Returns empty when the path is
// unsafe, so a caller that forgets to check still cannot escape.
std::string packFile(const PackInfo& pack, std::string_view relative);

// ---------------------------------------------------------------------------
// Selection
//
// The enabled packs, **highest priority first** — the first id in the list is the
// one whose files win. That is PackStack's own documented convention ("add the
// highest priority first"), it is the order the Resource Packs screen shows top
// to bottom, and it is the order Minecraft's own screen uses, so there is only
// one answer to remember rather than three.
//
// Stored in settings.json as one hidden Text row, because it is a player
// preference of the same kind as the menu background and there is no reason to
// invent a second file for it.
// ---------------------------------------------------------------------------

inline constexpr const char* kEnabledSetting = "resourcePacks";

std::vector<std::string> enabledPackIds();
void setEnabledPackIds(const std::vector<std::string>& ids);

// The scan filtered to the enabled ids, in selection order, skipping any that are
// no longer installed. An id left over from a deleted pack is dropped silently:
// the alternative is an error about a folder the player deleted on purpose.
std::vector<PackInfo> enabledPacks(const std::vector<PackInfo>& installed);

// Writes a ready-to-fill pack: pack.mcmeta, a sounds.json naming every event in
// `events`, and the folder tree under it. Making a pack is then a matter of
// dropping files in, which is the difference between the format being documented
// and it being usable.
//
// The event list is passed in rather than read from the audio layer, so this file
// stays a filesystem module: what a sound event *is* belongs to audio/soundbank.h,
// and resource/ has no business knowing.
bool writeExamplePack(const std::string& directory, const std::vector<std::string>& events,
                      std::string* errorOut);

}  // namespace hr::resource
