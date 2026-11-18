#include "MediaPrep.hpp"

#include "hpdf_types.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace margelo::nitro::pdfwriter {

namespace MediaPrep {

namespace fs = std::filesystem;

namespace {

/** HEIC/WebP/AVIF: not embeddable in PDF, must go through the platform decoder. */
bool isTranscodable(ImageContainer container) {
  return container == ImageContainer::Heic || container == ImageContainer::Webp ||
         container == ImageContainer::Avif;
}

/** Read just enough bytes to sniff a container (no full read on cache hits). */
std::vector<uint8_t> readFileHeader(const std::string& path, size_t maxBytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot open file: " + path);
  }
  std::vector<uint8_t> head(maxBytes);
  in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(maxBytes));
  head.resize(static_cast<size_t>(std::max<std::streamsize>(in.gcount(), 0)));
  return head;
}

/** PNG/JPEG magic wins over the caller's extension- or format-based guess. */
MediaBlob::Kind kindForContainer(ImageContainer container, MediaBlob::Kind fallback) {
  switch (container) {
    case ImageContainer::Png:
      return MediaBlob::Kind::Png;
    case ImageContainer::Jpeg:
      return MediaBlob::Kind::Jpeg;
    default:
      return fallback;
  }
}

const char* kindTag(MediaBlob::Kind kind) {
  return kind == MediaBlob::Kind::Png   ? "png"
         : kind == MediaBlob::Kind::Jpeg ? "jpeg"
                                         : "raw";
}

} // namespace

FileStamp statFile(const std::string& path) {
  std::error_code ec;
  fs::path p(path);
  FileStamp stamp;
  stamp.size = static_cast<uint64_t>(fs::file_size(p, ec));
  if (ec) {
    throw std::runtime_error("Cannot stat file: " + path);
  }
  auto tp = fs::last_write_time(p, ec);
  stamp.mtime = ec ? 0 : static_cast<int64_t>(tp.time_since_epoch().count());
  return stamp;
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    throw std::runtime_error("Cannot open file: " + path);
  }
  std::streamsize size = in.tellg();
  if (size < 0) {
    throw std::runtime_error("Cannot determine file size: " + path);
  }
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
    throw std::runtime_error("Failed to read file: " + path);
  }
  return bytes;
}

void parseImageMeta(MediaBlob& blob) {
  const auto& b = blob.bytes;
  if (blob.kind == MediaBlob::Kind::Png) {
    // PNG signature (8) + IHDR length(4) + "IHDR"(4) + width(4) + height(4)
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (b.size() < 24 || std::memcmp(b.data(), sig, 8) != 0) {
      throw std::runtime_error("Invalid PNG data");
    }
    blob.width = (static_cast<uint32_t>(b[16]) << 24) | (static_cast<uint32_t>(b[17]) << 16) |
                 (static_cast<uint32_t>(b[18]) << 8) | b[19];
    blob.height = (static_cast<uint32_t>(b[20]) << 24) | (static_cast<uint32_t>(b[21]) << 16) |
                  (static_cast<uint32_t>(b[22]) << 8) | b[23];
    if (blob.width == 0 || blob.height == 0) {
      throw std::runtime_error("Invalid PNG dimensions");
    }
    return;
  }
  if (blob.kind == MediaBlob::Kind::Jpeg) {
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) {
      throw std::runtime_error("Invalid JPEG data");
    }
    // Walk markers to find a SOFn frame header.
    size_t i = 2;
    while (i + 9 < b.size()) {
      if (b[i] != 0xFF) {
        ++i;
        continue;
      }
      uint8_t marker = b[i + 1];
      if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
        i += 2;
        continue;
      }
      if (marker == 0xD9 || marker == 0xDA) {
        break;
      }
      uint16_t segLen = static_cast<uint16_t>((b[i + 2] << 8) | b[i + 3]);
      bool isSof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
      if (isSof && i + 9 < b.size()) {
        blob.height = static_cast<uint32_t>((b[i + 5] << 8) | b[i + 6]);
        blob.width = static_cast<uint32_t>((b[i + 7] << 8) | b[i + 8]);
        if (blob.width == 0 || blob.height == 0) {
          throw std::runtime_error("Invalid JPEG dimensions");
        }
        return;
      }
      if (segLen < 2) {
        break;
      }
      i += 2 + segLen;
    }
    throw std::runtime_error("JPEG frame header not found");
  }
  // Raw images: dimensions supplied by the caller.
}

std::shared_ptr<MediaBlob> prepareImageFile(const std::string& path, MediaBlob::Kind kind,
                                            bool useCache, ImageContainer declared) {
  FileStamp stamp = statFile(path);

  // Raw payloads are pixel data, not a container: never sniff those.
  const bool encoded = kind != MediaBlob::Kind::Raw;
  ImageContainer container = ImageContainer::Unknown;
  if (encoded) {
    const std::vector<uint8_t> header = readFileHeader(path, 32);
    container = sniffImageContainer(header.data(), header.size());
    if (container == ImageContainer::Unknown) {
      container = declared;
    }
  }
  const bool needsTranscode = encoded && isTranscodable(container);
  const TranscodeOptions options{};

  std::string key =
      needsTranscode
          ? MediaCache::fileKey(transcodeKeyTag(container, options), path, stamp.mtime, stamp.size)
          : MediaCache::fileKey(kindTag(kindForContainer(container, kind)), path, stamp.mtime,
                                stamp.size);

  auto& cache = MediaCache::instance();
  if (useCache) {
    if (auto hit = cache.get(key)) {
      try {
        restoreImageFromDisk(*hit);
        return hit;
      } catch (...) {
        // Corrupt disk entry: drop memory + disk so the key cannot keep
        // re-throwing, then rebuild from the caller's source bytes below.
        cache.discardEntry(key, hit.get());
      }
    }
  }

  auto blob = std::make_shared<MediaBlob>();
  blob->versionMtime = stamp.mtime;
  blob->sourceSize = stamp.size;
  blob->bytes = readFile(path);

  if (needsTranscode) {
    // Cache the transcoded payload: a cold start must not re-decode the photo.
    auto transcoded =
        transcodeToPdfImage(blob->bytes.data(), blob->bytes.size(), container, options);
    blob->kind = transcoded.kind;
    blob->bytes = std::move(transcoded.bytes);
  } else {
    blob->kind = kindForContainer(container, kind);
  }
  if (blob->kind != MediaBlob::Kind::Raw) {
    parseImageMeta(*blob);
  }

  if (useCache) {
    cache.put(key, blob);
    return cache.get(key); // pinned
  }
  return blob;
}

std::shared_ptr<MediaBlob> prepareImageBuffer(const uint8_t* data, size_t size,
                                              MediaBlob::Kind kind, uint32_t width,
                                              uint32_t height, int colorSpace, bool useCache,
                                              ImageContainer declared) {
  const bool encoded = kind != MediaBlob::Kind::Raw;
  ImageContainer container = ImageContainer::Unknown;
  if (encoded) {
    container = sniffImageContainer(data, size);
    if (container == ImageContainer::Unknown) {
      container = declared;
    }
  } else {
    // Raw pixels are copied verbatim into the PDF by HPDF_LoadRawImageFromMem,
    // which reads width * height * components bytes without looking at the
    // buffer length. Reject a short buffer here instead of over-reading later.
    const size_t expected = checkedPixelBytes(width, height, colorSpaceComponents(colorSpace));
    if (size < expected) {
      throw std::invalid_argument(
          "Raw image buffer is smaller than width * height * components");
    }
  }
  const bool needsTranscode = encoded && isTranscodable(container);
  const TranscodeOptions options{};

  uint64_t hash = MediaCache::hashBytes(data, size);
  std::string key;
  if (needsTranscode) {
    // Geometry fields are meaningless here; the content hash already pins the source.
    key = MediaCache::bufferKey(transcodeKeyTag(container, options), hash, 0, 0, 0);
  } else {
    MediaBlob::Kind keyKind = kindForContainer(container, kind);
    // Encoded payloads derive their geometry from the header, so the ignored
    // width/height/colorSpace arguments must not leak into the key.
    key = MediaCache::bufferKey(kindTag(keyKind), hash, encoded ? 0 : width, encoded ? 0 : height,
                                encoded ? 0 : colorSpace);
  }

  auto& cache = MediaCache::instance();
  if (useCache) {
    if (auto hit = cache.get(key)) {
      // A corrupt disk entry must not stay cached: drop it so the next call
      // re-derives the blob from the source bytes instead of throwing forever.
      try {
        restoreImageFromDisk(*hit);
        return hit;
      } catch (...) {
        // Corrupt disk entry: drop memory + disk so the key cannot keep
        // re-throwing, then rebuild from the caller's source bytes below.
        cache.discardEntry(key, hit.get());
      }
    }
  }

  auto blob = std::make_shared<MediaBlob>();
  blob->versionMtime = 0;
  blob->sourceSize = size;

  if (needsTranscode) {
    auto transcoded = transcodeToPdfImage(data, size, container, options);
    blob->kind = transcoded.kind;
    blob->bytes = std::move(transcoded.bytes);
  } else {
    blob->kind = kindForContainer(container, kind);
    blob->bytes.assign(data, data + size);
  }
  blob->width = width;
  blob->height = height;
  blob->colorSpace = colorSpace;
  if (blob->kind != MediaBlob::Kind::Raw) {
    parseImageMeta(*blob); // encoded payload: dimensions come from the header
  }

  if (useCache) {
    cache.put(key, blob);
    return cache.get(key); // pinned
  }
  return blob;
}

uint32_t checkedDimension(double value, const char* what) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(what) + " must be a finite number");
  }
  if (value != std::trunc(value)) {
    throw std::invalid_argument(std::string(what) + " must be an integer");
  }
  if (value <= 0 || value > static_cast<double>(kMaxImageDimension)) {
    throw std::invalid_argument(std::string(what) + " must be between 1 and " +
                                std::to_string(kMaxImageDimension));
  }
  return static_cast<uint32_t>(value);
}

int checkedColorSpace(double value) {
  if (!std::isfinite(value) || value != std::trunc(value)) {
    throw std::invalid_argument("colorSpace must be an integer");
  }
  // Compare in double space: narrowing a value like 1e30 to int is undefined
  // behaviour, so the range check has to happen before the cast.
  if (value != static_cast<double>(HPDF_CS_DEVICE_GRAY) &&
      value != static_cast<double>(HPDF_CS_DEVICE_RGB) &&
      value != static_cast<double>(HPDF_CS_DEVICE_CMYK)) {
    throw std::invalid_argument(
        "colorSpace must be DEVICE_GRAY, DEVICE_RGB or DEVICE_CMYK");
  }
  return static_cast<int>(value);
}

size_t colorSpaceComponents(int colorSpace) {
  switch (colorSpace) {
    case HPDF_CS_DEVICE_GRAY:
      return 1;
    case HPDF_CS_DEVICE_RGB:
      return 3;
    case HPDF_CS_DEVICE_CMYK:
      return 4;
    default:
      throw std::invalid_argument("Unsupported raw image color space");
  }
}

size_t checkedPixelBytes(uint32_t width, uint32_t height, size_t components) {
  if (width == 0 || height == 0 || components == 0) {
    throw std::invalid_argument("Image dimensions and components must be non-zero");
  }
  size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (pixels / static_cast<size_t>(width) != static_cast<size_t>(height)) {
    throw std::invalid_argument("Image dimensions overflow");
  }
  if (pixels > std::numeric_limits<size_t>::max() / components) {
    throw std::invalid_argument("Image byte count overflows size_t");
  }
  return pixels * components;
}

void restoreImageFromDisk(MediaBlob& blob) {
  // Always take the lock: the previous unlocked fast-path read raced with the
  // writer holding the mutex.
  std::lock_guard<std::mutex> lock(blob.restoreMu);
  if (!blob.restoredFromDisk) {
    return;
  }
  if (blob.kind == MediaBlob::Kind::Png || blob.kind == MediaBlob::Kind::Jpeg) {
    // Re-derive dimensions from the encoded bytes. A truncated cache file makes
    // this throw; the loader then discards the entry (memory + disk) so the key
    // re-derives from the real source on the next call.
    parseImageMeta(blob);
  }
  blob.restoredFromDisk = false;
}

} // namespace MediaPrep

} // namespace margelo::nitro::pdfwriter
