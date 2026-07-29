// The per-entity state-machine record: which state a mob is in, how long it has
// been there, and a blackboard of named numbers.
//
// This is its own header, rather than part of fsm.h, because the record now lives
// on the entity — `EntityData::fsm` — and entity.h must not have to pull in the
// whole state-machine machinery to declare a field.
//
// M8 kept these records inside the StateMachine, keyed by entity id, and wrote down
// that M9's save format was where they had to move: the web build stored them in
// `e.data.fsm` precisely so they flowed through save/load untouched and a mob
// resumed in the state it was saved in. An entity carries an empty vector until
// something puts it on a machine, which costs the 24 bytes of the vector header and
// nothing else, and moving them here also closes a leak the id-keyed map had — a
// record outlived the entity that owned it and was only reclaimed by an explicit
// forget().
//
// The blackboard is a small sorted-by-nothing vector rather than a hash map. It
// holds a handful of named values at most, so a linear scan beats hashing a string,
// and a vector is trivially serialisable in a way an unordered_map's iteration
// order is not.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace hr::game {

class Blackboard {
 public:
  float get(const std::string& name, float fallback = 0.0f) const {
    for (const auto& [k, v] : values_) {
      if (k == name) return v;
    }
    return fallback;
  }
  void set(const std::string& name, float value) {
    for (auto& [k, v] : values_) {
      if (k == name) {
        v = value;
        return;
      }
    }
    values_.emplace_back(name, value);
  }
  bool has(const std::string& name) const {
    for (const auto& [k, v] : values_) {
      (void)v;
      if (k == name) return true;
    }
    return false;
  }
  void clear() { values_.clear(); }

  const std::vector<std::pair<std::string, float>>& values() const { return values_; }
  std::vector<std::pair<std::string, float>>& values() { return values_; }

  // How long the current state has been running, refreshed every tick.
  float timeIn = 0.0f;

 private:
  std::vector<std::pair<std::string, float>> values_;
};

// One machine slot's record. `slot` is "root" for a top-level machine and
// "root/<parent state>" for a nested one, which is how a sub-machine gets its own
// record without a second map.
struct FsmRecord {
  std::string slot;
  std::string state;
  float timeIn = 0.0f;
  Blackboard bb;
};

}  // namespace hr::game
