// Little-endian binary reader and writer.
//
// The web build had no equivalent: saves were JSON and the network protocol was
// JSON over a data channel. Both become binary here, and both need the same two
// properties, which is why this is one shared type rather than two:
//
//   * A fixed byte order, written by shifting rather than by memcpy of a struct,
//     so a file written on one machine reads on another whatever the host does.
//   * A reader that FAILS CLOSED. Every read is bounds-checked; the first read
//     past the end latches a failure flag and every later read returns zero
//     without touching memory. A decoder can therefore run straight through a
//     truncated or malicious file and check `ok()` once at the end, instead of
//     testing every field — which is the only way this stays readable, and the
//     only way the multiplayer protocol at M11 can be written to reject a hostile
//     packet without a hand-audited branch per field.
//
// Counts are the one thing a decoder still has to check itself: a garbage file can
// claim four billion elements, and reserving that up front is a denial of service
// even though every read afterwards would fail safely. `remaining()` is the guard —
// no element is smaller than a byte, so a count larger than the bytes left is a
// lie. `readCount()` does exactly that check.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace hr {

class ByteWriter {
 public:
  void u8(std::uint8_t v) { data_.push_back(v); }
  void u16(std::uint16_t v) {
    data_.push_back(static_cast<std::uint8_t>(v));
    data_.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    data_.push_back(static_cast<std::uint8_t>(v));
    data_.push_back(static_cast<std::uint8_t>(v >> 8));
    data_.push_back(static_cast<std::uint8_t>(v >> 16));
    data_.push_back(static_cast<std::uint8_t>(v >> 24));
  }
  void u64(std::uint64_t v) {
    u32(static_cast<std::uint32_t>(v));
    u32(static_cast<std::uint32_t>(v >> 32));
  }
  void i8(std::int8_t v) { u8(static_cast<std::uint8_t>(v)); }
  void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
  void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
  void boolean(bool v) { u8(v ? 1 : 0); }

  void f32(float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    u32(bits);
  }
  void f64(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    u64(bits);
  }

  // Length-prefixed, u16 length. Long enough for any key, name or path the game
  // produces; a longer string is truncated rather than silently corrupting the
  // stream by writing a length that does not match the bytes.
  void str(std::string_view s) {
    const std::size_t n = s.size() > 0xFFFFu ? 0xFFFFu : s.size();
    u16(static_cast<std::uint16_t>(n));
    bytes(s.data(), n);
  }

  void bytes(const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    data_.insert(data_.end(), b, b + n);
  }

  std::size_t size() const { return data_.size(); }
  const std::vector<std::uint8_t>& data() const { return data_; }
  std::vector<std::uint8_t>& data() { return data_; }

  // Rewrites a u32 already written, for a length field placed before its payload.
  void patchU32(std::size_t offset, std::uint32_t v) {
    if (offset + 4 > data_.size()) return;
    data_[offset] = static_cast<std::uint8_t>(v);
    data_[offset + 1] = static_cast<std::uint8_t>(v >> 8);
    data_[offset + 2] = static_cast<std::uint8_t>(v >> 16);
    data_[offset + 3] = static_cast<std::uint8_t>(v >> 24);
  }

 private:
  std::vector<std::uint8_t> data_;
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
  explicit ByteReader(const std::vector<std::uint8_t>& v) : ByteReader(v.data(), v.size()) {}

  bool ok() const { return !failed_; }
  // Latches failure by hand, for a decoder that finds a value it cannot accept.
  void fail() { failed_ = true; }
  std::size_t remaining() const { return failed_ ? 0 : size_ - pos_; }
  std::size_t position() const { return pos_; }

  std::uint8_t u8() {
    if (!take(1)) return 0;
    return data_[pos_ - 1];
  }
  std::uint16_t u16() {
    if (!take(2)) return 0;
    const std::size_t p = pos_ - 2;
    return static_cast<std::uint16_t>(data_[p] | (data_[p + 1] << 8));
  }
  std::uint32_t u32() {
    if (!take(4)) return 0;
    const std::size_t p = pos_ - 4;
    return static_cast<std::uint32_t>(data_[p]) |
           (static_cast<std::uint32_t>(data_[p + 1]) << 8) |
           (static_cast<std::uint32_t>(data_[p + 2]) << 16) |
           (static_cast<std::uint32_t>(data_[p + 3]) << 24);
  }
  std::uint64_t u64() {
    const std::uint64_t lo = u32();
    const std::uint64_t hi = u32();
    return lo | (hi << 32);
  }
  std::int8_t i8() { return static_cast<std::int8_t>(u8()); }
  std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
  std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
  std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
  bool boolean() { return u8() != 0; }

  float f32() {
    const std::uint32_t bits = u32();
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof v);
    return v;
  }
  double f64() {
    const std::uint64_t bits = u64();
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof v);
    return v;
  }

  std::string str() {
    const std::size_t n = u16();
    if (!take(n)) return {};
    return std::string(reinterpret_cast<const char*>(data_ + pos_ - n), n);
  }

  bool bytes(void* out, std::size_t n) {
    if (!take(n)) return false;
    std::memcpy(out, data_ + pos_ - n, n);
    return true;
  }

  void skip(std::size_t n) { take(n); }

  // An element count that has to be believable: nothing in any format here encodes
  // in less than a byte, so a count past the end of the buffer is corruption and
  // fails the reader rather than reserving a gigabyte.
  std::uint32_t readCount() {
    const std::uint32_t n = u32();
    if (n > remaining()) {
      failed_ = true;
      return 0;
    }
    return n;
  }

  // A bounds-checked view of the next `n` bytes, which then become skipped. Used
  // for length-delimited sections: the sub-reader cannot run past its section even
  // if the section's own contents claim otherwise.
  ByteReader sub(std::size_t n) {
    if (!take(n)) return ByteReader(data_, 0);
    return ByteReader(data_ + pos_ - n, n);
  }

 private:
  bool take(std::size_t n) {
    if (failed_ || n > size_ - pos_) {
      failed_ = true;
      return false;
    }
    pos_ += n;
    return true;
  }

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
  bool failed_ = false;
};

// CRC-32 (IEEE 802.3, the zlib/PNG polynomial). Catches the corruption a length
// check cannot: a file whose bytes were mangled in place.
std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t seed = 0);

}  // namespace hr
