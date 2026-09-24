#pragma once

#include "HandleRegistry.hpp"
#include "HaruWorker.hpp"
#include "HybridNitroPdfWriterSpec.hpp"
#include "MediaCache.hpp"
#include <NitroModules/AnyMap.hpp>
#include <NitroModules/ArrayBuffer.hpp>
#include <NitroModules/Promise.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <unordered_set>

namespace margelo::nitro::pdfwriter {

using namespace margelo::nitro;

class HybridNitroPdfWriter : public HybridNitroPdfWriterSpec {
public:
  static constexpr auto TAG = "NitroPdfWriter";

  HybridNitroPdfWriter();
  ~HybridNitroPdfWriter() override;

  // Document lifecycle
  std::shared_ptr<Promise<double>> createDocument() override;
  std::shared_ptr<Promise<void>> freeDocument(double doc) override;
  std::shared_ptr<Promise<void>> saveToFile(double doc, const std::string& path) override;
  std::shared_ptr<Promise<std::shared_ptr<ArrayBuffer>>> saveToBuffer(double doc) override;
  std::shared_ptr<Promise<bool>> hasDoc(double doc) override;

  // Error handling
  std::shared_ptr<Promise<double>> getError(double doc) override;
  std::shared_ptr<Promise<double>> getErrorDetail(double doc) override;
  std::shared_ptr<Promise<void>> resetError(double doc) override;

  // Document properties
  std::shared_ptr<Promise<void>> setCompressionMode(double doc, double mode) override;
  std::shared_ptr<Promise<void>> setPassword(double doc, const std::string& ownerPassword,
                                             const std::string& userPassword) override;
  std::shared_ptr<Promise<void>> setPermission(double doc, double permission) override;
  std::shared_ptr<Promise<void>> setEncryptionMode(double doc, double mode, double keyLen) override;

  // Pages
  std::shared_ptr<Promise<double>> addPage(double doc) override;
  std::shared_ptr<Promise<double>> getCurrentPage(double doc) override;
  std::shared_ptr<Promise<void>> setCurrentPage(double doc, double page) override;
  std::shared_ptr<Promise<double>> insertPage(double doc, double page) override;
  std::shared_ptr<Promise<double>> getPageByIndex(double doc, double index) override;

  // Page sizes
  std::shared_ptr<Promise<void>> setPageWidth(double page, double width) override;
  std::shared_ptr<Promise<void>> setPageHeight(double page, double height) override;
  std::shared_ptr<Promise<void>> setPageSize(double page, double size) override;
  std::shared_ptr<Promise<void>> setSize(double page, double size, double direction) override;
  std::shared_ptr<Promise<void>> setRotate(double page, double angle) override;

  // Fonts
  std::shared_ptr<Promise<double>> getFont(double doc, const std::string& fontName,
                                           const std::optional<std::string>& encodingName) override;
  // Two-step: load (file/buffer → media handle, pre-parse outside lock) + attach (to doc)
  std::shared_ptr<Promise<double>> loadFontFromFile(const std::string& fileName,
                                                    std::optional<double> faceIndex) override;
  std::shared_ptr<Promise<double>> loadFontFromBuffer(const std::shared_ptr<ArrayBuffer>& buffer,
                                                      std::optional<double> faceIndex) override;
  std::shared_ptr<Promise<double>> attachFont(double doc, double media,
                                              std::optional<bool> embedding) override;
  // Sugar (load + attach)
  std::shared_ptr<Promise<double>> loadType1FontFromFile(double doc, const std::string& afmPath,
                                                         const std::optional<std::string>& pfmPath) override;
  std::shared_ptr<Promise<double>> loadTTFontFromFile(double doc, const std::string& fileName,
                                                      std::optional<bool> embedding) override;
  std::shared_ptr<Promise<double>> loadTTFontFromFile2(double doc, const std::string& fileName,
                                                       double index,
                                                       std::optional<bool> embedding) override;
  std::shared_ptr<Promise<void>> setCurrentFont(double doc, double font) override;
  std::shared_ptr<Promise<void>> setFontAndSize(double page, double font, double size) override;
  std::shared_ptr<Promise<std::string>> getFontName(double font) override;
  std::shared_ptr<Promise<double>> measureText(double page, const std::string& text, double width,
                                               std::optional<bool> wordwrap) override;

  // Text state
  std::shared_ptr<Promise<void>> beginText(double page) override;
  std::shared_ptr<Promise<void>> endText(double page) override;
  std::shared_ptr<Promise<void>> textOut(double page, double x, double y,
                                         const std::string& text) override;
  std::shared_ptr<Promise<void>> textRect(double page, double left, double top, double right,
                                          double bottom, const std::string& text,
                                          double align) override;
  std::shared_ptr<Promise<void>> setTextLeading(double page, double leading) override;
  std::shared_ptr<Promise<void>> setTextRenderingMode(double page, double mode) override;
  std::shared_ptr<Promise<void>> setTextRise(double page, double rise) override;
  std::shared_ptr<Promise<void>> setCharSpace(double page, double space) override;
  std::shared_ptr<Promise<void>> setWordSpace(double page, double space) override;
  std::shared_ptr<Promise<void>> setHorizontalScalling(double page, double scale) override;

  // Text transformation
  std::shared_ptr<Promise<void>> moveTextPos(double page, double x, double y) override;
  std::shared_ptr<Promise<void>> moveTextPos2(double page, double x, double y) override;
  std::shared_ptr<Promise<void>> setTextMatrix(double page, double a, double b, double c, double d,
                                               double x, double y) override;

  // Graphics state
  std::shared_ptr<Promise<void>> setLineWidth(double page, double width) override;
  std::shared_ptr<Promise<void>> setLineCap(double page, double cap) override;
  std::shared_ptr<Promise<void>> setLineJoin(double page, double join) override;
  std::shared_ptr<Promise<void>> setMiterLimit(double page, double miterLimit) override;
  std::shared_ptr<Promise<void>> setDash(double page, const std::vector<double>& dashPattern,
                                         double phase) override;
  std::shared_ptr<Promise<void>> setFlat(double page, double flatness) override;
  std::shared_ptr<Promise<void>> setExtGState(double page, double extGState) override;

  // Colors
  std::shared_ptr<Promise<void>> setRGBFill(double page, double r, double g, double b) override;
  std::shared_ptr<Promise<void>> setRGBStroke(double page, double r, double g, double b) override;
  std::shared_ptr<Promise<void>> setCMYKFill(double page, double c, double m, double y,
                                             double k) override;
  std::shared_ptr<Promise<void>> setCMYKStroke(double page, double c, double m, double y,
                                               double k) override;
  std::shared_ptr<Promise<void>> setGrayFill(double page, double gray) override;
  std::shared_ptr<Promise<void>> setGrayStroke(double page, double gray) override;

  // Path construction
  std::shared_ptr<Promise<void>> moveTo(double page, double x, double y) override;
  std::shared_ptr<Promise<void>> lineTo(double page, double x, double y) override;
  std::shared_ptr<Promise<void>> curveTo(double page, double x1, double y1, double x2, double y2,
                                         double x3, double y3) override;
  std::shared_ptr<Promise<void>> curveTo2(double page, double x2, double y2, double x3,
                                          double y3) override;
  std::shared_ptr<Promise<void>> curveTo3(double page, double x1, double y1, double x3,
                                          double y3) override;
  std::shared_ptr<Promise<void>> closePath(double page) override;
  std::shared_ptr<Promise<void>> rectangle(double page, double x, double y, double width,
                                           double height) override;
  std::shared_ptr<Promise<void>> arc(double page, double x, double y, double radius, double angle1,
                                     double angle2) override;
  std::shared_ptr<Promise<void>> ellipse(double page, double x, double y, double xRadius,
                                         double yRadius) override;
  std::shared_ptr<Promise<void>> circle(double page, double x, double y, double radius) override;

  // Path painting
  std::shared_ptr<Promise<void>> stroke(double page) override;
  std::shared_ptr<Promise<void>> closePathStroke(double page) override;
  std::shared_ptr<Promise<void>> fill(double page) override;
  std::shared_ptr<Promise<void>> eofill(double page) override;
  std::shared_ptr<Promise<void>> fillStroke(double page) override;
  std::shared_ptr<Promise<void>> eofillStroke(double page) override;
  std::shared_ptr<Promise<void>> closePathFillStroke(double page) override;
  std::shared_ptr<Promise<void>> closePathEofillStroke(double page) override;
  std::shared_ptr<Promise<void>> endPath(double page) override;

  // Images — two-step load / attach
  std::shared_ptr<Promise<double>> loadImageFromFile(const std::string& fileName,
                                                     std::optional<bool> useCache) override;
  std::shared_ptr<Promise<double>> loadImageFromBuffer(const std::shared_ptr<ArrayBuffer>& buffer,
                                                       const std::string& format, double width,
                                                       double height, double colorSpace,
                                                       std::optional<bool> useCache) override;
  std::shared_ptr<Promise<double>> attachImage(double doc, double media) override;
  std::shared_ptr<Promise<void>> freeMedia(double media) override;

  // Sugar (load + attach)
  std::shared_ptr<Promise<double>> loadPngImageFromFile(double doc, const std::string& fileName) override;
  std::shared_ptr<Promise<double>> loadPngImageFromBuffer(double doc,
                                                          const std::shared_ptr<ArrayBuffer>& buffer,
                                                          std::optional<bool> useCache) override;
  std::shared_ptr<Promise<double>> loadJpegImageFromFile(double doc, const std::string& fileName) override;
  std::shared_ptr<Promise<double>> loadRawImageFromFile(double doc, const std::string& fileName,
                                                        double width, double height,
                                                        double colorSpace) override;
  std::shared_ptr<Promise<double>> loadRawImageFromBuffer(double doc,
                                                          const std::shared_ptr<ArrayBuffer>& buffer,
                                                          double width, double height,
                                                          double colorSpace,
                                                          std::optional<bool> useCache) override;
  std::shared_ptr<Promise<void>> setImageMask(double image, double mask) override;
  std::shared_ptr<Promise<void>> drawImage(double page, double image, double x, double y,
                                           double width, double height) override;
  std::shared_ptr<Promise<void>> drawRawImage(double page,
                                               const std::shared_ptr<ArrayBuffer>& buffer,
                                               double width, double height, double colorSpace,
                                               double x, double y,
                                               double drawWidth, double drawHeight) override;

  // Coordinate transforms
  std::shared_ptr<Promise<void>> gSave(double page) override;
  std::shared_ptr<Promise<void>> gRestore(double page) override;
  std::shared_ptr<Promise<void>> concat(double page, double a, double b, double c, double d,
                                        double x, double y) override;

  // Annotations
  std::shared_ptr<Promise<double>> createTextAnnot(double page, const std::vector<double>& rect,
                                                   const std::string& text,
                                                   const std::optional<std::string>& encoder) override;
  std::shared_ptr<Promise<double>> createLinkAnnot(double page, const std::vector<double>& rect,
                                                   double dst) override;
  std::shared_ptr<Promise<double>> createURILinkAnnot(double page, const std::vector<double>& rect,
                                                      const std::string& uri) override;

  // Destinations
  std::shared_ptr<Promise<double>> createDestination(double page) override;
  std::shared_ptr<Promise<void>> setDestinationXYZ(double dst, double x, double y,
                                                   double zoom) override;
  std::shared_ptr<Promise<void>> setDestinationFit(double dst) override;
  std::shared_ptr<Promise<void>> setDestinationFitH(double dst, double top) override;
  std::shared_ptr<Promise<void>> setDestinationFitV(double dst, double left) override;
  std::shared_ptr<Promise<void>> setDestinationFitR(double dst, double left, double bottom,
                                                    double right, double top) override;
  std::shared_ptr<Promise<void>> setDestinationFitB(double dst) override;
  std::shared_ptr<Promise<void>> setDestinationFitBH(double dst, double top) override;
  std::shared_ptr<Promise<void>> setDestinationFitBV(double dst, double left) override;

  // Outlines
  std::shared_ptr<Promise<double>> createOutline(double doc, double parent, const std::string& title,
                                                 const std::optional<std::string>& encoder) override;
  std::shared_ptr<Promise<void>> setOpened(double outline, bool opened) override;

  // ExtGState
  std::shared_ptr<Promise<double>> createExtGState(double doc) override;
  std::shared_ptr<Promise<void>> setAlphaStroke(double extGState, double alpha) override;
  std::shared_ptr<Promise<void>> setAlphaFill(double extGState, double alpha) override;
  std::shared_ptr<Promise<void>> setBlendMode(double extGState, double mode) override;

  // Info
  std::shared_ptr<Promise<void>> setInfoAttr(double doc, double infoType,
                                             const std::string& value) override;
  std::shared_ptr<Promise<std::string>> getInfoAttr(double doc, double infoType) override;
  std::shared_ptr<Promise<void>> setInfoDateAttr(double doc, double infoType,
                                                 const std::string& value) override;

  // Utility
  std::shared_ptr<Promise<double>> pageTextWidth(double page, const std::string& text) override;
  std::shared_ptr<Promise<double>> pageTextHeight(double page, const std::string& text) override;
  std::shared_ptr<Promise<double>> pageMeasureText(double page, const std::string& text, double width,
                                                   std::optional<bool> wordwrap) override;

  // Media cache (shared font pre-parse / image bytes across documents)
  std::shared_ptr<Promise<void>> setCacheDir(const std::string& path) override;
  std::shared_ptr<Promise<void>> clearMediaCache() override;
  std::shared_ptr<Promise<void>> setMediaCacheLimit(double maxBytes) override;
  std::shared_ptr<Promise<std::shared_ptr<AnyMap>>> getMediaCacheStats() override;

  // Quick Draw (High-level API)
  std::shared_ptr<Promise<std::variant<std::string, double>>> quickDraw(
      const std::vector<std::shared_ptr<AnyMap>>& operations, const std::string& unit,
      const std::optional<std::string>& outputPath, std::optional<double> dpi) override;
  std::shared_ptr<Promise<std::vector<std::variant<std::string, double>>>> quickBatchDraw(
      const std::vector<std::shared_ptr<AnyMap>>& items, std::optional<double> dpi) override;

  // Utility
  std::shared_ptr<Promise<std::shared_ptr<ArrayBuffer>>> yuv2rgb(
      const std::shared_ptr<ArrayBuffer>& buffer,
      double width, double height,
      YuvFormat format) override;

  /** Pre-parsed file sources for one quickDraw call (filled outside HaruLock). */
  struct QuickDrawMedia {
    // source path -> prepared image/font blob (parsed outside HaruLock)
    std::unordered_map<std::string, std::shared_ptr<MediaBlob>> images;
    std::unordered_map<std::string, std::shared_ptr<MediaBlob>> fonts;
  };

private:
  // Shared install helpers (HaruLock must be held).
  double installImageBlob(HPDF_Doc doc, const std::shared_ptr<MediaBlob>& blob);
  double installFontBlob(HPDF_Doc doc, const std::shared_ptr<MediaBlob>& blob,
                         std::optional<bool> embedding,
                         const char* encoding = nullptr);

  void executeOperation(HPDF_Doc doc, HPDF_Page& page, const std::shared_ptr<AnyMap>& op,
                       const std::string& defaultUnit, double dpi,
                       const QuickDrawMedia* media = nullptr);

  HandleRegistry<HPDF_Doc> _docs;
  HandleRegistry<HPDF_Page> _pages;
  HandleRegistry<HPDF_Font> _fonts;
  HandleRegistry<HPDF_Image> _images;
  HandleRegistry<HPDF_Destination> _destinations;
  HandleRegistry<HPDF_Outline> _outlines;
  HandleRegistry<HPDF_ExtGState> _extGStates;
  HandleRegistry<HPDF_Annotation> _annotations;
  MediaRegistry _media;
  std::unordered_set<double> _pagesInTextMode;  // Track pages with active text mode
};

} // namespace margelo::nitro::pdfwriter
