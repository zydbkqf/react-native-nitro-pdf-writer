#include "HybridNitroPdfWriter.hpp"
#include "FontPrep.hpp"
#include "HaruError.hpp"
#include "HaruLock.hpp"
#include "MediaPrep.hpp"
#include "MediaWorker.hpp"
#include <NitroModules/ArrayBuffer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <variant>
#include <vector>

namespace margelo::nitro::pdfwriter {

using namespace margelo::nitro;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static HPDF_Rect toRect(const std::vector<double>& values) {
  if (values.size() != 4) {
    throw std::invalid_argument("Rect array must have exactly 4 elements");
  }
  HPDF_Rect rect;
  rect.left = static_cast<HPDF_REAL>(values[0]);
  rect.top = static_cast<HPDF_REAL>(values[1]);
  rect.right = static_cast<HPDF_REAL>(values[2]);
  rect.bottom = static_cast<HPDF_REAL>(values[3]);
  return rect;
}

static std::string makeTempPath() {
  auto tempDir = std::filesystem::temp_directory_path();
  std::string path = (tempDir / "nitropdfwriter_XXXXXX").string();
  std::vector<char> pathBuf(path.begin(), path.end());
  pathBuf.push_back('\0');
  int fd = mkstemp(pathBuf.data());
  if (fd < 0) {
    throw std::runtime_error("Failed to create temporary file");
  }
  close(fd);
  return std::string(pathBuf.data());
}

static HPDF_DashMode toDashMode(const std::vector<double>& pattern) {
  HPDF_DashMode mode{};
  mode.num_ptn = static_cast<HPDF_UINT>(std::min(pattern.size(), static_cast<size_t>(8)));
  for (size_t i = 0; i < mode.num_ptn; ++i) {
    mode.ptn[i] = static_cast<HPDF_REAL>(pattern[i]);
  }
  mode.phase = 0;
  return mode;
}

// ---------------------------------------------------------------------------
// AnyValue extraction helpers — tolerate string-encoded numbers/bools
// ---------------------------------------------------------------------------

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// JS Number() semantics: "" → 0, "123abc" → throws, "0x10" → 16
static double anyToDouble(const AnyValue& v) {
  return std::visit(overloaded{
    [](double arg) -> double { return arg; },
    [](int64_t arg) -> double { return static_cast<double>(arg); },
    [](bool arg) -> double { return arg ? 1.0 : 0.0; },
    [](NullType) -> double { return 0.0; },
    [](const std::string& s) -> double {
      if (s.empty()) return 0.0;
      try {
        size_t pos = 0;
        double result = std::stod(s, &pos);
        if (pos != s.size()) throw std::invalid_argument("trailing");
        return result;
      } catch (...) {
        throw std::invalid_argument("Cannot convert string to number: " + s);
      }
    },
    [](const auto&) -> double { throw std::invalid_argument("Value is not a number"); }
  }, v);
}

static float anyToFloat(const AnyValue& v) {
  return static_cast<HPDF_REAL>(anyToDouble(v));
}

static bool anyToBool(const AnyValue& v) {
  return std::visit(overloaded{
    [](bool arg) -> bool { return arg; },
    [](double arg) -> bool { return arg != 0.0 && !std::isnan(arg); },
    [](int64_t arg) -> bool { return arg != 0; },
    [](NullType) -> bool { return false; },
    [](const std::string& s) -> bool { return !s.empty(); },
    [](const AnyArray&) -> bool { return true; },
    [](const AnyObject&) -> bool { return true; },
  }, v);
}

static std::string anyToString(const AnyValue& v) {
  return std::visit(overloaded{
    [](const std::string& s) -> std::string { return s; },
    [](double arg) -> std::string {
      if (std::isnan(arg)) return "NaN";
      if (std::isinf(arg)) return arg > 0 ? "Infinity" : "-Infinity";
      // Use shortest round-trip representation
      char buf[32];
      snprintf(buf, sizeof(buf), "%.17g", arg);
      // Trim to shortest representation that round-trips
      for (int prec = 1; prec <= 17; ++prec) {
        char tryBuf[32];
        snprintf(tryBuf, sizeof(tryBuf), "%.*g", prec, arg);
        try {
          if (std::stod(tryBuf) == arg) { snprintf(buf, sizeof(buf), "%s", tryBuf); break; }
        } catch (...) {
          // Try a higher precision
        }
      }
      return buf;
    },
    [](int64_t arg) -> std::string { return std::to_string(arg); },
    [](bool arg) -> std::string { return arg ? "true" : "false"; },
    [](NullType) -> std::string { return ""; },
    [](const auto&) -> std::string { throw std::invalid_argument("Value cannot be converted to string"); }
  }, v);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

HybridNitroPdfWriter::HybridNitroPdfWriter() : HybridObject(TAG) {}

HybridNitroPdfWriter::~HybridNitroPdfWriter() {
  std::lock_guard<std::mutex> lock(HaruLock::get());
  // Free all documents. Pages/fonts/images are owned by the doc and will be
  // invalidated automatically.
  for (const auto& entry : _docs.allEntries()) {
    HPDF_Free(entry.second.pointer);
  }
  _docs.clear();
  _pages.clear();
  _fonts.clear();
  _images.clear();
  _destinations.clear();
  _outlines.clear();
  _extGStates.clear();
  _annotations.clear();
  _media.clear();
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::createDocument() {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    HPDF_Doc doc = HPDF_New(nullptr, nullptr);
    if (doc == nullptr) {
      throw std::runtime_error("Failed to create HPDF document");
    }
    if (HPDF_UseUTFEncodings(doc) != HPDF_OK) {
      throw std::runtime_error("Failed to enable UTF-8 encodings");
    }
    return _docs.registerPointer(doc, nullptr);
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::freeDocument(double doc) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    HPDF_Doc pointer = _docs.getPointer(doc);
    if (pointer != nullptr) {
      _docs.unregisterHandle(doc);
      HPDF_Free(pointer);
      // Child handles are owned by the document and are now dangling.
      auto cleanup = [pointer](auto& registry) {
        std::vector<double> stale;
        for (const auto& entry : registry.allEntries()) {
          if (entry.second.owner == pointer) {
            stale.push_back(entry.first);
          }
        }
        for (double handle : stale) {
          registry.unregisterHandle(handle);
        }
        return stale;
      };
      for (double handle : cleanup(_pages)) {
        _pagesInTextMode.erase(handle);
      }
      cleanup(_fonts);
      cleanup(_images);
      cleanup(_destinations);
      cleanup(_outlines);
      cleanup(_extGStates);
      cleanup(_annotations);
    }
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::saveToFile(double doc, const std::string& path) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc, path]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    throwOnHaruError(HPDF_SaveToFile(h.get(), path.c_str()), h.get());
  });
  return promise;
}

std::shared_ptr<Promise<std::shared_ptr<ArrayBuffer>>> HybridNitroPdfWriter::saveToBuffer(double doc) {
  auto promise = Promise<std::shared_ptr<ArrayBuffer>>::create();
  HaruWorker::getInstance().run<std::shared_ptr<ArrayBuffer>>(promise, [this, doc]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_Doc pointer = h.get();

    // Save to a temporary file and read it back. libHaru's public API does
    // not expose a simple save-to-memory helper, so this is the safest route.
    std::string tempPath = makeTempPath();
    struct TempFileGuard {
      std::string path;
      ~TempFileGuard() { std::remove(path.c_str()); }
    } tempGuard{tempPath};
    throwOnHaruError(HPDF_SaveToFile(pointer, tempPath.c_str()), pointer);
    throwHaruError(pointer);

    std::ifstream file(tempPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
      throw std::runtime_error("Failed to open temporary PDF file");
    }
    std::streamsize size = file.tellg();
    if (size < 0) {
      throw std::runtime_error("Failed to determine temporary PDF file size");
    }
    file.seekg(0, std::ios::beg);

    std::shared_ptr<ArrayBuffer> buffer = ArrayBuffer::allocate(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer->data()), size)) {
      throw std::runtime_error("Failed to read temporary PDF file");
    }
    return buffer;
  });
  return promise;
}

std::shared_ptr<Promise<bool>> HybridNitroPdfWriter::hasDoc(double doc) {
  auto promise = Promise<bool>::create();
  HaruWorker::getInstance().run<bool>(promise, [this, doc]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    return _docs.getPointer(doc) != nullptr;
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::getError(double doc) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    return static_cast<double>(HPDF_GetError(h.get()));
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::getErrorDetail(double doc) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    return static_cast<double>(HPDF_GetErrorDetail(h.get()));
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::resetError(double doc) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_ResetError(h.get());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Document properties
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setCompressionMode(double doc, double mode) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc, mode]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    throwOnHaruError(HPDF_SetCompressionMode(h.get(), static_cast<HPDF_UINT>(mode)), h.get());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setPassword(double doc, const std::string& ownerPassword,
                                                           const std::string& userPassword) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc, ownerPassword, userPassword]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    throwOnHaruError(HPDF_SetPassword(h.get(), ownerPassword.c_str(), userPassword.c_str()), h.get());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setPermission(double doc, double permission) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc, permission]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    throwOnHaruError(HPDF_SetPermission(h.get(), static_cast<HPDF_UINT>(permission)), h.get());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setEncryptionMode(double doc, double mode, double keyLen) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc, mode, keyLen]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    throwOnHaruError(HPDF_SetEncryptionMode(h.get(), static_cast<HPDF_EncryptMode>(mode),
                                        static_cast<HPDF_UINT>(keyLen)),
                 h.get());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::addPage(double doc) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_Page page = HPDF_AddPage(h.get());
    throwHaruError(h.get());
    return _pages.registerPointer(page, h.get());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::getCurrentPage(double doc) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_Page page = HPDF_GetCurrentPage(h.get());
    throwHaruError(h.get());
    return _pages.registerPointer(page, h.get());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setCurrentPage(double doc, double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> docH(doc, _docs);
    Handle<HPDF_Page> pageH(page, _pages);
    throwOnHaruError(HPDF_Doc_SetCurrentPage(docH.get(), pageH.get()), docH.get());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::insertPage(double doc, double page) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> docH(doc, _docs);
    Handle<HPDF_Page> pageH(page, _pages);
    HPDF_Page newPage = HPDF_InsertPage(docH.get(), pageH.get());
    throwHaruError(docH.get());
    return _pages.registerPointer(newPage, docH.get());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::getPageByIndex(double doc, double index) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, index]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_Page page = HPDF_GetPageByIndex(h.get(), static_cast<HPDF_UINT16>(index));
    throwHaruError(h.get());
    return _pages.registerPointer(page, h.get());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Page sizes
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setPageWidth(double page, double width) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, width]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetWidth(h.get(), static_cast<HPDF_REAL>(width)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setPageHeight(double page, double height) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, height]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetHeight(h.get(), static_cast<HPDF_REAL>(height)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setPageSize(double page, double size) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, size]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(
        HPDF_Page_SetSize(h.get(), static_cast<HPDF_PageSizes>(size), HPDF_PAGE_PORTRAIT), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setSize(double page, double size, double direction) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, size, direction]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetSize(h.get(), static_cast<HPDF_PageSizes>(size),
                                   static_cast<HPDF_PageDirection>(direction)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setRotate(double page, double angle) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, angle]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetRotate(h.get(), static_cast<HPDF_UINT16>(angle)), h.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::getFont(double doc, const std::string& fontName,
                                                         const std::optional<std::string>& encodingName) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, fontName, encodingName]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    const char* encoding = encodingName.has_value() ? encodingName->c_str() : nullptr;
    HPDF_Font font = HPDF_GetFont(h.get(), fontName.c_str(), encoding);
    throwHaruError(h.get());
    return _fonts.registerPointer(font, h.get());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Font / image install helpers (HaruLock held)
// ---------------------------------------------------------------------------

double HybridNitroPdfWriter::installImageBlob(HPDF_Doc doc, const std::shared_ptr<MediaBlob>& blob) {
  if (!blob) {
    throw std::invalid_argument("Invalid media handle");
  }
  HPDF_Image image = nullptr;
  switch (blob->kind) {
    case MediaBlob::Kind::Png:
      image = HPDF_LoadPngImageFromMem(doc, blob->bytes.data(),
                                       static_cast<HPDF_UINT>(blob->bytes.size()));
      break;
    case MediaBlob::Kind::Jpeg:
      image = HPDF_LoadJpegImageFromMem(doc, blob->bytes.data(),
                                        static_cast<HPDF_UINT>(blob->bytes.size()));
      break;
    case MediaBlob::Kind::Raw:
      image = HPDF_LoadRawImageFromMem(doc, blob->bytes.data(),
                                       static_cast<HPDF_UINT>(blob->width),
                                       static_cast<HPDF_UINT>(blob->height),
                                       static_cast<HPDF_ColorSpace>(blob->colorSpace), 8);
      break;
    default:
      throw std::invalid_argument("Media handle is not an image");
  }
  throwHaruError(doc);
  return _images.registerPointer(image, doc);
}

double HybridNitroPdfWriter::installFontBlob(HPDF_Doc doc, const std::shared_ptr<MediaBlob>& blob,
                                             std::optional<bool> embedding,
                                             const char* encoding) {
  if (!blob || blob->kind != MediaBlob::Kind::Font) {
    throw std::invalid_argument("Media handle is not a font");
  }
  if (blob->fontPath.empty()) {
    throw std::runtime_error("Font media has no load path");
  }
  // attachFont / quickDraw default embedding=true (see PdfTypes). One-step
  // sugar passes an explicit value to preserve historical defaults.
  HPDF_BOOL embed = embedding.value_or(HPDF_TRUE);
  const char* fontName = nullptr;
  if (blob->isTtc) {
    fontName = HPDF_LoadTTFontFromFile2(doc, blob->fontPath.c_str(), blob->faceIndex, embed);
  } else {
    fontName = HPDF_LoadTTFontFromFile(doc, blob->fontPath.c_str(), embed);
  }
  throwHaruError(doc);
  if (!fontName) throw std::runtime_error("Failed to load font");
  // Embedded TrueType fonts default to UTF-8 so CJK / non-ASCII text renders.
  const char* enc = encoding ? encoding : "UTF-8";
  HPDF_Font font = HPDF_GetFont(doc, fontName, enc);
  throwHaruError(doc);
  return _fonts.registerPointer(font, doc);
}

// ---------------------------------------------------------------------------
// Fonts — two-step load / attach
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadFontFromFile(
    const std::string& fileName, std::optional<double> faceIndex) {
  auto promise = Promise<double>::create();
  uint32_t face = static_cast<uint32_t>(faceIndex.value_or(0));
  if (faceIndex.has_value() && *faceIndex < 0) face = 0;
  MediaWorker::getInstance().run<double>(promise, [this, fileName, face]() {
    // Outside HaruLock: read bytes, TTC index, glyph metrics.
    auto blob = FontPrep::loadFromFile(fileName, face);
    return _media.registerBlob(blob);
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadFontFromBuffer(
    const std::shared_ptr<ArrayBuffer>& buffer, std::optional<double> faceIndex) {
  auto promise = Promise<double>::create();
  uint32_t face = static_cast<uint32_t>(faceIndex.value_or(0));
  if (faceIndex.has_value() && *faceIndex < 0) face = 0;
  MediaWorker::getInstance().run<double>(promise, [this, buffer, face]() {
    auto blob = FontPrep::loadFromBuffer(buffer->data(), buffer->size(), face,
                                         /*useCache=*/false);
    return _media.registerBlob(blob);
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::attachFont(double doc, double media,
                                                                  std::optional<bool> embedding) {
  auto promise = Promise<double>::create();
  MediaWorker::getInstance().run<double>(promise, [this, doc, media, embedding]() {
    auto blob = _media.get(media);
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    return installFontBlob(h.get(), blob, embedding);
  });
  return promise;
}

// --- Font sugar (load + attach in one call) ---

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadType1FontFromFile(double doc,
                                                                       const std::string& afmPath,
                                                                       const std::optional<std::string>& pfmPath) {
  auto promise = Promise<double>::create();
  // Type1 AFM/PFM have no useful cross-doc pre-parse; install directly.
  HaruWorker::getInstance().run<double>(promise, [this, doc, afmPath, pfmPath]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    const char* pfm = pfmPath.has_value() ? pfmPath->c_str() : nullptr;
    const char* fontName = HPDF_LoadType1FontFromFile(h.get(), afmPath.c_str(), pfm);
    throwHaruError(h.get());
    if (!fontName) throw std::runtime_error("Failed to load font");
    HPDF_Font font = HPDF_GetFont(h.get(), fontName, nullptr);
    throwHaruError(h.get());
    return _fonts.registerPointer(font, h.get());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadTTFontFromFile(double doc, const std::string& fileName,
                                                                    std::optional<bool> embedding) {
  return loadTTFontFromFile2(doc, fileName, 0, embedding);
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadTTFontFromFile2(double doc, const std::string& fileName,
                                                                     double index,
                                                                     std::optional<bool> embedding) {
  auto promise = Promise<double>::create();
  uint32_t faceIndex = static_cast<uint32_t>(index < 0 ? 0 : index);
  // Sugar = loadFontFromFile + attachFont. Keep historical default (embed=false)
  // for this one-step API; attachFont defaults to true.
  bool embed = embedding.value_or(false);
  MediaWorker::getInstance().run<double>(promise, [this, doc, fileName, faceIndex, embed]() {
    std::shared_ptr<MediaBlob> prep = FontPrep::loadFromFile(fileName, faceIndex);
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    return installFontBlob(h.get(), prep, embed);
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setCurrentFont(double doc, double font) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc, font]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> docH(doc, _docs);
    Handle<HPDF_Font> fontH(font, _fonts);
    (void)docH.get();
    (void)fontH.get();
    throw std::runtime_error("setCurrentFont is not supported; use setFontAndSize instead");
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setFontAndSize(double page, double font, double size) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, font, size]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> pageH(page, _pages);
    Handle<HPDF_Font> fontH(font, _fonts);
    throwOnHaruError(HPDF_Page_SetFontAndSize(pageH.get(), fontH.get(), static_cast<HPDF_REAL>(size)),
                 pageH.owner());
  });
  return promise;
}

std::shared_ptr<Promise<std::string>> HybridNitroPdfWriter::getFontName(double font) {
  auto promise = Promise<std::string>::create();
  HaruWorker::getInstance().run<std::string>(promise, [this, font]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Font> h(font, _fonts);
    const char* name = HPDF_Font_GetFontName(h.get());
    return std::string(name ? name : "");
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::measureText(double page, const std::string& text, double width,
                                                             std::optional<bool> wordwrap) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, page, text, width, wordwrap]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    HPDF_BOOL wrap = wordwrap.value_or(HPDF_FALSE);
    HPDF_REAL realWidth = 0;
    HPDF_Page_MeasureText(h.get(), text.c_str(), static_cast<HPDF_REAL>(width), wrap, &realWidth);
    throwHaruError(h.owner());
    return static_cast<double>(realWidth);
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Text state
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::beginText(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);

    // Check if already in text mode
    if (_pagesInTextMode.count(page) > 0) {
      throw std::runtime_error("Nested beginText() is not allowed. Call endText() before calling beginText() again.");
    }

    throwOnHaruError(HPDF_Page_BeginText(h.get()), h.owner());
    _pagesInTextMode.insert(page);
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::endText(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_EndText(h.get()), h.owner());
    _pagesInTextMode.erase(page);
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::textOut(double page, double x, double y, const std::string& text) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x, y, text]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);

    // Check if in text mode
    if (_pagesInTextMode.count(page) == 0) {
      throw std::runtime_error("textOut() requires beginText() to be called first.");
    }

    throwOnHaruError(HPDF_Page_TextOut(h.get(), static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y), text.c_str()),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::textRect(double page, double left, double top, double right,
                                                        double bottom, const std::string& text,
                                                        double align) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, left, top, right, bottom, text, align]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);

    // Check if in text mode
    if (_pagesInTextMode.count(page) == 0) {
      throw std::runtime_error("textRect() requires beginText() to be called first.");
    }

    throwOnHaruError(HPDF_Page_TextRect(h.get(), static_cast<HPDF_REAL>(left), static_cast<HPDF_REAL>(top),
                                    static_cast<HPDF_REAL>(right), static_cast<HPDF_REAL>(bottom),
                                    text.c_str(), static_cast<HPDF_TextAlignment>(align), nullptr),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setTextLeading(double page, double leading) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, leading]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetTextLeading(h.get(), static_cast<HPDF_REAL>(leading)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setTextRenderingMode(double page, double mode) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, mode]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetTextRenderingMode(h.get(), static_cast<HPDF_TextRenderingMode>(mode)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setTextRise(double page, double rise) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, rise]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetTextRise(h.get(), static_cast<HPDF_REAL>(rise)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setCharSpace(double page, double space) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, space]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetCharSpace(h.get(), static_cast<HPDF_REAL>(space)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setWordSpace(double page, double space) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, space]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetWordSpace(h.get(), static_cast<HPDF_REAL>(space)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setHorizontalScalling(double page, double scale) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, scale]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetHorizontalScalling(h.get(), static_cast<HPDF_REAL>(scale)), h.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Text transformation
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::moveTextPos(double page, double x, double y) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x, y]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_MoveTextPos(h.get(), static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::moveTextPos2(double page, double x, double y) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x, y]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_MoveTextPos2(h.get(), static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setTextMatrix(double page, double a, double b, double c,
                                                             double d, double x, double y) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, a, b, c, d, x, y]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetTextMatrix(h.get(), static_cast<HPDF_REAL>(a), static_cast<HPDF_REAL>(b),
                                         static_cast<HPDF_REAL>(c), static_cast<HPDF_REAL>(d),
                                         static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y)),
                 h.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Graphics state
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setLineWidth(double page, double width) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, width]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetLineWidth(h.get(), static_cast<HPDF_REAL>(width)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setLineCap(double page, double cap) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, cap]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetLineCap(h.get(), static_cast<HPDF_LineCap>(cap)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setLineJoin(double page, double join) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, join]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetLineJoin(h.get(), static_cast<HPDF_LineJoin>(join)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setMiterLimit(double page, double miterLimit) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, miterLimit]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetMiterLimit(h.get(), static_cast<HPDF_REAL>(miterLimit)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setDash(double page, const std::vector<double>& dashPattern,
                                                       double phase) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, dashPattern, phase]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    HPDF_DashMode mode = toDashMode(dashPattern);
    mode.phase = static_cast<HPDF_REAL>(phase);
    throwOnHaruError(HPDF_Page_SetDash(h.get(), mode.ptn, mode.num_ptn, mode.phase), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setFlat(double page, double flatness) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, flatness]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetFlat(h.get(), static_cast<HPDF_REAL>(flatness)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setExtGState(double page, double extGState) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, extGState]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> pageH(page, _pages);
    Handle<HPDF_ExtGState> stateH(extGState, _extGStates);
    throwOnHaruError(HPDF_Page_SetExtGState(pageH.get(), stateH.get()), pageH.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setRGBFill(double page, double r, double g, double b) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, r, g, b]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetRGBFill(h.get(), static_cast<HPDF_REAL>(r), static_cast<HPDF_REAL>(g),
                                      static_cast<HPDF_REAL>(b)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setRGBStroke(double page, double r, double g, double b) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, r, g, b]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetRGBStroke(h.get(), static_cast<HPDF_REAL>(r), static_cast<HPDF_REAL>(g),
                                        static_cast<HPDF_REAL>(b)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setCMYKFill(double page, double c, double m, double y,
                                                           double k) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, c, m, y, k]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetCMYKFill(h.get(), static_cast<HPDF_REAL>(c), static_cast<HPDF_REAL>(m),
                                       static_cast<HPDF_REAL>(y), static_cast<HPDF_REAL>(k)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setCMYKStroke(double page, double c, double m, double y,
                                                             double k) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, c, m, y, k]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetCMYKStroke(h.get(), static_cast<HPDF_REAL>(c), static_cast<HPDF_REAL>(m),
                                         static_cast<HPDF_REAL>(y), static_cast<HPDF_REAL>(k)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setGrayFill(double page, double gray) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, gray]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetGrayFill(h.get(), static_cast<HPDF_REAL>(gray)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setGrayStroke(double page, double gray) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, gray]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_SetGrayStroke(h.get(), static_cast<HPDF_REAL>(gray)), h.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Path construction
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::moveTo(double page, double x, double y) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x, y]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_MoveTo(h.get(), static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::lineTo(double page, double x, double y) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x, y]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_LineTo(h.get(), static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::curveTo(double page, double x1, double y1, double x2,
                                                       double y2, double x3, double y3) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x1, y1, x2, y2, x3, y3]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_CurveTo(h.get(), static_cast<HPDF_REAL>(x1), static_cast<HPDF_REAL>(y1),
                                   static_cast<HPDF_REAL>(x2), static_cast<HPDF_REAL>(y2),
                                   static_cast<HPDF_REAL>(x3), static_cast<HPDF_REAL>(y3)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::curveTo2(double page, double x2, double y2, double x3,
                                                        double y3) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x2, y2, x3, y3]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_CurveTo2(h.get(), static_cast<HPDF_REAL>(x2), static_cast<HPDF_REAL>(y2),
                                    static_cast<HPDF_REAL>(x3), static_cast<HPDF_REAL>(y3)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::curveTo3(double page, double x1, double y1, double x3,
                                                        double y3) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x1, y1, x3, y3]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_CurveTo3(h.get(), static_cast<HPDF_REAL>(x1), static_cast<HPDF_REAL>(y1),
                                    static_cast<HPDF_REAL>(x3), static_cast<HPDF_REAL>(y3)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::closePath(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_ClosePath(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::rectangle(double page, double x, double y, double width,
                                                         double height) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x, y, width, height]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_Rectangle(h.get(), static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y),
                                     static_cast<HPDF_REAL>(width), static_cast<HPDF_REAL>(height)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::arc(double page, double x, double y, double radius,
                                                   double angle1, double angle2) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x, y, radius, angle1, angle2]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_Arc(h.get(), static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y),
                               static_cast<HPDF_REAL>(radius), static_cast<HPDF_REAL>(angle1),
                               static_cast<HPDF_REAL>(angle2)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::ellipse(double page, double x, double y, double xRadius,
                                                       double yRadius) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x, y, xRadius, yRadius]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_Ellipse(h.get(), static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y),
                                   static_cast<HPDF_REAL>(xRadius), static_cast<HPDF_REAL>(yRadius)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::circle(double page, double x, double y, double radius) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x, y, radius]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_Circle(h.get(), static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y),
                                  static_cast<HPDF_REAL>(radius)),
                 h.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Path painting
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::stroke(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_Stroke(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::closePathStroke(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_ClosePathStroke(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::fill(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_Fill(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::eofill(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_Eofill(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::fillStroke(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_FillStroke(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::eofillStroke(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_EofillStroke(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::closePathFillStroke(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_ClosePathFillStroke(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::closePathEofillStroke(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_ClosePathEofillStroke(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::endPath(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_EndPath(h.get()), h.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Images — two-step load / attach
// ---------------------------------------------------------------------------

static MediaBlob::Kind parseImageFormat(const std::string& format) {
  if (format == "jpeg" || format == "jpg" || format == "JPEG" || format == "JPG") {
    return MediaBlob::Kind::Jpeg;
  }
  if (format == "raw" || format == "RAW") {
    return MediaBlob::Kind::Raw;
  }
  return MediaBlob::Kind::Png;
}

static MediaBlob::Kind detectImageKind(const std::string& path) {
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return (ext == ".jpg" || ext == ".jpeg") ? MediaBlob::Kind::Jpeg : MediaBlob::Kind::Png;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadImageFromFile(const std::string& fileName,
                                                                         std::optional<bool> useCache) {
  auto promise = Promise<double>::create();
  bool cache = useCache.value_or(false);
  MediaWorker::getInstance().run<double>(promise, [this, fileName, cache]() {
    MediaBlob::Kind kind = detectImageKind(fileName);
    auto blob = MediaPrep::prepareImageFile(fileName, kind, cache);
    return _media.registerBlob(blob);
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadImageFromBuffer(
    const std::shared_ptr<ArrayBuffer>& buffer, const std::string& format, double width,
    double height, double colorSpace, std::optional<bool> useCache) {
  auto promise = Promise<double>::create();
  bool cache = useCache.value_or(false);
  MediaBlob::Kind kind = parseImageFormat(format);
  MediaWorker::getInstance().run<double>(promise, [this, buffer, kind, width, height, colorSpace, cache]() {
    auto blob = MediaPrep::prepareImageBuffer(
        buffer->data(), buffer->size(), kind, static_cast<uint32_t>(width),
        static_cast<uint32_t>(height), static_cast<int>(colorSpace), cache);
    return _media.registerBlob(blob);
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::attachImage(double doc, double media) {
  auto promise = Promise<double>::create();
  MediaWorker::getInstance().run<double>(promise, [this, doc, media]() {
    auto blob = _media.get(media);
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    return installImageBlob(h.get(), blob);
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::freeMedia(double media) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, media]() {
    _media.release(media);
  });
  return promise;
}

// --- Image sugar (load + attach in one call) ---

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadPngImageFromFile(double doc, const std::string& fileName) {
  auto promise = Promise<double>::create();
  MediaWorker::getInstance().run<double>(promise, [this, doc, fileName]() {
    auto blob = MediaPrep::prepareImageFile(fileName, MediaBlob::Kind::Png, /*useCache=*/false);
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    return installImageBlob(h.get(), blob);
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadPngImageFromBuffer(double doc,
                                                                        const std::shared_ptr<ArrayBuffer>& buffer,
                                                                        std::optional<bool> useCache) {
  auto promise = Promise<double>::create();
  bool cache = useCache.value_or(false);
  MediaWorker::getInstance().run<double>(promise, [this, doc, buffer, cache]() {
    auto blob = MediaPrep::prepareImageBuffer(buffer->data(), buffer->size(), MediaBlob::Kind::Png,
                                              0, 0, 0, cache);
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    return installImageBlob(h.get(), blob);
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadJpegImageFromFile(double doc, const std::string& fileName) {
  auto promise = Promise<double>::create();
  MediaWorker::getInstance().run<double>(promise, [this, doc, fileName]() {
    auto blob = MediaPrep::prepareImageFile(fileName, MediaBlob::Kind::Jpeg, /*useCache=*/false);
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    return installImageBlob(h.get(), blob);
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadRawImageFromFile(double doc, const std::string& fileName,
                                                                      double width, double height,
                                                                      double colorSpace) {
  auto promise = Promise<double>::create();
  MediaWorker::getInstance().run<double>(promise, [this, doc, fileName, width, height, colorSpace]() {
    auto blob = MediaPrep::prepareImageFile(fileName, MediaBlob::Kind::Raw, /*useCache=*/false);
    blob->width = static_cast<uint32_t>(width);
    blob->height = static_cast<uint32_t>(height);
    blob->colorSpace = static_cast<int>(colorSpace);
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    return installImageBlob(h.get(), blob);
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadRawImageFromBuffer(double doc,
                                                                        const std::shared_ptr<ArrayBuffer>& buffer,
                                                                        double width, double height,
                                                                        double colorSpace,
                                                                        std::optional<bool> useCache) {
  auto promise = Promise<double>::create();
  bool cache = useCache.value_or(false);
  MediaWorker::getInstance().run<double>(promise, [this, doc, buffer, width, height, colorSpace, cache]() {
    if (width <= 0 || height <= 0) {
      throw std::invalid_argument("Width and height must be positive");
    }
    size_t components = 3;
    HPDF_ColorSpace cs = static_cast<HPDF_ColorSpace>(colorSpace);
    if (cs == HPDF_CS_DEVICE_GRAY) {
      components = 1;
    } else if (cs == HPDF_CS_DEVICE_RGB) {
      components = 3;
    } else if (cs == HPDF_CS_DEVICE_CMYK) {
      components = 4;
    }
    size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * components;
    if (buffer->size() < expected) {
      throw std::invalid_argument("Buffer too small for raw image data");
    }
    auto blob = MediaPrep::prepareImageBuffer(buffer->data(), buffer->size(), MediaBlob::Kind::Raw,
                                              static_cast<uint32_t>(width),
                                              static_cast<uint32_t>(height),
                                              static_cast<int>(colorSpace), cache);
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    return installImageBlob(h.get(), blob);
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setImageMask(double image, double mask) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, image, mask]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Image> imageH(image, _images);
    Handle<HPDF_Image> maskH(mask, _images);
    throwOnHaruError(HPDF_Image_SetMaskImage(imageH.get(), maskH.get()), imageH.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::drawImage(double page, double image, double x, double y,
                                                         double width, double height) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, image, x, y, width, height]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> pageH(page, _pages);
    Handle<HPDF_Image> imageH(image, _images);
    throwOnHaruError(HPDF_Page_DrawImage(pageH.get(), imageH.get(), static_cast<HPDF_REAL>(x),
                                     static_cast<HPDF_REAL>(y), static_cast<HPDF_REAL>(width),
                                     static_cast<HPDF_REAL>(height)),
                 pageH.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// drawRawImage
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::drawRawImage(double page,
                                                             const std::shared_ptr<ArrayBuffer>& buffer,
                                                             double width, double height, double colorSpace,
                                                             double x, double y,
                                                             double drawWidth, double drawHeight) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, buffer, width, height, colorSpace, x, y, drawWidth, drawHeight]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> pageH(page, _pages);
    HPDF_Doc doc = pageH.owner();

    if (width <= 0 || height <= 0) {
      throw std::invalid_argument("Width and height must be positive");
    }
    size_t components = 3;
    HPDF_ColorSpace cs = static_cast<HPDF_ColorSpace>(colorSpace);
    if (cs == HPDF_CS_DEVICE_GRAY) {
      components = 1;
    } else if (cs == HPDF_CS_DEVICE_RGB) {
      components = 3;
    } else if (cs == HPDF_CS_DEVICE_CMYK) {
      components = 4;
    }
    size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * components;
    if (buffer->size() < expected) {
      throw std::invalid_argument("Buffer too small for raw image data");
    }

    HPDF_Image image = HPDF_LoadRawImageFromMem(doc, buffer->data(),
                                                static_cast<HPDF_UINT>(width),
                                                static_cast<HPDF_UINT>(height),
                                                static_cast<HPDF_ColorSpace>(colorSpace),
                                                8);
    throwHaruError(doc);

    throwOnHaruError(HPDF_Page_DrawImage(pageH.get(), image, static_cast<HPDF_REAL>(x),
                                      static_cast<HPDF_REAL>(y), static_cast<HPDF_REAL>(drawWidth),
                                      static_cast<HPDF_REAL>(drawHeight)),
                 doc);
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Coordinate transforms
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::gSave(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_GSave(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::gRestore(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_GRestore(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::concat(double page, double a, double b, double c, double d,
                                                      double x, double y) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, a, b, c, d, x, y]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_Concat(h.get(), static_cast<HPDF_REAL>(a), static_cast<HPDF_REAL>(b),
                                  static_cast<HPDF_REAL>(c), static_cast<HPDF_REAL>(d),
                                  static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y)),
                 h.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Annotations
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::createTextAnnot(double page, const std::vector<double>& rect,
                                                                 const std::string& text,
                                                                 const std::optional<std::string>& encoder) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, page, rect, text, encoder]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    HPDF_Encoder encoderPtr = nullptr;
    if (encoder.has_value() && !encoder->empty()) {
      encoderPtr = HPDF_GetEncoder(h.owner(), encoder->c_str());
      throwHaruError(h.owner());
    }
    HPDF_Annotation annot = HPDF_Page_CreateTextAnnot(h.get(), toRect(rect), text.c_str(), encoderPtr);
    throwHaruError(h.owner());
    return _annotations.registerPointer(annot, h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::createLinkAnnot(double page, const std::vector<double>& rect,
                                                                 double dst) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, page, rect, dst]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    Handle<HPDF_Destination> dstH(dst, _destinations);
    HPDF_Annotation annot = HPDF_Page_CreateLinkAnnot(h.get(), toRect(rect), dstH.get());
    throwHaruError(h.owner());
    return _annotations.registerPointer(annot, h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::createURILinkAnnot(double page, const std::vector<double>& rect,
                                                                    const std::string& uri) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, page, rect, uri]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    HPDF_Annotation annot = HPDF_Page_CreateURILinkAnnot(h.get(), toRect(rect), uri.c_str());
    throwHaruError(h.owner());
    return _annotations.registerPointer(annot, h.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Destinations
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::createDestination(double page) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    HPDF_Destination dst = HPDF_Page_CreateDestination(h.get());
    throwHaruError(h.owner());
    return _destinations.registerPointer(dst, h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setDestinationXYZ(double dst, double x, double y,
                                                                 double zoom) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, dst, x, y, zoom]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Destination> h(dst, _destinations);
    throwOnHaruError(HPDF_Destination_SetXYZ(h.get(), static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y),
                                         static_cast<HPDF_REAL>(zoom)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setDestinationFit(double dst) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, dst]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Destination> h(dst, _destinations);
    throwOnHaruError(HPDF_Destination_SetFit(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setDestinationFitH(double dst, double top) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, dst, top]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Destination> h(dst, _destinations);
    throwOnHaruError(HPDF_Destination_SetFitH(h.get(), static_cast<HPDF_REAL>(top)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setDestinationFitV(double dst, double left) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, dst, left]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Destination> h(dst, _destinations);
    throwOnHaruError(HPDF_Destination_SetFitV(h.get(), static_cast<HPDF_REAL>(left)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setDestinationFitR(double dst, double left, double bottom,
                                                                  double right, double top) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, dst, left, bottom, right, top]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Destination> h(dst, _destinations);
    throwOnHaruError(HPDF_Destination_SetFitR(h.get(), static_cast<HPDF_REAL>(left),
                                          static_cast<HPDF_REAL>(bottom), static_cast<HPDF_REAL>(right),
                                          static_cast<HPDF_REAL>(top)),
                 h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setDestinationFitB(double dst) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, dst]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Destination> h(dst, _destinations);
    throwOnHaruError(HPDF_Destination_SetFitB(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setDestinationFitBH(double dst, double top) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, dst, top]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Destination> h(dst, _destinations);
    throwOnHaruError(HPDF_Destination_SetFitBH(h.get(), static_cast<HPDF_REAL>(top)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setDestinationFitBV(double dst, double left) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, dst, left]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Destination> h(dst, _destinations);
    throwOnHaruError(HPDF_Destination_SetFitBV(h.get(), static_cast<HPDF_REAL>(left)), h.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Outlines
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::createOutline(double doc, double parent,
                                                               const std::string& title,
                                                               const std::optional<std::string>& encoder) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, parent, title, encoder]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> docH(doc, _docs);
    HPDF_Outline parentPointer = nullptr;
    if (parent != 0) {
      parentPointer = _outlines.getPointer(parent);
      if (parentPointer == nullptr) {
        throw std::invalid_argument("Invalid parent outline handle");
      }
    }
    HPDF_Encoder encoderPtr = nullptr;
    if (encoder.has_value() && !encoder->empty()) {
      encoderPtr = HPDF_GetEncoder(docH.get(), encoder->c_str());
      throwHaruError(docH.get());
    }
    HPDF_Outline outline = HPDF_CreateOutline(docH.get(), parentPointer, title.c_str(), encoderPtr);
    throwHaruError(docH.get());
    return _outlines.registerPointer(outline, docH.get());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setOpened(double outline, bool opened) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, outline, opened]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Outline> h(outline, _outlines);
    throwOnHaruError(HPDF_Outline_SetOpened(h.get(), opened ? HPDF_TRUE : HPDF_FALSE), h.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// ExtGState
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::createExtGState(double doc) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_ExtGState state = HPDF_CreateExtGState(h.get());
    throwHaruError(h.get());
    return _extGStates.registerPointer(state, h.get());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setAlphaStroke(double extGState, double alpha) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, extGState, alpha]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_ExtGState> h(extGState, _extGStates);
    throwOnHaruError(HPDF_ExtGState_SetAlphaStroke(h.get(), static_cast<HPDF_REAL>(alpha)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setAlphaFill(double extGState, double alpha) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, extGState, alpha]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_ExtGState> h(extGState, _extGStates);
    throwOnHaruError(HPDF_ExtGState_SetAlphaFill(h.get(), static_cast<HPDF_REAL>(alpha)), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setBlendMode(double extGState, double mode) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, extGState, mode]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_ExtGState> h(extGState, _extGStates);
    throwOnHaruError(HPDF_ExtGState_SetBlendMode(h.get(), static_cast<HPDF_BlendMode>(mode)), h.owner());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setInfoAttr(double doc, double infoType,
                                                           const std::string& value) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc, infoType, value]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    throwOnHaruError(HPDF_SetInfoAttr(h.get(), static_cast<HPDF_InfoType>(infoType), value.c_str()), h.get());
  });
  return promise;
}

std::shared_ptr<Promise<std::string>> HybridNitroPdfWriter::getInfoAttr(double doc, double infoType) {
  auto promise = Promise<std::string>::create();
  HaruWorker::getInstance().run<std::string>(promise, [this, doc, infoType]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    const char* value = HPDF_GetInfoAttr(h.get(), static_cast<HPDF_InfoType>(infoType));
    return std::string(value ? value : "");
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setInfoDateAttr(double doc, double infoType,
                                                               const std::string& value) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc, infoType, value]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_Date date{};
    // Parse a tiny subset: "YYYY-MM-DD HH:MM:SS"
    if (std::sscanf(value.c_str(), "%d-%d-%d %d:%d:%d", &date.year, &date.month, &date.day,
                &date.hour, &date.minutes, &date.seconds) != 6) {
      throw std::invalid_argument("Invalid date format, expected YYYY-MM-DD HH:MM:SS");
    }
    throwOnHaruError(HPDF_SetInfoDateAttr(h.get(), static_cast<HPDF_InfoType>(infoType), date), h.get());
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::pageTextWidth(double page, const std::string& text) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, page, text]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    HPDF_REAL width = HPDF_Page_TextWidth(h.get(), text.c_str());
    throwHaruError(h.owner());
    return static_cast<double>(width);
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::pageTextHeight(double page, const std::string& text) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, page, text]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    // libHaru does not expose a direct Page_TextHeight API. Approximate the
    // text height from the current font metrics and the text length.
    HPDF_Font font = HPDF_Page_GetCurrentFont(h.get());
    HPDF_REAL size = HPDF_Page_GetCurrentFontSize(h.get());
    if (font == nullptr || size <= 0) {
      throw std::runtime_error("No font selected on page");
    }
    HPDF_INT ascent = HPDF_Font_GetAscent(font);
    HPDF_INT descent = HPDF_Font_GetDescent(font);
    HPDF_REAL lineHeight = (ascent - descent) * size / 1000.0f;
    HPDF_REAL textWidth = HPDF_Page_TextWidth(h.get(), text.c_str());
    HPDF_REAL pageWidth = HPDF_Page_GetWidth(h.get());
    HPDF_UINT lines = pageWidth > 0 ? static_cast<HPDF_UINT>(std::ceil(textWidth / pageWidth)) : 1;
    lines = std::max(lines, 1u);
    throwHaruError(h.owner());
    return static_cast<double>(lines * lineHeight);
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::pageMeasureText(double page, const std::string& text,
                                                                 double width,
                                                                 std::optional<bool> wordwrap) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, page, text, width, wordwrap]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    HPDF_BOOL wrap = wordwrap.value_or(HPDF_FALSE);
    HPDF_REAL realWidth = 0;
    HPDF_Page_MeasureText(h.get(), text.c_str(), static_cast<HPDF_REAL>(width), wrap, &realWidth);
    throwHaruError(h.owner());
    return static_cast<double>(realWidth);
  });
  return promise;
}

// ---------------------------------------------------------------------------
// Quick Draw (High-level API)
// ---------------------------------------------------------------------------

static double convertToPt(double value, const std::string& unit, double dpi = 72.0) {
    if (unit == "mm") return value * (72.0 / 25.4);
    if (unit == "cm") return value * (72.0 / 2.54);
    if (unit == "in") return value * 72.0;
    if (unit == "px") return (value / dpi) * 72.0;
    return value; // "pt" or default
}

static HPDF_ColorSpace getColorSpace(const std::string& cs) {
    if (cs == "rgb" || cs == "RGB") return HPDF_CS_DEVICE_RGB;
    if (cs == "gray" || cs == "Gray" || cs == "GRAY") return HPDF_CS_DEVICE_GRAY;
    if (cs == "cmyk" || cs == "CMYK") return HPDF_CS_DEVICE_CMYK;
    return HPDF_CS_DEVICE_RGB;
}

static HPDF_TextAlignment getAlign(const std::string& align) {
    if (align == "right") return HPDF_TALIGN_RIGHT;
    if (align == "center") return HPDF_TALIGN_CENTER;
    if (align == "justify") return HPDF_TALIGN_JUSTIFY;
    return HPDF_TALIGN_LEFT;
}

static HPDF_LineCap getLineCap(const std::string& cap) {
    if (cap == "round") return HPDF_ROUND_END;
    if (cap == "projectingSquare") return HPDF_PROJECTING_SQUARE_END;
    return HPDF_BUTT_END;
}

static HPDF_LineJoin getLineJoin(const std::string& join) {
    if (join == "round") return HPDF_ROUND_JOIN;
    if (join == "bevel") return HPDF_BEVEL_JOIN;
    return HPDF_MITER_JOIN;
}

void HybridNitroPdfWriter::executeOperation(
    HPDF_Doc doc, HPDF_Page& page, const std::shared_ptr<AnyMap>& op,
    const std::string& defaultUnit, double dpi, const QuickDrawMedia* media) {

    std::string type = op->getString("type");
    if (type.empty()) return;

    if (type == "page") {
        std::string unit = op->contains("unit") ? op->getString("unit") : defaultUnit;

        page = HPDF_AddPage(doc);
        throwHaruError(doc);
        if (!page) throw std::runtime_error("Failed to add page");
        if (op->contains("width") && op->contains("height")) {
            HPDF_Page_SetWidth(page, static_cast<HPDF_REAL>(convertToPt(anyToDouble(op->getAny("width")), unit, dpi)));
            HPDF_Page_SetHeight(page, static_cast<HPDF_REAL>(convertToPt(anyToDouble(op->getAny("height")), unit, dpi)));
        } else if (op->contains("size")) {
            std::string sizeStr = op->getString("size");
            std::transform(sizeStr.begin(), sizeStr.end(), sizeStr.begin(), ::tolower);
            HPDF_PageSizes size = HPDF_PAGE_SIZE_A4;
            if (sizeStr == "letter") size = HPDF_PAGE_SIZE_LETTER;
            else if (sizeStr == "legal") size = HPDF_PAGE_SIZE_LEGAL;
            else if (sizeStr == "a3") size = HPDF_PAGE_SIZE_A3;
            else if (sizeStr == "a4") size = HPDF_PAGE_SIZE_A4;
            else if (sizeStr == "a5") size = HPDF_PAGE_SIZE_A5;
            else if (sizeStr == "b4") size = HPDF_PAGE_SIZE_B4;
            else if (sizeStr == "b5") size = HPDF_PAGE_SIZE_B5;

            HPDF_PageDirection direction = HPDF_PAGE_PORTRAIT;
            if (op->contains("direction") && op->getString("direction") == "landscape") {
                direction = HPDF_PAGE_LANDSCAPE;
            }
            HPDF_Page_SetSize(page, size, direction);
        } else if (op->contains("direction") && op->getString("direction") == "landscape") {
            HPDF_Page_SetWidth(page, 841.89f);
            HPDF_Page_SetHeight(page, 595.28f);
        } else {
            HPDF_Page_SetWidth(page, 595.28f);
            HPDF_Page_SetHeight(page, 841.89f);
        }
    }
    else if (type == "font" && op->contains("data")) {
        auto data = op->getObject("data");

        // Explicit guard — must run before any data.at(...) so a missing
        // identifier does not surface as an opaque map::at error.
        if (!data.contains("media") && !data.contains("filePath") && !data.contains("family") &&
            !data.contains("name")) {
            throw std::invalid_argument("Font operation requires media, filePath, family or name");
        }

        double fontSize = anyToDouble(data.at("size"));

        std::string unit = data.contains("unit") ? anyToString(data.at("unit")) : defaultUnit;
        fontSize = convertToPt(fontSize, unit, dpi);

        std::string encodingStr;
        const char* encoding = nullptr;
        if (data.contains("encoding")) {
          encodingStr = anyToString(data.at("encoding"));
          encoding = encodingStr.c_str();
        }

        HPDF_Font font = nullptr;
        if (data.contains("media")) {
            // Two-step API: attach a pre-loaded (optionally cached) font media handle.
            double mediaHandle = anyToDouble(data.at("media"));
            auto blob = _media.get(mediaHandle);
            if (!blob) {
                throw std::invalid_argument("Invalid font media handle (already freed?)");
            }
            bool embed = !data.contains("embedding") || anyToBool(data.at("embedding"));
            double fontHandle = installFontBlob(doc, blob, embed, encoding);
            font = _fonts.getPointer(fontHandle);
        } else if (data.contains("filePath")) {
            std::string path = anyToString(data.at("filePath"));

            std::string ext = path.length() >= 4 ? path.substr(path.length() - 4) : "";
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".otf") {
                throw std::runtime_error("OTF fonts with CFF outlines are not supported by libharu. Please convert to TTF or use a TTC file instead.");
            }

            HPDF_UINT index = data.contains("fontIndex") ? static_cast<HPDF_UINT>(anyToDouble(data.at("fontIndex"))) : 0;
            // Prefer pre-parsed TTC index / glyph metrics from the media prep pass.
            std::shared_ptr<MediaBlob> prep;
            if (media != nullptr) {
                std::string key = path + "#" + std::to_string(index);
                auto it = media->fonts.find(key);
                if (it != media->fonts.end()) prep = it->second;
            }
            if (!prep) {
                prep = FontPrep::loadFromFile(path, index);
            }

            bool embed = !data.contains("embedding") || anyToBool(data.at("embedding"));
            double fontHandle = installFontBlob(doc, prep, embed, encoding);
            font = _fonts.getPointer(fontHandle);
        } else {
            // `family` is the preferred key; `name` kept as an alias.
            std::string fontName = data.contains("family") ? anyToString(data.at("family"))
                                                           : anyToString(data.at("name"));
            font = HPDF_GetFont(doc, fontName.c_str(), encoding);
        }
        throwHaruError(doc);
        HPDF_Page_SetFontAndSize(page, font, static_cast<HPDF_REAL>(fontSize));
        throwHaruError(doc);
    }
    else if (type == "color" && op->contains("data")) {
        auto data = op->getObject("data");

        std::string target = op->contains("target") ? op->getString("target") : "both";

        if (data.contains("r") && data.contains("g") && data.contains("b")) {
            float r = anyToFloat(data.at("r"));
            float g = anyToFloat(data.at("g"));
            float b = anyToFloat(data.at("b"));
            if (target == "fill" || target == "both") HPDF_Page_SetRGBFill(page, r, g, b);
            if (target == "stroke" || target == "both") HPDF_Page_SetRGBStroke(page, r, g, b);
        } else if (data.contains("gray")) {
            float gray = anyToFloat(data.at("gray"));
            if (target == "fill" || target == "both") HPDF_Page_SetGrayFill(page, gray);
            if (target == "stroke" || target == "both") HPDF_Page_SetGrayStroke(page, gray);
        } else if (data.contains("c") && data.contains("m") && data.contains("y") && data.contains("k")) {
            float c = anyToFloat(data.at("c"));
            float m = anyToFloat(data.at("m"));
            float y = anyToFloat(data.at("y"));
            float k = anyToFloat(data.at("k"));
            if (target == "fill" || target == "both") HPDF_Page_SetCMYKFill(page, c, m, y, k);
            if (target == "stroke" || target == "both") HPDF_Page_SetCMYKStroke(page, c, m, y, k);
        }
        throwHaruError(doc);
    }
    else if (type == "text" && op->contains("data")) {
        auto data = op->getObject("data");

        std::string content = anyToString(data.at("content"));
        std::string unit = data.contains("unit") ? anyToString(data.at("unit")) : defaultUnit;

        double x = convertToPt(anyToDouble(data.at("x")), unit, dpi);
        double y = convertToPt(anyToDouble(data.at("y")), unit, dpi);

        if (data.contains("font")) {
            auto fontObj = std::get<AnyObject>(data.at("font"));

            if (fontObj.find("media") == fontObj.end() && fontObj.find("filePath") == fontObj.end() &&
                fontObj.find("family") == fontObj.end() && fontObj.find("name") == fontObj.end()) {
                throw std::invalid_argument("Font operation requires media, filePath, family or name");
            }

            double fontSize = anyToDouble(fontObj.at("size"));
            std::string fontUnit = fontObj.contains("unit") ? anyToString(fontObj.at("unit")) : unit;
            fontSize = convertToPt(fontSize, fontUnit, dpi);

            HPDF_Font hFont = nullptr;
            if (fontObj.contains("media")) {
                double mediaHandle = anyToDouble(fontObj.at("media"));
                auto blob = _media.get(mediaHandle);
                if (!blob) {
                    throw std::invalid_argument("Invalid font media handle (already freed?)");
                }
                std::string encodingStr;
                const char* encoding = nullptr;
                if (fontObj.contains("encoding")) {
                    encodingStr = anyToString(fontObj.at("encoding"));
                    encoding = encodingStr.c_str();
                }
                bool embed = !fontObj.contains("embedding") || anyToBool(fontObj.at("embedding"));
                double fontHandle = installFontBlob(doc, blob, embed, encoding);
                hFont = _fonts.getPointer(fontHandle);
            } else if (fontObj.contains("filePath")) {
                std::string path = anyToString(fontObj.at("filePath"));
                HPDF_UINT index = fontObj.contains("fontIndex")
                    ? static_cast<HPDF_UINT>(anyToDouble(fontObj.at("fontIndex"))) : 0;
                std::shared_ptr<MediaBlob> prep;
                if (media != nullptr) {
                    auto it = media->fonts.find(path + "#" + std::to_string(index));
                    if (it != media->fonts.end()) prep = it->second;
                }
                if (!prep) {
                    prep = FontPrep::loadFromFile(path, index);
                }
                bool embed = !fontObj.contains("embedding") || anyToBool(fontObj.at("embedding"));
                double fontHandle = installFontBlob(doc, prep, embed, nullptr);
                hFont = _fonts.getPointer(fontHandle);
            } else {
                std::string fontName = fontObj.contains("family") ? anyToString(fontObj.at("family"))
                                                                  : anyToString(fontObj.at("name"));
                hFont = HPDF_GetFont(doc, fontName.c_str(), nullptr);
            }
            throwHaruError(doc);
            HPDF_Page_SetFontAndSize(page, hFont, static_cast<HPDF_REAL>(fontSize));
            throwHaruError(doc);
        }

        if (data.contains("color")) {
            auto color = std::get<AnyObject>(data.at("color"));
            if (color.contains("r") && color.contains("g") && color.contains("b")) {
                HPDF_Page_SetRGBFill(page, anyToFloat(color.at("r")),
                                     anyToFloat(color.at("g")),
                                     anyToFloat(color.at("b")));
            } else if (color.contains("gray")) {
                HPDF_Page_SetGrayFill(page, anyToFloat(color.at("gray")));
            } else if (color.contains("c") && color.contains("m") && color.contains("y") && color.contains("k")) {
                HPDF_Page_SetCMYKFill(page, anyToFloat(color.at("c")),
                                      anyToFloat(color.at("m")),
                                      anyToFloat(color.at("y")),
                                      anyToFloat(color.at("k")));
            }
        }

        HPDF_Page_BeginText(page);
        throwHaruError(doc);

        if (data.contains("width") && data.contains("height")) {
            double w = convertToPt(anyToDouble(data.at("width")), unit, dpi);
            double h = convertToPt(anyToDouble(data.at("height")), unit, dpi);
            HPDF_TextAlignment align = data.contains("align") ?
                                       getAlign(anyToString(data.at("align"))) : HPDF_TALIGN_LEFT;
            HPDF_Page_TextRect(page, static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y),
                              static_cast<HPDF_REAL>(x + w), static_cast<HPDF_REAL>(y - h),
                              content.c_str(), align, nullptr);
        } else {
            HPDF_Page_TextOut(page, static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y), content.c_str());
        }
        throwHaruError(doc);

        HPDF_Page_EndText(page);
        throwHaruError(doc);
    }
    else if (type == "image" && op->contains("data")) {
        auto data = op->getObject("data");

        std::string unit = data.contains("unit") ? anyToString(data.at("unit")) : defaultUnit;

        double x = convertToPt(anyToDouble(data.at("x")), unit, dpi);
        double y = convertToPt(anyToDouble(data.at("y")), unit, dpi);
        double width = convertToPt(anyToDouble(data.at("width")), unit, dpi);
        double height = convertToPt(anyToDouble(data.at("height")), unit, dpi);

        if (!data.contains("source") && !data.contains("media")) {
            throw std::invalid_argument("Image operation requires source or media");
        }

        std::shared_ptr<MediaBlob> blob;
        if (data.contains("media")) {
            // Two-step API: attach a pre-loaded (optionally cached) image media handle.
            double mediaHandle = anyToDouble(data.at("media"));
            blob = _media.get(mediaHandle);
            if (!blob) {
                throw std::invalid_argument("Invalid image media handle (already freed?)");
            }
        } else {
            std::string source = anyToString(data.at("source"));
            bool useCache = data.contains("useCache") && anyToBool(data.at("useCache"));
            std::string format = data.contains("format") ? anyToString(data.at("format")) : "";

            MediaBlob::Kind kind = MediaBlob::Kind::Png;
            if (!format.empty()) {
                kind = (format == "jpeg") ? MediaBlob::Kind::Jpeg : MediaBlob::Kind::Png;
            } else {
                std::string ext = std::filesystem::path(source).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                kind = (ext == ".jpg" || ext == ".jpeg") ? MediaBlob::Kind::Jpeg : MediaBlob::Kind::Png;
            }

            // Prefer the media prep pass (I/O + header parse already done outside the lock).
            if (media != nullptr) {
                auto it = media->images.find(source);
                if (it != media->images.end()) blob = it->second;
            }
            if (!blob) {
                blob = MediaPrep::prepareImageFile(source, kind, useCache);
            }
        }

        double imageHandle = installImageBlob(doc, blob);
        HPDF_Image image = _images.getPointer(imageHandle);
        if (!image) throw std::runtime_error("Failed to load image");

        HPDF_Page_DrawImage(page, image, static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y),
                           static_cast<HPDF_REAL>(width), static_cast<HPDF_REAL>(height));
        throwHaruError(doc);
    }
    else if (type == "line" && op->contains("data")) {
        auto data = op->getObject("data");

        std::string unit = data.contains("unit") ? anyToString(data.at("unit")) : defaultUnit;

        double x1 = convertToPt(anyToDouble(data.at("x1")), unit, dpi);
        double y1 = convertToPt(anyToDouble(data.at("y1")), unit, dpi);
        double x2 = convertToPt(anyToDouble(data.at("x2")), unit, dpi);
        double y2 = convertToPt(anyToDouble(data.at("y2")), unit, dpi);

        if (data.contains("lineWidth")) {
            HPDF_Page_SetLineWidth(page, static_cast<HPDF_REAL>(convertToPt(anyToDouble(data.at("lineWidth")), unit, dpi)));
        }

        if (data.contains("lineCap")) {
            HPDF_Page_SetLineCap(page, getLineCap(anyToString(data.at("lineCap"))));
        }

        if (data.contains("lineJoin")) {
            HPDF_Page_SetLineJoin(page, getLineJoin(anyToString(data.at("lineJoin"))));
        }

        if (data.contains("color")) {
            auto color = std::get<AnyObject>(data.at("color"));
            if (color.contains("r") && color.contains("g") && color.contains("b")) {
                HPDF_Page_SetRGBStroke(page, anyToFloat(color.at("r")),
                                       anyToFloat(color.at("g")),
                                       anyToFloat(color.at("b")));
            } else if (color.contains("gray")) {
                HPDF_Page_SetGrayStroke(page, anyToFloat(color.at("gray")));
            } else if (color.contains("c") && color.contains("m") && color.contains("y") && color.contains("k")) {
                HPDF_Page_SetCMYKStroke(page, anyToFloat(color.at("c")),
                                        anyToFloat(color.at("m")),
                                        anyToFloat(color.at("y")),
                                        anyToFloat(color.at("k")));
            }
        }

        HPDF_Page_MoveTo(page, static_cast<HPDF_REAL>(x1), static_cast<HPDF_REAL>(y1));
        HPDF_Page_LineTo(page, static_cast<HPDF_REAL>(x2), static_cast<HPDF_REAL>(y2));
        HPDF_Page_Stroke(page);
        throwHaruError(doc);
    }
    else if (type == "rectangle" && op->contains("data")) {
        auto data = op->getObject("data");

        std::string unit = data.contains("unit") ? anyToString(data.at("unit")) : defaultUnit;

        double x = convertToPt(anyToDouble(data.at("x")), unit, dpi);
        double y = convertToPt(anyToDouble(data.at("y")), unit, dpi);
        double width = convertToPt(anyToDouble(data.at("width")), unit, dpi);
        double height = convertToPt(anyToDouble(data.at("height")), unit, dpi);

        if (data.contains("lineWidth")) {
            HPDF_Page_SetLineWidth(page, static_cast<HPDF_REAL>(convertToPt(anyToDouble(data.at("lineWidth")), unit, dpi)));
        }

        if (data.contains("fillColor")) {
            auto color = std::get<AnyObject>(data.at("fillColor"));
            if (color.contains("r") && color.contains("g") && color.contains("b")) {
                HPDF_Page_SetRGBFill(page, anyToFloat(color.at("r")),
                                       anyToFloat(color.at("g")),
                                       anyToFloat(color.at("b")));
            } else if (color.contains("gray")) {
                HPDF_Page_SetGrayFill(page, anyToFloat(color.at("gray")));
            } else if (color.contains("c") && color.contains("m") && color.contains("y") && color.contains("k")) {
                HPDF_Page_SetCMYKFill(page, anyToFloat(color.at("c")),
                                      anyToFloat(color.at("m")),
                                      anyToFloat(color.at("y")),
                                      anyToFloat(color.at("k")));
            }
        }

        if (data.contains("strokeColor")) {
            auto color = std::get<AnyObject>(data.at("strokeColor"));
            if (color.contains("r") && color.contains("g") && color.contains("b")) {
                HPDF_Page_SetRGBStroke(page, anyToFloat(color.at("r")),
                                       anyToFloat(color.at("g")),
                                       anyToFloat(color.at("b")));
            } else if (color.contains("gray")) {
                HPDF_Page_SetGrayStroke(page, anyToFloat(color.at("gray")));
            } else if (color.contains("c") && color.contains("m") && color.contains("y") && color.contains("k")) {
                HPDF_Page_SetCMYKStroke(page, anyToFloat(color.at("c")),
                                        anyToFloat(color.at("m")),
                                        anyToFloat(color.at("y")),
                                        anyToFloat(color.at("k")));
            }
        }

        HPDF_Page_Rectangle(page, static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y),
                            static_cast<HPDF_REAL>(width), static_cast<HPDF_REAL>(height));

        bool fill = data.contains("fill") && anyToBool(data.at("fill"));
        bool stroke = !data.contains("stroke") || anyToBool(data.at("stroke"));

        if (fill && stroke) {
            HPDF_Page_FillStroke(page);
        } else if (fill) {
            HPDF_Page_Fill(page);
        } else if (stroke) {
            HPDF_Page_Stroke(page);
        } else {
            HPDF_Page_EndPath(page);
        }
        throwHaruError(doc);
    }
    else if (type == "circle" && op->contains("data")) {
        auto data = op->getObject("data");

        std::string unit = data.contains("unit") ? anyToString(data.at("unit")) : defaultUnit;

        double x = convertToPt(anyToDouble(data.at("x")), unit, dpi);
        double y = convertToPt(anyToDouble(data.at("y")), unit, dpi);
        double radius = convertToPt(anyToDouble(data.at("radius")), unit, dpi);

        if (data.contains("lineWidth")) {
            HPDF_Page_SetLineWidth(page, static_cast<HPDF_REAL>(convertToPt(anyToDouble(data.at("lineWidth")), unit, dpi)));
        }

        if (data.contains("fillColor")) {
            auto color = std::get<AnyObject>(data.at("fillColor"));
            if (color.contains("r") && color.contains("g") && color.contains("b")) {
                HPDF_Page_SetRGBFill(page, anyToFloat(color.at("r")),
                                       anyToFloat(color.at("g")),
                                       anyToFloat(color.at("b")));
            } else if (color.contains("gray")) {
                HPDF_Page_SetGrayFill(page, anyToFloat(color.at("gray")));
            } else if (color.contains("c") && color.contains("m") && color.contains("y") && color.contains("k")) {
                HPDF_Page_SetCMYKFill(page, anyToFloat(color.at("c")),
                                      anyToFloat(color.at("m")),
                                      anyToFloat(color.at("y")),
                                      anyToFloat(color.at("k")));
            }
        }

        if (data.contains("strokeColor")) {
            auto color = std::get<AnyObject>(data.at("strokeColor"));
            if (color.contains("r") && color.contains("g") && color.contains("b")) {
                HPDF_Page_SetRGBStroke(page, anyToFloat(color.at("r")),
                                       anyToFloat(color.at("g")),
                                       anyToFloat(color.at("b")));
            } else if (color.contains("gray")) {
                HPDF_Page_SetGrayStroke(page, anyToFloat(color.at("gray")));
            } else if (color.contains("c") && color.contains("m") && color.contains("y") && color.contains("k")) {
                HPDF_Page_SetCMYKStroke(page, anyToFloat(color.at("c")),
                                        anyToFloat(color.at("m")),
                                        anyToFloat(color.at("y")),
                                        anyToFloat(color.at("k")));
            }
        }

        HPDF_Page_Circle(page, static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y),
                         static_cast<HPDF_REAL>(radius));

        bool fill = data.contains("fill") && anyToBool(data.at("fill"));
        bool stroke = !data.contains("stroke") || anyToBool(data.at("stroke"));

        if (fill && stroke) {
            HPDF_Page_FillStroke(page);
        } else if (fill) {
            HPDF_Page_Fill(page);
        } else if (stroke) {
            HPDF_Page_Stroke(page);
        } else {
            HPDF_Page_EndPath(page);
        }
        throwHaruError(doc);
    }
    else if (type == "ellipse" && op->contains("data")) {
        auto data = op->getObject("data");

        std::string unit = data.contains("unit") ? anyToString(data.at("unit")) : defaultUnit;

        double x = convertToPt(anyToDouble(data.at("x")), unit, dpi);
        double y = convertToPt(anyToDouble(data.at("y")), unit, dpi);
        double xRadius = convertToPt(anyToDouble(data.at("xRadius")), unit, dpi);
        double yRadius = convertToPt(anyToDouble(data.at("yRadius")), unit, dpi);

        if (data.contains("lineWidth")) {
            HPDF_Page_SetLineWidth(page, static_cast<HPDF_REAL>(convertToPt(anyToDouble(data.at("lineWidth")), unit, dpi)));
        }

        if (data.contains("fillColor")) {
            auto color = std::get<AnyObject>(data.at("fillColor"));
            if (color.contains("r") && color.contains("g") && color.contains("b")) {
                HPDF_Page_SetRGBFill(page, anyToFloat(color.at("r")),
                                       anyToFloat(color.at("g")),
                                       anyToFloat(color.at("b")));
            } else if (color.contains("gray")) {
                HPDF_Page_SetGrayFill(page, anyToFloat(color.at("gray")));
            } else if (color.contains("c") && color.contains("m") && color.contains("y") && color.contains("k")) {
                HPDF_Page_SetCMYKFill(page, anyToFloat(color.at("c")),
                                      anyToFloat(color.at("m")),
                                      anyToFloat(color.at("y")),
                                      anyToFloat(color.at("k")));
            }
        }

        if (data.contains("strokeColor")) {
            auto color = std::get<AnyObject>(data.at("strokeColor"));
            if (color.contains("r") && color.contains("g") && color.contains("b")) {
                HPDF_Page_SetRGBStroke(page, anyToFloat(color.at("r")),
                                       anyToFloat(color.at("g")),
                                       anyToFloat(color.at("b")));
            } else if (color.contains("gray")) {
                HPDF_Page_SetGrayStroke(page, anyToFloat(color.at("gray")));
            } else if (color.contains("c") && color.contains("m") && color.contains("y") && color.contains("k")) {
                HPDF_Page_SetCMYKStroke(page, anyToFloat(color.at("c")),
                                        anyToFloat(color.at("m")),
                                        anyToFloat(color.at("y")),
                                        anyToFloat(color.at("k")));
            }
        }

        HPDF_Page_Ellipse(page, static_cast<HPDF_REAL>(x), static_cast<HPDF_REAL>(y),
                          static_cast<HPDF_REAL>(xRadius), static_cast<HPDF_REAL>(yRadius));

        bool fill = data.contains("fill") && anyToBool(data.at("fill"));
        bool stroke = !data.contains("stroke") || anyToBool(data.at("stroke"));

        if (fill && stroke) {
            HPDF_Page_FillStroke(page);
        } else if (fill) {
            HPDF_Page_Fill(page);
        } else if (stroke) {
            HPDF_Page_Stroke(page);
        } else {
            HPDF_Page_EndPath(page);
        }
        throwHaruError(doc);
    }
    else if (type == "path" && op->contains("data")) {
        auto data = op->getObject("data");

        auto points = std::get<AnyArray>(data.at("points"));
        if (points.size() < 2) return;

        std::string unit = data.contains("unit") ? anyToString(data.at("unit")) : defaultUnit;

        if (data.contains("lineWidth")) {
            HPDF_Page_SetLineWidth(page, static_cast<HPDF_REAL>(convertToPt(anyToDouble(data.at("lineWidth")), unit, dpi)));
        }

        if (data.contains("color")) {
            auto color = std::get<AnyObject>(data.at("color"));
            if (color.contains("r") && color.contains("g") && color.contains("b")) {
                HPDF_Page_SetRGBFill(page, anyToFloat(color.at("r")),
                                     anyToFloat(color.at("g")),
                                     anyToFloat(color.at("b")));
                HPDF_Page_SetRGBStroke(page, anyToFloat(color.at("r")),
                                       anyToFloat(color.at("g")),
                                       anyToFloat(color.at("b")));
            } else if (color.contains("gray")) {
                float gray = anyToFloat(color.at("gray"));
                HPDF_Page_SetGrayFill(page, gray);
                HPDF_Page_SetGrayStroke(page, gray);
            } else if (color.contains("c") && color.contains("m") && color.contains("y") && color.contains("k")) {
                float c = anyToFloat(color.at("c"));
                float m = anyToFloat(color.at("m"));
                float y = anyToFloat(color.at("y"));
                float k = anyToFloat(color.at("k"));
                HPDF_Page_SetCMYKFill(page, c, m, y, k);
                HPDF_Page_SetCMYKStroke(page, c, m, y, k);
            }
        }

        auto firstPt = std::get<AnyObject>(points[0]);
        double fx = convertToPt(anyToDouble(firstPt.at("x")), unit, dpi);
        double fy = convertToPt(anyToDouble(firstPt.at("y")), unit, dpi);

        HPDF_Page_MoveTo(page, static_cast<HPDF_REAL>(fx), static_cast<HPDF_REAL>(fy));

        for (size_t i = 1; i < points.size(); i++) {
            auto pt = std::get<AnyObject>(points[i]);
            double px = convertToPt(anyToDouble(pt.at("x")), unit, dpi);
            double py = convertToPt(anyToDouble(pt.at("y")), unit, dpi);
            HPDF_Page_LineTo(page, static_cast<HPDF_REAL>(px), static_cast<HPDF_REAL>(py));
        }

        bool close = data.contains("close") && anyToBool(data.at("close"));
        if (close) HPDF_Page_ClosePath(page);

        bool fill = data.contains("fill") && anyToBool(data.at("fill"));
        bool stroke = !data.contains("stroke") || anyToBool(data.at("stroke"));

        if (fill && stroke) {
            HPDF_Page_FillStroke(page);
        } else if (fill) {
            HPDF_Page_Fill(page);
        } else if (stroke) {
            HPDF_Page_Stroke(page);
        } else {
            HPDF_Page_EndPath(page);
        }
        throwHaruError(doc);
    }
    else if (type == "rotate") {
        if (op->contains("angle")) {
            HPDF_Page_SetRotate(page, static_cast<HPDF_UINT16>(anyToDouble(op->getAny("angle"))));
        }
        throwHaruError(doc);
    }
    else if (type == "gSave") {
        HPDF_Page_GSave(page);
        throwHaruError(doc);
    }
    else if (type == "gRestore") {
        HPDF_Page_GRestore(page);
        throwHaruError(doc);
    }
    else if (type == "transform") {
        if (op->contains("a") && op->contains("b") && op->contains("c") &&
            op->contains("d") && op->contains("x") && op->contains("y")) {
            std::string unit = op->contains("unit") ? op->getString("unit") : defaultUnit;
            HPDF_Page_Concat(page,
                            static_cast<HPDF_REAL>(anyToDouble(op->getAny("a"))),
                            static_cast<HPDF_REAL>(anyToDouble(op->getAny("b"))),
                            static_cast<HPDF_REAL>(anyToDouble(op->getAny("c"))),
                            static_cast<HPDF_REAL>(anyToDouble(op->getAny("d"))),
                            static_cast<HPDF_REAL>(convertToPt(anyToDouble(op->getAny("x")), unit, dpi)),
                            static_cast<HPDF_REAL>(convertToPt(anyToDouble(op->getAny("y")), unit, dpi)));
            throwHaruError(doc);
        }
    }
}

// Pre-parse file-based image/font sources outside HaruLock. Ops that already
// carry a `media` handle skip this — the blob lives in MediaRegistry.
static void preparseQuickDrawOps(const std::vector<std::shared_ptr<AnyMap>>& operations,
                                 HybridNitroPdfWriter::QuickDrawMedia& media) {
  auto prepFontData = [&](const AnyObject& obj) {
    if (obj.find("media") != obj.end()) {
      return; // already loaded via loadFontFrom*
    }
    if (obj.find("filePath") == obj.end()) {
      return;
    }
    std::string path = anyToString(obj.at("filePath"));
    HPDF_UINT index =
        obj.find("fontIndex") != obj.end() ? static_cast<HPDF_UINT>(anyToDouble(obj.at("fontIndex"))) : 0;
    media.fonts[path + "#" + std::to_string(index)] = FontPrep::loadFromFile(path, index);
  };

  for (const auto& op : operations) {
    if (!op->contains("type")) continue;
    std::string type = op->getString("type");
    if (type == "image" && op->contains("data")) {
      auto data = op->getObject("data");
      if (data.find("media") != data.end()) continue; // already loaded
      if (data.find("source") == data.end()) continue;
      std::string source = anyToString(data.at("source"));
      bool useCache = data.contains("useCache") && anyToBool(data.at("useCache"));
      std::string format = data.contains("format") ? anyToString(data.at("format")) : "";
      MediaBlob::Kind kind = MediaBlob::Kind::Png;
      if (!format.empty()) {
        kind = (format == "jpeg") ? MediaBlob::Kind::Jpeg : MediaBlob::Kind::Png;
      } else {
        std::string ext = std::filesystem::path(source).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        kind = (ext == ".jpg" || ext == ".jpeg") ? MediaBlob::Kind::Jpeg : MediaBlob::Kind::Png;
      }
      media.images[source] = MediaPrep::prepareImageFile(source, kind, useCache);
    } else if (type == "font" && op->contains("data")) {
      prepFontData(op->getObject("data"));
    } else if (type == "text" && op->contains("data")) {
      auto data = op->getObject("data");
      if (data.find("font") != data.end()) {
        auto fontVal = data.at("font");
        if (std::holds_alternative<AnyObject>(fontVal)) {
          prepFontData(std::get<AnyObject>(fontVal));
        }
      }
    }
  }
}

std::shared_ptr<Promise<std::variant<std::string, double>>> HybridNitroPdfWriter::quickDraw(
    const std::vector<std::shared_ptr<AnyMap>>& operations, const std::string& unit,
    const std::optional<std::string>& outputPath, std::optional<double> dpi) {

    auto promise = Promise<std::variant<std::string, double>>::create();
    // MediaWorker: pre-parse fonts/images OUTSIDE HaruLock, then install under the lock.
    MediaWorker::getInstance().run<std::variant<std::string, double>>(promise,
        [this, operations, unit, outputPath, dpi]() {
            double dpiVal = dpi.value_or(72.0);
            QuickDrawMedia media;
            preparseQuickDrawOps(operations, media);

            std::lock_guard<std::mutex> lock(HaruLock::get());

            // Create document
            HPDF_Doc doc = HPDF_New(nullptr, nullptr);
            if (doc == nullptr) {
                throw std::runtime_error("Failed to create HPDF document");
            }
            if (HPDF_UseUTFEncodings(doc) != HPDF_OK) {
                throw std::runtime_error("Failed to enable UTF-8 encodings");
            }

            try {
                HPDF_Page page = nullptr;

                for (const auto& op : operations) {
                    // If no page exists yet and this isn't a page operation, create a default one
                    if (page == nullptr && !(op->contains("type") && op->getString("type") == "page")) {
                        page = HPDF_AddPage(doc);
                        throwHaruError(doc);
                    }
                    // executeOperation takes page by reference; "page" ops create a new page internally
                    executeOperation(doc, page, op, unit, dpiVal, &media);
                }

                if (outputPath.has_value()) {
                    // Save to file
                    HPDF_SaveToFile(doc, outputPath->c_str());
                    throwHaruError(doc);
                    HPDF_Free(doc);
                    return std::variant<std::string, double>(outputPath.value());
                } else {
                    // Return doc handle
                    double handle = _docs.registerPointer(doc, nullptr);
                    return std::variant<std::string, double>(handle);
                }
            } catch (...) {
                HPDF_Free(doc);
                throw;
            }
        });

    return promise;
}

std::shared_ptr<Promise<std::vector<std::variant<std::string, double>>>> HybridNitroPdfWriter::quickBatchDraw(
    const std::vector<std::shared_ptr<AnyMap>>& items, std::optional<double> dpi) {

    auto promise = Promise<std::vector<std::variant<std::string, double>>>::create();
    MediaWorker::getInstance().run<std::vector<std::variant<std::string, double>>>(promise,
        [this, items, dpi]() {
            double dpiVal = dpi.value_or(72.0);
            std::vector<std::variant<std::string, double>> results;

            for (const auto& item : items) {
                try {
                    // Get unit
                    std::string unit = "pt";
                    if (item->contains("unit")) {
                        unit = item->getString("unit");
                    }

                    // Get operations
                    if (!item->contains("operations")) {
                        results.push_back(std::string("Missing or invalid operations"));
                        continue;
                    }

                    auto opsArr = item->getArray("operations");
                    std::vector<std::shared_ptr<AnyMap>> ops;
                    for (auto& v : opsArr) {
                        if (std::holds_alternative<AnyObject>(v)) {
                            auto m = AnyMap::make();
                            for (auto& [k, val] : std::get<AnyObject>(v)) m->setAny(k, val);
                            ops.push_back(m);
                        }
                    }

                    // Get output path
                    std::optional<std::string> outputPath;
                    if (item->contains("output")) {
                        outputPath = item->getString("output");
                    }

                    // Pre-parse fonts/images outside HaruLock (shared cache across batch items).
                    QuickDrawMedia media;
                    preparseQuickDrawOps(ops, media);

                    std::lock_guard<std::mutex> lock(HaruLock::get());

                    // Create document
                    HPDF_Doc doc = HPDF_New(nullptr, nullptr);
                    if (doc == nullptr) {
                        results.push_back(std::string("Failed to create document"));
                        continue;
                    }
                    if (HPDF_UseUTFEncodings(doc) != HPDF_OK) {
                        results.push_back(std::string("Failed to enable UTF-8 encodings"));
                        continue;
                    }

                    try {
                        HPDF_Page page = nullptr;

                        for (const auto& op : ops) {
                            if (page == nullptr && !(op->contains("type") && op->getString("type") == "page")) {
                                page = HPDF_AddPage(doc);
                                throwHaruError(doc);
                            }
                            executeOperation(doc, page, op, unit, dpiVal, &media);
                        }

                        if (outputPath.has_value()) {
                            HPDF_SaveToFile(doc, outputPath->c_str());
                            throwHaruError(doc);
                            HPDF_Free(doc);
                            results.push_back(outputPath.value());
                        } else {
                            double handle = _docs.registerPointer(doc, nullptr);
                            results.push_back(handle);
                        }
                    } catch (const std::exception& e) {
                        HPDF_Free(doc);
                        results.push_back(std::string(e.what()));
                    }
                } catch (const std::exception& e) {
                    results.push_back(std::string(e.what()));
                }
            }

            return results;
        });

    return promise;
}

// ---------------------------------------------------------------------------
// YUV to RGB conversion
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<std::shared_ptr<ArrayBuffer>>> HybridNitroPdfWriter::yuv2rgb(
    const std::shared_ptr<ArrayBuffer>& buffer,
    double width, double height,
    YuvFormat format) {

    auto promise = Promise<std::shared_ptr<ArrayBuffer>>::create();
    HaruWorker::getInstance().run<std::shared_ptr<ArrayBuffer>>(promise,
        [buffer, width, height, format]() {
            int w = static_cast<int>(width);
            int h = static_cast<int>(height);

            if (w <= 0 || h <= 0) {
                throw std::invalid_argument("Width and height must be positive");
            }

            size_t rgbSize = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
            std::shared_ptr<ArrayBuffer> rgbBuffer = ArrayBuffer::allocate(rgbSize);
            uint8_t* rgb = rgbBuffer->data();
            const uint8_t* yuv = buffer->data();

            auto clamp = [](int v) -> uint8_t {
                return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
            };

            if (format == YuvFormat::NV12) {
                // NV12: Y plane followed by interleaved UV plane
                // UV plane: width w (aligned to 2), height (h+1)/2
                size_t ySize = static_cast<size_t>(w) * static_cast<size_t>(h);
                size_t uvStride = static_cast<size_t>(w);  // interleaved UV pairs, width aligned to 2
                size_t uvHeight = (static_cast<size_t>(h) + 1) / 2;
                size_t uvPlaneSize = uvStride * uvHeight;
                if (buffer->size() < ySize + uvPlaneSize) {
                    throw std::invalid_argument("Buffer too small for NV12 format");
                }

                const uint8_t* yPlane = yuv;
                const uint8_t* uvPlane = yuv + ySize;

                for (int j = 0; j < h; j++) {
                    for (int i = 0; i < w; i++) {
                        int y = yPlane[j * w + i];
                        size_t uvIndex = (static_cast<size_t>(j / 2) * uvStride) + static_cast<size_t>(i & ~1);
                        int u = uvPlane[uvIndex] - 128;
                        int v = uvPlane[uvIndex + 1] - 128;

                        int r = y + ((359 * v) >> 8);
                        int g = y - ((88 * u + 183 * v) >> 8);
                        int b = y + ((454 * u) >> 8);

                        size_t idx = (static_cast<size_t>(j) * w + i) * 3;
                        rgb[idx] = clamp(r);
                        rgb[idx + 1] = clamp(g);
                        rgb[idx + 2] = clamp(b);
                    }
                }
            } else if (format == YuvFormat::NV21) {
                // NV21: Y plane followed by interleaved VU plane
                size_t ySize = static_cast<size_t>(w) * static_cast<size_t>(h);
                size_t uvStride = static_cast<size_t>(w);
                size_t uvHeight = (static_cast<size_t>(h) + 1) / 2;
                size_t uvPlaneSize = uvStride * uvHeight;
                if (buffer->size() < ySize + uvPlaneSize) {
                    throw std::invalid_argument("Buffer too small for NV21 format");
                }

                const uint8_t* yPlane = yuv;
                const uint8_t* vuPlane = yuv + ySize;

                for (int j = 0; j < h; j++) {
                    for (int i = 0; i < w; i++) {
                        int y = yPlane[j * w + i];
                        size_t uvIndex = (static_cast<size_t>(j / 2) * uvStride) + static_cast<size_t>(i & ~1);
                        int v = vuPlane[uvIndex] - 128;
                        int u = vuPlane[uvIndex + 1] - 128;

                        int r = y + ((359 * v) >> 8);
                        int g = y - ((88 * u + 183 * v) >> 8);
                        int b = y + ((454 * u) >> 8);

                        size_t idx = (static_cast<size_t>(j) * w + i) * 3;
                        rgb[idx] = clamp(r);
                        rgb[idx + 1] = clamp(g);
                        rgb[idx + 2] = clamp(b);
                    }
                }
            } else if (format == YuvFormat::I420 || format == YuvFormat::YUV420P) {
                // I420: Y plane, then U plane, then V plane
                // Chroma dimensions: ceil(w/2) x ceil(h/2)
                size_t ySize = static_cast<size_t>(w) * static_cast<size_t>(h);
                size_t uvWidth = (static_cast<size_t>(w) + 1) / 2;
                size_t uvHeight = (static_cast<size_t>(h) + 1) / 2;
                size_t uvSize = uvWidth * uvHeight;
                if (buffer->size() < ySize + uvSize * 2) {
                    throw std::invalid_argument("Buffer too small for I420 format");
                }

                const uint8_t* yPlane = yuv;
                const uint8_t* uPlane = yuv + ySize;
                const uint8_t* vPlane = yuv + ySize + uvSize;

                for (int j = 0; j < h; j++) {
                    for (int i = 0; i < w; i++) {
                        int y = yPlane[j * w + i];
                        size_t chromaIdx = (static_cast<size_t>(j / 2) * uvWidth) + static_cast<size_t>(i / 2);
                        int u = uPlane[chromaIdx] - 128;
                        int v = vPlane[chromaIdx] - 128;

                        int r = y + ((359 * v) >> 8);
                        int g = y - ((88 * u + 183 * v) >> 8);
                        int b = y + ((454 * u) >> 8);

                        size_t idx = (static_cast<size_t>(j) * w + i) * 3;
                        rgb[idx] = clamp(r);
                        rgb[idx + 1] = clamp(g);
                        rgb[idx + 2] = clamp(b);
                    }
                }
            } else {
                throw std::invalid_argument("Unsupported YUV format");
            }

            return rgbBuffer;
        });

    return promise;
}

// ---------------------------------------------------------------------------
// Media cache
// ---------------------------------------------------------------------------

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setCacheDir(const std::string& path) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [path]() {
    MediaCache::instance().setCacheDir(path);
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::clearMediaCache() {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, []() {
    MediaCache::instance().clear();
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setMediaCacheLimit(double maxBytes) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [maxBytes]() {
    MediaCache::instance().trimTo(maxBytes > 0 ? static_cast<size_t>(maxBytes) : 0);
  });
  return promise;
}

std::shared_ptr<Promise<std::shared_ptr<AnyMap>>> HybridNitroPdfWriter::getMediaCacheStats() {
  auto promise = Promise<std::shared_ptr<AnyMap>>::create();
  HaruWorker::getInstance().run<std::shared_ptr<AnyMap>>(promise, []() {
    auto& cache = MediaCache::instance();
    auto map = AnyMap::make();
    map->setDouble("entries", static_cast<double>(cache.entryCount()));
    map->setDouble("totalBytes", static_cast<double>(cache.totalBytes()));
    map->setDouble("maxBytes", static_cast<double>(cache.maxBytes()));
    return map;
  });
  return promise;
}

} // namespace margelo::nitro::pdfwriter
