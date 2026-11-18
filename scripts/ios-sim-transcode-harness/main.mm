#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "PlatformImageTranscoder.hpp"

// Runs inside an iOS Simulator process (see scripts/test-ios-sim-transcode.sh).
// It links the real ios/NitroPdfWriterImageTranscoder.mm and the core, so what
// gets exercised is the same ImageIO code path an app binary would use.

using namespace margelo::nitro::pdfwriter;
using MediaPrep::ImageContainer;
using MediaPrep::TranscodeOptions;

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("  FAIL: %s\n", what);
    ++g_failures;
  }
}

static std::vector<uint8_t> readFile(const std::string& path) {
  NSData* data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];
  if (data == nil) {
    return {};
  }
  const uint8_t* bytes = (const uint8_t*)data.bytes;
  return std::vector<uint8_t>(bytes, bytes + data.length);
}

static const char* kindName(MediaBlob::Kind kind) {
  switch (kind) {
    case MediaBlob::Kind::Png: return "png";
    case MediaBlob::Kind::Jpeg: return "jpeg";
    case MediaBlob::Kind::Raw: return "raw";
    default: return "unknown";
  }
}

static bool isPngMagic(const std::vector<uint8_t>& bytes) {
  return bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
         bytes[3] == 'G';
}

static bool isJpegMagic(const std::vector<uint8_t>& bytes) {
  return bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
}

static void runCase(const std::string& dir, const char* file, ImageContainer expectedContainer,
                    uint32_t maxDimension, int quality, MediaBlob::Kind expectedKind,
                    uint32_t expectedWidth, uint32_t expectedHeight, bool expectedAlpha) {
  const std::string path = dir + "/" + file;
  const std::vector<uint8_t> source = readFile(path);
  std::printf("%s (container=%s, max=%u)\n", file, MediaPrep::containerName(expectedContainer),
              maxDimension);
  if (source.empty()) {
    std::printf("  FAIL: cannot read %s\n", path.c_str());
    ++g_failures;
    return;
  }
  check(MediaPrep::sniffImageContainer(source.data(), source.size()) == expectedContainer,
        "sniffed container");

  TranscodeOptions options;
  options.maxPixelDimension = maxDimension;
  options.jpegQuality = quality;

  try {
    const MediaPrep::TranscodedImage out =
        MediaPrep::transcodeToPdfImage(source.data(), source.size(), expectedContainer, options);
    std::printf("  -> %s %ux%u %zu bytes alpha=%s\n", kindName(out.kind), out.width, out.height,
                out.bytes.size(), out.hadAlpha ? "yes" : "no");
    check(out.kind == expectedKind, "output kind");
    check(out.width == expectedWidth, "output width");
    check(out.height == expectedHeight, "output height");
    check(out.hadAlpha == expectedAlpha, "alpha flag");
    check(out.kind == MediaBlob::Kind::Png ? isPngMagic(out.bytes) : isJpegMagic(out.bytes),
          "encoded magic matches kind");
    check(std::max(out.width, out.height) <= maxDimension, "long edge within bound");
  } catch (const std::exception& error) {
    std::printf("  FAIL: unexpected throw: %s\n", error.what());
    ++g_failures;
  }
}

static void runErrorCase() {
  const std::vector<uint8_t> garbage = {0x01, 0x02, 0x03, 0x04};
  std::printf("garbage payload (expect error)\n");
  try {
    MediaPrep::transcodeToPdfImage(garbage.data(), garbage.size(), ImageContainer::Heic);
    std::printf("  FAIL: expected an exception\n");
    ++g_failures;
  } catch (const std::exception& error) {
    std::printf("  -> threw: %s\n", error.what());
    check(std::strlen(error.what()) > 0, "error message is not empty");
  }
}

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc < 2) {
      std::printf("usage: harness <fixtures_dir>\n");
      return 2;
    }
    const std::string dir = argv[1];
    std::printf("platform transcoder registered: %s\n",
                MediaPrep::hasPlatformImageTranscoder() ? "yes" : "no");
    check(MediaPrep::hasPlatformImageTranscoder(), "iOS hook registered by constructor");

    // Same corpus the Android instrumentation tests use (android/src/androidTest/assets).
    runCase(dir, "heic_opaque_16x16.heic", ImageContainer::Heic, 4096, 85, MediaBlob::Kind::Jpeg,
            16, 16, false);
    runCase(dir, "heic_alpha_64x64.heic", ImageContainer::Heic, 4096, 85, MediaBlob::Kind::Png,
            64, 64, true);
    runCase(dir, "webp_opaque_16x16.webp", ImageContainer::Webp, 4096, 85, MediaBlob::Kind::Jpeg,
            16, 16, false);
    runCase(dir, "webp_alpha_64x64.webp", ImageContainer::Webp, 4096, 85, MediaBlob::Kind::Png,
            64, 64, true);
    // 12 MP (4032x3024) is already under 4096, so nothing is upscaled...
    runCase(dir, "heic_12mp_4032x3024.heic", ImageContainer::Heic, 4096, 85,
            MediaBlob::Kind::Jpeg, 4032, 3024, false);
    // ...and a tighter bound really downscales.
    runCase(dir, "heic_12mp_4032x3024.heic", ImageContainer::Heic, 2048, 85,
            MediaBlob::Kind::Jpeg, 2048, 1536, false);

    runErrorCase();

    std::printf("%s (%d failures)\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
  }
}
