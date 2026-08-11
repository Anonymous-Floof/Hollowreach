// What to offer the player halfway through typing a command.
//
// The point of this is that nobody should have to remember that the key for a
// stone block is "stone" and the key for a stone pickaxe is "pick_stone". A command
// system whose arguments are registry keys is unusable from memory, and a list of
// them in the README is a list nobody reads while typing.
//
// FUZZY, not prefix. Typing `pkst` should find `pick_stone`, because the thing
// people remember about a key is its parts and not its punctuation. That is a
// subsequence match with a score, and the score is what makes the ordering useful
// rather than merely correct: an exact match, then a prefix, then a match that
// starts at a word boundary, then anything else.
//
// The candidates come from the same ArgSpec that documents the parameter, so the
// popup and the parser can never disagree about what a parameter accepts — see
// cmd/command.h's note on why ArgType is a type rather than a comment.
//
// Nothing here knows about the interface. It takes a line and a caret and returns
// a byte range to replace and a list of things to put there, which is testable
// without a window and is exactly what the chat overlay needs.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "cmd/command.h"

namespace hr::cmd {

// Enough to be worth a popup, few enough to fit above a chat box without covering
// the world. A longer list is not more useful — if the right answer is not in the
// first ten, another character of typing is faster than reading forty.
inline constexpr std::size_t kMaxSuggestions = 10;

struct Suggestion {
  // What to put in the line. Quoted already when the value contains a space, so
  // the caller inserts it verbatim and does not have to know the quoting rules.
  std::string text;
  // What to show. The unquoted value, which is what somebody is reading for.
  std::string label;
  // Dimmed, to the right: a command's summary, or the name of the parameter this
  // value is for. Empty when there is nothing useful to say.
  std::string hint;
  int score = 0;
};

// The live facts the registries cannot supply. Everything else — items, blocks,
// settings, command names — is a static table this reads directly.
struct Sources {
  // Display names of everybody in the session.
  std::vector<std::string> players;
  // The asking player's level. Commands above it are not offered: the popup is a
  // reminder of what you can do, and listing things that will be refused is worse
  // than listing nothing. It is a courtesy and NOT a gate — the host checks again,
  // and a modified client that offers itself /stop still gets refused.
  Level level = Level::Owner;
  // Commands needing a world are not offered from the menu, for the same reason.
  bool inWorld = true;
};

struct Completion {
  // The byte range of the line to replace. Equal when the caret sits at the start
  // of a word that has not been typed yet.
  std::size_t begin = 0;
  std::size_t end = 0;
  std::vector<Suggestion> items;
};

// Suggestions for the word the caret is in, or for the word about to be typed when
// it sits after a space. Empty when the line is not a command, or when the
// parameter under the caret is free text with nothing to offer.
Completion complete(std::string_view line, std::size_t caret, const Sources& sources);

// How well `query` matches `candidate`, or -1 when it is not a subsequence of it at
// all. Case-insensitive.
//
// Exposed because it is the half worth testing directly: the ordering it produces
// is the whole user-visible behaviour, and testing it through complete() would test
// it only for the candidates that happen to be registered today.
int fuzzyScore(std::string_view candidate, std::string_view query);

// Ranks `candidates` against `query` and keeps the best kMaxSuggestions. Ties break
// by name, so the same query always produces the same list in the same order — a
// popup that reshuffles between keystrokes is unusable with the arrow keys.
std::vector<Suggestion> rank(const std::vector<std::string>& candidates,
                             std::string_view query, std::string_view hint = {});

}  // namespace hr::cmd
