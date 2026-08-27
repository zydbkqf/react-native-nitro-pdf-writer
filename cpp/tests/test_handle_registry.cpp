#include "HandleRegistry.hpp"
#include <doctest/doctest.h>

using namespace margelo::nitro::pdfwriter;

TEST_CASE("HandleRegistry registers and retrieves pointers") {
  HandleRegistry<HPDF_Doc> registry;

  int dummy1 = 1;
  int dummy2 = 2;
  HPDF_Doc doc1 = reinterpret_cast<HPDF_Doc>(&dummy1);
  HPDF_Doc doc2 = reinterpret_cast<HPDF_Doc>(&dummy2);

  double handle1 = registry.registerPointer(doc1);
  double handle2 = registry.registerPointer(doc2);

  CHECK(handle1 != 0.0);
  CHECK(handle2 != 0.0);
  CHECK(handle1 != handle2);

  CHECK(registry.getPointer(handle1) == doc1);
  CHECK(registry.getPointer(handle2) == doc2);

  CHECK(registry.getHandle(doc1) == handle1);
  CHECK(registry.getHandle(doc2) == handle2);
}

TEST_CASE("HandleRegistry stores owner documents") {
  HandleRegistry<HPDF_Page> registry;

  int dummyPage = 1;
  int dummyOwner = 2;
  HPDF_Page page = reinterpret_cast<HPDF_Page>(&dummyPage);
  HPDF_Doc owner = reinterpret_cast<HPDF_Doc>(&dummyOwner);

  double handle = registry.registerPointer(page, owner);
  CHECK(registry.getOwner(handle) == owner);
}

TEST_CASE("HandleRegistry rejects null pointers") {
  HandleRegistry<HPDF_Doc> registry;
  CHECK(registry.registerPointer(nullptr) == 0.0);
}

TEST_CASE("HandleRegistry unregisters handles") {
  HandleRegistry<HPDF_Doc> registry;

  int dummy = 1;
  HPDF_Doc doc = reinterpret_cast<HPDF_Doc>(&dummy);
  double handle = registry.registerPointer(doc);

  registry.unregisterHandle(handle);
  CHECK(registry.getPointer(handle) == nullptr);
  CHECK(registry.getHandle(doc) == 0.0);
}

TEST_CASE("HandleRegistry unregisters pointers") {
  HandleRegistry<HPDF_Doc> registry;

  int dummy = 1;
  HPDF_Doc doc = reinterpret_cast<HPDF_Doc>(&dummy);
  double handle = registry.registerPointer(doc);

  registry.unregisterPointer(doc);
  CHECK(registry.getPointer(handle) == nullptr);
}

TEST_CASE("HandleRegistry clears all entries") {
  HandleRegistry<HPDF_Doc> registry;

  int dummy = 1;
  HPDF_Doc doc = reinterpret_cast<HPDF_Doc>(&dummy);
  double handle = registry.registerPointer(doc);

  registry.clear();
  CHECK(registry.getPointer(handle) == nullptr);
  CHECK(registry.allEntries().empty());
}

TEST_CASE("Handle validates registered handles") {
  HandleRegistry<HPDF_Doc> registry;

  int dummy = 1;
  HPDF_Doc doc = reinterpret_cast<HPDF_Doc>(&dummy);
  double handle = registry.registerPointer(doc);

  Handle<HPDF_Doc> valid(handle, registry);
  CHECK(valid.isValid());
  CHECK(valid.get() == doc);

  Handle<HPDF_Doc> invalid(9999.0, registry);
  CHECK_FALSE(invalid.isValid());
  CHECK_THROWS_AS(invalid.get(), std::invalid_argument);
}
