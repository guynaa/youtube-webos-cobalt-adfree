// Host test for starfish_fps_hint.h. From the repository root:
//   c++ -std=c++14 -Wall -Wextra -Werror -I cobalt-platform/webos/arm
//       scripts/test-starfish-fps-hint.cc -o /tmp/fps-hint && /tmp/fps-hint
#include "starfish_fps_hint.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace starboard::shared::webos;

static int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,     \
                   __LINE__, #cond);                                  \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

static bool Near(double a, double b) { return a > b - 0.001 && a < b + 0.001; }

int main() {
  // Variant parsing: unknown bytes fall back to the current payload.
  CHECK(ParseFpsHintVariant('0') == FpsHintVariant::kCurrent);
  CHECK(ParseFpsHintVariant('1') == FpsHintVariant::kRealFps);
  CHECK(ParseFpsHintVariant('2') == FpsHintVariant::kOmitFpsRealMax);
  CHECK(ParseFpsHintVariant('3') == FpsHintVariant::kOmitFps);
  CHECK(ParseFpsHintVariant('4') == FpsHintVariant::kStockEsInfo);
  CHECK(ParseFpsHintVariant('x') == FpsHintVariant::kCurrent);
  CHECK(ParseFpsHintVariant(EOF) == FpsHintVariant::kCurrent);

  // Capability strings in YouTube's format.
  CHECK(Near(FrameRateFromCapabilities(
                 "width=1920; height=1080; framerate=25; bitrate=9026934"),
             25));
  CHECK(Near(FrameRateFromCapabilities("framerate=29.97"), 29.97));
  CHECK(FrameRateFromCapabilities("width=1920; height=1080") == 0);
  CHECK(FrameRateFromCapabilities("") == 0);
  CHECK(FrameRateFromCapabilities(nullptr) == 0);
  CHECK(FrameRateFromCapabilities("framerate=0") == 0);
  CHECK(FrameRateFromCapabilities("framerate=9999") == 0);

  // Timestamps: 25 fps, in order.
  std::vector<int64_t> pts;
  for (int i = 0; i < 10; ++i) pts.push_back(i * 40000);
  CHECK(Near(FrameRateFromTimestamps(pts), 25));
  // B-frame order (I P B B ...) and a duplicate still give 25.
  CHECK(Near(FrameRateFromTimestamps({0, 120000, 40000, 80000, 240000, 160000,
                                      200000, 200000}),
             25));
  // A seek gap and one missing frame do not move the median.
  CHECK(Near(FrameRateFromTimestamps({0, 40000, 80000, 120000, 160000,
                                      9000000, 9040000, 9120000}),
             25));
  // 59.94 fps.
  pts.clear();
  for (int i = 0; i < 8; ++i) pts.push_back(i * 16683);
  CHECK(FrameRateFromTimestamps(pts) > 59.9 &&
        FrameRateFromTimestamps(pts) < 60.0);
  // Too few samples.
  CHECK(FrameRateFromTimestamps({0, 40000, 80000}) == 0);
  CHECK(FrameRateFromTimestamps({}) == 0);

  // Ratios.
  CHECK(ToFpsRatio(25).value == 25 && ToFpsRatio(25).scale == 1);
  CHECK(ToFpsRatio(23.976).value == 24000 && ToFpsRatio(23.976).scale == 1001);
  CHECK(ToFpsRatio(59.94).value == 60000 && ToFpsRatio(59.94).scale == 1001);
  CHECK(ToFpsRatio(12.5).value == 12500 && ToFpsRatio(12.5).scale == 1000);
  CHECK(ToFpsRatio(0).value == 60 && ToFpsRatio(0).scale == 1);

  // maxFrameRate.
  CHECK(MaxFrameRateHint(FpsHintVariant::kCurrent, 25) == 60);
  CHECK(MaxFrameRateHint(FpsHintVariant::kOmitFps, 25) == 60);
  CHECK(MaxFrameRateHint(FpsHintVariant::kRealFps, 25) == 25);
  CHECK(MaxFrameRateHint(FpsHintVariant::kOmitFpsRealMax, 29.97) == 30);
  CHECK(MaxFrameRateHint(FpsHintVariant::kStockEsInfo, 59.94) == 60);
  CHECK(MaxFrameRateHint(FpsHintVariant::kRealFps, 0) == 60);

  // esInfo objects. Variant 0 must equal the v2.0.1 payload byte for byte.
  CHECK(BuildEsInfo(FpsHintVariant::kCurrent, 25, 1000, 1920, 1080) ==
        "{\"pauseAtDecodeTime\":false,\"seperatedPTS\":true,\"ptsToDecode\":1000,"
        "\"videoWidth\":1920,\"videoHeight\":1080,\"videoFpsValue\":60,"
        "\"videoFpsScale\":1}");
  CHECK(BuildEsInfo(FpsHintVariant::kRealFps, 23.976, 0, 1920, 1080) ==
        "{\"pauseAtDecodeTime\":false,\"seperatedPTS\":true,\"ptsToDecode\":0,"
        "\"videoWidth\":1920,\"videoHeight\":1080,\"videoFpsValue\":24000,"
        "\"videoFpsScale\":1001}");
  CHECK(BuildEsInfo(FpsHintVariant::kOmitFpsRealMax, 25, 0, 854, 480) ==
        "{\"pauseAtDecodeTime\":false,\"seperatedPTS\":true,\"ptsToDecode\":0,"
        "\"videoWidth\":854,\"videoHeight\":480}");
  CHECK(BuildEsInfo(FpsHintVariant::kOmitFps, 25, 0, 854, 480) ==
        BuildEsInfo(FpsHintVariant::kOmitFpsRealMax, 25, 0, 854, 480));
  CHECK(BuildEsInfo(FpsHintVariant::kStockEsInfo, 25, 9223372036854775807LL,
                    3840, 2160) ==
        "{\"seperatedPTS\":true,\"ptsToDecode\":9223372036854775807}");

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::puts("fps-hint: all checks passed");
  return EXIT_SUCCESS;
}
