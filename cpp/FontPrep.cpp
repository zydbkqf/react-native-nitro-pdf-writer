#include "FontPrep.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace margelo::nitro::pdfwriter {

namespace {

uint16_t readU16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t readU32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

int64_t fileMtimeNs(const std::filesystem::path& path) {
  std::error_code ec;
  auto tp = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return 0;
  }
  return static_cast<int64_t>(tp.time_since_epoch().count());
}

struct TableRecord {
  uint32_t offset;
  uint32_t length;
};

bool findTable(const uint8_t* data, size_t size, uint32_t faceOffset, const char* tag,
               TableRecord& out) {
  if (size < faceOffset + 12) {
    return false;
  }
  const uint8_t* face = data + faceOffset;
  uint16_t numTables = readU16(face + 4);
  if (size < faceOffset + 12 + static_cast<size_t>(numTables) * 16) {
    return false;
  }
  for (uint16_t i = 0; i < numTables; ++i) {
    const uint8_t* rec = face + 12 + i * 16;
    if (std::memcmp(rec, tag, 4) == 0) {
      out.offset = readU32(rec + 8);
      out.length = readU32(rec + 12);
      if (static_cast<size_t>(out.offset) + out.length > size) {
        return false;
      }
      return true;
    }
  }
  return false;
}

} // namespace

namespace FontPrep {

void parseTtcIndex(MediaBlob& blob) {
  blob.isTtc = false;
  blob.ttcFaceOffsets.clear();
  if (blob.bytes.size() < 12) {
    throw std::runtime_error("Font file too small");
  }
  const uint8_t* data = blob.bytes.data();
  uint32_t tag = readU32(data);
  if (tag != 0x74746366u) { // 'ttcf'
    // Plain TTF/OTF — single face at offset 0.
    blob.isTtc = false;
    blob.ttcFaceOffsets.push_back(0);
    return;
  }
  blob.isTtc = true;
  uint32_t numFonts = readU32(data + 8);
  if (numFonts == 0 || numFonts > 4096) {
    throw std::runtime_error("Invalid TTC face count");
  }
  if (blob.bytes.size() < 12 + static_cast<size_t>(numFonts) * 4) {
    throw std::runtime_error("Truncated TTC index");
  }
  blob.ttcFaceOffsets.resize(numFonts);
  for (uint32_t i = 0; i < numFonts; ++i) {
    uint32_t offset = readU32(data + 12 + i * 4);
    if (offset + 12 > blob.bytes.size()) {
      throw std::runtime_error("TTC face offset out of range");
    }
    blob.ttcFaceOffsets[i] = offset;
  }
}

void parseGlyphMetrics(MediaBlob& blob, uint32_t faceOffset) {
  const uint8_t* data = blob.bytes.data();
  size_t size = blob.bytes.size();
  if (size < faceOffset + 12) {
    throw std::runtime_error("Invalid font face offset");
  }

  TableRecord head{}, maxp{}, hhea{}, hmtx{}, name{};
  if (!findTable(data, size, faceOffset, "head", head) || head.length < 54) {
    throw std::runtime_error("Missing or truncated head table");
  }
  if (!findTable(data, size, faceOffset, "maxp", maxp) || maxp.length < 6) {
    throw std::runtime_error("Missing or truncated maxp table");
  }
  if (!findTable(data, size, faceOffset, "hhea", hhea) || hhea.length < 36) {
    throw std::runtime_error("Missing or truncated hhea table");
  }
  if (!findTable(data, size, faceOffset, "hmtx", hmtx)) {
    throw std::runtime_error("Missing hmtx table");
  }

  const uint8_t* headP = data + head.offset;
  blob.unitsPerEm = readU16(headP + 18);

  const uint8_t* maxpP = data + maxp.offset;
  blob.numGlyphs = readU16(maxpP + 4);

  const uint8_t* hheaP = data + hhea.offset;
  uint16_t numHMetrics = readU16(hheaP + 34);
  if (numHMetrics == 0) {
    throw std::runtime_error("Invalid numberOfHMetrics");
  }

  size_t needed = static_cast<size_t>(numHMetrics) * 4;
  if (hmtx.length < needed) {
    throw std::runtime_error("Truncated hmtx table");
  }
  blob.advanceWidths.resize(numHMetrics);
  const uint8_t* hmtxP = data + hmtx.offset;
  for (uint16_t i = 0; i < numHMetrics; ++i) {
    blob.advanceWidths[i] = readU16(hmtxP + i * 4);
  }

  blob.postScriptName.clear();
  if (findTable(data, size, faceOffset, "name", name) && name.length >= 6) {
    const uint8_t* nameP = data + name.offset;
    uint16_t count = readU16(nameP + 2);
    uint16_t stringOffset = readU16(nameP + 4);
    for (uint16_t i = 0; i < count && name.length >= 6 + (i + 1) * 12; ++i) {
      const uint8_t* rec = nameP + 6 + i * 12;
      uint16_t nameId = readU16(rec + 6);
      if (nameId != 6) {
        continue; // PostScript name
      }
      uint16_t length = readU16(rec + 8);
      uint16_t offset = readU16(rec + 10);
      size_t abs = name.offset + stringOffset + offset;
      if (abs + length > size) {
        break;
      }
      blob.postScriptName.assign(reinterpret_cast<const char*>(data + abs), length);
      break;
    }
  }
}

std::shared_ptr<MediaBlob> parseFromBytes(std::vector<uint8_t> bytes, uint32_t faceIndex,
                                          int64_t versionMtime, uint64_t sourceSize) {
  auto blob = std::make_shared<MediaBlob>();
  blob->kind = MediaBlob::Kind::Font;
  blob->bytes = std::move(bytes);
  blob->versionMtime = versionMtime;
  blob->sourceSize = sourceSize;
  blob->faceIndex = faceIndex;

  parseTtcIndex(*blob);
  if (faceIndex >= blob->ttcFaceOffsets.size()) {
    throw std::invalid_argument("Font face index out of range");
  }
  parseGlyphMetrics(*blob, blob->ttcFaceOffsets[faceIndex]);
  return blob;
}

std::shared_ptr<MediaBlob> loadFromFile(const std::string& path, uint32_t faceIndex) {
  namespace fs = std::filesystem;
  std::error_code ec;
  uint64_t size = static_cast<uint64_t>(fs::file_size(path, ec));
  if (ec) {
    throw std::runtime_error("Cannot stat font file: " + path);
  }
  int64_t mtime = fileMtimeNs(fs::path(path));

  // Key includes mtime + size so a rewritten font invalidates the entry.
  std::string key =
      MediaCache::fileKey("font", path + "#" + std::to_string(faceIndex), mtime, size);

  auto& cache = MediaCache::instance();
  if (auto hit = cache.get(key)) {
    return hit;
  }

  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    throw std::runtime_error("Cannot open font file: " + path);
  }
  std::streamsize fileSize = in.tellg();
  if (fileSize <= 0) {
    throw std::runtime_error("Empty font file: " + path);
  }
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
  if (!in.read(reinterpret_cast<char*>(bytes.data()), fileSize)) {
    throw std::runtime_error("Failed to read font file: " + path);
  }

  auto blob = parseFromBytes(std::move(bytes), faceIndex, mtime, size);
  blob->fontPath = path;
  cache.put(key, blob);
  // Return a pinned reference so the caller keeps the entry alive while in use.
  return cache.get(key);
}

std::shared_ptr<MediaBlob> loadFromBuffer(const uint8_t* data, size_t size, uint32_t faceIndex,
                                          bool useCache) {
  uint64_t hash = MediaCache::hashBytes(data, size);
  std::string key = "b:font:" + std::to_string(hash) + "#" + std::to_string(faceIndex);

  auto& cache = MediaCache::instance();
  if (useCache) {
    if (auto hit = cache.get(key)) {
      return hit;
    }
  }

  std::vector<uint8_t> bytes(data, data + size);
  auto blob = parseFromBytes(std::move(bytes), faceIndex, 0, size);

  // HPDF_LoadTTFont* requires a path — materialize one file per blob instance.
  // Unique names avoid two blobs sharing (and deleting) the same path.
  namespace fs = std::filesystem;
  std::string dir = cache.cacheDir();
  if (dir.empty()) {
    dir = fs::temp_directory_path().string();
  }
  // font-src/ is NOT under the wipeable npdf-media-cache payload dir —
  // MediaCache::clear() must not delete files still owned by live blobs.
  fs::path outDir = fs::path(dir) / "npdf-font-src";
  std::error_code ec;
  fs::create_directories(outDir, ec);
  static std::atomic<uint64_t> s_seq{0};
  uint64_t seq = ++s_seq;
  char name[80];
  std::snprintf(name, sizeof(name), "f_%016llx_%u_%llu.ttf",
                static_cast<unsigned long long>(hash), faceIndex,
                static_cast<unsigned long long>(seq));
  fs::path outPath = outDir / name;
  {
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      throw std::runtime_error("Failed to materialize font buffer");
    }
    out.write(reinterpret_cast<const char*>(blob->bytes.data()),
              static_cast<std::streamsize>(blob->bytes.size()));
  }
  blob->fontPath = outPath.string();
  // Reuse the file as long as the blob lives; delete with the blob.
  blob->ownedTempPath = blob->fontPath;

  if (useCache) {
    cache.put(key, blob);
    return cache.get(key);
  }
  return blob;
}

} // namespace FontPrep

} // namespace margelo::nitro::pdfwriter
