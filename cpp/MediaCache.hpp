#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace margelo::nitro::pdfwriter {

/**
 * Shared, document-independent media blob.
 *
 * Fonts are bound to an HPDF_Doc and cannot be shared across documents, but the
 * expensive pre-parse work can: file bytes, glyph metrics, and TTC face index.
 * Images store source bytes (PNG/JPEG/raw) so every document can install them
 * via the FromMem APIs without re-reading the file.
 */
struct MediaBlob {
  enum class Kind : uint8_t { Font, Png, Jpeg, Raw };

  Kind kind = Kind::Raw;
  std::vector<uint8_t> bytes;

  // File-version stamp used in cache keys (0 for in-memory buffers).
  int64_t versionMtime = 0;
  uint64_t sourceSize = 0;

  // --- Font ---
  bool isTtc = false;
  std::vector<uint32_t> ttcFaceOffsets;
  uint32_t faceIndex = 0;
  uint16_t unitsPerEm = 0;
  uint16_t numGlyphs = 0;
  std::vector<uint16_t> advanceWidths;
  std::string postScriptName;
  /** Path passed to HPDF_LoadTTFont* at attach time (original file or materialized buffer). */
  std::string fontPath;
  /** Materialized temp file that should be deleted when the blob is destroyed. */
  std::string ownedTempPath;

  // --- Image ---
  uint32_t width = 0;
  uint32_t height = 0;
  int colorSpace = 0;

  ~MediaBlob() {
    if (!ownedTempPath.empty()) {
      std::remove(ownedTempPath.c_str());
    }
  }

  MediaBlob() = default;
  MediaBlob(const MediaBlob&) = delete;
  MediaBlob& operator=(const MediaBlob&) = delete;

  size_t byteSize() const {
    size_t n = bytes.size() + advanceWidths.size() * sizeof(uint16_t) +
               ttcFaceOffsets.size() * sizeof(uint32_t) + postScriptName.size() + 64;
    return n;
  }
};

/**
 * Thread-safe media cache shared across PDF documents.
 *
 * - Concurrent map (shared_mutex + unordered_map)
 * - Explicit reference counting: get() pins an entry (refCount++); the returned
 *   shared_ptr unpins on destruction. Only unpinned entries are LRU-evictable.
 * - Hard cap on total payload bytes with LRU eviction
 * - Optional disk backing under cacheDir (mtime/version checked via the key)
 */
class MediaCache {
public:
  static MediaCache& instance();

  MediaCache(const MediaCache&) = delete;
  MediaCache& operator=(const MediaCache&) = delete;

  void setCacheDir(const std::string& dir);
  std::string cacheDir() const;

  void setMaxBytes(size_t maxBytes);
  size_t maxBytes() const;
  size_t totalBytes() const;
  size_t entryCount() const;

  /**
   * Returns a pinned blob (refCount++) or nullptr on miss.
   * The returned shared_ptr unpins automatically when destroyed.
   */
  std::shared_ptr<MediaBlob> get(const std::string& key);

  /**
   * Inserts or replaces an entry and evicts LRU victims as needed.
   * Does not pin; use get() for a pinned reference.
   */
  void put(const std::string& key, std::shared_ptr<MediaBlob> blob);

  /** Drops every unpinned entry and deletes disk cache files. */
  void clear();

  /** Sets the byte cap and evicts immediately. */
  void trimTo(size_t maxBytes);

  // --- Key builders (include mtime / content version) ---
  static std::string fileKey(const std::string& kind, const std::string& path, int64_t mtime,
                             uint64_t size);
  static std::string bufferKey(const std::string& kind, uint64_t contentHash, uint32_t width,
                               uint32_t height, int colorSpace);

  static uint64_t hashBytes(const void* data, size_t size);

private:
  MediaCache() = default;

  struct Entry {
    std::shared_ptr<MediaBlob> blob;
    size_t byteSize = 0;
    uint64_t lruStamp = 0;
    int refCount = 0; // external pins
  };

  void unpin(const std::string& key);
  void evictLocked(const std::string* keepKey = nullptr);
  bool loadFromDiskLocked(const std::string& key, std::shared_ptr<MediaBlob>& out);
  void writeToDiskLocked(const std::string& key, const MediaBlob& blob);
  void clearDiskLocked();
  std::string diskPathFor(const std::string& key) const;

  mutable std::shared_mutex _mutex;
  std::unordered_map<std::string, Entry> _map;
  size_t _totalBytes = 0;
  size_t _maxBytes = 32ull * 1024ull * 1024ull; // 32 MiB default
  uint64_t _lruClock = 0;
  std::string _cacheDir;
};

/**
 * Registry of prepared (loaded but not yet attached) media handles.
 * load* returns a handle; attach* consumes it against a document; freeMedia drops it.
 * A single media handle may be attached to many documents.
 */
class MediaRegistry {
public:
  double registerBlob(std::shared_ptr<MediaBlob> blob);
  std::shared_ptr<MediaBlob> get(double handle) const;
  void release(double handle);
  void clear();
  size_t size() const;

private:
  mutable std::mutex _mutex;
  std::unordered_map<double, std::shared_ptr<MediaBlob>> _entries;
  double _nextHandle = 0;
};

} // namespace margelo::nitro::pdfwriter
