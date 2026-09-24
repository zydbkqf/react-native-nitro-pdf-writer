#pragma once

#include "MediaCache.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace margelo::nitro::pdfwriter {

/**
 * TrueType / TrueType Collection pre-parse (runs OUTSIDE HaruLock).
 *
 * HPDF_Font is document-bound and cannot be shared across PDFs. What *can* be
 * shared is the expensive decode work:
 *   - file byte reading
 *   - TTC face-index parsing
 *   - glyph metric (hmtx/hhea/maxp/head/name) pre-parse
 */
struct FontPrepResult {
  std::shared_ptr<MediaBlob> blob;
};

namespace FontPrep {

/** Parse TTC 'ttcf' header. Fills blob->isTtc / ttcFaceOffsets. Throws on bad data. */
void parseTtcIndex(MediaBlob& blob);

/**
 * Parse glyph metrics for the face at `faceOffset` (0 for plain TTF).
 * Fills unitsPerEm / numGlyphs / advanceWidths / postScriptName.
 */
void parseGlyphMetrics(MediaBlob& blob, uint32_t faceOffset);

/**
 * Full prep from a file path: read bytes, parse TTC index, parse glyph metrics.
 * Results go through MediaCache (key includes mtime + size) when shared.
 * Sets blob->fontPath to `path` for later attach.
 */
std::shared_ptr<MediaBlob> loadFromFile(const std::string& path, uint32_t faceIndex);

/**
 * Full prep from an in-memory buffer. Materializes a temp file for
 * HPDF_LoadTTFont* (set as blob->fontPath) and parses metrics from the bytes.
 */
std::shared_ptr<MediaBlob> loadFromBuffer(const uint8_t* data, size_t size, uint32_t faceIndex,
                                          bool useCache);

/** Parse from bytes without touching the filesystem (no fontPath). */
std::shared_ptr<MediaBlob> parseFromBytes(std::vector<uint8_t> bytes, uint32_t faceIndex,
                                          int64_t versionMtime, uint64_t sourceSize);

} // namespace FontPrep

} // namespace margelo::nitro::pdfwriter
