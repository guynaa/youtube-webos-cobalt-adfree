#ifndef STARBOARD_WEBOS_ARM_STARFISH_FPS_HINT_H_
#define STARBOARD_WEBOS_ARM_STARFISH_FPS_HINT_H_

// EXPERIMENT: which frame-rate hint the shared Starfish Load payload sends.
//
// On an LG C5 with webOS 11.2 (firmware 43.21.96), every playback rate other
// than 1x visibly drops frames in the display plane (0 dropped frames in the
// web player), while LG's own lm21u starter on the same TV is smooth at the
// same rates. Both use SetPlayRate with the same trick-play options. The
// visible payload difference: this backend sends esInfo videoFpsValue 60/1 and
// adaptiveStreaming maxFrameRate 60 for every stream; the stock starter sends
// no esInfo frame rate at all and derives maxFrameRate from the stream.
//
// The variant is chosen per process from $YTAF_FPS_HINT or the first byte of
// <app dir>/ytaf-fps-hint (webOS 11.2 gives Cobalt a private /tmp, so /tmp
// markers are unreachable for developers). Missing or unknown = current.
//
// Self-contained (no Starboard dependencies) so it can be unit-tested on the
// host: scripts/test-starfish-fps-hint.cc.

#include <limits.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace starboard {
namespace shared {
namespace webos {

enum class FpsHintVariant {
  kCurrent = 0,         // esInfo 60/1, maxFrameRate 60 (v2.0.1 behaviour)
  kRealFps = 1,         // esInfo real value/scale, maxFrameRate real
  kOmitFpsRealMax = 2,  // no esInfo frame rate, maxFrameRate real
  kOmitFps = 3,         // no esInfo frame rate, maxFrameRate 60
  kStockEsInfo = 4,     // esInfo only seperatedPTS + ptsToDecode, maxFrameRate real
};

inline FpsHintVariant ParseFpsHintVariant(int c) {
  switch (c) {
    case '1': return FpsHintVariant::kRealFps;
    case '2': return FpsHintVariant::kOmitFpsRealMax;
    case '3': return FpsHintVariant::kOmitFps;
    case '4': return FpsHintVariant::kStockEsInfo;
    default: return FpsHintVariant::kCurrent;
  }
}

// Directory holding the running executable, without a trailing slash.
inline std::string ExecutableDirectory() {
  char path[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n <= 0) return std::string();
  path[n] = '\0';
  char* slash = std::strrchr(path, '/');
  if (!slash) return std::string();
  *slash = '\0';
  return std::string(path);
}

inline FpsHintVariant ReadConfiguredFpsHintVariant() {
  const char* env = std::getenv("YTAF_FPS_HINT");
  if (env && env[0]) return ParseFpsHintVariant(env[0]);
  const std::string dir = ExecutableDirectory();
  if (dir.empty()) return FpsHintVariant::kCurrent;
  FILE* marker = std::fopen((dir + "/ytaf-fps-hint").c_str(), "r");
  if (!marker) return FpsHintVariant::kCurrent;
  const int c = std::fgetc(marker);
  std::fclose(marker);
  return ParseFpsHintVariant(c);
}

// Latched on first use so every player in a process uses one policy.
inline FpsHintVariant ConfiguredFpsHintVariant() {
  static const FpsHintVariant variant = ReadConfiguredFpsHintVariant();
  return variant;
}

inline bool PlausibleFrameRate(double fps) {
  return std::isfinite(fps) && fps >= 1.0 && fps <= 240.0;
}

// "...; framerate=25; ..." -> 25. Returns 0 when absent or implausible.
inline double FrameRateFromCapabilities(const char* capabilities) {
  if (!capabilities) return 0;
  const char* key = std::strstr(capabilities, "framerate=");
  if (!key) return 0;
  const double fps = std::strtod(key + std::strlen("framerate="), nullptr);
  return PlausibleFrameRate(fps) ? fps : 0;
}

// Median spacing of distinct presentation timestamps (microseconds). Sorting
// first makes it robust to B-frame reordering; the median ignores a seek gap
// or a missing frame. Needs at least 4 distinct timestamps, else returns 0.
inline double FrameRateFromTimestamps(std::vector<int64_t> pts_us) {
  std::sort(pts_us.begin(), pts_us.end());
  pts_us.erase(std::unique(pts_us.begin(), pts_us.end()), pts_us.end());
  if (pts_us.size() < 4) return 0;
  std::vector<int64_t> gaps;
  for (size_t i = 1; i < pts_us.size(); ++i)
    gaps.push_back(pts_us[i] - pts_us[i - 1]);
  std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
  const int64_t median = gaps[gaps.size() / 2];
  if (median <= 0) return 0;
  const double fps = 1e6 / static_cast<double>(median);
  return PlausibleFrameRate(fps) ? fps : 0;
}

struct FpsRatio {
  int value;
  int scale;
};

// Exact ratio for the NTSC rates (24000/1001 ...), integer rates otherwise.
inline FpsRatio ToFpsRatio(double fps) {
  if (!PlausibleFrameRate(fps)) return {60, 1};
  const int ntsc[] = {24, 30, 48, 60, 120};
  for (int n : ntsc) {
    if (std::fabs(fps - n * 1000.0 / 1001.0) < 0.01) return {n * 1000, 1001};
  }
  const double rounded = std::round(fps);
  if (std::fabs(fps - rounded) < 0.05) return {static_cast<int>(rounded), 1};
  return {static_cast<int>(std::lround(fps * 1000.0)), 1000};
}

inline int MaxFrameRateHint(FpsHintVariant variant, double fps) {
  if (variant == FpsHintVariant::kCurrent ||
      variant == FpsHintVariant::kOmitFps || !PlausibleFrameRate(fps)) {
    return 60;
  }
  return static_cast<int>(std::ceil(fps - 0.01));
}

// The complete "esInfo" JSON object for the Load payload.
inline std::string BuildEsInfo(FpsHintVariant variant,
                               double fps,
                               long long pts_to_decode_ns,
                               int width,
                               int height) {
  char out[256];
  if (variant == FpsHintVariant::kStockEsInfo) {
    std::snprintf(out, sizeof(out),
                  "{\"seperatedPTS\":true,\"ptsToDecode\":%lld}",
                  pts_to_decode_ns);
    return out;
  }
  const int written = std::snprintf(
      out, sizeof(out),
      "{\"pauseAtDecodeTime\":false,\"seperatedPTS\":true,\"ptsToDecode\":%lld,"
      "\"videoWidth\":%d,\"videoHeight\":%d",
      pts_to_decode_ns, width, height);
  std::string es_info(out, written > 0 ? static_cast<size_t>(written) : 0);
  if (variant == FpsHintVariant::kCurrent) {
    es_info += ",\"videoFpsValue\":60,\"videoFpsScale\":1";
  } else if (variant == FpsHintVariant::kRealFps) {
    const FpsRatio ratio = ToFpsRatio(fps);
    std::snprintf(out, sizeof(out), ",\"videoFpsValue\":%d,\"videoFpsScale\":%d",
                  ratio.value, ratio.scale);
    es_info += out;
  }
  es_info += "}";
  return es_info;
}

}  // namespace webos
}  // namespace shared
}  // namespace starboard

#endif  // STARBOARD_WEBOS_ARM_STARFISH_FPS_HINT_H_
