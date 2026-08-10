// Turning a sound file into samples the mixer can play.
//
// The engine has never read a file: every sound in the game is synthesised from
// noise buffers and band-limited oscillators (see audio/dsp.h). A resource pack
// changes that, and the two formats that matter are decided for us:
//
//  * **Ogg Vorbis**, because that is what a Minecraft resource pack ships. A pack
//    downloaded from anywhere will be .ogg and nothing else, so without this the
//    format compatibility is theoretical. Decoded by stb_vorbis, vendored beside
//    the three other stb libraries this project already uses.
//  * **WAV**, because that is what somebody recording or exporting their own
//    sounds has on disk. Parsed here rather than pulled from a library: RIFF is
//    a header and a loop, and miniaudio's decoders are deliberately compiled out
//    (MA_NO_DECODING in CMakeLists.txt) precisely so that miniaudio stays a
//    device abstraction and nothing more.
//
// Everything comes back **mono**, whatever the file held. That is not laziness:
// the panner in dsp.h is a mono equal-power PannerNode, exactly as the web build's
// was, so a positional sound has to be one channel to be placed in the world at
// all. A stereo file played positionally would either be silently half-discarded
// or need a second panner nobody would hear the point of. Downmixing once, at
// load, makes it one code path and one documented answer.
//
// The source sample rate is kept rather than resampled here. Playback multiplies
// its read cursor by clipRate/deviceRate, which is the same mechanism the noise
// buffers already use for their `rate` parameter, so a 44.1 kHz file on a 48 kHz
// device costs one multiply rather than a resampling pass at load.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hr::audio {

// Refuses anything longer than this. A sound effect is under a second; a long
// ambience loop is a handful. The cap exists because a pack is a folder somebody
// downloaded, and "decode whatever is in it" with no ceiling is how one 400 MB
// file takes the game down.
inline constexpr double kMaxClipSeconds = 30.0;

struct DecodedAudio {
  std::vector<float> mono;
  int sampleRate = 0;
  // What the file actually held, for the pack report. Not used by playback.
  int sourceChannels = 0;

  bool empty() const { return mono.empty() || sampleRate <= 0; }
  double seconds() const {
    return sampleRate > 0 ? static_cast<double>(mono.size()) / sampleRate : 0.0;
  }
};

// Detects the format from the bytes rather than the extension. A pack with a
// .ogg that is really a WAV still loads, and — the case that actually matters —
// a file that is neither gets one clear message instead of a confusing failure
// deep inside the wrong decoder.
bool decodeAudio(const std::uint8_t* bytes, std::size_t size, DecodedAudio& out,
                 std::string* errorOut = nullptr);

bool decodeAudioFile(const std::string& path, DecodedAudio& out, std::string* errorOut = nullptr);

// Exposed individually so the self-test can feed each decoder a buffer it built
// itself, rather than needing a fixture file on disk.
bool decodeWav(const std::uint8_t* bytes, std::size_t size, DecodedAudio& out,
               std::string* errorOut = nullptr);
bool decodeOggVorbis(const std::uint8_t* bytes, std::size_t size, DecodedAudio& out,
                     std::string* errorOut = nullptr);

// A 16-bit PCM WAV of `samples`, for the self-test and for --dump-sound. Writing
// one is four lines of header, and it means the decoder can be tested against
// bytes this file produced rather than against a binary blob checked into git.
std::vector<std::uint8_t> encodeWav16(const std::vector<float>& mono, int sampleRate);

}  // namespace hr::audio
