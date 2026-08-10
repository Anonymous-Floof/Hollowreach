#include "audio/decode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

// Declared rather than included. stb_vorbis.c is compiled as its own C
// translation unit (audio/stb_vorbis_impl.c) with warnings off, the same
// arrangement miniaudio already has; pulling the header half of a .c file into
// C++ to get one prototype would buy nothing.
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels,
                                        int* sample_rate, short** output);

namespace hr::audio {
namespace {

// Every multi-byte field in RIFF is little-endian regardless of the host, so
// they are assembled from bytes rather than memcpy'd over a struct.
std::uint16_t readU16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t readU32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool tagIs(const std::uint8_t* p, const char* tag) { return std::memcmp(p, tag, 4) == 0; }

bool fail(std::string* errorOut, const char* message) {
  if (errorOut) *errorOut = message;
  return false;
}

// WAVE_FORMAT_*, from mmreg.h. Only three of the dozens matter: everything a
// recorder or an exporter writes is integer PCM, IEEE float, or extensible
// (which is a wrapper naming one of the other two in a GUID).
constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

constexpr int kMaxChannels = 32;

}  // namespace

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

bool decodeWav(const std::uint8_t* bytes, std::size_t size, DecodedAudio& out,
               std::string* errorOut) {
  out = DecodedAudio{};
  if (size < 12 || !tagIs(bytes, "RIFF") || !tagIs(bytes + 8, "WAVE")) {
    return fail(errorOut, "not a RIFF/WAVE file");
  }

  std::uint16_t format = 0, channels = 0, bits = 0;
  std::uint32_t rate = 0;
  const std::uint8_t* data = nullptr;
  std::size_t dataSize = 0;
  bool haveFmt = false;

  // Chunks are walked rather than assumed to be in order. "fmt " before "data"
  // is the convention, not the rule, and files carrying LIST/INFO metadata
  // between them are ordinary — a parser that reads a fixed 44-byte header
  // rejects perfectly good files from half the editors in existence.
  std::size_t pos = 12;
  while (pos + 8 <= size) {
    const std::uint8_t* id = bytes + pos;
    const std::uint32_t chunkSize = readU32(bytes + pos + 4);
    const std::size_t body = pos + 8;
    // Guard before trusting the declared size: a truncated file (or a hostile
    // one) can claim a chunk that runs past the end of the buffer.
    if (chunkSize > size - body) break;

    if (tagIs(id, "fmt ") && chunkSize >= 16) {
      format = readU16(bytes + body + 0);
      channels = readU16(bytes + body + 2);
      rate = readU32(bytes + body + 4);
      bits = readU16(bytes + body + 14);
      // WAVE_FORMAT_EXTENSIBLE's real format is the first two bytes of the
      // SubFormat GUID at offset 24. Without this, every 24-bit file from a
      // modern recorder — which is what extensible exists for — is rejected as
      // an unknown format 0xFFFE.
      if (format == kFormatExtensible && chunkSize >= 40) {
        format = readU16(bytes + body + 24);
      }
      haveFmt = true;
    } else if (tagIs(id, "data")) {
      data = bytes + body;
      dataSize = chunkSize;
    }

    pos = body + chunkSize + (chunkSize & 1u);  // chunks are word-aligned
  }

  if (!haveFmt) return fail(errorOut, "no fmt chunk");
  if (!data || dataSize == 0) return fail(errorOut, "no data chunk");
  if (channels == 0 || channels > kMaxChannels) return fail(errorOut, "unsupported channel count");
  if (rate < 1000 || rate > 384000) return fail(errorOut, "unsupported sample rate");

  const int bytesPerSample = bits / 8;
  if (bytesPerSample <= 0) return fail(errorOut, "unsupported bit depth");
  if (format == kFormatPcm && bits != 8 && bits != 16 && bits != 24 && bits != 32) {
    return fail(errorOut, "unsupported PCM bit depth");
  }
  if (format == kFormatFloat && bits != 32 && bits != 64) {
    return fail(errorOut, "unsupported float bit depth");
  }
  if (format != kFormatPcm && format != kFormatFloat) {
    return fail(errorOut, "unsupported WAV encoding (not PCM or float)");
  }

  const std::size_t frameBytes = static_cast<std::size_t>(bytesPerSample) * channels;
  const std::size_t frames = dataSize / frameBytes;
  if (frames == 0) return fail(errorOut, "no audio frames");
  if (static_cast<double>(frames) / rate > kMaxClipSeconds) {
    return fail(errorOut, "clip is longer than the 30 second limit");
  }

  out.mono.resize(frames);
  out.sampleRate = static_cast<int>(rate);
  out.sourceChannels = channels;

  const float invChannels = 1.0f / static_cast<float>(channels);
  for (std::size_t f = 0; f < frames; ++f) {
    const std::uint8_t* frame = data + f * frameBytes;
    float sum = 0.0f;
    for (int c = 0; c < channels; ++c) {
      const std::uint8_t* s = frame + static_cast<std::size_t>(c) * bytesPerSample;
      float v = 0.0f;
      if (format == kFormatFloat) {
        if (bits == 32) {
          std::uint32_t raw = readU32(s);
          float f32;
          std::memcpy(&f32, &raw, 4);
          v = f32;
        } else {
          std::uint64_t raw = 0;
          for (int i = 7; i >= 0; --i) raw = (raw << 8) | s[i];
          double f64;
          std::memcpy(&f64, &raw, 8);
          v = static_cast<float>(f64);
        }
      } else if (bits == 8) {
        // 8-bit WAV is UNSIGNED with 128 as silence — the one depth that is not
        // two's complement. Reading it as signed puts a DC offset of half full
        // scale on the clip, which sounds like a click and then nothing.
        v = (static_cast<int>(s[0]) - 128) / 128.0f;
      } else if (bits == 16) {
        v = static_cast<std::int16_t>(readU16(s)) / 32768.0f;
      } else if (bits == 24) {
        std::int32_t raw = static_cast<std::int32_t>(s[0]) | (static_cast<std::int32_t>(s[1]) << 8) |
                           (static_cast<std::int32_t>(s[2]) << 16);
        if (raw & 0x800000) raw |= ~0xFFFFFF;  // sign-extend the 24th bit
        v = static_cast<float>(raw) / 8388608.0f;
      } else {
        v = static_cast<float>(static_cast<std::int32_t>(readU32(s))) / 2147483648.0f;
      }
      sum += v;
    }
    out.mono[f] = sum * invChannels;
  }
  if (errorOut) errorOut->clear();
  return true;
}

// ---------------------------------------------------------------------------
// Ogg Vorbis
// ---------------------------------------------------------------------------

bool decodeOggVorbis(const std::uint8_t* bytes, std::size_t size, DecodedAudio& out,
                     std::string* errorOut) {
  out = DecodedAudio{};
  if (size < 4 || std::memcmp(bytes, "OggS", 4) != 0) return fail(errorOut, "not an Ogg stream");
  // stb_vorbis takes an int length, and the cast is only safe because a clip is
  // capped at 30 seconds long — but the cap is enforced on the DECODED length,
  // after this call. Check the compressed size too, so a 3 GB file cannot wrap
  // the length negative before the decoder ever sees how long it is.
  if (size > 256u * 1024u * 1024u) return fail(errorOut, "ogg file is implausibly large");

  int channels = 0;
  int rate = 0;
  short* samples = nullptr;
  const int frames =
      stb_vorbis_decode_memory(bytes, static_cast<int>(size), &channels, &rate, &samples);
  if (frames <= 0 || !samples) {
    if (samples) std::free(samples);
    return fail(errorOut, "not a readable Ogg Vorbis stream");
  }
  if (channels <= 0 || channels > kMaxChannels || rate < 1000 || rate > 384000) {
    std::free(samples);
    return fail(errorOut, "unsupported Ogg Vorbis stream");
  }
  if (static_cast<double>(frames) / rate > kMaxClipSeconds) {
    std::free(samples);
    return fail(errorOut, "clip is longer than the 30 second limit");
  }

  out.mono.resize(static_cast<std::size_t>(frames));
  out.sampleRate = rate;
  out.sourceChannels = channels;
  const float invChannels = 1.0f / static_cast<float>(channels);
  for (int f = 0; f < frames; ++f) {
    int sum = 0;
    for (int c = 0; c < channels; ++c) sum += samples[static_cast<std::size_t>(f) * channels + c];
    out.mono[static_cast<std::size_t>(f)] = (sum * invChannels) / 32768.0f;
  }
  // stb_vorbis allocates with malloc and documents free() as the counterpart.
  std::free(samples);
  if (errorOut) errorOut->clear();
  return true;
}

// ---------------------------------------------------------------------------

bool decodeAudio(const std::uint8_t* bytes, std::size_t size, DecodedAudio& out,
                 std::string* errorOut) {
  if (size >= 4 && std::memcmp(bytes, "OggS", 4) == 0) {
    return decodeOggVorbis(bytes, size, out, errorOut);
  }
  if (size >= 4 && std::memcmp(bytes, "RIFF", 4) == 0) {
    return decodeWav(bytes, size, out, errorOut);
  }
  // Named explicitly, because the two formats a pack author is most likely to
  // reach for by mistake both fail here and the reason is not obvious from
  // "unrecognised": MP3 has no magic worth trusting and FLAC is a different
  // codec entirely.
  if (size >= 4 && std::memcmp(bytes, "fLaC", 4) == 0) {
    return fail(errorOut, "FLAC is not supported — convert to .ogg or .wav");
  }
  if (size >= 3 && (std::memcmp(bytes, "ID3", 3) == 0 ||
                    (bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0))) {
    return fail(errorOut, "MP3 is not supported — convert to .ogg or .wav");
  }
  return fail(errorOut, "unrecognised audio format (expected .ogg or .wav)");
}

bool decodeAudioFile(const std::string& path, DecodedAudio& out, std::string* errorOut) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (errorOut) *errorOut = "cannot open " + path;
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string blob = buffer.str();
  if (blob.empty()) {
    if (errorOut) *errorOut = path + ": empty file";
    return false;
  }
  std::string error;
  if (!decodeAudio(reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size(), out, &error)) {
    if (errorOut) *errorOut = path + ": " + error;
    return false;
  }
  if (errorOut) errorOut->clear();
  return true;
}

// ---------------------------------------------------------------------------

std::vector<std::uint8_t> encodeWav16(const std::vector<float>& mono, int sampleRate) {
  const std::uint32_t frames = static_cast<std::uint32_t>(mono.size());
  const std::uint32_t dataBytes = frames * 2;
  std::vector<std::uint8_t> out;
  out.reserve(44 + dataBytes);

  auto put32 = [&out](std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
  };
  auto put16 = [&out](std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
  };
  auto tag = [&out](const char* s) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(s[i]));
  };

  tag("RIFF");
  put32(36 + dataBytes);
  tag("WAVE");
  tag("fmt ");
  put32(16);
  put16(kFormatPcm);
  put16(1);                                          // mono
  put32(static_cast<std::uint32_t>(sampleRate));
  put32(static_cast<std::uint32_t>(sampleRate) * 2);  // byte rate
  put16(2);                                           // block align
  put16(16);                                          // bits
  tag("data");
  put32(dataBytes);
  for (float v : mono) {
    const float clamped = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    put16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(clamped * 32767.0f))));
  }
  return out;
}

}  // namespace hr::audio
