#include "cmd/command.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace hr::cmd {
namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool equalsNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

bool isSpace(char c) { return c == ' ' || c == '\t'; }

}  // namespace

const char* levelName(Level level) {
  switch (level) {
    case Level::Anyone: return "anyone";
    case Level::Trusted: return "trusted";
    case Level::Operator: return "operator";
    case Level::Owner: return "owner";
  }
  return "anyone";
}

bool levelFromName(std::string_view text, Level& out) {
  static constexpr Level kAll[] = {Level::Anyone, Level::Trusted, Level::Operator, Level::Owner};
  for (const Level level : kAll) {
    if (equalsNoCase(text, levelName(level))) {
      out = level;
      return true;
    }
  }
  // The digits too. Somebody administering a server from a console should not have
  // to learn our vocabulary before they can promote anyone.
  if (text.size() == 1 && text[0] >= '0' && text[0] <= '3') {
    out = static_cast<Level>(text[0] - '0');
    return true;
  }
  return false;
}

// ---- parsing ----------------------------------------------------------------

std::vector<Token> tokenize(std::string_view line) {
  std::vector<Token> out;
  std::size_t i = 0;
  // A leading slash is punctuation, not a token: `/give` and `give` are the same
  // command, which is what lets a server console and a chat box share this.
  if (i < line.size() && line[i] == '/') ++i;

  while (i < line.size()) {
    while (i < line.size() && isSpace(line[i])) ++i;
    if (i >= line.size()) break;

    Token token;
    token.begin = i;
    if (line[i] == '"') {
      token.quoted = true;
      ++i;
      while (i < line.size() && line[i] != '"') {
        // A backslash escapes the next character, which is only ever needed for a
        // quote inside a quoted name. Anything else keeps the backslash, so a
        // Windows path typed into a command is not silently mangled.
        if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '"') ++i;
        token.text.push_back(line[i]);
        ++i;
      }
      // An unterminated quote runs to the end of the line rather than failing:
      // somebody typing `"Ada Lo` has not made a mistake yet, and the completion
      // popup has to be able to offer them the rest of the name.
      if (i < line.size()) ++i;
    } else {
      while (i < line.size() && !isSpace(line[i])) {
        token.text.push_back(line[i]);
        ++i;
      }
    }
    token.end = i;
    out.push_back(std::move(token));
  }
  return out;
}

bool parseInt(std::string_view text, int lo, int hi, int& out) {
  if (text.empty()) return false;
  const std::string copy(text);
  char* end = nullptr;
  const long value = std::strtol(copy.c_str(), &end, 10);
  if (end == copy.c_str() || *end != '\0') return false;
  if (value < lo || value > hi) return false;
  out = static_cast<int>(value);
  return true;
}

bool parseBool(std::string_view text, bool& out) {
  if (equalsNoCase(text, "true") || equalsNoCase(text, "on") || equalsNoCase(text, "yes") ||
      text == "1") {
    out = true;
    return true;
  }
  if (equalsNoCase(text, "false") || equalsNoCase(text, "off") || equalsNoCase(text, "no") ||
      text == "0") {
    out = false;
    return true;
  }
  return false;
}

bool parseCoord(std::string_view text, float base, float& out) {
  if (text.empty()) return false;
  if (text[0] == '~') {
    if (text.size() == 1) {
      out = base;
      return true;
    }
    const std::string rest(text.substr(1));
    char* end = nullptr;
    const double offset = std::strtod(rest.c_str(), &end);
    if (end == rest.c_str() || *end != '\0') return false;
    out = base + static_cast<float>(offset);
    return true;
  }
  const std::string copy(text);
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end == copy.c_str() || *end != '\0') return false;
  out = static_cast<float>(value);
  return true;
}

const Participant* findParticipant(const std::vector<Participant>& people, std::string_view name,
                                   std::string* error) {
  if (name == "me" || name == "@s") {
    for (const Participant& p : people) {
      if (p.self) return &p;
    }
    if (error) *error = "there is nobody here to be";
    return nullptr;
  }
  // An exact match wins outright. Without this, two people called "ada" and "Ada"
  // would make every command naming either of them ambiguous — and the failure mode
  // of guessing is that somebody else gets kicked.
  for (const Participant& p : people) {
    if (p.name == name) return &p;
  }
  const Participant* found = nullptr;
  int matches = 0;
  for (const Participant& p : people) {
    if (equalsNoCase(p.name, name)) {
      found = &p;
      ++matches;
    }
  }
  if (matches == 1) return found;
  if (matches > 1) {
    if (error) *error = std::string("more than one player is called that");
    return nullptr;
  }
  if (error) *error = std::string("nobody here is called '") + std::string(name) + "'";
  return nullptr;
}

// ---- registry ---------------------------------------------------------------

const Registry& Registry::get() {
  static const Registry registry;
  return registry;
}

const Command* Registry::find(std::string_view nameOrAlias) const {
  for (const Command& c : commands_) {
    if (equalsNoCase(nameOrAlias, c.name)) return &c;
    for (const char* alias : c.aliases) {
      if (equalsNoCase(nameOrAlias, alias)) return &c;
    }
  }
  return nullptr;
}

// ---- dispatch ---------------------------------------------------------------

Result run(std::string_view line, const Context& ctx) {
  if (!ctx.hooks) return Result::fail("commands are not available here");

  const std::vector<Token> tokens = tokenize(line);
  if (tokens.empty()) return Result::fail("type a command name after the slash");

  const Command* command = Registry::get().find(tokens[0].text);
  if (!command) {
    return Result::fail("there is no command called '" + tokens[0].text +
                        "' \xC2\xB7 try /help");
  }
  // The level check comes before everything else including the world check, so a
  // guest cannot learn whether a world is open by watching which refusal they get.
  if (ctx.level < command->level) {
    return Result::fail(std::string("/") + command->name + " needs " +
                        levelName(command->level) + " \xC2\xB7 you are " + levelName(ctx.level));
  }
  if (command->needsWorld && !ctx.world) {
    return Result::fail(std::string("/") + command->name + " needs a world to be open");
  }

  // Arguments. Everything after the name, with one exception: a trailing Text
  // parameter swallows the rest of the LINE rather than the rest of the tokens, so
  // `/say  two   spaces` says exactly what was typed and a quoted stretch inside it
  // keeps its quotes.
  std::vector<std::string> args;
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    const std::size_t argIndex = i - 1;
    if (argIndex < command->args.size() && command->args[argIndex].type == ArgType::Text) {
      std::string rest(line.substr(tokens[i].begin));
      while (!rest.empty() && isSpace(rest.back())) rest.pop_back();
      args.push_back(std::move(rest));
      break;
    }
    args.push_back(tokens[i].text);
  }

  std::size_t required = 0;
  for (const ArgSpec& spec : command->args) {
    if (spec.required) ++required;
  }
  if (args.size() < required) {
    return Result::fail(std::string("usage: ") + command->usage);
  }
  if (!command->run) return Result::fail("that command does nothing yet");
  return command->run(ctx, args);
}

}  // namespace hr::cmd
