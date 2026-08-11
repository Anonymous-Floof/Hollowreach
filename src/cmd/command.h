// Typed commands: who may run what, what a command may touch, and how a line of
// text becomes an action.
//
// This layer exists twice over. Today it is what a player types into chat. Later
// it is the whole interface of the dedicated server, which has no window, no
// settings screen and no menu — every operational act, from kicking somebody to
// shutting the thing down, has to be expressible as a line of text or it cannot be
// done at all. That is why this is a module of its own rather than a switch
// statement inside the chat overlay, and why nothing here includes a UI header.
//
// THE TRUST MODEL, which is the part worth reading:
//
//   A command from a guest is a REQUEST, exactly like an edit or a hit. It is
//   parsed and executed on the host, at the level the host has recorded for that
//   guest, and the guest is told the result. Nothing is decided on the asking end.
//   The completion popup on a guest's screen is a convenience and is deliberately
//   given the same level so it does not offer what the host would refuse — but it
//   is a hint, not a gate, and a modified client that offers itself /stop still
//   gets a refusal from the host.
//
// Two structural choices follow from that:
//
//  1. **Everything a command can touch is named.** A Context holds live pointers to
//     the five objects the game is made of, any of which may be null, plus a Hooks
//     struct for the side effects. Same reasoning as net/session.h: "what a command
//     can reach" should be a list you can read rather than a property of what
//     happened to be in scope.
//  2. **Anything affecting somebody else goes through a hook.** The host does not
//     simulate a guest's body, so there is no Player object to move; and in single
//     player there is nobody else to move. A hook is the only shape that is honest
//     in both cases.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/mat4.h"

namespace hr::world {
class World;
}
namespace hr::render {
class Sky;
}
namespace hr::game {
class Player;
class Inventory;
class EntityManager;
}  // namespace hr::game

namespace hr::cmd {

// Who may run what.
//
// Four levels rather than Minecraft's numeric 1-4, because the numbers there are a
// historical accident and nobody remembers which is which. These are named for what
// they let you do, and the boundaries are drawn at the three questions worth
// separating:
//
//   Anyone    — affects only yourself, or only reads. Talking, listing who is here,
//               asking the seed, changing your own field of view.
//   Trusted   — bends the world's rules for yourself. Teleporting to a friend,
//               finding a dungeon. Granted by the host to people they trust not to
//               spoil their own game; harmless to everyone else.
//   Operator  — the world's rules, and other people's bodies. Difficulty, time of
//               day, giving items, moving somebody who did not ask to be moved.
//   Owner     — the session itself: who may connect, who is an operator, and
//               whether the thing keeps running. The host always holds this, and on
//               a dedicated server it is what the console holds.
//
// The order matters and is compared numerically, so a check is `caller >= needed`.
enum class Level : std::uint8_t {
  Anyone = 0,
  Trusted = 1,
  Operator = 2,
  Owner = 3,
};

const char* levelName(Level level);
// Accepts the names above, case-insensitively, and the digits 0-3 — the digits
// because a server console operator typing `op ada 3` should not have to guess our
// vocabulary. False for anything else, leaving `out` untouched.
bool levelFromName(std::string_view text, Level& out);

// What a parameter accepts.
//
// A type rather than a free-form string because it drives two things that must not
// disagree: what the completion popup offers, and what the command gets handed. A
// parameter documented as a player name and completed from the block registry is a
// bug that a comment cannot prevent and this can.
enum class ArgType : std::uint8_t {
  Word,     // one bare token, no completion
  Text,     // the rest of the line, spaces and all. Only ever the last parameter.
  Int,
  Number,
  Bool,
  Player,   // completed from whoever is in the session
  Item,     // completed from the item registry
  Block,    // completed from the block registry
  Entity,   // completed from the spawnable entity types
  Setting,  // completed from the settings schema
  Value,    // the value for the setting named in the PREVIOUS argument
  Perm,     // a permission level name
  Coord,    // one axis of a position; `~` means "where you already are"
  Choice,   // completed from this parameter's own `options`
};

struct ArgSpec {
  const char* name = "";
  ArgType type = ArgType::Word;
  bool required = false;
  // Choice only. Empty for every other type, whose candidates come from the world.
  std::vector<const char*> options;
};

// One person the command layer can name. Assembled by whoever owns the session —
// App from the host's roster, the dedicated server from its own — so this layer
// never has to know whether a body is local, remote, or not simulated at all.
struct Participant {
  std::string playerId;
  std::string name;
  Level level = Level::Anyone;
  bool self = false;   // the caller
  bool host = false;
  Vec3 pos;
  bool hasPos = false;  // false for somebody who has not sent a pose yet
};

// The side effects. Every one of these can fail, and every one says why: a command
// that quietly does nothing is worse than one that refuses out loud, because the
// player has no way to tell the two apart.
//
// `reply` and `announce` are the only two that are always present; a caller that
// leaves one of the others empty is saying "this session cannot do that", and the
// command reports it as such rather than crashing on an empty std::function.
struct Hooks {
  // One line back to whoever ran the command. May be called any number of times.
  std::function<void(std::string_view)> reply;
  // One line to everybody, as a system line rather than as somebody's chat.
  std::function<void(std::string_view)> announce;
  // Everyone in the session, the caller included. Empty in a menu with no world.
  std::function<std::vector<Participant>()> participants;

  // The rest may be unset. Each fills `error` and returns false when it refuses.
  std::function<bool(const Participant&, const Vec3&, std::string& error)> teleport;
  std::function<bool(const Participant&, const std::string& key, int count,
                     std::string& error)>
      give;
  std::function<bool(const Participant&, std::string& error)> clearInventory;
  // Health, hunger and breath together: `kill` is this at zero. One hook rather
  // than three because the three always move together and a partial application —
  // healed but still starving — is not a state any command wants.
  std::function<bool(const Participant&, float health, std::string& error)> setVitals;
  std::function<bool(const Participant&, std::string_view reason, std::string& error)> kick;
  // One line to one person. Its own hook rather than a flavour of `announce`
  // because who may read it is the whole point, and a broadcast that the receivers
  // are trusted to filter is not private at all.
  std::function<bool(const Participant&, const std::string& text, std::string& error)> whisper;
  std::function<bool(const std::string& playerId, const std::string& name, Level,
                     std::string& error)>
      setLevel;
  // Bans and the whitelist. `on` false is a pardon / a removal.
  std::function<bool(const std::string& name, bool on, std::string_view reason,
                     std::string& error)>
      setBanned;
  std::function<bool(const std::string& name, bool on, std::string& error)> setAllowed;
  std::function<bool(bool on, std::string& error)> setWhitelistEnabled;
  // Lines describing the ban list, the whitelist and the permission table, for the
  // commands that only report. Separate from `participants` because these name
  // people who are NOT here, which is the entire point of them.
  std::function<std::vector<std::string>()> banList;
  std::function<std::vector<std::string>()> allowList;
  std::function<std::vector<std::string>()> permList;

  std::function<bool(std::string& error)> saveWorld;
  // Ends the session. On a host this closes the world; on the dedicated server it
  // is how the process is told to stop.
  std::function<bool(std::string& error)> stopSession;
  // Writes one settings row. The store is process-wide, but this goes through a
  // hook because applying a setting means pushing it into whichever subsystem owns
  // it — and on a host, telling every guest.
  std::function<bool(const std::string& key, const std::string& value, std::string& error)>
      applySetting;
  // Spawns `count` of a mob near the caller. Refused with no world.
  std::function<bool(const std::string& type, int count, std::string& error)> summon;
  // The nearest dungeon to the caller, for /locate.
  std::function<bool(Vec3& out, std::string& error)> locateDungeon;
  // Where an unbound player wakes up. Not derivable here: it is chosen from the
  // terrain when the world opens and held by whoever opened it.
  std::function<bool(Vec3& out, std::string& error)> worldSpawn;
};

// Everything a command may reach.
struct Context {
  // Who is asking.
  std::string playerId;
  std::string name = "Player";
  Level level = Level::Anyone;
  // Whether this came from a console rather than from somebody standing in the
  // world. The dedicated server's own input is the case that matters: it has no
  // body, so a command that needs one refuses rather than reaching for a null
  // player. Also true for --command, which runs before anyone could have typed it.
  bool console = false;

  // The live game. Any of these may be null — from the main menu they all are —
  // and a command that needs one says so through `needsWorld` rather than checking.
  world::World* world = nullptr;
  game::Player* player = nullptr;
  game::Inventory* inventory = nullptr;
  game::EntityManager* entities = nullptr;
  render::Sky* sky = nullptr;

  const Hooks* hooks = nullptr;

  // Convenience for the command bodies: never null-checks, because dispatch
  // refuses a context with no hooks before any command runs.
  void reply(std::string_view line) const {
    if (hooks && hooks->reply) hooks->reply(line);
  }
  void announce(std::string_view line) const {
    if (hooks && hooks->announce) hooks->announce(line);
  }
};

// What running a line produced. `message` is replied automatically by run(); a
// command that has already said everything it wants through ctx.reply() leaves it
// empty.
struct Result {
  bool ok = true;
  std::string message;

  static Result fail(std::string why) { return Result{false, std::move(why)}; }
  static Result done(std::string what = {}) { return Result{true, std::move(what)}; }
};

struct Command {
  const char* name = "";
  // Alternative spellings. Matched exactly like the name, but never offered by the
  // completion popup — an alias exists to be typed by somebody who already knows
  // it, and listing both doubles the length of every suggestion list.
  std::vector<const char*> aliases;
  const char* summary = "";
  // Shown by /help and in the suggestion hint. Written out rather than generated
  // from `args` so it can say things the type system cannot — that /tp takes
  // EITHER a player or three coordinates.
  const char* usage = "";
  // The minimum to see this command at all. A command may demand MORE for a
  // particular argument — /set is Anyone because your own field of view is your
  // own business, and refuses a world rule below Operator — but never less.
  Level level = Level::Anyone;
  std::vector<ArgSpec> args;
  // Refused from the main menu, with a reason, rather than crashing on a null
  // world. Checked by dispatch so no command body has to.
  bool needsWorld = false;
  Result (*run)(const Context&, const std::vector<std::string>&) = nullptr;
};

// Every command, in help order. A table rather than a registration API: the set is
// fixed at compile time, and a table can be checked for consistency by a test.
class Registry {
 public:
  static const Registry& get();

  const std::vector<Command>& all() const { return commands_; }
  // Matches the name or any alias, case-insensitively. Null when nothing matches.
  const Command* find(std::string_view nameOrAlias) const;

 private:
  Registry();
  std::vector<Command> commands_;
};

// ---- parsing ----------------------------------------------------------------

// One token and where it came from. The byte range is what makes completion able
// to replace exactly the word under the caret rather than guessing at whitespace.
struct Token {
  std::string text;   // unquoted
  std::size_t begin = 0;  // index of the first byte, the opening quote included
  std::size_t end = 0;    // one past the last, the closing quote included
  bool quoted = false;
};

// Splits a line into tokens. Double quotes group a token containing spaces, and a
// backslash escapes a quote inside one. An unterminated quote runs to the end of
// the line rather than failing: somebody is still typing it.
//
// A leading '/' is not a token and is skipped.
std::vector<Token> tokenize(std::string_view line);

// Runs one line. The leading '/' is optional, so the same function serves chat
// (where it is typed) and a server console (where it is not).
//
// Refuses, rather than running, when: the line is empty, the command is unknown,
// the caller's level is too low, the command needs a world and there is none, or a
// required argument is missing. Each refusal says which.
Result run(std::string_view line, const Context& ctx);

// The named participant, or null. Accepts a name case-insensitively, "me" and "@s"
// for the caller. Ambiguity — two people whose names differ only by case — resolves
// to the exact match if there is one and fails otherwise, because guessing which
// of two people you meant is how somebody else gets kicked.
const Participant* findParticipant(const std::vector<Participant>& people,
                                   std::string_view name, std::string* error = nullptr);

// Parses one coordinate. `~` is `base`, `~n` is `base + n`, anything else is an
// absolute number. False when it is not a number at all.
bool parseCoord(std::string_view text, float base, float& out);
// Parses an integer with bounds, so no command body repeats the same three lines.
bool parseInt(std::string_view text, int lo, int hi, int& out);
// "true"/"false", "on"/"off", "yes"/"no", "1"/"0".
bool parseBool(std::string_view text, bool& out);

}  // namespace hr::cmd
