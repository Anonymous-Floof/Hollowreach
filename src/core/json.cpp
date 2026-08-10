#include "core/json.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hr::json {
namespace {

bool isWhitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// The three bytes of a UTF-8 encoding for a BMP code point, appended in place.
void appendUtf8(std::string& out, unsigned int cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

const Value& Value::null() {
  static const Value kNull;
  return kNull;
}

const Value& Value::operator[](std::string_view key) const {
  if (kind_ != Kind::Object) return null();
  for (const auto& field : object_) {
    if (field.first == key) return field.second;
  }
  return null();
}

const Value& Value::at(std::size_t index) const {
  if (kind_ != Kind::Array || index >= array_.size()) return null();
  return array_[index];
}

// ---------------------------------------------------------------------------

class Parser {
 public:
  Parser(std::string_view src, std::string& error) : src_(src), error_(error) {}

  bool parseDocument(Value& out) {
    skipBom();
    skipSpace();
    if (!parseValue(out, 0)) return false;
    skipSpace();
    if (pos_ != src_.size()) return fail("trailing content after the top-level value");
    return true;
  }

 private:
  bool fail(const char* what) {
    if (error_.empty()) {
      char buffer[160];
      std::snprintf(buffer, sizeof(buffer), "%s at byte %zu", what, pos_);
      error_ = buffer;
    }
    return false;
  }

  // A UTF-8 byte-order mark. Windows text editors write one by default, and a
  // pack hand-edited in Notepad is the common case rather than the exotic one.
  void skipBom() {
    if (src_.size() >= 3 && static_cast<unsigned char>(src_[0]) == 0xEF &&
        static_cast<unsigned char>(src_[1]) == 0xBB &&
        static_cast<unsigned char>(src_[2]) == 0xBF) {
      pos_ = 3;
    }
  }

  void skipSpace() {
    while (pos_ < src_.size() && isWhitespace(src_[pos_])) ++pos_;
  }

  bool atEnd() const { return pos_ >= src_.size(); }
  char peek() const { return src_[pos_]; }

  bool literal(std::string_view word) {
    if (src_.compare(pos_, word.size(), word) != 0) return false;
    pos_ += word.size();
    return true;
  }

  bool parseValue(Value& out, int depth) {
    // Checked before descending, not after: the point is to never make the call.
    if (depth >= kMaxDepth) return fail("nested too deeply");
    if (atEnd()) return fail("unexpected end of document");
    switch (peek()) {
      case '{': return parseObject(out, depth);
      case '[': return parseArray(out, depth);
      case '"':
        out.kind_ = Value::Kind::String;
        return parseString(out.string_);
      case 't':
        if (!literal("true")) return fail("bad literal");
        out.kind_ = Value::Kind::Bool;
        out.bool_ = true;
        return true;
      case 'f':
        if (!literal("false")) return fail("bad literal");
        out.kind_ = Value::Kind::Bool;
        out.bool_ = false;
        return true;
      case 'n':
        if (!literal("null")) return fail("bad literal");
        out.kind_ = Value::Kind::Null;
        return true;
      default: return parseNumber(out);
    }
  }

  bool parseObject(Value& out, int depth) {
    out.kind_ = Value::Kind::Object;
    ++pos_;  // '{'
    skipSpace();
    if (!atEnd() && peek() == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      skipSpace();
      if (atEnd() || peek() != '"') return fail("expected a key");
      std::string key;
      if (!parseString(key)) return false;
      skipSpace();
      if (atEnd() || peek() != ':') return fail("expected ':'");
      ++pos_;
      skipSpace();
      Value child;
      if (!parseValue(child, depth + 1)) return false;
      // A duplicate key takes the last value, which is what every mainstream
      // parser does and therefore what a pack author will have tested against.
      bool replaced = false;
      for (auto& field : out.object_) {
        if (field.first == key) {
          field.second = std::move(child);
          replaced = true;
          break;
        }
      }
      if (!replaced) out.object_.emplace_back(std::move(key), std::move(child));
      skipSpace();
      if (atEnd()) return fail("unterminated object");
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool parseArray(Value& out, int depth) {
    out.kind_ = Value::Kind::Array;
    ++pos_;  // '['
    skipSpace();
    if (!atEnd() && peek() == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      skipSpace();
      Value child;
      if (!parseValue(child, depth + 1)) return false;
      out.array_.push_back(std::move(child));
      skipSpace();
      if (atEnd()) return fail("unterminated array");
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parseString(std::string& out) {
    ++pos_;  // '"'
    out.clear();
    while (pos_ < src_.size()) {
      const char c = src_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= src_.size()) return fail("unterminated escape");
      const char e = src_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned int cp = 0;
          if (!hex4(cp)) return false;
          // A surrogate pair is two escapes describing one code point. Decoding
          // the halves separately produces two invalid three-byte sequences,
          // which is how non-BMP characters turn into mojibake in a description.
          if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < src_.size() && src_[pos_] == '\\' &&
              src_[pos_ + 1] == 'u') {
            const std::size_t save = pos_;
            pos_ += 2;
            unsigned int low = 0;
            if (hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else {
              pos_ = save;  // a lone high surrogate; emit it as-is
              error_.clear();
            }
          }
          appendUtf8(out, cp);
          break;
        }
        default: return fail("unknown escape");
      }
    }
    return fail("unterminated string");
  }

  bool hex4(unsigned int& out) {
    if (pos_ + 4 > src_.size()) return fail("short \\u escape");
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = src_[pos_++];
      out <<= 4;
      if (c >= '0' && c <= '9') out |= static_cast<unsigned int>(c - '0');
      else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned int>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned int>(c - 'A' + 10);
      else return fail("bad hex digit");
    }
    return true;
  }

  bool parseNumber(Value& out) {
    const std::size_t start = pos_;
    if (!atEnd() && (peek() == '-' || peek() == '+')) ++pos_;
    bool anyDigits = false;
    while (!atEnd() && peek() >= '0' && peek() <= '9') {
      ++pos_;
      anyDigits = true;
    }
    if (!atEnd() && peek() == '.') {
      ++pos_;
      while (!atEnd() && peek() >= '0' && peek() <= '9') {
        ++pos_;
        anyDigits = true;
      }
    }
    if (!anyDigits) return fail("expected a value");
    if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
      ++pos_;
      if (!atEnd() && (peek() == '-' || peek() == '+')) ++pos_;
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    // strtod reads the locale's decimal separator, and a German locale makes
    // "0.8" parse as 0. The game never calls setlocale, so this is the C locale
    // and '.' is correct — but the span is bounded by the scan above rather than
    // by strtod's own idea of where the number ends, so a malformed tail cannot
    // be silently swallowed.
    const std::string text(src_.substr(start, pos_ - start));
    out.kind_ = Value::Kind::Number;
    out.number_ = std::strtod(text.c_str(), nullptr);
    return true;
  }

  std::string_view src_;
  std::string& error_;
  std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------

Value parse(std::string_view src, std::string* errorOut) {
  std::string error;
  Value out;
  Parser parser(src, error);
  if (!parser.parseDocument(out)) {
    if (errorOut) *errorOut = error;
    return Value();
  }
  if (errorOut) errorOut->clear();
  return out;
}

Value parseFile(const std::string& path, std::string* errorOut) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (errorOut) *errorOut = "cannot open " + path;
    return Value();
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string text = buffer.str();
  std::string error;
  Value out = parse(text, &error);
  if (!error.empty()) {
    if (errorOut) *errorOut = path + ": " + error;
    return Value();
  }
  if (errorOut) errorOut->clear();
  return out;
}

std::string escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
          out += buffer;
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

}  // namespace hr::json
