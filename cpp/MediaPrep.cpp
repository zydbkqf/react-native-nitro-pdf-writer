#include "MediaPrep.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace margelo::nitro::pdfwriter {

namespace MediaPrep {

namespace fs = std::filesystem;

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
                                            bool useCache) {
  FileStamp stamp = statFile(path);
  const char* kindTag = kind == MediaBlob::Kind::Png   ? "png"
                        : kind == MediaBlob::Kind::Jpeg ? "jpeg"
                                                        : "raw";
  std::string key = MediaCache::fileKey(kindTag, path, stamp.mtime, stamp.size);

  auto& cache = MediaCache::instance();
  if (useCache) {
    if (auto hit = cache.get(key)) {
      return hit;
    }
  }

  auto blob = std::make_shared<MediaBlob>();
  blob->kind = kind;
  blob->bytes = readFile(path);
  blob->versionMtime = stamp.mtime;
  blob->sourceSize = stamp.size;
  if (kind == MediaBlob::Kind::Png || kind == MediaBlob::Kind::Jpeg) {
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
                                              uint32_t height, int colorSpace, bool useCache) {
  uint64_t hash = MediaCache::hashBytes(data, size);
  std::string key = MediaCache::bufferKey(
      kind == MediaBlob::Kind::Png ? "png" : kind == MediaBlob::Kind::Jpeg ? "jpeg" : "raw", hash,
      width, height, colorSpace);

  auto& cache = MediaCache::instance();
  if (useCache) {
    if (auto hit = cache.get(key)) {
      return hit;
    }
  }

  auto blob = std::make_shared<MediaBlob>();
  blob->kind = kind;
  blob->bytes.assign(data, data + size);
  blob->versionMtime = 0;
  blob->sourceSize = size;
  blob->width = width;
  blob->height = height;
  blob->colorSpace = colorSpace;
  if (kind == MediaBlob::Kind::Png || kind == MediaBlob::Kind::Jpeg) {
    parseImageMeta(*blob);
  }

  if (useCache) {
    cache.put(key, blob);
    return cache.get(key); // pinned
  }
  return blob;
}

} // namespace MediaPrep

} // namespace margelo::nitro::pdfwriter
