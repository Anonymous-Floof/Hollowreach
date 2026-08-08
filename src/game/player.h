// First-person player, ported from js/game/player.js.
//
// AABB physics against the voxel world, walking, jumping, swimming, ladders, and a
// creative-style fly toggle on a double-tap of space.
//
// Horizontal velocity is *assigned* from input each frame rather than integrated,
// with no damping term — which is why the variable timestep is kept: horizontal
// displacement is exactly speed x elapsed at any frame rate. Only the vertical axis
// integrates, and its jump apex varies by less than a fifth of a block across the
// whole rate range, never crossing a ledge height that matters.

#pragma once

#include <functional>

#include "core/input.h"
#include "core/mat4.h"
#include "game/physics.h"
#include "world/world.h"

namespace hr::game {

// Movement constants, from js/game/player.js:9-23.
namespace playerConst {
inline constexpr float kHalfWidth = 0.3f;
inline constexpr float kHeight = 1.8f;
inline constexpr float kEyeHeight = 1.62f;
inline constexpr float kGravity = 28.0f;
inline constexpr float kJump = 8.6f;
inline constexpr float kWalk = 4.5f;
inline constexpr float kFly = 11.0f;
inline constexpr float kSwim = 3.8f;      // horizontal swim speed
inline constexpr float kSwimUp = 4.6f;    // ascend while holding jump in water
inline constexpr float kSwimDown = 3.4f;  // dive while holding sneak in water
inline constexpr float kSwimSink = 1.1f;  // passive sink terminal velocity
inline constexpr float kFlowPush = 7.0f;  // how hard a current shoves the player
inline constexpr float kClimb = 3.2f;     // ladder climb and descend speed
inline constexpr float kStep = 0.6f;      // auto-step height on land
inline constexpr float kSneakScale = 0.45f;
// Crouching sinks the head this far, eased in and out at this rate. The drop is
// applied to the aiming eye and not only to the camera, so what you can see under
// an overhang is what you can reach under it.
inline constexpr float kSneakDrop = 0.35f;  // 1.62 standing to 1.27 crouched
inline constexpr float kSneakRate = 14.0f;
// How far below the feet the ground is looked for when crouching near a drop, and
// how much of a foothold counts. A crouching player will not step off solid ground
// into nothing, so this is the test for "is there still something under me".
inline constexpr float kLedgeProbe = 0.06f;
// Camera catch-up after an auto-step. Exponential, so the eye is never left
// visibly low, with a linear floor in blocks per second so it actually arrives
// instead of asymptoting for the rest of the walk. A cap, because the swimming
// climb-out lifts a whole block and a full block of lag reads as falling.
inline constexpr float kStepSmoothRate = 14.0f;
inline constexpr float kStepSmoothFloor = 1.2f;
inline constexpr float kStepSmoothMax = 1.0f;
inline constexpr float kSprintScale = 1.3f;
// The window for a second press of space to mean "fly".
inline constexpr double kDoubleTapSeconds = 0.3;
}  // namespace playerConst

// Survival constants, from js/game/player.js:36-52 and survival().
namespace survivalConst {
inline constexpr float kMaxHealth = 20.0f;
inline constexpr float kMaxHunger = 20.0f;
inline constexpr float kMaxBreath = 10.0f;
inline constexpr float kBreathRefill = 5.0f;   // per second out of water
inline constexpr float kDrownInterval = 1.0f;  // seconds between drowning hits
inline constexpr float kDrownDamage = 2.0f;
inline constexpr float kStarveInterval = 4.0f;
inline constexpr float kHungerBase = 0.15f;    // exhaustion per second, standing
inline constexpr float kHungerSprint = 0.4f;
inline constexpr float kHungerSwim = 0.15f;
inline constexpr float kExhaustionPerPoint = 4.0f;
inline constexpr float kRegenInterval = 2.5f;
inline constexpr float kRegenDelay = 6.0f;     // pause after a hit
inline constexpr float kWellFed = 18.0f;
inline constexpr float kRegenExhaustion = 3.0f;
inline constexpr float kFallSafe = 3.5f;       // free fall distance
inline constexpr float kLandThud = 1.4f;
}  // namespace survivalConst

// Settings-driven flags, passed per frame rather than read from a global so the
// player stays testable.
struct PlayerOptions {
  bool flightAllowed = true;
  // Creative. Nothing can take health off you, and the world is not solid.
  //
  // No-clip implies flight rather than standing alone: moveAxis is what reports
  // whether a move was blocked, and that report is what sets onGround_ and drives
  // fall damage. A body that never collides also never lands, so walking through
  // walls without flying means falling through the floor forever.
  bool invulnerable = false;
  bool noClip = false;
  bool hungerEnabled = false;
  bool fallDamageEnabled = true;
  float stepHeight = playerConst::kStep;
  float sensitivity = 0.0024f;
  bool invertY = false;
  // Armour points, summed from the worn pieces. Halves fall damage per two points
  // and soaks a fraction of every non-environmental hit.
  float defense = 0.0f;
};

// What a food item does when eaten. Only the fields Player::eat() reads, so the item
// registry stays out of the player's header.
struct FoodEffect {
  float food = 0;
  bool risky = false;  // rotten flesh: a symmetric gamble around `food`
};

// Everything about the player that survives a save, and nothing else — the same
// whitelist as js/game/player.js:332. Breath, the drown and starve timers and the
// regeneration delay are deliberately left out: the web build did not save them,
// and every one of them refills or expires within seconds of loading anyway.
struct PlayerState {
  Vec3 pos;
  float yaw = 0.0f, pitch = 0.0f;
  float health = survivalConst::kMaxHealth;
  float hunger = survivalConst::kMaxHunger;
  float saturation = 5.0f;
  bool flying = false;
};

class Player {
 public:
  Player(float x, float y, float z);

  PlayerState state() const;
  // Restores a saved player. Out-of-range values are clamped rather than rejected,
  // so a hand-edited or partly corrupt save produces a playable player instead of
  // one with negative health.
  void loadState(const PlayerState& s);

  Body& body() { return body_; }
  const Body& body() const { return body_; }
  const Vec3& pos() const { return body_.pos; }
  void setPos(const Vec3& p) { body_.pos = p; }

  // Moves the player somewhere else without charging them for the trip: velocity is
  // cleared and the fall tracker restarted from the destination. `setPos` alone
  // leaves fallStartY_ wherever the journey began, so a respawn taken mid-fall lands
  // as a drop of however far the player had already fallen — which kills them again
  // on touchdown, and again, and again.
  void teleport(const Vec3& p);

  // The entity id of whatever the player is riding, or 0. An id rather than a
  // pointer because the entity vector reallocates whenever anything spawns, and a
  // mount that dies underneath its rider must not leave a dangling reference.
  int mount() const { return mount_; }
  void setMount(int entityId) { mount_ = entityId; }

  Vec3 velocity() const { return vel_; }
  void setVelocity(const Vec3& v) { vel_ = v; }

  float yaw() const { return yaw_; }
  float pitch() const { return pitch_; }
  void setLook(float yaw, float pitch) {
    yaw_ = yaw;
    pitch_ = pitch;
  }

  // The eye used for aiming and raycasts. Deliberately excludes head bob so the
  // crosshair does not drift while walking — but it does include the crouch, which
  // is a real change of where the head is rather than a camera flourish.
  Vec3 eye() const {
    return {body_.pos.x,
            body_.pos.y + playerConst::kEyeHeight - sneakSmooth_ * playerConst::kSneakDrop,
            body_.pos.z};
  }
  // Added to the render camera only: the walk bob, plus whatever is left of the
  // last auto-step's rise. Both are presentation, which is why the aiming eye
  // above does not see either of them.
  Vec3 viewOffset() const;
  // The same walk cycle the camera bob rides on, so the held item can sway in step
  // with the head instead of on a clock of its own.
  float bobPhase() const { return bobPhase_; }
  float bobMagnitude() const { return bobMagnitude_; }
  Vec3 forward() const { return lookDir(yaw_, pitch_); }

  bool onGround() const { return onGround_; }
  bool flying() const { return flying_; }
  bool swimming() const { return swimming_; }
  bool climbing() const { return climbing_; }
  bool sprinting() const { return sprinting_; }
  bool sneaking() const { return sneaking_; }
  // How far into the crouch the head currently is, 0 to 1. Presentation reads this
  // to pose the body; the eye already has it applied.
  float sneakAmount() const { return sneakSmooth_; }

  // Mouse look, applied per frame at render rate so aim latency stays low.
  void look(double dx, double dy, const PlayerOptions& options);

  void update(float dt, const Input& input, const world::World& world,
              const PlayerOptions& options, double simTime);

  // True when the player's mid-body is inside water, which is what drives swimming.
  bool inWater(const world::World& world) const;
  bool headInWater(const world::World& world) const;
  bool onLadder(const world::World& world) const;

  // Distance fallen since leaving the ground, or 0 when not falling. Fall damage
  // reads it on landing; the landing thud will read it in the audio milestone.
  float fallDistance() const;

  // --- survival (js/game/player.js:197-293) ---------------------------------
  // Breath drains underwater then drowns, hunger burns a hidden saturation buffer
  // before the visible bar, and regeneration waits out a delay after every hit.
  // Called from update(), so a sub-stepped frame drains at the same rate.
  float health() const { return health_; }
  float maxHealth() const { return survivalConst::kMaxHealth; }
  float hunger() const { return hunger_; }
  float maxHunger() const { return survivalConst::kMaxHunger; }
  float breath() const { return breath_; }
  float maxBreath() const { return survivalConst::kMaxBreath; }
  bool hungerOn() const { return hungerOn_; }
  bool dead() const { return health_ <= 0.0f; }

  void damage(float amount, const PlayerOptions& options, bool ignoreArmor = false);
  void heal(float amount);
  // Returns false when the item should not be consumed — a full bar, or a heal with
  // nothing to heal.
  bool eat(const FoodEffect& food);
  // Restores full health, hunger and breath. Used by respawn and by a fresh world.
  void reviveFull();
  void setHealth(float v);

  // Fires when a hit lands, so the interface can flash. The hurt sound is played
  // from damage() directly, which is where the JS put it too.
  std::function<void()> onHurt;
  // Fires when health reaches zero.
  std::function<void()> onDeath;
  // Armour takes the wear from a blow that was not environmental.
  std::function<void(int)> onArmorDamage;

 private:
  void survival(float dt, const world::World& world, const PlayerOptions& options);
  void addFood(float food, float saturation);

  // Horizontal move with auto-step. Swimming climbs a full block so you can haul
  // yourself onto a shore, but only when the head would surface into air.
  void stepMove(const world::World& world, int axis, float delta, bool swimming,
                bool movingInto, bool wasGround);
  bool moveAxis(const world::World& world, int axis, float delta);
  // Is there anything solid immediately under the body's footprint? The crouch
  // edge-guard asks this before and after each horizontal step.
  bool groundBelow(const world::World& world) const;
  // Hands the camera the vertical distance an auto-step just moved the body.
  void noteStepRise(float rise);

  Body body_;
  Vec3 vel_;
  float yaw_ = 0.0f;
  float pitch_ = 0.0f;

  int mount_ = 0;
  bool onGround_ = false;
  bool flying_ = false;
  bool swimming_ = false;
  bool climbing_ = false;
  bool sprinting_ = false;

  // Accumulated simulation time, not wall clock: multiplayer never pauses, and the
  // web build's use of performance.now() here made the fly toggle behave differently
  // depending on how long a menu had been open.
  double lastSpaceTime_ = -1.0;

  bool falling_ = false;
  float fallStartY_ = 0.0f;

  // Refreshed from PlayerOptions each frame; the "High Step" setting raises it to a
  // full block.
  float stepHeight_ = playerConst::kStep;

  // Bob phase advances with walking speed; magnitude eases in and out so the camera
  // sways while moving and settles when still.
  float bobPhase_ = 0.0f;
  float bobMagnitude_ = 0.0f;

  // How much of the last auto-step's rise the camera has yet to catch up on. An
  // auto-step moves the body a whole half-block in one frame, and without this the
  // eye teleports with it — every stair tread a visible jolt.
  float stepSmooth_ = 0.0f;
  bool sneaking_ = false;
  float sneakSmooth_ = 0.0f;

  float health_ = survivalConst::kMaxHealth;
  float hunger_ = survivalConst::kMaxHunger;
  float saturation_ = 5.0f;   // hidden buffer, drains before the visible bar
  float exhaustion_ = 0.0f;   // every 4 points costs one saturation or food point
  float breath_ = survivalConst::kMaxBreath;
  float starveTimer_ = 0.0f;
  float drownTimer_ = 0.0f;
  float regenTimer_ = 0.0f;
  float regenDelay_ = 0.0f;
  bool hungerOn_ = false;
  bool noClip_ = false;
};

}  // namespace hr::game
