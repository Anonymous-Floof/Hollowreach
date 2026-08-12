// Break / place interaction, ported from js/game/interact.js.
//
// Raycasts every frame to find the targeted block, accumulates break progress
// modulated by the held tool and the block's hardness (behind the mining-tier
// gate), and places the selected block item with the right orientation metadata.
//
// Everything the surrounding game has to decide — opening a station's interface,
// sleeping, eating, warping, showing a refusal message — is a hook rather than a
// direct call, which is how the web build wired it too and what keeps this file
// free of the interface and the audio engine.

#pragma once

#include <functional>
#include <string>

#include "core/input.h"
#include "game/entities/manager.h"
#include "game/inventory.h"
#include "game/items.h"
#include "game/player.h"
#include "game/raycast.h"
#include "world/world.h"

namespace hr::game {

// What using an item on a block did. Three outcomes rather than a bool, because a
// hoe tilling soil is not consumed and a seed being sown is, and the caller cannot
// tell which from the item alone.
enum class UseResult : std::uint8_t {
  Ignored,          // nothing happened; the click falls through to eating, placing
  Used,             // it worked and the stack stays
  UsedAndConsume,   // it worked and one is taken off the stack
};

struct InteractHooks {
  // A transient line of text, for refusals like "needs a stone+ tier pickaxe".
  std::function<void(const std::string&)> notify;
  // Right-clicked a workbench, forge or chest.
  std::function<void(world::Station, int, int, int)> onOpenStation;
  // Right-clicked a bed.
  std::function<void()> onSleep;
  // Right-clicked a painting: open the picture chooser for that block.
  std::function<void(int, int, int)> onOpenPainting;
  // Right-clicked a soul anchor.
  std::function<void(int, int, int)> onSetSpawn;
  // Mined a soul anchor that had been bound.
  std::function<void(int, int, int)> onAnchorBroken;
  // Ate food; returns false when the player is already full, which leaves the
  // stack alone.
  std::function<bool(const ItemDef&)> onEat;
  // Used a wayshard; returns false when there is no open sky to warp to.
  std::function<bool()> onWarp;
  // Used the held item ON a block: a hoe tilling soil, a crop being sown.
  //
  // Right-clicking a Tool did nothing whatsoever before this existed — Food, Bucket
  // and Warp were the only item types with a right-click verb at all. It is written
  // generically (the item, the block, and where) rather than as onTill and onSow,
  // because this is the seam every future "use this on that" wants, and two hooks
  // that differ only in which item they accept would be the wrong shape to add a
  // third to.
  std::function<UseResult(const ItemDef&, int x, int y, int z)> onUseOn;
  // Placed a boat. Returns false when it could not be created, which leaves the
  // item in hand. A hook rather than a direct spawn because a guest's boat is the
  // host's to create — the same division relayEntity draws for hitting a mob.
  std::function<bool(const Vec3&)> spawnBoat;

  // Multiplayer guest only. The host owns every entity, so hitting a cow or
  // boarding a boat is a request, not a decision — a guest that applied it
  // locally would see a mob die that the host never killed. Returns true when the
  // action was sent, which is exactly when the local hook must not also run.
  // Absent in single player and on the host, where the local hook is the truth.
  std::function<bool(const Entity&, InteractButton)> relayEntity;

  // The live entity manager, when there is one. Entities are raycast alongside
  // blocks: a mob or a boat closer than the targeted block takes the click. Null
  // in the headless tools, which have no entities to hit.
  EntityManager* entities = nullptr;
  // What an entity hook needs to reach back into. Assembled by App.
  EntityContext* entityContext = nullptr;
};

class Interact {
 public:
  void update(float dt, const Input& input, Player& player, world::World& world,
              Inventory& inventory, const InteractHooks& hooks);

  // The block the crosshair is on, for the selection outline.
  bool hasSelection() const { return hasSelection_; }
  int selectionX() const { return selX_; }
  int selectionY() const { return selY_; }
  int selectionZ() const { return selZ_; }

  // 0..1 through the current break, for the crack overlay.
  float breakFraction() const { return breakFrac_; }

  // True on any frame the player actually used what they are holding — mining,
  // hitting, placing, eating. The viewmodel reads it to swing the hand; holding a
  // mine keeps it true, which chains into a repeating swing.
  bool swung() const { return swung_; }

  // Creative's instant break. Blocks come out on the first frame and drop
  // nothing, and the tier refusal goes quiet with them — being told you need a
  // stone pickaxe for a block you just removed anyway is noise.
  void setInstantBreak(bool on) { instantBreak_ = on; }

  void reset();

 private:
  void toggleDoor(world::World& world, const RayHit& hit, const world::BlockDef& block,
                  Player& player);
  bool useBucket(world::World& world, Inventory& inventory, Player& player, const RayHit& hit,
                 const InteractHooks& hooks);
  void complete(world::World& world, Inventory& inventory, const RayHit& hit,
                const world::BlockDef& block, const ItemDef* tool, const InteractHooks& hooks);
  void tryPlace(world::World& world, Inventory& inventory, Player& player, const RayHit& hit,
                const InteractHooks& hooks);
  static void pickBlock(Inventory& inventory, const world::BlockDef& block,
                        const InteractHooks& hooks);

  // Cardinal the player is facing: 0:+x 1:-x 2:+z 3:-z.
  static int facingOf(const Player& player);
  // Orientation / state metadata for a freshly placed shaped block.
  static int placementMeta(const world::BlockDef& def, const Player& player, const RayHit& hit);
  static bool intersectsPlayer(int cx, int cy, int cz, const Player& player);

  bool hasSelection_ = false;
  int selX_ = 0, selY_ = 0, selZ_ = 0;

  bool hasTarget_ = false;
  int targetX_ = 0, targetY_ = 0, targetZ_ = 0;
  float progress_ = 0.0f;
  float breakFrac_ = 0.0f;

  float attackCooldown_ = 0.0f;  // seconds until the next melee swing can land
  float digSoundClock_ = 0.0f;   // rhythm clock for the mining tick sound
  bool swung_ = false;
  bool instantBreak_ = false;
};

}  // namespace hr::game
