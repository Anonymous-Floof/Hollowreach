#include "game/player.h"

#include <algorithm>
#include <cmath>

#include "audio/sfx.h"
#include "core/prng.h"

namespace hr::game {
namespace {
using namespace playerConst;
constexpr float EPS = kCollisionEpsilon;
constexpr float kPi = 3.14159265358979323846f;

// The heights the things in the way actually present, lowest first.
//
// stepMove used to lift the body by the WHOLE step height and test for headroom
// there, which asks for more room than the step it is about to make. At High
// Step's 1.0 the body occupies feet+1.0 to feet+2.8 during that probe, while
// landing on a stair's half-block tread only ever needed 2.3. So a staircase with
// a roof over it — which is every staircase indoors — refused the step that the
// smaller default step happily made, and turning High Step ON was what stopped
// you walking up stairs. Measured: a flight under a four-block ceiling climbs its
// full six blocks at 0.6 and stalls after one and a half at 1.0.
//
// So the probe now asks for the least it can. The candidates are the tops of
// whatever is in front of the body, within reach, tried in ascending order — a
// stair is tried at 0.5 before 1.0 is ever considered.
int gatherStepRises(const world::World& world, const Body& body, int axis, float delta,
                    float maxStep, float* out, int cap) {
  // The volume the body was trying to move into, from its feet up to as far as it
  // could possibly step.
  Vec3 lo {body.pos.x - body.hw, body.pos.y + EPS, body.pos.z - body.hw};
  Vec3 hi {body.pos.x + body.hw, body.pos.y + maxStep, body.pos.z + body.hw};
  if (axis == 0) {
    (delta > 0 ? hi.x : lo.x) += delta;
  } else {
    (delta > 0 ? hi.z : lo.z) += delta;
  }

  int n = 0;
  const int x0 = static_cast<int>(std::floor(lo.x)), x1 = static_cast<int>(std::floor(hi.x));
  const int y0 = static_cast<int>(std::floor(lo.y)), y1 = static_cast<int>(std::floor(hi.y));
  const int z0 = static_cast<int>(std::floor(lo.z)), z1 = static_cast<int>(std::floor(hi.z));
  for (int x = x0; x <= x1 && n < cap; ++x) {
    for (int y = y0; y <= y1 && n < cap; ++y) {
      for (int z = z0; z <= z1 && n < cap; ++z) {
        const std::vector<world::Box> boxes = world.collisionBoxesAt(x, y, z);
        for (const world::Box& b : boxes) {
          const float rise = b.y1 - body.pos.y;
          if (rise <= EPS || rise > maxStep + EPS) continue;
          bool seen = false;
          for (int i = 0; i < n; ++i) {
            if (std::fabs(out[i] - rise) < EPS) seen = true;
          }
          if (!seen && n < cap) out[n++] = rise;
        }
      }
    }
  }
  std::sort(out, out + n);
  return n;
}
}  // namespace

Player::Player(float x, float y, float z) {
  body_.pos = {x, y, z};
  body_.hw = kHalfWidth;
  body_.h = kHeight;
}

void Player::look(double dx, double dy, const PlayerOptions& options) {
  yaw_ -= static_cast<float>(dx) * options.sensitivity;
  pitch_ += static_cast<float>(options.invertY ? dy : -dy) * options.sensitivity;
  // Just short of straight up or down, so the view basis never degenerates.
  const float limit = kPi / 2.0f - 0.01f;
  pitch_ = std::clamp(pitch_, -limit, limit);
}

bool Player::inWater(const world::World& world) const {
  return world.getBlock(static_cast<int>(std::floor(body_.pos.x)),
                        static_cast<int>(std::floor(body_.pos.y + 0.9f)),
                        static_cast<int>(std::floor(body_.pos.z))) == world::wk().water;
}

bool Player::headInWater(const world::World& world) const {
  const Vec3 e = eye();
  return world.getBlock(static_cast<int>(std::floor(e.x)), static_cast<int>(std::floor(e.y)),
                        static_cast<int>(std::floor(e.z))) == world::wk().water;
}

bool Player::onLadder(const world::World& world) const {
  const Vec3& p = body_.pos;
  const int x0 = static_cast<int>(std::floor(p.x - kHalfWidth));
  const int x1 = static_cast<int>(std::floor(p.x + kHalfWidth - EPS));
  const int y0 = static_cast<int>(std::floor(p.y));
  const int y1 = static_cast<int>(std::floor(p.y + kHeight - EPS));
  const int z0 = static_cast<int>(std::floor(p.z - kHalfWidth));
  const int z1 = static_cast<int>(std::floor(p.z + kHalfWidth - EPS));
  for (int x = x0; x <= x1; ++x) {
    for (int y = y0; y <= y1; ++y) {
      for (int z = z0; z <= z1; ++z) {
        if (world::blocks().def(world.getBlock(x, y, z)).climb) return true;
      }
    }
  }
  return false;
}

float Player::fallDistance() const {
  if (!falling_) return 0.0f;
  const float d = fallStartY_ - body_.pos.y;
  return d > 0 ? d : 0.0f;
}

bool Player::moveAxis(const world::World& world, int axis, float delta) {
  // Every move the player makes goes through here, including the probes auto-step
  // uses, so this one branch is the whole of no-clip.
  //
  // Returning false says "nothing was in the way", which is true and also what
  // keeps onGround_ clear and fall damage silent while phasing through rock.
  if (noClip_) {
    Vec3 p = body_.pos;
    if (axis == 0) p.x += delta;
    else if (axis == 1) p.y += delta;
    else p.z += delta;
    body_.pos = p;
    return false;
  }
  return sweepAxis(world, body_, axis, delta);
}

bool Player::groundBelow(const world::World& world) const {
  // A thin slab just under the feet. Deliberately the full width of the body: any
  // corner still over solid ground counts as a foothold, which is what lets a
  // crouching player stand right on the lip of a block rather than being held back
  // half a block from it.
  const Vec3 lo {body_.pos.x - kHalfWidth, body_.pos.y - kLedgeProbe, body_.pos.z - kHalfWidth};
  const Vec3 hi {body_.pos.x + kHalfWidth, body_.pos.y - EPS, body_.pos.z + kHalfWidth};
  return aabbBlocked(world, lo, hi);
}

void Player::noteStepRise(float rise) {
  if (rise <= 0.0f) return;
  stepSmooth_ = std::min(kStepSmoothMax, stepSmooth_ + rise);
}

void Player::stepMove(const world::World& world, int axis, float delta, bool swimming,
                      bool movingInto, bool wasGround) {
  const bool blocked = moveAxis(world, axis, delta);
  if (!blocked || !movingInto || !(swimming || wasGround)) return;

  // Swimming lifts a whole block so you can haul yourself onto a shore; on land it
  // is the small ledge height that walks up stairs and slabs.
  const float stepH = swimming ? 1.0f : stepHeight_;
  const float savedAxis = axis == 0 ? body_.pos.x : body_.pos.z;
  const float savedY = body_.pos.y;

  // What is actually in the way, lowest first, with the full step height as the
  // last resort — that last entry is what this used to try, and only try. Water
  // keeps the single full-height probe: hauling yourself onto a shore is a lift of
  // a whole block by definition, and the surfacing rule below is written for it.
  float rises[9];
  int count = 0;
  if (!swimming) count = gatherStepRises(world, body_, axis, delta, stepH, rises, 8);
  rises[count++] = stepH;

  for (int i = 0; i < count; ++i) {
    body_.pos.y = savedY + rises[i] + EPS;
    // No headroom at this height. A taller one may still clear whatever is
    // overhead, so keep going rather than giving up here.
    if (bodyOverlaps(world, body_)) continue;

    if (!moveAxis(world, axis, delta)) {
      if (!swimming) {
        // Stepped onto the ledge. Settle straight down onto it rather than leaving
        // the body floating and letting gravity find the surface: that fall takes
        // several frames during which onGround_ is false, which stops the walk
        // cycle dead and starts it again on landing. Once per tread, that flicker
        // is what a staircase felt like.
        //
        // Dropped by the whole step height, not by the rise that succeeded: the
        // sweep stops at the first surface under the new footprint, so asking for
        // more than is needed still lands on the tread and cannot overshoot it.
        moveAxis(world, 1, -(stepH + EPS));
        noteStepRise(body_.pos.y - savedY);
        return;
      }
      // Only climb out of water if the head would surface into open air, otherwise
      // you would swim up through an overhang.
      const bool headAir =
          world.getBlock(static_cast<int>(std::floor(body_.pos.x)),
                         static_cast<int>(std::floor(body_.pos.y + kHeight)),
                         static_cast<int>(std::floor(body_.pos.z))) == world::kAir;
      if (headAir) {
        vel_.y = std::max(vel_.y, 0.0f);
        noteStepRise(body_.pos.y - savedY);
        return;
      }
    }
    // Blocked at this height too. Put the horizontal position back before trying
    // the next one, or a partial slide would be carried into it.
    if (axis == 0) body_.pos.x = savedAxis;
    else body_.pos.z = savedAxis;
  }

  // Nothing worked: undo the probe entirely.
  if (axis == 0) body_.pos.x = savedAxis;
  else body_.pos.z = savedAxis;
  body_.pos.y = savedY;
}

void Player::update(float dt, const Input& input, const world::World& world,
                    const PlayerOptions& options, double simTime) {
  stepHeight_ = options.stepHeight;
  hungerOn_ = options.hungerEnabled;
  // Mirrored for the same reason hunger is: moveAxis is reached from places that
  // never saw the options, including the auto-step probes.
  noClip_ = options.noClip;

  // No-clip flies, always. See the note on PlayerOptions::noClip: a body that never
  // collides never lands, so without this it falls out of the world.
  const bool canFly = options.flightAllowed || options.noClip;
  // Switched into no-clip mid-stride: start flying rather than waiting for a
  // double tap, since the floor has just stopped being there.
  if (options.noClip && !flying_) {
    flying_ = true;
    vel_.y = 0.0f;
  }
  if (!canFly && flying_) {
    flying_ = false;
    vel_.y = 0.0f;
  }

  // Fly toggles on a double tap of space. Timed against accumulated simulation
  // time, not wall clock, so a paused menu cannot swallow or fake the second tap.
  if (input.pressed(Key::Space)) {
    if (canFly && lastSpaceTime_ >= 0.0 && simTime - lastSpaceTime_ < kDoubleTapSeconds) {
      flying_ = !flying_;
      vel_.y = 0.0f;
    }
    lastSpaceTime_ = simTime;
  }

  const float sinYaw = std::sin(yaw_);
  const float cosYaw = std::cos(yaw_);
  float fx = 0.0f, fz = 0.0f;
  if (input.down(Key::W)) {
    fx -= sinYaw;
    fz -= cosYaw;
  }
  if (input.down(Key::S)) {
    fx += sinYaw;
    fz += cosYaw;
  }
  if (input.down(Key::A)) {
    fx -= cosYaw;
    fz += sinYaw;
  }
  if (input.down(Key::D)) {
    fx += cosYaw;
    fz -= sinYaw;
  }
  const float len = std::sqrt(fx * fx + fz * fz);
  if (len > 0) {
    fx /= len;
    fz /= len;
  }

  const bool wasGround = onGround_;
  swimming_ = !flying_ && inWater(world);
  climbing_ = !flying_ && !swimming_ && onLadder(world);
  const bool sneak = input.down(Key::ShiftLeft) && !flying_ && !swimming_;
  sneaking_ = sneak;
  // Eased rather than snapped, so the head sinks into the crouch and rises out of
  // it. A step change here reads as a glitch; this reads as ducking.
  {
    const float want = sneak ? 1.0f : 0.0f;
    sneakSmooth_ += (want - sneakSmooth_) * std::min(1.0f, dt * kSneakRate);
    if (std::fabs(want - sneakSmooth_) < 0.001f) sneakSmooth_ = want;
  }

  float speed = flying_ ? kFly : (swimming_ ? kSwim : (sneak ? kWalk * kSneakScale : kWalk));
  sprinting_ = !swimming_ && !sneak && len > 0 && input.down(Key::ControlLeft);
  if (sprinting_) speed *= kSprintScale;

  // Assigned, not accumulated: this is what makes horizontal motion exact at any
  // frame rate.
  vel_.x = fx * speed;
  vel_.z = fz * speed;

  if (flying_) {
    float vy = 0.0f;
    if (input.down(Key::Space)) vy += 1.0f;
    if (input.down(Key::ShiftLeft)) vy -= 1.0f;
    vel_.y = vy * kFly;
  } else if (swimming_) {
    // Buoyant: weak gravity, a gently capped sink, space rises and shift dives.
    vel_.y -= kGravity * 0.22f * dt;
    if (vel_.y < -kSwimSink) vel_.y = -kSwimSink;
    if (input.down(Key::Space)) vel_.y = kSwimUp;
    else if (input.down(Key::ShiftLeft)) vel_.y = -kSwimDown;
  } else if (climbing_) {
    // Cling to the ladder: climb with W or space, descend with shift, otherwise
    // slide slowly rather than falling.
    if (input.down(Key::W) || input.down(Key::Space)) vel_.y = kClimb;
    else if (input.down(Key::ShiftLeft)) vel_.y = -kClimb;
    else vel_.y = -kClimb * 0.25f;
    falling_ = false;
  } else {
    vel_.y -= kGravity * dt;
    if (input.down(Key::Space) && onGround_) {
      vel_.y = kJump;
      onGround_ = false;
    }
  }

  // Fall tracking, which never accrues while flying, swimming or climbing.
  if (!flying_ && !swimming_ && !climbing_) {
    if (vel_.y < 0 && !falling_ && !onGround_) {
      falling_ = true;
      fallStartY_ = body_.pos.y;
    }
  } else {
    falling_ = false;
  }

  // A water current drags the player downstream, wading or swimming. The world had
  // no fluid-flow query until the entity milestone needed one for floating bodies;
  // this call site is js/game/player.js:148-154 and had been left out with it.
  if (!flying_) {
    const int fcx = static_cast<int>(std::floor(body_.pos.x));
    const int fcy = static_cast<int>(std::floor(body_.pos.y + 0.1f));
    const int fcz = static_cast<int>(std::floor(body_.pos.z));
    if (world.getBlock(fcx, fcy, fcz) == world::wk().water) {
      float fx = 0.0f, fz = 0.0f;
      world.waterFlow(fcx, fcy, fcz, fx, fz);
      vel_.x += fx * kFlowPush;
      vel_.z += fz * kFlowPush;
    }
  }

  onGround_ = false;
  const bool movingInto = len > 0;

  // Crouching on solid ground will not walk you off it. The guard is armed from
  // what was under the feet *before* the step, so a player already over a drop —
  // mid-fall, or standing where the ground was just mined away — is not frozen in
  // the air by holding crouch.
  //
  // Each axis is tested and undone on its own, which is what makes it feel right
  // rather than merely safe: walking diagonally at a corner, the component that
  // would take you off is refused and the one along the edge still goes through,
  // so you slide along the rim instead of stopping dead against nothing.
  const bool edgeGuard = sneak && wasGround && groundBelow(world);

  const float savedX = body_.pos.x;
  stepMove(world, 0, vel_.x * dt, swimming_, movingInto, wasGround);
  if (edgeGuard && !groundBelow(world)) {
    body_.pos.x = savedX;
    vel_.x = 0.0f;
  }
  const float savedZ = body_.pos.z;
  stepMove(world, 2, vel_.z * dt, swimming_, movingInto, wasGround);
  if (edgeGuard && !groundBelow(world)) {
    body_.pos.z = savedZ;
    vel_.z = 0.0f;
  }
  const bool hitVertical = moveAxis(world, 1, vel_.y * dt);

  if (hitVertical) {
    if (vel_.y < 0) {
      // Landing: the drop is measured before falling_ is cleared, because that is
      // the only record of where the fall started.
      const float dropped = falling_ ? fallStartY_ - body_.pos.y : 0.0f;
      onGround_ = true;
      falling_ = false;
      // The thud scales with the drop, and starts well below the height that hurts.
      if (dropped > survivalConst::kLandThud) {
        audio::sfx::land(std::min(1.0f, (dropped - survivalConst::kLandThud) / 10.0f));
      }
      if (options.fallDamageEnabled && dropped > survivalConst::kFallSafe) {
        float dmg = std::floor(dropped - survivalConst::kFallSafe);
        dmg = std::max(0.0f, dmg - options.defense * 0.5f);
        if (dmg > 0.0f) damage(dmg, options);
      }
    }
    // Landing or bonking the head both stop vertical motion.
    vel_.y = 0.0f;
  }

  survival(dt, world, options);

  // View bob, only while actually walking on the ground.
  const float hspeed = std::sqrt(vel_.x * vel_.x + vel_.z * vel_.z);
  const bool bobbing = !flying_ && !swimming_ && onGround_ && hspeed > 0.5f;
  const float target = bobbing ? std::min(1.4f, hspeed / kWalk) : 0.0f;
  bobMagnitude_ += (target - bobMagnitude_) * std::min(1.0f, dt * 8.0f);
  if (bobbing) bobPhase_ += hspeed * dt * 1.65f;

  // Work off whatever the last auto-step owes the camera.
  if (stepSmooth_ > 0.0f) {
    const float ease = stepSmooth_ * std::min(1.0f, dt * kStepSmoothRate);
    stepSmooth_ -= std::max(ease, dt * kStepSmoothFloor);
    if (stepSmooth_ < 0.0f) stepSmooth_ = 0.0f;
  }
}

// js/game/player.js:198-235. Kept in one function, in the same order, because the
// three systems are coupled: drowning and starving both feed damage(), and damage()
// is what stalls regeneration.
void Player::survival(float dt, const world::World& world, const PlayerOptions& options) {
  using namespace survivalConst;

  if (headInWater(world)) {
    breath_ -= dt;
    if (breath_ <= 0.0f) {
      breath_ = 0.0f;
      drownTimer_ += dt;
      if (drownTimer_ >= kDrownInterval) {
        drownTimer_ = 0.0f;
        damage(kDrownDamage, options, /*ignoreArmor=*/true);
      }
    }
  } else {
    breath_ = std::min(kMaxBreath, breath_ + dt * kBreathRefill);
    drownTimer_ = 0.0f;
  }

  if (hungerOn_) {
    float rate = kHungerBase;
    if (sprinting_) rate += kHungerSprint;
    if (swimming_) rate += kHungerSwim;
    exhaustion_ += rate * dt;
    while (exhaustion_ >= kExhaustionPerPoint) {
      exhaustion_ -= kExhaustionPerPoint;
      if (saturation_ > 0.0f) saturation_ = std::max(0.0f, saturation_ - 1.0f);
      else hunger_ = std::max(0.0f, hunger_ - 1.0f);
    }
    if (hunger_ <= 0.0f) {
      starveTimer_ += dt;
      if (starveTimer_ >= kStarveInterval) {
        starveTimer_ = 0.0f;
        // Starvation ignores armour, so it cannot chew through your gear.
        damage(1.0f, options, /*ignoreArmor=*/true);
      }
    } else {
      starveTimer_ = 0.0f;
    }
  }

  if (regenDelay_ > 0.0f) regenDelay_ -= dt;
  const bool wellFed = !hungerOn_ || hunger_ >= kWellFed;
  if (regenDelay_ <= 0.0f && wellFed && health_ > 0.0f && health_ < kMaxHealth) {
    regenTimer_ += dt;
    if (regenTimer_ >= kRegenInterval) {
      regenTimer_ = 0.0f;
      heal(1.0f);
      if (hungerOn_) exhaustion_ += kRegenExhaustion;
    }
  } else {
    regenTimer_ = 0.0f;
  }
}

void Player::damage(float amount, const PlayerOptions& options, bool ignoreArmor) {
  // The single door every kind of harm comes through — falling, drowning, starving,
  // a zombie, and both network paths. Closing it here closes all of them, which is
  // the whole reason this method exists rather than each caller subtracting health.
  if (options.invulnerable) return;
  if (!ignoreArmor && options.defense > 0.0f) {
    amount *= 1.0f - std::min(0.7f, options.defense * 0.04f);
  }
  if (amount <= 0.0f) return;
  const bool wasAlive = health_ > 0.0f;
  health_ = std::max(0.0f, health_ - amount);
  regenDelay_ = survivalConst::kRegenDelay;
  audio::sfx::hurt();
  if (!ignoreArmor && onArmorDamage) onArmorDamage(1);
  if (onHurt) onHurt();
  if (wasAlive && health_ <= 0.0f && onDeath) onDeath();
}

void Player::heal(float amount) {
  health_ = std::min(survivalConst::kMaxHealth, health_ + amount);
}

void Player::addFood(float food, float saturation) {
  hunger_ = std::min(survivalConst::kMaxHunger, hunger_ + food);
  saturation_ = std::min(hunger_, saturation_ + saturation);
}

bool Player::eat(const FoodEffect& food) {
  if (!hungerOn_) {
    // Hunger disabled: food just heals a bit, and a full bar refuses so the stack is
    // not wasted.
    if (health_ >= survivalConst::kMaxHealth) return false;
    const float amount =
        food.risky ? (randomUnit() < 0.5 ? 1.0f : 0.0f) : std::ceil(food.food / 2.0f);
    heal(amount);
    return true;
  }
  if (food.risky) {
    // The gamble is symmetric around the tooltip's number.
    if (randomUnit() < 0.5) {
      addFood(food.food, 0.0f);
    } else {
      hunger_ = std::max(0.0f, hunger_ - food.food);
      saturation_ = std::min(saturation_, hunger_);
    }
    return true;
  }
  if (hunger_ >= survivalConst::kMaxHunger) return false;
  addFood(food.food, food.food * 0.6f);
  return true;
}

void Player::teleport(const Vec3& p) {
  body_.pos = p;
  vel_ = {0, 0, 0};
  falling_ = false;
  fallStartY_ = p.y;
  // A warp is not a step, and easing the camera into one would drag the view up
  // out of the ground you arrived on. The crouch goes with it: whatever the head
  // was doing before the jump has nothing to do with where it lands.
  stepSmooth_ = 0.0f;
  sneaking_ = false;
  sneakSmooth_ = 0.0f;
}

void Player::reviveFull() {
  health_ = survivalConst::kMaxHealth;
  hunger_ = survivalConst::kMaxHunger;
  saturation_ = 5.0f;
  exhaustion_ = 0.0f;
  breath_ = survivalConst::kMaxBreath;
  starveTimer_ = drownTimer_ = regenTimer_ = regenDelay_ = 0.0f;
}

void Player::setHealth(float v) {
  health_ = std::min(survivalConst::kMaxHealth, std::max(0.0f, v));
}

PlayerState Player::state() const {
  PlayerState s;
  s.pos = body_.pos;
  s.yaw = yaw_;
  s.pitch = pitch_;
  s.health = health_;
  s.hunger = hunger_;
  s.saturation = saturation_;
  s.flying = flying_;
  return s;
}

void Player::loadState(const PlayerState& s) {
  // A NaN compares false against every bound, so each clamp below is written as
  // "keep only a value that is provably in range" rather than "reject what is
  // provably out of it" — the difference matters, because one NaN reaching the
  // physics spreads through the camera, the raycast and the chunk streamer within a
  // frame and the world simply stops drawing.
  const auto sane = [](float v, float limit, float fallback) {
    return (v >= -limit && v <= limit) ? v : fallback;
  };
  body_.pos = {sane(s.pos.x, 3.0e7f, 0.0f), sane(s.pos.y, 1.0e4f, 80.0f),
               sane(s.pos.z, 3.0e7f, 0.0f)};
  yaw_ = sane(s.yaw, 1.0e4f, 0.0f);
  pitch_ = sane(s.pitch, kPi, 0.0f);
  health_ = (s.health > 0.0f && s.health <= survivalConst::kMaxHealth)
                ? s.health
                : survivalConst::kMaxHealth;
  hunger_ = (s.hunger >= 0.0f && s.hunger <= survivalConst::kMaxHunger)
                ? s.hunger
                : survivalConst::kMaxHunger;
  saturation_ = (s.saturation >= 0.0f && s.saturation <= survivalConst::kMaxHunger)
                    ? s.saturation
                    : 5.0f;
  flying_ = s.flying;
  // Not saved, and all of them settle within seconds of loading.
  breath_ = survivalConst::kMaxBreath;
  exhaustion_ = 0.0f;
  starveTimer_ = drownTimer_ = regenTimer_ = regenDelay_ = 0.0f;
  vel_ = {0, 0, 0};
  falling_ = false;
  fallStartY_ = body_.pos.y;
  stepSmooth_ = 0.0f;
  sneaking_ = false;
  sneakSmooth_ = 0.0f;
}

Vec3 Player::viewOffset() const {
  // Negative: the body has already risen, so the eye trails behind it and catches
  // up. Applied whether or not the walk cycle is running, because an auto-step is
  // just as abrupt when you edge up a stair from a standstill.
  const float step = -stepSmooth_;
  if (bobMagnitude_ <= 0.001f) return {0, step, 0};
  const float vert = std::sin(bobPhase_ * 2.0f) * 0.05f * bobMagnitude_;  // a dip per footfall
  const float horiz = std::cos(bobPhase_) * 0.045f * bobMagnitude_;       // sway side to side
  // Camera right, from yaw alone: the bob should not tilt with pitch.
  const float rx = std::cos(yaw_);
  const float rz = -std::sin(yaw_);
  return {rx * horiz, vert + step, rz * horiz};
}

}  // namespace hr::game

