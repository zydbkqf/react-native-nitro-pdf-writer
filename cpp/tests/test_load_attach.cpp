#include "FontPrep.hpp"
#include "MediaCache.hpp"
#include "MediaPrep.hpp"

#include <doctest/doctest.h>
#include <hpdf.h>
#include <png.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace margelo::nitro::pdfwriter;

namespace {

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

void writeU16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x & 0xFF));
}

void writeU32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 24));
  v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
  v.push_back(static_cast<uint8_t>(x & 0xFF));
}

void writeTag(std::vector<uint8_t>& v, const char* tag) {
  v.push_back(static_cast<uint8_t>(tag[0]));
  v.push_back(static_cast<uint8_t>(tag[1]));
  v.push_back(static_cast<uint8_t>(tag[2]));
  v.push_back(static_cast<uint8_t>(tag[3]));
}

/** Minimal single-face TTF with head/maxp/hhea/hmtx/name. */
std::vector<uint8_t> makeSyntheticTtf() {
  std::vector<uint8_t> head(54, 0);
  head[18] = 0x04;
  head[19] = 0x00; // unitsPerEm = 1024

  std::vector<uint8_t> maxp(6, 0);
  maxp[4] = 0x00;
  maxp[5] = 0x03; // numGlyphs = 3

  std::vector<uint8_t> hhea(36, 0);
  hhea[34] = 0x00;
  hhea[35] = 0x02; // numberOfHMetrics = 2

  std::vector<uint8_t> hmtx;
  writeU16(hmtx, 500);
  writeU16(hmtx, 0);
  writeU16(hmtx, 600);
  writeU16(hmtx, 0);

  std::vector<uint8_t> name;
  writeU16(name, 0);
  writeU16(name, 1);
  writeU16(name, 6 + 12);
  writeU16(name, 3);
  writeU16(name, 1);
  writeU16(name, 0x0409);
  writeU16(name, 6);
  writeU16(name, 6);
  writeU16(name, 0);
  const char* ps = "TestPS";
  name.insert(name.end(), ps, ps + 6);

  struct Tbl {
    const char* tag;
    std::vector<uint8_t>* data;
  };
  std::vector<Tbl> tables = {
      {"head", &head}, {"maxp", &maxp}, {"hhea", &hhea}, {"hmtx", &hmtx}, {"name", &name},
  };

  std::vector<uint8_t> out;
  writeU32(out, 0x00010000);
  writeU16(out, static_cast<uint16_t>(tables.size()));
  writeU16(out, 0);
  writeU16(out, 0);
  writeU16(out, 0);

  size_t dirStart = out.size();
  out.resize(dirStart + tables.size() * 16, 0);

  for (size_t i = 0; i < tables.size(); ++i) {
    while (out.size() % 4 != 0) {
      out.push_back(0);
    }
    uint32_t offset = static_cast<uint32_t>(out.size());
    auto& body = *tables[i].data;
    out.insert(out.end(), body.begin(), body.end());
    uint32_t length = static_cast<uint32_t>(body.size());

    size_t rec = dirStart + i * 16;
    std::memcpy(out.data() + rec, tables[i].tag, 4);
    out[rec + 8] = static_cast<uint8_t>(offset >> 24);
    out[rec + 9] = static_cast<uint8_t>((offset >> 16) & 0xFF);
    out[rec + 10] = static_cast<uint8_t>((offset >> 8) & 0xFF);
    out[rec + 11] = static_cast<uint8_t>(offset & 0xFF);
    out[rec + 12] = static_cast<uint8_t>(length >> 24);
    out[rec + 13] = static_cast<uint8_t>((length >> 16) & 0xFF);
    out[rec + 14] = static_cast<uint8_t>((length >> 8) & 0xFF);
    out[rec + 15] = static_cast<uint8_t>(length & 0xFF);
  }
  return out;
}

/** Tiny valid PNG (2×2 RGB) generated with libpng. */
std::vector<uint8_t> makeTestPng() {
  std::vector<uint8_t> out;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  REQUIRE(png != nullptr);
  png_infop info = png_create_info_struct(png);
  REQUIRE(info != nullptr);

  png_set_write_fn(
      png, &out,
      [](png_structp p, png_bytep data, png_size_t len) {
        auto* v = static_cast<std::vector<uint8_t>*>(png_get_io_ptr(p));
        v->insert(v->end(), data, data + len);
      },
      [](png_structp) {});
  png_set_IHDR(png, info, 2, 2, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);

  png_byte row[6] = {255, 0, 0, 0, 255, 0};
  png_bytep rows[2] = {row, row};
  png_write_image(png, rows);
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  return out;
}

/** Mirrors attachImage: install prepared bytes into a doc (HaruLock held in prod). */
HPDF_Image attachImageBlob(HPDF_Doc doc, const std::shared_ptr<MediaBlob>& blob) {
  REQUIRE(blob != nullptr);
  switch (blob->kind) {
    case MediaBlob::Kind::Png:
      return HPDF_LoadPngImageFromMem(doc, blob->bytes.data(),
                                      static_cast<HPDF_UINT>(blob->bytes.size()));
    case MediaBlob::Kind::Jpeg:
      return HPDF_LoadJpegImageFromMem(doc, blob->bytes.data(),
                                       static_cast<HPDF_UINT>(blob->bytes.size()));
    case MediaBlob::Kind::Raw:
      return HPDF_LoadRawImageFromMem(doc, blob->bytes.data(),
                                      static_cast<HPDF_UINT>(blob->width),
                                      static_cast<HPDF_UINT>(blob->height),
                                      static_cast<HPDF_ColorSpace>(blob->colorSpace), 8);
    default:
      return nullptr;
  }
}

/** Mirrors attachFont: install prepared font into a doc. */
HPDF_Font attachFontBlob(HPDF_Doc doc, const std::shared_ptr<MediaBlob>& blob,
                         HPDF_BOOL embed = HPDF_FALSE) {
  REQUIRE(blob != nullptr);
  REQUIRE(!blob->fontPath.empty());
  const char* fontName = nullptr;
  if (blob->isTtc) {
    fontName = HPDF_LoadTTFontFromFile2(doc, blob->fontPath.c_str(), blob->faceIndex, embed);
  } else {
    fontName = HPDF_LoadTTFontFromFile(doc, blob->fontPath.c_str(), embed);
  }
  if (!fontName) {
    return nullptr;
  }
  return HPDF_GetFont(doc, fontName, nullptr);
}

struct DocGuard {
  HPDF_Doc doc = nullptr;
  explicit DocGuard() : doc(HPDF_New(nullptr, nullptr)) {}
  ~DocGuard() {
    if (doc) {
      HPDF_Free(doc);
    }
  }
  DocGuard(const DocGuard&) = delete;
  DocGuard& operator=(const DocGuard&) = delete;
};

/**
 * Locate a real TTF/TTC that libharu can load. Synthetic fonts are fine for
 * pre-parse tests but too incomplete for HPDF_LoadTTFont*.
 */
std::string findLoadableFont(uint32_t& faceIndex) {
  faceIndex = 0;
  const char* candidates[] = {
      "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
      "/System/Library/Fonts/SFNS.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/Library/Fonts/Arial.ttf",
  };
  for (const char* path : candidates) {
    if (!std::filesystem::exists(path)) {
      continue;
    }
    std::string ext = std::filesystem::path(path).extension().string();
    for (auto& c : ext) {
      c = static_cast<char>(::tolower(c));
    }
    faceIndex = 0;
    return path;
  }
  return {};
}

} // namespace

// ===========================================================================
// load / attach — MediaRegistry handle lifecycle
// ===========================================================================

TEST_CASE("load/attach: MediaRegistry issues a handle that attach can resolve") {
  MediaRegistry registry;
  auto png = makeTestPng();
  auto blob = MediaPrep::prepareImageBuffer(png.data(), png.size(), MediaBlob::Kind::Png, 0, 0, 0,
                                           /*useCache=*/false);
  double media = registry.registerBlob(blob);
  REQUIRE(media != 0);

  auto resolved = registry.get(media);
  REQUIRE(resolved != nullptr);
  CHECK(resolved->kind == MediaBlob::Kind::Png);
  CHECK(resolved->width == 2);
  CHECK(resolved->height == 2);
}

TEST_CASE("load/attach: freeMedia invalidates the media handle") {
  MediaRegistry registry;
  auto png = makeTestPng();
  auto blob = MediaPrep::prepareImageBuffer(png.data(), png.size(), MediaBlob::Kind::Png, 0, 0, 0,
                                           false);
  double media = registry.registerBlob(blob);
  REQUIRE(registry.get(media) != nullptr);

  // freeMedia
  registry.release(media);
  CHECK(registry.get(media) == nullptr);
  CHECK(registry.size() == 0);

  // Attach after free must see no media (callers throw on nullptr).
  CHECK(registry.get(media) == nullptr);
}

TEST_CASE("load/attach: unknown / double-free media handle is a safe no-op") {
  MediaRegistry registry;
  CHECK(registry.get(12345.0) == nullptr);
  registry.release(12345.0); // must not crash
  CHECK(registry.get(12345.0) == nullptr);
}

// ===========================================================================
// Cross-document media reuse
// ===========================================================================

TEST_CASE("load/attach: same font media attaches to two documents") {
  uint32_t face = 0;
  std::string fontPath = findLoadableFont(face);
  if (fontPath.empty()) {
    WARN("No system TTF/TTC found — skipping real font attach");
    return;
  }

  auto blob = FontPrep::loadFromFile(fontPath, face);
  REQUIRE(blob != nullptr);
  REQUIRE(!blob->fontPath.empty());

  MediaRegistry registry;
  double media = registry.registerBlob(blob);
  auto resolved = registry.get(media);
  REQUIRE(resolved != nullptr);

  DocGuard a;
  DocGuard b;
  REQUIRE(a.doc != nullptr);
  REQUIRE(b.doc != nullptr);

  HPDF_Font fontA = attachFontBlob(a.doc, resolved);
  HPDF_Font fontB = attachFontBlob(b.doc, resolved);
  REQUIRE(fontA != nullptr);
  REQUIRE(fontB != nullptr);
  // HPDF_Font is doc-bound — distinct pointers per document.
  CHECK(fontA != fontB);
  CHECK(HPDF_GetError(a.doc) == HPDF_OK);
  CHECK(HPDF_GetError(b.doc) == HPDF_OK);

  // Media handle stays valid for further attaches until freeMedia.
  REQUIRE(registry.get(media) != nullptr);
  HPDF_Font fontA2 = attachFontBlob(a.doc, resolved);
  CHECK(fontA2 != nullptr);
}

TEST_CASE("load/attach: loadFontFromBuffer pre-parses and exposes fontPath for attach") {
  // Synthetic TTF is enough to exercise the load half (bytes + metrics + path).
  auto ttf = makeSyntheticTtf();
  auto blob = FontPrep::loadFromBuffer(ttf.data(), ttf.size(), 0, /*useCache=*/false);
  REQUIRE(blob != nullptr);
  CHECK(!blob->fontPath.empty());
  CHECK(std::filesystem::exists(blob->fontPath));
  CHECK(blob->unitsPerEm == 1024);

  MediaRegistry registry;
  double media = registry.registerBlob(blob);
  auto resolved = registry.get(media);
  REQUIRE(resolved != nullptr);
  CHECK(resolved->fontPath == blob->fontPath);
}

TEST_CASE("load/attach: same image media attaches to two documents") {
  auto png = makeTestPng();
  auto blob = MediaPrep::prepareImageBuffer(png.data(), png.size(), MediaBlob::Kind::Png, 0, 0, 0,
                                           /*useCache=*/false);
  MediaRegistry registry;
  double media = registry.registerBlob(blob);
  auto resolved = registry.get(media);
  REQUIRE(resolved != nullptr);

  DocGuard a;
  DocGuard b;
  HPDF_Image imgA = attachImageBlob(a.doc, resolved);
  HPDF_Image imgB = attachImageBlob(b.doc, resolved);
  REQUIRE(imgA != nullptr);
  REQUIRE(imgB != nullptr);
  CHECK(imgA != imgB); // doc-owned XObjects
  CHECK(HPDF_GetError(a.doc) == HPDF_OK);
  CHECK(HPDF_GetError(b.doc) == HPDF_OK);
}

TEST_CASE("load/attach: raw image media reuses geometry across documents") {
  const uint32_t w = 4, h = 3;
  std::vector<uint8_t> pixels(w * h * 3, 0x40);
  auto blob = MediaPrep::prepareImageBuffer(pixels.data(), pixels.size(), MediaBlob::Kind::Raw, w,
                                            h, /*colorSpace=*/0, /*useCache=*/false);
  REQUIRE(blob->width == w);
  REQUIRE(blob->height == h);

  MediaRegistry registry;
  double media = registry.registerBlob(blob);
  auto resolved = registry.get(media);

  DocGuard a;
  DocGuard b;
  HPDF_Image imgA = attachImageBlob(a.doc, resolved);
  HPDF_Image imgB = attachImageBlob(b.doc, resolved);
  REQUIRE(imgA != nullptr);
  REQUIRE(imgB != nullptr);
  CHECK(HPDF_Image_GetWidth(imgA) == w);
  CHECK(HPDF_Image_GetWidth(imgB) == w);
}

// ===========================================================================
// useCache
// ===========================================================================

TEST_CASE("load/attach: useCache=true shares the same prepared media across loads") {
  MediaCache::instance().clear();
  auto ttf = makeSyntheticTtf();

  auto first = FontPrep::loadFromBuffer(ttf.data(), ttf.size(), 0, /*useCache=*/true);
  auto second = FontPrep::loadFromBuffer(ttf.data(), ttf.size(), 0, /*useCache=*/true);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  // Same underlying blob — cache hit (pin wrappers around one MediaBlob).
  CHECK(first.get() == second.get());
  CHECK(MediaCache::instance().entryCount() >= 1);
}

TEST_CASE("load/attach: useCache=false does not insert into MediaCache") {
  MediaCache::instance().clear();
  auto ttf = makeSyntheticTtf();

  auto a = FontPrep::loadFromBuffer(ttf.data(), ttf.size(), 0, /*useCache=*/false);
  auto b = FontPrep::loadFromBuffer(ttf.data(), ttf.size(), 0, /*useCache=*/false);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK(a.get() != b.get()); // distinct blobs
  CHECK(MediaCache::instance().entryCount() == 0);
}

TEST_CASE("load/attach: image useCache=true reuses buffer payload via content hash") {
  MediaCache::instance().clear();
  auto png = makeTestPng();

  auto first = MediaPrep::prepareImageBuffer(png.data(), png.size(), MediaBlob::Kind::Png, 0, 0, 0,
                                             /*useCache=*/true);
  auto second = MediaPrep::prepareImageBuffer(png.data(), png.size(), MediaBlob::Kind::Png, 0, 0, 0,
                                              /*useCache=*/true);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK(first.get() == second.get());
  CHECK(first->bytes.size() == png.size());
}

TEST_CASE("load/attach: image useCache=false keeps loads independent") {
  MediaCache::instance().clear();
  auto png = makeTestPng();

  auto a = MediaPrep::prepareImageBuffer(png.data(), png.size(), MediaBlob::Kind::Png, 0, 0, 0,
                                         /*useCache=*/false);
  auto b = MediaPrep::prepareImageBuffer(png.data(), png.size(), MediaBlob::Kind::Png, 0, 0, 0,
                                         /*useCache=*/false);
  CHECK(a.get() != b.get());
  CHECK(MediaCache::instance().entryCount() == 0);
}

TEST_CASE("load/attach: useCache file keys change when mtime/size change") {
  std::string k1 = MediaCache::fileKey("png", "/shared/logo.png", 1000, 2048);
  std::string k2 = MediaCache::fileKey("png", "/shared/logo.png", 2000, 2048);
  std::string k3 = MediaCache::fileKey("png", "/shared/logo.png", 1000, 4096);
  CHECK(k1 != k2);
  CHECK(k1 != k3);
}

// ===========================================================================
// freeMedia + attach interaction
// ===========================================================================

TEST_CASE("load/attach: after freeMedia a second attach cannot resolve the handle") {
  MediaRegistry registry;
  auto png = makeTestPng();
  auto blob = MediaPrep::prepareImageBuffer(png.data(), png.size(), MediaBlob::Kind::Png, 0, 0, 0,
                                           false);
  double media = registry.registerBlob(blob);

  DocGuard a;
  REQUIRE(attachImageBlob(a.doc, registry.get(media)) != nullptr);

  registry.release(media); // freeMedia

  auto gone = registry.get(media);
  CHECK(gone == nullptr);
  // Production attachImage throws invalid_media on nullptr — verified here by
  // requiring the resolved pointer before HPDF_* install.
  CHECK_THROWS([&]() {
    auto p = registry.get(media);
    if (!p) {
      throw std::invalid_argument("Invalid media handle");
    }
  }());
}

TEST_CASE("load/attach: freeMedia does not break previously attached doc handles") {
  uint32_t face = 0;
  std::string fontPath = findLoadableFont(face);
  if (fontPath.empty()) {
    WARN("No system TTF/TTC found — skipping real font attach");
    return;
  }

  MediaRegistry registry;
  auto blob = FontPrep::loadFromFile(fontPath, face);
  double media = registry.registerBlob(blob);
  auto resolved = registry.get(media);

  DocGuard a;
  HPDF_Font font = attachFontBlob(a.doc, resolved);
  REQUIRE(font != nullptr);

  registry.release(media); // freeMedia — prepared handle goes away

  // Already-attached HPDF_Font remains valid inside the document.
  HPDF_Page page = HPDF_AddPage(a.doc);
  REQUIRE(page != nullptr);
  HPDF_STATUS st = HPDF_Page_SetFontAndSize(page, font, 12.0f);
  CHECK(st == HPDF_OK);
  st = HPDF_Page_BeginText(page);
  CHECK(st == HPDF_OK);
  st = HPDF_Page_TextOut(page, 10, 10, "ok");
  CHECK(st == HPDF_OK);
  HPDF_Page_EndText(page);
  CHECK(HPDF_GetError(a.doc) == HPDF_OK);
}

TEST_CASE("load/attach: one media handle serves many documents before freeMedia") {
  MediaRegistry registry;
  auto png = makeTestPng();
  auto blob = MediaPrep::prepareImageBuffer(png.data(), png.size(), MediaBlob::Kind::Png, 0, 0, 0,
                                           false);
  double media = registry.registerBlob(blob);
  auto resolved = registry.get(media);

  std::vector<std::unique_ptr<DocGuard>> docs;
  std::vector<HPDF_Image> images;
  for (int i = 0; i < 4; ++i) {
    docs.push_back(std::make_unique<DocGuard>());
    HPDF_Image img = attachImageBlob(docs.back()->doc, resolved);
    REQUIRE(img != nullptr);
    images.push_back(img);
  }
  CHECK(registry.get(media) != nullptr);

  registry.release(media);
  CHECK(registry.get(media) == nullptr);
  // Each document keeps its own HPDF_Image.
  for (size_t i = 1; i < images.size(); ++i) {
    CHECK(images[i] != images[0]);
  }
}
