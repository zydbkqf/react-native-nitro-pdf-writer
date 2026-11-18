#include "fixtures/image_fixtures.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <hpdf.h>
#include <string>
#include <unistd.h>

#include "MediaPrep.hpp"
#include "PlatformImageTranscoder.hpp"

using namespace margelo::nitro::pdfwriter;

namespace fs = std::filesystem;

namespace {

int g_transcodeCalls = 0;

/** Stand-in for the platform decoder: reports a fixed PNG, counts invocations. */
bool stubTranscoder(const uint8_t* /*data*/, size_t /*size*/, MediaPrep::ImageContainer /*container*/,
                    const MediaPrep::TranscodeOptions& /*options*/, MediaPrep::TranscodedImage& out,
                    std::string& /*error*/) {
  ++g_transcodeCalls;
  out.bytes = fixtures::kPngRgb;
  out.kind = MediaBlob::Kind::Png;
  out.width = 16;
  out.height = 16;
  out.hadAlpha = false;
  return true;
}

/** Decoder that reports an unsupported device, like Android API 24-27 on HEIC. */
bool refusingTranscoder(const uint8_t*, size_t, MediaPrep::ImageContainer,
                        const MediaPrep::TranscodeOptions&, MediaPrep::TranscodedImage&,
                        std::string& error) {
  error = "HEIC/HEIF decoding requires Android 9 (API 28) or newer; this device runs API 24.";
  return false;
}

struct CacheDirGuard {
  std::string dir;
  CacheDirGuard() {
    dir = (fs::temp_directory_path() /
           ("npdf-image-test-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
            std::to_string(static_cast<long long>(
                std::filesystem::file_time_type::clock::now().time_since_epoch().count()))))
              .string();
    fs::create_directories(dir);
    MediaCache::instance().setCacheDir(dir);
  }
  ~CacheDirGuard() {
    MediaCache::instance().clear();
    MediaCache::instance().setCacheDir("");
    fs::remove_all(dir);
  }
};

} // namespace

TEST_CASE("sniffImageContainer recognises every supported container") {
  using MediaPrep::ImageContainer;
  CHECK(MediaPrep::sniffImageContainer(fixtures::kPngRgb.data(), fixtures::kPngRgb.size()) ==
        ImageContainer::Png);
  CHECK(MediaPrep::sniffImageContainer(fixtures::kPngInterlaced.data(),
                                       fixtures::kPngInterlaced.size()) == ImageContainer::Png);
  CHECK(MediaPrep::sniffImageContainer(fixtures::kJpeg.data(), fixtures::kJpeg.size()) ==
        ImageContainer::Jpeg);
  CHECK(MediaPrep::sniffImageContainer(fixtures::kWebpHeader.data(),
                                       fixtures::kWebpHeader.size()) == ImageContainer::Webp);
  CHECK(MediaPrep::sniffImageContainer(fixtures::kHeicHeader.data(),
                                       fixtures::kHeicHeader.size()) == MediaPrep::ImageContainer::Heic);
  CHECK(MediaPrep::sniffImageContainer(fixtures::kAvifHeader.data(),
                                       fixtures::kAvifHeader.size()) == MediaPrep::ImageContainer::Avif);

  const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03};
  CHECK(MediaPrep::sniffImageContainer(garbage, sizeof(garbage)) == ImageContainer::Unknown);
  CHECK(MediaPrep::sniffImageContainer(nullptr, 0) == ImageContainer::Unknown);

  // "RIFF" without the WEBP fourcc is a RIFF container, not WebP.
  const uint8_t riffOnly[] = {'R', 'I', 'F', 'F', 0x10, 0x00, 0x00, 0x00,
                              'W', 'A', 'V', 'E'};
  CHECK(MediaPrep::sniffImageContainer(riffOnly, sizeof(riffOnly)) == ImageContainer::Unknown);
}

TEST_CASE("HEIC/WebP without a platform decoder fail with a readable error") {
  MediaPrep::clearPlatformImageTranscoder();
  CHECK_FALSE(MediaPrep::hasPlatformImageTranscoder());

  CHECK_THROWS_WITH_AS(
      MediaPrep::transcodeToPdfImage(fixtures::kWebpHeader.data(),
                                     fixtures::kWebpHeader.size(), MediaPrep::ImageContainer::Webp),
      doctest::Contains("platform decoder"), std::runtime_error);

  // The failure must reach the media-prep entry point too, not only the low-level hook.
  CHECK_THROWS_WITH_AS(
      MediaPrep::prepareImageBuffer(fixtures::kHeicHeader.data(), fixtures::kHeicHeader.size(),
                                    MediaBlob::Kind::Png, 0, 0, 0, /*useCache=*/false,
                                    MediaPrep::ImageContainer::Heic),
      doctest::Contains("platform decoder"), std::runtime_error);
}

TEST_CASE("platform refusal surfaces the decoder's message") {
  MediaPrep::setPlatformImageTranscoder(&refusingTranscoder);
  CHECK_THROWS_WITH_AS(
      MediaPrep::prepareImageBuffer(fixtures::kHeicHeader.data(), fixtures::kHeicHeader.size(),
                                    MediaBlob::Kind::Png, 0, 0, 0, /*useCache=*/false),
      doctest::Contains("API 28"), std::runtime_error);
  MediaPrep::clearPlatformImageTranscoder();
}

TEST_CASE("transcoded payload is cached and survives a cold start") {
  CacheDirGuard guard;
  MediaPrep::setPlatformImageTranscoder(&stubTranscoder);
  g_transcodeCalls = 0;

  {
    auto blob = MediaPrep::prepareImageBuffer(fixtures::kWebpHeader.data(),
                                              fixtures::kWebpHeader.size(), MediaBlob::Kind::Png,
                                              0, 0, 0, /*useCache=*/true);
    REQUIRE(blob != nullptr);
    // The blob carries the transcoded PNG, not the WebP source.
    CHECK(blob->kind == MediaBlob::Kind::Png);
    CHECK(blob->width == 16);
    CHECK(blob->height == 16);
    CHECK(g_transcodeCalls == 1);
    CHECK(MediaCache::instance().diskBytes() > 0);
  }

  // Cold start: memory dropped, disk kept. The handle above must be released
  // first — clearMemory() skips pinned entries, and a still-pinned blob would
  // make the "second" load a memory hit that never touches the disk layer.
  MediaCache::instance().clearMemory();
  CHECK(MediaCache::instance().entryCount() == 0);

  auto again = MediaPrep::prepareImageBuffer(fixtures::kWebpHeader.data(),
                                             fixtures::kWebpHeader.size(), MediaBlob::Kind::Png,
                                             0, 0, 0, /*useCache=*/true);
  REQUIRE(again != nullptr);
  CHECK(again->width == 16);
  CHECK(again->height == 16);
  CHECK(g_transcodeCalls == 1); // no second transcode: the disk entry was used

  MediaPrep::clearPlatformImageTranscoder();
}

TEST_CASE("PNG variants decode through libharu") {
  HPDF_Doc doc = HPDF_New(nullptr, nullptr);
  REQUIRE(doc != nullptr);

  struct Case {
    const char* name;
    const std::vector<uint8_t>* bytes;
    uint32_t expectedWidth;
    uint32_t expectedHeight;
  };
  const Case cases[] = {
      {"RGB 16x16", &fixtures::kPngRgb, 16, 16},
      {"palette+tRNS 8x8", &fixtures::kPngPaletteTrns, 8, 8},
      {"16-bit gray 8x8", &fixtures::kPngGray16, 8, 8},
      {"Adam7 interlaced 16x16", &fixtures::kPngInterlaced, 16, 16},
  };

  for (const auto& testCase : cases) {
    INFO("case: " << testCase.name);
    HPDF_Image image = HPDF_LoadPngImageFromMem(doc, testCase.bytes->data(),
                                                static_cast<HPDF_UINT>(testCase.bytes->size()));
    REQUIRE(image != nullptr);
    CHECK(HPDF_Image_GetWidth(image) == testCase.expectedWidth);
    CHECK(HPDF_Image_GetHeight(image) == testCase.expectedHeight);
  }

  HPDF_Free(doc);
}

TEST_CASE("interlaced PNG draws into a saved PDF") {
  HPDF_Doc doc = HPDF_New(nullptr, nullptr);
  REQUIRE(doc != nullptr);
  HPDF_Page page = HPDF_AddPage(doc);
  REQUIRE(page != nullptr);
  HPDF_Page_SetWidth(page, 200.0f);
  HPDF_Page_SetHeight(page, 200.0f);

  HPDF_Image image = HPDF_LoadPngImageFromMem(doc, fixtures::kPngInterlaced.data(),
                                              static_cast<HPDF_UINT>(fixtures::kPngInterlaced.size()));
  REQUIRE(image != nullptr);
  CHECK(HPDF_Image_GetBitsPerComponent(image) == 8);
  CHECK(HPDF_Page_DrawImage(page, image, 20.0f, 20.0f, 160.0f, 160.0f) == HPDF_OK);

  const char* path = "test_interlaced_png.pdf";
  CHECK(HPDF_SaveToFile(doc, path) == HPDF_OK);
  CHECK(fs::exists(path));
  CHECK(fs::file_size(path) > 1000);

  fs::remove(path);
  HPDF_Free(doc);
}

TEST_CASE("JPEG decodes through libharu") {
  HPDF_Doc doc = HPDF_New(nullptr, nullptr);
  REQUIRE(doc != nullptr);
  HPDF_Image image =
      HPDF_LoadJpegImageFromMem(doc, fixtures::kJpeg.data(), static_cast<HPDF_UINT>(fixtures::kJpeg.size()));
  REQUIRE(image != nullptr);
  CHECK(HPDF_Image_GetWidth(image) == 16);
  CHECK(HPDF_Image_GetHeight(image) == 16);
  HPDF_Free(doc);
}

TEST_CASE("prepareImageBuffer derives dimensions from the encoded payload") {
  // Declared as HEIC but actually PNG bytes: sniffing must win over the hint.
  auto blob = MediaPrep::prepareImageBuffer(fixtures::kPngRgb.data(), fixtures::kPngRgb.size(),
                                            MediaBlob::Kind::Png, 0, 0, 0, /*useCache=*/false,
                                            MediaPrep::ImageContainer::Heic);
  REQUIRE(blob != nullptr);
  CHECK(blob->kind == MediaBlob::Kind::Png);
  CHECK(blob->width == 16);
  CHECK(blob->height == 16);
}

TEST_CASE("AVIF that only lists avif among compatible brands is not mistaken for HEIC") {
  // ISO-BMFF ftyp: size(4) 'ftyp' major(4) minor(4) compatible[]. AVIF encoders
  // routinely write major=mif1 and only advertise "avif" in the compatible list.
  const uint8_t avifMif1[] = {0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p',
                              'm', 'i', 'f', '1', 0x00, 0x00, 0x00, 0x00,
                              'a', 'v', 'i', 'f', 'm', 'i', 'a', 'f'};
  CHECK(MediaPrep::sniffImageContainer(avifMif1, sizeof(avifMif1)) == MediaPrep::ImageContainer::Avif);

  // A genuine HEIC still wins when heic is the major brand.
  const uint8_t heic[] = {0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p',
                          'h', 'e', 'i', 'c', 0x00, 0x00, 0x00, 0x00,
                          'm', 'i', 'f', '1', 'm', 'i', 'a', 'f'};
  CHECK(MediaPrep::sniffImageContainer(heic, sizeof(heic)) == MediaPrep::ImageContainer::Heic);

  // Generic mif1 without an avif brand stays in the HEIF family.
  const uint8_t plainHeif[] = {0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p',
                               'm', 'i', 'f', '1', 0x00, 0x00, 0x00, 0x00,
                               'm', 'i', 'a', 'f', 'm', 'i', 'f', '1'};
  CHECK(MediaPrep::sniffImageContainer(plainHeif, sizeof(plainHeif)) == MediaPrep::ImageContainer::Heic);
}

TEST_CASE("a corrupt disk entry is discarded instead of poisoning the key forever") {
  CacheDirGuard guard;
  MediaPrep::setPlatformImageTranscoder(&stubTranscoder);
  g_transcodeCalls = 0;

  {
    auto blob = MediaPrep::prepareImageBuffer(fixtures::kWebpHeader.data(),
                                              fixtures::kWebpHeader.size(), MediaBlob::Kind::Png,
                                              0, 0, 0, /*useCache=*/true);
    REQUIRE(blob != nullptr);
    CHECK(blob->width == 16);
  }
  CHECK(g_transcodeCalls == 1);

  MediaCache::instance().clearMemory();
  REQUIRE(MediaCache::instance().entryCount() == 0);

  // Keep the 40-byte envelope intact but destroy the payload: the entry still
  // loads from disk, then fails its PNG header parse during restore.
  const fs::path dir = fs::path(MediaCache::instance().cacheDir()) / "npdf-media-cache";
  size_t corrupted = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const std::string name = entry.path().filename().string();
    if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".bin") != 0) {
      continue;
    }
    std::vector<uint8_t> bytes;
    {
      std::ifstream in(entry.path(), std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    REQUIRE(bytes.size() > 40);
    std::fill(bytes.begin() + 40, bytes.end(), 0xFF);
    std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
    ++corrupted;
  }
  CHECK(corrupted == 1);

  // The restore throws once, the cache drops the entry, and the call still
  // succeeds by re-deriving from the source — it must never keep re-throwing.
  auto reloaded = MediaPrep::prepareImageBuffer(fixtures::kWebpHeader.data(),
                                                fixtures::kWebpHeader.size(), MediaBlob::Kind::Png,
                                                0, 0, 0, /*useCache=*/true);
  REQUIRE(reloaded != nullptr);
  CHECK(reloaded->kind == MediaBlob::Kind::Png);
  CHECK(reloaded->width == 16);
  CHECK(g_transcodeCalls == 2); // re-derived, not served from the bad entry

  auto third = MediaPrep::prepareImageBuffer(fixtures::kWebpHeader.data(),
                                             fixtures::kWebpHeader.size(), MediaBlob::Kind::Png,
                                             0, 0, 0, /*useCache=*/true);
  REQUIRE(third != nullptr);
  CHECK(third->width == 16);
  CHECK(g_transcodeCalls == 2); // the rewritten disk entry is healthy again

  MediaPrep::clearPlatformImageTranscoder();
}
