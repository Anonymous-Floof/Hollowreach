#include "game/entities/fsm.h"

#include <utility>

#include "game/entities/entity.h"

namespace hr::game {
namespace {

// Erases one slot's record, used when a parent state is left and its nested machine
// has to restart from scratch.
void eraseSlot(Entity& e, const std::string& slot) {
  auto& records = e.data.fsm;
  for (auto it = records.begin(); it != records.end(); ++it) {
    if (it->slot == slot) {
      records.erase(it);
      return;
    }
  }
}

}  // namespace

bool cooldown(Blackboard& bb, const std::string& name, float period, float dt) {
  const std::string key = "_cd_" + name;
  const float left = bb.get(key, 0.0f) - dt;
  if (left <= 0.0f) {
    bb.set(key, period);
    return true;
  }
  bb.set(key, left);
  return false;
}

StateMachine::StateMachine(std::string initial, std::unordered_map<std::string, State> states)
    : initial_(std::move(initial)), states_(std::move(states)) {}

FsmRecord& StateMachine::record(Entity& e, const std::string& slot) {
  for (FsmRecord& r : e.data.fsm) {
    if (r.slot == slot) return r;
  }
  FsmRecord fresh;
  fresh.slot = slot;
  fresh.state = initial_;
  e.data.fsm.push_back(std::move(fresh));
  return e.data.fsm.back();
}

const std::string& StateMachine::stateOf(const Entity& e, const std::string& slot) const {
  for (const FsmRecord& r : e.data.fsm) {
    if (r.slot == slot) return r.state;
  }
  return initial_;
}

void StateMachine::transition(Entity& e, EntityContext& ctx, const std::string& to,
                              const std::string& slot) {
  auto next = states_.find(to);
  if (next == states_.end()) return;  // unknown target: stay put rather than crash

  FsmRecord& rec = record(e, slot);
  auto from = states_.find(rec.state);
  if (from != states_.end()) {
    if (from->second.exit) from->second.exit(e, ctx, rec.bb);
    // A nested machine restarts fresh each time its parent state is re-entered.
    // Erasing invalidates `rec`, so the sub-slot name is built first and the
    // record re-found afterwards.
    if (from->second.sub) {
      const std::string subSlot = slot + "/" + rec.state;
      eraseSlot(e, subSlot);
    }
  }
  FsmRecord& target = record(e, slot);
  target.state = to;
  target.timeIn = 0.0f;
  if (next->second.enter) next->second.enter(e, ctx, target.bb);
}

const std::string& StateMachine::update(Entity& e, float dt, EntityContext& ctx,
                                        const std::string& slot) {
  {
    FsmRecord& rec = record(e, slot);
    // A state saved under an old name — or a renamed one — falls back to the
    // initial. This is the load path's safety net: a save written by a build whose
    // brain had different state names still produces a mob that behaves.
    if (states_.find(rec.state) == states_.end()) {
      rec.state = initial_;
      rec.timeIn = 0.0f;
    }
    rec.timeIn += dt;
    rec.bb.timeIn = rec.timeIn;
  }

  // The state's own name is copied out because `update` may spawn entities, and a
  // spawn reallocates the entity vector — which would leave a reference into the
  // old storage dangling. Everything below re-finds the record for the same reason.
  const std::string current = record(e, slot).state;
  State& st = states_[current];
  const std::string nextName =
      st.update ? st.update(e, dt, ctx, record(e, slot).bb) : std::string();

  if (!nextName.empty() && nextName != current) {
    transition(e, ctx, nextName, slot);
  } else if (st.sub) {
    st.sub->update(e, dt, ctx, slot + "/" + current);
  }
  return record(e, slot).state;
}

void StateMachine::forget(Entity& e) { e.data.fsm.clear(); }

}  // namespace hr::game
