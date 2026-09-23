#include "HybridNitroPdfWriter.hpp"
#include "HaruError.hpp"
#include "HaruLock.hpp"
#include <NitroModules/ArrayBuffer.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
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
  HPDF_DashMode mode;
  mode.num_ptn = static_cast<HPDF_UINT>(std::min(pattern.size(), static_cast<size_t>(8)));
  for (size_t i = 0; i < mode.num_ptn; ++i) {
    mode.ptn[i] = static_cast<HPDF_REAL>(pattern[i]);
  }
  mode.phase = 0;
  return mode;
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
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::createDocument() {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    HPDF_Doc doc = HPDF_New(nullptr, nullptr);
    if (doc == nullptr) {
      throw std::runtime_error("Failed to create HPDF document");
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
    throwOnHaruError(HPDF_SaveToFile(pointer, tempPath.c_str()), pointer);
    throwHaruError(pointer);

    std::ifstream file(tempPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
      std::remove(tempPath.c_str());
      throw std::runtime_error("Failed to open temporary PDF file");
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::shared_ptr<ArrayBuffer> buffer = ArrayBuffer::allocate(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer->data()), size)) {
      std::remove(tempPath.c_str());
      throw std::runtime_error("Failed to read temporary PDF file");
    }
    std::remove(tempPath.c_str());
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

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadType1FontFromFile(double doc,
                                                                       const std::string& afmPath,
                                                                       const std::optional<std::string>& pfmPath) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, afmPath, pfmPath]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    const char* pfm = pfmPath.has_value() ? pfmPath->c_str() : nullptr;
    const char* fontName = HPDF_LoadType1FontFromFile(h.get(), afmPath.c_str(), pfm);
    throwHaruError(h.get());
    HPDF_Font font = HPDF_GetFont(h.get(), fontName, nullptr);
    throwHaruError(h.get());
    return _fonts.registerPointer(font, h.get());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadTTFontFromFile(double doc, const std::string& fileName,
                                                                    std::optional<bool> embedding) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, fileName, embedding]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_BOOL embed = embedding.value_or(HPDF_FALSE);
    const char* fontName = HPDF_LoadTTFontFromFile(h.get(), fileName.c_str(), embed);
    throwHaruError(h.get());
    HPDF_Font font = HPDF_GetFont(h.get(), fontName, nullptr);
    throwHaruError(h.get());
    return _fonts.registerPointer(font, h.get());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadTTFontFromFile2(double doc, const std::string& fileName,
                                                                     double index,
                                                                     std::optional<bool> embedding) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, fileName, index, embedding]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_BOOL embed = embedding.value_or(HPDF_FALSE);
    const char* fontName = HPDF_LoadTTFontFromFile2(h.get(), fileName.c_str(), static_cast<HPDF_UINT>(index), embed);
    throwHaruError(h.get());
    HPDF_Font font = HPDF_GetFont(h.get(), fontName, nullptr);
    throwHaruError(h.get());
    return _fonts.registerPointer(font, h.get());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::setCurrentFont(double doc, double font) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, doc, font]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> docH(doc, _docs);
    Handle<HPDF_Font> fontH(font, _fonts);
    // libHaru tracks the current font internally per page; this is a no-op
    // placeholder kept for API completeness.
    (void)docH;
    (void)fontH;
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
    throwOnHaruError(HPDF_Page_BeginText(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::endText(double page) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
    throwOnHaruError(HPDF_Page_EndText(h.get()), h.owner());
  });
  return promise;
}

std::shared_ptr<Promise<void>> HybridNitroPdfWriter::textOut(double page, double x, double y, const std::string& text) {
  auto promise = Promise<void>::create();
  HaruWorker::getInstance().run(promise, [this, page, x, y, text]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Page> h(page, _pages);
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
    HPDF_Rect rect{static_cast<HPDF_REAL>(left), static_cast<HPDF_REAL>(top),
                   static_cast<HPDF_REAL>(right), static_cast<HPDF_REAL>(bottom)};
    throwOnHaruError(HPDF_Page_TextRect(h.get(), rect.left, rect.top, rect.right, rect.bottom, text.c_str(),
                                    static_cast<HPDF_TextAlignment>(align), nullptr),
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
    mode.phase = static_cast<HPDF_UINT>(phase);
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

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadPngImageFromFile(double doc, const std::string& fileName) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, fileName]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_Image image = HPDF_LoadPngImageFromFile(h.get(), fileName.c_str());
    throwHaruError(h.get());
    return _images.registerPointer(image, h.get());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadPngImageFromBuffer(double doc,
                                                                        const std::shared_ptr<ArrayBuffer>& buffer) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, buffer]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_Image image = HPDF_LoadPngImageFromMem(h.get(), buffer->data(),
                                                static_cast<HPDF_UINT>(buffer->size()));
    throwHaruError(h.get());
    return _images.registerPointer(image, h.get());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadJpegImageFromFile(double doc, const std::string& fileName) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, fileName]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_Image image = HPDF_LoadJpegImageFromFile(h.get(), fileName.c_str());
    throwHaruError(h.get());
    return _images.registerPointer(image, h.get());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadRawImageFromFile(double doc, const std::string& fileName,
                                                                      double width, double height,
                                                                      double colorSpace) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, fileName, width, height, colorSpace]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_Image image = HPDF_LoadRawImageFromFile(h.get(), fileName.c_str(), static_cast<HPDF_UINT>(width),
                                                 static_cast<HPDF_UINT>(height),
                                                 static_cast<HPDF_ColorSpace>(colorSpace));
    throwHaruError(h.get());
    return _images.registerPointer(image, h.get());
  });
  return promise;
}

std::shared_ptr<Promise<double>> HybridNitroPdfWriter::loadRawImageFromBuffer(double doc,
                                                                        const std::shared_ptr<ArrayBuffer>& buffer,
                                                                        double width, double height,
                                                                        double colorSpace) {
  auto promise = Promise<double>::create();
  HaruWorker::getInstance().run<double>(promise, [this, doc, buffer, width, height, colorSpace]() {
    std::lock_guard<std::mutex> lock(HaruLock::get());
    Handle<HPDF_Doc> h(doc, _docs);
    HPDF_Image image = HPDF_LoadRawImageFromMem(h.get(), buffer->data(),
                                                static_cast<HPDF_UINT>(width),
                                                static_cast<HPDF_UINT>(height),
                                                static_cast<HPDF_ColorSpace>(colorSpace),
                                                8);
    throwHaruError(h.get());
    return _images.registerPointer(image, h.get());
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
    HPDF_Outline parentPointer = parent == 0 ? nullptr : _outlines.getPointer(parent);
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
    std::sscanf(value.c_str(), "%d-%d-%d %d:%d:%d", &date.year, &date.month, &date.day,
                &date.hour, &date.minutes, &date.seconds);
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
    HPDF_UINT charCount = static_cast<HPDF_UINT>(std::strlen(text.c_str()));
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

} // namespace margelo::nitro::pdfwriter
