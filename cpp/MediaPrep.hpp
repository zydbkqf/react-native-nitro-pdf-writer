#pragma once

#include "MediaCache.hpp"
#include "PlatformImageTranscoder.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace margelo::nitro::pdfwriter {

/**
 * Image / font preparation helpers that run OUTSIDE HaruLock.
 * Callers then install the prepared bytes into an HPDF_Doc under the lock.
 */
namespace MediaPrep {

struct FileStamp {
  int64_t mtime = 0;
  uint64_t size = 0;
};

/** Stat a file (mtime + size) used for cache-key version checks. */
FileStamp statFile(const std::string& path);

/** Read an entire file into memory. */
std::vector<uint8_t> readFile(const std::string& path);

/**
 * Prepare a PNG/JPEG/raw image from a file path.
 * Reads bytes and parses dimensions outside the lock; optionally shares the
 * result through MediaCache so multiple PDF documents reuse it.
 *
 * `declared` is the caller's hint (extension or `format` argument). HEIC/WebP
 * content is recognised from magic bytes and transcoded through the platform
 * decoder; the cache then stores the transcoded PNG/JPEG payload.
 */
std::shared_ptr<MediaBlob> prepareImageFile(const std::string& path, MediaBlob::Kind kind,
                                            bool useCache,
                                            ImageContainer declared = ImageContainer::Unknown);

/**
 * Prepare a PNG/raw image from an in-memory buffer.
 * `useCache` shares the payload across documents (key = content hash + geometry).
 */
std::shared_ptr<MediaBlob> prepareImageBuffer(const uint8_t* data, size_t size,
                                              MediaBlob::Kind kind, uint32_t width,
                                              uint32_t height, int colorSpace, bool useCache,
                                              ImageContainer declared = ImageContainer::Unknown);

/** Parse PNG IHDR / JPEG SOF dimensions (also validates magic). */
void parseImageMeta(MediaBlob& blob);

/**
 * Rebuild derived image state for a blob that came back from the disk cache.
 * The envelope already carries the geometry, so this only re-validates the
 * encoded headers (PNG IHDR / JPEG SOF) against the stored payload.
 */
void restoreImageFromDisk(MediaBlob& blob);

// ---------------------------------------------------------------------------
// Dimension validation shared by raw images and YUV conversion
// ---------------------------------------------------------------------------

/** Upper bound accepted for a single image edge; keeps byte counts far from overflow. */
constexpr uint32_t kMaxImageDimension = 32768;

/** Accepts only finite, integral values in [1, kMaxImageDimension]. */
uint32_t checkedDimension(double value, const char* what);

/** Accepts only the raw-image color spaces libharu can install (gray / rgb / cmyk). */
int checkedColorSpace(double value);

/** Components per pixel for a checked color space. */
size_t colorSpaceComponents(int colorSpace);

/** Overflow-checked width * height * components. */
size_t checkedPixelBytes(uint32_t width, uint32_t height, size_t components);

} // namespace MediaPrep

} // namespace margelo::nitro::pdfwriter
