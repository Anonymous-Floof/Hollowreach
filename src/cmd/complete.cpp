#include "cmd/complete.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "game/entities/entity.h"
#include "game/items.h"
#include "ui/settings.h"
#include "world/blocks.h"

namespace hr::cmd {
namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// The characters a key is built out of. A match landing just after one of these —
// the `s` of `pick_stone` — is what somebody typing `pkst` means, and scoring it
// above a match in the middle of a word is most of what makes the ordering read as
// intelligent rather than arbitrary.
bool isBoundary(char c) {
  return c == '_' || c == '.' || c == '-' || c == '/' || c == ' ' || c == ':';
}

bool startsBoundary(std::string_view candidate, std::size_t i) {
  if (i == 0) return true;
  if (isBoundary(candidate[i - 1])) return true;
  // A capital after a lower case letter starts a word in camelCase, which is how
  // every settings key is spelled.
  const bool prevLower = candidate[i - 1] >= 'a' && candidate[i - 1] <= 'z';
  const bool thisUpper = candidate[i] >= 'A' && candidate[i] <= 'Z';
  return prevLower && thisUpper;
}

std::string quoteIfNeeded(const std::string& value) {
  if (value.find(' ') == std::string::npos) return value;
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::vector<std::string> itemKeys() {
  std::vector<std::string> out;
  out.reserve(game::items().count());
  for (const game::ItemDef& def : game::items().all()) out.push_back(def.key);
  return out;
}

std::vector<std::string> blockKeys() {
  std::vector<std::string> out;
  for (const world::BlockDef& def : world::blocks().all()) {
    if (!def.key.empty()) out.push_back(def.key);
  }
  return out;
}

// The types a player can reasonably ask for. Drop, RemotePlayer and FallingBlock
// are excluded on purpose: they are how the game represents something that has
// already happened, and summoning one directly produces an entity with no item,
// no owner or no block to be.
std::vector<std::string> entityKeys() {
  std::vector<std::string> out;
  static constexpr game::EntityType kSummonable[] = {
      game::EntityType::Sheep, game::EntityType::Pig, game::EntityType::Cow,
      game::EntityType::Zombie, game::EntityType::Boat};
  for (const game::EntityType type : kSummonable) out.emplace_back(game::entityTypeKey(type));
  return out;
}

std::vector<std::string> settingKeys() {
  std::vector<std::string> out;
  for (const ui::SettingDef& def : ui::settingsSchema()) {
    // A hidden row is not a row anyone chooses; offering `menuBackground` here
    // would invite somebody to type a file path into a chat box.
    if (def.hidden) continue;
    // An Action stores nothing, so there is no value to set — it belongs to the
    // settings screen's buttons, not to /set.
    if (def.type == ui::SettingType::Action) continue;
    out.emplace_back(def.key);
  }
  return out;
}

// What the value for a named setting may be. An empty list means "a number", which
// has nothing to offer but is worth saying in the hint.
std::vector<std::string> settingValues(std::string_view key, std::string& hint) {
  const ui::SettingDef* def = ui::settings().find(std::string(key));
  if (!def) return {};
  switch (def->type) {
    case ui::SettingType::Toggle:
      hint = "on or off";
      return {"true", "false"};
    case ui::SettingType::Select: {
      hint = "one of";
      std::vector<std::string> out;
      for (const char* option : def->options) out.emplace_back(option);
      return out;
    }
    case ui::SettingType::Slider: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%g to %g", def->min, def->max);
      hint = buf;
      return {};
    }
    default:
      return {};
  }
}

std::vector<std::string> levelKeys() {
  return {levelName(Level::Anyone), levelName(Level::Trusted), levelName(Level::Operator),
          levelName(Level::Owner)};
}

}  // namespace

int fuzzyScore(std::string_view candidate, std::string_view query) {
  if (query.empty()) return 0;
  if (query.size() > candidate.size()) return -1;

  // Whether the rest of `query` from `qi` can still be found in `candidate` from
  // `from`. Needed because the loop below deliberately steps past an early match to
  // reach a better one, and a preference that could turn a real match into a miss
  // would be a matcher that sometimes lies about what it contains.
  const auto restFits = [&](std::size_t qi, std::size_t from) {
    for (std::size_t i = qi; i < query.size(); ++i) {
      while (from < candidate.size() && lower(candidate[from]) != lower(query[i])) ++from;
      if (from >= candidate.size()) return false;
      ++from;
    }
    return true;
  };

  int score = 0;
  std::size_t at = 0;
  std::size_t previous = std::string_view::npos;
  for (std::size_t qi = 0; qi < query.size(); ++qi) {
    const char want = lower(query[qi]);
    std::size_t found = std::string_view::npos;
    for (std::size_t i = at; i < candidate.size(); ++i) {
      if (lower(candidate[i]) == want) {
        found = i;
        break;
      }
    }
    if (found == std::string_view::npos) return -1;  // not a subsequence at all

    // The FIRST match is not always the one meant. `rd` against `renderDistance`
    // lands on the `d` three characters into "render", while the `D` that starts
    // "Distance" is plainly what was intended — and because matching is
    // case-insensitive, plain greedy never gets there and scores the camelCase key
    // exactly as low as it scores `renderdistance`.
    //
    // So look past a poor match for one at a word boundary. Only when the first is
    // not already good, and only when the rest of the query still fits afterwards.
    const bool firstIsGood = found == 0 ||
                             (previous != std::string_view::npos && found == previous + 1) ||
                             startsBoundary(candidate, found);
    if (!firstIsGood) {
      for (std::size_t i = found + 1; i < candidate.size(); ++i) {
        if (lower(candidate[i]) != want || !startsBoundary(candidate, i)) continue;
        if (restFits(qi + 1, i + 1)) found = i;
        break;  // the FIRST boundary match or none: a later one is further to skip
      }
    }

    if (found == 0) {
      score += 20;
    } else if (previous != std::string_view::npos && found == previous + 1) {
      score += 12;  // a run: `sto` inside `stone` beats s..t..o scattered
    } else if (startsBoundary(candidate, found)) {
      score += 14;
    } else {
      score += 2;
    }
    // Every character stepped over is a character the typist did not mean. Capped
    // so one long key does not score below a short bad match purely on length.
    const std::size_t skipped = found - at;
    score -= static_cast<int>(std::min<std::size_t>(skipped, 8));
    previous = found;
    at = found + 1;
  }

  // The two shapes worth lifting clear of everything else, because they are what
  // somebody typing a name they already know is aiming at.
  const bool prefix =
      candidate.size() >= query.size() &&
      std::equal(query.begin(), query.end(), candidate.begin(),
                 [](char a, char b) { return lower(a) == lower(b); });
  if (prefix) score += 30;
  if (prefix && candidate.size() == query.size()) score += 60;

  // A tiebreaker, not a factor: between two candidates that matched equally well,
  // the shorter one is the one with less of it left unexplained.
  score += static_cast<int>(32 - std::min<std::size_t>(candidate.size(), 32)) / 2;
  return score;
}

std::vector<Suggestion> rank(const std::vector<std::string>& candidates, std::string_view query,
                             std::string_view hint) {
  std::vector<Suggestion> out;
  out.reserve(candidates.size());
  for (const std::string& candidate : candidates) {
    const int score = fuzzyScore(candidate, query);
    if (score < 0) continue;
    Suggestion s;
    s.text = quoteIfNeeded(candidate);
    s.label = candidate;
    s.hint = std::string(hint);
    s.score = score;
    out.push_back(std::move(s));
  }
  // Score first, then name. Name rather than nothing because a popup that
  // reshuffles equal-scoring entries between keystrokes cannot be driven with the
  // arrow keys — the row under the cursor has to still be the row you were aiming
  // at after the next character.
  std::sort(out.begin(), out.end(), [](const Suggestion& a, const Suggestion& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.label < b.label;
  });
  if (out.size() > kMaxSuggestions) out.resize(kMaxSuggestions);
  return out;
}

Completion complete(std::string_view line, std::size_t caret, const Sources& sources) {
  Completion out;
  caret = std::min(caret, line.size());
  out.begin = out.end = caret;

  // Only a command line has anything to complete. A chat message is prose.
  if (line.empty() || line[0] != '/') return out;

  const std::vector<Token> tokens = tokenize(line);

  // Which word the caret is in, or the one about to be typed. A caret resting
  // inside a word completes that whole word rather than the half before it: the
  // common case is the end of the word, where the two are the same, and replacing
  // half a word would leave the other half behind as garbage.
  std::size_t index = tokens.size();  // past the end: a new, empty word
  std::string query;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (caret >= tokens[i].begin && caret <= tokens[i].end) {
      index = i;
      query = tokens[i].text;
      out.begin = tokens[i].begin;
      out.end = tokens[i].end;
      break;
    }
  }

  if (index == 0) {
    std::vector<std::string> names;
    std::vector<std::string> summaries;
    for (const Command& c : Registry::get().all()) {
      if (sources.level < c.level) continue;
      if (c.needsWorld && !sources.inWorld) continue;
      names.emplace_back(c.name);
      summaries.emplace_back(c.summary);
    }
    out.items = rank(names, query);
    // The summary as the hint, so the list explains itself. Looked up by name
    // rather than carried through rank(), which has no business knowing what a
    // command is.
    for (Suggestion& s : out.items) {
      for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == s.label) {
          s.hint = summaries[i];
          break;
        }
      }
    }
    return out;
  }

  // An argument. Which one it is depends on the command, so an unknown command
  // offers nothing rather than guessing.
  const Command* command = Registry::get().find(tokens[0].text);
  if (!command || sources.level < command->level) return out;

  const std::size_t argIndex = index - 1;
  if (argIndex >= command->args.size()) return out;
  const ArgSpec& spec = command->args[argIndex];

  std::string hint = spec.name;
  std::vector<std::string> candidates;
  switch (spec.type) {
    case ArgType::Player: candidates = sources.players; break;
    case ArgType::Item: candidates = itemKeys(); break;
    case ArgType::Block: candidates = blockKeys(); break;
    case ArgType::Entity: candidates = entityKeys(); break;
    case ArgType::Setting: candidates = settingKeys(); break;
    case ArgType::Perm: candidates = levelKeys(); break;
    case ArgType::Bool: candidates = {"true", "false"}; break;
    case ArgType::Coord: candidates = {"~"}; break;
    case ArgType::Choice:
      for (const char* option : spec.options) candidates.emplace_back(option);
      break;
    case ArgType::Value: {
      // The values a setting accepts depend on WHICH setting, which is the token
      // before this one. That is the whole reason Value is its own type: a list of
      // every legal value of every setting would be meaningless.
      if (argIndex >= 1 && argIndex < tokens.size()) {
        candidates = settingValues(tokens[argIndex].text, hint);
      }
      break;
    }
    case ArgType::Word:
    case ArgType::Text:
    case ArgType::Int:
    case ArgType::Number:
      break;  // free text: nothing to offer, and a popup of nothing is noise
  }

  out.items = rank(candidates, query, hint);
  return out;
}

}  // namespace hr::cmd
