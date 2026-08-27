#include <doctest/doctest.h>
#include <filesystem>
#include <hpdf.h>

TEST_CASE("libharu can create a document with a page") {
  HPDF_Doc doc = HPDF_New(nullptr, nullptr);
  REQUIRE(doc != nullptr);

  HPDF_Page page = HPDF_AddPage(doc);
  REQUIRE(page != nullptr);

  HPDF_Page_SetWidth(page, 200.0f);
  HPDF_Page_SetHeight(page, 200.0f);

  const char* path = "test_smoke.pdf";
  HPDF_STATUS status = HPDF_SaveToFile(doc, path);
  CHECK(status == HPDF_OK);
  CHECK(std::filesystem::exists(path));
  CHECK(std::filesystem::file_size(path) > 0);

  std::filesystem::remove(path);
  HPDF_Free(doc);
}

TEST_CASE("libharu reproduces user's gray-background text sequence") {
  HPDF_Doc doc = HPDF_New(nullptr, nullptr);
  REQUIRE(doc != nullptr);

  HPDF_Page page = HPDF_AddPage(doc);
  REQUIRE(page != nullptr);

  HPDF_Page_SetWidth(page, 595.0f);
  HPDF_Page_SetHeight(page, 842.0f);

  HPDF_Page_SetRGBFill(page, 0.9f, 0.9f, 0.9f);
  HPDF_Page_Rectangle(page, 0, 0, 595.0f, 842.0f);
  HPDF_Page_Fill(page);

  HPDF_Page_BeginText(page);
  // NOTE: no font set -- this is what the user did
  HPDF_STATUS status = HPDF_Page_TextOut(page, 0, 0, "test");
  CHECK(status == HPDF_PAGE_FONT_NOT_FOUND);

  HPDF_Page_EndText(page);

  // Simulate user continuing to save even after error (if they caught it)
  HPDF_ResetError(doc);
  const char* path = "test_smoke_no_font.pdf";
  status = HPDF_SaveToFile(doc, path);
  CHECK(status == HPDF_OK);
  CHECK(std::filesystem::exists(path));
  std::filesystem::remove(path);

  HPDF_Free(doc);
}

TEST_CASE("libharu gray-background text sequence with font works") {
  HPDF_Doc doc = HPDF_New(nullptr, nullptr);
  REQUIRE(doc != nullptr);

  HPDF_Page page = HPDF_AddPage(doc);
  REQUIRE(page != nullptr);

  HPDF_Page_SetWidth(page, 595.0f);
  HPDF_Page_SetHeight(page, 842.0f);

  HPDF_Page_SetRGBFill(page, 0.9f, 0.9f, 0.9f);
  HPDF_Page_Rectangle(page, 0, 0, 595.0f, 842.0f);
  HPDF_Page_Fill(page);

  HPDF_Font font = HPDF_GetFont(doc, "Helvetica", nullptr);
  REQUIRE(font != nullptr);
  HPDF_Page_SetFontAndSize(page, font, 24.0f);

  HPDF_Page_BeginText(page);
  HPDF_STATUS status = HPDF_Page_TextOut(page, 50, 800, "test");
  CHECK(status == HPDF_OK);
  HPDF_Page_EndText(page);

  const char* path = "test_smoke_font.pdf";
  CHECK(HPDF_SaveToFile(doc, path) == HPDF_OK);
  CHECK(std::filesystem::exists(path));
  CHECK(std::filesystem::file_size(path) > 0);

  std::filesystem::remove(path);
  HPDF_Free(doc);
}

