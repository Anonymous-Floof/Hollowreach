// The first-person hand: the animation clocks that pose the held item.
//
// Ported from js/render/viewmodel.js. The pose itself lives in the ItemModel's
// HoldStyle (render/itemmodel.h) and the animation is applied OUTSIDE it, about
// the view's axes rather than the item's — so a swing means "the arm comes down"
// no matter how the hand is gripping the thing. The JS added the two together,
// which works only while every hold style points roughly the same way; the moment
// a tool was yawed a quarter turn to lead with its edge, the same "swing down"
// term started spinning it in the picture plane instead.

#pragma once

#include <string>

#include "core/mat4.h"
#include "render/itemmodel.h"

namespace hr::render {

class Viewmodel {
 public:
  // Called every frame with the selected hotbar key: a change lowers the old item
  // and raises the new one.
  void setItem(const std::string& key);
  const std::string& item() const { return key_; }

  // Starts a swing, or queues one so a swing triggered mid-arc still happens.
  // Held mining calls this every frame, which chains into a continuous swing.
  void swing();

  // Drops a swing in progress and anything queued behind it. Called when the
  // world stops running: the swing clock only advances while playing, so an arc
  // interrupted by a screen would otherwise be frozen at whatever frame it had
  // reached and finish itself the moment the screen closed — which reads as the
  // tool swinging on its own at a moment nothing was clicked.
  void cancelSwing();

  // `bobPhase` and `bobMagnitude` come from the player's walk cycle, so the hand
  // and the head agree about which foot is down.
  void update(float dt, float bobPhase, float bobMagnitude);

  // The view-space model matrix for the held item.
  Mat4 modelMatrix(const HoldStyle& style, float fovRadians, float aspect) const;

  // 0 = fully lowered out of frame, 1 = in place.
  float equipProgress() const { return equipT_; }

 private:
  std::string key_;
  float swingT_ = 1.0f;  // 0..1 through a swing; >= 1 means idle
  float equipT_ = 1.0f;
  float clock_ = 0.0f;   // free-running seconds, for the idle drift
  float bobPhase_ = 0.0f;
  float bobMagnitude_ = 0.0f;
  bool queued_ = false;  // a swing asked for while one was already running
  bool hasItem_ = false;
};

}  // namespace hr::render
