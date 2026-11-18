#include "MediaPrep.hpp"
#include <doctest/doctest.h>

#include <cmath>
#include <stdexcept>

using namespace margelo::nitro::pdfwriter;

TEST_CASE("checkedDimension accepts whole numbers in range") {
  CHECK(MediaPrep::checkedDimension(1.0, "width") == 1);
  CHECK(MediaPrep::checkedDimension(1024.0, "height") == 1024);
  CHECK(MediaPrep::checkedDimension(static_cast<double>(MediaPrep::kMaxImageDimension), "width") ==
        MediaPrep::kMaxImageDimension);
}

TEST_CASE("checkedDimension rejects non-finite, fractional and out-of-range values") {
  CHECK_THROWS_AS(MediaPrep::checkedDimension(std::nan(""), "width"), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedDimension(INFINITY, "height"), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedDimension(-INFINITY, "height"), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedDimension(1.5, "width"), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedDimension(-4.0, "width"), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedDimension(0.0, "width"), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedDimension(1e12, "width"), std::invalid_argument);
  CHECK_THROWS_AS(
      MediaPrep::checkedDimension(static_cast<double>(MediaPrep::kMaxImageDimension) + 1.0,
                                  "width"),
      std::invalid_argument);
}

TEST_CASE("checkedColorSpace only accepts gray, rgb and cmyk") {
  CHECK(MediaPrep::checkedColorSpace(0.0) == 0);
  CHECK(MediaPrep::checkedColorSpace(1.0) == 1);
  CHECK(MediaPrep::checkedColorSpace(2.0) == 2);
  CHECK_THROWS_AS(MediaPrep::checkedColorSpace(3.0), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedColorSpace(-1.0), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedColorSpace(0.5), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedColorSpace(std::nan("")), std::invalid_argument);
  // A value this large would be undefined behaviour if narrowed before the
  // range check; the validator has to compare in double space.
  CHECK_THROWS_AS(MediaPrep::checkedColorSpace(1e30), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedColorSpace(-1e30), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedColorSpace(INFINITY), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedColorSpace(-INFINITY), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedColorSpace(1e300), std::invalid_argument);
}

TEST_CASE("colorSpaceComponents maps validated color spaces") {
  CHECK(MediaPrep::colorSpaceComponents(0) == 1);
  CHECK(MediaPrep::colorSpaceComponents(1) == 3);
  CHECK(MediaPrep::colorSpaceComponents(2) == 4);
  CHECK_THROWS_AS(MediaPrep::colorSpaceComponents(7), std::invalid_argument);
}

TEST_CASE("checkedPixelBytes computes byte counts and rejects overflow") {
  CHECK(MediaPrep::checkedPixelBytes(10, 10, 3) == 300);
  CHECK_THROWS_AS(MediaPrep::checkedPixelBytes(0, 10, 3), std::invalid_argument);
  CHECK_THROWS_AS(MediaPrep::checkedPixelBytes(10, 10, 0), std::invalid_argument);
  // Largest uint32 edges still fit in a 64-bit pixel count, but *4 components
  // does not — the multiplication must be checked, not silently wrapped.
  uint32_t maxEdge = 0xFFFFFFFFu;
  CHECK_THROWS_AS(MediaPrep::checkedPixelBytes(maxEdge, maxEdge, 4), std::invalid_argument);
}

TEST_CASE("raw pixel buffers smaller than width * height * components are rejected") {
  // PNG payload fed to the raw path: too small for 4x4 RGB (48 bytes).
  const std::vector<uint8_t> tiny = {1, 2, 3};
  CHECK_THROWS_WITH_AS(
      MediaPrep::prepareImageBuffer(tiny.data(), tiny.size(), MediaBlob::Kind::Raw, 4, 4,
                                    /*colorSpace=*/1, /*useCache=*/false),
      doctest::Contains("smaller than width * height * components"), std::invalid_argument);

  // Exactly sized payload is accepted (16 bytes for 2x2 CMYK).
  const std::vector<uint8_t> exact(16, 0x40);
  auto blob = MediaPrep::prepareImageBuffer(exact.data(), exact.size(), MediaBlob::Kind::Raw, 2, 2,
                                            /*colorSpace=*/2, /*useCache=*/false);
  REQUIRE(blob != nullptr);
  CHECK(blob->kind == MediaBlob::Kind::Raw);
  CHECK(blob->width == 2);
  CHECK(blob->height == 2);
  CHECK(blob->colorSpace == 2);
}
