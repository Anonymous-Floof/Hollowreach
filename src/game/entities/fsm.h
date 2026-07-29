// Hierarchical state machines for mob brains, ported from
// js/game/entities/ai/fsm.js.
//
// AI backend: built for future advanced mobs. The shipped sheep, pig, cow and
// zombie keep their ad-hoc logic until they are migrated.
//
// One StateMachine is defined per mob TYPE — shared and stateless. Each entity
// carries only a record: which state it is in, how long it has been there, and a
// blackboard of named numbers (target ids, home positions, counters).
//
// A state is enter / update / exit, plus an optional nested machine ticked while
// the state is active and reset each time it is entered. `update` returns the name
// of the state to move to, or nothing to stay.
//
// The records live on the entity, in `EntityData::fsm` — the same place the web
// build kept them, and for the same reason: they have to flow through save and load
// untouched so a mob resumes in the state it was saved in. M8 parked them in the
// machine keyed by entity id and noted that M9's save format was where they had to
// move; this is that move. See game/entities/blackboard.h.

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/entities/blackboard.h"

namespace hr::game {

struct Entity;
struct EntityContext;

class StateMachine;

// A per-entity cooldown for use inside a state:
//   if (cooldown(bb, "attack", 1.0f, dt)) strike();
// True when the named timer is ready, and it immediately re-arms.
bool cooldown(Blackboard& bb, const std::string& name, float period, float dt);

struct State {
  std::function<void(Entity&, EntityContext&, Blackboard&)> enter;
  // Returns the name of the state to move to, or an empty string to stay put.
  std::function<std::string(Entity&, float dt, EntityContext&, Blackboard&)> update;
  std::function<void(Entity&, EntityContext&, Blackboard&)> exit;
  StateMachine* sub = nullptr;
};

class StateMachine {
 public:
  StateMachine(std::string initial, std::unordered_map<std::string, State> states);

  // Current state name for an entity, useful for a debug overlay or a drop table.
  // An entity that has never been ticked reports the initial state rather than
  // creating a record, which is why this can stay const.
  const std::string& stateOf(const Entity& e, const std::string& slot = "root") const;

  // Force a transition, running the exit and enter hooks.
  void transition(Entity& e, EntityContext& ctx, const std::string& to,
                  const std::string& slot = "root");

  // Tick one entity. Returns the state name after the tick.
  const std::string& update(Entity& e, float dt, EntityContext& ctx,
                            const std::string& slot = "root");

  // Drops every record for an entity. Kept from the id-keyed version so a brain can
  // reset a mob deliberately; it is no longer needed for hygiene, because the
  // records die with the entity that carries them.
  void forget(Entity& e);

 private:
  FsmRecord& record(Entity& e, const std::string& slot);

  std::string initial_;
  std::unordered_map<std::string, State> states_;
};

}  // namespace hr::game
