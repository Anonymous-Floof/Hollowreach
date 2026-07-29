#include "game/interact.h"

#include <algorithm>
#include <cmath>

#include "audio/sfx.h"
#include "game/blockentities.h"
#include "game/physics.h"

namespace hr::game {
namespace {

using world::RenderKind;

constexpr float kReach = 6.0f;
constexpr float kPlayerHalfWidth = playerConst::kHalfWidth;
constexpr float kPlayerHeight = playerConst::kHeight;

// Facing -> step from the bed's foot cell to its head cell (0:+x 1:-x 2:+z 3:-z).
constexpr int kBedDir[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

bool isReplaceable(const world::World& world, int x, int y, int z) {
  return world::blocks().def(world.getBlock(x, y, z)).replaceable;
}

// The mining-tool families line up one for one apart from the sword, which mines
// nothing; a block's `tool` is never Sword, so this never matches for one.
bool toolMatches(ToolKind held, world::ToolType required) {
  switch (required) {
    case world::ToolType::Pick: return held == ToolKind::Pick;
    case world::ToolType::Axe: return held == ToolKind::Axe;
    case world::ToolType::Shovel: return held == ToolKind::Shovel;
    case world::ToolType::None: break;
  }
  return false;
}

}  // namespace

void Interact::reset() {
  hasTarget_ = false;
  progress_ = 0.0f;
  breakFrac_ = 0.0f;
}

void Interact::update(float dt, const Input& input, Player& player, world::World& world,
                      Inventory& inventory, const InteractHooks& hooks) {
  if (attackCooldown_ > 0) attackCooldown_ -= dt;
  swung_ = false;

  const Vec3 eye = player.eye();
  const Vec3 dir = player.forward();
  const RayHit hit = raycast(world, eye, dir, kReach);

  // Entity interaction: a mob or a rideable boat closer than the targeted block
  // takes the click, and the block path below never sees it.
  if (hooks.entities && hooks.entityContext) {
    EntityRayHit eHit;
    const bool hitEntity =
        hooks.entities->raycast(eye, dir, kReach, eHit) && (!hit || eHit.dist < hit.dist);
    if (hitEntity) {
      const EntityDef* def = defOf(eHit.entity->type);
      if (def && def->interact) {
        hasTarget_ = false;
        reset();
        if (input.buttonDown(MouseButton::Left) || input.clicked(MouseButton::Right)) {
          swung_ = true;
        }
        // On a guest the relay takes it and the local hook is skipped; the host's
        // answer arrives as a snapshot a moment later. The swing and the cooldown
        // are local either way, because those are feedback, not consequences.
        const auto act = [&](InteractButton button) {
          if (hooks.relayEntity && hooks.relayEntity(*eHit.entity, button)) return;
          def->interact(*eHit.entity, *hooks.entityContext, button);
        };
        if (input.clicked(MouseButton::Right)) {
          act(InteractButton::Right);
        } else if (input.buttonDown(MouseButton::Left) && attackCooldown_ <= 0.0f) {
          // A cooldown, so holding the left button does not instakill.
          audio::sfx::swing();
          act(InteractButton::Left);
          attackCooldown_ = 0.45f;
        }
        return;
      }
    }
  }

  // Left-click always swings the hand, whether it lands on a block, an
  // unbreakable one, or nothing at all.
  if (input.buttonDown(MouseButton::Left)) swung_ = true;

  const bool sneaking = input.down(Key::ShiftLeft);
  const ItemStack& heldSlot = inventory.selectedSlot();
  const ItemDef* held = heldSlot.empty() ? nullptr : getItem(heldSlot.key);

  if (input.clicked(MouseButton::Right) && held) {
    // Whether an interactive block under the cursor should take the click first.
    const world::BlockDef* target =
        hit ? &world::blocks().def(world.getBlock(hit.x, hit.y, hit.z)) : nullptr;
    const auto blockHandles = [&](bool includeAnchor) {
      if (!target || sneaking) return false;
      return target->station != world::Station::None || target->sleep || target->toggle ||
             (includeAnchor && target->anchor);
    };

    // ---- eating: works even when aiming at the sky ----
    if (held->type == ItemType::Food && !blockHandles(false)) {
      if (hooks.onEat && hooks.onEat(*held)) {
        inventory.consumeSelected();
        swung_ = true;
        return;
      }
    }
    // ---- buckets: scoop / pour. Runs before the "no hit" early-out below,
    // because the solid raycast skips straight through the water being aimed at.
    if (held->type == ItemType::Bucket && !blockHandles(true)) {
      if (useBucket(world, inventory, player, hit, hooks)) {
        swung_ = true;
        return;
      }
    }
    // ---- wayshard: warp to the surface, aiming anywhere ----
    if (held->type == ItemType::Warp && !blockHandles(true)) {
      if (hooks.onWarp && hooks.onWarp()) {
        inventory.consumeSelected();
        swung_ = true;
        return;
      }
    }
  }

  hasSelection_ = hit.hit;
  if (hit) {
    selX_ = hit.x;
    selY_ = hit.y;
    selZ_ = hit.z;
  }
  if (!hit) {
    reset();
    return;
  }

  const world::BlockId id = world.getBlock(hit.x, hit.y, hit.z);
  const world::BlockDef& b = world::blocks().def(id);

  // ---- breaking ----
  if (input.buttonDown(MouseButton::Left) && b.breakable) {
    if (!hasTarget_ || targetX_ != hit.x || targetY_ != hit.y || targetZ_ != hit.z) {
      hasTarget_ = true;
      targetX_ = hit.x;
      targetY_ = hit.y;
      targetZ_ = hit.z;
      progress_ = 0.0f;
    }
    // Only the *correct* tool speeds up mining. A block with no designated tool
    // (leaves, glass, torches) always mines at hand speed, so swinging an axe at
    // leaves is no faster than bare hands.
    float speed = 1.0f;
    if (held && held->type == ItemType::Tool && b.tool != world::ToolType::None &&
        toolMatches(held->toolType, b.tool)) {
      speed = held->speed;
    }
    const float breakTime = b.hardness / speed;
    progress_ += dt;
    breakFrac_ = std::min(1.0f, breakTime > 0 ? progress_ / breakTime : 1.0f);

    // Rhythmic dig ticks while chipping away, but not on insta-broken blocks.
    digSoundClock_ -= dt;
    if (digSoundClock_ <= 0 && breakTime > 0.25f) {
      digSoundClock_ = 0.24f;
      audio::sfx::blockHit(b, Vec3{hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f});
    }
    if (progress_ >= breakTime) {
      audio::sfx::blockBreak(b, Vec3{hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f});
      complete(world, inventory, hit, b, held, hooks);
      reset();
    }
  } else {
    hasTarget_ = false;
    progress_ = 0.0f;
    breakFrac_ = 0.0f;
    digSoundClock_ = 0.0f;
  }

  // ---- placing / using stations / toggling ----
  if (input.clicked(MouseButton::Right)) {
    swung_ = true;
    if (b.station != world::Station::None && !sneaking) {
      if (hooks.onOpenStation) hooks.onOpenStation(b.station, hit.x, hit.y, hit.z);
    } else if (b.sleep && !sneaking) {
      if (hooks.onSleep) hooks.onSleep();
    } else if (b.anchor && !sneaking) {
      if (hooks.onSetSpawn) hooks.onSetSpawn(hit.x, hit.y, hit.z);
    } else if (b.toggle && !sneaking) {
      toggleDoor(world, hit, b, player);
    } else {
      tryPlace(world, inventory, player, hit, hooks);
    }
  }
}

void Interact::toggleDoor(world::World& world, const RayHit& hit, const world::BlockDef& b,
                          Player& player) {
  // Doors flip both halves together.
  int cells[2][3] = {{hit.x, hit.y, hit.z}, {0, 0, 0}};
  int cellCount = 1;
  if (b.tall) {
    const bool upper = (world.getMeta(hit.x, hit.y, hit.z) & 2) != 0;
    const int oy = upper ? hit.y - 1 : hit.y + 1;
    if (world::blocks().def(world.getBlock(hit.x, oy, hit.z)).tall) {
      cells[1][0] = hit.x;
      cells[1][1] = oy;
      cells[1][2] = hit.z;
      cellCount = 2;
    }
  }

  // A toggle that would close the door INTO a body standing in the frame is
  // refused: otherwise the next physics sweep ejects the body out of the new
  // collision box, which was the old "close a door on yourself to teleport"
  // glitch. Entities join this list with the entity milestone; the player is the
  // one body that always exists.
  const bool playerWasStuck = bodyOverlaps(world, player.body());

  int previous[2] = {0, 0};
  for (int i = 0; i < cellCount; ++i) {
    previous[i] = world.getMeta(cells[i][0], cells[i][1], cells[i][2]);
    world.setMeta(cells[i][0], cells[i][1], cells[i][2], previous[i] ^ 1);
  }

  if (!playerWasStuck && bodyOverlaps(world, player.body())) {
    for (int i = 0; i < cellCount; ++i) {
      world.setMeta(cells[i][0], cells[i][1], cells[i][2], previous[i]);
    }
    return;
  }
  audio::sfx::doorToggle(b, (world.getMeta(hit.x, hit.y, hit.z) & 1) != 0,
                         Vec3{hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f});
}

bool Interact::useBucket(world::World& world, Inventory& inventory, Player& player,
                         const RayHit& hit, const InteractHooks& hooks) {
  const ItemStack& slot = inventory.selectedSlot();
  if (slot.empty()) return false;
  const Vec3 eye = player.eye();
  const Vec3 dir = player.forward();
  const world::WellKnownBlocks& known = world::wk();

  if (slot.key == "bucket") {
    // Scoop: a ray that stops on liquid. Only a still SOURCE cell can be taken.
    const RayHit wet = raycast(world, eye, dir, kReach, /*hitLiquid=*/true);
    if (!wet || world.getBlock(wet.x, wet.y, wet.z) != known.water) return false;
    if (world.getMeta(wet.x, wet.y, wet.z) != 0) {
      if (hooks.notify) hooks.notify("Only still source water can be scooped");
      return true;
    }
    world.setBlock(wet.x, wet.y, wet.z, world::kAir, 0);
    inventory.consumeSelected();
    const int left = inventory.give("water_bucket", 1);
    if (left > 0) {
      world.spawnDrop(player.pos().x, player.pos().y + 0.6f, player.pos().z, "water_bucket",
                      left);
    }
    audio::sfx::splash(false);
    return true;
  }

  if (slot.key == "water_bucket") {
    // Pour: into the near cell of a solid hit, or straight into whatever water or
    // air the liquid ray finds — which is how a flowing cell is topped back up to
    // a source.
    int cx, cy, cz;
    if (hit) {
      if (isReplaceable(world, hit.x, hit.y, hit.z)) {
        cx = hit.x;
        cy = hit.y;
        cz = hit.z;
      } else {
        cx = hit.nx;
        cy = hit.ny;
        cz = hit.nz;
      }
    } else {
      const RayHit wet = raycast(world, eye, dir, kReach, /*hitLiquid=*/true);
      if (!wet) return false;
      cx = wet.x;
      cy = wet.y;
      cz = wet.z;
    }
    const world::BlockId existing = world.getBlock(cx, cy, cz);
    const world::BlockDef& eb = world::blocks().def(existing);
    if (existing != world::kAir && eb.render != RenderKind::Liquid && !eb.replaceable) {
      return false;
    }
    world.setBlock(cx, cy, cz, known.water, 0);  // meta 0 = a source
    inventory.consumeSelected();
    const int left = inventory.give("bucket", 1);
    if (left > 0) {
      world.spawnDrop(player.pos().x, player.pos().y + 0.6f, player.pos().z, "bucket", left);
    }
    audio::sfx::splash(true);
    return true;
  }

  return false;  // the milk bucket has no pour use yet
}

void Interact::complete(world::World& world, Inventory& inventory, const RayHit& hit,
                        const world::BlockDef& b, const ItemDef* tool,
                        const InteractHooks& hooks) {
  // A block only *requires* a tool when its mining tier is above hand. Soft
  // blocks — turf, loam, wood, sand — always drop, by hand or any tool; the
  // matching tool just mines them faster.
  const bool gated = b.minTier > 0;
  const bool correctTool =
      tool && tool->type == ItemType::Tool && toolMatches(tool->toolType, b.tool);
  const bool canDrop = !b.drop.empty() && (!gated || (correctTool && tool->tier >= b.minTier));
  if (canDrop) {
    world.spawnDrop(hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f, b.drop, b.dropCount);
  } else if (gated && hooks.notify) {
    static const char* kTierNames[] = {"", "wood", "stone", "copper", "iron"};
    const char* need = kTierNames[std::min(4, b.minTier)];
    const char* what = b.tool == world::ToolType::Pick     ? "pick"
                       : b.tool == world::ToolType::Axe    ? "axe"
                       : b.tool == world::ToolType::Shovel ? "shovel"
                                                           : "tool";
    hooks.notify(std::string("Needs a ") + (*need ? need : "better") + "+ tier " + what +
                 " to harvest");
  }

  // Spill any block-entity contents (forge, chest) as item drops, then drop the
  // entity itself.
  if (b.station != world::Station::None) {
    if (const BlockEntity* be = world.getBlockEntity(hit.x, hit.y, hit.z)) {
      for (const ItemStack& s : entityContents(*be)) {
        world.spawnDrop(hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f, s.key, s.count, s.dura);
      }
    }
    world.removeBlockEntity(hit.x, hit.y, hit.z);
  }

  // Doors are two stacked cells — remove the partner half too.
  if (b.tall) {
    const bool upper = (world.getMeta(hit.x, hit.y, hit.z) & 2) != 0;
    const int oy = upper ? hit.y - 1 : hit.y + 1;
    if (world::blocks().def(world.getBlock(hit.x, oy, hit.z)).tall) {
      world.setBlock(hit.x, oy, hit.z, world::kAir, 0);
    }
  }
  // Beds are two horizontal cells — remove the partner (foot <-> head) too.
  if (b.render == RenderKind::Bed) {
    const int meta = world.getMeta(hit.x, hit.y, hit.z);
    const int dx = kBedDir[meta & 3][0], dz = kBedDir[meta & 3][1];
    const bool isHead = (meta & 4) != 0;
    const int ox = isHead ? hit.x - dx : hit.x + dx;
    const int oz = isHead ? hit.z - dz : hit.z + dz;
    if (world::blocks().def(world.getBlock(ox, hit.y, oz)).render == RenderKind::Bed) {
      world.setBlock(ox, hit.y, oz, world::kAir, 0);
    }
  }

  world.setBlock(hit.x, hit.y, hit.z, world::kAir, 0);
  if (b.anchor && hooks.onAnchorBroken) hooks.onAnchorBroken(hit.x, hit.y, hit.z);
  if (b.isLog) world.decayLeavesAround(hit.x, hit.y, hit.z);
  if (tool && tool->type == ItemType::Tool) inventory.damageSelectedTool(1);
}

void Interact::tryPlace(world::World& world, Inventory& inventory, Player& player,
                        const RayHit& hit, const InteractHooks& hooks) {
  const ItemStack& slot = inventory.selectedSlot();
  if (slot.empty()) return;
  const ItemDef* item = getItem(slot.key);
  if (!item) return;

  // Boats are not blocks — using one spawns a rideable entity in the empty cell.
  // That entity arrives with the entity milestone.
  if (item->type == ItemType::Boat) return;
  if (item->type != ItemType::Block) return;

  // Aiming at a replaceable plant (grass, a flower) places into that cell and
  // swaps the plant out; otherwise the block lands on the near face as usual.
  int cx = hit.nx, cy = hit.ny, cz = hit.nz;
  if (isReplaceable(world, hit.x, hit.y, hit.z)) {
    cx = hit.x;
    cy = hit.y;
    cz = hit.z;
  }
  const world::BlockId existing = world.getBlock(cx, cy, cz);
  const world::BlockDef& eb = world::blocks().def(existing);
  if (existing != world::kAir && eb.render != RenderKind::Liquid && !eb.replaceable) return;

  const world::BlockDef& placed = world::blocks().def(item->blockId);
  if (placed.solid && intersectsPlayer(cx, cy, cz, player)) return;

  // Shore plants (papyrus): only on sand, grass or dirt beside water, or stacked
  // on their own kind.
  if (placed.shore) {
    const world::WellKnownBlocks& known = world::wk();
    const world::BlockId below = world.getBlock(cx, cy - 1, cz);
    const bool groundOk = below == known.sand || below == known.turf || below == known.loam;
    bool nearWater = below == item->blockId;  // stacking a reed is always fine
    if (!nearWater && groundOk) {
      static constexpr int kSides[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& s : kSides) {
        if (world.getBlock(cx + s[0], cy - 1, cz + s[1]) == known.water) {
          nearWater = true;
          break;
        }
      }
    }
    if (!nearWater) {
      if (hooks.notify) hooks.notify("Papyrus needs wet shore ground");
      return;
    }
  }

  // bit0 marks a player-placed leaf, exempting it from natural leaf decay.
  const int meta = placed.isLeaf ? 1 : placementMeta(placed.render, player, hit);

  // Beds occupy two horizontal cells: foot plus head, extending away from the
  // player.
  if (placed.render == RenderKind::Bed) {
    const int f = facingOf(player);
    const int hx = cx + kBedDir[f][0], hz = cz + kBedDir[f][1];
    const world::BlockId headExisting = world.getBlock(hx, cy, hz);
    if (headExisting != world::kAir &&
        world::blocks().def(headExisting).render != RenderKind::Liquid) {
      return;
    }
    if (intersectsPlayer(hx, cy, hz, player)) return;
    world.setBlock(cx, cy, cz, item->blockId, f);      // foot (bit2 = 0)
    world.setBlock(hx, cy, hz, item->blockId, f | 4);  // head (bit2 = 1)
    inventory.consumeSelected();
    audio::sfx::blockPlace(placed, Vec3{cx + 0.5f, cy + 0.5f, cz + 0.5f});
    return;
  }

  // Doors occupy two stacked cells.
  if (placed.tall) {
    if (world.getBlock(cx, cy + 1, cz) != world::kAir) return;
    if (intersectsPlayer(cx, cy + 1, cz, player)) return;
    world.setBlock(cx, cy, cz, item->blockId, meta);
    world.setBlock(cx, cy + 1, cz, item->blockId, meta | 2);  // bit1 = upper
    inventory.consumeSelected();
    audio::sfx::blockPlace(placed, Vec3{cx + 0.5f, cy + 0.5f, cz + 0.5f});
    return;
  }

  world.setBlock(cx, cy, cz, item->blockId, meta);
  // A station block carries state from the moment it exists, so its forge starts
  // smelting or its chest keeps items even if it is never opened.
  const BlockEntityKind kind = entityKindFor(placed.key);
  if (kind != BlockEntityKind::None) world.getOrCreateBlockEntity(cx, cy, cz, kind);
  inventory.consumeSelected();
  audio::sfx::blockPlace(placed, Vec3{cx + 0.5f, cy + 0.5f, cz + 0.5f});
}

int Interact::facingOf(const Player& player) {
  const Vec3 f = player.forward();
  if (std::fabs(f.x) >= std::fabs(f.z)) return f.x >= 0 ? 0 : 1;
  return f.z >= 0 ? 2 : 3;
}

int Interact::placementMeta(RenderKind render, const Player& player, const RayHit& hit) {
  const int f = facingOf(player);
  const int fnx = hit.nx - hit.x, fny = hit.ny - hit.y, fnz = hit.nz - hit.z;
  // Where on the clicked cell the cursor landed: the top half if we clicked the
  // underside of a block, or the upper half of a side face.
  const float fracY = hit.point.y - static_cast<float>(hit.y);
  const bool topHalf = fny == -1 || (fny == 0 && fracY > 0.5f);

  switch (render) {
    case RenderKind::Slab:
      return topHalf ? 1 : 0;
    case RenderKind::Stair:
      return f | (topHalf ? 4 : 0);  // bit2 = upside-down
    case RenderKind::VSlab: {
      if (fnx == 1) return 0;  // hug the wall we clicked
      if (fnx == -1) return 1;
      if (fnz == 1) return 2;
      if (fnz == -1) return 3;
      static constexpr int kFaceThePlayer[4] = {1, 0, 3, 2};
      return kFaceThePlayer[f];
    }
    case RenderKind::Cross: {
      // Torch: a wall mount faces into the room.
      if (fnx == 1) return 1;
      if (fnx == -1) return 2;
      if (fnz == 1) return 3;
      if (fnz == -1) return 4;
      return 0;  // floor (standing)
    }
    case RenderKind::Door:
      return f << 2;
    case RenderKind::Trapdoor: {
      const int top = fny == -1 ? 1 : 0;  // placed under a block -> hinge at top
      return (top << 1) | (f << 2);
    }
    case RenderKind::Ladder: {
      if (fnx == -1) return 0;  // hug the wall we clicked
      if (fnx == 1) return 1;
      if (fnz == -1) return 2;
      if (fnz == 1) return 3;
      return f ^ 1;  // floor or ceiling -> the wall behind
    }
    default:
      return 0;
  }
}

bool Interact::intersectsPlayer(int cx, int cy, int cz, const Player& player) {
  const Vec3 p = player.pos();
  return cx + 1 > p.x - kPlayerHalfWidth && cx < p.x + kPlayerHalfWidth && cy + 1 > p.y &&
         cy < p.y + kPlayerHeight && cz + 1 > p.z - kPlayerHalfWidth &&
         cz < p.z + kPlayerHalfWidth;
}

}  // namespace hr::game
