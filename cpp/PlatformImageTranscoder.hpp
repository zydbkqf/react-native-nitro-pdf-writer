#pragma once

#include "MediaCache.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace margelo::nitro::pdfwriter {

namespace MediaPrep {

/**
 * Image containers we can recognise from the byte stream.
 *
 * Only PNG / JPEG / raw pixels can be embedded in PDF directly, so anything
 * else has to be decoded and re-encoded first. That work needs the OS decoder
 * (ImageIO on iOS, BitmapFactory/ImageDecoder on Android) and therefore lives
 * behind the platform hook declared below — the core stays pure C++.
 */
enum class ImageContainer : uint8_t { Unknown, Png, Jpeg, Heic, Webp, Avif };

/** Sniff PNG / JPEG / HEIC(HEIF) / WebP / AVIF from magic bytes. */
ImageContainer sniffImageContainer(const uint8_t* data, size_t size);

/** Short lowercase name, e.g. "heic"; used in error messages and cache keys. */
const char* containerName(ImageContainer container);

/** `image/heic` style mime type handed to the platform decoder. */
const char* containerMime(ImageContainer container);

/**
 * Default downscale bound for HEIC/WebP decoding.
 *
 * A 12 MP phone photo is 48 MB as RGBA, and if it ends up in the PDF as a
 * Flate stream the file balloons the same way. A4 at 300 dpi is ~2480 px, so
 * 4096 keeps quality headroom while bounding both memory and output size.
 */
constexpr uint32_t kDefaultMaxPixelDimension = 4096;
constexpr int kDefaultJpegQuality = 85;

struct TranscodeOptions {
  uint32_t maxPixelDimension = kDefaultMaxPixelDimension;
  int jpegQuality = kDefaultJpegQuality;
};

/** Result of decoding a HEIC/WebP source into something PDF can embed. */
struct TranscodedImage {
  std::vector<uint8_t> bytes;
  MediaBlob::Kind kind = MediaBlob::Kind::Png;
  uint32_t width = 0;
  uint32_t height = 0;
  /** True when the source carried alpha; the payload is then PNG (SMask source). */
  bool hadAlpha = false;
};

/**
 * Platform decoder contract. An implementation must:
 *  - bake EXIF orientation into the pixels (PDF image XObjects carry none, so
 *    a portrait photo would otherwise land sideways),
 *  - downscale so that max(width, height) <= options.maxPixelDimension,
 *  - encode PNG when the source has alpha, JPEG (options.jpegQuality) otherwise,
 *  - report unsupported *devices* through `error` instead of crashing — that is
 *    how Android API 24-27 rejects HEIC with a readable message,
 *  - never throw C++ exceptions across the bridge (Objective-C++ / JNI).
 *
 * `bytes`/`width`/`height` are only meaningful when the function returns true.
 */
using TranscodeFn = bool (*)(const uint8_t* data, size_t size, ImageContainer container,
                             const TranscodeOptions& options, TranscodedImage& out,
                             std::string& error);

/** Registered once by the platform bridge (constructor on iOS, class init on Android). */
void setPlatformImageTranscoder(TranscodeFn fn) noexcept;
void clearPlatformImageTranscoder() noexcept;
bool hasPlatformImageTranscoder() noexcept;

/**
 * Cache-key tag covering everything that changes the transcoded payload:
 * container, pixel bound and JPEG quality. A HEIC re-encoded at a different
 * size must not reuse an older cache entry.
 */
std::string transcodeKeyTag(ImageContainer container, const TranscodeOptions& options);

/**
 * Decode + re-encode, or throw std::runtime_error carrying the platform message.
 * Throws when no platform decoder is registered (tests, desktop builds).
 */
TranscodedImage transcodeToPdfImage(const uint8_t* data, size_t size, ImageContainer container,
                                    const TranscodeOptions& options = {});

} // namespace MediaPrep

} // namespace margelo::nitro::pdfwriter
