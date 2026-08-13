#include "save/migrate.h"

#include "core/log.h"

namespace hr::save {
namespace {

using Migration = void (*)(WorldSave&);

// One entry per version n, upgrading n -> n+1. Index i holds the migration from
// version i+1 to i+2. Example, for when one is needed:
//
//   void v1ToV2(WorldSave& s) { s.player.stamina = 20.0f; }
//   constexpr Migration kMigrations[] = { v1ToV2 };
//
// STILL EMPTY AT v2, and that is a real answer rather than an oversight. The v1 ->
// v2 change was ItemStack gaining a colour, and format.cpp's readStack takes the
// file's version and hands back an undyed stack for a v1 file. There is nothing
// left for a pass over the decoded save to correct.
//
// The rule this settles for next time: a field ADDED with a sensible default
// belongs in the version-aware reader. A migration is for what the reader cannot
// know — a value that has to be computed from other fields, or one whose meaning
// changed rather than its presence.
constexpr Migration kMigrations[] = {nullptr};
constexpr int kMigrationCount = 0;  // deliberately 0, not sizeof(kMigrations)

}  // namespace

void migrate(WorldSave& save, std::uint16_t fromVersion) {
  std::uint16_t v = fromVersion < 1 ? 1 : fromVersion;
  while (v < kSaveVersion) {
    const int index = static_cast<int>(v) - 1;
    if (index >= 0 && index < kMigrationCount && kMigrations[index]) {
      kMigrations[index](save);
    }
    ++v;
  }
  if (fromVersion != kSaveVersion) {
    log::info("save: migrated v%u -> v%u", static_cast<unsigned>(fromVersion),
              static_cast<unsigned>(kSaveVersion));
  }
}

}  // namespace hr::save
