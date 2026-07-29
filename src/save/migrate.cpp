#include "save/migrate.h"

#include "core/log.h"

namespace hr::save {
namespace {

using Migration = void (*)(WorldSave&);

// One entry per version n, upgrading n -> n+1. Empty at v1, which is the whole
// table's state until a section layout changes. Example, for when it does:
//
//   void v1ToV2(WorldSave& s) { s.player.stamina = 20.0f; }
//   constexpr Migration kMigrations[] = { v1ToV2 };
//
// Index i holds the migration from version i+1 to i+2.
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
