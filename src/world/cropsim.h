// Crop growth.
//
// THE SCALE PROBLEM IS THE SAME ONE BlockUpdateSim OPENS WITH, and the answer here
// is different because the question is. Support checks are event-driven because
// nothing needs looking at until something near it changes. Growth has no event: a
// crop ripens because time passed, and time passing is not a thing the world can
// schedule off an edit.
//
// Minecraft answers this with random ticks — sample random cells in every loaded
// chunk and see what happens to be there. That is affordable there because a dozen
// behaviours ride on the same sampling. Here exactly one would, and sampling a
// 16x192x16 chunk to find the four cells a player has planted is almost all waste.
//
// So this keeps a SET of the planted cells instead, and it costs no save format to
// do it:
//
//   * A crop is always a player edit. Wild stands come out of the generator, and
//     the generator's output is re-derived rather than stored — so every crop in
//     `edits_` is one somebody sowed, and every crop somebody sowed is in `edits_`.
//   * The set is therefore REBUILT from the edit map on load (World::indexCrops),
//     not written to disk. A derived index cannot go stale against the thing it is
//     derived from, which is the same argument the roadmap makes for re-deriving
//     dungeon chest positions instead of recording them.
//   * Growth stage lives in cell metadata, which the edit map already carries. So a
//     crop keeps growing across a save, a chunk unload and a regeneration without
//     one byte of new format.
//
// There is no timer anywhere. Each sweep, each crop rolls against a chance; that is
// Minecraft's model too, and it means nothing has to be stored per crop, nothing
// drifts when the game is paused, and a crop cannot be "half grown" in a way a save
// would have to describe.

#pragma once

#include <cstddef>
#include <cstdint>

namespace hr::world {

class World;

class CropSim {
 public:
  explicit CropSim(World& world) : world_(world) {}

  void tick(float dt);

  // Bounded work per sweep, carried across sweeps the way BlockUpdateSim carries
  // its queue: a player with a thousand-cell farm should see it grow slower per
  // sweep, never see a frame stall.
  std::size_t cursor() const { return cursor_; }
  void reset() { cursor_ = 0; acc_ = 0.0f; }

  // Half a hertz. Growth is measured in minutes, so a faster sweep would only burn
  // rolls; a slower one would make a small farm look frozen.
  static constexpr float kTick = 2.0f;
  static constexpr std::size_t kMaxPerTick = 2048;

  // Chance one crop advances one stage in one sweep, before modifiers.
  //
  // At 0.0125 a stage takes about 160s of sweeps on plain dry soil, so a full crop
  // is roughly ten minutes — and about two and a half on soil that is both damp and
  // fertilised, since those multiply. This was four times faster to begin with,
  // which made a field ripen while you were still building the fence around it.
  static constexpr float kAdvanceChance = 0.0125f;

  // Damp soil and fertiliser each double the rate, and they stack. Kept as named
  // constants because the cost of the fertiliser recipe is balanced against this
  // number, not against a literal buried in the sweep.
  static constexpr float kMoistBoost = 2.0f;
  static constexpr float kFertiliserBoost = 2.0f;

 private:
  World& world_;
  // Where the last sweep stopped, so a big farm is covered across several sweeps
  // rather than the same prefix every time.
  std::size_t cursor_ = 0;
  float acc_ = 0.0f;
  std::uint32_t rng_ = 0x9e3779b9u;
};

}  // namespace hr::world
