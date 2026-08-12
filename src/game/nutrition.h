// Food groups, and the diet built out of them.
//
// This header exists to be included by BOTH ends of eating without joining them:
// `items.h` needs the group to author a food, and `player.h` needs it to record
// having eaten one. Putting the enum in either would drag the whole item registry
// into the player's header, which the FoodEffect comment at player.h:107 says in as
// many words is not allowed to happen. So it lives here, depends on nothing, and
// both sides include it.
//
// WHY A DIET AT ALL. Before this, one number decided everything a food was worth,
// so the strictly correct play was to find the single highest-`food` item in the
// game and eat only that forever. Cooking cannot compete with that no matter how
// good a stew is, because a stew is still just a number in the same units. The diet
// is what makes *variety* a thing the game can reward — you cannot eat your way to
// full nutrition out of one crop, however much of it you grow.
//
// The reward is deliberately extra hearts rather than a hidden buff: a player must
// be able to see that cooking did something, and a number that only exists in the
// simulation teaches nobody anything.

#pragma once

#include <cstdint>

namespace hr::game {

// The five groups a meal can feed, plus None for everything that is not food.
//
// Five is a judgement call, not a law. Fewer and a "varied diet" is satisfied by
// two crops; more and the bars become bookkeeping. These five also map cleanly onto
// what the world can actually supply: four grains, seven vegetables, four fruits, a
// legume and the meat, and the milk bucket that has been sitting in the game with no
// use since cows were added.
enum class NutritionGroup : std::uint8_t {
  None = 0,
  Grain,
  Vegetable,
  Fruit,
  Protein,
  Dairy,
};

// How many groups a diet actually tracks. `None` is not one of them, so the levels
// array is indexed by `group - 1` and this is one less than the enum's size.
inline constexpr int kNutritionGroups = 5;

// Enum -> levels index, or -1 for None. Every caller has to handle the -1, which is
// the point: an item with no group must not silently feed group zero.
inline int nutritionIndex(NutritionGroup g) {
  return g == NutritionGroup::None ? -1 : static_cast<int>(g) - 1;
}

// For settings rows, tooltips and the inventory's diet bars.
inline const char* nutritionName(NutritionGroup g) {
  switch (g) {
    case NutritionGroup::Grain: return "Grain";
    case NutritionGroup::Vegetable: return "Vegetable";
    case NutritionGroup::Fruit: return "Fruit";
    case NutritionGroup::Protein: return "Protein";
    case NutritionGroup::Dairy: return "Dairy";
    case NutritionGroup::None: break;
  }
  return "";
}

// --- the diet ---------------------------------------------------------------

namespace dietConst {

// A group sits in [0, 1]. Eating fills it, time empties it.
inline constexpr float kMax = 1.0f;

// One full level drains over about an in-game day. The day is 20 minutes of real
// time, so a group left completely unfed empties in 1200 seconds — long enough that
// a diet is a standing state you maintain rather than a meter you chase, short
// enough that a week-old meal is not still counting.
inline constexpr float kDecayPerSecond = kMax / 1200.0f;

// A group counts toward the bonus above this. Half is chosen so that one good meal
// puts a group over the line and lets it coast for a while, rather than demanding
// the bar be topped up constantly.
inline constexpr float kCountsAbove = 0.5f;

// Extra health per group over the line, and the ceiling. Five groups at +2 would be
// +10, which is a doubling and far too much; three hearts is a real reward that
// still leaves armour mattering more.
inline constexpr float kBonusPerGroup = 2.0f;
inline constexpr float kBonusMax = 6.0f;

}  // namespace dietConst

// The five levels, saved with the player and displayed on the inventory screen.
//
// A plain struct rather than a class: it is five floats with no invariant beyond
// the clamp, and every consumer wants to read them directly to draw a bar.
struct Diet {
  float level[kNutritionGroups] = {0, 0, 0, 0, 0};

  // How many groups are over the line right now.
  int groupsFed() const {
    int n = 0;
    for (float v : level) {
      if (v > dietConst::kCountsAbove) ++n;
    }
    return n;
  }

  // The extra health those groups are worth.
  float healthBonus() const {
    const float bonus = static_cast<float>(groupsFed()) * dietConst::kBonusPerGroup;
    return bonus < dietConst::kBonusMax ? bonus : dietConst::kBonusMax;
  }
};

}  // namespace hr::game
