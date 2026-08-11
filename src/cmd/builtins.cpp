// Every command the game ships with.
//
// One table, in help order, grouped by what the commands are FOR rather than
// alphabetically — somebody reading /help is looking for a capability, not a
// spelling. The order here is the order the popup and the help page both use.
//
// Three conventions worth knowing before reading any of the bodies:
//
//  * A command never touches another player directly. It resolves a name to a
//    Participant and hands that to a hook. On a host the target's body may not be
//    simulated at all, so there is nothing local to reach for; see cmd/command.h.
//  * A command that refuses says why, in the same sentence it refuses. "You are
//    trusted, that needs operator" is a useful answer; "no" is not.
//  * `Command::level` is a floor. /set is Anyone because your own field of view is
//    your own business, and it demands Operator once the setting turns out to be
//    one of the world's rules. The floor is what the popup filters on, so a
//    command with an argument-dependent bar is offered and then argues.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "cmd/command.h"
#include "game/items.h"
#include "render/sky.h"
#include "ui/settings.h"
#include "world/world.h"

namespace hr::cmd {
namespace {

std::string fmt(const char* pattern, ...) {
  char buffer[512];
  va_list args;
  va_start(args, pattern);
  std::vsnprintf(buffer, sizeof(buffer), pattern, args);
  va_end(args);
  return buffer;
}

// The people in the session, or an empty list when the session cannot say. Every
// command that names somebody starts here, so the null check happens once.
std::vector<Participant> everyone(const Context& ctx) {
  if (ctx.hooks && ctx.hooks->participants) return ctx.hooks->participants();
  return {};
}

const Participant* self(const Context& ctx, const std::vector<Participant>& people) {
  for (const Participant& p : people) {
    if (p.self) return &p;
  }
  (void)ctx;
  return nullptr;
}

// A hook that was never wired means this session cannot do that thing — a guest's
// build with no world open, or a future headless server that has not grown the
// feature yet. Reported as a refusal rather than crashing on an empty function.
Result noHook(const char* what) {
  return Result::fail(std::string("this session cannot ") + what);
}

// ---- talking ----------------------------------------------------------------

Result cmdHelp(const Context& ctx, const std::vector<std::string>& args) {
  const std::vector<Command>& all = Registry::get().all();

  if (!args.empty()) {
    // A command name, if it names one. Otherwise fall through to a page number, so
    // `/help 2` works and `/help gibberish` says something useful.
    if (const Command* c = Registry::get().find(args[0])) {
      ctx.reply(fmt("/%s \xC2\xB7 %s", c->name, c->summary));
      ctx.reply(fmt("  usage: %s", c->usage));
      ctx.reply(fmt("  needs: %s", levelName(c->level)));
      if (!c->aliases.empty()) {
        std::string line = "  also: ";
        for (std::size_t i = 0; i < c->aliases.size(); ++i) {
          line += (i ? ", /" : "/");
          line += c->aliases[i];
        }
        ctx.reply(line);
      }
      return Result::done();
    }
  }

  std::vector<const Command*> visible;
  for (const Command& c : all) {
    if (ctx.level >= c.level) visible.push_back(&c);
  }

  constexpr int kPerPage = 8;
  const int pages = std::max(1, (static_cast<int>(visible.size()) + kPerPage - 1) / kPerPage);
  int page = 1;
  if (!args.empty() && !parseInt(args[0], 1, pages, page)) {
    return Result::fail("there is no command or page called '" + args[0] + "'");
  }

  ctx.reply(fmt("Commands \xC2\xB7 page %d/%d \xC2\xB7 you are %s", page, pages,
                levelName(ctx.level)));
  const std::size_t start = static_cast<std::size_t>(page - 1) * kPerPage;
  for (std::size_t i = start; i < visible.size() && i < start + kPerPage; ++i) {
    ctx.reply(fmt("  /%s \xC2\xB7 %s", visible[i]->name, visible[i]->summary));
  }
  if (pages > 1) ctx.reply(fmt("  /help %d for more", page < pages ? page + 1 : 1));
  return Result::done();
}

Result cmdList(const Context& ctx, const std::vector<std::string>&) {
  const std::vector<Participant> people = everyone(ctx);
  if (people.empty()) return Result::fail("nobody is here, not even you");
  ctx.reply(fmt("%d here:", static_cast<int>(people.size())));
  for (const Participant& p : people) {
    std::string tags;
    if (p.host) tags += " (host)";
    if (p.level != Level::Anyone) tags += std::string(" \xC2\xB7 ") + levelName(p.level);
    ctx.reply("  " + p.name + tags);
  }
  return Result::done();
}

Result cmdMe(const Context& ctx, const std::vector<std::string>& args) {
  ctx.announce("* " + ctx.name + " " + args[0]);
  return Result::done();
}

Result cmdSay(const Context& ctx, const std::vector<std::string>& args) {
  ctx.announce("[" + ctx.name + "] " + args[0]);
  return Result::done();
}

Result cmdMsg(const Context& ctx, const std::vector<std::string>& args) {
  const std::vector<Participant> people = everyone(ctx);
  std::string error;
  const Participant* target = findParticipant(people, args[0], &error);
  if (!target) return Result::fail(error);
  if (target->self) return Result::fail("you are already talking to yourself");
  if (!ctx.hooks->whisper) return noHook("send private messages");
  if (!ctx.hooks->whisper(*target, args[1], error)) return Result::fail(error);
  // Echoed back so the sender has a record of what they sent and to whom, which is
  // the whole reason a whisper feels different from shouting into an empty room.
  ctx.reply("you \xE2\x86\x92 " + target->name + ": " + args[1]);
  return Result::done();
}

// ---- the world ---------------------------------------------------------------

Result cmdSeed(const Context& ctx, const std::vector<std::string>&) {
  return Result::done(fmt("seed: %u", ctx.world->seed()));
}

Result cmdTime(const Context& ctx, const std::vector<std::string>& args) {
  if (!ctx.sky) return noHook("change the time");
  if (args.empty()) {
    const float hours = ctx.sky->time * 24.0f;
    return Result::done(fmt("it is %02d:%02d", static_cast<int>(hours),
                            static_cast<int>((hours - std::floor(hours)) * 60.0f)));
  }

  // The clock is 0..1 across a day, with 0 at midnight. The named hours are the
  // same ones the bed's dial offers, so "day" means the same thing in both places.
  struct Named {
    const char* name;
    float time;
  };
  static constexpr Named kNamed[] = {
      {"midnight", 0.00f}, {"dawn", 0.22f}, {"day", 0.30f},
      {"noon", 0.50f},     {"dusk", 0.76f}, {"night", 0.85f},
  };
  for (const Named& n : kNamed) {
    if (args[0] == n.name) {
      ctx.sky->time = n.time;
      return Result::done(fmt("time set to %s", n.name));
    }
  }

  // hh:mm, which is what anyone actually wants when they are being precise.
  const std::size_t colon = args[0].find(':');
  if (colon != std::string::npos) {
    int hh = 0, mm = 0;
    if (!parseInt(args[0].substr(0, colon), 0, 23, hh) ||
        !parseInt(args[0].substr(colon + 1), 0, 59, mm)) {
      return Result::fail("give an hour from 00:00 to 23:59");
    }
    ctx.sky->time = (static_cast<float>(hh) + static_cast<float>(mm) / 60.0f) / 24.0f;
    return Result::done(fmt("time set to %02d:%02d", hh, mm));
  }

  float raw = 0;
  if (!parseCoord(args[0], 0.0f, raw) || raw < 0.0f || raw > 1.0f) {
    return Result::fail("give a name (day, night, noon\xE2\x80\xA6), an hh:mm, or 0 to 1");
  }
  ctx.sky->time = raw;
  return Result::done(fmt("time set to %.3f", raw));
}

Result cmdLocate(const Context& ctx, const std::vector<std::string>&) {
  if (!ctx.hooks->locateDungeon) return noHook("search for dungeons");
  Vec3 at;
  std::string error;
  if (!ctx.hooks->locateDungeon(at, error)) return Result::fail(error);
  return Result::done(fmt("nearest dungeon: %d %d %d", static_cast<int>(at.x),
                          static_cast<int>(at.y), static_cast<int>(at.z)));
}

Result cmdSummon(const Context& ctx, const std::vector<std::string>& args) {
  if (!ctx.hooks->summon) return noHook("summon anything");
  int count = 1;
  if (args.size() > 1 && !parseInt(args[1], 1, 32, count)) {
    return Result::fail("summon between 1 and 32 at a time");
  }
  std::string error;
  if (!ctx.hooks->summon(args[0], count, error)) return Result::fail(error);
  return Result::done(fmt("summoned %d \xC3\x97 %s", count, args[0].c_str()));
}

// ---- bodies ------------------------------------------------------------------

// Resolves the target of a command that defaults to the caller, and enforces the
// rule the three of them share: doing it to yourself is your business, doing it to
// somebody else needs Operator. One function so /kill, /heal and /clear cannot
// drift apart on the question of who may aim them at whom.
const Participant* resolveTarget(const Context& ctx, const std::vector<Participant>& people,
                                 const std::vector<std::string>& args, std::size_t at,
                                 Result& refusal) {
  const Participant* target = nullptr;
  if (args.size() > at) {
    std::string error;
    target = findParticipant(people, args[at], &error);
    if (!target) {
      refusal = Result::fail(error);
      return nullptr;
    }
  } else {
    target = self(ctx, people);
    if (!target) {
      refusal = Result::fail("you have no body here to aim that at");
      return nullptr;
    }
  }
  if (!target->self && ctx.level < Level::Operator) {
    refusal = Result::fail("moving somebody else needs operator");
    return nullptr;
  }
  return target;
}

Result cmdKill(const Context& ctx, const std::vector<std::string>& args) {
  const std::vector<Participant> people = everyone(ctx);
  Result refusal;
  const Participant* target = resolveTarget(ctx, people, args, 0, refusal);
  if (!target) return refusal;
  if (!ctx.hooks->setVitals) return noHook("do that");
  std::string error;
  if (!ctx.hooks->setVitals(*target, 0.0f, error)) return Result::fail(error);
  return Result::done(target->self ? "you died" : target->name + " died");
}

Result cmdHeal(const Context& ctx, const std::vector<std::string>& args) {
  const std::vector<Participant> people = everyone(ctx);
  Result refusal;
  const Participant* target = resolveTarget(ctx, people, args, 0, refusal);
  if (!target) return refusal;
  if (!ctx.hooks->setVitals) return noHook("do that");
  std::string error;
  if (!ctx.hooks->setVitals(*target, 20.0f, error)) return Result::fail(error);
  return Result::done(target->self ? "healed" : "healed " + target->name);
}

Result cmdClear(const Context& ctx, const std::vector<std::string>& args) {
  const std::vector<Participant> people = everyone(ctx);
  Result refusal;
  const Participant* target = resolveTarget(ctx, people, args, 0, refusal);
  if (!target) return refusal;
  if (!ctx.hooks->clearInventory) return noHook("do that");
  std::string error;
  if (!ctx.hooks->clearInventory(*target, error)) return Result::fail(error);
  return Result::done(target->self ? "inventory cleared" : "cleared " + target->name + "'s bag");
}

Result cmdGive(const Context& ctx, const std::vector<std::string>& args) {
  const std::vector<Participant> people = everyone(ctx);
  const game::ItemDef* item = game::getItem(args[0]);
  if (!item) return Result::fail("there is no item called '" + args[0] + "'");

  int count = 1;
  if (args.size() > 1 && !parseInt(args[1], 1, 999, count)) {
    return Result::fail("give between 1 and 999");
  }
  Result refusal;
  const Participant* target = resolveTarget(ctx, people, args, 2, refusal);
  if (!target) return refusal;
  if (!ctx.hooks->give) return noHook("hand out items");

  std::string error;
  if (!ctx.hooks->give(*target, item->key, count, error)) return Result::fail(error);
  return Result::done(fmt("gave %s %d \xC3\x97 %s", target->self ? "you" : target->name.c_str(),
                          count, item->name.c_str()));
}

Result cmdTeleport(const Context& ctx, const std::vector<std::string>& args) {
  const std::vector<Participant> people = everyone(ctx);
  const Participant* me = self(ctx, people);
  if (!ctx.hooks->teleport) return noHook("teleport anyone");

  // Where the caller is standing, which is what `~` means. Absent for a console,
  // where `~` has nothing to be relative to and is refused rather than read as 0 —
  // silently teleporting somebody to the world origin is a long walk home.
  const bool haveOrigin = me && me->hasPos;
  const Vec3 origin = haveOrigin ? me->pos : Vec3{0, 0, 0};

  // Three coordinates, moving the caller. Recognised by the first argument not
  // naming anybody, which is also why a player called "100" would be ambiguous —
  // and why names are letters (see net::cleanName).
  const bool coords = args.size() >= 3 && !findParticipant(people, args[0], nullptr);
  if (coords) {
    if (!me) return Result::fail("you have no body here to move");
    Vec3 to;
    if (!parseCoord(args[0], origin.x, to.x) || !parseCoord(args[1], origin.y, to.y) ||
        !parseCoord(args[2], origin.z, to.z)) {
      return Result::fail("give three numbers, or ~ for where you already are");
    }
    if (!haveOrigin && (args[0][0] == '~' || args[1][0] == '~' || args[2][0] == '~')) {
      return Result::fail("~ needs somewhere to be relative to");
    }
    std::string error;
    if (!ctx.hooks->teleport(*me, to, error)) return Result::fail(error);
    return Result::done(fmt("teleported to %d %d %d", static_cast<int>(to.x),
                            static_cast<int>(to.y), static_cast<int>(to.z)));
  }

  std::string error;
  const Participant* first = findParticipant(people, args[0], &error);
  if (!first) return Result::fail(error);

  // `/tp <player>` moves the caller. `/tp <a> <b>` moves a to b, which is somebody
  // else's business and needs operator.
  if (args.size() == 1) {
    if (!me) return Result::fail("you have no body here to move");
    if (!first->hasPos) return Result::fail(first->name + " is not anywhere yet");
    if (!ctx.hooks->teleport(*me, first->pos, error)) return Result::fail(error);
    return Result::done("teleported to " + first->name);
  }

  const Participant* second = findParticipant(people, args[1], &error);
  if (!second) return Result::fail(error);
  if (!first->self && ctx.level < Level::Operator) {
    return Result::fail("moving somebody else needs operator");
  }
  if (!second->hasPos) return Result::fail(second->name + " is not anywhere yet");
  if (!ctx.hooks->teleport(*first, second->pos, error)) return Result::fail(error);
  return Result::done("teleported " + first->name + " to " + second->name);
}

Result cmdSpawn(const Context& ctx, const std::vector<std::string>&) {
  const std::vector<Participant> people = everyone(ctx);
  const Participant* me = self(ctx, people);
  if (!me) return Result::fail("you have no body here to move");
  if (!ctx.hooks->worldSpawn || !ctx.hooks->teleport) return noHook("teleport anyone");
  Vec3 at;
  std::string error;
  if (!ctx.hooks->worldSpawn(at, error)) return Result::fail(error);
  if (!ctx.hooks->teleport(*me, at, error)) return Result::fail(error);
  return Result::done("teleported to spawn");
}

// ---- settings ----------------------------------------------------------------

Result cmdSet(const Context& ctx, const std::vector<std::string>& args) {
  const ui::SettingDef* def = ui::settings().find(args[0]);
  if (!def || def->hidden || def->type == ui::SettingType::Action) {
    return Result::fail("there is no setting called '" + args[0] + "'");
  }

  // Reading is free. Writing depends on whose setting it is: the installation's
  // are the player's own and need nothing, the world's are a rule everybody in it
  // plays by and need operator.
  const auto describe = [&]() {
    switch (def->type) {
      case ui::SettingType::Toggle:
        return fmt("%s is %s", def->key, ui::settings().flag(def->key) ? "on" : "off");
      case ui::SettingType::Slider:
        return fmt("%s is %g (%g to %g)", def->key, ui::settings().number(def->key), def->min,
                   def->max);
      default:
        return fmt("%s is '%s'", def->key, ui::settings().text(def->key).c_str());
    }
  };
  if (args.size() < 2) return Result::done(describe());

  if (def->scope == ui::SettingScope::World && ctx.level < Level::Operator) {
    return Result::fail(fmt("%s is one of the world's rules \xC2\xB7 that needs operator",
                            def->key));
  }
  if (!ctx.hooks->applySetting) return noHook("change settings");

  std::string error;
  if (!ctx.hooks->applySetting(def->key, args[1], error)) return Result::fail(error);
  return Result::done(describe());
}

Result cmdGamemode(const Context& ctx, const std::vector<std::string>& args) {
  bool creative = false;
  if (args[0] == "creative" || args[0] == "c" || args[0] == "1") {
    creative = true;
  } else if (args[0] == "survival" || args[0] == "s" || args[0] == "0") {
    creative = false;
  } else {
    return Result::fail("say creative or survival");
  }
  if (!ctx.hooks->applySetting) return noHook("change settings");
  std::string error;
  // Deliberately the same road the settings screen takes, so the rule that a world
  // created Survival can never become Creative is enforced in exactly one place —
  // the gate on the schema row — rather than being restated here and drifting.
  if (!ctx.hooks->applySetting("creativeMode", creative ? "true" : "false", error)) {
    return Result::fail(error);
  }
  return Result::done(creative ? "creative mode" : "survival mode");
}

// ---- the session -------------------------------------------------------------

// Whether the caller may act ON somebody — deop, kick, ban.
//
// STRICTLY BELOW, not "below or equal". Two operators able to kick each other is
// how a disagreement becomes a kicking match, and the point of having an owner is
// that there is somebody to settle it. Acting on yourself is always allowed:
// demoting or removing yourself takes nothing from anybody else.
//
// The mirror rule for GRANTING is at most your own level, in cmdOp — you may hand
// out what you hold, and you may only overrule somebody who holds less.
bool mayActOn(const Participant& target, Level caller) {
  return target.self || target.level < caller;
}

Result cmdOp(const Context& ctx, const std::vector<std::string>& args) {
  Level level = Level::Operator;
  if (args.size() > 1 && !levelFromName(args[1], level)) {
    return Result::fail("levels are anyone, trusted, operator, owner");
  }
  // Nobody may hand out what they do not hold. This is what makes /op safe to
  // leave at operator rather than reserving it to the owner: an operator can
  // vouch for a newcomer up to their own rank and no further, so delegating the
  // ability to delegate does not also hand over the world.
  if (level > ctx.level) {
    return Result::fail(fmt("you cannot grant %s \xC2\xB7 you are %s", levelName(level),
                            levelName(ctx.level)));
  }
  if (!ctx.hooks->setLevel) return noHook("change permissions");

  // By name, because whoever is typing this knows a name and not an id. The id is
  // filled in from the roster when the person named is actually here, which is what
  // makes the grant survive them renaming themselves later.
  const std::vector<Participant> people = everyone(ctx);
  const Participant* here = findParticipant(people, args[0], nullptr);
  std::string error;
  if (!ctx.hooks->setLevel(here ? here->playerId : std::string(), here ? here->name : args[0],
                           level, error)) {
    return Result::fail(error);
  }
  ctx.announce(fmt("%s is now %s", (here ? here->name : args[0]).c_str(), levelName(level)));
  return Result::done();
}

Result cmdDeop(const Context& ctx, const std::vector<std::string>& args) {
  if (!ctx.hooks->setLevel) return noHook("change permissions");
  const std::vector<Participant> people = everyone(ctx);
  const Participant* here = findParticipant(people, args[0], nullptr);
  if (here && !mayActOn(*here, ctx.level)) {
    return Result::fail(fmt("%s is %s, and you are %s \xC2\xB7 you can only demote somebody "
                            "below you",
                            here->name.c_str(), levelName(here->level), levelName(ctx.level)));
  }
  std::string error;
  if (!ctx.hooks->setLevel(here ? here->playerId : std::string(), here ? here->name : args[0],
                           Level::Anyone, error)) {
    return Result::fail(error);
  }
  ctx.announce(fmt("%s is now %s", (here ? here->name : args[0]).c_str(),
                   levelName(Level::Anyone)));
  return Result::done();
}

// The three list-printing commands are the same shape, so they share one body.
Result printList(const Context& ctx, const std::function<std::vector<std::string>()>& source,
                 const char* what, const char* empty) {
  if (!source) return noHook("answer that");
  const std::vector<std::string> lines = source();
  if (lines.empty()) return Result::done(empty);
  ctx.reply(fmt("%s (%d):", what, static_cast<int>(lines.size())));
  for (const std::string& line : lines) ctx.reply("  " + line);
  return Result::done();
}

Result cmdPerms(const Context& ctx, const std::vector<std::string>&) {
  return printList(ctx, ctx.hooks->permList, "Trusted", "nobody has been given a level");
}

Result cmdBanList(const Context& ctx, const std::vector<std::string>&) {
  return printList(ctx, ctx.hooks->banList, "Banned", "nobody is banned");
}

Result cmdKick(const Context& ctx, const std::vector<std::string>& args) {
  const std::vector<Participant> people = everyone(ctx);
  std::string error;
  const Participant* target = findParticipant(people, args[0], &error);
  if (!target) return Result::fail(error);
  if (target->self) return Result::fail("kicking yourself will not work the way you hope");
  if (!mayActOn(*target, ctx.level)) {
    return Result::fail(fmt("%s is %s, and you are %s \xC2\xB7 you can only remove somebody "
                            "below you",
                            target->name.c_str(), levelName(target->level),
                            levelName(ctx.level)));
  }
  if (!ctx.hooks->kick) return noHook("remove anyone");
  const std::string reason = args.size() > 1 ? args[1] : std::string();
  if (!ctx.hooks->kick(*target, reason, error)) return Result::fail(error);
  ctx.announce(target->name + " was kicked" + (reason.empty() ? "" : " \xC2\xB7 " + reason));
  return Result::done();
}

Result cmdBan(const Context& ctx, const std::vector<std::string>& args) {
  if (!ctx.hooks->setBanned) return noHook("ban anyone");
  const std::vector<Participant> people = everyone(ctx);
  const Participant* here = findParticipant(people, args[0], nullptr);
  if (here && here->self) return Result::fail("banning yourself will not work the way you hope");
  if (here && !mayActOn(*here, ctx.level)) {
    return Result::fail(fmt("%s is %s, and you are %s \xC2\xB7 you can only ban somebody "
                            "below you",
                            here->name.c_str(), levelName(here->level), levelName(ctx.level)));
  }

  const std::string reason = args.size() > 1 ? args[1] : std::string();
  const std::string name = here ? here->name : args[0];
  std::string error;
  if (!ctx.hooks->setBanned(name, true, reason, error)) return Result::fail(error);
  // The kick is separate from the ban on purpose: banning somebody who is not here
  // has to work, and banning somebody who IS here should not depend on the kick
  // succeeding for the ban to have been recorded.
  if (here && ctx.hooks->kick) {
    std::string ignored;
    ctx.hooks->kick(*here, reason.empty() ? "banned" : reason, ignored);
  }
  ctx.announce(name + " was banned" + (reason.empty() ? "" : " \xC2\xB7 " + reason));
  return Result::done();
}

Result cmdPardon(const Context& ctx, const std::vector<std::string>& args) {
  if (!ctx.hooks->setBanned) return noHook("ban anyone");
  std::string error;
  if (!ctx.hooks->setBanned(args[0], false, "", error)) return Result::fail(error);
  return Result::done(args[0] + " is no longer banned");
}

Result cmdWhitelist(const Context& ctx, const std::vector<std::string>& args) {
  const std::string& action = args[0];
  std::string error;

  if (action == "on" || action == "off") {
    if (!ctx.hooks->setWhitelistEnabled) return noHook("change the whitelist");
    if (!ctx.hooks->setWhitelistEnabled(action == "on", error)) return Result::fail(error);
    return Result::done(action == "on" ? "whitelist on \xC2\xB7 only listed players may join"
                                       : "whitelist off \xC2\xB7 anyone may join");
  }
  if (action == "list") {
    return printList(ctx, ctx.hooks->allowList, "Whitelisted", "the whitelist is empty");
  }
  if (action == "add" || action == "remove") {
    if (args.size() < 2) return Result::fail("say who: /whitelist " + action + " <player>");
    if (!ctx.hooks->setAllowed) return noHook("change the whitelist");
    const std::vector<Participant> people = everyone(ctx);
    const Participant* here = findParticipant(people, args[1], nullptr);
    const std::string name = here ? here->name : args[1];
    if (!ctx.hooks->setAllowed(name, action == "add", error)) return Result::fail(error);
    return Result::done(action == "add" ? name + " may join" : name + " may no longer join");
  }
  return Result::fail("say on, off, add, remove or list");
}

Result cmdSave(const Context& ctx, const std::vector<std::string>&) {
  if (!ctx.hooks->saveWorld) return noHook("save");
  std::string error;
  if (!ctx.hooks->saveWorld(error)) return Result::fail(error);
  return Result::done("world saved");
}

Result cmdStop(const Context& ctx, const std::vector<std::string>&) {
  if (!ctx.hooks->stopSession) return noHook("be stopped");
  std::string error;
  // Announced before it happens, because after it happens there is nobody left to
  // tell. A guest who is disconnected without warning has no way to tell a shutdown
  // from a crash or from their own network dropping.
  ctx.announce("the world is closing");
  if (!ctx.hooks->stopSession(error)) return Result::fail(error);
  return Result::done();
}

}  // namespace

// The table. Order is help order, grouped by what these are for.
Registry::Registry() {
  commands_ = {
      // --- talking ---
      {"help", {"?"}, "what you can type", "/help [command|page]", Level::Anyone,
       {{"command or page", ArgType::Word}}, false, cmdHelp},
      {"list", {"who", "players"}, "who is here", "/list", Level::Anyone, {}, false, cmdList},
      {"me", {}, "say what you are doing", "/me <action>", Level::Anyone,
       {{"action", ArgType::Text, true}}, false, cmdMe},
      {"msg", {"w", "tell", "whisper"}, "say something to one person",
       "/msg <player> <message>", Level::Anyone,
       {{"player", ArgType::Player, true}, {"message", ArgType::Text, true}}, false, cmdMsg},
      {"say", {}, "say something as the world", "/say <message>", Level::Operator,
       {{"message", ArgType::Text, true}}, false, cmdSay},

      // --- getting around ---
      {"spawn", {}, "go to the world's spawn point", "/spawn", Level::Trusted, {}, true,
       cmdSpawn},
      {"tp", {"teleport"}, "go to a player, or to a place", "/tp <player> | <x> <y> <z>",
       Level::Trusted,
       {{"player", ArgType::Player, true},
        {"x", ArgType::Coord},
        {"y", ArgType::Coord},
        {"z", ArgType::Coord}},
       true, cmdTeleport},
      {"locate", {}, "find the nearest dungeon", "/locate", Level::Trusted, {}, true, cmdLocate},

      // --- the world's state ---
      {"seed", {}, "the number this world grew from", "/seed", Level::Anyone, {}, true, cmdSeed},
      {"time", {}, "read or set the hour", "/time [day|night|noon|hh:mm]", Level::Operator,
       {{"when", ArgType::Choice,
         false,
         {"day", "night", "noon", "midnight", "dawn", "dusk"}}},
       true, cmdTime},
      {"gamemode", {"gm"}, "switch creative and survival", "/gamemode <creative|survival>",
       Level::Operator, {{"mode", ArgType::Choice, true, {"creative", "survival"}}}, true,
       cmdGamemode},
      {"set", {}, "read or change a setting", "/set <setting> [value]", Level::Anyone,
       {{"setting", ArgType::Setting, true}, {"value", ArgType::Value}}, false, cmdSet},

      // --- bodies and bags ---
      {"give", {"i"}, "put an item in somebody's bag", "/give <item> [count] [player]",
       Level::Operator,
       {{"item", ArgType::Item, true}, {"count", ArgType::Int}, {"player", ArgType::Player}},
       true, cmdGive},
      {"clear", {}, "empty somebody's bag", "/clear [player]", Level::Operator,
       {{"player", ArgType::Player}}, true, cmdClear},
      {"heal", {}, "fill somebody's hearts", "/heal [player]", Level::Operator,
       {{"player", ArgType::Player}}, true, cmdHeal},
      {"kill", {}, "empty somebody's hearts", "/kill [player]", Level::Anyone,
       {{"player", ArgType::Player}}, true, cmdKill},
      {"summon", {}, "place a mob beside you", "/summon <entity> [count]", Level::Operator,
       {{"entity", ArgType::Entity, true}, {"count", ArgType::Int}}, true, cmdSummon},

      // --- who may do what ---
      {"perms", {"ops"}, "who has been given a level", "/perms", Level::Operator, {}, false,
       cmdPerms},
      // Operator, not owner, and the grant ceiling in cmdOp is what makes that
      // safe: an operator may vouch for somebody up to their own rank and no
      // further, so a host can delegate looking after a world without handing it
      // over. Only an owner can mint another owner.
      {"op", {}, "give somebody a level", "/op <player> [level]", Level::Operator,
       {{"player", ArgType::Player, true}, {"level", ArgType::Perm}}, false, cmdOp},
      {"deop", {}, "take somebody's level away", "/deop <player>", Level::Operator,
       {{"player", ArgType::Player, true}}, false, cmdDeop},

      // --- who may be here ---
      {"kick", {}, "send somebody home", "/kick <player> [reason]", Level::Operator,
       {{"player", ArgType::Player, true}, {"reason", ArgType::Text}}, false, cmdKick},
      {"ban", {}, "send somebody home and keep them there", "/ban <player> [reason]",
       Level::Owner, {{"player", ArgType::Player, true}, {"reason", ArgType::Text}}, false,
       cmdBan},
      {"pardon", {"unban"}, "lift a ban", "/pardon <player>", Level::Owner,
       {{"player", ArgType::Word, true}}, false, cmdPardon},
      {"banlist", {}, "who is banned", "/banlist", Level::Owner, {}, false, cmdBanList},
      {"whitelist", {}, "only let named players in",
       "/whitelist <on|off|add|remove|list> [player]", Level::Owner,
       {{"action", ArgType::Choice, true, {"on", "off", "add", "remove", "list"}},
        {"player", ArgType::Player}},
       false, cmdWhitelist},

      // --- the session itself ---
      {"save", {}, "write the world to disk now", "/save", Level::Operator, {}, true, cmdSave},
      {"stop", {}, "save and close the world", "/stop", Level::Owner, {}, false, cmdStop},
  };
}

}  // namespace hr::cmd
