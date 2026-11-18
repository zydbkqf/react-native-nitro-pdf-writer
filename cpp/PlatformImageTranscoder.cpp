#include "PlatformImageTranscoder.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include <atomic>
#include <cstring>
#include <stdexcept>

#if defined(__APPLE__) && TARGET_OS_IPHONE
// Strong symbol lives in ios/NitroPdfWriterImageTranscoder.mm. Declared weak so
// a build that happens to omit that file still links; iOS targets always compile
// it, while macOS unit tests see TARGET_OS_IPHONE == 0 and never reference it.
extern "C" void NitroPdfWriterEnsurePlatformImageTranscoder(void) __attribute__((weak));
#define NPDF_HAS_IOS_TRANSCODER 1
#else
#define NPDF_HAS_IOS_TRANSCODER 0
#endif

namespace margelo::nitro::pdfwriter {

namespace MediaPrep {

namespace {

std::atomic<TranscodeFn> g_transcoder{nullptr};

bool startsWith(const uint8_t* data, size_t size, const char* text) {
  const size_t n = std::strlen(text);
  return size >= n && std::memcmp(data, text, n) == 0;
}

uint32_t readBigEndian32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/**
 * ISO-BMFF `ftyp`: box size(4) 'ftyp' major_brand(4) minor_version(4)
 * compatible_brands[]. The major brand alone is not enough — AVIF is routinely
 * written as major=mif1 with "avif" only in the compatible list, and mif1 is
 * also the HEIF brand.
 */
bool ftypHasBrand(const uint8_t* data, size_t size, const char* brand) {
  if (size < 12 || std::memcmp(data + 4, "ftyp", 4) != 0) {
    return false;
  }
  const uint32_t boxSize = readBigEndian32(data);
  // Trust the box header only when it fits inside the buffer we were given.
  const size_t limit = (boxSize >= 12 && boxSize <= size) ? static_cast<size_t>(boxSize) : size;
  if (std::memcmp(data + 8, brand, 4) == 0) {
    return true;
  }
  for (size_t offset = 16; offset + 4 <= limit; offset += 4) {
    if (std::memcmp(data + offset, brand, 4) == 0) {
      return true;
    }
  }
  return false;
}

} // namespace

ImageContainer sniffImageContainer(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 3) {
    return ImageContainer::Unknown;
  }

  static const uint8_t pngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (size >= 8 && std::memcmp(data, pngSig, 8) == 0) {
    return ImageContainer::Png;
  }
  // JPEG: SOI followed by any marker (FFD8FF). Some encoders emit FFD8FF only.
  if (data[0] == 0xFF && data[1] == 0xD8 && (size < 3 || data[2] == 0xFF)) {
    return ImageContainer::Jpeg;
  }
  if (size >= 12 && startsWith(data, size, "RIFF") && std::memcmp(data + 8, "WEBP", 4) == 0) {
    return ImageContainer::Webp;
  }

  // HEIF family (HEIC/HEIF) and AVIF both use the ISO-BMFF ftyp box.
  if (size >= 12 && std::memcmp(data + 4, "ftyp", 4) == 0) {
    // AVIF first: it is often major=mif1 with "avif" only among compatible
    // brands, and mif1 would otherwise send it down the HEIC path.
    if (ftypHasBrand(data, size, "avif") || ftypHasBrand(data, size, "avis")) {
      return ImageContainer::Avif;
    }
    static const char* const kHeifBrands[] = {"heic", "heix", "hevc", "hevx",
                                              "heim", "heis"};
    for (const char* brand : kHeifBrands) {
      if (ftypHasBrand(data, size, brand)) {
        return ImageContainer::Heic;
      }
    }
    // Generic HEIF brands: only reachable when no specific brand matched.
    if (ftypHasBrand(data, size, "mif1") || ftypHasBrand(data, size, "msf1")) {
      return ImageContainer::Heic;
    }
  }

  return ImageContainer::Unknown;
}

const char* containerName(ImageContainer container) {
  switch (container) {
    case ImageContainer::Png:
      return "png";
    case ImageContainer::Jpeg:
      return "jpeg";
    case ImageContainer::Heic:
      return "heic";
    case ImageContainer::Webp:
      return "webp";
    case ImageContainer::Avif:
      return "avif";
    default:
      return "unknown";
  }
}

const char* containerMime(ImageContainer container) {
  switch (container) {
    case ImageContainer::Png:
      return "image/png";
    case ImageContainer::Jpeg:
      return "image/jpeg";
    case ImageContainer::Heic:
      return "image/heic";
    case ImageContainer::Webp:
      return "image/webp";
    case ImageContainer::Avif:
      return "image/avif";
    default:
      return "application/octet-stream";
  }
}

void setPlatformImageTranscoder(TranscodeFn fn) noexcept {
  g_transcoder.store(fn, std::memory_order_release);
}

void clearPlatformImageTranscoder() noexcept {
  g_transcoder.store(nullptr, std::memory_order_release);
}

bool hasPlatformImageTranscoder() noexcept {
  return g_transcoder.load(std::memory_order_acquire) != nullptr;
}

std::string transcodeKeyTag(ImageContainer container, const TranscodeOptions& options) {
  return std::string("transcode-") + containerName(container) + "-max" +
         std::to_string(options.maxPixelDimension) + "-q" +
         std::to_string(options.jpegQuality);
}

TranscodedImage transcodeToPdfImage(const uint8_t* data, size_t size, ImageContainer container,
                                    const TranscodeOptions& options) {
  if (container != ImageContainer::Heic && container != ImageContainer::Webp &&
      container != ImageContainer::Avif) {
    throw std::invalid_argument(
        std::string("Image transcoder does not handle ") + containerName(container) + " input");
  }
  if (data == nullptr || size == 0) {
    throw std::invalid_argument(std::string("Empty ") + containerName(container) + " payload");
  }

  // Static-link safety net: the iOS bridge normally registers itself from
  // +load / __attribute__((constructor)), but a linker that drops the archive
  // member (static framework without -ObjC) would leave the hook null. The
  // declaration is weak, so this stays a no-op on desktop/test builds that
  // never compile the Objective-C++ file.
#if NPDF_HAS_IOS_TRANSCODER
  if (g_transcoder.load(std::memory_order_acquire) == nullptr &&
      NitroPdfWriterEnsurePlatformImageTranscoder != nullptr) {
    NitroPdfWriterEnsurePlatformImageTranscoder();
  }
#endif

  TranscodeFn fn = g_transcoder.load(std::memory_order_acquire);
  if (fn == nullptr) {
    throw std::runtime_error(std::string(containerName(container)) +
                             " images require the platform decoder, which is not registered in "
                             "this build (iOS/Android only)");
  }

  TranscodedImage out;
  std::string error;
  bool ok = fn(data, size, container, options, out, error);
  if (!ok) {
    const std::string detail =
        error.empty()
            ? std::string("Failed to decode ") + containerName(container) + " image"
            : error;
    throw std::runtime_error(detail);
  }
  if (out.bytes.empty()) {
    throw std::runtime_error(std::string("Platform decoder produced no data for ") +
                             containerName(container) + " image");
  }
  // Only encoded payloads are usable here: the caller parses PNG/JPEG headers to
  // recover geometry, so a Raw result would slip past parseImageMeta with
  // unvalidated dimensions. No in-tree decoder returns Raw, but the contract is
  // enforced rather than assumed.
  if (out.kind != MediaBlob::Kind::Png && out.kind != MediaBlob::Kind::Jpeg) {
    throw std::runtime_error(std::string("Platform decoder returned an unsupported payload kind for ") +
                             containerName(container) + " image (expected a PNG or JPEG payload)");
  }
  return out;
}

} // namespace MediaPrep

} // namespace margelo::nitro::pdfwriter
