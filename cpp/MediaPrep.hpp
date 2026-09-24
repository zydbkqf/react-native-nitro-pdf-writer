#pragma once

#include "MediaCache.hpp"
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
 */
std::shared_ptr<MediaBlob> prepareImageFile(const std::string& path, MediaBlob::Kind kind,
                                            bool useCache);

/**
 * Prepare a PNG/raw image from an in-memory buffer.
 * `useCache` shares the payload across documents (key = content hash + geometry).
 */
std::shared_ptr<MediaBlob> prepareImageBuffer(const uint8_t* data, size_t size,
                                              MediaBlob::Kind kind, uint32_t width,
                                              uint32_t height, int colorSpace, bool useCache);

/** Parse PNG IHDR / JPEG SOF dimensions (also validates magic). */
void parseImageMeta(MediaBlob& blob);

} // namespace MediaPrep

} // namespace margelo::nitro::pdfwriter
