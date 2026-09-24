#include "FontPrep.hpp"
#include <doctest/doctest.h>
#include <cstring>
#include <filesystem>

using namespace margelo::nitro::pdfwriter;

namespace {

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
  // Table bodies first so we know offsets after the directory.
  std::vector<uint8_t> head(54, 0);
  // unitsPerEm at +18
  head[18] = 0x04;
  head[19] = 0x00; // 1024

  std::vector<uint8_t> maxp(6, 0);
  maxp[4] = 0x00;
  maxp[5] = 0x03; // numGlyphs = 3

  std::vector<uint8_t> hhea(36, 0);
  hhea[34] = 0x00;
  hhea[35] = 0x02; // numberOfHMetrics = 2

  std::vector<uint8_t> hmtx;
  writeU16(hmtx, 500); writeU16(hmtx, 0); // glyph 0
  writeU16(hmtx, 600); writeU16(hmtx, 0); // glyph 1

  // name table: 1 record, PostScript name id=6 "TestPS"
  std::vector<uint8_t> name;
  writeU16(name, 0); // format
  writeU16(name, 1); // count
  writeU16(name, 6 + 12); // stringOffset
  // record: platform 3, encoding 1, lang 0x409, nameId 6, len 6, offset 0
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
  // Offset table
  writeU32(out, 0x00010000); // sfnt version
  writeU16(out, static_cast<uint16_t>(tables.size()));
  writeU16(out, 0);
  writeU16(out, 0);
  writeU16(out, 0);

  size_t dirStart = out.size();
  out.resize(dirStart + tables.size() * 16, 0);

  // Align and write bodies, fill directory.
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
    out[rec + 4] = 0;
    out[rec + 5] = 0;
    out[rec + 6] = 0;
    out[rec + 7] = 0;
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

std::vector<uint8_t> makeSyntheticTtc(const std::vector<uint8_t>& face) {
  std::vector<uint8_t> out;
  writeTag(out, "ttcf");
  writeU16(out, 1);
  writeU16(out, 0);
  writeU32(out, 1); // numFonts
  uint32_t faceOffset = 16;
  writeU32(out, faceOffset);

  // TTC table offsets are absolute (from the start of the TTC file).
  std::vector<uint8_t> faceAbs = face;
  uint16_t numTables = static_cast<uint16_t>((face[4] << 8) | face[5]);
  for (uint16_t i = 0; i < numTables && (12 + i * 16 + 12) <= faceAbs.size(); ++i) {
    size_t rec = 12 + i * 16;
    uint32_t off = (static_cast<uint32_t>(faceAbs[rec + 8]) << 24) |
                   (static_cast<uint32_t>(faceAbs[rec + 9]) << 16) |
                   (static_cast<uint32_t>(faceAbs[rec + 10]) << 8) | faceAbs[rec + 11];
    off += faceOffset;
    faceAbs[rec + 8] = static_cast<uint8_t>(off >> 24);
    faceAbs[rec + 9] = static_cast<uint8_t>((off >> 16) & 0xFF);
    faceAbs[rec + 10] = static_cast<uint8_t>((off >> 8) & 0xFF);
    faceAbs[rec + 11] = static_cast<uint8_t>(off & 0xFF);
  }

  out.insert(out.end(), faceAbs.begin(), faceAbs.end());
  return out;
}

} // namespace

TEST_CASE("FontPrep parses glyph metrics from a synthetic TTF") {
  auto bytes = makeSyntheticTtf();
  auto blob = FontPrep::parseFromBytes(bytes, 0, 111, bytes.size());
  REQUIRE(blob != nullptr);
  CHECK(blob->isTtc == false);
  CHECK(blob->ttcFaceOffsets.size() == 1);
  CHECK(blob->unitsPerEm == 1024);
  CHECK(blob->numGlyphs == 3);
  REQUIRE(blob->advanceWidths.size() == 2);
  CHECK(blob->advanceWidths[0] == 500);
  CHECK(blob->advanceWidths[1] == 600);
  CHECK(blob->postScriptName == "TestPS");
  CHECK(blob->versionMtime == 111);
}

TEST_CASE("FontPrep parses TTC face index") {
  auto face = makeSyntheticTtf();
  auto ttc = makeSyntheticTtc(face);
  auto blob = FontPrep::parseFromBytes(ttc, 0, 0, ttc.size());
  REQUIRE(blob != nullptr);
  CHECK(blob->isTtc == true);
  REQUIRE(blob->ttcFaceOffsets.size() == 1);
  CHECK(blob->ttcFaceOffsets[0] == 16);
  CHECK(blob->unitsPerEm == 1024);
  CHECK(blob->postScriptName == "TestPS");
}

TEST_CASE("FontPrep rejects out-of-range face index") {
  auto face = makeSyntheticTtf();
  auto ttc = makeSyntheticTtc(face);
  CHECK_THROWS(FontPrep::parseFromBytes(ttc, 5, 0, ttc.size()));
}

TEST_CASE("FontPrep rejects truncated data") {
  std::vector<uint8_t> junk = {0x00, 0x01, 0x00, 0x00};
  CHECK_THROWS(FontPrep::parseFromBytes(junk, 0, 0, junk.size()));
}

TEST_CASE("FontPrep loadFromBuffer materializes a font path and parses metrics") {
  auto bytes = makeSyntheticTtf();
  auto blob = FontPrep::loadFromBuffer(bytes.data(), bytes.size(), 0, /*useCache=*/false);
  REQUIRE(blob != nullptr);
  CHECK(!blob->fontPath.empty());
  CHECK(std::filesystem::exists(blob->fontPath));
  CHECK(blob->unitsPerEm == 1024);
  CHECK(blob->postScriptName == "TestPS");
  CHECK(blob->faceIndex == 0);
}

TEST_CASE("FontPrep loadFromBuffer uses unique paths so blobs do not delete each other's files") {
  auto bytes = makeSyntheticTtf();
  auto a = FontPrep::loadFromBuffer(bytes.data(), bytes.size(), 0, /*useCache=*/false);
  auto b = FontPrep::loadFromBuffer(bytes.data(), bytes.size(), 0, /*useCache=*/false);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK(a->fontPath != b->fontPath);
  REQUIRE(std::filesystem::exists(a->fontPath));
  REQUIRE(std::filesystem::exists(b->fontPath));

  // Dropping `a` must not invalidate `b`'s materialized file.
  std::string pathB = b->fontPath;
  a.reset();
  CHECK(std::filesystem::exists(pathB));
}

TEST_CASE("FontPrep loadFromBuffer files survive MediaCache::clear") {
  MediaCache::instance().clear();
  auto bytes = makeSyntheticTtf();
  auto blob = FontPrep::loadFromBuffer(bytes.data(), bytes.size(), 0, /*useCache=*/false);
  REQUIRE(blob != nullptr);
  std::string path = blob->fontPath;
  REQUIRE(std::filesystem::exists(path));
  MediaCache::instance().clear();
  CHECK(std::filesystem::exists(path));
}
