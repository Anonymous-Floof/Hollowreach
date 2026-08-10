// A small JSON reader.
//
// settings.json is a flat object of scalars, and settings.cpp reads it with a
// scanner rather than a parser for exactly that reason. Resource packs are not
// flat: `pack.mcmeta` nests one level and `sounds.json` is an object of events,
// each holding an array whose entries are *either* a string or an object —
//
//     { "block.stone.break": { "sounds": [ "block/stone/break1",
//                                          {"name": "...", "volume": 0.8} ] } }
//
// so the scanner cannot express it and something has to walk the grammar.
//
// Two things matter more here than in the settings file, because these bytes come
// from a pack somebody downloaded rather than from a file the game wrote:
//
//  * **Depth is capped.** Recursive descent on `[[[[[...` is a stack overflow, and
//    a stack overflow is not an exception you can catch. kMaxDepth is the whole
//    defence and it is checked on the way in, not on the way out.
//  * **Trailing content is an error.** A file that parses as `{}` followed by junk
//    is a malformed file, and silently taking the first value would load a pack
//    that the author has no idea is broken.
//
// Nothing here throws. A parse failure returns a Null value and fills `errorOut`,
// matching how the rest of the codebase reports bad files.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hr::json {

// Deep enough for any hand-written config, shallow enough that the recursion
// cannot run the stack out. sounds.json needs 4.
inline constexpr int kMaxDepth = 32;

class Value {
 public:
  enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

  Value() = default;

  Kind kind() const { return kind_; }
  bool isNull() const { return kind_ == Kind::Null; }
  bool isBool() const { return kind_ == Kind::Bool; }
  bool isNumber() const { return kind_ == Kind::Number; }
  bool isString() const { return kind_ == Kind::String; }
  bool isArray() const { return kind_ == Kind::Array; }
  bool isObject() const { return kind_ == Kind::Object; }

  // Scalar reads with a fallback, so a caller never has to test the kind first.
  // A wrong-typed field reads as the fallback rather than as an error, which is
  // the tolerance a pack format wants: one mistyped `"volume": "loud"` should
  // cost that one field, not the pack.
  bool flag(bool fallback = false) const { return kind_ == Kind::Bool ? bool_ : fallback; }
  double num(double fallback = 0.0) const { return kind_ == Kind::Number ? number_ : fallback; }
  const std::string& str(const std::string& fallback) const {
    return kind_ == Kind::String ? string_ : fallback;
  }
  std::string str() const { return kind_ == Kind::String ? string_ : std::string(); }

  std::size_t size() const {
    if (kind_ == Kind::Array) return array_.size();
    if (kind_ == Kind::Object) return object_.size();
    return 0;
  }

  // Absent keys and out-of-range indices give the shared Null value rather than
  // throwing, so `doc["pack"]["description"].str()` is safe on any document.
  const Value& operator[](std::string_view key) const;
  const Value& at(std::size_t index) const;

  const std::vector<Value>& items() const { return array_; }
  // Insertion order is preserved. sounds.json has no ordering requirement, but a
  // stable order makes a dump of what a pack supplies reproducible.
  const std::vector<std::pair<std::string, Value>>& fields() const { return object_; }

  static const Value& null();

 private:
  friend class Parser;

  Kind kind_ = Kind::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<Value> array_;
  std::vector<std::pair<std::string, Value>> object_;
};

// Parses a whole document. Returns Null and sets `errorOut` on any failure,
// including trailing content after the top-level value.
Value parse(std::string_view src, std::string* errorOut = nullptr);

// Reads and parses a file. A missing file and a malformed one are both failures
// with a message, because a pack that names a sounds.json it does not have is as
// broken as one whose sounds.json does not parse.
Value parseFile(const std::string& path, std::string* errorOut = nullptr);

// Escapes a string for embedding in JSON output. The example-pack writer needs
// it; nothing else does.
std::string escape(std::string_view s);

}  // namespace hr::json
